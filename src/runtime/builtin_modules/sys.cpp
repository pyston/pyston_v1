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
int Py_DivisionWarningFlag = 0;
int Py_HashRandomizationFlag = 0;
int Py_DebugFlag = 0;
int _Py_QnewFlag = 0;
int Py_DontWriteBytecodeFlag = 0;
int Py_NoUserSiteDirectory = 0;
}

Box* sysExcInfo() {
    ExcInfo* exc = getFrameExcInfo();
    assert(exc->type);
    assert(exc->value);
    Box* tb = exc->traceback ? exc->traceback : Py_None;
    return BoxedTuple::create({ exc->type, exc->value, tb });
}

Box* sysExcClear() {
    ExcInfo* exc = getFrameExcInfo();
    assert(exc->type);
    assert(exc->value);

    Box* old_type = exc->type;
    Box* old_value = exc->value;
    Box* old_traceback = exc->traceback;
    exc->type = incref(Py_None);
    exc->value = incref(Py_None);
    exc->traceback = NULL;
    Py_DECREF(old_type);
    Py_DECREF(old_value);
    Py_XDECREF(old_traceback);

    Py_RETURN_NONE;
}

static Box* sysExit(Box* arg) {
    assert(arg);
    Box* exc = runtimeCall(SystemExit, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    raiseExc(exc);
}

BORROWED(BoxedDict*) getSysModulesDict() {
    // PyPy's behavior: fetch from sys.modules each time:
    // Box *_sys_modules = sys_module->getattr("modules");
    // assert(_sys_modules);
    // assert(_sys_modules->cls == dict_cls);
    // return static_cast<BoxedDict*>(_sys_modules);

    // CPython's behavior: return an internalized reference:
    return sys_modules_dict;
}

BORROWED(BoxedList*) getSysPath() {
    // Unlike sys.modules, CPython handles sys.path by fetching it each time:
    auto path_str = getStaticString("path");

    Box* _sys_path = sys_module->getattr(path_str);
    assert(_sys_path);

    if (_sys_path->cls != list_cls) {
        raiseExcHelper(RuntimeError, "sys.path must be a list of directory names");
    }

    assert(_sys_path->cls == list_cls);
    return static_cast<BoxedList*>(_sys_path);
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
    return incref(frame);
}

Box* sysCurrentFrames() {
    Box* rtn = _PyThread_CurrentFrames();
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* sysGetDefaultEncoding() {
    return boxString(PyUnicode_GetDefaultEncoding());
}

Box* sysGetFilesystemEncoding() {
    if (Py_FileSystemDefaultEncoding)
        return boxString(Py_FileSystemDefaultEncoding);
    Py_RETURN_NONE;
}

Box* sysGetRecursionLimit() {
    return PyInt_FromLong(Py_GetRecursionLimit());
}

extern "C" int PySys_SetObject(const char* name, PyObject* v) noexcept {
    try {
        if (!v) {
            if (sys_module->getattr(autoDecref(internStringMortal(name))))
                sys_module->delattr(autoDecref(internStringMortal(name)), NULL);
        } else
            sys_module->setattr(autoDecref(internStringMortal(name)), v, NULL);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" BORROWED(PyObject*) PySys_GetObject(const char* name) noexcept {
    return sys_module->getattr(autoDecref(internStringMortal(name)));
}

extern "C" FILE* PySys_GetFile(char* name, FILE* def) noexcept {
    FILE* fp = NULL;
    PyObject* v = PySys_GetObject(name);
    if (v != NULL && PyFile_Check(v))
        fp = PyFile_AsFile(v);
    if (fp == NULL)
        fp = def;
    return fp;
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
    static BoxedString* argv_str = getStaticString("argv");
    Box* sys_argv = sys_module->getattr(argv_str);
    assert(sys_argv);
    assert(sys_argv->cls == list_cls);
    listAppendInternalStolen(sys_argv, boxString(str));
}

void appendToSysPath(llvm::StringRef path) {
    BoxedList* sys_path = getSysPath();
    listAppendInternalStolen(sys_path, boxString(path));
}

void prependToSysPath(llvm::StringRef path) {
    BoxedList* sys_path = getSysPath();
    static BoxedString* insert_str = getStaticString("insert");
    CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(2) };
    autoDecref(callattr(sys_path, insert_str, callattr_flags, autoDecref(boxInt(0)), autoDecref(boxString(path)), NULL,
                        NULL, NULL));
}

static std::string generateVersionString() {
    std::ostringstream oss;
    oss << PY_MAJOR_VERSION << '.' << PY_MINOR_VERSION << '.' << PY_MICRO_VERSION;
    oss << '\n';
    oss << "[Pyston " << PYSTON_VERSION_MAJOR << '.' << PYSTON_VERSION_MINOR << '.' << PYSTON_VERSION_MICRO << "]";
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

extern "C" BORROWED(PyObject*) PySys_GetModulesDict() noexcept {
    return getSysModulesDict();
}

size_t _PySys_GetSizeOf(PyObject* o) {
    static PyObject* str__sizeof__ = NULL;
    PyObject* res = NULL;
    Py_ssize_t size;

    /* Make sure the type is initialized. float gets initialized late */
    if (PyType_Ready(Py_TYPE(o)) < 0)
        return (size_t)-1;

    /* Instance of old-style class */
    if (PyInstance_Check(o))
        size = PyInstance_Type.tp_basicsize;
    /* all other objects */
    else {
        PyObject* method = _PyObject_LookupSpecial(o, "__sizeof__", &str__sizeof__);
        if (method == NULL) {
            if (!PyErr_Occurred())
                PyErr_Format(PyExc_TypeError, "Type %.100s doesn't define __sizeof__", Py_TYPE(o)->tp_name);
        } else {
            res = PyObject_CallFunctionObjArgs(method, NULL);
            Py_DECREF(method);
        }

        if (res == NULL)
            return (size_t)-1;

        size = (size_t)PyInt_AsSsize_t(res);
        Py_DECREF(res);
        if (size == -1 && PyErr_Occurred())
            return (size_t)-1;
    }

    if (size < 0) {
        PyErr_SetString(PyExc_ValueError, "__sizeof__() should return >= 0");
        return (size_t)-1;
    }

    /* add gc_head size */
    if (PyObject_IS_GC(o))
        return ((size_t)size) + sizeof(PyGC_Head);
    return (size_t)size;
}

static PyObject* sys_getsizeof(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    static const char* kwlist[] = { "object", "default", 0 };
    size_t size;
    PyObject* o, * dflt = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O:getsizeof", const_cast<char**>(kwlist), &o, &dflt))
        return NULL;

    size = _PySys_GetSizeOf(o);

    if (size == (size_t)-1 && PyErr_Occurred()) {
        /* Has a default value been given */
        if (dflt != NULL && PyErr_ExceptionMatches(PyExc_TypeError)) {
            PyErr_Clear();
            Py_INCREF(dflt);
            return dflt;
        } else
            return NULL;
    }

    return PyInt_FromSize_t(size);
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

static PyObject* sys_clear_type_cache(PyObject* self, PyObject* args) {
    PyType_ClearCache();
    Py_RETURN_NONE;
}

PyDoc_STRVAR(sys_clear_type_cache__doc__, "_clear_type_cache() -> None\n\
        Clear the internal type lookup cache.");

static PyObject* sys_getrefcount(PyObject* self, PyObject* arg) {
    return PyInt_FromSsize_t(arg->ob_refcnt);
}

#ifdef Py_REF_DEBUG
static PyObject* sys_gettotalrefcount(PyObject* self) {
    return PyInt_FromSsize_t(_Py_GetRefTotal());
}
#endif /* Py_REF_DEBUG */

PyDoc_STRVAR(getrefcount_doc, "getrefcount(object) -> integer\n\
        \n\
        Return the reference count of object.  The count returned is generally\n\
        one higher than you might expect, because it includes the (temporary)\n\
        reference as an argument to getrefcount().");

PyDoc_STRVAR(excepthook_doc, "excepthook(exctype, value, traceback) -> None\n"
                             "\n"
                             "Handle an exception by displaying it with a traceback on sys.stderr.\n");

PyDoc_STRVAR(displayhook_doc, "displayhook(object) -> None\n"
                              "\n"
                              "Print an object to sys.stdout and also save it in __builtin__._\n");

PyDoc_STRVAR(getsizeof_doc, "getsizeof(object, default) -> int\n\
\n\
Return the size of object in bytes.");

static PyMethodDef sys_methods[] = {
    { "excepthook", sys_excepthook, METH_VARARGS, excepthook_doc },
    { "displayhook", sys_displayhook, METH_O, displayhook_doc },
    { "_clear_type_cache", sys_clear_type_cache, METH_NOARGS, sys_clear_type_cache__doc__ },
    { "getrefcount", (PyCFunction)sys_getrefcount, METH_O, getrefcount_doc },
    { "getsizeof", (PyCFunction)sys_getsizeof, METH_VARARGS | METH_KEYWORDS, getsizeof_doc },
};

PyDoc_STRVAR(flags__doc__, "sys.flags\n\
\n\
Flags provided through command line arguments or environment vars.");

static struct _typeobject FlagsType;

static PyStructSequence_Field flags_fields[] = {
    { "debug", "-d" },
    { "py3k_warning", "-3" },
    { "division_warning", "-Q" },
    { "division_new", "-Qnew" },
    { "inspect", "-i" },
    { "interactive", "-i" },
    { "optimize", "-O or -OO" },
    { "dont_write_bytecode", "-B" },
    { "no_user_site", "-s" },
    { "no_site", "-S" },
    { "ignore_environment", "-E" },
    { "tabcheck", "-t or -tt" },
    { "verbose", "-v" },
#ifdef RISCOS
    { "riscos_wimp", "???" },
#endif
    /* {"unbuffered",                   "-u"}, */
    { "unicode", "-U" },
    /* {"skip_first",                   "-x"}, */
    { "bytes_warning", "-b" },
    { "hash_randomization", "-R" },
    { 0, 0 },
};

static PyStructSequence_Desc flags_desc = { "sys.flags",  /* name */
                                            flags__doc__, /* doc */
                                            flags_fields, /* fields */
#ifdef RISCOS
                                            17
#else
                                            16
#endif
};

static PyObject* make_flags(void) {
    int pos = 0;
    PyObject* seq;

    seq = PyStructSequence_New(&FlagsType);
    if (seq == NULL)
        return NULL;

#define SetFlag(flag) PyStructSequence_SET_ITEM(seq, pos++, PyInt_FromLong(flag))

    SetFlag(Py_DebugFlag);
    SetFlag(Py_Py3kWarningFlag);
    SetFlag(Py_DivisionWarningFlag);
    SetFlag(_Py_QnewFlag);
    SetFlag(Py_InspectFlag);
    SetFlag(Py_InteractiveFlag);
    SetFlag(Py_OptimizeFlag);
    SetFlag(Py_DontWriteBytecodeFlag);
    SetFlag(Py_NoUserSiteDirectory);
    SetFlag(Py_NoSiteFlag);
    SetFlag(Py_IgnoreEnvironmentFlag);
    SetFlag(Py_TabcheckFlag);
    SetFlag(Py_VerboseFlag);
#ifdef RISCOS
    SetFlag(Py_RISCOSWimpFlag);
#endif
    /* SetFlag(saw_unbuffered_flag); */
    SetFlag(Py_UnicodeFlag);
    /* SetFlag(skipfirstline); */
    SetFlag(Py_BytesWarningFlag);
    SetFlag(Py_HashRandomizationFlag);
#undef SetFlag

    if (PyErr_Occurred()) {
        Py_DECREF(seq);
        return NULL;
    }
    return seq;
}

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

PyDoc_STRVAR(exc_info_doc, "exc_info() -> (type, value, traceback)\n\
\n\
Return information about the most recent exception caught by an except\n\
clause in the current stack frame or in an older stack frame.");

PyDoc_STRVAR(exc_clear_doc, "exc_clear() -> None\n\
\n\
Clear global information on the current exception.  Subsequent calls to\n\
exc_info() will return (None,None,None) until another exception is raised\n\
in the current thread or the execution stack returns to a frame where\n\
another exception is being handled.");


PyDoc_STRVAR(exit_doc, "exit([status])\n\
\n\
Exit the interpreter by raising SystemExit(status).\n\
If the status is omitted or None, it defaults to zero (i.e., success).\n\
If the status is an integer, it will be used as the system exit status.\n\
If it is another kind of object, it will be printed and the system\n\
exit status will be one (i.e., failure).");

PyDoc_STRVAR(getdefaultencoding_doc, "getdefaultencoding() -> string\n\
\n\
Return the current default string encoding used by the Unicode \n\
implementation.");

PyDoc_STRVAR(getfilesystemencoding_doc, "getfilesystemencoding() -> string\n\
\n\
Return the encoding used to convert Unicode filenames in\n\
operating system filenames.");

PyDoc_STRVAR(getrecursionlimit_doc, "getrecursionlimit()\n\
\n\
Return the current value of the recursion limit, the maximum depth\n\
of the Python interpreter stack.  This limit prevents infinite\n\
recursion from causing an overflow of the C stack and crashing Python.");

static int _check_and_flush(FILE* stream) {
    int prev_fail = ferror(stream);
    return fflush(stream) || prev_fail ? EOF : 0;
}

void setupSys() {
    sys_modules_dict = new BoxedDict();
    PyThreadState_GET()->interp->modules = incref(sys_modules_dict);

    constants.push_back(sys_modules_dict);

    // This is ok to call here because we've already created the sys_modules_dict
    sys_module = createModule(autoDecref(boxString("sys")));

    // sys_module is what holds on to all of the other modules:
    Py_INCREF(sys_module);
    late_constants.push_back(sys_module);

    sys_module->giveAttrBorrowed("modules", sys_modules_dict);

    BoxedList* sys_path = new BoxedList();
    constants.push_back(sys_path);
    sys_module->giveAttrBorrowed("path", sys_path);

    sys_module->giveAttr("argv", new BoxedList());

    sys_module->giveAttr("exc_info",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sysExcInfo, BOXED_TUPLE, 0),
                                                          "exc_info", exc_info_doc));
    sys_module->giveAttr("exc_clear", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sysExcClear, NONE, 0),
                                                                       "exc_clear", exc_clear_doc));
    sys_module->giveAttr("exit",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sysExit, NONE, 1, false, false),
                                                          "exit", { Py_None }, NULL, exit_doc));

    sys_module->giveAttr("warnoptions", new BoxedList());
    sys_module->giveAttrBorrowed("py3kwarning", Py_False);
    sys_module->giveAttr("byteorder", boxString(isLittleEndian() ? "little" : "big"));

    sys_module->giveAttr("platform", boxString(Py_GetPlatform()));

    sys_module->giveAttr("executable", boxString(Py_GetProgramFullPath()));

    sys_module->giveAttr("_getframe",
                         new BoxedFunction(BoxedCode::create((void*)sysGetFrame, UNKNOWN, 1, false, false), { NULL }));
    sys_module->giveAttr("_current_frames", new BoxedFunction(BoxedCode::create((void*)sysCurrentFrames, UNKNOWN, 0)));
    sys_module->giveAttr("getdefaultencoding",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sysGetDefaultEncoding, STR, 0),
                                                          "getdefaultencoding", getdefaultencoding_doc));

    sys_module->giveAttr("getfilesystemencoding",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sysGetFilesystemEncoding, STR, 0),
                                                          "getfilesystemencoding", getfilesystemencoding_doc));

    sys_module->giveAttr("getrecursionlimit",
                         new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sysGetRecursionLimit, UNKNOWN, 0),
                                                          "getrecursionlimit", getrecursionlimit_doc));

    // As we don't support compile() etc yet force 'dont_write_bytecode' to true.
    sys_module->giveAttrBorrowed("dont_write_bytecode", Py_True);

    sys_module->giveAttr("prefix", boxString(Py_GetPrefix()));
    sys_module->giveAttr("exec_prefix", boxString(Py_GetExecPrefix()));

    sys_module->giveAttr("copyright",
                         boxString("Copyright 2014-2016 Dropbox.\nAll Rights Reserved.\n\nCopyright (c) 2001-2014 "
                                   "Python Software Foundation.\nAll Rights Reserved.\n\nCopyright (c) 2000 "
                                   "BeOpen.com.\nAll Rights Reserved.\n\nCopyright (c) 1995-2001 Corporation for "
                                   "National Research Initiatives.\nAll Rights Reserved.\n\nCopyright (c) "
                                   "1991-1995 Stichting Mathematisch Centrum, Amsterdam.\nAll Rights Reserved."));

    sys_module->giveAttr("version", boxString(generateVersionString()));
    sys_module->giveAttr("hexversion", boxInt(PY_VERSION_HEX));
    sys_module->giveAttr("subversion", BoxedTuple::create({ autoDecref(boxString("Pyston")), autoDecref(boxString("")),
                                                            autoDecref(boxString("")) }));
    sys_module->giveAttr("maxint", boxInt(PYSTON_INT_MAX));
    sys_module->giveAttr("maxsize", boxInt(PY_SSIZE_T_MAX));

#define SET_SYS_FROM_STRING(key, value) sys_module->giveAttr((key), (value))
#ifdef Py_USING_UNICODE
    SET_SYS_FROM_STRING("maxunicode", PyInt_FromLong(PyUnicode_GetMax()));
#endif

/* float repr style: 0.03 (short) vs 0.029999999999999999 (legacy) */
#ifndef PY_NO_SHORT_FLOAT_REPR
    SET_SYS_FROM_STRING("float_repr_style", PyString_FromString("short"));
#else
    SET_SYS_FROM_STRING("float_repr_style", PyString_FromString("legacy"));
#endif


    auto sys_str = getStaticString("sys");
    for (auto& md : sys_methods) {
        sys_module->giveAttr(md.ml_name, new BoxedCApiFunction(&md, NULL, sys_str));
    }

    sys_module->giveAttrBorrowed("__displayhook__", sys_module->getattr(autoDecref(internStringMortal("displayhook"))));
}

void setupSysEnd() {
    std::vector<Box*> builtin_module_names;
    for (int i = 0; PyImport_Inittab[i].name != NULL; i++)
        builtin_module_names.push_back(boxString(PyImport_Inittab[i].name));

    std::sort<decltype(builtin_module_names)::iterator, PyLt>(builtin_module_names.begin(), builtin_module_names.end(),
                                                              PyLt());

    sys_module->giveAttr("builtin_module_names",
                         BoxedTuple::create(builtin_module_names.size(), &builtin_module_names[0]));

    for (Box* b : builtin_module_names)
        Py_DECREF(b);

#ifndef NDEBUG
    for (const auto& p : *sys_modules_dict) {
        assert(PyString_Check(p.first));

        bool found = false;
        for (int i = 0; PyImport_Inittab[i].name != NULL; i++) {
            if (((BoxedString*)p.first)->s() == PyImport_Inittab[i].name) {
                found = true;
            }
        }
        if (!found)
            assert(0 && "found a module which is inside sys.modules but not listed inside PyImport_Inittab!");
    }
#endif

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
    /* flags */
    if (FlagsType.tp_name == 0)
        PyStructSequence_InitType((PyTypeObject*)&FlagsType, &flags_desc);
    SET_SYS_FROM_STRING("flags", make_flags());
    /* prevent user from creating new instances */
    FlagsType.tp_init = NULL;
    FlagsType.tp_new = NULL;
    /* prevent user from creating new instances */
    FloatInfoType.tp_init = NULL;
    FloatInfoType.tp_new = NULL;

    SET_SYS_FROM_STRING("float_info", PyFloat_GetInfo());
}
}
