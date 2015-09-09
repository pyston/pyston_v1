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
#include <cfloat>
#include <cmath>
#include <langinfo.h>
#include <sstream>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "Python.h"
#include "structseq.h"

#include "capi/types.h"
#include "codegen/unwinding.h"
#include "core/types.h"
#include "runtime/file.h"
#include "runtime/inline/boxing.h"
#include "runtime/inline/list.h"
#include "runtime/int.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* sys_module;
BoxedDict* sys_modules_dict;

extern "C" {
// supposed to be exposed through sys.flags
int Py_BytesWarningFlag = 0;
int Py_HashRandomizationFlag = 0;
}

Box* sysExcInfo() {
    ExcInfo* exc = getFrameExcInfo();
    assert(exc->type);
    assert(exc->value);
    assert(exc->traceback);
    return BoxedTuple::create({ exc->type, exc->value, exc->traceback });
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
    Box* _sys_path = sys_module->getattr(internStringMortal("path"));
    assert(_sys_path);

    if (_sys_path->cls != list_cls) {
        raiseExcHelper(RuntimeError, "sys.path must be a list of directory names");
    }

    assert(_sys_path->cls == list_cls);
    return static_cast<BoxedList*>(_sys_path);
}

Box* getSysStdout() {
    Box* sys_stdout = sys_module->getattr(internStringMortal("stdout"));
    RELEASE_ASSERT(sys_stdout, "lost sys.stdout??");
    return sys_stdout;
}

Box* sysGetFrame(Box* val) {
    int depth = 0;
    if (val) {
        if (!PyInt_Check(val)) {
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
    return boxString(PyUnicode_GetDefaultEncoding());
}

Box* sysGetFilesystemEncoding() {
    if (Py_FileSystemDefaultEncoding)
        return boxString(Py_FileSystemDefaultEncoding);
    return None;
}

Box* sysGetRecursionLimit() {
    return PyInt_FromLong(Py_GetRecursionLimit());
}

extern "C" int PySys_SetObject(const char* name, PyObject* v) noexcept {
    try {
        if (!v) {
            if (sys_module->getattr(internStringMortal(name)))
                sys_module->delattr(internStringMortal(name), NULL);
        } else
            sys_module->setattr(internStringMortal(name), v, NULL);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" PyObject* PySys_GetObject(const char* name) noexcept {
    return sys_module->getattr(internStringMortal(name));
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
    Box* sys_argv = sys_module->getattr(internStringMortal("argv"));
    assert(sys_argv);
    assert(sys_argv->cls == list_cls);
    listAppendInternal(sys_argv, boxString(str));
}

void appendToSysPath(llvm::StringRef path) {
    BoxedList* sys_path = getSysPath();
    listAppendInternal(sys_path, boxString(path));
}

void prependToSysPath(llvm::StringRef path) {
    BoxedList* sys_path = getSysPath();
    static BoxedString* insert_str = internStringImmortal("insert");
    CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(2) };
    callattr(sys_path, insert_str, callattr_flags, boxInt(0), boxString(path), NULL, NULL, NULL);
}

static BoxedClass* sys_flags_cls;
class BoxedSysFlags : public Box {
public:
    Box* division_warning, *bytes_warning, *no_user_site, *optimize;

    BoxedSysFlags() {
        auto zero = boxInt(0);
        assert(zero);
        division_warning = zero;
        bytes_warning = zero;
        no_user_site = zero;
        optimize = zero;
    }

    DEFAULT_CLASS(sys_flags_cls);

    static void gcHandler(GCVisitor* v, Box* _b) {
        assert(_b->cls == sys_flags_cls);
        Box::gcHandler(v, _b);

        BoxedSysFlags* self = static_cast<BoxedSysFlags*>(_b);
        v->visit(&self->division_warning);
        v->visit(&self->bytes_warning);
        v->visit(&self->no_user_site);
        v->visit(&self->optimize);
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

extern "C" const char* Py_GetVersion(void) noexcept {
    static std::string version = generateVersionString();
    return version.c_str();
}

static bool isLittleEndian() {
    unsigned long number = 1;
    char* s = (char*)&number;
    return s[0] != 0;
}

void setEncodingAndErrors() {
    // Adapted from pythonrun.c in CPython, with modifications for Pyston.

    char* p;
    char* icodeset = nullptr;
    char* codeset = nullptr;
    char* errors = nullptr;
    int free_codeset = 0;
    int overridden = 0;
    PyObject* sys_stream, *sys_isatty;
    char* saved_locale, *loc_codeset;

    if ((p = Py_GETENV("PYTHONIOENCODING")) && *p != '\0') {
        p = icodeset = codeset = strdup(p);
        free_codeset = 1;
        errors = strchr(p, ':');
        if (errors) {
            *errors = '\0';
            errors++;
        }
        overridden = 1;
    }

#if defined(Py_USING_UNICODE) && defined(HAVE_LANGINFO_H) && defined(CODESET)
    /* On Unix, set the file system encoding according to the
       user's preference, if the CODESET names a well-known
       Python codec, and Py_FileSystemDefaultEncoding isn't
       initialized by other means. Also set the encoding of
       stdin and stdout if these are terminals, unless overridden.  */

    if (!overridden || !Py_FileSystemDefaultEncoding) {
        saved_locale = strdup(setlocale(LC_CTYPE, NULL));
        setlocale(LC_CTYPE, "");
        loc_codeset = nl_langinfo(CODESET);
        if (loc_codeset && *loc_codeset) {
            PyObject* enc = PyCodec_Encoder(loc_codeset);
            if (enc) {
                loc_codeset = strdup(loc_codeset);
                Py_DECREF(enc);
            } else {
                if (PyErr_ExceptionMatches(PyExc_LookupError)) {
                    PyErr_Clear();
                    loc_codeset = NULL;
                } else {
                    PyErr_Print();
                    exit(1);
                }
            }
        } else
            loc_codeset = NULL;
        setlocale(LC_CTYPE, saved_locale);
        free(saved_locale);

        if (!overridden) {
            codeset = icodeset = loc_codeset;
            free_codeset = 1;
        }

        /* Initialize Py_FileSystemDefaultEncoding from
           locale even if PYTHONIOENCODING is set. */
        if (!Py_FileSystemDefaultEncoding) {
            Py_FileSystemDefaultEncoding = loc_codeset;
            if (!overridden)
                free_codeset = 0;
        }
    }
#endif

#ifdef MS_WINDOWS
    if (!overridden) {
        icodeset = ibuf;
        codeset = buf;
        sprintf(ibuf, "cp%d", GetConsoleCP());
        sprintf(buf, "cp%d", GetConsoleOutputCP());
    }
#endif

    if (codeset) {
        sys_stream = PySys_GetObject("stdin");
        sys_isatty = PyObject_CallMethod(sys_stream, "isatty", "");
        if (!sys_isatty)
            PyErr_Clear();
        if ((overridden || (sys_isatty && PyObject_IsTrue(sys_isatty))) && PyFile_Check(sys_stream)) {
            if (!PyFile_SetEncodingAndErrors(sys_stream, icodeset, errors))
                Py_FatalError("Cannot set codeset of stdin");
        }
        Py_XDECREF(sys_isatty);

        sys_stream = PySys_GetObject("stdout");
        sys_isatty = PyObject_CallMethod(sys_stream, "isatty", "");
        if (!sys_isatty)
            PyErr_Clear();
        if ((overridden || (sys_isatty && PyObject_IsTrue(sys_isatty))) && PyFile_Check(sys_stream)) {
            if (!PyFile_SetEncodingAndErrors(sys_stream, codeset, errors))
                Py_FatalError("Cannot set codeset of stdout");
        }
        Py_XDECREF(sys_isatty);

        sys_stream = PySys_GetObject("stderr");
        sys_isatty = PyObject_CallMethod(sys_stream, "isatty", "");
        if (!sys_isatty)
            PyErr_Clear();
        if ((overridden || (sys_isatty && PyObject_IsTrue(sys_isatty))) && PyFile_Check(sys_stream)) {
            if (!PyFile_SetEncodingAndErrors(sys_stream, codeset, errors))
                Py_FatalError("Cannot set codeset of stderr");
        }
        Py_XDECREF(sys_isatty);

        if (free_codeset)
            free(codeset);
    }
}

extern "C" const char* Py_GetPlatform() noexcept {
// cpython does this check in their configure script
#if defined(__linux__)
    return "linux2";
#elif defined(__APPLE__) && defined(__MACH__)
    return "darwin";
#else
    // cpython also supports returning "atheos", "irix6", "win32", or
    // whatever the user can get PLATFORM to be #defined as at
    // build-time.
    return "unknown";
#endif
}

static PyObject* sys_excepthook(PyObject* self, PyObject* args) noexcept {
    PyObject* exc, *value, *tb;
    if (!PyArg_UnpackTuple(args, "excepthook", 3, 3, &exc, &value, &tb))
        return NULL;
    PyErr_Display(exc, value, tb);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* sys_displayhook(PyObject* self, PyObject* o) noexcept {
    PyObject* outf;

    // Pyston change: we currently hardcode the builtins module
    /*
    PyInterpreterState* interp = PyThreadState_GET()->interp;
    PyObject* modules = interp->modules;
    PyObject* builtins = PyDict_GetItemString(modules, "__builtin__");

    if (builtins == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "lost __builtin__");
        return NULL;
    }
    */
    PyObject* builtins = builtins_module;

    /* Print value except if None */
    /* After printing, also assign to '_' */
    /* Before, set '_' to None to avoid recursion */
    if (o == Py_None) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    if (PyObject_SetAttrString(builtins, "_", Py_None) != 0)
        return NULL;
    if (Py_FlushLine() != 0)
        return NULL;
    outf = PySys_GetObject("stdout");
    if (outf == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "lost sys.stdout");
        return NULL;
    }
    if (PyFile_WriteObject(o, outf, 0) != 0)
        return NULL;
    PyFile_SoftSpace(outf, 1);
    if (Py_FlushLine() != 0)
        return NULL;
    if (PyObject_SetAttrString(builtins, "_", o) != 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(excepthook_doc, "excepthook(exctype, value, traceback) -> None\n"
                             "\n"
                             "Handle an exception by displaying it with a traceback on sys.stderr.\n");

PyDoc_STRVAR(displayhook_doc, "displayhook(object) -> None\n"
                              "\n"
                              "Print an object to sys.stdout and also save it in __builtin__._\n");

static PyMethodDef sys_methods[] = {
    { "excepthook", sys_excepthook, METH_VARARGS, excepthook_doc },
    { "displayhook", sys_displayhook, METH_O, displayhook_doc },
};

PyDoc_STRVAR(version_info__doc__, "sys.version_info\n\
        \n\
        Version information as a named tuple.");

static struct _typeobject VersionInfoType;

static PyStructSequence_Field version_info_fields[]
    = { { "major", "Major release number" },
        { "minor", "Minor release number" },
        { "micro", "Patch release number" },
        { "releaselevel", "'alpha', 'beta', 'candidate', or 'release'" },
        { "serial", "Serial release number" },
        { 0, 0 } };

static PyStructSequence_Desc version_info_desc = { "sys.version_info",  /* name */
                                                   version_info__doc__, /* doc */
                                                   version_info_fields, /* fields */
                                                   5 };

static PyObject* make_version_info(void) noexcept {
    PyObject* version_info;
    const char* s;
    int pos = 0;
    version_info = PyStructSequence_New(&VersionInfoType);
    if (version_info == NULL) {
        return NULL;
    }

/*
 * These release level checks are mutually exclusive and cover
 * the field, so don't get too fancy with the pre-processor!
 */
#if PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_ALPHA
    s = "alpha";
#elif PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_BETA
    s = "beta";
#elif PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_GAMMA
    s = "candidate";
#elif PY_RELEASE_LEVEL == PY_RELEASE_LEVEL_FINAL
    s = "final";
#endif

#define SetIntItem(flag) PyStructSequence_SET_ITEM(version_info, pos++, PyInt_FromLong(flag))
#define SetStrItem(flag) PyStructSequence_SET_ITEM(version_info, pos++, PyString_FromString(flag))

    SetIntItem(PY_MAJOR_VERSION);
    SetIntItem(PY_MINOR_VERSION);
    SetIntItem(PY_MICRO_VERSION);
    SetStrItem(s);
    SetIntItem(PY_RELEASE_SERIAL);
#undef SetIntItem
#undef SetStrItem

    if (PyErr_Occurred()) {
        Py_CLEAR(version_info);
        return NULL;
    }
    return version_info;
}

static struct _typeobject FloatInfoType;

PyDoc_STRVAR(floatinfo__doc__, "sys.float_info\n\
\n\
A structseq holding information about the float type. It contains low level\n\
information about the precision and internal representation. Please study\n\
your system's :file:`float.h` for more information.");

static PyStructSequence_Field floatinfo_fields[]
    = { { "max", "DBL_MAX -- maximum representable finite float" },
        { "max_exp", "DBL_MAX_EXP -- maximum int e such that radix**(e-1) "
                     "is representable" },
        { "max_10_exp", "DBL_MAX_10_EXP -- maximum int e such that 10**e "
                        "is representable" },
        { "min", "DBL_MIN -- Minimum positive normalizer float" },
        { "min_exp", "DBL_MIN_EXP -- minimum int e such that radix**(e-1) "
                     "is a normalized float" },
        { "min_10_exp", "DBL_MIN_10_EXP -- minimum int e such that 10**e is "
                        "a normalized" },
        { "dig", "DBL_DIG -- digits" },
        { "mant_dig", "DBL_MANT_DIG -- mantissa digits" },
        { "epsilon", "DBL_EPSILON -- Difference between 1 and the next "
                     "representable float" },
        { "radix", "FLT_RADIX -- radix of exponent" },
        { "rounds", "FLT_ROUNDS -- addition rounds" },
        { NULL, NULL } };

static PyStructSequence_Desc float_info_desc = { "sys.float_info", /* name */
                                                 floatinfo__doc__, /* doc */
                                                 floatinfo_fields, /* fields */
                                                 11 };

PyObject* PyFloat_GetInfo(void) {
    PyObject* floatinfo;
    int pos = 0;

    floatinfo = PyStructSequence_New(&FloatInfoType);
    if (floatinfo == NULL) {
        return NULL;
    }

#define SetIntFlag(flag) PyStructSequence_SET_ITEM(floatinfo, pos++, PyInt_FromLong(flag))
#define SetDblFlag(flag) PyStructSequence_SET_ITEM(floatinfo, pos++, PyFloat_FromDouble(flag))

    SetDblFlag(DBL_MAX);
    SetIntFlag(DBL_MAX_EXP);
    SetIntFlag(DBL_MAX_10_EXP);
    SetDblFlag(DBL_MIN);
    SetIntFlag(DBL_MIN_EXP);
    SetIntFlag(DBL_MIN_10_EXP);
    SetIntFlag(DBL_DIG);
    SetIntFlag(DBL_MANT_DIG);
    SetDblFlag(DBL_EPSILON);
    SetIntFlag(FLT_RADIX);
    SetIntFlag(FLT_ROUNDS);
#undef SetIntFlag
#undef SetDblFlag

    if (PyErr_Occurred()) {
        Py_CLEAR(floatinfo);
        return NULL;
    }
    return floatinfo;
}

void setupSys() {
    sys_modules_dict = new BoxedDict();
    gc::registerPermanentRoot(sys_modules_dict);

    // This is ok to call here because we've already created the sys_modules_dict
    sys_module = createModule(boxString("sys"));

    sys_module->giveAttr("modules", sys_modules_dict);

    BoxedList* sys_path = new BoxedList();
    sys_module->giveAttr("path", sys_path);

    sys_module->giveAttr("argv", new BoxedList());

    sys_module->giveAttr("stdout", new BoxedFile(stdout, "<stdout>", "w"));
    sys_module->giveAttr("stdin", new BoxedFile(stdin, "<stdin>", "r"));
    sys_module->giveAttr("stderr", new BoxedFile(stderr, "<stderr>", "w"));
    sys_module->giveAttr("__stdout__", sys_module->getattr(internStringMortal("stdout")));
    sys_module->giveAttr("__stdin__", sys_module->getattr(internStringMortal("stdin")));
    sys_module->giveAttr("__stderr__", sys_module->getattr(internStringMortal("stderr")));

    sys_module->giveAttr(
        "exc_info", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysExcInfo, BOXED_TUPLE, 0), "exc_info"));
    sys_module->giveAttr("exc_clear",
                         new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysExcClear, NONE, 0), "exc_clear"));
    sys_module->giveAttr("exit", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysExit, NONE, 1, false, false),
                                                                  "exit", { None }));

    sys_module->giveAttr("warnoptions", new BoxedList());
    sys_module->giveAttr("py3kwarning", False);
    sys_module->giveAttr("byteorder", boxString(isLittleEndian() ? "little" : "big"));

    sys_module->giveAttr("platform", boxString(Py_GetPlatform()));

    sys_module->giveAttr("executable", boxString(Py_GetProgramFullPath()));

    sys_module->giveAttr("_getframe",
                         new BoxedFunction(boxRTFunction((void*)sysGetFrame, UNKNOWN, 1, false, false), { NULL }));
    sys_module->giveAttr(
        "getdefaultencoding",
        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysGetDefaultEncoding, STR, 0), "getdefaultencoding"));

    sys_module->giveAttr("getfilesystemencoding",
                         new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysGetFilesystemEncoding, STR, 0),
                                                          "getfilesystemencoding"));

    sys_module->giveAttr(
        "getrecursionlimit",
        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sysGetRecursionLimit, UNKNOWN, 0), "getrecursionlimit"));

    sys_module->giveAttr("meta_path", new BoxedList());
    sys_module->giveAttr("path_hooks", new BoxedList());
    sys_module->giveAttr("path_importer_cache", new BoxedDict());

    // As we don't support compile() etc yet force 'dont_write_bytecode' to true.
    sys_module->giveAttr("dont_write_bytecode", True);

    sys_module->giveAttr("prefix", boxString(Py_GetPrefix()));
    sys_module->giveAttr("exec_prefix", boxString(Py_GetExecPrefix()));

    sys_module->giveAttr("copyright",
                         boxString("Copyright 2014-2015 Dropbox.\nAll Rights Reserved.\n\nCopyright (c) 2001-2014 "
                                   "Python Software Foundation.\nAll Rights Reserved.\n\nCopyright (c) 2000 "
                                   "BeOpen.com.\nAll Rights Reserved.\n\nCopyright (c) 1995-2001 Corporation for "
                                   "National Research Initiatives.\nAll Rights Reserved.\n\nCopyright (c) "
                                   "1991-1995 Stichting Mathematisch Centrum, Amsterdam.\nAll Rights Reserved."));

    sys_module->giveAttr("version", boxString(generateVersionString()));
    sys_module->giveAttr("hexversion", boxInt(PY_VERSION_HEX));
    sys_module->giveAttr("maxint", boxInt(PYSTON_INT_MAX));
    sys_module->giveAttr("maxsize", boxInt(PY_SSIZE_T_MAX));

    sys_flags_cls = new (0) BoxedHeapClass(object_cls, BoxedSysFlags::gcHandler, 0, 0, sizeof(BoxedSysFlags), false,
                                           static_cast<BoxedString*>(boxString("flags")));
    sys_flags_cls->giveAttr("__new__",
                            new BoxedFunction(boxRTFunction((void*)BoxedSysFlags::__new__, UNKNOWN, 1, true, true)));
#define ADD(name)                                                                                                      \
    sys_flags_cls->giveAttr(STRINGIFY(name),                                                                           \
                            new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSysFlags, name)))
    ADD(division_warning);
    ADD(bytes_warning);
    ADD(no_user_site);
    ADD(optimize);
#undef ADD

#define SET_SYS_FROM_STRING(key, value) sys_module->giveAttr((key), (value))
#ifdef Py_USING_UNICODE
    SET_SYS_FROM_STRING("maxunicode", PyInt_FromLong(PyUnicode_GetMax()));
#endif
    sys_flags_cls->tp_mro = BoxedTuple::create({ sys_flags_cls, object_cls });
    sys_flags_cls->freeze();

    for (auto& md : sys_methods) {
        sys_module->giveAttr(md.ml_name, new BoxedCApiFunction(&md, sys_module));
    }

    sys_module->giveAttr("__displayhook__", sys_module->getattr(internStringMortal("displayhook")));
    sys_module->giveAttr("flags", new BoxedSysFlags());
}

void setupSysEnd() {
    std::vector<Box*, StlCompatAllocator<Box*>> builtin_module_names;
    for (const auto& p : *sys_modules_dict) {
        builtin_module_names.push_back(p.first);
    }

    std::sort<decltype(builtin_module_names)::iterator, PyLt>(builtin_module_names.begin(), builtin_module_names.end(),
                                                              PyLt());

    sys_module->giveAttr("builtin_module_names",
                         BoxedTuple::create(builtin_module_names.size(), &builtin_module_names[0]));
    sys_flags_cls->finishInitialization();

    /* version_info */
    if (VersionInfoType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&VersionInfoType, &version_info_desc);
    /* prevent user from creating new instances */
    VersionInfoType.tp_init = NULL;
    VersionInfoType.tp_new = NULL;

    SET_SYS_FROM_STRING("version_info", make_version_info());

    /* float_info */
    if (FloatInfoType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&FloatInfoType, &float_info_desc);
    /* prevent user from creating new instances */
    FloatInfoType.tp_init = NULL;
    FloatInfoType.tp_new = NULL;

    SET_SYS_FROM_STRING("float_info", PyFloat_GetInfo());
}
}
