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
#include "core/types.h"

namespace pyston {
class Box;
class BoxedGenerator;

namespace gc {
class GCVisitor;
}

// somewhat similar to CPython's WHY_* enum
// UNWIND_STATE_NORMAL : == WHY_EXCEPTION.  we call it "NORMAL" since we often unwind due to things other than
// exceptions (getGlobals, getLocals, etc)
// UNWIND_STATE_RERAISE: same as NORMAL, except we are supposed to skip the first frame.
// UNWIND_STATE_OSR    : The previous frame was an osr replacement for the next one, so we should skip it
enum UnwindState { UNWIND_STATE_NORMAL = 0, UNWIND_STATE_RERAISE, UNWIND_STATE_OSR };

namespace threading {

class ThreadStateInternal {
private:
    bool saved;
    ucontext_t ucontext;

    void _popGenerator() {
        assert(previous_stacks.size());
        StackInfo& stack = previous_stacks.back();
        stack_start = stack.stack_start;
        previous_stacks.pop_back();
    }
    void _pushGenerator(BoxedGenerator* g, void* new_stack_start, void* old_stack_limit) {
        previous_stacks.emplace_back(g, this->stack_start, old_stack_limit);
        this->stack_start = new_stack_start;
    }

public:
    static __thread ThreadStateInternal* current;

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

    UnwindState unwind_state;
    ExcInfo exc_info;

    PyThreadState* public_thread_state;

    ThreadStateInternal(void* stack_start, pthread_t pthread_id, PyThreadState* public_thread_state);
    void saveCurrent() {
        assert(!saved);
        getcontext(&ucontext);
        saved = true;
    }
    void popCurrent() {
        assert(saved);
        saved = false;
    }
    bool isValid() const { return saved; }
    ucontext_t* getContext() { return &ucontext; }
    void assertNoGenerators() { assert(previous_stacks.size() == 0); }
    void accept(gc::GCVisitor* v);

    // Some hooks to keep track of the list of stacks that this thread has been using.
    // Every time we switch to a new generator, we need to pass a reference to the generator
    // itself (so we can access the registers it is saving), the location of the new stack, and
    // where we stopped executing on the old stack.
    inline static void pushGenerator(BoxedGenerator* g, void* new_stack_start, void* old_stack_limit) {
        assert(new_stack_start);
        assert(old_stack_limit);
        assert(ThreadStateInternal::current);
        current->_pushGenerator(g, new_stack_start, old_stack_limit);
    }

    inline static void popGenerator() {
        assert(ThreadStateInternal::current);
        current->_popGenerator();
    }

    inline static void setUnwindState(UnwindState state) {
        assert(ThreadStateInternal::current);
        current->unwind_state = state;
    }

    inline static UnwindState getUnwindState() {
        assert(ThreadStateInternal::current);
        return current->unwind_state;
    }

    static ExcInfo* getExceptionFerry() {
        assert(ThreadStateInternal::current);
        return &current->exc_info;
    }
};

// Whether or not a second thread was ever started:
bool threadWasStarted();

// returns a thread id (currently, the pthread_t id)
intptr_t start_thread(void* (*start_func)(Box*, Box*, Box*), Box* arg1, Box* arg2, Box* arg3);

// Hooks to tell the threading machinery about the main thread:
void registerMainThread();
void finishMainThread();

// Hook for the GC; will visit all the threads (including the current one), visiting their
// stacks and thread-local PyThreadState objects
void visitAllStacks(gc::GCVisitor* v);

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

extern "C" void beginAllowThreads() noexcept;
extern "C" void endAllowThreads() noexcept;

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
