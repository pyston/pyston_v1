// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_ERRORS_H
#define Py_ERRORS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Error objects */

// Pyston change: these are not our object formats
#if 0
typedef struct {
    PyObject_HEAD
    PyObject *dict;
    PyObject *args;
    PyObject *message;
} PyBaseExceptionObject;

typedef struct {
    PyObject_HEAD
    PyObject *dict;
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
    PyObject *dict;
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
    PyObject *dict;
    PyObject *args;
    PyObject *message;
    PyObject *code;
} PySystemExitObject;

typedef struct {
    PyObject_HEAD
    PyObject *dict;
    PyObject *args;
    PyObject *message;
    PyObject *myerrno;
    PyObject *strerror;
    PyObject *filename;
} PyEnvironmentErrorObject;

#ifdef MS_WINDOWS
typedef struct {
    PyObject_HEAD
    PyObject *dict;
    PyObject *args;
    PyObject *message;
    PyObject *myerrno;
    PyObject *strerror;
    PyObject *filename;
    PyObject *winerror;
} PyWindowsErrorObject;
#endif
#endif
// (Pyston TODO: add opaque definitions of those names)

/* Error handling definitions */

PyAPI_FUNC(void) PyErr_SetNone(PyObject *);
PyAPI_FUNC(void) PyErr_SetObject(PyObject *, PyObject *);
PyAPI_FUNC(void) PyErr_SetString(PyObject *, const char *);
PyAPI_FUNC(PyObject *) PyErr_Occurred(void);
PyAPI_FUNC(void) PyErr_Clear(void);
PyAPI_FUNC(void) PyErr_Fetch(PyObject **, PyObject **, PyObject **);
PyAPI_FUNC(void) PyErr_Restore(PyObject *, PyObject *, PyObject *);

#ifdef Py_DEBUG
#define _PyErr_OCCURRED() PyErr_Occurred()
#else
#define _PyErr_OCCURRED() (_PyThreadState_Current->curexc_type)
#endif

/* Error testing and normalization */
PyAPI_FUNC(int) PyErr_GivenExceptionMatches(PyObject *, PyObject *);
PyAPI_FUNC(int) PyErr_ExceptionMatches(PyObject *);
PyAPI_FUNC(void) PyErr_NormalizeException(PyObject**, PyObject**, PyObject**);

/* */

// Pyston change: made these function calls for now
#if 0
#define PyExceptionClass_Check(x)                                       \
    (PyClass_Check((x)) || (PyType_Check((x)) &&                        \
      PyType_FastSubclass((PyTypeObject*)(x), Py_TPFLAGS_BASE_EXC_SUBCLASS)))

#define PyExceptionInstance_Check(x)                    \
    (PyInstance_Check((x)) ||                           \
     PyType_FastSubclass((x)->ob_type, Py_TPFLAGS_BASE_EXC_SUBCLASS))

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
PyAPI_FUNC(int) PyExceptionClass_Check(PyObject*);
PyAPI_FUNC(int) PyExceptionInstance_Check(PyObject*);
PyAPI_FUNC(const char*) PyExceptionClass_Name(PyObject*);
PyAPI_FUNC(PyObject*) PyExceptionInstance_Class(PyObject*);

/* Predefined exceptions */

// Pyston change: make these just expand to casts around our internal types, which are declared
// as PyTypeObject's.
// TODO not sure if this is worth it -- a fair amount of confusion (duplicating these names) for fairly
// little gain (getting to treat them as PyTypeObject's and using our own names)
#define PyExc_BaseException ((PyObject*)BaseException)
PyAPI_DATA(PyTypeObject *) BaseException;
#define PyExc_Exception ((PyObject*)Exception)
PyAPI_DATA(PyTypeObject *) Exception;
#define PyExc_StopIteration ((PyObject*)StopIteration)
PyAPI_DATA(PyTypeObject *) StopIteration;
#define PyExc_GeneratorExit ((PyObject*)GeneratorExit)
PyAPI_DATA(PyTypeObject *) GeneratorExit;
#define PyExc_StandardError ((PyObject*)StandardError)
PyAPI_DATA(PyTypeObject *) StandardError;
#define PyExc_ArithmeticError ((PyObject*)ArithmeticError)
PyAPI_DATA(PyTypeObject *) ArithmeticError;
#define PyExc_LookupError ((PyObject*)LookupError)
PyAPI_DATA(PyTypeObject *) LookupError;

#define PyExc_AssertionError ((PyObject*)AssertionError)
PyAPI_DATA(PyTypeObject *) AssertionError;
#define PyExc_AttributeError ((PyObject*)AttributeError)
PyAPI_DATA(PyTypeObject *) AttributeError;
#define PyExc_EOFError ((PyObject*)EOFError)
PyAPI_DATA(PyTypeObject *) EOFError;
#define PyExc_FloatingPointError ((PyObject*)FloatingPointError)
PyAPI_DATA(PyTypeObject *) FloatingPointError;
#define PyExc_EnvironmentError ((PyObject*)EnvironmentError)
PyAPI_DATA(PyTypeObject *) EnvironmentError;
#define PyExc_IOError ((PyObject*)IOError)
PyAPI_DATA(PyTypeObject *) IOError;
#define PyExc_OSError ((PyObject*)OSError)
PyAPI_DATA(PyTypeObject *) OSError;
#define PyExc_ImportError ((PyObject*)ImportError)
PyAPI_DATA(PyTypeObject *) ImportError;
#define PyExc_IndexError ((PyObject*)IndexError)
PyAPI_DATA(PyTypeObject *) IndexError;
#define PyExc_KeyError ((PyObject*)KeyError)
PyAPI_DATA(PyTypeObject *) KeyError;
#define PyExc_KeyboardInterrupt ((PyObject*)KeyboardInterrupt)
PyAPI_DATA(PyTypeObject *) KeyboardInterrupt;
#define PyExc_MemoryError ((PyObject*)MemoryError)
PyAPI_DATA(PyTypeObject *) MemoryError;
#define PyExc_NameError ((PyObject*)NameError)
PyAPI_DATA(PyTypeObject *) NameError;
#define PyExc_OverflowError ((PyObject*)OverflowError)
PyAPI_DATA(PyTypeObject *) OverflowError;
#define PyExc_RuntimeError ((PyObject*)RuntimeError)
PyAPI_DATA(PyTypeObject *) RuntimeError;
#define PyExc_NotImplementedError ((PyObject*)NotImplementedError)
PyAPI_DATA(PyTypeObject *) NotImplementedError;
#define PyExc_SyntaxError ((PyObject*)SyntaxError)
PyAPI_DATA(PyTypeObject *) SyntaxError;
#define PyExc_IndentationError ((PyObject*)IndentationError)
PyAPI_DATA(PyTypeObject *) IndentationError;
#define PyExc_TabError ((PyObject*)TabError)
PyAPI_DATA(PyTypeObject *) TabError;
#define PyExc_ReferenceError ((PyObject*)ReferenceError)
PyAPI_DATA(PyTypeObject *) ReferenceError;
#define PyExc_SystemError ((PyObject*)SystemError)
PyAPI_DATA(PyTypeObject *) SystemError;
#define PyExc_SystemExit ((PyObject*)SystemExit)
PyAPI_DATA(PyTypeObject *) SystemExit;
#define PyExc_TypeError ((PyObject*)TypeError)
PyAPI_DATA(PyTypeObject *) TypeError;
#define PyExc_UnboundLocalError ((PyObject*)UnboundLocalError)
PyAPI_DATA(PyTypeObject *) UnboundLocalError;
#define PyExc_UnicodeError ((PyObject*)UnicodeError)
PyAPI_DATA(PyTypeObject *) UnicodeError;
#define PyExc_UnicodeEncodeError ((PyObject*)UnicodeEncodeError)
PyAPI_DATA(PyTypeObject *) UnicodeEncodeError;
#define PyExc_UnicodeDecodeError ((PyObject*)UnicodeDecodeError)
PyAPI_DATA(PyTypeObject *) UnicodeDecodeError;
#define PyExc_UnicodeTranslateError ((PyObject*)UnicodeTranslateError)
PyAPI_DATA(PyTypeObject *) UnicodeTranslateError;
#define PyExc_ValueError ((PyObject*)ValueError)
PyAPI_DATA(PyTypeObject *) ValueError;
#define PyExc_ZeroDivisionError ((PyObject*)ZeroDivisionError)
PyAPI_DATA(PyTypeObject *) ZeroDivisionError;
#ifdef MS_WINDOWS
#define PyExc_WindowsError ((PyObject*)WindowsError)
PyAPI_DATA(PyTypeObject *) WindowsError;
#endif
#ifdef __VMS
#define PyExc_VMSError ((PyObject*)VMSError)
PyAPI_DATA(PyTypeObject *) VMSError;
#endif

#define PyExc_BufferError ((PyObject*)BufferError)
PyAPI_DATA(PyTypeObject *) BufferError;

#define PyExc_MemoryErrorInst ((PyObject*)MemoryErrorInst)
PyAPI_DATA(PyTypeObject *) MemoryErrorInst;
#define PyExc_RecursionErrorInst ((PyObject*)RecursionErrorInst)
PyAPI_DATA(PyTypeObject *) RecursionErrorInst;

/* Predefined warning categories */
#define PyExc_Warning ((PyObject*)Warning)
PyAPI_DATA(PyTypeObject *) Warning;
#define PyExc_UserWarning ((PyObject*)UserWarning)
PyAPI_DATA(PyTypeObject *) UserWarning;
#define PyExc_DeprecationWarning ((PyObject*)DeprecationWarning)
PyAPI_DATA(PyTypeObject *) DeprecationWarning;
#define PyExc_PendingDeprecationWarning ((PyObject*)PendingDeprecationWarning)
PyAPI_DATA(PyTypeObject *) PendingDeprecationWarning;
#define PyExc_SyntaxWarning ((PyObject*)SyntaxWarning)
PyAPI_DATA(PyTypeObject *) SyntaxWarning;
#define PyExc_RuntimeWarning ((PyObject*)RuntimeWarning)
PyAPI_DATA(PyTypeObject *) RuntimeWarning;
#define PyExc_FutureWarning ((PyObject*)FutureWarning)
PyAPI_DATA(PyTypeObject *) FutureWarning;
#define PyExc_ImportWarning ((PyObject*)ImportWarning)
PyAPI_DATA(PyTypeObject *) ImportWarning;
#define PyExc_UnicodeWarning ((PyObject*)UnicodeWarning)
PyAPI_DATA(PyTypeObject *) UnicodeWarning;
#define PyExc_BytesWarning ((PyObject*)BytesWarning)
PyAPI_DATA(PyTypeObject *) BytesWarning;


/* Convenience functions */

PyAPI_FUNC(int) PyErr_BadArgument(void);
PyAPI_FUNC(PyObject *) PyErr_NoMemory(void);
PyAPI_FUNC(PyObject *) PyErr_SetFromErrno(PyObject *);
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilenameObject(
    PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithFilename(
    PyObject *, const char *);
#ifdef MS_WINDOWS
PyAPI_FUNC(PyObject *) PyErr_SetFromErrnoWithUnicodeFilename(
    PyObject *, const Py_UNICODE *);
#endif /* MS_WINDOWS */

PyAPI_FUNC(PyObject *) PyErr_Format(PyObject *, const char *, ...)
                        Py_GCC_ATTRIBUTE((format(printf, 2, 3)));

#ifdef MS_WINDOWS
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithFilenameObject(
    int, const char *);
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithFilename(
    int, const char *);
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErrWithUnicodeFilename(
    int, const Py_UNICODE *);
PyAPI_FUNC(PyObject *) PyErr_SetFromWindowsErr(int);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilenameObject(
    PyObject *,int, PyObject *);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithFilename(
    PyObject *,int, const char *);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErrWithUnicodeFilename(
    PyObject *,int, const Py_UNICODE *);
PyAPI_FUNC(PyObject *) PyErr_SetExcFromWindowsErr(PyObject *, int);
#endif /* MS_WINDOWS */

/* Export the old function so that the existing API remains available: */
PyAPI_FUNC(void) PyErr_BadInternalCall(void);
// Pyston change: changed this from char* to const char*
PyAPI_FUNC(void) _PyErr_BadInternalCall(const char *filename, int lineno);
/* Mask the old API with a call to the new API for code compiled under
   Python 2.0: */
#define PyErr_BadInternalCall() _PyErr_BadInternalCall(__FILE__, __LINE__)

/* Function to create a new exception */
PyAPI_FUNC(PyObject *) PyErr_NewException(
    char *name, PyObject *base, PyObject *dict);
PyAPI_FUNC(PyObject *) PyErr_NewExceptionWithDoc(
    char *name, char *doc, PyObject *base, PyObject *dict);
PyAPI_FUNC(void) PyErr_WriteUnraisable(PyObject *);

/* In sigcheck.c or signalmodule.c */
PyAPI_FUNC(int) PyErr_CheckSignals(void);
PyAPI_FUNC(void) PyErr_SetInterrupt(void);

/* In signalmodule.c */
int PySignal_SetWakeupFd(int fd);

/* Support for adding program text to SyntaxErrors */
PyAPI_FUNC(void) PyErr_SyntaxLocation(const char *, int);
PyAPI_FUNC(PyObject *) PyErr_ProgramText(const char *, int);

#ifdef Py_USING_UNICODE
/* The following functions are used to create and modify unicode
   exceptions from C */

/* create a UnicodeDecodeError object */
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_Create(
    const char *, const char *, Py_ssize_t, Py_ssize_t, Py_ssize_t, const char *);

/* create a UnicodeEncodeError object */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_Create(
    const char *, const Py_UNICODE *, Py_ssize_t, Py_ssize_t, Py_ssize_t, const char *);

/* create a UnicodeTranslateError object */
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_Create(
    const Py_UNICODE *, Py_ssize_t, Py_ssize_t, Py_ssize_t, const char *);

/* get the encoding attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetEncoding(PyObject *);
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetEncoding(PyObject *);

/* get the object attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetObject(PyObject *);
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetObject(PyObject *);
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_GetObject(PyObject *);

/* get the value of the start attribute (the int * may not be NULL)
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_GetStart(PyObject *, Py_ssize_t *);
PyAPI_FUNC(int) PyUnicodeDecodeError_GetStart(PyObject *, Py_ssize_t *);
PyAPI_FUNC(int) PyUnicodeTranslateError_GetStart(PyObject *, Py_ssize_t *);

/* assign a new value to the start attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetStart(PyObject *, Py_ssize_t);
PyAPI_FUNC(int) PyUnicodeDecodeError_SetStart(PyObject *, Py_ssize_t);
PyAPI_FUNC(int) PyUnicodeTranslateError_SetStart(PyObject *, Py_ssize_t);

/* get the value of the end attribute (the int *may not be NULL)
 return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_GetEnd(PyObject *, Py_ssize_t *);
PyAPI_FUNC(int) PyUnicodeDecodeError_GetEnd(PyObject *, Py_ssize_t *);
PyAPI_FUNC(int) PyUnicodeTranslateError_GetEnd(PyObject *, Py_ssize_t *);

/* assign a new value to the end attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetEnd(PyObject *, Py_ssize_t);
PyAPI_FUNC(int) PyUnicodeDecodeError_SetEnd(PyObject *, Py_ssize_t);
PyAPI_FUNC(int) PyUnicodeTranslateError_SetEnd(PyObject *, Py_ssize_t);

/* get the value of the reason attribute */
PyAPI_FUNC(PyObject *) PyUnicodeEncodeError_GetReason(PyObject *);
PyAPI_FUNC(PyObject *) PyUnicodeDecodeError_GetReason(PyObject *);
PyAPI_FUNC(PyObject *) PyUnicodeTranslateError_GetReason(PyObject *);

/* assign a new value to the reason attribute
   return 0 on success, -1 on failure */
PyAPI_FUNC(int) PyUnicodeEncodeError_SetReason(
    PyObject *, const char *);
PyAPI_FUNC(int) PyUnicodeDecodeError_SetReason(
    PyObject *, const char *);
PyAPI_FUNC(int) PyUnicodeTranslateError_SetReason(
    PyObject *, const char *);
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
                        Py_GCC_ATTRIBUTE((format(printf, 3, 4)));
PyAPI_FUNC(int) PyOS_vsnprintf(char *str, size_t size, const char  *format, va_list va)
                        Py_GCC_ATTRIBUTE((format(printf, 3, 0)));

#ifdef __cplusplus
}
#endif
#endif /* !Py_ERRORS_H */
