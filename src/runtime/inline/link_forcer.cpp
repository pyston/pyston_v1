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

// This file is for forcing the inclusion of function declarations into the stdlib.
// This is so that the types of the functions are available to the compiler.

#include "core/types.h"
#include "gc/heap.h"
#include "runtime/float.h"
#include "runtime/gc_runtime.h"
#include "runtime/inline/boxing.h"
#include "runtime/int.h"
#include "runtime/list.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static void forceLink(void* x) {
    printf("%p\n", x);
}

extern "C" void __py_personality_v0() {
    RELEASE_ASSERT(0, "not used");
}


namespace _force {

#define FORCE(name) forceLink((void*)name)
void force() {
    FORCE(my_assert);

    FORCE(boxInt);
    FORCE(unboxInt);
    FORCE(boxFloat);
    FORCE(unboxFloat);
    FORCE(boxStringPtr);
    FORCE(boxCLFunction);
    FORCE(unboxCLFunction);
    FORCE(boxInstanceMethod);
    FORCE(boxBool);
    FORCE(unboxBool);
    FORCE(createTuple);
    FORCE(createDict);
    FORCE(createList);
    FORCE(createSlice);
    FORCE(createUserClass);
    FORCE(createClosure);

    FORCE(getattr);
    FORCE(setattr);
    FORCE(print);
    FORCE(nonzero);
    FORCE(binop);
    FORCE(compare);
    FORCE(augbinop);
    FORCE(unboxedLen);
    FORCE(getitem);
    FORCE(getclsattr);
    FORCE(getGlobal);
    FORCE(setitem);
    FORCE(delitem);
    FORCE(unaryop);
    FORCE(import);
    FORCE(importFrom);
    FORCE(repr);
    FORCE(isinstance);

    FORCE(checkUnpackingLength);
    FORCE(raiseAttributeError);
    FORCE(raiseAttributeErrorStr);
    FORCE(raiseNotIterableError);
    FORCE(assertNameDefined);
    FORCE(assertFail);

    FORCE(printFloat);
    FORCE(listAppendInternal);

    FORCE(runtimeCall);
    FORCE(callattr);

    FORCE(raise0);
    FORCE(raise1);

    FORCE(div_i64_i64);
    FORCE(mod_i64_i64);
    FORCE(pow_i64_i64);

    FORCE(div_float_float);
    FORCE(mod_float_float);
    FORCE(pow_float_float);

    FORCE(boxFloat);

    FORCE(createModule);

    FORCE(gc::sizes);

    // FORCE(listIter);
}
}
}
