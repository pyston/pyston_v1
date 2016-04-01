// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_ERRORS_H
#define Py_ERRORS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Error objects */

typedef struct {
    PyObject_HEAD
    // Pyston change: changed from dict to hcattrs
    // PyObject *dict;
    struct _hcattrs hcattrs;
    PyObject *args;
    PyObject *message;
} PyBaseExceptionObject;

typedef struct {
    PyObject_HEAD
    // Pyston change: changed from dict to hcattrs
    // PyObject *dict;
    struct _hcattrs hcattrs;
    PyObject *args;
    PyObject *message;
    PyObject *msg;
    PyObject *filename;
    PyObject *lineno;
    PyObject *offset;
    PyObject *text;
    PyObject *print_file_and_line;
} PySyntaxErrorObject;

#ifdef Py_USING_UNICODE
typedef struct {
    PyObject_HEAD
    // Pyston change: changed from dict to hcattrs
    // PyObject *dict;
    struct _hcattrs hcattrs;
    PyObject *args;
    PyObject *message;
    PyObject *encoding;
    PyObject *object;
    Py_ssize_t start;
    Py_ssize_t end;
    PyObject *reason;
} PyUnicodeErrorObject;
#endif

typedef struct {
    PyObject_HEAD
    // Pyston change: changed from dict to hcattrs
    // PyObject *dict;
    struct _hcattrs hcattrs;
    PyObject *args;
    PyObject *message;
    PyObject *code;
} PySystemExitObject;

typedef struct {
    PyObject_HEAD
    // Pyston change: changed from dict to hcattrs
    // PyObject *dict;
    struct _hcattrs hcattrs;
    PyObject *args;
    PyObject *message;
    PyObject *myerrno;
    PyObject *strerror;
    PyObject *filename;
} PyEnvironmentErrorObject;

#ifdef MS_WINDOWS
typedef struct {
    PyObject_HEAD
    // Pyston change: changed from dict to hcattrs
    // PyObject *dict;
    struct _hcattrs hcattrs;
    PyObject *args;
    PyObject *message;
    PyObject *myerrno;
    PyObject *strerror;
    PyObject *filename;
    PyObject *winerror;
} PyWindowsErrorObject;
#endif

/* Error handling definitions */

PyAPI_FUNC(void) PyErr_SetNone(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetObject(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetString(PyObject *, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(BORROWED(PyObject *)) PyErr_Occurred(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_Clear(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_Fetch(PyObject **, PyObject **, PyObject **) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_Restore(STOLEN(PyObject *), STOLEN(PyObject *), STOLEN(PyObject *)) PYSTON_NOEXCEPT;

// Pyton change: This functions are normally only available in CPython >= 3.3 and PyPy but Cython can use them.
PyAPI_FUNC(void) PyErr_GetExcInfo(PyObject **ptype, PyObject **pvalue, PyObject **ptraceback) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetExcInfo(PyObject *type, PyObject *value, PyObject *traceback) PYSTON_NOEXCEPT;

#ifdef Py_DEBUG
#define _PyErr_OCCURRED() PyErr_Occurred()
#else
#define _PyErr_OCCURRED() (_PyThreadState_Current->curexc_type)
#endif

/* Error testing and normalization */
PyAPI_FUNC(int) PyErr_GivenExceptionMatches(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyErr_ExceptionMatches(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_NormalizeException(PyObject**, PyObject**, PyObject**) PYSTON_NOEXCEPT;

/* */

#define PyExceptionClass_Check(x)                                       \
    (PyClass_Check((x)) || (PyType_Check((x)) &&                        \
      PyType_FastSubclass((PyTypeObject*)(x), Py_TPFLAGS_BASE_EXC_SUBCLASS)))

#define PyExceptionInstance_Check(x)                    \
    (PyInstance_Check((x)) ||                           \
     PyType_FastSubclass(Py_TYPE(x), Py_TPFLAGS_BASE_EXC_SUBCLASS))

// Pyston change: made these function calls for now
#if 0
#define PyExceptionClass_Name(x)                                   \
    (PyClass_Check((x))                                            \
     ? PyString_AS_STRING(((PyClassObject*)(x))->cl_name)          \
     : (char *)(((PyTypeObject*)(x))->tp_name))

#define PyExceptionInstance_Class(x)                                    \
    ((PyInstance_Check((x))                                             \
      ? (PyObject*)((PyInstanceObject*)(x))->in_class                   \
      : (PyObject*)((x)->ob_type)))
#endif
// (We might have to make these wrapper macros that do appropriate casting to PyObject)
PyAPI_FUNC(const char*) PyExceptionClass_Name(PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject*) PyExceptionInstance_Class(PyObject*) PYSTON_NOEXCEPT;

/* Predefined exceptions */

// Pyston change: make these just expand to casts around our internal types, which are declared
// as PyTypeObject's.
// TODO not sure if this is worth it -- a fair amount of confusion (duplicating these names) for fairly
// little gain (getting to treat them as PyTypeObject's and using our own names)
PyAPI_DATA(PyObject *) PyExc_BaseException;
PyAPI_DATA(PyObject *) PyExc_Exception;
PyAPI_DATA(PyObject *) PyExc_StopIteration;
PyAPI_DATA(PyObject *) PyExc_GeneratorExit;
PyAPI_DATA(PyObject *) PyExc_StandardError;
PyAPI_DATA(PyObject *) PyExc_ArithmeticError;
PyAPI_DATA(PyObject *) PyExc_LookupError;

PyAPI_DATA(PyObject *) PyExc_AssertionError;
PyAPI_DATA(PyObject *) PyExc_AttributeError;
PyAPI_DATA(PyObject *) PyExc_EOFError;
PyAPI_DATA(PyObject *) PyExc_FloatingPointError;
PyAPI_DATA(PyObject *) PyExc_EnvironmentError;
PyAPI_DATA(PyObject *) PyExc_IOError;
PyAPI_DATA(PyObject *) PyExc_OSError;
PyAPI_DATA(PyObject *) PyExc_ImportError;
PyAPI_DATA(PyObject *) PyExc_IndexError;
PyAPI_DATA(PyObject *) PyExc_KeyError;
PyAPI_DATA(PyObject *) PyExc_KeyboardInterrupt;
PyAPI_DATA(PyObject *) PyExc_MemoryError;
PyAPI_DATA(PyObject *) PyExc_NameError;
PyAPI_DATA(PyObject *) PyExc_OverflowError;
PyAPI_DATA(PyObject *) PyExc_RuntimeError;
PyAPI_DATA(PyObject *) PyExc_NotImplementedError;
PyAPI_DATA(PyObject *) PyExc_SyntaxError;
PyAPI_DATA(PyObject *) PyExc_IndentationError;
PyAPI_DATA(PyObject *) PyExc_TabError;
PyAPI_DATA(PyObject *) PyExc_ReferenceError;
PyAPI_DATA(PyObject *) PyExc_SystemError;
PyAPI_DATA(PyObject *) PyExc_SystemExit;
PyAPI_DATA(PyObject *) PyExc_TypeError;
PyAPI_DATA(PyObject *) PyExc_UnboundLocalError;
PyAPI_DATA(PyObject *) PyExc_UnicodeError;
PyAPI_DATA(PyObject *) PyExc_UnicodeEncodeError;
PyAPI_DATA(PyObject *) PyExc_UnicodeDecodeError;
PyAPI_DATA(PyObject *) PyExc_UnicodeTranslateError;
PyAPI_DATA(PyObject *) PyExc_ValueError;
PyAPI_DATA(PyObject *) PyExc_ZeroDivisionError;
#ifdef MS_WINDOWS
PyAPI_DATA(PyObject *) PyExc_WindowsError;
#endif
#ifdef __VMS
PyAPI_DATA(PyObject *) PyExc_VMSError;
#endif

PyAPI_DATA(PyObject *) PyExc_BufferError;

PyAPI_DATA(PyObject *) PyExc_MemoryErrorInst;
PyAPI_DATA(PyObject *) PyExc_RecursionErrorInst;


/* Predefined warning categories */
PyAPI_DATA(PyObject *) PyExc_Warning;
PyAPI_DATA(PyObject *) PyExc_UserWarning;
PyAPI_DATA(PyObject *) PyExc_DeprecationWarning;
PyAPI_DATA(PyObject *) PyExc_PendingDeprecationWarning;
PyAPI_DATA(PyObject *) PyExc_SyntaxWarning;
PyAPI_DATA(PyObject *) PyExc_RuntimeWarning;
PyAPI_DATA(PyObject *) PyExc_FutureWarning;
PyAPI_DATA(PyObject *) PyExc_ImportWarning;
PyAPI_DATA(PyObject *) PyExc_UnicodeWarning;
PyAPI_DATA(PyObject *) PyExc_BytesWarning;

/* Convenience functions */

PyAPI_FUNC(int) PyErr_BadArgument(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_NoMemory(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromErrno(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilenameObject(
    PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilename(
    PyObject *, const char *) PYSTON_NOEXCEPT;
#ifdef MS_WINDOWS
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithUnicodeFilename(
    PyObject *, const Py_UNICODE *) PYSTON_NOEXCEPT;
#endif /* MS_WINDOWS */

// This actually always returns NULL:
PyAPI_FUNC(BORROWED(PyObject *)) PyErr_Format(PyObject *, const char *, ...)
                        PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 2, 3)));

#ifdef MS_WINDOWS
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithFilenameObject(
    int, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithFilename(
    int, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithUnicodeFilename(
    int, const Py_UNICODE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErr(int) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilenameObject(
    PyObject *,int, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilename(
    PyObject *,int, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithUnicodeFilename(
    PyObject *,int, const Py_UNICODE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErr(PyObject *, int) PYSTON_NOEXCEPT;
#endif /* MS_WINDOWS */

/* Export the old function so that the existing API remains available: */
PyAPI_FUNC(void) PyErr_BadInternalCall(void) PYSTON_NOEXCEPT;
// Pyston change: changed this from char* to const char*
PyAPI_FUNC(void) _PyErr_BadInternalCall(const char *filename, int lineno) PYSTON_NOEXCEPT;
/* Mask the old API with a call to the new API for code compiled under
   Python 2.0: */
#define PyErr_BadInternalCall() _PyErr_BadInternalCall(__FILE__, __LINE__)

/* Function to create a new exception */
PyAPI_FUNC(PyObject *) PyErr_NewException(
    const char *name, PyObject *base, PyObject *dict) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_NewExceptionWithDoc(
    char *name, char *doc, PyObject *base, PyObject *dict) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_WriteUnraisable(PyObject *) PYSTON_NOEXCEPT;

// Pyston addition: allocate + initialize an instance of type `type`.
// arg represents the single value that will be passed to the constructor;
// a NULL value represents passing zero arguments, and a tuple value will
// not be expanded into multiple arguments.
// In the common cases this will be faster than creating the instance using
// PyObject_Call(type, PyTuple_Pack(1, arg), NULL)
PyAPI_FUNC(PyObject *) PyErr_CreateExceptionInstance(PyObject* type, PyObject* arg) PYSTON_NOEXCEPT;

/* In sigcheck.c or signalmodule.c */
PyAPI_FUNC(int) PyErr_CheckSignals(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyErr_SetInterrupt(void) PYSTON_NOEXCEPT;

/* In signalmodule.c */
int PySignal_SetWakeupFd(int fd);

/* Support for adding program text to SyntaxErrors */
PyAPI_FUNC(void) PyErr_SyntaxLocation(const char *, int) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyErr_ProgramText(const char *, int) PYSTON_NOEXCEPT;

#ifdef Py_USING_UNICODE
/* The following functions are used to create and modify unicode
   exceptions from C */

/* create a UnicodeDecodeError object */
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_Create(
    const char *, const char *, Py_ssize_t, Py_ssize_t, Py_ssize_t, const char *) PYSTON_NOEXCEPT;

/* create a UnicodeEncodeError object */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_Create(
    const char *, const Py_UNICODE *, Py_ssize_t, Py_ssize_t, Py_ssize_t, const char *) PYSTON_NOEXCEPT;

/* create a UnicodeTranslateError object */
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_Create(
    const Py_UNICODE *, Py_ssize_t, Py_ssize_t, Py_ssize_t, const char *) PYSTON_NOEXCEPT;

/* get the encoding attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetEncoding(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetEncoding(PyObject *) PYSTON_NOEXCEPT;

/* get the object attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetObject(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetObject(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_GetObject(PyObject *) PYSTON_NOEXCEPT;

/* get the value of the start attribute (the int * may not be NULL)
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_GetStart(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_GetStart(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_GetStart(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;

/* assign a new value to the start attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetStart(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_SetStart(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_SetStart(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;

/* get the value of the end attribute (the int *may not be NULL)
 return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_GetEnd(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_GetEnd(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_GetEnd(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;

/* assign a new value to the end attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetEnd(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_SetEnd(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_SetEnd(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;

/* get the value of the reason attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetReason(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetReason(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_GetReason(PyObject *) PYSTON_NOEXCEPT;

/* assign a new value to the reason attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetReason(
    PyObject *, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeDecodeError_SetReason(
    PyObject *, const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyUnicodeTranslateError_SetReason(
    PyObject *, const char *) PYSTON_NOEXCEPT;
#endif


/* These APIs aren't really part of the error implementation, but
   often needed to format error messages; the native C lib APIs are
   not available on all platforms, which is why we provide emulations
   for those platforms in Python/mysnprintf.c,
   WARNING:  The return value of snprintf varies across platforms; do
   not rely on any particular behavior; eventually the C99 defn may
   be reliable.
*/
#if defined(MS_WIN32) && !defined(HAVE_SNPRINTF)
# define HAVE_SNPRINTF
# define snprintf _snprintf
# define vsnprintf _vsnprintf
#endif

#include <stdarg.h>
PyAPI_FUNC(int) PyOS_snprintf(char *str, size_t size, const char  *format, ...)
                        PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 3, 4)));
PyAPI_FUNC(int) PyOS_vsnprintf(char *str, size_t size, const char  *format, va_list va)
                        PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 3, 0)));

#ifdef __cplusplus
}
#endif
#endif /* !Py_ERRORS_H */
