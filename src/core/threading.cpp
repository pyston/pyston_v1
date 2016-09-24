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

#include "core/threading.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <err.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "Python.h"

#include "codegen/codegen.h" // sigprof_pending
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/objmodel.h" // _printStacktrace
#include "runtime/types.h"

extern "C" {
PyThreadState* _PyThreadState_Current;
}

namespace pyston {
namespace threading {

void _acquireGIL();
void _releaseGIL();

#ifdef WITH_THREAD
#include "pythread.h"
static PyThread_type_lock head_mutex = NULL; /* Protects interp->tstate_head */
#define HEAD_INIT() (void)(head_mutex || (head_mutex = PyThread_allocate_lock()))
#define HEAD_LOCK() PyThread_acquire_lock(head_mutex, WAIT_LOCK)
#define HEAD_UNLOCK() PyThread_release_lock(head_mutex)

/* The single PyInterpreterState used by this process'
   GILState implementation
*/
// Pyston change:
// static PyInterpreterState *autoInterpreterState = NULL;
// static int autoTLSkey = 0;
#else
#define HEAD_INIT()   /* Nothing */
#define HEAD_LOCK()   /* Nothing */
#define HEAD_UNLOCK() /* Nothing */
#endif

static PyInterpreterState interpreter_state;

std::unordered_set<PerThreadSetBase*> PerThreadSetBase::all_instances;

PthreadFastMutex threading_lock;

// Certain thread examination functions won't be valid for a brief
// period while a thread is starting up.
// To handle this, track the number of threads in an uninitialized state,
// and wait until they start up.
// As a minor optimization, this is not a std::atomic since it should only
// be checked while the threading_lock is held; might not be worth it.
int num_starting_threads(0);

// TODO: this is a holdover from our GC days, and now there's pretty much nothing left here
// and it should just get refactored out.
class ThreadStateInternal {
private:
    bool holds_gil = false;

public:
    pthread_t pthread_id;
    PyThreadState* public_thread_state;

    ThreadStateInternal(pthread_t pthread_id, PyThreadState* tstate)
        : pthread_id(pthread_id), public_thread_state(tstate) {
        HEAD_LOCK();

        tstate->thread_id = pthread_id;

        tstate->next = interpreter_state.tstate_head;
        interpreter_state.tstate_head = tstate;
        HEAD_UNLOCK();
    }

    bool holdsGil() {
        assert(pthread_self() == this->pthread_id);
        return holds_gil;
    }

    void gilTaken() {
        assert(pthread_self() == this->pthread_id);

        assert(!_PyThreadState_Current);
        _PyThreadState_Current = public_thread_state;

        assert(!holds_gil);
        holds_gil = true;
    }

    void gilReleased() {
        assert(pthread_self() == this->pthread_id);

        assert(_PyThreadState_Current == public_thread_state);
        _PyThreadState_Current = NULL;

        assert(holds_gil);
        holds_gil = false;
    }
};
static std::unordered_map<pthread_t, ThreadStateInternal*> current_threads;
static __thread ThreadStateInternal* current_internal_thread_state = 0;

// These are guarded by threading_lock
static int signals_waiting(0);

// This better not get inlined:
void* getCurrentStackLimit() __attribute__((noinline));
void* getCurrentStackLimit() {
    return __builtin_frame_address(0);
}

static void registerThread(bool is_starting_thread) {
    pthread_t current_thread = pthread_self();

    LOCK_REGION(&threading_lock);

    current_internal_thread_state = new ThreadStateInternal(current_thread, &cur_thread_state);
    current_threads[current_thread] = current_internal_thread_state;

    if (is_starting_thread)
        num_starting_threads--;

    if (VERBOSITY() >= 2)
        printf("child initialized; tid=%ld\n", current_thread);
}

/* Common code for PyThreadState_Delete() and PyThreadState_DeleteCurrent() */
static void tstate_delete_common(PyThreadState* tstate) {
    PyInterpreterState* interp;
    PyThreadState** p;
    PyThreadState* prev_p = NULL;
    if (tstate == NULL)
        Py_FatalError("PyThreadState_Delete: NULL tstate");
    interp = tstate->interp;
    if (interp == NULL)
        Py_FatalError("PyThreadState_Delete: NULL interp");
    HEAD_LOCK();
    for (p = &interp->tstate_head;; p = &(*p)->next) {
        if (*p == NULL)
            Py_FatalError("PyThreadState_Delete: invalid tstate");
        if (*p == tstate)
            break;
        /* Sanity check.  These states should never happen but if
         * they do we must abort.  Otherwise we'll end up spinning in
         * in a tight loop with the lock held.  A similar check is done
         * in thread.c find_key().  */
        if (*p == prev_p)
            Py_FatalError("PyThreadState_Delete: small circular list(!)"
                          " and tstate not found.");
        prev_p = *p;
        if ((*p)->next == interp->tstate_head)
            Py_FatalError("PyThreadState_Delete: circular list(!) and"
                          " tstate not found.");
    }
    *p = tstate->next;
    HEAD_UNLOCK();
    // Pyston change:
    // free(tstate);
}

static void unregisterThread() {
    tstate_delete_common(current_internal_thread_state->public_thread_state);
    PyThreadState_Clear(current_internal_thread_state->public_thread_state);
    assert(current_internal_thread_state->holdsGil());

    {
        pthread_t current_thread = pthread_self();
        LOCK_REGION(&threading_lock);

        current_threads.erase(current_thread);
        if (VERBOSITY() >= 2)
            printf("thread tid=%ld exited\n", current_thread);
    }

    current_internal_thread_state->gilReleased();
    _releaseGIL();

    delete current_internal_thread_state;
    current_internal_thread_state = 0;
}

extern "C" PyGILState_STATE PyGILState_Ensure(void) noexcept {
    if (!current_internal_thread_state) {
        /* Create a new thread state for this thread */
        registerThread(false);
        if (current_internal_thread_state == NULL)
            Py_FatalError("Couldn't create thread-state for new thread");

        endAllowThreads();
        return PyGILState_UNLOCKED;
    } else {
        ++cur_thread_state.gilstate_counter;
        if (current_internal_thread_state->holdsGil()) {
            return PyGILState_LOCKED;
        } else {
            endAllowThreads();
            return PyGILState_UNLOCKED;
        }
    }
}

extern "C" void PyGILState_Release(PyGILState_STATE oldstate) noexcept {
    if (!current_internal_thread_state)
        Py_FatalError("auto-releasing thread-state, but no thread-state for this thread");

    --cur_thread_state.gilstate_counter;
    RELEASE_ASSERT(cur_thread_state.gilstate_counter >= 0, "");

    if (cur_thread_state.gilstate_counter == 0) {
        assert(oldstate == PyGILState_UNLOCKED);
        RELEASE_ASSERT(0, "this is currently untested");
        // Pyston change:
        unregisterThread();
        return;
    }

    if (oldstate == PyGILState_UNLOCKED) {
        beginAllowThreads();
    }
}

extern "C" PyThreadState* PyGILState_GetThisThreadState(void) noexcept {
    return &cur_thread_state;
}

struct ThreadStartArgs {
    void* (*start_func)(Box*, Box*, Box*);
    Box* arg1, *arg2, *arg3;
};

static void* _thread_start(void* _arg) {
    ThreadStartArgs* arg = static_cast<ThreadStartArgs*>(_arg);
    auto start_func = arg->start_func;
    Box* arg1 = arg->arg1;
    Box* arg2 = arg->arg2;
    Box* arg3 = arg->arg3;
    delete arg;

    registerThread(true);
    endAllowThreads();
    assert(!PyErr_Occurred());

    void* rtn = start_func(arg1, arg2, arg3);

    unregisterThread();

    return rtn;
}

static bool thread_was_started = false;
bool threadWasStarted() {
    return thread_was_started;
}

intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3) {
    thread_was_started = true;

    {
        LOCK_REGION(&threading_lock);
        num_starting_threads++;
    }

    ThreadStartArgs* args = new ThreadStartArgs({.start_func = start_func, .arg1 = arg1, .arg2 = arg2, .arg3 = arg3 });

    pthread_t thread_id;
    int code = pthread_create(&thread_id, NULL, &_thread_start, args);
    RELEASE_ASSERT(code == 0, "");
    if (VERBOSITY() >= 2)
        printf("pthread thread_id: 0x%lx\n", thread_id);
    pthread_detach(thread_id);

    static_assert(sizeof(pthread_t) <= sizeof(intptr_t), "");
    return thread_id;
}

static long main_thread_id;

void registerMainThread() {
    LOCK_REGION(&threading_lock);

    HEAD_INIT();

    main_thread_id = pthread_self();

    assert(!interpreter_state.tstate_head);
    assert(!current_internal_thread_state);
    current_internal_thread_state = new ThreadStateInternal(pthread_self(), &cur_thread_state);
    current_threads[pthread_self()] = current_internal_thread_state;

    endAllowThreads();
}

/* Wait until threading._shutdown completes, provided
   the threading module was imported in the first place.
   The shutdown routine will wait until all non-daemon
   "threading" threads have completed. */
static void wait_for_thread_shutdown(void) noexcept {
#ifdef WITH_THREAD
    PyObject* result;
    PyThreadState* tstate = PyThreadState_GET();
    PyObject* threading = PyMapping_GetItemString(getSysModulesDict(), "threading");
    if (threading == NULL) {
        /* threading not imported */
        PyErr_Clear();
        return;
    }
    result = PyObject_CallMethod(threading, "_shutdown", "");
    if (result == NULL)
        PyErr_WriteUnraisable(threading);
    else
        Py_DECREF(result);
    Py_DECREF(threading);
#endif
}

void finishMainThread() {
    assert(current_internal_thread_state);

    wait_for_thread_shutdown();
}

bool isMainThread() {
    return pthread_self() == main_thread_id;
}


// For the "AllowThreads" regions, let's save the thread state at the beginning of the region.
// This means that the thread won't get interrupted by the signals we would otherwise need to
// send to get the GC roots.
// It adds some perf overhead I suppose, though I haven't measured it.
// It also means that you're not allowed to do that much inside an AllowThreads region...
// TODO maybe we should let the client decide which way to handle it
extern "C" void beginAllowThreads() noexcept {
    assert(current_internal_thread_state);
    current_internal_thread_state->gilReleased();

    _releaseGIL();
}

extern "C" void endAllowThreads() noexcept {
    _acquireGIL();

    assert(current_internal_thread_state);
    current_internal_thread_state->gilTaken();
}

static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;

std::atomic<int> threads_waiting_on_gil(0);
static pthread_cond_t gil_acquired = PTHREAD_COND_INITIALIZER;
bool forgot_refs_via_fork = false;

extern "C" void PyEval_ReInitThreads() noexcept {
    pthread_t current_thread = pthread_self();
    assert(current_threads.count(pthread_self()));

    auto it = current_threads.begin();
    while (it != current_threads.end()) {
        if (it->second->pthread_id == current_thread) {
            ++it;
        } else {
            PyThreadState_Clear(it->second->public_thread_state);
            tstate_delete_common(it->second->public_thread_state);
            delete it->second;
            it = current_threads.erase(it);

            // Like CPython, we make no effort to try to clean anything referenced via other
            // threads.  Set this variable to know that that we won't be able to do much leak
            // checking after this happens.
            forgot_refs_via_fork = true;
        }
    }

    // We need to make sure the threading lock is released, so we unconditionally unlock it. After a fork, we are the
    // only thread, so this won't race; and since it's a "fast" mutex (see `man pthread_mutex_lock`), this works even
    // if it isn't locked. If we needed to avoid unlocking a non-locked mutex, though, we could trylock it first:
    //
    //     int err = pthread_mutex_trylock(&threading_lock.mutex);
    //     ASSERT(!err || err == EBUSY, "pthread_mutex_trylock failed, but not with EBUSY");
    //
    threading_lock.unlock();

    num_starting_threads = 0;
    threads_waiting_on_gil = 0;

    PerThreadSetBase::runAllForkHandlers();

    /* Update the threading module with the new state.
     */
    Box* threading = PyMapping_GetItemString(getSysModulesDict(), "threading");
    if (threading == NULL) {
        /* threading not imported */
        PyErr_Clear();
        return;
    }
    Box* result = PyObject_CallMethod(threading, "_after_fork", NULL);
    if (result == NULL)
        PyErr_WriteUnraisable(threading);
    else
        Py_DECREF(result);
    Py_DECREF(threading);
}

void _acquireGIL() {
    threads_waiting_on_gil++;
    pthread_mutex_lock(&gil);
    threads_waiting_on_gil--;

    pthread_cond_signal(&gil_acquired);
}

void _releaseGIL() {
    pthread_mutex_unlock(&gil);
}

int gil_check_count = 0;

// TODO: this function is fair in that it forces a thread to give up the GIL
// after a bounded amount of time, but currently we have no guarantees about
// who it will release the GIL to.  So we could have two threads that are
// switching back and forth, and a third that never gets run.
// We could enforce fairness by having a FIFO of events (implementd with mutexes?)
// and make sure to always wake up the longest-waiting one.
void _allowGLReadPreemption() {
    assert(gil_check_count >= GIL_CHECK_INTERVAL);
    gil_check_count = 0;

    // Double check this, since if we are wrong about there being a thread waiting on the gil,
    // we're going to get stuck in the following pthread_cond_wait:
    if (!threads_waiting_on_gil.load(std::memory_order_seq_cst))
        return;

    current_internal_thread_state->gilReleased();

    threads_waiting_on_gil++;
    pthread_cond_wait(&gil_acquired, &gil);
    threads_waiting_on_gil--;
    pthread_cond_signal(&gil_acquired);

    current_internal_thread_state->gilTaken();
}

// We don't support CPython's TLS (yet?)
extern "C" void PyThread_ReInitTLS(void) noexcept {
    // don't have to do anything since we don't support TLS
}
extern "C" int PyThread_create_key(void) noexcept {
    Py_FatalError("unimplemented");
}
extern "C" void PyThread_delete_key(int) noexcept {
    Py_FatalError("unimplemented");
}
extern "C" int PyThread_set_key_value(int, void*) noexcept {
    Py_FatalError("unimplemented");
}
extern "C" void* PyThread_get_key_value(int) noexcept {
    Py_FatalError("unimplemented");
}
extern "C" void PyThread_delete_key_value(int key) noexcept {
    Py_FatalError("unimplemented");
}


extern "C" {
volatile int _stop_thread = 1;
}

// The number of threads with pending async excs
static std::atomic<int> _async_excs;

static PyThread_type_lock pending_lock = 0; /* for pending calls */

/* The WITH_THREAD implementation is thread-safe.  It allows
   scheduling to be made from any thread, and even from an executing
   callback.
 */

#define NPENDINGCALLS 32
static struct {
    int (*func)(void*);
    void* arg;
} pendingcalls[NPENDINGCALLS];
static int pendingfirst = 0;
static int pendinglast = 0;
// Not sure if this has to be atomic:
static std::atomic<int> pendingcalls_to_do(1); /* trigger initialization of lock */
static char pendingbusy = 0;

// _stop_thread is the OR of a number of conditions that should stop threads.
// When these conditions become true, we can unconditionally set _stop_thread=1,
// but when a condition becomes false, we have to check all the conditions:
static void _recalcStopThread() {
    _stop_thread = (_async_excs > 0 || (pendingfirst != pendinglast));
}

extern "C" int Py_AddPendingCall(int (*func)(void*), void* arg) noexcept {
    int i, j, result = 0;
    PyThread_type_lock lock = pending_lock;

    /* try a few times for the lock.  Since this mechanism is used
     * for signal handling (on the main thread), there is a (slim)
     * chance that a signal is delivered on the same thread while we
     * hold the lock during the Py_MakePendingCalls() function.
     * This avoids a deadlock in that case.
     * Note that signals can be delivered on any thread.  In particular,
     * on Windows, a SIGINT is delivered on a system-created worker
     * thread.
     * We also check for lock being NULL, in the unlikely case that
     * this function is called before any bytecode evaluation takes place.
     */
    if (lock != NULL) {
        for (i = 0; i < 100; i++) {
            if (PyThread_acquire_lock(lock, NOWAIT_LOCK))
                break;
        }
        if (i == 100)
            return -1;
    }

    i = pendinglast;
    j = (i + 1) % NPENDINGCALLS;
    if (j == pendingfirst) {
        result = -1; /* Queue full */
    } else {
        pendingcalls[i].func = func;
        pendingcalls[i].arg = arg;
        pendinglast = j;
    }
    /* signal main loop */
    // Pyston change: we don't have a _Py_Ticker
    // _Py_Ticker = 0;

    _stop_thread = 1;
    if (lock != NULL)
        PyThread_release_lock(lock);
    return result;
}

extern "C" int Py_MakePendingCalls(void) noexcept {
    int i;
    int r = 0;

    if (!pending_lock) {
        /* initial allocation of the lock */
        pending_lock = PyThread_allocate_lock();
        if (pending_lock == NULL)
            return -1;
    }

    if (cur_thread_state.async_exc) {
        auto x = cur_thread_state.async_exc;
        cur_thread_state.async_exc = NULL;
        PyErr_SetNone(x);
        Py_DECREF(x);

        _async_excs--;
        _recalcStopThread();

        return -1;
    }

    /* only service pending calls on main thread */
    // Pyston change:
    // if (main_thread && PyThread_get_thread_ident() != main_thread)
    if (!threading::isMainThread())
        return 0;
    /* don't perform recursive pending calls */
    if (pendingbusy)
        return 0;
    pendingbusy = 1;
    /* perform a bounded number of calls, in case of recursion */
    for (i = 0; i < NPENDINGCALLS; i++) {
        int j;
        int (*func)(void*);
        void* arg = NULL;

        /* pop one item off the queue while holding the lock */
        PyThread_acquire_lock(pending_lock, WAIT_LOCK);
        j = pendingfirst;
        if (j == pendinglast) {
            func = NULL; /* Queue empty */
        } else {
            func = pendingcalls[j].func;
            arg = pendingcalls[j].arg;
            pendingfirst = (j + 1) % NPENDINGCALLS;
        }
        _recalcStopThread();
        PyThread_release_lock(pending_lock);
        /* having released the lock, perform the callback */
        if (func == NULL)
            break;
        r = func(arg);
        if (r)
            break;
    }
    pendingbusy = 0;
    return r;
}

extern "C" void makePendingCalls() {
    int ret = Py_MakePendingCalls();
    if (ret != 0)
        throwCAPIException();
}

extern "C" int PyThreadState_SetAsyncExc(long id, PyObject* exc) noexcept {
    PyThreadState* tstate = PyThreadState_GET();
    PyInterpreterState* interp = tstate->interp;
    PyThreadState* p;

    /* Although the GIL is held, a few C API functions can be called
     * without the GIL held, and in particular some that create and
     * destroy thread and interpreter states.  Those can mutate the
     * list of thread states we're traversing, so to prevent that we lock
     * head_mutex for the duration.
     */
    HEAD_LOCK();
    for (p = interp->tstate_head; p != NULL; p = p->next) {
        if (p->thread_id == id) {
            /* Tricky:  we need to decref the current value
             * (if any) in p->async_exc, but that can in turn
             * allow arbitrary Python code to run, including
             * perhaps calls to this function.  To prevent
             * deadlock, we need to release head_mutex before
             * the decref.
             */
            PyObject* old_exc = p->async_exc;
            Py_XINCREF(exc);
            p->async_exc = exc;

            _async_excs++;
            _stop_thread = 1;

            HEAD_UNLOCK();
            Py_XDECREF(old_exc);
            return 1;
        }
    }
    HEAD_UNLOCK();
    return 0;
}

extern "C" PyObject* _PyThread_CurrentFrames(void) noexcept {
    try {
        LOCK_REGION(&threading_lock);
        BoxedDict* result = new BoxedDict();
        for (auto& pair : current_threads) {
            FrameInfo* frame_info = (FrameInfo*)pair.second->public_thread_state->frame_info;
            Box* frame = getFrame(frame_info);
            assert(frame);
            PyDict_SetItem(result, autoDecref(boxInt(pair.first)), frame);
        }
        return result;
    } catch (ExcInfo) {
        RELEASE_ASSERT(0, "not implemented");
    }
}

extern "C" void PyInterpreterState_Clear(PyInterpreterState* interp) noexcept {
    PyThreadState* p;
    HEAD_LOCK();
    for (p = interp->tstate_head; p != NULL; p = p->next)
        PyThreadState_Clear(p);
    HEAD_UNLOCK();
    // Py_CLEAR(interp->codec_search_path);
    // Py_CLEAR(interp->codec_search_cache);
    // Py_CLEAR(interp->codec_error_registry);
    Py_CLEAR(interp->modules);
    // Py_CLEAR(interp->modules_reloading);
    // Py_CLEAR(interp->sysdict);
    Py_CLEAR(interp->builtins);
}

extern "C" void PyThreadState_DeleteCurrent() noexcept {
    Py_FatalError("unimplemented");
}

extern "C" void PyThreadState_Clear(PyThreadState* tstate) noexcept {
    assert(tstate);

    assert(!tstate->trash_delete_later);
    // TODO: should we try to clean this up at all?
    // CPython decrefs the frame object:
    // assert(!tstate->frame_info);

    Py_CLEAR(tstate->dict);
    Py_CLEAR(tstate->curexc_type);
    Py_CLEAR(tstate->curexc_value);
    Py_CLEAR(tstate->curexc_traceback);

    Py_CLEAR(tstate->async_exc);
}

extern "C" PyThreadState* PyInterpreterState_ThreadHead(PyInterpreterState* interp) noexcept {
    return interp->tstate_head;
}

extern "C" PyThreadState* PyThreadState_Next(PyThreadState* tstate) noexcept {
    return tstate->next;
}


extern "C" void PyEval_AcquireThread(PyThreadState* tstate) noexcept {
    RELEASE_ASSERT(tstate == &cur_thread_state, "");
    endAllowThreads();
}

extern "C" void PyEval_ReleaseThread(PyThreadState* tstate) noexcept {
    RELEASE_ASSERT(tstate == &cur_thread_state, "");
    beginAllowThreads();
}

extern "C" PyThreadState* PyThreadState_Get(void) noexcept {
    if (_PyThreadState_Current == NULL)
        Py_FatalError("PyThreadState_Get: no current thread");

    return _PyThreadState_Current;
}

extern "C" PyThreadState* PyEval_SaveThread(void) noexcept {
    auto rtn = PyThreadState_GET();
    assert(rtn);
    beginAllowThreads();
    return rtn;
}

extern "C" void PyEval_RestoreThread(PyThreadState* tstate) noexcept {
    RELEASE_ASSERT(tstate == &cur_thread_state, "");
    endAllowThreads();
}

} // namespace threading

__thread PyThreadState cur_thread_state
    = { NULL, &threading::interpreter_state, NULL, 0, 1, NULL, NULL, NULL, NULL, 0, NULL, NULL, 0 };

} // namespace pyston
