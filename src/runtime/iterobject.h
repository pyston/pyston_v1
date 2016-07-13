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

#ifndef PYSTON_RUNTIME_ITEROBJECT_H
#define PYSTON_RUNTIME_ITEROBJECT_H

#include <climits>

#include "core/common.h"
#include "runtime/types.h"

namespace pyston {

extern BoxedClass* seqiter_cls;
extern BoxedClass* seqreviter_cls;

// Analogue of CPython's PySeqIter: wraps an object that has a __getitem__
// and uses that to iterate.
class BoxedSeqIter : public Box {
public:
    Box* b;
    int64_t idx;
    Box* next;

    // Pyston change:
    // For types that allow it, this class will do the more efficient length-based
    // iteration, storing the length here.  Otherwise len is -1.
    int64_t len;

    BoxedSeqIter(Box* b, int64_t start) : b(b), idx(start), next(NULL) {
        Py_INCREF(b);

        if (b->cls == str_cls) {
            len = static_cast<BoxedString*>(b)->size();
        } else if (b->cls == unicode_cls) {
            len = reinterpret_cast<PyUnicodeObject*>(b)->length;
        } else {
            len = -1;
        }
    }

    DEFAULT_CLASS_SIMPLE(seqiter_cls, true);

    static void dealloc(BoxedSeqIter* o) noexcept {
        PyObject_GC_UnTrack(o);
        Py_XDECREF(o->b);
        Py_XDECREF(o->next);
        o->cls->tp_free(o);
    }

    static int traverse(BoxedSeqIter* self, visitproc visit, void* arg) noexcept {
        Py_VISIT(self->b);
        Py_VISIT(self->next);
        return 0;
    }
};

extern BoxedClass* iterwrapper_cls;
// Pyston wrapper that wraps CPython-style iterators (next() which throws StopException)
// and converts it to Pyston-style (__hasnext__)
class BoxedIterWrapper : public Box {
public:
    Box* iter;
    Box* next;

    BoxedIterWrapper(Box* iter) : iter(iter), next(NULL) { Py_INCREF(iter); }

    DEFAULT_CLASS(iterwrapper_cls);

    static void dealloc(BoxedIterWrapper* o) noexcept {
        PyObject_GC_UnTrack(o);
        Py_DECREF(o->iter);
        Py_XDECREF(o->next);
        o->cls->tp_free(o);
    }

    static int traverse(BoxedIterWrapper* self, visitproc visit, void* arg) noexcept {
        Py_VISIT(self->iter);
        Py_VISIT(self->next);
        return 0;
    }
};

llvm_compat_bool calliterHasnextUnboxed(Box* b);

void setupIter();
}

#endif
