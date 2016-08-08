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

// See https://docs.python.org/2/reference/expressions.html#yieldexpr for the relevant Python language reference
// documentation on generators.

#include "runtime/generator.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <sys/mman.h>
#include <ucontext.h>

#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/ctxswitching.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

static uint64_t next_stack_addr = 0x4270000000L;
static std::deque<uint64_t> available_addrs;

// There should be a better way of getting this:
#define PAGE_SIZE 4096

#define INITIAL_STACK_SIZE (8 * PAGE_SIZE)
#define STACK_REDZONE_SIZE PAGE_SIZE
#define MAX_STACK_SIZE (4 * 1024 * 1024)

static llvm::DenseMap<void*, BoxedGenerator*> s_generator_map;
static_assert(THREADING_USE_GIL, "have to make the generator map thread safe!");

class RegisterHelper {
private:
    void* frame_addr;

public:
    RegisterHelper(BoxedGenerator* generator, void* frame_addr) : frame_addr(frame_addr) {
        s_generator_map[frame_addr] = generator;
    }
    ~RegisterHelper() {
        assert(s_generator_map.count(frame_addr));
        s_generator_map.erase(frame_addr);
    }
};

static void freeGeneratorStack(BoxedGenerator* g) {
    if (g->stack_begin == NULL)
        return;

    available_addrs.push_back((uint64_t)g->stack_begin);
    // Limit the number of generator stacks we keep around:
    if (available_addrs.size() > 5) {
        uint64_t addr = available_addrs.front();
        available_addrs.pop_front();
        int r = munmap((void*)(addr - MAX_STACK_SIZE), MAX_STACK_SIZE);
        assert(r == 0);
    }

    g->stack_begin = NULL;
}

Context* getReturnContextForGeneratorFrame(void* frame_addr) {
    BoxedGenerator* generator = s_generator_map[frame_addr];
    assert(generator);
    return generator->returnContext;
}

void generatorEntry(BoxedGenerator* g) noexcept {
    {
        assert(g->cls == generator_cls);
        assert(g->function->cls == function_cls);

        assert(g->returnValue == Py_None);
        Py_CLEAR(g->returnValue);

        {
            RegisterHelper context_registerer(g, __builtin_frame_address(0));

            g->top_caller_frame_info = (FrameInfo*)cur_thread_state.frame_info;

            // call body of the generator
            BoxedFunctionBase* func = g->function;
            // unnecessary because the generator owns g->function
            // KEEP_ALIVE(func);

            Box** args = g->args ? &g->args->elts[0] : nullptr;
            auto r = callCLFunc<ExceptionStyle::CAPI, NOT_REWRITABLE>(func->md, nullptr, func->md->numReceivedArgs(),
                                                                      func->closure, g, func->globals, g->arg1, g->arg2,
                                                                      g->arg3, args);
            if (r)
                Py_DECREF(r);
            else {
                // unhandled exception: propagate the exception to the caller
                PyErr_Fetch(&g->exception.type, &g->exception.value, &g->exception.traceback);
                PyErr_Clear();
            }
        }

        // we returned from the body of the generator. next/send/throw will notify the caller
        g->entryExited = true;
    }
    assert(g->top_caller_frame_info == cur_thread_state.frame_info);
    swapContext(&g->context, g->returnContext, 0);
}

Box* generatorIter(Box* s) {
    return incref(s);
}

// called from both generatorHasNext and generatorSend/generatorNext (but only if generatorHasNext hasn't been called)
template <ExceptionStyle S> static bool generatorSendInternal(BoxedGenerator* self, Box* v) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_generator_switching", 0);

    if (!self->returnContext && v != Py_None) {
        if (S == CAPI) {
            PyErr_SetString(TypeError, "can't send non-None value to a just-started generator");
            return true;
        } else
            raiseExcHelper(TypeError, "can't send non-None value to a just-started generator");
    }

    if (self->running) {
        if (S == CAPI) {
            PyErr_SetString(ValueError, "generator already executing");
            return true;
        } else
            raiseExcHelper(ValueError, "generator already executing");
    }

    // check if the generator already exited
    if (self->entryExited) {
        freeGeneratorStack(self);
        if (S == CAPI) {
            PyErr_SetObject(StopIteration, Py_None);
            return true;
        } else
            raiseExcHelper(StopIteration, (const char*)nullptr);
    }

    assert(!self->returnValue);
    self->returnValue = incref(v);
    self->running = true;

#if STAT_TIMERS
    if (!self->prev_stack)
        self->prev_stack = StatTimer::createStack(self->my_timer);
    else
        self->prev_stack = StatTimer::swapStack(self->prev_stack);
#endif
    auto* top_caller_frame_info = (FrameInfo*)cur_thread_state.frame_info;
    swapContext(&self->returnContext, self->context, (intptr_t)self);
    assert(cur_thread_state.frame_info == top_caller_frame_info
           && "the generator should reset the frame info before the swapContext");


#if STAT_TIMERS
    self->prev_stack = StatTimer::swapStack(self->prev_stack);
    if (self->entryExited) {
        assert(self->prev_stack == &self->my_timer);
        assert(self->my_timer.isPaused());
    }
#endif

    self->running = false;

    // propagate exception to the caller
    if (self->exception.type) {
        freeGeneratorStack(self);
        // don't raise StopIteration exceptions because those are handled specially.
        if (!self->exception.matches(StopIteration)) {
            if (S == CAPI) {
                setCAPIException(self->exception);
                self->exception = ExcInfo(NULL, NULL, NULL);
                return true;
            } else {
                auto exc = self->exception;
                self->exception = ExcInfo(NULL, NULL, NULL);
                throw exc;
            }
        }
        return false;
    }

    if (self->entryExited) {
        freeGeneratorStack(self);
        // Reset the current exception.
        // We could directly create the StopIteration exception but we delay creating it because often the caller is not
        // interested in the exception (=generatorHasnext). If we really need it we will create it inside generatorSend.
        assert(!self->exception.type && "need to decref existing exception");
        self->exception = ExcInfo(NULL, NULL, NULL);
        return false;
    }
    return false;
}

template <ExceptionStyle S> static Box* generatorSend(Box* s, Box* v) noexcept(S == CAPI) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    if (self->iterated_from__hasnext__)
        Py_FatalError(".throw called on generator last advanced with __hasnext__");

    bool exc = generatorSendInternal<S>(self, v);
    if (S == CAPI && exc)
        return NULL;

    // throw StopIteration if the generator exited
    if (self->entryExited) {
        // But we can't just create a new exc because the generator may have exited because of an explicit
        // 'raise StopIterationSubClass, "test"' statement and we can't replace it with the generic StopIteration
        // exception.
        // That's why we set inside 'generatorSendInternal()' 'self->exception' to the raised StopIteration exception or
        // create a new one if the generator exited implicit.
        // CPython raises the custom exception just once, on the next generator 'next' it will we a normal StopIteration
        // exc.
        assert(self->exception.type == NULL || self->exception.matches(StopIteration));
        ExcInfo old_exc = self->exception;
        // Clear the exception for GC purposes:
        self->exception = ExcInfo(nullptr, nullptr, nullptr);
        if (old_exc.type == NULL) {
            if (S == CAPI) {
                PyErr_SetObject(StopIteration, Py_None);
                return NULL;
            } else
                raiseExcHelper(StopIteration, (const char*)nullptr);
        } else {
            if (S == CAPI) {
                setCAPIException(old_exc);
                return NULL;
            } else
                throw old_exc;
        }
    }

    Box* rtn = self->returnValue;
    assert(rtn);
    self->returnValue = NULL;
    return rtn;
}

template <ExceptionStyle S>
Box* generatorThrow(Box* s, BoxedClass* exc_cls, Box* exc_val = nullptr, Box** args = nullptr) noexcept(S == CAPI) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    if (self->iterated_from__hasnext__ && !self->entryExited)
        Py_FatalError(".throw called on generator last advanced with __hasnext__");

    Box* exc_tb = args ? args[0] : nullptr;
    if (exc_tb && exc_tb != Py_None && !PyTraceBack_Check(exc_tb)) {
        if (S == CAPI) {
            PyErr_SetString(TypeError, "throw() third argument must be a traceback object");
            return NULL;
        }
        raiseExcHelper(TypeError, "throw() third argument must be a traceback object");
    }
    if (!exc_val)
        exc_val = Py_None;
    if (!exc_tb)
        exc_tb = Py_None;

    ExcInfo exc_info = excInfoForRaise(incref(exc_cls), incref(exc_val), incref(exc_tb));
    if (self->entryExited) {
        if (S == CAPI) {
            setCAPIException(exc_info);
            return NULL;
        }
        throw exc_info;
    }

    self->exception = exc_info;
    return generatorSend<S>(self, Py_None);
}

template <ExceptionStyle S> Box* generatorClose(Box* s) noexcept(S == CAPI) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    // check if the generator already exited
    if (self->entryExited)
        return incref(Py_None);

    if (S == CAPI) {
        Box* rtn = generatorThrow<S>(self, GeneratorExit, nullptr, nullptr);
        if (rtn) {
            PyErr_SetString(RuntimeError, "generator ignored GeneratorExit");
            return NULL;
        }
        if (PyErr_ExceptionMatches(PyExc_StopIteration) || PyErr_ExceptionMatches(PyExc_GeneratorExit)) {
            PyErr_Clear();
            return incref(Py_None);
        }
        return NULL;
    } else {
        try {
            autoDecref(generatorThrow<S>(self, GeneratorExit, nullptr, nullptr));
            raiseExcHelper(RuntimeError, "generator ignored GeneratorExit");
        } catch (ExcInfo e) {
            if (e.matches(StopIteration) || e.matches(GeneratorExit)) {
                e.clear();
                return incref(Py_None);
            }
            throw e;
        }
    }
    assert(0); // unreachable
}

template <ExceptionStyle S> static Box* generatorNext(Box* s) noexcept(S == CAPI) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    if (self->iterated_from__hasnext__) {
        self->iterated_from__hasnext__ = false;
        Box* rtn = self->returnValue;
        assert(rtn);
        self->returnValue = NULL;
        return rtn;
    }

    return generatorSend<S>(s, Py_None);
}

llvm_compat_bool generatorHasnextUnboxed(Box* s) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    if (!self->iterated_from__hasnext__) {
        generatorSendInternal<CXX>(self, Py_None);
        self->iterated_from__hasnext__ = true;
    }

    return !self->entryExited;
}

Box* generatorHasnext(Box* s) {
    return boxBool(generatorHasnextUnboxed(s));
}

template <ExceptionStyle S>
static Box* yieldInternal(BoxedGenerator* obj, STOLEN(Box*) value,
                          llvm::ArrayRef<Box*> live_values) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_generator_switching", 0);

    assert(obj->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(obj);
    assert(!self->returnValue);
    self->returnValue = value;

    FrameInfo* generator_frame_info = (FrameInfo*)cur_thread_state.frame_info;
    // a generator will only switch back (yield/unhandled exception) to its caller when it is one frame away from the
    // caller
    assert(self->top_caller_frame_info == generator_frame_info->back);

    // reset current frame to the caller tops frame --> removes the frame the generator added
    cur_thread_state.frame_info = self->top_caller_frame_info;
    obj->paused_frame_info = generator_frame_info;
    obj->live_values = live_values;
    swapContext(&self->context, self->returnContext, 0);
    FrameInfo* top_new_caller_frame_info = (FrameInfo*)cur_thread_state.frame_info;
    obj->paused_frame_info = NULL;
    obj->live_values = llvm::ArrayRef<Box*>();

    // the caller of the generator can change between yield statements that means we can't just restore the top of the
    // frame to the point before the yield instead we have to update it.
    if (top_new_caller_frame_info != self->top_caller_frame_info) {
        // caller changed
        self->top_caller_frame_info = top_new_caller_frame_info;
        generator_frame_info->back = top_new_caller_frame_info;
        if (generator_frame_info->frame_obj)
            frameInvalidateBack(generator_frame_info->frame_obj);
    }
    cur_thread_state.frame_info = generator_frame_info;

    // if the generator receives a exception from the caller we have to throw it
    if (self->exception.type) {
        ExcInfo e = self->exception;
        self->exception = ExcInfo(NULL, NULL, NULL);
        Py_CLEAR(self->returnValue);
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        }
        throw e;
    }

    Box* r = self->returnValue;
    self->returnValue = NULL;
    return r;
}

extern "C" Box* yield_capi(BoxedGenerator* obj, STOLEN(Box*) value, int num_live_values, ...) noexcept {
    Box** live_values = (Box**)alloca(sizeof(Box*) * num_live_values);
    va_list ap;
    va_start(ap, num_live_values);
    for (int i = 0; i < num_live_values; ++i) {
        live_values[i] = va_arg(ap, Box*);
    }
    va_end(ap);

    return yieldInternal<CAPI>(obj, value, llvm::makeArrayRef(live_values, num_live_values));
}

extern "C" Box* yield(BoxedGenerator* obj, STOLEN(Box*) value, llvm::ArrayRef<Box*> live_values) {
    return yieldInternal<CXX>(obj, value, live_values);
}

extern "C" BoxedGenerator* createGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args) {
    assert(function);
    assert(function->cls == function_cls);
    return new BoxedGenerator(function, arg1, arg2, arg3, args);
}

#if STAT_TIMERS
static uint64_t* generator_timer_counter = Stats::getStatCounter("us_timer_generator_toplevel");
#endif
extern "C" BoxedGenerator::BoxedGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args)
    : function(function),
      arg1(arg1),
      arg2(arg2),
      arg3(arg3),
      args(nullptr),
      entryExited(false),
      running(false),
      returnValue(nullptr),
      exception(nullptr, nullptr, nullptr),
      context(nullptr),
      returnContext(nullptr),
      top_caller_frame_info(nullptr),
      paused_frame_info(nullptr)
#if STAT_TIMERS
      ,
      prev_stack(NULL),
      my_timer(generator_timer_counter, 0, true)
#endif
{
    Py_INCREF(function);

    int numArgs = function->md->numReceivedArgs();
    if (numArgs > 0)
        Py_XINCREF(arg1);
    if (numArgs > 1)
        Py_XINCREF(arg2);
    if (numArgs > 2)
        Py_XINCREF(arg3);
    if (numArgs > 3) {
        numArgs -= 3;
        this->args = new (numArgs) GCdArray();
        memcpy(&this->args->elts[0], args, numArgs * sizeof(Box*));
        for (int i = 0; i < numArgs; i++) {
            Py_XINCREF(args[i]);
        }
    }

    static StatCounter generator_stack_reused("generator_stack_reused");
    static StatCounter generator_stack_created("generator_stack_created");

    void* initial_stack_limit;
    if (available_addrs.size() == 0) {
        generator_stack_created.log();

        uint64_t stack_low = next_stack_addr;
        uint64_t stack_high = stack_low + MAX_STACK_SIZE;
        next_stack_addr = stack_high;

#if STACK_GROWS_DOWN
        this->stack_begin = (void*)stack_high;

        initial_stack_limit = (void*)(stack_high - INITIAL_STACK_SIZE);
        void* p = mmap(initial_stack_limit, INITIAL_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
        ASSERT(p == initial_stack_limit, "%p %s", p, strerror(errno));

        // Create an inaccessible redzone so that the generator stack won't grow indefinitely.
        // Looks like it throws a SIGBUS if we reach the redzone; it's unclear if that's better
        // or worse than being able to consume all available memory.
        void* p2
            = mmap((void*)stack_low, STACK_REDZONE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
        assert(p2 == (void*)stack_low);
        // Interestingly, it seems like MAP_GROWSDOWN will leave a page-size gap between the redzone and the growable
        // region.

        if (VERBOSITY() >= 3) {
            printf("Created new generator stack, starts at %p, currently extends to %p\n", (void*)stack_high,
                   initial_stack_limit);
            printf("Created a redzone from %p-%p\n", (void*)stack_low, (void*)(stack_low + STACK_REDZONE_SIZE));
        }
#else
#error "implement me"
#endif
    } else {
        generator_stack_reused.log();

#if STACK_GROWS_DOWN
        uint64_t stack_high = available_addrs.back();
        this->stack_begin = (void*)stack_high;
        initial_stack_limit = (void*)(stack_high - INITIAL_STACK_SIZE);
        available_addrs.pop_back();
#else
#error "implement me"
#endif
    }

    assert(((intptr_t)stack_begin & (~(intptr_t)(0xF))) == (intptr_t)stack_begin && "stack must be aligned");

    context = makeContext(stack_begin, (void (*)(intptr_t))generatorEntry);
}

Box* generator_name(Box* _self, void* context) noexcept {
    assert(isSubclass(_self->cls, generator_cls));
    BoxedGenerator* self = static_cast<BoxedGenerator*>(_self);

    return incref(self->function->md->source->getName());
}

extern "C" int PyGen_NeedsFinalizing(PyGenObject* gen) noexcept {
    auto self = (BoxedGenerator*)gen;

    // CPython has some optimizations for not needing to finalize generators that haven't exited, but
    // which are guaranteed to not need any special cleanups.
    // For now just say anything still in-progress needs finalizing.
    if (!(bool)self->paused_frame_info)
        return false;

    return true;
// TODO: is this safe? probably not...
// return self->paused_frame_info->stmt->type == AST_TYPE::Invoke;
#if 0
    int i;
    PyFrameObject* f = gen->gi_frame;

    if (f == NULL || f->f_stacktop == NULL || f->f_iblock <= 0)
        return 0; /* no frame or empty blockstack == no finalization */

    /* Any block type besides a loop requires cleanup. */
    i = f->f_iblock;
    while (--i >= 0) {
        if (f->f_blockstack[i].b_type != SETUP_LOOP)
            return 1;
    }

    /* No blocks except loops, it's safe to skip finalization. */
    return 0;
#endif
}

static void generator_del(PyObject* self) noexcept {
    PyObject* res;
    PyObject* error_type, *error_value, *error_traceback;
    BoxedGenerator* gen = (BoxedGenerator*)self;

    // Pyston change:
    // if (gen->gi_frame == NULL || gen->gi_frame->f_stacktop == NULL)
    if (!gen->paused_frame_info)
        /* Generator isn't paused, so no need to close */
        return;

    /* Temporarily resurrect the object. */
    assert(self->ob_refcnt == 0);
    self->ob_refcnt = 1;

    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);

    // Pyston change:
    // res = gen_close(gen, NULL);
    res = generatorClose<CAPI>((Box*)gen);

    if (res == NULL)
        PyErr_WriteUnraisable(self);
    else
        Py_DECREF(res);

    /* Restore the saved exception. */
    PyErr_Restore(error_type, error_value, error_traceback);

    /* Undo the temporary resurrection; can't use DECREF here, it would
     * cause a recursive call.
     */
    assert(self->ob_refcnt > 0);
    if (--self->ob_refcnt == 0)
        return; /* this is the normal path out */

    /* close() resurrected it!  Make it look like the original Py_DECREF
     * never happened.
     */
    {
        Py_ssize_t refcnt = self->ob_refcnt;
        _Py_NewReference(self);
        self->ob_refcnt = refcnt;
    }
    assert(PyType_IS_GC(self->cls) && _Py_AS_GC(self)->gc.gc_refs != _PyGC_REFS_UNTRACKED);

    /* If Py_REF_DEBUG, _Py_NewReference bumped _Py_RefTotal, so
     * we need to undo that. */
    _Py_DEC_REFTOTAL;
/* If Py_TRACE_REFS, _Py_NewReference re-added self to the object
 * chain, so no more to do there.
 * If COUNT_ALLOCS, the original decref bumped tp_frees, and
 * _Py_NewReference bumped tp_allocs:  both of those need to be
 * undone.
 */
#ifdef COUNT_ALLOCS
    --self->ob_type->tp_frees;
    --self->ob_type->tp_allocs;
#endif
}

static void generator_dealloc(BoxedGenerator* self) noexcept {
    assert(isSubclass(self->cls, generator_cls));

    // Hopefully this never happens:
    assert(!self->running);

    _PyObject_GC_UNTRACK(self);

    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs(self);

    _PyObject_GC_TRACK(self);

    if (self->paused_frame_info) {
        Py_TYPE(self)->tp_del(self);
        if (self->ob_refcnt > 0)
            return; /* resurrected.  :( */
    }

    _PyObject_GC_UNTRACK(self);

    freeGeneratorStack(self);

    int numArgs = self->function->md->numReceivedArgs();
    if (numArgs > 3) {
        for (int i = 0; i < numArgs - 3; i++) {
            Py_CLEAR(self->args->elts[i]);
        }
    }
    if (numArgs > 2)
        Py_CLEAR(self->arg3);
    if (numArgs > 1)
        Py_CLEAR(self->arg2);
    if (numArgs > 0)
        Py_CLEAR(self->arg1);

    Py_CLEAR(self->function);

    Py_CLEAR(self->returnValue);

    Py_CLEAR(self->exception.type);
    Py_CLEAR(self->exception.value);
    Py_CLEAR(self->exception.traceback);

    self->cls->tp_free(self);
}

static int generator_traverse(BoxedGenerator* self, visitproc visit, void* arg) noexcept {
    assert(isSubclass(self->cls, generator_cls));

    if (self->paused_frame_info) {
        int r = frameinfo_traverse(self->paused_frame_info, visit, arg);
        if (r)
            return r;
    }

    for (auto v : self->live_values) {
        Py_VISIT(v);
    }

    int numArgs = self->function->md->numReceivedArgs();
    if (numArgs > 3) {
        for (int i = 0; i < numArgs - 3; i++) {
            Py_VISIT(self->args->elts[i]);
        }
    }
    if (numArgs > 2)
        Py_VISIT(self->arg3);
    if (numArgs > 1)
        Py_VISIT(self->arg2);
    if (numArgs > 0)
        Py_VISIT(self->arg1);

    Py_VISIT(self->function);

    Py_VISIT(self->returnValue);

    Py_VISIT(self->exception.type);
    Py_VISIT(self->exception.value);
    Py_VISIT(self->exception.traceback);

    return 0;
}

void setupGenerator() {
    generator_cls = BoxedClass::create(type_cls, object_cls, 0, offsetof(BoxedGenerator, weakreflist),
                                       sizeof(BoxedGenerator), false, "generator", false, (destructor)generator_dealloc,
                                       NULL, true, (traverseproc)generator_traverse, NOCLEAR);
    generator_cls->giveAttr(
        "__iter__", new BoxedFunction(FunctionMetadata::create((void*)generatorIter, typeFromClass(generator_cls), 1)));

    auto generator_close = FunctionMetadata::create((void*)generatorClose<CXX>, UNKNOWN, 1);
    generator_close->addVersion((void*)generatorClose<CAPI>, UNKNOWN, CAPI);
    generator_cls->giveAttr("close", new BoxedFunction(generator_close));

    auto generator_next = FunctionMetadata::create((void*)generatorNext<CXX>, UNKNOWN, 1, ParamNames::empty(), CXX);
    generator_next->addVersion((void*)generatorNext<CAPI>, UNKNOWN, CAPI);
    generator_cls->giveAttr("next", new BoxedFunction(generator_next));

    FunctionMetadata* hasnext = FunctionMetadata::create((void*)generatorHasnextUnboxed, BOOL, 1);
    hasnext->addVersion((void*)generatorHasnext, BOXED_BOOL);
    generator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));

    auto generator_send = FunctionMetadata::create((void*)generatorSend<CXX>, UNKNOWN, 2);
    generator_send->addVersion((void*)generatorSend<CAPI>, UNKNOWN, CAPI);
    generator_cls->giveAttr("send", new BoxedFunction(generator_send));

    auto generator_throw = FunctionMetadata::create((void*)generatorThrow<CXX>, UNKNOWN, 4, false, false);
    generator_throw->addVersion((void*)generatorThrow<CAPI>, UNKNOWN, CAPI);
    generator_cls->giveAttr("throw", new BoxedFunction(generator_throw, { NULL, NULL }));

    generator_cls->giveAttrDescriptor("__name__", generator_name, NULL);

    generator_cls->freeze();
    generator_cls->tp_iter = PyObject_SelfIter;
    generator_cls->tp_del = generator_del; // don't do giveAttr("__del__") because it should not be visible from python
    generator_cls->tpp_hasnext = generatorHasnextUnboxed;
    generator_cls->tp_iternext = generatorNext<CAPI>;
}
}
