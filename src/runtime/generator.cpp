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

#include "runtime/generator.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <ucontext.h>

#include "codegen/compvars.h"
#include "codegen/llvm_interpreter.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {


static void generatorEntry(BoxedGenerator* g) {
    assert(g->cls == generator_cls);
    assert(g->function->cls == function_cls);
    threading::pushGenerator(&g->returnContext);

    try {
        // call body of the generator
        BoxedFunction* func = g->function;

        Box** args = g->args ? &g->args->elts[0] : nullptr;
        callCLFunc(func->f, nullptr, func->f->numReceivedArgs(), func->closure, g, g->arg1, g->arg2, g->arg3, args);
    } catch (Box* e) {
        // unhandled exception: propagate the exception to the caller
        g->exception = e;
    }

    // we returned from the body of the generator. next/send/throw will notify the caller
    g->entryExited = true;
    threading::popGenerator();
    swapcontext(&g->context, &g->returnContext);
}

Box* generatorIter(Box* s) {
    return s;
}

Box* generatorSend(Box* s, Box* v) {
    assert(s->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);

    // check if the generator already exited
    if (self->entryExited)
        raiseExcHelper(StopIteration, "");

    self->returnValue = v;
    swapcontext(&self->returnContext, &self->context);

    // propagate exception to the caller
    if (self->exception)
        raiseExc(self->exception);

    // throw StopIteration if the generator exited
    if (self->entryExited)
        raiseExcHelper(StopIteration, "");

    return self->returnValue;
}

Box* generatorThrow(Box* s, BoxedClass* e) {
    assert(s->cls == generator_cls);
    assert(isSubclass(e, Exception));
    BoxedGenerator* self = static_cast<BoxedGenerator*>(s);
    self->exception = exceptionNew1(e);
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
    swapcontext(&self->context, &self->returnContext);
    threading::pushGenerator(&self->returnContext);

    // if the generator receives a exception from the caller we have to throw it
    if (self->exception) {
        Box* exception = self->exception;
        self->exception = nullptr;
        raiseExc(exception);
    }
    return self->returnValue;
}


extern "C" BoxedGenerator* createGenerator(BoxedFunction* function, Box* arg1, Box* arg2, Box* arg3, Box** args) {
    assert(function);
    assert(function->cls == function_cls);
    return new BoxedGenerator(function, arg1, arg2, arg3, args);
}


extern "C" BoxedGenerator::BoxedGenerator(BoxedFunction* function, Box* arg1, Box* arg2, Box* arg3, Box** args)
    : Box(generator_cls), function(function), arg1(arg1), arg2(arg2), arg3(arg3), args(nullptr), entryExited(false),
      returnValue(nullptr), exception(nullptr) {

    giveAttr("__name__", boxString(function->f->source->getName()));

    int numArgs = function->f->num_args;
    if (numArgs > 3) {
        numArgs -= 3;
        this->args = new (numArgs) GCdArray();
        memcpy(&this->args->elts[0], args, numArgs * sizeof(Box*));
    }

    getcontext(&context);
    context.uc_link = 0;
    context.uc_stack.ss_sp = stack;
    context.uc_stack.ss_size = STACK_SIZE;
    makecontext(&context, (void (*)(void))generatorEntry, 1, this);
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
    if (num_args > 3)
        v->visitPotentialRange(reinterpret_cast<void* const*>(&g->args->elts[0]),
                               reinterpret_cast<void* const*>(&g->args->elts[num_args - 3]));
    if (g->returnValue)
        v->visit(g->returnValue);
    if (g->exception)
        v->visit(g->exception);

    v->visitPotentialRange((void**)&g->context, ((void**)&g->context) + sizeof(g->context) / sizeof(void*));
    v->visitPotentialRange((void**)&g->returnContext,
                           ((void**)&g->returnContext) + sizeof(g->returnContext) / sizeof(void*));
    v->visitPotentialRange((void**)&g->stack[0], (void**)&g->stack[BoxedGenerator::STACK_SIZE]);
}


void setupGenerator() {
    generator_cls = new BoxedClass(type_cls, object_cls, &generatorGCHandler, offsetof(BoxedGenerator, attrs),
                                   sizeof(BoxedGenerator), false);
    generator_cls->giveAttr("__name__", boxStrConstant("generator"));
    generator_cls->giveAttr("__iter__",
                            new BoxedFunction(boxRTFunction((void*)generatorIter, typeFromClass(generator_cls), 1)));

    generator_cls->giveAttr("close", new BoxedFunction(boxRTFunction((void*)generatorClose, UNKNOWN, 1)));
    generator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)generatorNext, UNKNOWN, 1)));
    generator_cls->giveAttr("send", new BoxedFunction(boxRTFunction((void*)generatorSend, UNKNOWN, 2)));
    generator_cls->giveAttr("throw", new BoxedFunction(boxRTFunction((void*)generatorThrow, UNKNOWN, 2)));

    generator_cls->freeze();
}
}
