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

#ifndef PYSTON_RUNTIME_INLINE_LIST_H
#define PYSTON_RUNTIME_INLINE_LIST_H

#include "runtime/list.h"
#include "runtime/objmodel.h"

namespace pyston {

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
inline void BoxedList::grow(int min_free) {
    if (capacity == 0) {
        const int INITIAL_CAPACITY = 8;
        int initial = std::max(INITIAL_CAPACITY, min_free);
        elts = new (initial) GCdArray();
        capacity = initial;
    } else {
        int new_capacity = std::max(capacity * 2, size + min_free);
        elts = GCdArray::realloc(elts, new_capacity);
        capacity = new_capacity;
    }
}

inline void BoxedList::ensure(int min_free) {
    if (unlikely(size + min_free > capacity)) {
        grow(min_free);
    }
    assert(capacity >= size + min_free);
}

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
extern "C" inline void listAppendInternal(Box* s, Box* v) {
    // Lock must be held!

    assert(isSubclass(s->cls, list_cls));
    BoxedList* self = static_cast<BoxedList*>(s);

    assert(self->size <= self->capacity);
    self->ensure(1);

    assert(self->size < self->capacity);
    self->elts->elts[self->size] = v;
    self->size++;
}
}

#endif
