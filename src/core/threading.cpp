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

extern "C" int start_thread(void* arg);

namespace pyston {
namespace threading {

// Linux specific: TODO should be in a plat/linux/ directory?
pid_t gettid() {
    pid_t tid = syscall(SYS_gettid);
    assert(tid > 0);
    return tid;
}
int tgkill(int tgid, int tid, int sig) {
    return syscall(SYS_tgkill, tgid, tid, sig);
}

// Certain thread examination functions won't be valid for a brief
// period while a thread is starting up.
// To handle this, track the number of threads in an uninitialized state,
// and wait until they start up.
int num_starting_threads(0);

PthreadFastMutex threading_lock;
struct ThreadInfo {
    // "bottom" in the sense of a stack, which in a down-growing stack is the highest address:
    void* stack_bottom;
    pthread_t pthread_id;
};
static std::unordered_map<pid_t, ThreadInfo> current_threads;

struct ThreadStateInternal {
    bool valid;
    ucontext_t ucontext;

    ThreadStateInternal() : valid(false) {}
};
static std::unordered_map<pid_t, ThreadStateInternal> saved_thread_states;

void* getStackBottom() {
    return current_threads[gettid()].stack_bottom;
}

static int signals_waiting(0);
static std::vector<ThreadState> thread_states;

static void pushThreadState(pid_t tid, ucontext_t* context) {
#if STACK_GROWS_DOWN
    void* stack_start = (void*)context->uc_mcontext.gregs[REG_RSP];
    void* stack_end = current_threads[tid].stack_bottom;
#else
    void* stack_start = current_threads[tid].stack_bottom;
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
    // If they did save their state (as indicated by saved_thread_states[tid].valid), then we use that.
    // Otherwise, we send them a signal and use the signal handler to look at their thread state.

    pid_t tgid = getpid();
    pid_t mytid = gettid();
    for (auto& pair : current_threads) {
        pid_t tid = pair.first;

        // TODO I'm pretty skeptical about this... are we really guaranteed that this is still valid?
        // ex what if an object pointer got pushed onto the stack, below where we thought the stack
        // ended.  We might be able to handle that case by examining the entire stack region, but are
        // there other issues as well?
        if (saved_thread_states[tid].valid) {
            pushThreadState(tid, &saved_thread_states[tid].ucontext);
            signals_waiting--;
            continue;
        }

        if (tid == mytid)
            continue;
        tgkill(tgid, tid, SIGUSR2);
    }

    // TODO shouldn't busy-wait:
    while (signals_waiting) {
        threading_lock.unlock();
        sleep(0);
        threading_lock.lock();
    }

    assert(num_starting_threads == 0);

    return std::move(thread_states);
}

static void _thread_context_dump(int signum, siginfo_t* info, void* _context) {
    LOCK_REGION(&threading_lock);

    ucontext_t* context = static_cast<ucontext_t*>(_context);

    pid_t tid = gettid();
    if (VERBOSITY() >= 2) {
        printf("in thread_context_dump, tid=%d\n", tid);
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
    ThreadStartArgs* arg = static_cast<ThreadStartArgs*>(_arg);
    auto start_func = arg->start_func;
    Box* arg1 = arg->arg1;
    Box* arg2 = arg->arg2;
    Box* arg3 = arg->arg3;
    delete arg;

    {
        LOCK_REGION(&threading_lock);

        pid_t tid = gettid();
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

        current_threads[tid] = ThreadInfo {
#if STACK_GROWS_DOWN
            .stack_bottom = static_cast<char*>(stack_start) + stack_size,
#else
            .stack_bottom = stack_start,
#endif
            .pthread_id = current_thread,
        };
        saved_thread_states[tid] = ThreadStateInternal();

        num_starting_threads--;

        if (VERBOSITY() >= 2)
            printf("child initialized; tid=%d\n", gettid());
    }

    threading::GLReadRegion _glock;

    void* rtn = start_func(arg1, arg2, arg3);

    {
        LOCK_REGION(&threading_lock);

        current_threads.erase(gettid());
        saved_thread_states.erase(gettid());
        if (VERBOSITY() >= 2)
            printf("thread tid=%d exited\n", gettid());
    }

    return rtn;
}

intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3) {
    {
        LOCK_REGION(&threading_lock);
        num_starting_threads++;
    }

    ThreadStartArgs* args = new ThreadStartArgs({ .start_func = start_func, .arg1 = arg1, .arg2 = arg2, .arg3 = arg3 });

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

intptr_t call_frame_base;
void registerMainThread() {
    LOCK_REGION(&threading_lock);

    // Would be nice if we could set this to the pthread start_thread,
    // since _thread_start doesn't always show up in the traceback.
    // call_frame_base = (intptr_t)::start_thread;
    call_frame_base = (intptr_t)_thread_start;

    current_threads[gettid()] = ThreadInfo{
        .stack_bottom = find_stack(), .pthread_id = pthread_self(),
    };

    struct sigaction act;
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = _thread_context_dump;
    struct sigaction oldact;

    int code = sigaction(SIGUSR2, &act, &oldact);
    if (code)
        err(1, NULL);
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

        ThreadStateInternal& state = saved_thread_states[gettid()];
        assert(!state.valid);
        getcontext(&state.ucontext);
        state.valid = true;
    }
}

GLAllowThreadsReadRegion::~GLAllowThreadsReadRegion() {
    {
        LOCK_REGION(&threading_lock);
        saved_thread_states[gettid()].valid = false;
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
