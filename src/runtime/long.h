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

#ifndef PYSTON_RUNTIME_LONG_H
#define PYSTON_RUNTIME_LONG_H

#include <cstddef>
#include <gmp.h>

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

void setupLong();

extern BoxedClass* long_cls;

class BoxedLong : public Box {
public:
    mpz_t n;

    BoxedLong() __attribute__((visibility("default"))) {}

    static void gchandler(GCVisitor* v, Box* b);

    DEFAULT_CLASS(long_cls);
};

extern "C" Box* createLong(llvm::StringRef s);
extern "C" BoxedLong* boxLong(int64_t n);

Box* longNeg(BoxedLong* lhs);
Box* longAbs(BoxedLong* v1);

Box* longAdd(BoxedLong* lhs, Box* rhs);
Box* longSub(BoxedLong* lhs, Box* rhs);
Box* longMul(BoxedLong* lhs, Box* rhs);
Box* longDiv(BoxedLong* lhs, Box* rhs);
Box* longPow(BoxedLong* lhs, Box* rhs, Box* mod = None);
Box* longLshift(BoxedLong* lhs, Box* rhs);
Box* longRshift(BoxedLong* lhs, Box* rhs);

Box* longHex(BoxedLong* v);
Box* longOct(BoxedLong* v);
Box* longStr(BoxedLong* v);
Box* longInt(Box* v);

bool longNonzeroUnboxed(BoxedLong* n);
}

#endif
