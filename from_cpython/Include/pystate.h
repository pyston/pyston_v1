// This file is originally from CPython 2.7, with modifications for Pyston

/* Thread and interpreter state structures and their interfaces */


#ifndef Py_PYSTATE_H
#define Py_PYSTATE_H
#ifdef __cplusplus
extern "C" {
#endif

/* State shared between threads */

// Pyston change: this is not our format
#if 0
struct _ts; /* Forward */
struct _is; /* Forward */

typedef struct _is {

    struct _is *next;
    struct _ts *tstate_head;

    PyObject *modules;
    PyObject *sysdict;
    PyObject *builtins;
    PyObject *modules_reloading;

    PyObject *codec_search_path;
    PyObject *codec_search_cache;
    PyObject *codec_error_registry;

#ifdef HAVE_DLOPEN
    int dlopenflags;
#endif
#ifdef WITH_TSC
    int tscdump;
#endif

} PyInterpreterState;
#endif
struct _PyInterpreterState;
typedef struct _PyInterpreterState PyInterpreterState;


/* State unique per thread */

struct _frame; /* Avoid including frameobject.h */

/* Py_tracefunc return -1 when raising an exception, or 0 for success. */
typedef int (*Py_tracefunc)(PyObject *, struct _frame *, int, PyObject *);

/* The following values are used for 'what' for tracefunc functions: */
#define PyTrace_CALL 0
#define PyTrace_EXCEPTION 1
#define PyTrace_LINE 2
#define PyTrace_RETURN 3
#define PyTrace_C_CALL 4
#define PyTrace_C_EXCEPTION 5
#define PyTrace_C_RETURN 6

// Pyston change: this is not our format
#if 0
typedef struct _ts {
    /* See Python/ceval.c for comments explaining most fields */

    struct _ts *next;
    PyInterpreterState *interp;

    struct _frame *frame;
    int recursion_depth;
    /* 'tracing' keeps track of the execution depth when tracing/profiling.
       This is to prevent the actual trace/profile code from being recorded in
       the trace/profile. */
    int tracing;
    int use_tracing;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    PyObject *exc_type;
    PyObject *exc_value;
    PyObject *exc_traceback;

    PyObject *dict;  /* Stores per-thread state */

    /* tick_counter is incremented whenever the check_interval ticker
     * reaches zero. The purpose is to give a useful measure of the number
     * of interpreted bytecode instructions in a given thread.  This
     * extremely lightweight statistic collector may be of interest to
     * profilers (like psyco.jit()), although nothing in the core uses it.
     */
    int tick_counter;

    int gilstate_counter;

    PyObject *async_exc; /* Asynchronous exception to raise */
    long thread_id; /* Thread id where this tstate was created */

    int trash_delete_nesting;
    PyObject *trash_delete_later;

    /* XXX signal handlers should also be here */

} PyThreadState;
#endif
typedef struct _ts {
    int recursion_depth;

    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    PyObject *dict;  /* Stores per-thread state */

    // Pyston note: additions in here need to be mirrored in ThreadStateInternal::accept
} PyThreadState;


PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_New(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyInterpreterState_Clear(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyInterpreterState_Delete(PyInterpreterState *) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyThreadState *) PyThreadState_New(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyThreadState *) _PyThreadState_Prealloc(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyThreadState_Init(PyThreadState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyThreadState_Clear(PyThreadState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyThreadState_Delete(PyThreadState *) PYSTON_NOEXCEPT;
#ifdef WITH_THREAD
PyAPI_FUNC(void) PyThreadState_DeleteCurrent(void) PYSTON_NOEXCEPT;
#endif

PyAPI_FUNC(PyThreadState *) PyThreadState_Get(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyThreadState *) PyThreadState_Swap(PyThreadState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyThreadState_GetDict(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyThreadState_SetAsyncExc(long, PyObject *) PYSTON_NOEXCEPT;


/* Variable and macro for in-line access to current thread state */

// Pyston change: use our internal name for this
//PyAPI_DATA(PyThreadState *) _PyThreadState_Current;
PyAPI_DATA(__thread PyThreadState) cur_thread_state;
#define _PyThreadState_Current (&cur_thread_state)

#ifdef Py_DEBUG
#define PyThreadState_GET() PyThreadState_Get()
#else
#define PyThreadState_GET() (_PyThreadState_Current)
#endif

typedef
    enum {PyGILState_LOCKED, PyGILState_UNLOCKED}
        PyGILState_STATE;

/* Ensure that the current thread is ready to call the Python
   C API, regardless of the current state of Python, or of its
   thread lock.  This may be called as many times as desired
   by a thread so long as each call is matched with a call to
   PyGILState_Release().  In general, other thread-state APIs may
   be used between _Ensure() and _Release() calls, so long as the
   thread-state is restored to its previous state before the Release().
   For example, normal use of the Py_BEGIN_ALLOW_THREADS/
   Py_END_ALLOW_THREADS macros are acceptable.

   The return value is an opaque "handle" to the thread state when
   PyGILState_Ensure() was called, and must be passed to
   PyGILState_Release() to ensure Python is left in the same state. Even
   though recursive calls are allowed, these handles can *not* be shared -
   each unique call to PyGILState_Ensure must save the handle for its
   call to PyGILState_Release.

   When the function returns, the current thread will hold the GIL.

   Failure is a fatal error.
*/
PyAPI_FUNC(PyGILState_STATE) PyGILState_Ensure(void) PYSTON_NOEXCEPT;

/* Release any resources previously acquired.  After this call, Python's
   state will be the same as it was prior to the corresponding
   PyGILState_Ensure() call (but generally this state will be unknown to
   the caller, hence the use of the GILState API.)

   Every call to PyGILState_Ensure must be matched by a call to
   PyGILState_Release on the same thread.
*/
PyAPI_FUNC(void) PyGILState_Release(PyGILState_STATE) PYSTON_NOEXCEPT;

/* Helper/diagnostic function - get the current thread state for
   this thread.  May return NULL if no GILState API has been used
   on the current thread.  Note that the main thread always has such a
   thread-state, even if no auto-thread-state call has been made
   on the main thread.
*/
PyAPI_FUNC(PyThreadState *) PyGILState_GetThisThreadState(void) PYSTON_NOEXCEPT;

/* The implementation of sys._current_frames()  Returns a dict mapping
   thread id to that thread's current frame.
*/
PyAPI_FUNC(PyObject *) _PyThread_CurrentFrames(void) PYSTON_NOEXCEPT;

/* Routines for advanced debuggers, requested by David Beazley.
   Don't use unless you know what you are doing! */
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Head(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyInterpreterState *) PyInterpreterState_Next(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyThreadState *) PyInterpreterState_ThreadHead(PyInterpreterState *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyThreadState *) PyThreadState_Next(PyThreadState *) PYSTON_NOEXCEPT;

typedef struct _frame *(*PyThreadFrameGetter)(PyThreadState *self_);

/* hook for PyEval_GetFrame(), requested for Psyco */
PyAPI_DATA(PyThreadFrameGetter) _PyThreadState_GetFrame;

#ifdef __cplusplus
}
#endif
#endif /* !Py_PYSTATE_H */
