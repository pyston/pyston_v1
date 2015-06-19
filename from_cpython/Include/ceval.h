// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_CEVAL_H
#define Py_CEVAL_H
#ifdef __cplusplus
extern "C" {
#endif


/* Interface to random parts in ceval.c */

PyAPI_FUNC(PyObject *) PyEval_CallObjectWithKeywords(
    PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;

/* Inline this */
#define PyEval_CallObject(func,arg) \
    PyEval_CallObjectWithKeywords(func, arg, (PyObject *)NULL)

PyAPI_FUNC(PyObject *) PyEval_CallFunction(PyObject *obj,
                                           const char *format, ...) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyEval_CallMethod(PyObject *obj,
                                         const char *methodname,
                                         const char *format, ...) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PyEval_SetProfile(Py_tracefunc, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_SetTrace(Py_tracefunc, PyObject *) PYSTON_NOEXCEPT;

struct _frame; /* Avoid including frameobject.h */

PyAPI_FUNC(PyObject *) PyEval_GetBuiltins(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyEval_GetGlobals(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyEval_GetLocals(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(struct _frame *) PyEval_GetFrame(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyEval_GetRestricted(void) PYSTON_NOEXCEPT;

/* Look at the current frame's (if any) code's co_flags, and turn on
   the corresponding compiler flags in cf->cf_flags.  Return 1 if any
   flag was set, else return 0. */
PyAPI_FUNC(int) PyEval_MergeCompilerFlags(PyCompilerFlags *cf) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) Py_FlushLine(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) Py_AddPendingCall(int (*func)(void *), void *arg) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) Py_MakePendingCalls(void) PYSTON_NOEXCEPT;

/* Protection against deeply nested recursive calls */
PyAPI_FUNC(void) Py_SetRecursionLimit(int) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) Py_GetRecursionLimit(void) PYSTON_NOEXCEPT;

#define Py_EnterRecursiveCall(where)                                    \
            (_Py_MakeRecCheck(PyThreadState_GET()->recursion_depth) &&  \
             _Py_CheckRecursiveCall(where))
#define Py_LeaveRecursiveCall()                         \
            (--PyThreadState_GET()->recursion_depth)
// Pyston change: changed this to const char*
PyAPI_FUNC(int) _Py_CheckRecursiveCall(const char *where) PYSTON_NOEXCEPT;
PyAPI_DATA(int) _Py_CheckRecursionLimit;
#ifdef USE_STACKCHECK
#  define _Py_MakeRecCheck(x)  (++(x) > --_Py_CheckRecursionLimit)
#else
#  define _Py_MakeRecCheck(x)  (++(x) > _Py_CheckRecursionLimit)
#endif

PyAPI_FUNC(const char *) PyEval_GetFuncName(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(const char *) PyEval_GetFuncDesc(PyObject *) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) PyEval_GetCallStats(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyEval_EvalFrame(struct _frame *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyEval_EvalFrameEx(struct _frame *f, int exc) PYSTON_NOEXCEPT;

/* this used to be handled on a per-thread basis - now just two globals */
PyAPI_DATA(volatile int) _Py_Ticker;
PyAPI_DATA(int) _Py_CheckInterval;

/* Interface for threads.

   A module that plans to do a blocking system call (or something else
   that lasts a long time and doesn't touch Python data) can allow other
   threads to run as follows:

    ...preparations here...
    Py_BEGIN_ALLOW_THREADS
    ...blocking system call here...
    Py_END_ALLOW_THREADS
    ...interpret result here...

   The Py_BEGIN_ALLOW_THREADS/Py_END_ALLOW_THREADS pair expands to a
   {}-surrounded block.
   To leave the block in the middle (e.g., with return), you must insert
   a line containing Py_BLOCK_THREADS before the return, e.g.

    if (...premature_exit...) {
        Py_BLOCK_THREADS
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
    }

   An alternative is:

    Py_BLOCK_THREADS
    if (...premature_exit...) {
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
    }
    Py_UNBLOCK_THREADS

   For convenience, that the value of 'errno' is restored across
   Py_END_ALLOW_THREADS and Py_BLOCK_THREADS.

   WARNING: NEVER NEST CALLS TO Py_BEGIN_ALLOW_THREADS AND
   Py_END_ALLOW_THREADS!!!

   The function PyEval_InitThreads() should be called only from
   initthread() in "threadmodule.c".

   Note that not yet all candidates have been converted to use this
   mechanism!
*/

PyAPI_FUNC(PyThreadState *) PyEval_SaveThread(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_RestoreThread(PyThreadState *) PYSTON_NOEXCEPT;

#ifdef WITH_THREAD

PyAPI_FUNC(int)  PyEval_ThreadsInitialized(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_InitThreads(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_AcquireLock(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_ReleaseLock(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_AcquireThread(PyThreadState *tstate) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_ReleaseThread(PyThreadState *tstate) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyEval_ReInitThreads(void) PYSTON_NOEXCEPT;

// Pyston change: add our internal API here that doesn't make reference to PyThreadState.
// If anyone goes out of their way to use the PyThreadState* APIs directly, we should
// fail instead of assuming that they didn't care about the PyThreadState.
PyAPI_FUNC(void) beginAllowThreads(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) endAllowThreads(void) PYSTON_NOEXCEPT;

// Pyston change: switch these to use our internal API
#define Py_BEGIN_ALLOW_THREADS { \
                        beginAllowThreads();
#define Py_BLOCK_THREADS        endAllowThreads();
#define Py_UNBLOCK_THREADS      beginAllowThreads();
#define Py_END_ALLOW_THREADS    endAllowThreads(); \
                 }

#else /* !WITH_THREAD */

#define Py_BEGIN_ALLOW_THREADS {
#define Py_BLOCK_THREADS
#define Py_UNBLOCK_THREADS
#define Py_END_ALLOW_THREADS }

#endif /* !WITH_THREAD */

PyAPI_FUNC(int) _PyEval_SliceIndex(PyObject *, Py_ssize_t *) PYSTON_NOEXCEPT;


#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL_H */
