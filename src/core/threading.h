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

#ifndef PYSTON_CORE_THREADING_H
#define PYSTON_CORE_THREADING_H

#include <cstdint>
#include <cstring>
#include <ucontext.h>
#include <vector>

#include "core/common.h"
#include "core/thread_utils.h"

namespace pyston {
class Box;

namespace threading {

// Whether or not a second thread was ever started:
bool threadWasStarted();

struct ThreadState {
    Box* exc_type, *exc_value, *exc_traceback;
};
extern __thread ThreadState cur_thread_state;

// returns a thread id (currently, the pthread_t id)
intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3);

void registerMainThread();
void finishMainThread();

struct ThreadGCState {
    pthread_t tid; // useful mostly for debugging
    ucontext_t* ucontext;

    // start and end (start < end) of the threads main stack.
    // The thread may not be actually executing on that stack, since it may be
    // in a generator, but those generators will be tracked separately.
    void* stack_start, *stack_end;

    ThreadState* thread_state;

    ThreadGCState(pthread_t tid, ucontext_t* ucontext, void* stack_start, void* stack_end, ThreadState* thread_state)
        : tid(tid), ucontext(ucontext), stack_start(stack_start), stack_end(stack_end), thread_state(thread_state) {}
};
// Gets a ThreadGCState per thread, not including the thread calling this function.
// For this call to make sense, the threads all should be blocked;
// as a corollary, this thread is very much not thread safe.
std::vector<ThreadGCState> getAllThreadStates();

// Get the stack "bottom" (ie first pushed data.  For stacks that grow down, this
// will be the highest address).
void* getStackBottom();
void* getStackTop();

// We need to track the state of the thread's main stack.  This can get complicated when
// generators are involved, so we add some hooks for the generator code to notify the threading
// code that it has switched onto of off of a generator.
// A generator should call pushGenerator() when it gets switched to, with a pointer to the context
// that it will return to (ie the context of the thing that called the generator).
// The generator should call popGenerator() when it is about to switch back to the caller.
void pushGenerator(ucontext_t* prev_context);
void popGenerator();


#ifndef THREADING_USE_GIL
#define THREADING_USE_GIL 1
#define THREADING_USE_GRWL 0
#endif
#define THREADING_SAFE_DATASTRUCTURES THREADING_USE_GRWL

#if THREADING_SAFE_DATASTRUCTURES
#define DS_DEFINE_MUTEX(name) pyston::threading::PthreadFastMutex name

#define DS_DECLARE_RWLOCK(name) extern pyston::threading::PthreadRWLock name
#define DS_DEFINE_RWLOCK(name) pyston::threading::PthreadRWLock name

#define DS_DEFINE_SPINLOCK(name) pyston::threading::PthreadSpinLock name
#else
#define DS_DEFINE_MUTEX(name) pyston::threading::NopLock name

#define DS_DECLARE_RWLOCK(name) extern pyston::threading::NopLock name
#define DS_DEFINE_RWLOCK(name) pyston::threading::NopLock name

#define DS_DEFINE_SPINLOCK(name) pyston::threading::NopLock name
#endif

void acquireGLRead();
void releaseGLRead();
void acquireGLWrite();
void releaseGLWrite();
void allowGLReadPreemption();
// Note: promoteGL is free to drop the lock and then reacquire
void promoteGL();
void demoteGL();



#define MAKE_REGION(name, start, end)                                                                                  \
    class name {                                                                                                       \
    public:                                                                                                            \
        name() { start(); }                                                                                            \
        ~name() { end(); }                                                                                             \
    };

MAKE_REGION(GLReadRegion, acquireGLRead, releaseGLRead);
MAKE_REGION(GLPromoteRegion, promoteGL, demoteGL);
// MAKE_REGION(GLReadReleaseRegion, releaseGLRead, acquireGLRead);
// MAKE_REGION(GLWriteReleaseRegion, releaseGLWrite, acquireGLWrite);
#undef MAKE_REGION

extern "C" void beginAllowThreads();
extern "C" void endAllowThreads();

class GLAllowThreadsReadRegion {
public:
    GLAllowThreadsReadRegion() { beginAllowThreads(); }
    ~GLAllowThreadsReadRegion() { endAllowThreads(); }
};


#if THREADING_USE_GIL
inline void acquireGLRead() {
    acquireGLWrite();
}
inline void releaseGLRead() {
    releaseGLWrite();
}
inline void promoteGL() {
}
inline void demoteGL() {
}
#endif

#if !THREADING_USE_GIL && !THREADING_USE_GRWL
inline void acquireGLRead() {
}
inline void releaseGLRead() {
}
inline void acquireGLWrite() {
}
inline void releaseGLWrite() {
}
inline void promoteGL() {
}
inline void demoteGL() {
}
inline void allowGLReadPreemption() {
}
#endif


} // namespace threading
} // namespace pyston

#endif
