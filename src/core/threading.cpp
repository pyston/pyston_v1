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
#include "core/thread_utils.h"

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

struct ThreadStartArgs {
    void* (*start_func)(Box*, Box*, Box*);
    Box* arg1, *arg2, *arg3;
};

static pthread_mutex_t threading_lock = PTHREAD_MUTEX_INITIALIZER;
struct ThreadInfo {
    // "bottom" in the sense of a stack, which in a down-growing stack is the highest address:
    void* stack_bottom;
    pthread_t pthread_id;
};
static std::unordered_map<pid_t, ThreadInfo> current_threads;

void* getStackBottom() {
    return current_threads[gettid()].stack_bottom;
}

static int signals_waiting(0);
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
    for (auto& pair : current_threads) {
        pid_t tid = pair.first;
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

    pid_t tid = gettid();
    if (VERBOSITY() >= 2) {
        printf("in thread_context_dump, tid=%d\n", tid);
        printf("%p %p %p\n", context, &context, context->uc_mcontext.fpregs);
        printf("old rip: 0x%lx\n", context->uc_mcontext.gregs[REG_RIP]);
    }

#if STACK_GROWS_DOWN
    void* stack_start = (void*)context->uc_mcontext.gregs[REG_RSP];
    void* stack_end = current_threads[tid].stack_bottom;
#else
    void* stack_start = current_threads[tid].stack_bottom;
    void* stack_end = (void*)(context->uc_mcontext.gregs[REG_RSP] + sizeof(void*));
#endif
    assert(stack_start < stack_end);
    thread_states.push_back(ThreadState(tid, context, stack_start, stack_end));
    signals_waiting--;
}

static void* _thread_start(void* _arg) {
    ThreadStartArgs* arg = static_cast<ThreadStartArgs*>(_arg);
    auto start_func = arg->start_func;
    Box* arg1 = arg->arg1;
    Box* arg2 = arg->arg2;
    Box* arg3 = arg->arg3;
    delete arg;

    {
        LockedRegion _lock(&threading_lock);

        pid_t tid = gettid();
        pthread_t current_thread = pthread_self();

        pthread_attr_t thread_attrs;
        int code = pthread_getattr_np(current_thread, &thread_attrs);
        RELEASE_ASSERT(code == 0, "");

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

        num_starting_threads--;

        if (VERBOSITY() >= 2)
            printf("child initialized; tid=%d\n", gettid());
    }

    threading::GLReadRegion _glock;

    void* rtn = start_func(arg1, arg2, arg3);

    {
        LockedRegion _lock(&threading_lock);

        current_threads.erase(gettid());
        if (VERBOSITY() >= 2)
            printf("thread tid=%d exited\n", gettid());
    }

    return rtn;
}

intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3) {
    {
        LockedRegion _lock(&threading_lock);
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
    LockedRegion _lock(&threading_lock);

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
