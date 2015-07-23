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

#ifndef PYSTON_RUNTIME_INT_H
#define PYSTON_RUNTIME_INT_H

#include <climits>

#include "core/common.h"
#include "runtime/types.h"

namespace pyston {

// These should probably be defined wherever we define the object-representation functions:
static_assert(sizeof(int64_t) == sizeof(long), "");
#define PYSTON_INT_MIN LONG_MIN
#define PYSTON_INT_MAX LONG_MAX

extern "C" Box* div_i64_i64(i64 lhs, i64 rhs);
extern "C" i64 mod_i64_i64(i64 lhs, i64 rhs);

extern "C" Box* add_i64_i64(i64 lhs, i64 rhs);
extern "C" Box* sub_i64_i64(i64 lhs, i64 rhs);
extern "C" Box* pow_i64_i64(i64 lhs, i64 rhs, Box* mod = None);
extern "C" Box* mul_i64_i64(i64 lhs, i64 rhs);
extern "C" i1 eq_i64_i64(i64 lhs, i64 rhs);
extern "C" i1 ne_i64_i64(i64 lhs, i64 rhs);
extern "C" i1 lt_i64_i64(i64 lhs, i64 rhs);
extern "C" i1 le_i64_i64(i64 lhs, i64 rhs);
extern "C" i1 gt_i64_i64(i64 lhs, i64 rhs);
extern "C" i1 ge_i64_i64(i64 lhs, i64 rhs);
extern "C" Box* intAdd(BoxedInt* lhs, Box* rhs);
extern "C" Box* intAnd(BoxedInt* lhs, Box* rhs);
extern "C" Box* intDiv(BoxedInt* lhs, Box* rhs);
extern "C" Box* intEq(BoxedInt* lhs, Box* rhs);
extern "C" Box* intNe(BoxedInt* lhs, Box* rhs);
extern "C" Box* intLt(BoxedInt* lhs, Box* rhs);
extern "C" Box* intLe(BoxedInt* lhs, Box* rhs);
extern "C" Box* intGt(BoxedInt* lhs, Box* rhs);
extern "C" Box* intGe(BoxedInt* lhs, Box* rhs);
extern "C" Box* intLShift(BoxedInt* lhs, Box* rhs);
extern "C" Box* intMod(BoxedInt* lhs, Box* rhs);
extern "C" Box* intMul(BoxedInt* lhs, Box* rhs);
extern "C" Box* intRShift(BoxedInt* lhs, Box* rhs);
extern "C" Box* intSub(BoxedInt* lhs, Box* rhs);
extern "C" Box* intInvert(BoxedInt* v);
extern "C" Box* intPos(BoxedInt* v);
extern "C" Box* intNeg(BoxedInt* v);
extern "C" Box* intNonzero(BoxedInt* v);
extern "C" BoxedString* intRepr(BoxedInt* v);
extern "C" Box* intHash(BoxedInt* self);
extern "C" Box* intNew1(Box* cls);
extern "C" Box* intNew2(Box* cls, Box* val);
extern "C" Box* intInit1(Box* self);
extern "C" Box* intInit2(BoxedInt* self, Box* val);
}

#endif
