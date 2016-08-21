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

#ifndef PYSTON_CORE_THREADING_H
#define PYSTON_CORE_THREADING_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ucontext.h>
#include <vector>

#include "Python.h"

#include "core/common.h"
#include "core/thread_utils.h"

namespace pyston {
class Box;
class BoxedGenerator;

#if ENABLE_SAMPLING_PROFILER
extern int sigprof_pending;
void _printStacktrace();
#endif

extern __thread PyThreadState cur_thread_state;

namespace threading {

// Whether or not a second thread was ever started:
bool threadWasStarted();

// returns a thread id (currently, the pthread_t id)
intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3);

// Hooks to tell the threading machinery about the main thread:
void registerMainThread();
void finishMainThread();

bool isMainThread();

void _allowGLReadPreemption();

#define GIL_CHECK_INTERVAL 1000

// Note: this doesn't need to be an atomic, since it should
// only be accessed by the thread that holds the gil:
extern int gil_check_count;
extern std::atomic<int> threads_waiting_on_gil;
extern "C" inline void allowGLReadPreemption() __attribute__((visibility("default")));
extern "C" inline void allowGLReadPreemption() {
#if ENABLE_SAMPLING_PROFILER
    if (unlikely(sigprof_pending)) {
        // Output multiple stacktraces if we received multiple signals
        // between being able to handle it (such as being in LLVM or the GC),
        // to try to fully account for that time.
        while (sigprof_pending) {
            _printStacktrace();
            sigprof_pending--;
        }
    }
#endif

    // Double-checked locking: first read with no ordering constraint:
    if (!threads_waiting_on_gil.load(std::memory_order_relaxed))
        return;

    gil_check_count++;
    if (likely(gil_check_count < GIL_CHECK_INTERVAL))
        return;

    _allowGLReadPreemption();
}


extern "C" void beginAllowThreads() noexcept;
extern "C" void endAllowThreads() noexcept;

class GLAllowThreadsReadRegion {
public:
    GLAllowThreadsReadRegion() { beginAllowThreads(); }
    ~GLAllowThreadsReadRegion() { endAllowThreads(); }
};


extern bool forgot_refs_via_fork;

} // namespace threading
} // namespace pyston

#endif
