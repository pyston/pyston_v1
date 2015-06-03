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

#include "runtime/inline/boxing.h"

#include "llvm/ADT/SmallString.h"

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

BoxedString* _boxStrConstant(const char* chars) {
    size_t len = strlen(chars);
    return new (len) BoxedString(chars, len);
}
extern "C" BoxedString* boxStrConstant(const char* chars) {
    return _boxStrConstant(chars);
}

BoxedString* _boxStrConstantSize(const char* chars, size_t n) {
    return new (n) BoxedString(chars, n);
}

extern "C" BoxedString* boxStrConstantSize(const char* chars, size_t n) {
    return _boxStrConstantSize(chars, n);
}

extern "C" Box* boxStringPtr(const std::string* s) {
    return new (s->size()) BoxedString(s->c_str(), s->size());
}

Box* boxString(llvm::StringRef s) {
    return new (s.size()) BoxedString(s);
}

Box* boxStringTwine(const llvm::Twine& t) {
    llvm::SmallString<256> Vec;
    return boxString(t.toStringRef(Vec));
}


extern "C" double unboxFloat(Box* b) {
    ASSERT(b->cls == float_cls, "%s", getTypeName(b));
    BoxedFloat* f = (BoxedFloat*)b;
    return f->d;
}

i64 unboxInt(Box* b) {
    ASSERT(b->cls == int_cls, "%s", getTypeName(b));
    return ((BoxedInt*)b)->n;
}

Box* boxInt(int64_t n) {
    if (0 <= n && n < NUM_INTERNED_INTS) {
        return interned_ints[n];
    }
    return new BoxedInt(n);
}

// BoxedInt::BoxedInt(int64_t n) : Box(int_cls), n(n) {}
}
