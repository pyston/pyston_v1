// Copyright (c) 2014 Dropbox, Inc.
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

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <err.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"

namespace pyston {
namespace threading {

static std::atomic<pyston_tid_t> tid_counter(0);
static pthread_key_t tid_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static void make_key() {
    pthread_key_create(&tid_key, NULL);
}

void allocate_tid() {
    pthread_once(&key_once, make_key);
    pyston_tid_t tid = tid_counter.fetch_add(1);
    pthread_setspecific(tid_key, (void*)tid);
}

pyston_tid_t gettid() {
    return (pyston_tid_t)pthread_getspecific(tid_key);
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

    ucontext_t* context_from_generator;
    int generator_depth;

public:
    void* stack_bottom;
    pthread_t pthread_id;

    ThreadStateInternal(void* stack_bottom, pthread_t pthread_id)
        : saved(false), generator_depth(0), stack_bottom(stack_bottom), pthread_id(pthread_id) {}

    void saveCurrent() {
        assert(!saved);
        if (generator_depth == 0) {
            getcontext(&ucontext);
        }
        saved = true;
    }

    void popCurrent() {
        assert(saved);
        saved = false;
    }

    bool isValid() { return saved || generator_depth; }

    ucontext_t* getContext() {
        if (generator_depth)
            return context_from_generator;
        return &ucontext;
    }

    void pushGenerator(ucontext_t* prev_context) {
        if (generator_depth == 0)
            context_from_generator = prev_context;
        generator_depth++;
    }

    void popGenerator() {
        generator_depth--;
        assert(generator_depth >= 0);
    }

    void assertNoGenerators() { assert(generator_depth == 0); }

    friend void* getStackTop();
};
static std::unordered_map<pyston_tid_t, ThreadStateInternal*> current_threads;

// TODO could optimize these by keeping a __thread local reference to current_threads[gettid()]
void* getStackBottom() {
    return current_threads[gettid()]->stack_bottom;
}

void* getStackTop() {
    ThreadStateInternal* state = current_threads[gettid()];
    int depth = state->generator_depth;
    if (depth == 0) {
        return __builtin_frame_address(0);
    }
    return (void*)state->context_from_generator->uc_mcontext.gregs[REG_RSP];
}

void pushGenerator(ucontext_t* prev_context) {
    current_threads[gettid()]->pushGenerator(prev_context);
}
void popGenerator() {
    current_threads[gettid()]->popGenerator();
}

static int signals_waiting(0);
static std::vector<ThreadState> thread_states;

static void pushThreadState(pthread_t tid, ucontext_t* context) {
#if STACK_GROWS_DOWN
    void* stack_start = (void*)context->uc_mcontext.gregs[REG_RSP];
    void* stack_end = current_threads[tid]->stack_bottom;
#else
    void* stack_start = current_threads[tid]->stack_bottom;
    void* stack_end = (void*)(context->uc_mcontext.gregs[REG_RSP] + sizeof(void*));
#endif
    assert(stack_start < stack_end);
    thread_states.push_back(ThreadState(tid, context, stack_start, stack_end));
}

std::vector<ThreadState> getAllThreadStates() {
    // TODO need to prevent new threads from starting,
    // though I suppose that will have been taken care of
    // by the caller of this function.

    LOCK_REGION(&threading_lock);

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
    thread_states.clear();

    // Current strategy:
    // Let the other threads decide whether they want to cooperate and save their state before we get here.
    // If they did save their state (as indicated by current_threads[tid]->isValid), then we use that.
    // Otherwise, we send them a signal and use the signal handler to look at their thread state.

    pyston_tid_t mytid = gettid();
    for (auto& pair : current_threads) {
        pyston_tid_t tid = pair.first;
        ThreadStateInternal* state = pair.second;

        if (tid == mytid)
            continue;

        // TODO I'm pretty skeptical about this... are we really guaranteed that this is still valid?
        // (in the non-generator case where the thread saved its own state)
        // ex what if an object pointer got pushed onto the stack, below where we thought the stack
        // ended.  We might be able to handle that case by examining the entire stack region, but are
        // there other issues as well?
        if (state->isValid()) {
            pushThreadState(tid, state->getContext());
            signals_waiting--;
            continue;
        }

        pthread_kill(state->pthread_id, SIGUSR2);
    }

    // TODO shouldn't busy-wait:
    while (signals_waiting) {
        threading_lock.unlock();
        // printf("Waiting for %d threads\n", signals_waiting);
        sleep(0);
        threading_lock.lock();
    }

    assert(num_starting_threads == 0);

    return std::move(thread_states);
}

static void _thread_context_dump(int signum, siginfo_t* info, void* _context) {
    LOCK_REGION(&threading_lock);

    ucontext_t* context = static_cast<ucontext_t*>(_context);

    pyston_tid_t tid = gettid();
    if (VERBOSITY() >= 2) {
        printf("in thread_context_dump, tid=%lu\n", tid);
        printf("%p %p %p\n", context, &context, context->uc_mcontext.fpregs);
        printf("old rip: 0x%lx\n", (intptr_t)context->uc_mcontext.gregs[REG_RIP]);
    }

    pushThreadState(tid, context);
    signals_waiting--;
}

struct ThreadStartArgs {
    void* (*start_func)(Box*, Box*, Box*);
    Box* arg1, *arg2, *arg3;
};

static void* _thread_start(void* _arg) {
    allocate_tid();
    ThreadStartArgs* arg = static_cast<ThreadStartArgs*>(_arg);
    auto start_func = arg->start_func;
    Box* arg1 = arg->arg1;
    Box* arg2 = arg->arg2;
    Box* arg3 = arg->arg3;
    delete arg;

    pyston_tid_t tid = gettid();
    {
        LOCK_REGION(&threading_lock);

        pthread_t current_thread = pthread_self();

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
        current_threads[tid] = new ThreadStateInternal(stack_bottom, current_thread);

        num_starting_threads--;

        if (VERBOSITY() >= 2)
            printf("child initialized; tid=%lu\n", gettid());
    }

    threading::GLReadRegion _glock;

    void* rtn = start_func(arg1, arg2, arg3);
    current_threads[tid]->assertNoGenerators();

    {
        LOCK_REGION(&threading_lock);

        current_threads.erase(gettid());
        if (VERBOSITY() >= 2)
            printf("thread tid=%lu exited\n", gettid());
    }

    return rtn;
}

intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3) {
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

    allocate_tid();

    current_threads[gettid()] = new ThreadStateInternal(find_stack(), pthread_self());

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = _thread_context_dump;
    struct sigaction oldact;

    int code = sigaction(SIGUSR2, &act, &oldact);
    if (code)
        err(1, NULL);
}

void finishMainThread() {
    current_threads[gettid()]->assertNoGenerators();

    // TODO maybe this is the place to wait for non-daemon threads?
}


// For the "AllowThreads" regions, let's save the thread state at the beginning of the region.
// This means that the thread won't get interrupted by the signals we would otherwise need to
// send to get the GC roots.
// It adds some perf overhead I suppose, though I haven't measured it.
// It also means that you're not allowed to do that much inside an AllowThreads region...
// TODO maybe we should let the client decide which way to handle it
GLAllowThreadsReadRegion::GLAllowThreadsReadRegion() {
    // I don't think it matters whether the GL release happens before or after the state
    // saving; do it before, then, to reduce the amount we hold the GL:
    releaseGLRead();

    {
        LOCK_REGION(&threading_lock);

        ThreadStateInternal* state = current_threads[gettid()];
        state->saveCurrent();
    }
}

GLAllowThreadsReadRegion::~GLAllowThreadsReadRegion() {
    {
        LOCK_REGION(&threading_lock);
        current_threads[gettid()]->popCurrent();
    }


    acquireGLRead();
}


#if THREADING_USE_GIL
#if THREADING_USE_GRWL
#error "Can't turn on both the GIL and the GRWL!"
#endif

static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;

static std::atomic<int> threads_waiting_on_gil(0);
void acquireGLWrite() {
    threads_waiting_on_gil++;
    pthread_mutex_lock(&gil);
    threads_waiting_on_gil--;
}

void releaseGLWrite() {
    pthread_mutex_unlock(&gil);
}

#define GIL_CHECK_INTERVAL 1000
// Note: this doesn't need to be an atomic, since it should
// only be accessed by the thread that holds the gil:
int gil_check_count = 0;
void allowGLReadPreemption() {
    // Can read this variable with relaxed consistency; not a huge problem if
    // we accidentally read a stale value for a little while.
    if (!threads_waiting_on_gil.load(std::memory_order_relaxed))
        return;

    gil_check_count++;
    if (gil_check_count >= GIL_CHECK_INTERVAL) {
        gil_check_count = 0;
        releaseGLRead();
        acquireGLRead();
    }
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

} // namespace threading
} // namespace pyston
