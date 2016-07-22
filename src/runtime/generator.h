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

#ifndef PYSTON_RUNTIME_GENERATOR_H
#define PYSTON_RUNTIME_GENERATOR_H

#include "core/types.h"
#include "runtime/types.h"


namespace pyston {

struct Context;

extern BoxedClass* generator_cls;

void setupGenerator();
void generatorEntry(BoxedGenerator* g) noexcept;
Context* getReturnContextForGeneratorFrame(void* frame_addr);

extern "C" Box* yield(BoxedGenerator* obj, STOLEN(Box*) value, llvm::ArrayRef<Box*> live_values = {});
extern "C" Box* yield_capi(BoxedGenerator* obj, STOLEN(Box*) value, int num_live_values = 0, ...) noexcept;
extern "C" BoxedGenerator* createGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args);
}

#endif
