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
#include "gc/collector.h"
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

static std::unordered_map<void*, BoxedGenerator*> s_generator_map;
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

Context* getReturnContextForGeneratorFrame(void* frame_addr) {
    BoxedGenerator* generator = s_generator_map[frame_addr];
    assert(generator);
    return generator->returnContext;
}

void generatorEntry(BoxedGenerator* g) {
    assert(g->cls == generator_cls);
    assert(g->function->cls == function_cls);

    threading::pushGenerator(g, g->stack_begin, g->returnContext);

    try {
        RegisterHelper context_registerer(g, __builtin_frame_address(0));

        // call body of the generator
        BoxedFunctionBase* func = g->function;

        Box** args = g->args ? &g->args->elts[0] : nullptr;
        callCLFunc(func->f, nullptr, func->f->numReceivedArgs(), func->closure, g, g->arg1, g->arg2, g->arg3, args);
    } catch (ExcInfo e) {
        // unhandled exception: propagate the exception to the caller
        g->exception = e;
    }

    // we returned from the body of the generator. next/send/throw will notify the caller
    g->entryExited = true;
    threading::popGenerator();
    swapContext(&g->context, g->returnContext, 0);
}

Box* generatorIter(Box* s) {
    return s;
}

Box* generatorSend(Box* s, Box* v) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    if (self->running)
        raiseExcHelper(ValueError, "generator already executing");

    // check if the generator already exited
    if (self->entryExited)
        raiseExcHelper(StopIteration, "");

    self->returnValue = v;
    self->running = true;
    swapContext(&self->returnContext, self->context, (intptr_t)self);
    self->running = false;

    // propagate exception to the caller
    if (self->exception.type)
        raiseRaw(self->exception);

    // throw StopIteration if the generator exited
    if (self->entryExited)
        raiseExcHelper(StopIteration, "");

    return self->returnValue;
}

Box* generatorThrow(Box* s, BoxedClass* e) {
    assert(s->cls == generator_cls);
    assert(isSubclass(e, Exception));
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);
    Box* ex = runtimeCall(e, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    self->exception = ExcInfo(ex->cls, ex, None);
    return generatorSend(self, None);
}

Box* generatorClose(Box* s) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    // check if the generator already exited
    if (self->entryExited)
        return None;

    return generatorThrow(self, GeneratorExit);
}

Box* generatorNext(Box* s) {
    return generatorSend(s, None);
}

extern "C" Box* yield(BoxedGenerator* obj, Box* value) {
    assert(obj->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(obj);
    self->returnValue = value;

    threading::popGenerator();
    swapContext(&self->context, self->returnContext, 0);
    threading::pushGenerator(obj, obj->stack_begin, obj->returnContext);

    // if the generator receives a exception from the caller we have to throw it
    if (self->exception.type) {
        ExcInfo e = self->exception;
        self->exception = ExcInfo(NULL, NULL, NULL);
        raiseRaw(e);
    }
    return self->returnValue;
}


extern "C" BoxedGenerator* createGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args) {
    assert(function);
    assert(function->cls == function_cls);
    return new BoxedGenerator(function, arg1, arg2, arg3, args);
}


extern "C" BoxedGenerator::BoxedGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args)
    : function(function), arg1(arg1), arg2(arg2), arg3(arg3), args(nullptr), entryExited(false), running(false),
      returnValue(nullptr), exception(nullptr, nullptr, nullptr), context(nullptr), returnContext(nullptr) {

    int numArgs = function->f->num_args;
    if (numArgs > 3) {
        numArgs -= 3;
        this->args = new (numArgs) GCdArray();
        memcpy(&this->args->elts[0], args, numArgs * sizeof(Box*));
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

        if (VERBOSITY() >= 1) {
            printf("Created new generator stack, starts at %p, currently extends to %p\n", (void*)stack_high,
                   initial_stack_limit);
            printf("Created a redzone from %p-%p\n", (void*)stack_low, (void*)(stack_low + STACK_REDZONE_SIZE));
        }
#else
#error "implement me"
#endif

        gc::registerGCManagedBytes(MAX_STACK_SIZE);
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

extern "C" void generatorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedGenerator* g = (BoxedGenerator*)b;

    v->visit(g->function);
    int num_args = g->function->f->num_args;
    if (num_args >= 1)
        v->visit(g->arg1);
    if (num_args >= 2)
        v->visit(g->arg2);
    if (num_args >= 3)
        v->visit(g->arg3);
    if (g->args)
        v->visit(g->args);
    if (num_args > 3)
        v->visitPotentialRange(reinterpret_cast<void* const*>(&g->args->elts[0]),
                               reinterpret_cast<void* const*>(&g->args->elts[num_args - 3]));
    if (g->returnValue)
        v->visit(g->returnValue);
    if (g->exception.type)
        v->visit(g->exception.type);
    if (g->exception.value)
        v->visit(g->exception.value);
    if (g->exception.traceback)
        v->visit(g->exception.traceback);

    if (g->running) {
        v->visitPotentialRange((void**)&g->returnContext,
                               ((void**)&g->returnContext) + sizeof(g->returnContext) / sizeof(void*));
    } else {
        // g->context is always set for a running generator, but we can trigger a GC while constructing
        // a generator in which case we can see a NULL context
        if (g->context) {
#if STACK_GROWS_DOWN
            v->visitPotentialRange((void**)g->context, (void**)g->stack_begin);
#endif
        }
    }
}

Box* generatorName(Box* _self, void* context) {
    assert(isSubclass(_self->cls, generator_cls));
    BoxedGenerator* self = static_cast<BoxedGenerator*>(_self);

    return boxString(self->function->f->source->getName());
}

void generatorDestructor(Box* b) {
    assert(isSubclass(b->cls, generator_cls));
    BoxedGenerator* self = static_cast<BoxedGenerator*>(b);

    if (self->stack_begin) {
        available_addrs.push_back((uint64_t)self->stack_begin);
        // Limit the number of generator stacks we keep around:
        if (available_addrs.size() > 5) {
            uint64_t addr = available_addrs.front();
            available_addrs.pop_front();
            int r = munmap((void*)(addr - MAX_STACK_SIZE), MAX_STACK_SIZE);
            assert(r == 0);
        }
    }
    self->stack_begin = NULL;
}

void setupGenerator() {
    generator_cls
        = BoxedHeapClass::create(type_cls, object_cls, &generatorGCHandler, 0, offsetof(BoxedGenerator, weakreflist),
                                 sizeof(BoxedGenerator), false, "generator");
    generator_cls->simple_destructor = generatorDestructor;
    generator_cls->giveAttr("__iter__",
                            new BoxedFunction(boxRTFunction((void*)generatorIter, typeFromClass(generator_cls), 1)));

    generator_cls->giveAttr("close", new BoxedFunction(boxRTFunction((void*)generatorClose, UNKNOWN, 1)));
    generator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)generatorNext, UNKNOWN, 1)));
    generator_cls->giveAttr("send", new BoxedFunction(boxRTFunction((void*)generatorSend, UNKNOWN, 2)));
    generator_cls->giveAttr("throw", new BoxedFunction(boxRTFunction((void*)generatorThrow, UNKNOWN, 2)));

    generator_cls->giveAttr("__name__", new (pyston_getset_cls) BoxedGetsetDescriptor(generatorName, NULL, NULL));

    generator_cls->freeze();
}
}
