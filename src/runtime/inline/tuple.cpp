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
#include "runtime/objmodel.h"
#include "runtime/tuple.h"

namespace pyston {

BoxedTupleIterator::BoxedTupleIterator(BoxedTuple* t) : Box(&tuple_iterator_flavor, tuple_iterator_cls), t(t), pos(0) {
}

Box* tupleIterIter(Box* s) {
    return s;
}

Box* tupleIter(Box* s) {
    assert(s->cls == tuple_cls);
    BoxedTuple* self = static_cast<BoxedTuple*>(s);
    return new BoxedTupleIterator(self);
}

Box* tupleiterHasnext(Box* s) {
    return boxBool(tupleiterHasnextUnboxed(s));
}

i1 tupleiterHasnextUnboxed(Box* s) {
    assert(s->cls == tuple_iterator_cls);
    BoxedTupleIterator* self = static_cast<BoxedTupleIterator*>(s);

    return self->pos < self->t->elts.size();
}

Box* tupleiterNext(Box* s) {
    assert(s->cls == tuple_iterator_cls);
    BoxedTupleIterator* self = static_cast<BoxedTupleIterator*>(s);

    if (!(self->pos >= 0 && self->pos < self->t->elts.size())) {
        raiseExcHelper(StopIteration, "");
    }

    Box* rtn = self->t->elts[self->pos];
    self->pos++;
    return rtn;
}
}
