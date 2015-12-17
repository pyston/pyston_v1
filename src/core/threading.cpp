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

#include "core/threading.h"

#include <cstdio>
#include <cstdlib>
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

namespace pyston {
namespace threading {

std::unordered_set<PerThreadSetBase*> PerThreadSetBase::all_instances;

extern "C" {
__thread PyThreadState cur_thread_state
    = { NULL, 0, 1, NULL, NULL, NULL, NULL }; // not sure if we need to explicitly request zero-initialization
}

PthreadFastMutex threading_lock;

// Certain thread examination functions won't be valid for a brief
// period while a thread is starting up.
// To handle this, track the number of threads in an uninitialized state,
// and wait until they start up.
// As a minor optimization, this is not a std::atomic since it should only
// be checked while the threading_lock is held; might not be worth it.
int num_starting_threads(0);

class ThreadStateInternal {
private:
    bool saved;
    ucontext_t ucontext;

    std::deque<gc::GCVisitable*> gc_objs_stacks;

public:
    void* stack_start;

    struct StackInfo {
        BoxedGenerator* next_generator;
        void* stack_start;
        void* stack_limit;

        StackInfo(BoxedGenerator* next_generator, void* stack_start, void* stack_limit)
            : next_generator(next_generator), stack_start(stack_start), stack_limit(stack_limit) {
#if STACK_GROWS_DOWN
            assert(stack_start > stack_limit);
            assert((char*)stack_start - (char*)stack_limit < (1L << 30));
#else
            assert(stack_start < stack_limit);
            assert((char*)stack_limit - (char*)stack_start < (1L << 30));
#endif
        }
    };

    std::vector<StackInfo> previous_stacks;
    pthread_t pthread_id;

    PyThreadState* public_thread_state;

    ThreadStateInternal(void* stack_start, pthread_t pthread_id, PyThreadState* public_thread_state)
        : saved(false), stack_start(stack_start), pthread_id(pthread_id), public_thread_state(public_thread_state) {}

    void saveCurrent() {
        assert(!saved);
        getcontext(&ucontext);
        saved = true;
    }

    void popCurrent() {
        assert(saved);
        saved = false;
    }

    bool isValid() { return saved; }

    // This is a quick and dirty way to determine if the current thread holds the gil:
    // the only way it can't (at least for now) is if it had saved its threadstate.
    // This only works when looking at a thread that is not actively acquiring or releasing
    // the GIL, so for now just guard on it only being called for the current thread.
    // TODO It's pretty brittle to reuse the saved flag like this.
    bool holdsGil() {
        assert(pthread_self() == this->pthread_id);
        return !saved;
    }

    ucontext_t* getContext() { return &ucontext; }

    void pushGCObject(gc::GCVisitable* obj) { gc_objs_stacks.push_back(obj); }

    void popGCObject(gc::GCVisitable* obj) {
        ASSERT(gc_objs_stacks.back() == obj, "push/pop of stack-bound object out of order");
        gc_objs_stacks.pop_back();
    }

    void pushGenerator(BoxedGenerator* g, void* new_stack_start, void* old_stack_limit) {
        previous_stacks.emplace_back(g, this->stack_start, old_stack_limit);
        this->stack_start = new_stack_start;
    }

    void popGenerator() {
        assert(previous_stacks.size());
        StackInfo& stack = previous_stacks.back();
        stack_start = stack.stack_start;
        previous_stacks.pop_back();
    }

    void assertNoGenerators() { assert(previous_stacks.size() == 0); }

    void accept(gc::GCVisitor* v) {
        auto pub_state = public_thread_state;
        if (pub_state->curexc_type)
            v->visit(&pub_state->curexc_type);
        if (pub_state->curexc_value)
            v->visit(&pub_state->curexc_value);
        if (pub_state->curexc_traceback)
            v->visit(&pub_state->curexc_traceback);
        if (pub_state->dict)
            v->visit(&pub_state->dict);

        for (auto& stack_info : previous_stacks) {
            v->visit(&stack_info.next_generator);
#if STACK_GROWS_DOWN
            v->visitPotentialRange((void**)stack_info.stack_limit, (void**)stack_info.stack_start);
#else
            v->visitPotentialRange((void**)stack_info.stack_start, (void**)stack_info.stack_limit);
#endif
        }

        for (auto& obj : gc_objs_stacks) {
            obj->gc_visit(v);
        }
    }
};
static std::unordered_map<pthread_t, ThreadStateInternal*> current_threads;
static __thread ThreadStateInternal* current_internal_thread_state = 0;

void pushGenerator(BoxedGenerator* g, void* new_stack_start, void* old_stack_limit) {
    assert(new_stack_start);
    assert(old_stack_limit);
    assert(current_internal_thread_state);
    current_internal_thread_state->pushGenerator(g, new_stack_start, old_stack_limit);
}

void popGenerator() {
    assert(current_internal_thread_state);
    current_internal_thread_state->popGenerator();
}

void pushGCObject(gc::GCVisitable* obj) {
    current_internal_thread_state->pushGCObject(obj);
}

void popGCObject(gc::GCVisitable* obj) {
    current_internal_thread_state->popGCObject(obj);
}

// These are guarded by threading_lock
static int signals_waiting(0);
static gc::GCVisitor* cur_visitor = NULL;

// This function should only be called with the threading_lock held:
static void pushThreadState(ThreadStateInternal* thread_state, ucontext_t* context) {
    assert(cur_visitor);
    cur_visitor->visitPotentialRange((void**)context, (void**)(context + 1));

#if STACK_GROWS_DOWN
    void* stack_low = (void*)context->uc_mcontext.gregs[REG_RSP];
    void* stack_high = thread_state->stack_start;
#else
    void* stack_low = thread_state->stack_start;
    void* stack_high = (void*)context->uc_mcontext.gregs[REG_RSP];
#endif

    assert(stack_low < stack_high);
    GC_TRACE_LOG("Visiting other thread's stack\n");
    cur_visitor->visitPotentialRange((void**)stack_low, (void**)stack_high);

    GC_TRACE_LOG("Visiting other thread's threadstate + generator stacks\n");
    thread_state->accept(cur_visitor);
}

// This better not get inlined:
void* getCurrentStackLimit() __attribute__((noinline));
void* getCurrentStackLimit() {
    return __builtin_frame_address(0);
}

static void visitLocalStack(gc::GCVisitor* v) {
    // force callee-save registers onto the stack:
    jmp_buf registers __attribute__((aligned(sizeof(void*))));
    setjmp(registers);
    assert(sizeof(registers) % 8 == 0);
    v->visitPotentialRange((void**)&registers, (void**)((&registers) + 1));

    assert(current_internal_thread_state);
#if STACK_GROWS_DOWN
    void* stack_low = getCurrentStackLimit();
    void* stack_high = current_internal_thread_state->stack_start;
#else
    void* stack_low = current_thread_state->stack_start;
    void* stack_high = getCurrentStackLimit();
#endif

    assert(stack_low < stack_high);

    GC_TRACE_LOG("Visiting current stack from %p to %p\n", stack_low, stack_high);

    v->visitPotentialRange((void**)stack_low, (void**)stack_high);

    GC_TRACE_LOG("Visiting current thread's threadstate + generator stacks\n");
    current_internal_thread_state->accept(v);
}

static void registerThread(bool is_starting_thread) {
    pthread_t current_thread = pthread_self();

    LOCK_REGION(&threading_lock);

    pthread_attr_t thread_attrs;
    int code = pthread_getattr_np(current_thread, &thread_attrs);
    if (code)
        err(1, NULL);

    void* stack_start;
    size_t stack_size;
    code = pthread_attr_getstack(&thread_attrs, &stack_start, &stack_size);
    RELEASE_ASSERT(code == 0, "");

    pthread_attr_destroy(&thread_attrs);

#if STACK_GROWS_DOWN
    void* stack_bottom = static_cast<char*>(stack_start) + stack_size;
#else
    void* stack_bottom = stack_start;
#endif
    current_internal_thread_state = new ThreadStateInternal(stack_bottom, current_thread, &cur_thread_state);
    current_threads[current_thread] = current_internal_thread_state;

    if (is_starting_thread)
        num_starting_threads--;

    if (VERBOSITY() >= 2)
        printf("child initialized; tid=%ld\n", current_thread);
}

static void unregisterThread() {
    current_internal_thread_state->assertNoGenerators();
    {
        pthread_t current_thread = pthread_self();
        LOCK_REGION(&threading_lock);

        current_threads.erase(current_thread);
        if (VERBOSITY() >= 2)
            printf("thread tid=%ld exited\n", current_thread);
    }
    current_internal_thread_state = 0;
}

extern "C" PyGILState_STATE PyGILState_Ensure(void) noexcept {
    if (!current_internal_thread_state) {
        /* Create a new thread state for this thread */
        registerThread(false);
        if (current_internal_thread_state == NULL)
            Py_FatalError("Couldn't create thread-state for new thread");

        acquireGLRead();
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
        unregisterThread();
    }
}

extern "C" PyThreadState* PyGILState_GetThisThreadState(void) noexcept {
    Py_FatalError("unimplemented");
}


void visitAllStacks(gc::GCVisitor* v) {
    visitLocalStack(v);

    // TODO need to prevent new threads from starting,
    // though I suppose that will have been taken care of
    // by the caller of this function.

    LOCK_REGION(&threading_lock);

    assert(cur_visitor == NULL);
    cur_visitor = v;

    while (true) {
        // TODO shouldn't busy-wait:
        if (num_starting_threads) {
            threading_lock.unlock();
            sleep(0);
            threading_lock.lock();
        } else {
            break;
        }
    }

    signals_waiting = (current_threads.size() - 1);

    // Current strategy:
    // Let the other threads decide whether they want to cooperate and save their state before we get here.
    // If they did save their state (as indicated by current_threads[tid]->isValid), then we use that.
    // Otherwise, we send them a signal and use the signal handler to look at their thread state.

    pthread_t mytid = pthread_self();
    for (auto& pair : current_threads) {
        pthread_t tid = pair.first;

        if (tid == mytid)
            continue;

        ThreadStateInternal* state = pair.second;
        if (state->isValid()) {
            pushThreadState(state, state->getContext());
            signals_waiting--;
            continue;
        }

        pthread_kill(tid, SIGUSR2);
    }

    // TODO shouldn't busy-wait:
    while (signals_waiting) {
        threading_lock.unlock();
        // printf("Waiting for %d threads\n", signals_waiting);
        sleep(0);
        threading_lock.lock();
    }

    assert(num_starting_threads == 0);

    cur_visitor = NULL;
}

static void _thread_context_dump(int signum, siginfo_t* info, void* _context) {
    LOCK_REGION(&threading_lock);

    ucontext_t* context = static_cast<ucontext_t*>(_context);

    pthread_t tid = pthread_self();
    if (VERBOSITY() >= 2) {
        printf("in thread_context_dump, tid=%ld\n", tid);
        printf("%p %p %p\n", context, &context, context->uc_mcontext.fpregs);
        printf("old rip: 0x%lx\n", (intptr_t)context->uc_mcontext.gregs[REG_RIP]);
    }

    assert(current_internal_thread_state == current_threads[tid]);
    pushThreadState(current_threads[tid], context);
    signals_waiting--;
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

    threading::GLReadRegion _glock;
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

// from https://www.sourceware.org/ml/guile/2000-07/msg00214.html
static void* find_stack() {
    FILE* input;
    char* line;
    char* s;
    size_t len;
    char hex[9];
    void* start;
    void* end;

    int dummy;

    input = fopen("/proc/self/maps", "r");
    if (input == NULL)
        return NULL;

    len = 0;
    line = NULL;
    while (getline(&line, &len, input) != -1) {
        s = strchr(line, '-');
        if (s == NULL)
            return NULL;
        *s++ = '\0';

        start = (void*)strtoul(line, NULL, 16);
        end = (void*)strtoul(s, NULL, 16);

        if ((void*)&dummy >= start && (void*)&dummy <= end) {
            free(line);
            fclose(input);

#if STACK_GROWS_DOWN
            return end;
#else
            return start;
#endif
        }
    }

    free(line);
    fclose(input);
    return NULL; /* not found =^P */
}

void registerMainThread() {
    LOCK_REGION(&threading_lock);

    assert(!current_internal_thread_state);
    current_internal_thread_state = new ThreadStateInternal(find_stack(), pthread_self(), &cur_thread_state);
    current_threads[pthread_self()] = current_internal_thread_state;

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = _thread_context_dump;
    struct sigaction oldact;

    int code = sigaction(SIGUSR2, &act, &oldact);
    if (code)
        err(1, NULL);

    assert(!PyErr_Occurred());
}

void finishMainThread() {
    assert(current_internal_thread_state);
    current_internal_thread_state->assertNoGenerators();

    // TODO maybe this is the place to wait for non-daemon threads?
}


// For the "AllowThreads" regions, let's save the thread state at the beginning of the region.
// This means that the thread won't get interrupted by the signals we would otherwise need to
// send to get the GC roots.
// It adds some perf overhead I suppose, though I haven't measured it.
// It also means that you're not allowed to do that much inside an AllowThreads region...
// TODO maybe we should let the client decide which way to handle it
extern "C" void beginAllowThreads() noexcept {
    // I don't think it matters whether the GL release happens before or after the state
    // saving; do it before, then, to reduce the amount we hold the GL:
    releaseGLRead();

    {
        LOCK_REGION(&threading_lock);

        assert(current_internal_thread_state);
        current_internal_thread_state->saveCurrent();
    }
}

extern "C" void endAllowThreads() noexcept {
    {
        LOCK_REGION(&threading_lock);

        assert(current_internal_thread_state);
        current_internal_thread_state->popCurrent();
    }


    acquireGLRead();
}

#if THREADING_USE_GIL
#if THREADING_USE_GRWL
#error "Can't turn on both the GIL and the GRWL!"
#endif

static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;

std::atomic<int> threads_waiting_on_gil(0);
static pthread_cond_t gil_acquired = PTHREAD_COND_INITIALIZER;

extern "C" void PyEval_ReInitThreads() noexcept {
    pthread_t current_thread = pthread_self();
    assert(current_threads.count(pthread_self()));

    auto it = current_threads.begin();
    while (it != current_threads.end()) {
        if (it->second->pthread_id == current_thread) {
            ++it;
        } else {
            it = current_threads.erase(it);
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
}

void acquireGLWrite() {
    threads_waiting_on_gil++;
    pthread_mutex_lock(&gil);
    threads_waiting_on_gil--;

    pthread_cond_signal(&gil_acquired);
}

void releaseGLWrite() {
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
#elif THREADING_USE_GRWL
static pthread_rwlock_t grwl = PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP;

enum class GRWLHeldState {
    N,
    R,
    W,
};
static __thread GRWLHeldState grwl_state = GRWLHeldState::N;

static std::atomic<int> writers_waiting(0);

void acquireGLRead() {
    assert(grwl_state == GRWLHeldState::N);
    pthread_rwlock_rdlock(&grwl);
    grwl_state = GRWLHeldState::R;
}

void releaseGLRead() {
    assert(grwl_state == GRWLHeldState::R);
    pthread_rwlock_unlock(&grwl);
    grwl_state = GRWLHeldState::N;
}

void acquireGLWrite() {
    assert(grwl_state == GRWLHeldState::N);

    writers_waiting++;
    pthread_rwlock_wrlock(&grwl);
    writers_waiting--;

    grwl_state = GRWLHeldState::W;
}

void releaseGLWrite() {
    assert(grwl_state == GRWLHeldState::W);
    pthread_rwlock_unlock(&grwl);
    grwl_state = GRWLHeldState::N;
}

void promoteGL() {
    Timer _t2("promoting", /*min_usec=*/10000);

    // Note: this is *not* the same semantics as normal promoting, on purpose.
    releaseGLRead();
    acquireGLWrite();

    long promote_us = _t2.end();
    static thread_local StatPerThreadCounter sc_promoting_us("grwl_promoting_us");
    sc_promoting_us.log(promote_us);
}

void demoteGL() {
    releaseGLWrite();
    acquireGLRead();
}

static __thread int gl_check_count = 0;
void allowGLReadPreemption() {
    assert(grwl_state == GRWLHeldState::R);

    // gl_check_count++;
    // if (gl_check_count < 10)
    // return;
    // gl_check_count = 0;

    if (__builtin_expect(!writers_waiting.load(std::memory_order_relaxed), 1))
        return;

    Timer _t2("preempted", /*min_usec=*/10000);
    pthread_rwlock_unlock(&grwl);
    // The GRWL is a writer-prefered rwlock, so this next statement will block even
    // if the lock is in read mode:
    pthread_rwlock_rdlock(&grwl);

    long preempt_us = _t2.end();
    static thread_local StatPerThreadCounter sc_preempting_us("grwl_preempt_us");
    sc_preempting_us.log(preempt_us);
}
#endif

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

} // namespace threading
} // namespace pyston
