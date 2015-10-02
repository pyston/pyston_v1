// Copyright (c) 2014-2015 Dropbox, Inc.
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

static const std::string all_str("__all__");
static const std::string name_str("__name__");
static const std::string package_str("__package__");
static BoxedClass* null_importer_cls;

static void removeModule(BoxedString* name) {
    BoxedDict* d = getSysModulesDict();
    d->d.erase(name);
}

Box* createAndRunModule(BoxedString* name, const std::string& fn) {
    BoxedModule* module = createModule(name, fn.c_str());

    AST_Module* ast = caching_parse_file(fn.c_str(), /* future_flags = */ 0);
    assert(ast);
    try {
        compileAndRunModule(ast, module);
    } catch (ExcInfo e) {
        removeModule(name);
        throw e;
    }

    Box* r = getSysModulesDict()->getOrNull(name);
    if (!r)
        raiseExcHelper(ImportError, "Loaded module %.200s not found in sys.modules", name->c_str());
    return r;
}

static Box* createAndRunModule(BoxedString* name, const std::string& fn, const std::string& module_path) {
    BoxedModule* module = createModule(name, fn.c_str());

    Box* b_path = boxString(module_path);

    BoxedList* path_list = new BoxedList();
    listAppendInternal(path_list, b_path);

    static BoxedString* path_str = internStringImmortal("__path__");
    module->setattr(path_str, path_list, NULL);

    AST_Module* ast = caching_parse_file(fn.c_str(), /* future_flags = */ 0);
    assert(ast);
    try {
        compileAndRunModule(ast, module);
    } catch (ExcInfo e) {
        removeModule(name);
        throw e;
    }

    Box* r = getSysModulesDict()->getOrNull(name);
    if (!r)
        raiseExcHelper(ImportError, "Loaded module %.200s not found in sys.modules", name->c_str());
    return r;
}

#if LLVMREV < 210072
#define LLVM_SYS_FS_EXISTS_CODE_OKAY(code) ((code) == 0)
#else
#define LLVM_SYS_FS_EXISTS_CODE_OKAY(code) (!(code))
#endif

static bool pathExists(const std::string& path) {
#if LLVMREV < 217625
    bool exists;
    llvm_error_code code = llvm::sys::fs::exists(path, exists);
    assert(LLVM_SYS_FS_EXISTS_CODE_OKAY(code));
#else
    bool exists = llvm::sys::fs::exists(path);
#endif
    return exists;
}

/* Return an importer object for a sys.path/pkg.__path__ item 'p',
   possibly by fetching it from the path_importer_cache dict. If it
   wasn't yet cached, traverse path_hooks until a hook is found
   that can handle the path item. Return None if no hook could;
   this tells our caller it should fall back to the builtin
   import mechanism. Cache the result in path_importer_cache.
   Returns a borrowed reference. */

extern "C" PyObject* get_path_importer(PyObject* path_importer_cache, PyObject* path_hooks, PyObject* p) noexcept {
    PyObject* importer;
    Py_ssize_t j, nhooks;

    /* These conditions are the caller's responsibility: */
    assert(PyList_Check(path_hooks));
    assert(PyDict_Check(path_importer_cache));

    nhooks = PyList_Size(path_hooks);
    if (nhooks < 0)
        return NULL; /* Shouldn't happen */

    importer = PyDict_GetItem(path_importer_cache, p);
    if (importer != NULL)
        return importer;

    /* set path_importer_cache[p] to None to avoid recursion */
    if (PyDict_SetItem(path_importer_cache, p, Py_None) != 0)
        return NULL;

    for (j = 0; j < nhooks; j++) {
        PyObject* hook = PyList_GetItem(path_hooks, j);
        if (hook == NULL)
            return NULL;
        importer = PyObject_CallFunctionObjArgs(hook, p, NULL);
        if (importer != NULL)
            break;

        if (!PyErr_ExceptionMatches(PyExc_ImportError)) {
            return NULL;
        }
        PyErr_Clear();
    }
    if (importer == NULL) {
        importer = PyObject_CallFunctionObjArgs(null_importer_cls, p, NULL);
        if (importer == NULL) {
            if (PyErr_ExceptionMatches(PyExc_ImportError)) {
                PyErr_Clear();
                return Py_None;
            }
        }
    }
    if (importer != NULL) {
        int err = PyDict_SetItem(path_importer_cache, p, importer);
        Py_DECREF(importer);
        if (err != 0)
            return NULL;
    }
    return importer;
}

struct SearchResult {
    // Each of these fields are only valid/used for certain filetypes:
    std::string path;
    Box* loader;

    enum filetype {
        SEARCH_ERROR,
        PY_SOURCE,
        PY_COMPILED,
        C_EXTENSION,
        PY_RESOURCE, /* Mac only */
        PKG_DIRECTORY,
        C_BUILTIN,
        PY_FROZEN,
        PY_CODERESOURCE, /* Mac only */
        IMP_HOOK
    } type;

    SearchResult(const std::string& path, filetype type) : path(path), type(type) {}
    SearchResult(std::string&& path, filetype type) : path(std::move(path)), type(type) {}
    SearchResult(Box* loader) : loader(loader), type(IMP_HOOK) {}
};

SearchResult findModule(const std::string& name, BoxedString* full_name, BoxedList* path_list) {
    static BoxedString* meta_path_str = internStringImmortal("meta_path");
    BoxedList* meta_path = static_cast<BoxedList*>(sys_module->getattr(meta_path_str));
    if (!meta_path || meta_path->cls != list_cls)
        raiseExcHelper(RuntimeError, "sys.meta_path must be a list of import hooks");

    static BoxedString* findmodule_str = internStringImmortal("find_module");
    for (int i = 0; i < meta_path->size; i++) {
        Box* finder = meta_path->elts->elts[i];

        auto path_pass = path_list ? path_list : None;
        CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(2) };
        Box* loader = callattr(finder, findmodule_str, callattr_flags, full_name, path_pass, NULL, NULL, NULL);

        if (loader != None)
            return SearchResult(loader);
    }

    if (!path_list)
        path_list = getSysPath();

    if (path_list == NULL || path_list->cls != list_cls) {
        raiseExcHelper(RuntimeError, "sys.path must be a list of directory names");
    }

    static BoxedString* path_hooks_str = internStringImmortal("path_hooks");
    BoxedList* path_hooks = static_cast<BoxedList*>(sys_module->getattr(path_hooks_str));
    if (!path_hooks || path_hooks->cls != list_cls)
        raiseExcHelper(RuntimeError, "sys.path_hooks must be a list of import hooks");

    static BoxedString* path_importer_cache_str = internStringImmortal("path_importer_cache");
    BoxedDict* path_importer_cache = static_cast<BoxedDict*>(sys_module->getattr(path_importer_cache_str));
    if (!path_importer_cache || path_importer_cache->cls != dict_cls)
        raiseExcHelper(RuntimeError, "sys.path_importer_cache must be a dict");

    llvm::SmallString<128> joined_path;
    for (int i = 0; i < path_list->size; i++) {
        Box* _p = path_list->elts->elts[i];
        if (_p->cls != str_cls)
            continue;
        BoxedString* p = static_cast<BoxedString*>(_p);

        joined_path.clear();
        llvm::sys::path::append(joined_path, p->s(), name);
        std::string dn(joined_path.str());

        llvm::sys::path::append(joined_path, "__init__.py");
        std::string fn(joined_path.str());

        PyObject* importer = get_path_importer(path_importer_cache, path_hooks, _p);
        if (importer == NULL)
            throwCAPIException();

        if (importer != None) {
            CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(1) };
            Box* loader = callattr(importer, findmodule_str, callattr_flags, full_name, NULL, NULL, NULL, NULL);
            if (loader != None)
                return SearchResult(loader);
        }

        if (pathExists(fn))
            return SearchResult(std::move(dn), SearchResult::PKG_DIRECTORY);

        joined_path.clear();
        llvm::sys::path::append(joined_path, std::string(p->s()), name + ".py");
        fn = joined_path.str();

        if (pathExists(fn))
            return SearchResult(std::move(fn), SearchResult::PY_SOURCE);

        joined_path.clear();
        llvm::sys::path::append(joined_path, p->s(), name + ".pyston.so");
        fn = joined_path.str();

        if (pathExists(fn))
            return SearchResult(std::move(fn), SearchResult::C_EXTENSION);
    }

    return SearchResult("", SearchResult::SEARCH_ERROR);
}

/* Return the package that an import is being performed in.  If globals comes
   from the module foo.bar.bat (not itself a package), this returns the
   sys.modules entry for foo.bar.  If globals is from a package's __init__.py,
   the package's entry in sys.modules is returned, as a borrowed reference.

   The *name* of the returned package is returned in buf.

   If globals doesn't come from a package or a module in a package, or a
   corresponding entry is not found in sys.modules, Py_None is returned.
*/
static Box* getParent(Box* globals, int level, std::string& buf) {
    int orig_level = level;

    if (globals == NULL || globals == None || level == 0)
        return None;

    static BoxedString* package_str = internStringImmortal("__package__");
    BoxedString* pkgname = static_cast<BoxedString*>(getFromGlobals(globals, package_str));
    if (pkgname != NULL && pkgname != None) {
        /* __package__ is set, so use it */
        if (pkgname->cls != str_cls) {
            raiseExcHelper(ValueError, "__package__ set to non-string");
        }
        size_t len = pkgname->size();
        if (len == 0) {
            if (level > 0) {
                raiseExcHelper(ValueError, "Attempted relative import in non-package");
            }
            return None;
        }
        if (len > PATH_MAX) {
            raiseExcHelper(ValueError, "Package name too long");
        }
        buf += pkgname->s();
    } else {
        static BoxedString* name_str = internStringImmortal("__name__");

        /* __package__ not set, so figure it out and set it */
        BoxedString* modname = static_cast<BoxedString*>(getFromGlobals(globals, name_str));
        if (modname == NULL || modname->cls != str_cls)
            return None;

        static BoxedString* path_str = internStringImmortal("__path__");
        Box* modpath = getFromGlobals(globals, path_str);

        if (modpath != NULL) {
            /* __path__ is set, so modname is already the package name */
            if (modname->size() > PATH_MAX) {
                raiseExcHelper(ValueError, "Module name too long");
            }
            buf += modname->s();
            setGlobal(globals, package_str, modname);
        } else {
            /* Normal module, so work out the package name if any */
            size_t lastdot = modname->s().rfind('.');
            if (lastdot == std::string::npos && level > 0) {
                raiseExcHelper(ValueError, "Attempted relative import in non-package");
            }
            if (lastdot == std::string::npos) {
                setGlobal(globals, package_str, None);
                return None;
            }
            if (lastdot >= PATH_MAX) {
                raiseExcHelper(ValueError, "Module name too long");
            }

            buf = std::string(modname->s(), 0, lastdot);
            setGlobal(globals, package_str, boxString(buf));
        }
    }

    size_t dot = buf.size() - 1;
    while (--level > 0) {
        dot = buf.rfind('.', dot);
        if (dot == std::string::npos) {
            raiseExcHelper(ValueError, "Attempted relative import beyond toplevel package");
        }
        dot--;
    }

    buf = std::string(buf, 0, dot + 1);

    BoxedDict* sys_modules = getSysModulesDict();
    Box* boxed_name = boxString(buf);
    Box* parent = sys_modules->d.find(boxed_name) != sys_modules->d.end() ? sys_modules->d[boxed_name] : NULL;
    if (parent == NULL) {
        if (orig_level < 1) {
            printf("Warning: Parent module '%.200s' not found "
                   "while handling absolute import\n",
                   buf.c_str());
        } else {
            raiseExcHelper(SystemError, "Parent module '%.200s' not loaded, "
                                        "cannot perform relative import",
                           buf.c_str());
        }
    }
    return parent;
    /* We expect, but can't guarantee, if parent != None, that:
       - parent.__name__ == buf
       - parent.__dict__ is globals
       If this is violated...  Who cares? */
}


static Box* importSub(const std::string& name, BoxedString* full_name, Box* parent_module) {
    BoxedDict* sys_modules = getSysModulesDict();
    if (sys_modules->d.find(full_name) != sys_modules->d.end()) {
        return sys_modules->d[full_name];
    }

    BoxedList* path_list;
    if (parent_module == NULL || parent_module == None) {
        path_list = NULL;
    } else {
        static BoxedString* path_str = internStringImmortal("__path__");
        path_list = static_cast<BoxedList*>(getattrInternal<ExceptionStyle::CXX>(parent_module, path_str, NULL));
        if (path_list == NULL || path_list->cls != list_cls) {
            return None;
        }
    }

    SearchResult sr = findModule(name, full_name, path_list);

    if (sr.type != SearchResult::SEARCH_ERROR) {
        Box* module;

        try {
            if (sr.type == SearchResult::PY_SOURCE)
                module = createAndRunModule(full_name, sr.path);
            else if (sr.type == SearchResult::PKG_DIRECTORY)
                module = createAndRunModule(full_name, sr.path + "/__init__.py", sr.path);
            else if (sr.type == SearchResult::C_EXTENSION)
                module = importCExtension(full_name, name, sr.path);
            else if (sr.type == SearchResult::IMP_HOOK) {
                static BoxedString* loadmodule_str = internStringImmortal("load_module");
                CallattrFlags callattr_flags{.cls_only = false,
                                             .null_on_nonexistent = false,
                                             .argspec = ArgPassSpec(1) };
                module = callattr(sr.loader, loadmodule_str, callattr_flags, full_name, NULL, NULL, NULL, NULL);
            } else
                RELEASE_ASSERT(0, "%d", sr.type);
        } catch (ExcInfo e) {
            removeModule(full_name);
            throw e;
        }

        if (parent_module && parent_module != None)
            parent_module->setattr(internStringMortal(name), module, NULL);
        return module;
    }

    return None;
}

static void markMiss(std::string& name) {
    BoxedDict* modules = getSysModulesDict();
    Box* b_name = boxString(name);
    modules->d[b_name] = None;
}

/* altmod is either None or same as mod */
static bool loadNext(Box* mod, Box* altmod, std::string& name, std::string& buf, Box** rtn) {
    size_t dot = name.find('.');
    size_t len;
    Box* result;
    bool call_again = true;

    if (name.size() == 0) {
        /* completely empty module name should only happen in
           'from . import' (or '__import__("")')*/
        *rtn = mod;
        return false;
    }

    std::string local_name(name);
    if (dot == std::string::npos) {
        len = name.size();
        call_again = false;
    } else {
        name = name.substr(dot + 1);
        len = dot;
    }
    if (len == 0) {
        raiseExcHelper(ValueError, "Empty module name");
    }

    if (buf.size() != 0)
        buf += ".";
    if (buf.size() >= PATH_MAX) {
        raiseExcHelper(ValueError, "Module name too long");
    }

    std::string subname(local_name.substr(0, len));
    buf += subname;

    result = importSub(subname, boxString(buf), mod);
    if (result == None && altmod != mod) {
        /* Here, altmod must be None and mod must not be None */
        result = importSub(subname, boxString(subname), altmod);
        if (result != NULL && result != None) {
            markMiss(buf);

            buf = subname;
        }
    }
    if (result == NULL)
        throwCAPIException();

    if (result == None)
        raiseExcHelper(ImportError, "No module named %.200s", local_name.c_str());

    *rtn = result;
    return call_again;
}

static void ensureFromlist(Box* module, Box* fromlist, std::string& buf, bool recursive);
Box* importModuleLevel(llvm::StringRef name, Box* globals, Box* from_imports, int level) {
    bool return_first = from_imports == None;

    static StatCounter slowpath_import("slowpath_import");
    slowpath_import.log();

    std::string buf;
    Box* parent = getParent(globals, level, buf);
    if (!parent)
        return NULL;

    // make a copy of the string we pass in, since loadNext while
    // modify the string as we load the modules along the path.
    std::string _name = name;

    Box* head;
    bool again = loadNext(parent, level < 0 ? None : parent, _name, buf, &head);
    if (head == NULL)
        return NULL;

    Box* tail = head;
    while (again) {
        Box* next;
        again = loadNext(tail, tail, _name, buf, &next);
        if (next == NULL) {
            return NULL;
        }
        tail = next;
    }
    if (tail == None) {
        /* If tail is Py_None, both get_parent and load_next found
           an empty module name: someone called __import__("") or
           doctored faulty bytecode */
        raiseExcHelper(ValueError, "Empty module name");
    }

    Box* module = return_first ? head : tail;
    if (from_imports != None) {
        ensureFromlist(module, from_imports, buf, false);
    }
    return module;
}

extern "C" void _PyImport_AcquireLock() noexcept {
    // TODO: currently no import lock!
}

extern "C" int _PyImport_ReleaseLock() noexcept {
    // TODO: currently no import lock!
    return 1;
}

extern "C" void _PyImport_ReInitLock() noexcept {
    // TODO: currently no import lock!
}

extern "C" PyObject* PyImport_ImportModuleNoBlock(const char* name) noexcept {
    return PyImport_ImportModule(name);
}

// This function has the same behaviour as __import__()
extern "C" PyObject* PyImport_ImportModuleLevel(const char* name, PyObject* globals, PyObject* locals,
                                                PyObject* fromlist, int level) noexcept {
    try {
        std::string module_name = name;
        return importModuleLevel(module_name, globals, fromlist ? fromlist : None, level);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static void ensureFromlist(Box* module, Box* fromlist, std::string& buf, bool recursive) {
    static BoxedString* path_str = internStringImmortal("__path__");
    Box* pathlist = NULL;
    try {
        pathlist = getattrInternal<ExceptionStyle::CXX>(module, path_str, NULL);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
    }

    if (pathlist == NULL) {
        // If it's not a package, then there's no sub-importing to do
        return;
    }

    for (Box* _s : fromlist->pyElements()) {
        RELEASE_ASSERT(PyString_Check(_s), "");
        BoxedString* s = static_cast<BoxedString*>(_s);
        internStringMortalInplace(s);

        if (s->s()[0] == '*') {
            // If __all__ contains a '*', just skip it:
            if (recursive)
                continue;

            static BoxedString* all_str = internStringImmortal("__all__");
            Box* all = getattrInternal<ExceptionStyle::CXX>(module, all_str, NULL);
            if (all) {
                ensureFromlist(module, all, buf, true);
            }
            continue;
        }

        Box* attr = getattrInternal<ExceptionStyle::CXX>(module, s, NULL);
        if (attr != NULL)
            continue;

        // Just want to import it and add it to the modules list for now:
        importSub(s->s(), boxStringTwine(llvm::Twine(buf) + "." + s->s()), module);
    }
}

extern "C" PyObject* PyImport_Import(PyObject* module_name) noexcept {
    RELEASE_ASSERT(module_name, "");
    RELEASE_ASSERT(module_name->cls == str_cls, "");

    try {
        // TODO: check if this has the same behaviour as the cpython implementation
        BoxedList* silly_list = new BoxedList();
        listAppendInternal(silly_list, boxString("__doc__"));
        return import(0, silly_list, ((BoxedString*)module_name)->s());
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyImport_ImportModule(const char* name) noexcept {
    return PyImport_Import(boxString(name));
}

extern "C" PyObject* PyImport_GetModuleDict(void) noexcept {
    try {
        return getSysModulesDict();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

/* Get the module object corresponding to a module name.
   First check the modules dictionary if there's one there,
   if not, create a new one and insert it in the modules dictionary.
   Because the former action is most common, THIS DOES NOT RETURN A
   'NEW' REFERENCE! */

extern "C" PyObject* PyImport_AddModule(const char* name) noexcept {
    try {
        PyObject* modules = getSysModulesDict();
        BoxedString* s = boxString(name);
        PyObject* m = PyDict_GetItem(modules, s);

        if (m != NULL && m->cls == module_cls)
            return m;
        return createModule(s);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyImport_ExecCodeModuleEx(char* name, PyObject* co, char* pathname) noexcept {
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

static int isdir(const char* path) {
    struct stat statbuf;
    return stat(path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode);
}

Box* nullImporterInit(Box* self, Box* _path) {
    RELEASE_ASSERT(self->cls == null_importer_cls, "");

    if (_path->cls != str_cls)
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(_path));

    BoxedString* path = (BoxedString*)_path;
    if (path->s().empty())
        raiseExcHelper(ImportError, "empty pathname");

    if (isdir(path->data()))
        raiseExcHelper(ImportError, "existing directory");

    return None;
}

Box* nullImporterFindModule(Box* self, Box* fullname, Box* path) {
    return None;
}

extern "C" Box* import(int level, Box* from_imports, llvm::StringRef module_name) {
    return importModuleLevel(module_name, getGlobals(), from_imports, level);
}

Box* impFindModule(Box* _name, BoxedList* path) {
    _name = coerceUnicodeToStr<CXX>(_name);
    RELEASE_ASSERT(_name->cls == str_cls, "");

    BoxedString* name = static_cast<BoxedString*>(_name);
    BoxedList* path_list = path && path != None ? path : getSysPath();

    SearchResult sr = findModule(name->s(), name, path_list);
    if (sr.type == SearchResult::SEARCH_ERROR)
        raiseExcHelper(ImportError, "%s", name->data());

    if (sr.type == SearchResult::PY_SOURCE) {
        Box* path = boxString(sr.path);
        Box* mode = boxString("r");
        Box* f = runtimeCall(file_cls, ArgPassSpec(2), path, mode, NULL, NULL, NULL);
        return BoxedTuple::create({ f, path, BoxedTuple::create({ boxString(".py"), mode, boxInt(sr.type) }) });
    }

    if (sr.type == SearchResult::PKG_DIRECTORY) {
        Box* path = boxString(sr.path);
        Box* mode = EmptyString;
        return BoxedTuple::create({ None, path, BoxedTuple::create({ mode, mode, boxInt(sr.type) }) });
    }

    if (sr.type == SearchResult::C_EXTENSION) {
        Box* path = boxString(sr.path);
        Box* mode = boxString("rb");
        Box* f = runtimeCall(file_cls, ArgPassSpec(2), path, mode, NULL, NULL, NULL);
        return BoxedTuple::create({ f, path, BoxedTuple::create({ boxString(".so"), mode, boxInt(sr.type) }) });
    }

    RELEASE_ASSERT(0, "unknown type: %d", sr.type);
}

Box* impLoadModule(Box* _name, Box* _file, Box* _pathname, Box** args) {
    Box* _description = args[0];

    RELEASE_ASSERT(_name->cls == str_cls, "");
    RELEASE_ASSERT(_pathname->cls == str_cls, "");
    RELEASE_ASSERT(_description->cls == tuple_cls, "");

    BoxedString* name = (BoxedString*)_name;
    BoxedString* pathname = (BoxedString*)_pathname;
    BoxedTuple* description = (BoxedTuple*)_description;

    RELEASE_ASSERT(description->size() == 3, "");
    BoxedString* suffix = (BoxedString*)description->elts[0];
    BoxedString* mode = (BoxedString*)description->elts[1];
    BoxedInt* type = (BoxedInt*)description->elts[2];

    RELEASE_ASSERT(mode->cls == str_cls, "");
    RELEASE_ASSERT(type->cls == int_cls, "");
    RELEASE_ASSERT(pathname->cls == str_cls, "");
    RELEASE_ASSERT(pathname->size(), "");

    if (type->n == SearchResult::PKG_DIRECTORY) {
        RELEASE_ASSERT(suffix->cls == str_cls, "");
        RELEASE_ASSERT(suffix->s().empty(), "");
        RELEASE_ASSERT(mode->s().empty(), "");
        RELEASE_ASSERT(_file == None, "");
        return createAndRunModule(name, (llvm::Twine(pathname->s()) + "/__init__.py").str(), pathname->s());
    } else if (type->n == SearchResult::PY_SOURCE) {
        RELEASE_ASSERT(_file->cls == file_cls, "");
        return createAndRunModule(name, pathname->s());
    }

    Py_FatalError("unimplemented");
}

Box* impLoadSource(Box* _name, Box* _pathname, Box* _file) {
    RELEASE_ASSERT(!_file, "'file' argument not support yet");

    RELEASE_ASSERT(_name->cls == str_cls, "");
    RELEASE_ASSERT(_pathname->cls == str_cls, "");

    return createAndRunModule(static_cast<BoxedString*>(_name), static_cast<BoxedString*>(_pathname)->s());
}

Box* impLoadDynamic(Box* _name, Box* _pathname, Box* _file) {
    RELEASE_ASSERT(_name->cls == str_cls, "");
    RELEASE_ASSERT(_pathname->cls == str_cls, "");
    RELEASE_ASSERT(_file == None, "");

    BoxedString* name = (BoxedString*)_name;
    BoxedString* pathname = (BoxedString*)_pathname;

    const char* lastdot = strrchr(name->s().data(), '.');

    const char* shortname;
    if (lastdot == NULL) {
        shortname = name->s().data();
    } else {
        shortname = lastdot + 1;
    }

    return importCExtension(name, shortname, pathname->s());
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

    if (should_add_bss) {
        // only track void* aligned memory
        bss_start = (bss_start + (sizeof(void*) - 1)) & ~(sizeof(void*) - 1);
        bss_end -= bss_end % sizeof(void*);
        gc::registerPotentialRootRange((void*)bss_start, (void*)bss_end);
    }
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
    return m;
}

Box* impGetSuffixes() {
    BoxedList* list = new BoxedList;
    // For now only add *.py
    listAppendInternal(list, BoxedTuple::create({ boxString(".py"), boxString("U"), boxInt(SearchResult::PY_SOURCE) }));
    return list;
}

Box* impAcquireLock() {
    _PyImport_AcquireLock();
    checkAndThrowCAPIException();
    return None;
}

Box* impReleaseLock() {
    _PyImport_ReleaseLock();
    checkAndThrowCAPIException();
    return None;
}

Box* impNewModule(Box* _name) {
    if (!PyString_Check(_name))
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(_name));

    BoxedModule* module = new BoxedModule();
    moduleInit(module, _name);
    return module;
}

Box* impIsBuiltin(Box* _name) {
    if (!PyString_Check(_name))
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(_name));

    static BoxedString* builtin_module_names_str = internStringImmortal("builtin_module_names");
    BoxedTuple* builtin_modules = (BoxedTuple*)sys_module->getattr(builtin_module_names_str);
    RELEASE_ASSERT(PyTuple_Check(builtin_modules), "");
    for (Box* m : builtin_modules->pyElements()) {
        if (compare(m, _name, AST_TYPE::Eq) == True)
            return boxInt(-1); // CPython returns 1 for modules which can get reinitialized.
    }
    return boxInt(0);
}

Box* impIsFrozen(Box* name) {
    if (!PyString_Check(name))
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(name));
    return False;
}

void setupImport() {
    BoxedModule* imp_module
        = createModule(boxString("imp"), NULL, "'This module provides the components needed to build your own\n"
                                               "__import__ function.  Undocumented functions are obsolete.'");

    imp_module->giveAttr("PY_SOURCE", boxInt(SearchResult::PY_SOURCE));
    imp_module->giveAttr("PY_COMPILED", boxInt(SearchResult::PY_COMPILED));
    imp_module->giveAttr("C_EXTENSION", boxInt(SearchResult::C_EXTENSION));
    imp_module->giveAttr("PKG_DIRECTORY", boxInt(SearchResult::PKG_DIRECTORY));
    imp_module->giveAttr("C_BUILTIN", boxInt(SearchResult::C_BUILTIN));
    imp_module->giveAttr("PY_FROZEN", boxInt(SearchResult::PY_FROZEN));

    null_importer_cls = BoxedClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(Box), false, "NullImporter");
    null_importer_cls->giveAttr(
        "__init__", new BoxedFunction(boxRTFunction((void*)nullImporterInit, NONE, 2, false, false), { None }));
    null_importer_cls->giveAttr("find_module", new BoxedBuiltinFunctionOrMethod(
                                                   boxRTFunction((void*)nullImporterFindModule, NONE, 2, false, false),
                                                   "find_module", { None }));
    null_importer_cls->freeze();
    imp_module->giveAttr("NullImporter", null_importer_cls);

    CLFunction* find_module_func
        = boxRTFunction((void*)impFindModule, UNKNOWN, 2, false, false, ParamNames({ "name", "path" }, "", ""));
    imp_module->giveAttr("find_module", new BoxedBuiltinFunctionOrMethod(find_module_func, "find_module", { None }));

    CLFunction* load_module_func = boxRTFunction((void*)impLoadModule, UNKNOWN, 4,
                                                 ParamNames({ "name", "file", "pathname", "description" }, "", ""));
    imp_module->giveAttr("load_module", new BoxedBuiltinFunctionOrMethod(load_module_func, "load_module"));
    imp_module->giveAttr("load_source",
                         new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)impLoadSource, UNKNOWN, 3, false, false),
                                                          "load_source", { NULL }));

    CLFunction* load_dynamic_func = boxRTFunction((void*)impLoadDynamic, UNKNOWN, 3, false, false,
                                                  ParamNames({ "name", "pathname", "file" }, "", ""));
    imp_module->giveAttr("load_dynamic", new BoxedBuiltinFunctionOrMethod(load_dynamic_func, "load_dynamic", { None }));

    imp_module->giveAttr("get_suffixes", new BoxedBuiltinFunctionOrMethod(
                                             boxRTFunction((void*)impGetSuffixes, UNKNOWN, 0), "get_suffixes"));
    imp_module->giveAttr("acquire_lock", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)impAcquireLock, NONE, 0),
                                                                          "acquire_lock"));
    imp_module->giveAttr("release_lock", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)impReleaseLock, NONE, 0),
                                                                          "release_lock"));

    imp_module->giveAttr("new_module",
                         new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)impNewModule, MODULE, 1), "new_module"));
    imp_module->giveAttr(
        "is_builtin", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)impIsBuiltin, BOXED_INT, 1), "is_builtin"));
    imp_module->giveAttr(
        "is_frozen", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)impIsFrozen, BOXED_BOOL, 1), "is_frozen"));
}
}
