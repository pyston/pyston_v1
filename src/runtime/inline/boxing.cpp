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

#include "runtime/inline/boxing.h"

#include "runtime/gc_runtime.h"
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" Box* createDict() {
    return new BoxedDict();
}

extern "C" Box* createList() {
    return new BoxedList();
}

extern "C" BoxedString* boxStrConstant(const char* chars) {
    return new BoxedString(chars);
}

extern "C" Box* boxStringPtr(const std::string* s) {
    return new BoxedString(*s);
}

Box* boxString(const std::string& s) {
    return new BoxedString(s);
}

extern "C" double unboxFloat(Box* b) {
    ASSERT(b->cls == float_cls, "%s", getTypeName(b)->c_str());
    BoxedFloat* f = (BoxedFloat*)b;
    return f->d;
}

i64 unboxInt(Box* b) {
    ASSERT(b->cls == int_cls, "%s", getTypeName(b)->c_str());
    return ((BoxedInt*)b)->n;
}

Box* boxInt(int64_t n) {
    if (0 <= n && n < NUM_INTERNED_INTS) {
        return interned_ints[n];
    }
    return new BoxedInt(int_cls, n);
}

// BoxedInt::BoxedInt(int64_t n) : Box(int_cls), n(n) {}
}
