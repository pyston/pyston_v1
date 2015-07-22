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

#include <pthread.h>
#include <stddef.h>

#include "Python.h"
#include "pythread.h"

#include "capi/typeobject.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace pyston::threading;

extern "C" void initthread();


static int initialized;
static void PyThread__init_thread(void); /* Forward */

extern "C" void PyThread_init_thread(void) noexcept {
#ifdef Py_DEBUG
    char* p = Py_GETENV("PYTHONTHREADDEBUG");

    if (p) {
        if (*p)
            thread_debug = atoi(p);
        else
            thread_debug = 1;
    }
#endif /* Py_DEBUG */
    if (initialized)
        return;
    initialized = 1;
    PyThread__init_thread();
}

/* Support for runtime thread stack size tuning.
   A value of 0 means using the platform's default stack size
   or the size specified by the THREAD_STACK_SIZE macro. */
static size_t _pythread_stacksize = 0;

#include "thread_pthread.h"

namespace pyston {

static void* thread_start(Box* target, Box* varargs, Box* kwargs) {
    assert(target);
    assert(varargs);

#if STAT_TIMERS
    // TODO: maybe we should just not log anything for threads...
    static uint64_t* timer_counter = Stats::getStatCounter("us_timer_thread_start");
    StatTimer timer(timer_counter, 0, true);
    timer.pushTopLevel(getCPUTicks());
#endif

    try {
        runtimeCall(target, ArgPassSpec(0, 0, true, kwargs != NULL), varargs, kwargs, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        e.printExcAndTraceback();
    }

#if STAT_TIMERS
    timer.popTopLevel(getCPUTicks());
#endif

    return NULL;
}

// TODO this should take kwargs, which defaults to empty
Box* startNewThread(Box* target, Box* args, Box* kw) {
    intptr_t thread_id = start_thread(&thread_start, target, args, kw);
    return boxInt(thread_id ^ 0x12345678901L);
}

#define CHECK_STATUS(name)                                                                                             \
    if (status != 0) {                                                                                                 \
        perror(name);                                                                                                  \
        error = 1;                                                                                                     \
    }

/*
 * As of February 2002, Cygwin thread implementations mistakenly report error
 * codes in the return value of the sem_ calls (like the pthread_ functions).
 * Correct implementations return -1 and put the code in errno. This supports
 * either.
 *
 * NOTE (2015-05-14): According to `man pthread_mutex_lock` on my system (Ubuntu
 * 14.10), returning the error code is expected behavior. - rntz
 */
static int fix_status(int status) {
    return (status == -1) ? errno : status;
}

static BoxedClass* thread_lock_cls;
class BoxedThreadLock : public Box {
private:
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

public:
    BoxedThreadLock() {}

    DEFAULT_CLASS(thread_lock_cls);

    static Box* acquire(Box* _self, Box* _waitflag) {
        RELEASE_ASSERT(_self->cls == thread_lock_cls, "");
        BoxedThreadLock* self = static_cast<BoxedThreadLock*>(_self);

        RELEASE_ASSERT(isSubclass(_waitflag->cls, int_cls), "");
        int waitflag = static_cast<BoxedInt*>(_waitflag)->n;

        // Copied + adapted from CPython:
        int success;
        auto thelock = &self->lock;
        int status, error = 0;

        {
            threading::GLAllowThreadsReadRegion _allow_threads;

            do {
                if (waitflag)
                    status = fix_status(pthread_mutex_lock(thelock));
                else
                    status = fix_status(pthread_mutex_trylock(thelock));
            } while (status == EINTR); /* Retry if interrupted by a signal */
        }

        if (waitflag) {
            CHECK_STATUS("mutex_lock");
        } else if (status != EBUSY) {
            CHECK_STATUS("mutex_trylock");
        }

        success = (status == 0) ? 1 : 0;

        RELEASE_ASSERT(status == 0 || !waitflag, "could not lock mutex! error %d", status);
        return boxBool(status == 0);
    }

    static Box* release(Box* _self) {
        RELEASE_ASSERT(_self->cls == thread_lock_cls, "");
        BoxedThreadLock* self = static_cast<BoxedThreadLock*>(_self);

        pthread_mutex_unlock(&self->lock);
        return None;
    }

    static Box* exit(Box* _self, Box* arg1, Box* arg2, Box** args) { return release(_self); }
};

Box* allocateLock() {
    return new BoxedThreadLock();
}

Box* getIdent() {
    return boxInt(pthread_self());
}

Box* stackSize() {
    Py_FatalError("unimplemented");
}

void setupThread() {
    // Hacky: we want to use some of CPython's implementation of the thread module (the threading local stuff),
    // and some of ours (thread handling).  Start off by calling a cut-down version of initthread, and then
    // add our own attributes to the module it creates.
    initthread();
    RELEASE_ASSERT(!PyErr_Occurred(), "");

    Box* thread_module = getSysModulesDict()->getOrNull(boxString("thread"));
    assert(thread_module);

    thread_module->giveAttr("start_new_thread", new BoxedBuiltinFunctionOrMethod(
                                                    boxRTFunction((void*)startNewThread, BOXED_INT, 3, 1, false, false),
                                                    "start_new_thread", { NULL }));
    thread_module->giveAttr("allocate_lock", new BoxedBuiltinFunctionOrMethod(
                                                 boxRTFunction((void*)allocateLock, UNKNOWN, 0), "allocate_lock"));
    thread_module->giveAttr(
        "get_ident", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)getIdent, BOXED_INT, 0), "get_ident"));
    thread_module->giveAttr(
        "stack_size", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)stackSize, BOXED_INT, 0), "stack_size"));

    thread_lock_cls = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(BoxedThreadLock), false, "lock");
    thread_lock_cls->giveAttr("__module__", boxString("thread"));
    thread_lock_cls->giveAttr(
        "acquire", new BoxedFunction(boxRTFunction((void*)BoxedThreadLock::acquire, BOXED_BOOL, 2, 1, false, false),
                                     { boxInt(1) }));
    thread_lock_cls->giveAttr("release", new BoxedFunction(boxRTFunction((void*)BoxedThreadLock::release, NONE, 1)));
    thread_lock_cls->giveAttr("acquire_lock", thread_lock_cls->getattr(internStringMortal("acquire")));
    thread_lock_cls->giveAttr("release_lock", thread_lock_cls->getattr(internStringMortal("release")));
    thread_lock_cls->giveAttr("__enter__", thread_lock_cls->getattr(internStringMortal("acquire")));
    thread_lock_cls->giveAttr("__exit__", new BoxedFunction(boxRTFunction((void*)BoxedThreadLock::exit, NONE, 4)));
    thread_lock_cls->freeze();

    BoxedClass* ThreadError
        = BoxedHeapClass::create(type_cls, Exception, NULL, Exception->attrs_offset, Exception->tp_weaklistoffset,
                                 Exception->tp_basicsize, false, "error");
    ThreadError->giveAttr("__module__", boxString("thread"));
    ThreadError->freeze();

    thread_module->giveAttr("error", ThreadError);
}
}
