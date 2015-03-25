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

// This file is for forcing the inclusion of function declarations into the stdlib.
// This is so that the types of the functions are available to the compiler.

#include "codegen/irgen/hooks.h"
#include "core/types.h"
#include "gc/heap.h"
#include "runtime/complex.h"
#include "runtime/float.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/int.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"

namespace pyston {

static void forceLink(void* x) {
    printf("%p\n", x);
}

namespace _force {

// Force the "FrameInfo" type to make it into the stdlib:
FrameInfo* _frame_info_forcer;

#define FORCE(name) forceLink((void*)name)
void force() {
    FORCE(softspace);
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
    FORCE(createGenerator);
    FORCE(createLong);
    FORCE(createPureImaginary);
    FORCE(createSet);
    FORCE(decodeUTF8StringPtr);

    FORCE(getattr);
    FORCE(setattr);
    FORCE(delattr);
    FORCE(nonzero);
    FORCE(binop);
    FORCE(compare);
    FORCE(augbinop);
    FORCE(unboxedLen);
    FORCE(getitem);
    FORCE(getclsattr);
    FORCE(getGlobal);
    FORCE(delGlobal);
    FORCE(setitem);
    FORCE(delitem);
    FORCE(unaryop);
    FORCE(import);
    FORCE(importFrom);
    FORCE(importStar);
    FORCE(repr);
    FORCE(str);
    FORCE(exceptionMatches);
    FORCE(yield);
    FORCE(getiterHelper);
    FORCE(hasnext);

    FORCE(unpackIntoArray);
    FORCE(raiseAttributeError);
    FORCE(raiseAttributeErrorStr);
    FORCE(raiseNotIterableError);
    FORCE(assertNameDefined);
    FORCE(assertFailDerefNameDefined);
    FORCE(assertFail);

    FORCE(printFloat);
    FORCE(listAppendInternal);
    FORCE(getSysStdout);

    FORCE(runtimeCall);
    FORCE(callattr);

    FORCE(raise0);
    FORCE(raise3);
    FORCE(deopt);

    FORCE(div_i64_i64);
    FORCE(mod_i64_i64);
    FORCE(pow_i64_i64);

    FORCE(div_float_float);
    FORCE(floordiv_float_float);
    FORCE(mod_float_float);
    FORCE(pow_float_float);

    FORCE(exec);

    FORCE(dump);

    FORCE(boxFloat);

    FORCE(createModule);

    FORCE(gc::sizes);

    FORCE(boxedLocalsSet);
    FORCE(boxedLocalsGet);
    FORCE(boxedLocalsDel);

    // FORCE(listIter);
}
}
}
