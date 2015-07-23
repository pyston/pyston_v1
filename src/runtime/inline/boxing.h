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

#ifndef PYSTON_RUNTIME_INLINE_BOXING_H
#define PYSTON_RUNTIME_INLINE_BOXING_H

#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" inline Box* boxFloat(double d) __attribute__((visibility("default")));
extern "C" inline Box* boxFloat(double d) {
    return new BoxedFloat(d);
}

extern "C" inline Box* boxComplex(double r, double i) __attribute__((visibility("default")));
extern "C" inline Box* boxComplex(double r, double i) {
    return new BoxedComplex(r, i);
}

extern "C" inline bool unboxBool(Box* b) __attribute__((visibility("default")));
extern "C" inline bool unboxBool(Box* b) {
    assert(b->cls == bool_cls);

    // I think this is worse statically than looking up the class attribute
    // (since we have to load the value of True), but:
    // - the jit knows True is constant once the program starts
    // - this function will get inlined as well as boxBool
    // So in the presence of optimizations, I think this should do better:
    return b == True;
    // return static_cast<BoxedBool*>(b)->b;
}
}

#endif
