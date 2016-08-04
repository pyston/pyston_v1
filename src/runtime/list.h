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

#ifndef PYSTON_RUNTIME_LIST_H
#define PYSTON_RUNTIME_LIST_H

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

extern BoxedClass* list_iterator_cls;
extern BoxedClass* list_reverse_iterator_cls;
class BoxedListIterator : public Box {
public:
    BoxedList* l;
    int pos;
    BoxedListIterator(BoxedList* l, int start);

    static void dealloc(BoxedListIterator* o) noexcept {
        PyObject_GC_UnTrack(o);
        Py_XDECREF(o->l);
        o->cls->tp_free(o);
    }

    static int traverse(BoxedListIterator* self, visitproc visit, void* arg) noexcept {
        Py_VISIT(self->l);
        return 0;
    }
};

Box* listIter(Box* self) noexcept;
Box* listIterIter(Box* self);
Box* listiterHasnext(Box* self);
llvm_compat_bool listiterHasnextUnboxed(Box* self);
template <ExceptionStyle S> Box* listiterNext(Box* self) noexcept(S == CAPI);
Box* listiter_next(Box* s) noexcept;
Box* listReversed(Box* self);
Box* listreviterHasnext(Box* self);
llvm_compat_bool listreviterHasnextUnboxed(Box* self);
Box* listreviterNext(Box* self);
Box* listreviter_next(Box* s) noexcept;
extern "C" Box* listAppend(Box* self, Box* v);
}

#endif
