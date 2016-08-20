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

PyInterpreterState interpreter_state;

std::unordered_set<PerThreadSetBase*> PerThreadSetBase::all_instances;

extern "C" {
__thread PyThreadState cur_thread_state = { NULL, &interpreter_state, NULL, 0, 1, NULL, NULL, NULL, NULL, 0,
                                            NULL }; // not sure if we need to explicitly request zero-initialization
}

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
    bool holds_gil = true;

public:
    pthread_t pthread_id;
    PyThreadState* public_thread_state;

    ThreadStateInternal(pthread_t pthread_id, PyThreadState* tstate)
        : pthread_id(pthread_id), public_thread_state(tstate) {
        HEAD_LOCK();

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

        assert(!holds_gil);
        holds_gil = true;
    }

    void gilReleased() {
        assert(pthread_self() == this->pthread_id);

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

    {
        pthread_t current_thread = pthread_self();
        LOCK_REGION(&threading_lock);

        current_threads.erase(current_thread);
        if (VERBOSITY() >= 2)
            printf("thread tid=%ld exited\n", current_thread);
    }
    delete current_internal_thread_state;
    current_internal_thread_state = 0;
}

extern "C" PyGILState_STATE PyGILState_Ensure(void) noexcept {
    if (!current_internal_thread_state) {
        /* Create a new thread state for this thread */
        registerThread(false);
        if (current_internal_thread_state == NULL)
            Py_FatalError("Couldn't create thread-state for new thread");

        _acquireGIL();
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

    if (oldstate == PyGILState_UNLOCKED) {
        beginAllowThreads();
    }

    if (cur_thread_state.gilstate_counter == 0) {
        assert(oldstate == PyGILState_UNLOCKED);
        RELEASE_ASSERT(0, "this is currently untested");
        // Pyston change:
        unregisterThread();
    }
}

extern "C" PyThreadState* PyGILState_GetThisThreadState(void) noexcept {
    Py_FatalError("unimplemented");
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

    _acquireGIL();
    registerThread(true);
    assert(!PyErr_Occurred());

    void* rtn = start_func(arg1, arg2, arg3);

    unregisterThread();
    _releaseGIL();

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

    _acquireGIL();
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
    {
        // TODO: I think this lock is no longer needed
        LOCK_REGION(&threading_lock);

        assert(current_internal_thread_state);
        current_internal_thread_state->gilReleased();
    }

    _releaseGIL();
}

extern "C" void endAllowThreads() noexcept {
    _acquireGIL();

    {
        LOCK_REGION(&threading_lock);

        assert(current_internal_thread_state);
        current_internal_thread_state->gilTaken();
    }
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

    threads_waiting_on_gil++;
    pthread_cond_wait(&gil_acquired, &gil);
    threads_waiting_on_gil--;
    pthread_cond_signal(&gil_acquired);
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
    // Py_CLEAR(interp->modules);
    // Py_CLEAR(interp->modules_reloading);
    // Py_CLEAR(interp->sysdict);
    // Py_CLEAR(interp->builtins);
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
}

extern "C" PyThreadState* PyInterpreterState_ThreadHead(PyInterpreterState* interp) noexcept {
    return interp->tstate_head;
}

extern "C" PyThreadState* PyThreadState_Next(PyThreadState* tstate) noexcept {
    return tstate->next;
}


} // namespace threading
} // namespace pyston
