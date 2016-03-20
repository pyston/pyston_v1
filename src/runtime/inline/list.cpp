
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

#include "runtime/inline/list.h"

#include <cstring>

#include "runtime/list.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedListIterator::BoxedListIterator(BoxedList* l, int start) : l(l), pos(start) {
}

Box* listIterIter(Box* s) {
    return s;
}

Box* listIter(Box* s) noexcept {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);
    return new BoxedListIterator(self, 0);
}

Box* listiterHasnext(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!self->l) {
        return False;
    }

    bool ans = (self->pos < self->l->size);
    if (!ans) {
        self->l = NULL;
    }
    return boxBool(ans);
}

llvm_compat_bool listiterHasnextUnboxed(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!self->l) {
        return false;
    }

    bool ans = (self->pos < self->l->size);
    if (!ans) {
        self->l = NULL;
    }
    return ans;
}

Box* listiter_next(Box* s) noexcept {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!self->l)
        return NULL;

    if (!(self->pos >= 0 && self->pos < self->l->size)) {
        self->l = NULL;
        return NULL;
    }

    Box* rtn = self->l->elts->elts[self->pos];
    self->pos++;
    return rtn;
}

Box* listReversed(Box* s) {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);
    return new (list_reverse_iterator_cls) BoxedListIterator(self, self->size - 1);
}

Box* listreviterHasnext(Box* s) {
    assert(s->cls == list_reverse_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    return boxBool(self->pos >= 0);
}

llvm_compat_bool listreviterHasnextUnboxed(Box* s) {
    assert(s->cls == list_reverse_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    return self->pos >= 0;
}

Box* listreviter_next(Box* s) noexcept {
    assert(s->cls == list_reverse_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!(self->pos >= 0 && self->pos < self->l->size)) {
        return NULL;
    }

    Box* rtn = self->l->elts->elts[self->pos];
    self->pos--;
    return rtn;
}

Box* listreviterNext(Box* s) {
    Box* rtn = listreviter_next(s);
    if (!rtn)
        raiseExcHelper(StopIteration, "");
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
            delete elts;
            capacity = 0;
        }
    }
}


extern "C" void listAppendArrayInternal(Box* s, Box** v, int nelts) {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);

    assert(self->size <= self->capacity);
    self->ensure(nelts);

    assert(self->size <= self->capacity);
    memcpy(&self->elts->elts[self->size], &v[0], nelts * sizeof(Box*));

    self->size += nelts;
}

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
extern "C" Box* listAppend(Box* s, Box* v) {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);

    listAppendInternal(self, v);

    return None;
}
}
