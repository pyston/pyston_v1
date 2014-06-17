
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

#include <cstring>

#include "runtime/gc_runtime.h"
#include "runtime/list.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedListIterator::BoxedListIterator(BoxedList* l) : Box(&list_iterator_flavor, list_iterator_cls), l(l), pos(0) {
}


Box* listIterIter(Box* s) {
    return s;
}

Box* listIter(Box* s) {
    assert(s->cls == list_cls);
    BoxedList* self = static_cast<BoxedList*>(s);
    return new BoxedListIterator(self);
}

Box* listiterHasnext(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    return boxBool(self->pos < self->l->size);
}

i1 listiterHasnextUnboxed(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    return self->pos < self->l->size;
}

Box* listiterNext(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!(self->pos >= 0 && self->pos < self->l->size)) {
        raiseExcHelper(StopIteration, "");
    }

    Box* rtn = self->l->elts->elts[self->pos];
    self->pos++;
    return rtn;
}

const int BoxedList::INITIAL_CAPACITY = 8;
// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
void BoxedList::shrink() {
    // TODO more attention to the shrink condition to avoid frequent shrink and alloc
    if (capacity > size * 3) {
        int new_capacity = std::max(static_cast<int64_t>(INITIAL_CAPACITY), capacity / 2);
        if (size > 0) {
            elts = GCdArray::realloc(elts, new_capacity);
            capacity = new_capacity;
        } else if (size == 0) {
            rt_free(elts);
            capacity = 0;
        }
    }
}

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
void BoxedList::ensure(int space) {
    if (size + space > capacity) {
        if (capacity == 0) {
            const int INITIAL_CAPACITY = 8;
            int initial = std::max(INITIAL_CAPACITY, space);
            elts = new (initial) GCdArray();
            capacity = initial;
        } else {
            int new_capacity = std::max(capacity * 2, size + space);
            elts = GCdArray::realloc(elts, new_capacity);
            capacity = new_capacity;
        }
    }
    assert(capacity >= size + space);
}

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
extern "C" void listAppendInternal(Box* s, Box* v) {
    // Lock must be held!

    assert(s->cls == list_cls);
    BoxedList* self = static_cast<BoxedList*>(s);

    assert(self->size <= self->capacity);
    self->ensure(1);

    assert(self->size < self->capacity);
    self->elts->elts[self->size] = v;
    self->size++;
}


extern "C" void listAppendArrayInternal(Box* s, Box** v, int nelts) {
    // Lock must be held!

    assert(s->cls == list_cls);
    BoxedList* self = static_cast<BoxedList*>(s);

    assert(self->size <= self->capacity);
    self->ensure(nelts);

    assert(self->size <= self->capacity);
    memcpy(&self->elts->elts[self->size], &v[0], nelts * sizeof(Box*));

    self->size += nelts;
}

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
extern "C" Box* listAppend(Box* s, Box* v) {
    assert(s->cls == list_cls);
    BoxedList* self = static_cast<BoxedList*>(s);

    LOCK_REGION(self->lock.asWrite());

    listAppendInternal(self, v);

    return None;
}
}
