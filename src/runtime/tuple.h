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

#ifndef PYSTON_RUNTIME_TUPLE_H
#define PYSTON_RUNTIME_TUPLE_H

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

extern BoxedClass* tuple_iterator_cls;
class BoxedTupleIterator : public Box {
public:
    BoxedTuple* t;
    int pos;
    BoxedTupleIterator(BoxedTuple* t);

    DEFAULT_CLASS(tuple_iterator_cls);

    static void gcHandler(GCVisitor* v, Box* _o);
};

Box* tupleIter(Box* self) noexcept;
Box* tupleIterIter(Box* self);
Box* tupleiterHasnext(Box* self);
llvm_compat_bool tupleiterHasnextUnboxed(Box* self);
Box* tupleiter_next(Box* self) noexcept;
Box* tupleiterNext(Box* self);
}

#endif
