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

#include <algorithm>
#include <cmath>
#include <sstream>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/unwinding.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/file.h"
#include "runtime/inline/boxing.h"
#include "runtime/int.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* sys_module;
BoxedDict* sys_modules_dict;

extern "C" {
// supposed to be exposed through sys.flags
int Py_BytesWarningFlag = 0;
}

Box* sysExcInfo() {
    ExcInfo* exc = getFrameExcInfo();
    assert(exc->type);
    assert(exc->value);
    assert(exc->traceback);
    return new BoxedTuple({ exc->type, exc->value, exc->traceback });
}

Box* sysExcClear() {
    ExcInfo* exc = getFrameExcInfo();
    assert(exc->type);
    assert(exc->value);
    assert(exc->traceback);

    exc->type = None;
    exc->value = None;
    exc->traceback = None;

    return None;
}

static Box* sysExit(Box* arg) {
    assert(arg);
    Box* exc = runtimeCall(SystemExit, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    // TODO this should be handled by the SystemExit constructor
    exc->giveAttr("code", arg);

    raiseExc(exc);
}

BoxedDict* getSysModulesDict() {
    // PyPy's behavior: fetch from sys.modules each time:
    // Box *_sys_modules = sys_module->getattr("modules");
    // assert(_sys_modules);
    // assert(_sys_modules->cls == dict_cls);
    // return static_cast<BoxedDict*>(_sys_modules);

    // CPython's behavior: return an internalized reference:
    return sys_modules_dict;
}

BoxedList* getSysPath() {
    // Unlike sys.modules, CPython handles sys.path by fetching it each time:
    Box* _sys_path = sys_module->getattr("path");
    assert(_sys_path);

    if (_sys_path->cls != list_cls) {
        fprintf(stderr, "RuntimeError: sys.path must be a list of directory name\n");
        raiseExcHelper(RuntimeError, "");
    }

    assert(_sys_path->cls == list_cls);
    return static_cast<BoxedList*>(_sys_path);
}

Box* getSysStdout() {
    Box* sys_stdout = sys_module->getattr("stdout");
    RELEASE_ASSERT(sys_stdout, "lost sys.stdout??");
    return sys_stdout;
}

Box* sysGetFrame(Box* val) {
    int depth = 0;
    if (val) {
        if (!isSubclass(val->cls, int_cls)) {
            raiseExcHelper(TypeError, "TypeError: an integer is required");
        }
        depth = static_cast<BoxedInt*>(val)->n;
    }

    Box* frame = getFrame(depth);
    if (!frame) {
        raiseExcHelper(ValueError, "call stack is not deep enough");
    }
    return frame;
}

Box* sysGetDefaultEncoding() {
    return boxStrConstant(PyUnicode_GetDefaultEncoding());
}

Box* sysGetFilesystemEncoding() {
    if (Py_FileSystemDefaultEncoding)
        return boxStrConstant(Py_FileSystemDefaultEncoding);
    return None;
}

extern "C" int PySys_SetObject(const char* name, PyObject* v) noexcept {
    try {
        if (!v) {
            if (sys_module->getattr(name))
                sys_module->delattr(name, NULL);
        } else
            sys_module->setattr(name, v, NULL);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" PyObject* PySys_GetObject(const char* name) noexcept {
    return sys_module->getattr(name);
}

static void mywrite(const char* name, FILE* fp, const char* format, va_list va) noexcept {
    PyObject* file;
    PyObject* error_type, *error_value, *error_traceback;

    PyErr_Fetch(&error_type, &error_value, &error_traceback);
    file = PySys_GetObject(name);
    if (file == NULL || PyFile_AsFile(file) == fp)
        vfprintf(fp, format, va);
    else {
        char buffer[1001];
        const int written = PyOS_vsnprintf(buffer, sizeof(buffer), format, va);
        if (PyFile_WriteString(buffer, file) != 0) {
            PyErr_Clear();
            fputs(buffer, fp);
        }
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            const char* truncated = "... truncated";
            if (PyFile_WriteString(truncated, file) != 0) {
                PyErr_Clear();
                fputs(truncated, fp);
            }
        }
    }
    PyErr_Restore(error_type, error_value, error_traceback);
}

extern "C" void PySys_WriteStdout(const char* format, ...) noexcept {
    va_list va;

    va_start(va, format);
    mywrite("stdout", stdout, format, va);
    va_end(va);
}

extern "C" void PySys_WriteStderr(const char* format, ...) noexcept {
    va_list va;

    va_start(va, format);
    mywrite("stderr", stderr, format, va);
    va_end(va);
}

void addToSysArgv(const char* str) {
    Box* sys_argv = sys_module->getattr("argv");
    assert(sys_argv);
    assert(sys_argv->cls == list_cls);
    listAppendInternal(sys_argv, boxStrConstant(str));
}

void appendToSysPath(const std::string& path) {
    BoxedList* sys_path = getSysPath();
    listAppendInternal(sys_path, boxStringPtr(&path));
}

void prependToSysPath(const std::string& path) {
    BoxedList* sys_path = getSysPath();
    static std::string attr = "insert";
    callattr(sys_path, &attr, CallattrFlags({.cls_only = false, .null_on_nonexistent = false }), ArgPassSpec(2),
             boxInt(0), new BoxedString(path), NULL, NULL, NULL);
}

static BoxedClass* sys_flags_cls;
class BoxedSysFlags : public Box {
public:
    Box* division_warning, *bytes_warning, *no_user_site;

    BoxedSysFlags() {
        auto zero = boxInt(0);
        assert(zero);
        division_warning = zero;
        bytes_warning = zero;
        no_user_site = zero;
    }

    DEFAULT_CLASS(sys_flags_cls);

    static void gcHandler(GCVisitor* v, Box* _b) {
        assert(_b->cls == sys_flags_cls);
        boxGCHandler(v, _b);

        BoxedSysFlags* self = static_cast<BoxedSysFlags*>(_b);
        v->visit(self->division_warning);
        v->visit(self->bytes_warning);
        v->visit(self->no_user_site);
    }

    static Box* __new__(Box* cls, Box* args, Box* kwargs) {
        raiseExcHelper(TypeError, "cannot create 'sys.flags' instances");
    }
};

static std::string generateVersionString() {
    std::ostringstream oss;
    oss << PYTHON_VERSION_MAJOR << '.' << PYTHON_VERSION_MINOR << '.' << PYTHON_VERSION_MICRO;
    oss << '\n';
    oss << "[Pyston " << PYSTON_VERSION_MAJOR << '.' << PYSTON_VERSION_MINOR << "]";
    return oss.str();
}

static bool isLittleEndian() {
    unsigned long number = 1;
    char* s = (char*)&number;
    return s[0] != 0;
}

void setupSys() {
    sys_modules_dict = new BoxedDict();
    gc::registerPermanentRoot(sys_modules_dict);

    // This is ok to call here because we've already created the sys_modules_dict
    sys_module = createModule("sys", "__builtin__");

    sys_module->giveAttr("modules", sys_modules_dict);

    BoxedList* sys_path = new BoxedList();
    sys_module->giveAttr("path", sys_path);

    sys_module->giveAttr("argv", new BoxedList());

    sys_module->giveAttr("stdout", new BoxedFile(stdout, "<stdout>", "w"));
    sys_module->giveAttr("stdin", new BoxedFile(stdin, "<stdin>", "r"));
    sys_module->giveAttr("stderr", new BoxedFile(stderr, "<stderr>", "w"));

    sys_module->giveAttr(
        "exc_info", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysExcInfo, BOXED_TUPLE, 0), "exc_info"));
    sys_module->giveAttr("exc_clear",
                         new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysExcClear, NONE, 0), "exc_clear"));
    sys_module->giveAttr("exit", new BoxedBuiltinFunctionOrMethod(
                                     boxRTFunction((void*)sysExit, NONE, 1, 1, false, false), "exit", { None }));

    sys_module->giveAttr("warnoptions", new BoxedList());
    sys_module->giveAttr("py3kwarning", False);
    sys_module->giveAttr("byteorder", new BoxedString(isLittleEndian() ? "little" : "big"));

    sys_module->giveAttr("platform", boxStrConstant("unknown")); // seems like a reasonable, if poor, default

    llvm::SmallString<128> main_fn;
    // TODO supposed to pass argv0, main_addr to this function:
    main_fn = llvm::sys::fs::getMainExecutable(NULL, NULL);
    sys_module->giveAttr("executable", boxString(main_fn.str()));

    sys_module->giveAttr("_getframe",
                         new BoxedFunction(boxRTFunction((void*)sysGetFrame, UNKNOWN, 1, 1, false, false), { NULL }));
    sys_module->giveAttr(
        "getdefaultencoding",
        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysGetDefaultEncoding, STR, 0), "getdefaultencoding"));

    sys_module->giveAttr("getfilesystemencoding",
                         new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysGetFilesystemEncoding, STR, 0),
                                                          "getfilesystemencoding"));

    sys_module->giveAttr("meta_path", new BoxedList());
    sys_module->giveAttr("path_hooks", new BoxedList());
    sys_module->giveAttr("path_importer_cache", new BoxedDict());

    sys_module->giveAttr("prefix", boxStrConstant(Py_GetPrefix()));
    sys_module->giveAttr("exec_prefix", boxStrConstant(Py_GetExecPrefix()));

    sys_module->giveAttr("copyright",
                         boxStrConstant("Copyright 2014-2015 Dropbox.\nAll Rights Reserved.\n\nCopyright (c) 2001-2014 "
                                        "Python Software Foundation.\nAll Rights Reserved.\n\nCopyright (c) 2000 "
                                        "BeOpen.com.\nAll Rights Reserved.\n\nCopyright (c) 1995-2001 Corporation for "
                                        "National Research Initiatives.\nAll Rights Reserved.\n\nCopyright (c) "
                                        "1991-1995 Stichting Mathematisch Centrum, Amsterdam.\nAll Rights Reserved."));

    sys_module->giveAttr("version", boxString(generateVersionString()));
    sys_module->giveAttr("hexversion", boxInt(PY_VERSION_HEX));
    // TODO: this should be a "sys.version_info" object, not just a tuple (ie can access fields by name)
    sys_module->giveAttr("version_info",
                         new BoxedTuple({ boxInt(PYTHON_VERSION_MAJOR), boxInt(PYTHON_VERSION_MINOR),
                                          boxInt(PYTHON_VERSION_MICRO), boxStrConstant("beta"), boxInt(0) }));

    sys_module->giveAttr("maxint", boxInt(PYSTON_INT_MAX));
    sys_module->giveAttr("maxsize", boxInt(PY_SSIZE_T_MAX));

    sys_flags_cls = new BoxedHeapClass(object_cls, BoxedSysFlags::gcHandler, 0, 0, sizeof(BoxedSysFlags), false,
                                       new BoxedString("flags"));
    sys_flags_cls->giveAttr("__new__",
                            new BoxedFunction(boxRTFunction((void*)BoxedSysFlags::__new__, UNKNOWN, 1, 0, true, true)));
#define ADD(name)                                                                                                      \
    sys_flags_cls->giveAttr(STRINGIFY(name),                                                                           \
                            new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSysFlags, name)))
    ADD(division_warning);
    ADD(bytes_warning);
    ADD(no_user_site);
#undef ADD

    sys_flags_cls->tp_mro = new BoxedTuple({ sys_flags_cls, object_cls });
    sys_flags_cls->freeze();

    sys_module->giveAttr("flags", new BoxedSysFlags());
}

void setupSysEnd() {
    BoxedTuple::GCVector builtin_module_names;
    for (auto& p : sys_modules_dict->d) {
        builtin_module_names.push_back(p.first);
    }

    std::sort<decltype(builtin_module_names)::iterator, PyLt>(builtin_module_names.begin(), builtin_module_names.end(),
                                                              PyLt());

    sys_module->giveAttr("builtin_module_names", new BoxedTuple(std::move(builtin_module_names)));
    sys_flags_cls->finishInitialization();
}
}
