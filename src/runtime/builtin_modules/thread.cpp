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

#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace pyston::threading;


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

BoxedModule* thread_module;

static void* thread_start(Box* target, Box* varargs, Box* kwargs) {
    assert(target);
    assert(varargs);

    try {
        runtimeCall(target, ArgPassSpec(0, 0, true, kwargs != NULL), varargs, kwargs, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        e.printExcAndTraceback();
    }
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

        RELEASE_ASSERT(_waitflag->cls == int_cls, "");
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

static BoxedClass* thread_local_cls;
class BoxedThreadLocal : public Box {
public:
    BoxedThreadLocal() {}

    static Box* getThreadLocalObject(Box* obj) {
        BoxedDict* dict = static_cast<BoxedDict*>(PyThreadState_GetDict());
        Box* tls_obj = dict->getOrNull(obj);
        if (tls_obj == NULL) {
            tls_obj = new BoxedDict();
            setitem(dict, obj, tls_obj);
        }
        return tls_obj;
    }

    static int setattr(Box* obj, char* name, Box* val) {
        Box* tls_obj = getThreadLocalObject(obj);
        setitem(tls_obj, boxString(name), val);
        return 0;
    }

    static Box* getattr(Box* obj, char* name) {
        Box* tls_obj = getThreadLocalObject(obj);
        if (!strcmp(name, "__dict__"))
            return tls_obj;

        try {
            return getitem(tls_obj, boxString(name));
        } catch (ExcInfo e) {
            raiseExcHelper(AttributeError, "'%.50s' object has no attribute '%.400s'", obj->cls->tp_name, name);
        }
    }

    static Box* hash(Box* obj) { return boxInt(PyThread_get_thread_ident()); }


    DEFAULT_CLASS(thread_local_cls);
};

Box* getIdent() {
    return boxInt(pthread_self());
}

Box* stackSize() {
    Py_FatalError("unimplemented");
}

void setupThread() {
    thread_module = createModule("thread", "__builtin__");

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
    thread_lock_cls->giveAttr("__module__", boxStrConstant("thread"));
    thread_lock_cls->giveAttr(
        "acquire", new BoxedFunction(boxRTFunction((void*)BoxedThreadLock::acquire, BOXED_BOOL, 2, 1, false, false),
                                     { boxInt(1) }));
    thread_lock_cls->giveAttr("release", new BoxedFunction(boxRTFunction((void*)BoxedThreadLock::release, NONE, 1)));
    thread_lock_cls->giveAttr("acquire_lock", thread_lock_cls->getattr("acquire"));
    thread_lock_cls->giveAttr("release_lock", thread_lock_cls->getattr("release"));
    thread_lock_cls->giveAttr("__enter__", thread_lock_cls->getattr("acquire"));
    thread_lock_cls->giveAttr("__exit__", new BoxedFunction(boxRTFunction((void*)BoxedThreadLock::exit, NONE, 4)));
    thread_lock_cls->freeze();

    thread_local_cls
        = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(BoxedThreadLocal), false, "_local");
    thread_local_cls->giveAttr("__module__", boxStrConstant("thread"));
    thread_local_cls->giveAttr("__hash__",
                               new BoxedFunction(boxRTFunction((void*)BoxedThreadLocal::hash, BOXED_INT, 1)));
    thread_local_cls->freeze();
    thread_module->giveAttr("_local", thread_local_cls);

    thread_local_cls->tp_setattr = BoxedThreadLocal::setattr;
    thread_local_cls->tp_getattr = BoxedThreadLocal::getattr;

    BoxedClass* ThreadError
        = BoxedHeapClass::create(type_cls, Exception, NULL, Exception->attrs_offset, Exception->tp_weaklistoffset,
                                 Exception->tp_basicsize, false, "error");
    ThreadError->giveAttr("__module__", boxStrConstant("thread"));
    ThreadError->freeze();

    thread_module->giveAttr("error", ThreadError);
}
}
