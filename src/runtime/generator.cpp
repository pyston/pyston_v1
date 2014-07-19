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
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedGenerator* g_gen; // HACK REMOVE ME!!!

static void generatorEntry(BoxedGenerator* self) {
    assert(self->cls == generator_cls);
    assert(self->function->cls == function_cls);

    try {
        // call body of the generator
        runtimeCall(self->function, ArgPassSpec(0), 0, 0, 0, 0, 0);
    } catch (Box* e) {
        // unhandled exception: propagate the exception to the caller
        self->exception = e;
    }

    // we returned from the body of the generator. next/send/throw will notify the caller
    self->entryExited = true;
    swapcontext(&self->context, &self->returnContext);
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

Box* generatorNext(Box* s) {
    return generatorSend(s, None);
}

extern "C" Box* yield(Box* obj, Box* value) {
    obj = g_gen;
    assert(obj->cls == generator_cls);
    BoxedGenerator* self = static_cast<BoxedGenerator*>(obj);
    self->returnValue = value;

    swapcontext(&self->context, &self->returnContext);

    // if the generator receives a exception from the caller we have throw it
    if (self->exception) {
        Box* exception = self->exception;
        self->exception = nullptr;
        raiseExc(exception);
    }
    return self->returnValue;
}

extern "C" BoxedGenerator* createGenerator(BoxedFunction* function) {
    assert(function);
    assert(function->cls == function_cls);
    return new BoxedGenerator(function);
}

extern "C" BoxedGenerator::BoxedGenerator(BoxedFunction* function)
    : Box(&generator_flavor, generator_cls), function(function), entryExited(false), returnValue(nullptr),
      exception(nullptr) {

    giveAttr("__name__", boxString(function->f->source->getName()));

    getcontext(&context);
    context.uc_link = 0;
    context.uc_stack.ss_sp = stack;
    context.uc_stack.ss_size = STACK_SIZE;
    makecontext(&context, (void (*)(void))generatorEntry, 1, this);

    g_gen = this;
}


void setupGenerator() {
    generator_cls = new BoxedClass(object_cls, offsetof(BoxedGenerator, attrs), sizeof(BoxedGenerator), false);
    generator_cls->giveAttr("__name__", boxStrConstant("generator"));
    generator_cls->giveAttr("__iter__",
                            new BoxedFunction(boxRTFunction((void*)generatorIter, typeFromClass(generator_cls), 1)));

    generator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)generatorNext, UNKNOWN, 1)));
    generator_cls->giveAttr("send", new BoxedFunction(boxRTFunction((void*)generatorSend, UNKNOWN, 2)));
    generator_cls->giveAttr("throw", new BoxedFunction(boxRTFunction((void*)generatorThrow, UNKNOWN, 2)));

    gc::registerStaticRootObj(generator_cls);
    generator_cls->freeze();
}
}
