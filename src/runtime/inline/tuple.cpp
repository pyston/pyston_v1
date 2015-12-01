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

#include <cstring>

#include "runtime/objmodel.h"
#include "runtime/tuple.h"

namespace pyston {

BoxedTupleIterator::BoxedTupleIterator(BoxedTuple* t) : t(t), pos(0) {
}

Box* tupleIterIter(Box* s) {
    return s;
}

Box* tupleIter(Box* s) noexcept {
    assert(PyTuple_Check(s));
    BoxedTuple* self = static_cast<BoxedTuple*>(s);
    return new BoxedTupleIterator(self);
}

Box* tupleiterHasnext(Box* s) {
    return boxBool(tupleiterHasnextUnboxed(s));
}

llvm_compat_bool tupleiterHasnextUnboxed(Box* s) {
    assert(s->cls == tuple_iterator_cls);
    BoxedTupleIterator* self = static_cast<BoxedTupleIterator*>(s);

    return self->pos < self->t->size();
}

Box* tupleiter_next(Box* s) noexcept {
    assert(s->cls == tuple_iterator_cls);
    BoxedTupleIterator* self = static_cast<BoxedTupleIterator*>(s);

    if (!(self->pos >= 0 && self->pos < self->t->size())) {
        return NULL;
    }

    Box* rtn = self->t->elts[self->pos];
    self->pos++;
    return rtn;
}

Box* tupleiterNext(Box* s) {
    Box* rtn = tupleiter_next(s);
    if (!rtn) {
        raiseExcHelper(StopIteration, "");
    }
    return rtn;
}
}
