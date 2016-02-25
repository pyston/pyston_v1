// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/import.h"

#include <dlfcn.h>
#include <fstream>
#include <limits.h>
#include <link.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/util.h"

namespace pyston {

static void removeModule(BoxedString* name) {
    BoxedDict* d = getSysModulesDict();
    d->d.erase(name);
}

extern "C" PyObject* PyImport_GetModuleDict(void) noexcept {
    try {
        return getSysModulesDict();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* _PyImport_LoadDynamicModule(char* name, char* pathname, FILE* fp) noexcept {
    BoxedString* name_boxed = boxString(name);
    try {
        const char* lastdot = strrchr(name, '.');
        const char* shortname;
        if (lastdot == NULL) {
            shortname = name;
        } else {
            shortname = lastdot + 1;
        }

        return importCExtension(name_boxed, shortname, pathname);
    } catch (ExcInfo e) {
        removeModule(name_boxed);
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* load_source_module(char* name, char* pathname, FILE* fp) noexcept {
    BoxedString* name_boxed = boxString(name);
    try {
        BoxedModule* module = createModule(name_boxed, pathname);
        AST_Module* ast = caching_parse_file(pathname, /* future_flags = */ 0);
        assert(ast);
        compileAndRunModule(ast, module);
        Box* r = getSysModulesDict()->getOrNull(name_boxed);
        if (!r) {
            PyErr_Format(ImportError, "Loaded module %.200s not found in sys.modules", name);
            return NULL;
        }
        if (Py_VerboseFlag)
            PySys_WriteStderr("import %s # from %s\n", name, pathname);
        return r;
    } catch (ExcInfo e) {
        removeModule(name_boxed);
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyImport_ExecCodeModuleEx(const char* name, PyObject* co, char* pathname) noexcept {
    BoxedString* s = boxString(name);
    try {
        RELEASE_ASSERT(co->cls == str_cls, "");
        BoxedString* code = (BoxedString*)co;

        BoxedModule* module = (BoxedModule*)PyImport_AddModule(name);
        if (module == NULL)
            return NULL;

        static BoxedString* file_str = internStringImmortal("__file__");
        module->setattr(file_str, boxString(pathname), NULL);
        AST_Module* ast = parse_string(code->data(), /* future_flags = */ 0);
        compileAndRunModule(ast, module);
        return module;
    } catch (ExcInfo e) {
        removeModule(s);
        setCAPIException(e);
        return NULL;
    }
}

extern "C" Box* import(int level, Box* from_imports, llvm::StringRef module_name) {
    Box* rtn = PyImport_ImportModuleLevel(module_name.str().c_str(), getGlobalsDict(), NULL, from_imports, level);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

// Parses the memory map and registers all memory regions with "rwxp" permission and which contain the requested file
// path with the GC. In addition adds the BSS segment.
static void registerDataSegment(void* dl_handle) {
    // get library path and resolve symlinks
    link_map* map = NULL;
    dlinfo(dl_handle, RTLD_DI_LINKMAP, &map);
    RELEASE_ASSERT(map && map->l_name, "this should never be NULL");
    char* converted_path = realpath(map->l_name, NULL);
    RELEASE_ASSERT(converted_path, "");
    std::string lib_path = converted_path;
    free(converted_path);

    std::ifstream mapmap("/proc/self/maps");
    std::string line;
    llvm::SmallVector<std::pair<uint64_t, uint64_t>, 4> mem_ranges;
    while (std::getline(mapmap, line)) {
        // format is:
        // address perms offset dev inode pathname
        llvm::SmallVector<llvm::StringRef, 6> line_split;
        llvm::StringRef(line).split(line_split, " ", 5, false);
        RELEASE_ASSERT(line_split.size() >= 5, "%zu", line_split.size());
        if (line_split.size() < 6)
            continue; // pathname is missing

        llvm::StringRef permissions = line_split[1].trim();
        llvm::StringRef pathname = line_split[5].trim();

        if (permissions == "rwxp" && pathname == lib_path) {
            llvm::StringRef mem_range = line_split[0].trim();
            auto mem_range_split = mem_range.split('-');
            uint64_t lower_addr = 0;
            bool error = mem_range_split.first.getAsInteger(16, lower_addr);
            RELEASE_ASSERT(!error, "");
            uint64_t upper_addr = 0;
            error = mem_range_split.second.getAsInteger(16, upper_addr);
            RELEASE_ASSERT(!error, "");

            RELEASE_ASSERT(lower_addr < upper_addr, "");
            RELEASE_ASSERT(upper_addr - lower_addr < 1000000,
                           "Large data section detected - there maybe something wrong");

            mem_ranges.emplace_back(lower_addr, upper_addr);
        }
    }
    RELEASE_ASSERT(mem_ranges.size() >= 1, "");

    uintptr_t bss_start = (uintptr_t)dlsym(dl_handle, "__bss_start");
    uintptr_t bss_end = (uintptr_t)dlsym(dl_handle, "_end");
    RELEASE_ASSERT(bss_end - bss_start < 1000000, "Large BSS section detected - there maybe something wrong");

    // most of the time the BSS section is inside one of memory regions, in that case we don't have to register it.
    bool should_add_bss = true;
    for (auto r : mem_ranges) {
        if (r.first <= bss_start && bss_end <= r.second)
            should_add_bss = false;
        gc::registerPotentialRootRange((void*)r.first, (void*)r.second);
    }

    if (should_add_bss)
        gc::registerPotentialRootRange((void*)bss_start, (void*)bss_end);
}

BoxedModule* importCExtension(BoxedString* full_name, const std::string& last_name, const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        // raiseExcHelper(ImportError, "%s", dlerror());
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    assert(handle);

    std::string initname = "init" + last_name;
    void (*init)() = (void (*)())dlsym(handle, initname.c_str());

    char* error;
    if ((error = dlerror()) != NULL) {
        // raiseExcHelper(ImportError, "%s", error);
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    assert(init);

    // Let the GC know about the static variables.
    registerDataSegment(handle);

    char* packagecontext = strdup(full_name->c_str());
    char* oldcontext = _Py_PackageContext;
    _Py_PackageContext = packagecontext;
    (*init)();
    _Py_PackageContext = oldcontext;
    free(packagecontext);

    checkAndThrowCAPIException();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* _m = sys_modules->d[full_name];
    RELEASE_ASSERT(_m, "dynamic module not initialized properly");
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);
    static BoxedString* file_str = internStringImmortal("__file__");
    m->setattr(file_str, boxString(path), NULL);

    if (Py_VerboseFlag)
        PySys_WriteStderr("import %s # dynamically loaded from %s\n", full_name->c_str(), path.c_str());

    return m;
}
}
