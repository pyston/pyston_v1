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

class LockedRegion {
private:
    pthread_mutex_t* mutex;

public:
    LockedRegion(pthread_mutex_t* mutex) : mutex(mutex) { pthread_mutex_lock(mutex); }
    ~LockedRegion() { pthread_mutex_unlock(mutex); }
};



// Certain thread examination functions won't be valid for a brief
// period while a thread is starting up.
// To handle this, track the number of threads in an uninitialized state,
// and wait until they start up.
int num_starting_threads(0);

struct ThreadStartArgs {
    void* (*start_func)(Box*, Box*, Box*);
    Box* arg1, *arg2, *arg3;
};

static pthread_mutex_t threading_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<pid_t> current_threads;

static std::atomic<int> signals_waiting(0);
static std::vector<ThreadState> thread_states;
std::vector<ThreadState> getAllThreadStates() {
    // TODO need to prevent new threads from starting,
    // though I suppose that will have been taken care of
    // by the caller of this function.

    LockedRegion _lock(&threading_lock);

    while (true) {
        // TODO shouldn't busy-wait:
        if (num_starting_threads) {
            pthread_mutex_unlock(&threading_lock);
            sleep(0);
            pthread_mutex_lock(&threading_lock);
        } else {
            break;
        }
    }

    signals_waiting = (current_threads.size() - 1);
    thread_states.clear();

    pid_t tgid = getpid();
    pid_t mytid = gettid();
    for (pid_t tid : current_threads) {
        if (tid == mytid)
            continue;
        tgkill(tgid, tid, SIGUSR2);
    }

    // TODO shouldn't busy-wait:
    while (signals_waiting) {
        pthread_mutex_unlock(&threading_lock);
        sleep(0);
        pthread_mutex_lock(&threading_lock);
    }

    assert(num_starting_threads == 0);

    return std::move(thread_states);
}

static void _thread_context_dump(int signum, siginfo_t* info, void* _context) {
    LockedRegion _lock(&threading_lock);

    ucontext_t* context = static_cast<ucontext_t*>(_context);

    if (VERBOSITY()) {
        pid_t tid = gettid();
        printf("in thread_context_dump, tid=%d\n", tid);
        printf("%p %p %p\n", context, &context, context->uc_mcontext.fpregs);
        printf("old rip: 0x%lx\n", context->uc_mcontext.gregs[REG_RIP]);
    }

    thread_states.push_back(ThreadState(context->uc_mcontext.gregs));
    signals_waiting--; // atomic on std::atomic
}

void registerMainThread() {
    LockedRegion _lock(&threading_lock);

    current_threads.push_back(gettid());

    struct sigaction act = {
        .sa_flags = SA_SIGINFO, .sa_sigaction = _thread_context_dump,
    };
    struct sigaction oldact;
    int code = sigaction(SIGUSR2, &act, &oldact);
    if (code)
        err(1, NULL);
}

static void* _thread_start(void* _arg) {
    pid_t tid = gettid();
    ThreadStartArgs* arg = static_cast<ThreadStartArgs*>(_arg);
    auto start_func = arg->start_func;
    Box* arg1 = arg->arg1;
    Box* arg2 = arg->arg2;
    Box* arg3 = arg->arg3;
    delete arg;

    {
        LockedRegion _lock(&threading_lock);

        current_threads.push_back(tid);
        num_starting_threads--;

        if (VERBOSITY() >= 2)
            printf("child initialized; tid=%d\n", tid);
    }

    threading::GLReadRegion _glock;

    return start_func(arg1, arg2, arg3);
}

intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3) {
    {
        LockedRegion _lock(&threading_lock);
        num_starting_threads++;
    }

    ThreadStartArgs* args = new ThreadStartArgs({ .start_func = start_func, .arg1 = arg1, .arg2 = arg2, .arg3 = arg3 });

    pthread_t thread_id;
    int code = pthread_create(&thread_id, NULL, &_thread_start, args);
    assert(code == 0);
    if (VERBOSITY() >= 2)
        printf("pthread thread_id: 0x%lx\n", thread_id);

    static_assert(sizeof(pthread_t) <= sizeof(intptr_t), "");
    return thread_id;
}


#if THREADING_USE_GIL
static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;

void acquireGLWrite() {
    pthread_mutex_lock(&gil);
}

void releaseGLWrite() {
    pthread_mutex_unlock(&gil);
}
#endif

} // namespace threading
} // namespace pyston
