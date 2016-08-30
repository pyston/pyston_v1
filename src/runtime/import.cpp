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

#ifdef Py_REF_DEBUG
bool imported_foreign_cextension = false;
#endif

static void removeModule(BoxedString* name) {
    BoxedDict* d = getSysModulesDict();
    PyDict_DelItem(d, name);
}

extern "C" BORROWED(PyObject*) PyImport_GetModuleDict(void) noexcept {
    try {
        return getSysModulesDict();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* _PyImport_LoadDynamicModule(char* name, char* pathname, FILE* fp) noexcept {
    BoxedString* name_boxed = boxString(name);
    AUTO_DECREF(name_boxed);
    try {
        PyObject* m = _PyImport_FindExtension(name, pathname);
        if (m != NULL) {
            Py_INCREF(m);
            return m;
        }

        const char* lastdot = strrchr(name, '.');
        const char* shortname;
        if (lastdot == NULL) {
            shortname = name;
        } else {
            shortname = lastdot + 1;
        }

        m = importCExtension(name_boxed, shortname, pathname);

        if (_PyImport_FixupExtension(name, pathname) == NULL) {
            Py_DECREF(m);
            return NULL;
        }

        return m;
    } catch (ExcInfo e) {
        removeModule(name_boxed);
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* load_source_module(char* name, char* pathname, FILE* fp) noexcept {
    BoxedString* name_boxed = boxString(name);
    AUTO_DECREF(name_boxed);
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
        return incref(r);
    } catch (ExcInfo e) {
        removeModule(name_boxed);
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyImport_ExecCodeModuleEx(const char* name, PyObject* co, char* pathname) noexcept {
    BoxedString* s = boxString(name);
    AUTO_DECREF(s);
    try {
        RELEASE_ASSERT(co->cls == str_cls, "");
        BoxedString* code = (BoxedString*)co;

        BoxedModule* module = (BoxedModule*)PyImport_AddModule(name);
        if (module == NULL)
            return NULL;

        static BoxedString* file_str = getStaticString("__file__");
        module->setattr(file_str, autoDecref(boxString(pathname)), NULL);
        AST_Module* ast = parse_string(code->data(), /* future_flags = */ 0);
        compileAndRunModule(ast, module);
        return incref(module);
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

BoxedModule* importCExtension(BoxedString* full_name, const std::string& last_name, const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle)
        raiseExcHelper(ImportError, "%s", dlerror());
    assert(handle);

    std::string initname = "init" + last_name;
    void (*init)() = (void (*)())dlsym(handle, initname.c_str());

    char* error;
    if ((error = dlerror()) != NULL)
        raiseExcHelper(ImportError, "%s", error);

    assert(init);

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
    static BoxedString* file_str = getStaticString("__file__");
    m->setattr(file_str, autoDecref(boxString(path)), NULL);

    if (Py_VerboseFlag)
        PySys_WriteStderr("import %s # dynamically loaded from %s\n", full_name->c_str(), path.c_str());

#ifdef Py_REF_DEBUG
    // if we load a foreign C extension we can't check that _Py_RefTotal == 0
    if (!llvm::StringRef(path).endswith("from_cpython/Lib/" + last_name + ".pyston.so"))
        imported_foreign_cextension = true;
#endif

    return incref(m);
}
}
