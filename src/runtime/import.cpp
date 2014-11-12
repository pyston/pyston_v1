// Copyright (c) 2014 Dropbox, Inc.
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

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "runtime/capi.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedModule* createAndRunModule(const std::string& name, const std::string& fn) {
    BoxedModule* module = createModule(name, fn);

    AST_Module* ast = caching_parse(fn.c_str());
    compileAndRunModule(ast, module);
    return module;
}

static BoxedModule* createAndRunModule(const std::string& name, const std::string& fn, const std::string& module_path) {
    BoxedModule* module = createModule(name, fn);

    Box* b_path = boxStringPtr(&module_path);

    BoxedList* path_list = new BoxedList();
    listAppendInternal(path_list, b_path);

    module->setattr("__path__", path_list, NULL);
    
    AST_Module* ast = caching_parse(fn.c_str());
    compileAndRunModule(ast, module);
    return module;
}

#if LLVMREV < 210072
#define LLVM_SYS_FS_EXISTS_CODE_OKAY(code) ((code) == 0)
#else
#define LLVM_SYS_FS_EXISTS_CODE_OKAY(code) (!(code))
#endif

static bool path_exists (const std::string& path) {
#if LLVMREV < 217625
    bool exists;
    llvm_error_code code = llvm::sys::fs::exists(path, exists);
    assert(LLVM_SYS_FS_EXISTS_CODE_OKAY(code));
#else
    bool exists = llvm::sys::fs::exists(path);
#endif
    return exists;
}

static BoxedModule* importPackageFromDirectory(const std::string& name, const std::string& path) {
    llvm::SmallString<128> joined_path;

    llvm::sys::path::append(joined_path, path, name);
    std::string dn(joined_path.str());

    llvm::sys::path::append(joined_path, "__init__.py");
    std::string fn(joined_path.str());

    if (VERBOSITY() >= 2)
        printf("Searching for %s at %s...\n", name.c_str(), fn.c_str());

    if (!path_exists(fn))
        return NULL;

    if (VERBOSITY() >= 1)
        printf("Importing %s from %s\n", name.c_str(), fn.c_str());


    return createAndRunModule(name, fn, dn);
}

static BoxedModule* importFile(const std::string& name, const std::string& path) {
    llvm::SmallString<128> joined_path;

    llvm::sys::path::append(joined_path, path, name + ".py");
    std::string fn(joined_path.str());

    if (VERBOSITY() >= 2)
        printf("Searching for %s at %s...\n", name.c_str(), fn.c_str());

    if (!path_exists(fn))
        return NULL;

    if (VERBOSITY() >= 1)
        printf("Importing %s from %s\n", name.c_str(), fn.c_str());

    return createAndRunModule(name, fn);
}

static Box* importSub(const std::string* name, Box* parent_module) {
    BoxedList* path_list;
    if (parent_module == NULL) {
        path_list = getSysPath();
        if (path_list == NULL || path_list->cls != list_cls) {
            raiseExcHelper(RuntimeError, "sys.path must be a list of directory names");
        }
    } else {
        path_list = static_cast<BoxedList*>(parent_module->getattr("__path__", NULL));
        if (path_list == NULL || path_list->cls != list_cls) {
            raiseExcHelper(ImportError, "No module named %s", name->c_str());
        }
    }

    llvm::SmallString<128> joined_path;
    for (int i = 0; i < path_list->size; i++) {
        Box* _p = path_list->elts->elts[i];
        if (_p->cls != str_cls)
            continue;
        BoxedString* p = static_cast<BoxedString*>(_p);

        joined_path.clear();
        llvm::sys::path::append(joined_path, p->s);
        std::string dn(joined_path.str());

	BoxedModule* module;

        module = importPackageFromDirectory(*name, dn);
        if (module)
            return module;

        module = importFile(*name, dn);
        if (module)
            return module;
    }

    if (*name == "basic_test") {
        return importTestExtension("basic_test");
    }
    if (*name == "descr_test") {
        return importTestExtension("descr_test");
    }

    raiseExcHelper(ImportError, "No module named %s", name->c_str());
}

static Box* import(const std::string* name, bool return_first) {
    assert(name);
    assert(name->size() > 0);

    static StatCounter slowpath_import("slowpath_import");
    slowpath_import.log();

    BoxedDict* sys_modules = getSysModulesDict();

    size_t l = 0, r;
    Box* last_module = NULL;
    Box* first_module = NULL;
    while (l < name->size()) {
        size_t r = name->find('.', l);
        if (r == std::string::npos) {
            r = name->size();
        }

        std::string prefix_name = std::string(*name, 0, r);
        Box* s = boxStringPtr(&prefix_name);
        if (sys_modules->d.find(s) != sys_modules->d.end()) {
            last_module = sys_modules->d[s];
        } else {
            std::string small_name = std::string(*name, l, r - l);
            last_module = importSub(&small_name, last_module);
        }

        if (l == 0) {
            first_module = last_module;
        }

        l = r + 1;
    }

    return return_first ? first_module : last_module;
}

extern "C" Box* import(int level, Box* from_imports, const std::string* module_name) {
    return import(module_name, from_imports == None);
}
}
