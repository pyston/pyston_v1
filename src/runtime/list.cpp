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

#include "runtime/list.h"

#include <algorithm>
#include <cstring>

#include "capi/types.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/roots.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

static int list_ass_slice(PyListObject* a, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject* v);

extern "C" int PyList_Append(PyObject* op, PyObject* newitem) noexcept {
    RELEASE_ASSERT(PyList_Check(op), "");
    try {
        listAppendInternal(op, newitem);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" PyObject** PyList_Items(PyObject* op) noexcept {
    RELEASE_ASSERT(PyList_Check(op), "");

    return &static_cast<BoxedList*>(op)->elts->elts[0];
}

extern "C" PyObject* PyList_AsTuple(PyObject* v) noexcept {
    PyObject* w;
    PyObject** p, **q;
    Py_ssize_t n;
    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    auto l = static_cast<BoxedList*>(v);
    return BoxedTuple::create(l->size, &l->elts->elts[0]);
}

extern "C" Box* listRepr(BoxedList* self) {
    std::vector<char> chars;
    int status = Py_ReprEnter((PyObject*)self);

    if (status != 0) {
        if (status < 0)
            throwCAPIException();

        chars.push_back('[');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back(']');
        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }
    try {
        chars.push_back('[');
        for (int i = 0; i < self->size; i++) {

            if (i > 0) {
                chars.push_back(',');
                chars.push_back(' ');
            }
            Box* r = self->elts->elts[i]->reprICAsString();
            assert(r->cls == str_cls);
            BoxedString* s = static_cast<BoxedString*>(r);
            chars.insert(chars.end(), s->s().begin(), s->s().end());
        }
        chars.push_back(']');
    } catch (ExcInfo e) {
        Py_ReprLeave((PyObject*)self);
        throw e;
    }
    Py_ReprLeave((PyObject*)self);
    return boxString(llvm::StringRef(&chars[0], chars.size()));
}

extern "C" Box* listNonzero(BoxedList* self) {
    return boxBool(self->size != 0);
}

extern "C" Box* listPop(BoxedList* self, Box* idx) {
    if (idx == None) {
        if (self->size == 0) {
            raiseExcHelper(IndexError, "pop from empty list");
        }

        self->size--;
        Box* rtn = self->elts->elts[self->size];
        return rtn;
    }

    int64_t n = PyInt_AsSsize_t(idx);
    if (n == -1 && PyErr_Occurred())
        throwCAPIException();

    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        if (self->size == 0)
            raiseExcHelper(IndexError, "pop from empty list");
        else
            raiseExcHelper(IndexError, "pop index out of range");
    }

    Box* rtn = self->elts->elts[n];
    memmove(self->elts->elts + n, self->elts->elts + n + 1, (self->size - n - 1) * sizeof(Box*));
    self->size--;

    return rtn;
}

extern "C" Py_ssize_t PyList_Size(PyObject* self) noexcept {
    RELEASE_ASSERT(PyList_Check(self), "");
    return static_cast<BoxedList*>(self)->size;
}

extern "C" Box* listLen(BoxedList* self) {
    assert(PyList_Check(self));
    return boxInt(self->size);
}

static Py_ssize_t list_length(Box* self) noexcept {
    return static_cast<BoxedList*>(self)->size;
}

static PyObject* list_concat(PyListObject* a, PyObject* bb) noexcept {
    Py_ssize_t size;
    Py_ssize_t i;
    PyObject** src, **dest;
    PyListObject* np;
    if (!PyList_Check(bb)) {
        PyErr_Format(PyExc_TypeError, "can only concatenate list (not \"%.200s\") to list", bb->cls->tp_name);
        return NULL;
    }
#define b ((PyListObject*)bb)
    size = Py_SIZE(a) + Py_SIZE(b);
    if (size < 0)
        return PyErr_NoMemory();
    np = (PyListObject*)PyList_New(size);
    if (np == NULL) {
        return NULL;
    }
    src = a->ob_item;
    dest = np->ob_item;
    for (i = 0; i < Py_SIZE(a); i++) {
        PyObject* v = src[i];
        Py_INCREF(v);
        dest[i] = v;
    }
    src = b->ob_item;
    dest = np->ob_item + Py_SIZE(a);
    for (i = 0; i < Py_SIZE(b); i++) {
        PyObject* v = src[i];
        Py_INCREF(v);
        dest[i] = v;
    }
    return (PyObject*)np;
#undef b
}


Box* _listSlice(BoxedList* self, i64 start, i64 stop, i64 step, i64 length) {
    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= self->size);
    } else {
        assert(start < self->size);
        assert(-1 <= stop);
    }

    BoxedList* rtn = new BoxedList();
    if (length > 0) {
        rtn->ensure(length);
        copySlice(&rtn->elts->elts[0], &self->elts->elts[0], start, step, length);
        rtn->size += length;
    }
    return rtn;
}

static Box* list_slice(Box* o, Py_ssize_t ilow, Py_ssize_t ihigh) noexcept {
    BoxedList* a = static_cast<BoxedList*>(o);

    PyObject** src, **dest;
    Py_ssize_t i, len;
    if (ilow < 0)
        ilow = 0;
    else if (ilow > Py_SIZE(a))
        ilow = Py_SIZE(a);
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > Py_SIZE(a))
        ihigh = Py_SIZE(a);
    len = ihigh - ilow;

    BoxedList* np = new BoxedList();

    np->ensure(len);
    if (len) {
        src = a->elts->elts + ilow;
        dest = np->elts->elts;
        for (i = 0; i < len; i++) {
            PyObject* v = src[i];
            Py_INCREF(v);
            dest[i] = v;
        }
    }
    np->size = len;

    return (PyObject*)np;
}

static inline Box* listGetitemUnboxed(BoxedList* self, int64_t n) {
    assert(PyList_Check(self));
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }
    Box* rtn = self->elts->elts[n];
    return rtn;
}

extern "C" Box* listGetitemInt(BoxedList* self, BoxedInt* slice) {
    assert(PyInt_Check(slice));
    return listGetitemUnboxed(self, slice->n);
}

extern "C" PyObject* PyList_GetItem(PyObject* op, Py_ssize_t i) noexcept {
    RELEASE_ASSERT(PyList_Check(op), "");
    RELEASE_ASSERT(i >= 0, ""); // unlike list.__getitem__, PyList_GetItem doesn't do index wrapping
    try {
        return listGetitemUnboxed(static_cast<BoxedList*>(op), i);
    } catch (ExcInfo e) {
        abort();
    }
}

template <ExceptionStyle S> Box* listGetitemSlice(BoxedList* self, BoxedSlice* slice) {
    if (S == CAPI) {
        try {
            return listGetitemSlice<CXX>(self, slice);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    }

    assert(PyList_Check(self));
    assert(slice->cls == slice_cls);
    i64 start, stop, step, length;
    parseSlice(slice, self->size, &start, &stop, &step, &length);
    return _listSlice(self, start, stop, step, length);
}

extern "C" Box* listGetslice(BoxedList* self, Box* boxedStart, Box* boxedStop) {
    assert(PyList_Check(self));
    i64 start, stop, step;
    sliceIndex(boxedStart, &start);
    sliceIndex(boxedStop, &stop);

    boundSliceWithLength(&start, &stop, start, stop, self->size);
    return _listSlice(self, start, stop, 1, stop - start);
}

// Analoguous to CPython's, used for sq_ slots.
static PyObject* list_item(PyListObject* a, Py_ssize_t i) noexcept {
    try {
        BoxedList* self = (BoxedList*)a;
        return listGetitemUnboxed(self, i);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

template <ExceptionStyle S> Box* listGetitem(BoxedList* self, Box* slice) {
    if (S == CAPI) {
        try {
            return listGetitem<CXX>(self, slice);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    }

    assert(PyList_Check(self));
    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            throwCAPIException();
        return listGetitemUnboxed(self, i);
    } else if (slice->cls == slice_cls) {
        return listGetitemSlice<CXX>(self, static_cast<BoxedSlice*>(slice));
    } else {
        raiseExcHelper(TypeError, "list indices must be integers, not %s", getTypeName(slice));
    }
}

static void _listSetitem(BoxedList* self, int64_t n, Box* v) {
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }

    self->elts->elts[n] = v;
}

extern "C" Box* listSetitemUnboxed(BoxedList* self, int64_t n, Box* v) {
    assert(PyList_Check(self));
    _listSetitem(self, n, v);
    return None;
}

extern "C" Box* listSetitemInt(BoxedList* self, BoxedInt* slice, Box* v) {
    assert(PyInt_Check(slice));
    return listSetitemUnboxed(self, slice->n, v);
}

// Analoguous to CPython's, used for sq_ slots.
static int list_ass_item(PyListObject* a, Py_ssize_t i, PyObject* v) {
    PyObject* old_value;
    if (i < 0 || i >= Py_SIZE(a)) {
        PyErr_SetString(PyExc_IndexError, "list assignment index out of range");
        return -1;
    }
    if (v == NULL)
        return list_ass_slice(a, i, i + 1, v);
    Py_INCREF(v);
    old_value = a->ob_item[i];
    a->ob_item[i] = v;
    Py_DECREF(old_value);
    return 0;
}

extern "C" int PyList_SetItem(PyObject* op, Py_ssize_t i, PyObject* newitem) noexcept {
    PyObject* olditem;
    PyObject** p;
    if (!PyList_Check(op)) {
        Py_XDECREF(newitem);
        PyErr_BadInternalCall();
        return -1;
    }
    if (i < 0 || i >= Py_SIZE(op)) {
        Py_XDECREF(newitem);
        PyErr_SetString(PyExc_IndexError, "list assignment index out of range");
        return -1;
    }
    p = ((PyListObject*)op)->ob_item + i;
    olditem = *p;
    *p = newitem;
    Py_XDECREF(olditem);
    return 0;
}

Box* listIAdd(BoxedList* self, Box* _rhs);

// Copied from CPython's list_ass_subscript
int list_ass_ext_slice(BoxedList* self, PyObject* item, PyObject* value) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx((PySliceObject*)item, Py_SIZE(self), &start, &stop, &step, &slicelength) < 0) {
        return -1;
    }

    RELEASE_ASSERT(step != 1, "should have handled this elsewhere");

    /* Make sure s[5:2] = [..] inserts at the right place:
       before 5, not before 2. */
    if ((step < 0 && start < stop) || (step > 0 && start > stop))
        stop = start;

    if (value == NULL) {
        /* delete slice */
        PyObject** garbage;
        size_t cur;
        Py_ssize_t i;

        if (slicelength <= 0)
            return 0;

        if (step < 0) {
            stop = start + 1;
            start = stop + step * (slicelength - 1) - 1;
            step = -step;
        }

        assert((size_t)slicelength <= PY_SIZE_MAX / sizeof(PyObject*));

        garbage = (PyObject**)PyMem_MALLOC(slicelength * sizeof(PyObject*));
        if (!garbage) {
            PyErr_NoMemory();
            return -1;
        }

        /* drawing pictures might help understand these for
           loops. Basically, we memmove the parts of the
           list that are *not* part of the slice: step-1
           items for each item that is part of the slice,
           and then tail end of the list that was not
           covered by the slice */
        for (cur = start, i = 0; cur < (size_t)stop; cur += step, i++) {
            Py_ssize_t lim = step - 1;

            garbage[i] = PyList_GET_ITEM(self, cur);

            if (cur + step >= self->size) {
                lim = self->size - cur - 1;
            }

            memmove(self->elts->elts + cur - i, self->elts->elts + cur + 1, lim * sizeof(PyObject*));
        }
        cur = start + slicelength * step;
        if (cur < self->size) {
            memmove(self->elts->elts + cur - slicelength, self->elts->elts + cur,
                    (self->size - cur) * sizeof(PyObject*));
        }

        self->size -= slicelength;

        // list_resize(self, Py_SIZE(self));

        for (i = 0; i < slicelength; i++) {
            Py_DECREF(garbage[i]);
        }
        PyMem_FREE(garbage);

        return 0;
    } else {
        /* assign slice */
        PyObject* ins, *seq;
        PyObject** garbage, **seqitems, **selfitems;
        Py_ssize_t cur, i;

        /* protect against a[::-1] = a */
        if (self == value) {
            seq = list_slice(value, 0, PyList_GET_SIZE(value));
        } else {
            seq = PySequence_Fast(value, "must assign iterable "
                                         "to extended slice");
        }
        if (!seq)
            return -1;

        if (PySequence_Fast_GET_SIZE(seq) != slicelength) {
            PyErr_Format(PyExc_ValueError, "attempt to assign sequence of "
                                           "size %zd to extended slice of "
                                           "size %zd",
                         PySequence_Fast_GET_SIZE(seq), slicelength);
            Py_DECREF(seq);
            return -1;
        }

        if (!slicelength) {
            Py_DECREF(seq);
            return 0;
        }

        garbage = (PyObject**)PyMem_MALLOC(slicelength * sizeof(PyObject*));
        if (!garbage) {
            Py_DECREF(seq);
            PyErr_NoMemory();
            return -1;
        }

        selfitems = self->elts->elts;
        seqitems = PySequence_Fast_ITEMS(seq);
        for (cur = start, i = 0; i < slicelength; cur += step, i++) {
            garbage[i] = selfitems[cur];
            ins = seqitems[i];
            Py_INCREF(ins);
            selfitems[cur] = ins;
        }

        for (i = 0; i < slicelength; i++) {
            Py_DECREF(garbage[i]);
        }

        PyMem_FREE(garbage);
        Py_DECREF(seq);

        return 0;
    }
}

static inline void listSetitemSliceInt64(BoxedList* self, i64 start, i64 stop, i64 step, Box* v) {
    RELEASE_ASSERT(step == 1, "step sizes must be 1 in this code path");

    boundSliceWithLength(&start, &stop, start, stop, self->size);

    size_t v_size;
    Box** v_elts;

    RootedBox v_as_seq((Box*)nullptr);
    if (!v) {
        v_size = 0;
        v_elts = NULL;
    } else {
        if (self == v) // handle self assignment by creating a copy
            v = _listSlice(self, 0, self->size, 1, self->size);

        v_as_seq = RootedBox(PySequence_Fast(v, "can only assign an iterable"));
        if (v_as_seq == NULL)
            throwCAPIException();

        v_size = PySequence_Fast_GET_SIZE((Box*)v_as_seq);
        // If lv->size is 0, lv->elts->elts is garbage
        if (v_size)
            v_elts = PySequence_Fast_ITEMS((Box*)v_as_seq);
        else
            v_elts = NULL;
    }

    int delts = v_size - (stop - start);
    int remaining_elts = self->size - stop;
    self->ensure(delts);

    memmove(self->elts->elts + start + v_size, self->elts->elts + stop, remaining_elts * sizeof(Box*));
    for (int i = 0; i < v_size; i++) {
        Box* r = v_elts[i];
        self->elts->elts[start + i] = r;
    }

    self->size += delts;
}

extern "C" Box* listSetitemSlice(BoxedList* self, BoxedSlice* slice, Box* v) {
    assert(PyList_Check(self));
    assert(slice->cls == slice_cls);

    i64 start = 0, stop = self->size, step = 1;

    sliceIndex(slice->start, &start);
    sliceIndex(slice->stop, &stop);
    sliceIndex(slice->step, &step);

    adjustNegativeIndicesOnObject(self, &start, &stop);

    if (step != 1) {
        int r = list_ass_ext_slice(self, slice, v);
        if (r)
            throwCAPIException();
        return None;
    }

    listSetitemSliceInt64(self, start, stop, step, v);
    return None;
}

// Analoguous to CPython's, used for sq_ slots.
static int list_ass_slice(PyListObject* a, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject* v) {
    listSetitemSliceInt64((BoxedList*)a, ilow, ihigh, 1, v);
    return 0;
}

extern "C" Box* listSetslice(BoxedList* self, Box* boxedStart, Box* boxedStop, Box** args) {
    Box* value = args[0];

    i64 start = 0, stop = self->size;

    sliceIndex(boxedStart, &start);
    sliceIndex(boxedStop, &stop);

    listSetitemSliceInt64(self, start, stop, 1, value);
    return None;
}

extern "C" Box* listSetitem(BoxedList* self, Box* slice, Box* v) {
    assert(PyList_Check(self));
    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            throwCAPIException();
        listSetitemUnboxed(self, i, v);
        return None;
    } else if (slice->cls == slice_cls) {
        return listSetitemSlice(self, static_cast<BoxedSlice*>(slice), v);
    } else {
        raiseExcHelper(TypeError, "list indices must be integers, not %s", getTypeName(slice));
    }
}

extern "C" Box* listDelitemInt(BoxedList* self, BoxedInt* slice) {
    int64_t n = slice->n;
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }
    memmove(self->elts->elts + n, self->elts->elts + n + 1, (self->size - n - 1) * sizeof(Box*));
    self->size--;
    return None;
}

extern "C" Box* listDelitemSlice(BoxedList* self, BoxedSlice* slice) {
    return listSetitemSlice(self, slice, NULL);
}

extern "C" Box* listDelslice(BoxedList* self, Box* start, Box* stop) {
    Box* args = { NULL };
    return listSetslice(self, start, stop, &args);
}

extern "C" Box* listDelitem(BoxedList* self, Box* slice) {
    Box* rtn;
    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            throwCAPIException();
        rtn = listDelitemInt(self, (BoxedInt*)boxInt(i));
    } else if (slice->cls == slice_cls) {
        rtn = listDelitemSlice(self, static_cast<BoxedSlice*>(slice));
    } else {
        raiseExcHelper(TypeError, "list indices must be integers, not %s", getTypeName(slice));
    }
    self->shrink();
    return rtn;
}

extern "C" Box* listInsert(BoxedList* self, Box* idx, Box* v) {
    if (idx->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    int64_t n = static_cast<BoxedInt*>(idx)->n;
    if (n < 0)
        n = self->size + n;

    if (n >= self->size) {
        listAppendInternal(self, v);
    } else {
        if (n < 0)
            n = 0;
        assert(0 <= n && n < self->size);

        self->ensure(1);
        memmove(self->elts->elts + n + 1, self->elts->elts + n, (self->size - n) * sizeof(Box*));

        self->size++;
        self->elts->elts[n] = v;
    }

    return None;
}

extern "C" int PyList_Insert(PyObject* op, Py_ssize_t where, PyObject* newitem) noexcept {
    try {
        if (!PyList_Check(op)) {
            PyErr_BadInternalCall();
            return -1;
        }
        if (newitem == NULL) {
            PyErr_BadInternalCall();
            return -1;
        }
        listInsert((BoxedList*)op, boxInt(where), newitem);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* listMul(BoxedList* self, Box* rhs) {
    static BoxedString* index_str = internStringImmortal("__index__");

    Py_ssize_t n = PyNumber_AsSsize_t(rhs, PyExc_IndexError);
    if (n == -1 && PyErr_Occurred())
        throwCAPIException();

    int s = self->size;

    BoxedList* rtn = new BoxedList();
    rtn->ensure(n * s);
    if (s == 1) {
        for (long i = 0; i < n; i++) {
            listAppendInternal(rtn, self->elts->elts[0]);
        }
    } else {
        for (long i = 0; i < n; i++) {
            listAppendArrayInternal(rtn, &self->elts->elts[0], s);
        }
    }

    return rtn;
}

Box* listImul(BoxedList* self, Box* rhs) {
    static BoxedString* index_str = internStringImmortal("__index__");

    Py_ssize_t n = PyNumber_AsSsize_t(rhs, PyExc_IndexError);
    if (n == -1 && PyErr_Occurred())
        throwCAPIException();

    int s = self->size;

    self->ensure(n * s);
    if (n == 0) {
        listSetitemSliceInt64(self, 0, s, 1, NULL);
    } else if (n == 1) {
        return self;
    } else if (s == 1) {
        for (long i = 1; i < n; i++) {
            listAppendInternal(self, self->elts->elts[0]);
        }
    } else {
        for (long i = 1; i < n; i++) {
            listAppendArrayInternal(self, &self->elts->elts[0], s);
        }
    }

    return self;
}

Box* listIAdd(BoxedList* self, Box* _rhs) {
    if (_rhs->cls == list_cls) {
        // This branch is safe if self==rhs:
        BoxedList* rhs = static_cast<BoxedList*>(_rhs);

        int s1 = self->size;
        int s2 = rhs->size;

        if (s2 == 0)
            return self;

        self->ensure(s1 + s2);

        memcpy(self->elts->elts + s1, rhs->elts->elts, sizeof(rhs->elts->elts[0]) * s2);
        self->size = s1 + s2;
        return self;
    }

    if (_rhs->cls == tuple_cls) {
        BoxedTuple* rhs = static_cast<BoxedTuple*>(_rhs);

        int s1 = self->size;
        int s2 = rhs->ob_size;

        if (s2 == 0)
            return self;

        self->ensure(s1 + s2);

        memcpy(self->elts->elts + s1, rhs->elts, sizeof(self->elts->elts[0]) * s2);
        self->size = s1 + s2;
        return self;
    }

    RELEASE_ASSERT(_rhs != self, "unsupported");

    for (auto* b : _rhs->pyElements())
        listAppendInternal(self, b);

    return self;
}

Box* listAdd(BoxedList* self, Box* _rhs) {
    if (!PyList_Check(_rhs)) {
        return NotImplemented;
        raiseExcHelper(TypeError, "can only concatenate list (not \"%s\") to list", getTypeName(_rhs));
    }

    BoxedList* rhs = static_cast<BoxedList*>(_rhs);

    BoxedList* rtn = new BoxedList();

    int s1 = self->size;
    int s2 = rhs->size;
    rtn->ensure(s1 + s2);

    memcpy(rtn->elts->elts, self->elts->elts, sizeof(self->elts->elts[0]) * s1);
    memcpy(rtn->elts->elts + s1, rhs->elts->elts, sizeof(rhs->elts->elts[0]) * s2);
    rtn->size = s1 + s2;
    return rtn;
}

Box* listReverse(BoxedList* self) {
    assert(PyList_Check(self));
    for (int i = 0, j = self->size - 1; i < j; i++, j--) {
        Box* e = self->elts->elts[i];
        self->elts->elts[i] = self->elts->elts[j];
        self->elts->elts[j] = e;
    }

    return None;
}

extern "C" int PyList_Reverse(PyObject* v) noexcept {
    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }

    try {
        listReverse(static_cast<BoxedList*>(v));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

class PyCmpComparer {
private:
    Box* cmp;

public:
    PyCmpComparer(Box* cmp) : cmp(cmp) {}
    bool operator()(Box* lhs, Box* rhs) {
        Box* r = runtimeCallInternal<CXX, NOT_REWRITABLE>(cmp, NULL, ArgPassSpec(2), lhs, rhs, NULL, NULL, NULL);
        if (!PyInt_Check(r))
            raiseExcHelper(TypeError, "comparison function must return int, not %.200s", r->cls->tp_name);
        return static_cast<BoxedInt*>(r)->n < 0;
    }
};

void listSort(BoxedList* self, Box* cmp, Box* key, Box* reverse) {
    assert(PyList_Check(self));

    if (cmp == None)
        cmp = NULL;

    if (key == None)
        key = NULL;

    RELEASE_ASSERT(!cmp || !key, "Specifying both the 'cmp' and 'key' keywords is currently not supported");

    // TODO(kmod): maybe we should just switch to CPython's sort.  not sure how the algorithms compare,
    // but they specifically try to support cases where __lt__ or the cmp function might end up inspecting
    // the current list being sorted.
    // I also don't know if std::stable_sort is exception-safe.

    if (cmp) {
        std::stable_sort<Box**, PyCmpComparer>(self->elts->elts, self->elts->elts + self->size, PyCmpComparer(cmp));
    } else {
        int num_keys_added = 0;
        auto remove_keys = [&]() {
            for (int i = 0; i < num_keys_added; i++) {
                Box** obj_loc = &self->elts->elts[i];
                assert((*obj_loc)->cls == tuple_cls);
                *obj_loc = static_cast<BoxedTuple*>(*obj_loc)->elts[2];
            }
        };

        try {
            if (key) {
                for (int i = 0; i < self->size; i++) {
                    Box** obj_loc = &self->elts->elts[i];

                    Box* key_val = runtimeCall(key, ArgPassSpec(1), *obj_loc, NULL, NULL, NULL, NULL);
                    // Add the index as part of the new tuple so that the comparison never hits the
                    // original object.
                    // TODO we could potentially make this faster by copying the CPython approach of
                    // creating special sortwrapper objects that compare only based on the key.
                    Box* new_obj = BoxedTuple::create({ key_val, boxInt(i), *obj_loc });

                    *obj_loc = new_obj;
                    num_keys_added++;
                }
            }

            // We don't need to do a stable sort if there's a keyfunc, since we explicitly added the index
            // as part of the sort key.
            // But we might want to get rid of that approach?  CPython doesn't do that (they create special
            // wrapper objects that compare only based on the key).
            std::stable_sort<Box**, PyLt>(self->elts->elts, self->elts->elts + self->size, PyLt());
        } catch (ExcInfo e) {
            remove_keys();
            throw e;
        }

        remove_keys();
    }

    if (nonzero(reverse)) {
        listReverse(self);
    }
}

Box* listSortFunc(BoxedList* self, Box* cmp, Box* key, Box** _args) {
    Box* reverse = _args[0];
    listSort(self, cmp, key, reverse);
    return None;
}

extern "C" int PyList_Sort(PyObject* v) noexcept {
    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }

    try {
        listSort((BoxedList*)v, None, None, False);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }

    return 0;
}

extern "C" Box* PyList_GetSlice(PyObject* a, Py_ssize_t ilow, Py_ssize_t ihigh) noexcept {
    assert(PyList_Check(a));
    BoxedList* self = static_cast<BoxedList*>(a);
    // Lots of extra copies here; we can do better if we need to:
    return listGetitemSlice<CAPI>(self, new BoxedSlice(boxInt(ilow), boxInt(ihigh), boxInt(1)));
}

static inline int list_contains_shared(BoxedList* self, Box* elt) {
    assert(PyList_Check(self));

    int size = self->size;
    for (int i = 0; i < size; i++) {
        Box* e = self->elts->elts[i];

        bool identity_eq = e == elt;
        if (identity_eq)
            return true;

        int r = PyObject_RichCompareBool(e, elt, Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r)
            return true;
    }
    return false;
}

static int list_contains(PyListObject* a, PyObject* el) noexcept {
    return list_contains_shared((BoxedList*)a, el);
}

static PyObject* list_repeat(PyListObject* a, Py_ssize_t n) noexcept {
    Py_ssize_t i, j;
    Py_ssize_t size;
    PyListObject* np;
    PyObject** p, **items;
    PyObject* elem;
    if (n < 0)
        n = 0;
    if (n > 0 && Py_SIZE(a) > PY_SSIZE_T_MAX / n)
        return PyErr_NoMemory();
    size = Py_SIZE(a) * n;
    if (size == 0)
        return PyList_New(0);
    np = (PyListObject*)PyList_New(size);
    if (np == NULL)
        return NULL;

    items = np->ob_item;
    if (Py_SIZE(a) == 1) {
        elem = a->ob_item[0];
        for (i = 0; i < n; i++) {
            items[i] = elem;
            Py_INCREF(elem);
        }
        return (PyObject*)np;
    }
    p = np->ob_item;
    items = a->ob_item;
    for (i = 0; i < n; i++) {
        for (j = 0; j < Py_SIZE(a); j++) {
            *p = items[j];
            Py_INCREF(*p);
            p++;
        }
    }
    return (PyObject*)np;
}

Box* listContains(BoxedList* self, Box* elt) {
    return boxBool(list_contains_shared(self, elt));
}

Box* listCount(BoxedList* self, Box* elt) {
    int size = self->size;
    int count = 0;

    for (int i = 0; i < size; i++) {
        Box* e = self->elts->elts[i];

        int r = PyObject_RichCompareBool(e, elt, Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r)
            count++;
    }
    return boxInt(count);
}

Box* listIndex(BoxedList* self, Box* elt, Box* _start, Box** args) {
    Box* _stop = (BoxedInt*)args[0];

    int64_t start = 0;
    int64_t stop = self->size;

    if (!_PyEval_SliceIndex(_start, &start))
        throwCAPIException();
    if (!_PyEval_SliceIndex(_stop, &stop))
        throwCAPIException();

    if (start < 0) {
        start += self->size;
        if (start < 0)
            start = 0;
    }

    if (stop < 0) {
        stop += self->size;
        if (stop < 0)
            stop = 0;
    }

    for (int64_t i = start; i < stop; i++) {
        Box* e = self->elts->elts[i];

        int r = PyObject_RichCompareBool(e, elt, Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r)
            return boxInt(i);
    }

    BoxedString* tostr = static_cast<BoxedString*>(repr(elt));
    raiseExcHelper(ValueError, "%s is not in list", tostr->data());
}

Box* listRemove(BoxedList* self, Box* elt) {
    assert(PyList_Check(self));

    for (int i = 0; i < self->size; i++) {
        Box* e = self->elts->elts[i];

        int r = PyObject_RichCompareBool(e, elt, Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r) {
            memmove(self->elts->elts + i, self->elts->elts + i + 1, (self->size - i - 1) * sizeof(Box*));
            self->size--;
            return None;
        }
    }

    raiseExcHelper(ValueError, "list.remove(x): x not in list");
}

BoxedClass* list_iterator_cls = NULL;
BoxedClass* list_reverse_iterator_cls = NULL;

Box* listInit(BoxedList* self, Box* container) {
    assert(PyList_Check(self));

    if (container) {
        listIAdd(self, container);
    }

    return None;
}

extern "C" PyObject* PyList_New(Py_ssize_t size) noexcept {
    try {
        BoxedList* l = new BoxedList();
        if (size) {
            // This function is supposed to return a list of `size` NULL elements.
            // That will probably trip an assert somewhere if we try to create that (ex
            // I think the GC will expect them to be real objects so they can be relocated),
            // so put None in instead
            l->ensure(size);

            for (Py_ssize_t i = 0; i < size; i++) {
                l->elts->elts[i] = None;
            }
            l->size = size;
        }
        return l;
    } catch (ExcInfo e) {
        abort();
    }
}

Box* _listCmp(BoxedList* lhs, BoxedList* rhs, AST_TYPE::AST_TYPE op_type) {
    int lsz = lhs->size;
    int rsz = rhs->size;

    bool is_order
        = (op_type == AST_TYPE::Lt || op_type == AST_TYPE::LtE || op_type == AST_TYPE::Gt || op_type == AST_TYPE::GtE);

    if (lsz != rsz) {
        if (op_type == AST_TYPE::Eq)
            return False;
        if (op_type == AST_TYPE::NotEq)
            return True;
    }

    int n = std::min(lsz, rsz);
    for (int i = 0; i < n; i++) {
        bool identity_eq = lhs->elts->elts[i] == rhs->elts->elts[i];
        if (identity_eq)
            continue;

        int r = PyObject_RichCompareBool(lhs->elts->elts[i], rhs->elts->elts[i], Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r)
            continue;

        if (op_type == AST_TYPE::Eq) {
            return boxBool(false);
        } else if (op_type == AST_TYPE::NotEq) {
            return boxBool(true);
        } else {
            Box* r = compareInternal<NOT_REWRITABLE>(lhs->elts->elts[i], rhs->elts->elts[i], op_type, NULL);
            return r;
        }
    }

    if (op_type == AST_TYPE::Lt)
        return boxBool(lsz < rsz);
    else if (op_type == AST_TYPE::LtE)
        return boxBool(lsz <= rsz);
    else if (op_type == AST_TYPE::Gt)
        return boxBool(lsz > rsz);
    else if (op_type == AST_TYPE::GtE)
        return boxBool(lsz >= rsz);
    else if (op_type == AST_TYPE::Eq)
        return boxBool(lsz == rsz);
    else if (op_type == AST_TYPE::NotEq)
        return boxBool(lsz != rsz);

    RELEASE_ASSERT(0, "%d", op_type);
}

Box* listEq(BoxedList* self, Box* rhs) {
    if (!PyList_Check(rhs)) {
        return NotImplemented;
    }

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::Eq);
}

Box* listNe(BoxedList* self, Box* rhs) {
    if (!PyList_Check(rhs)) {
        return NotImplemented;
    }

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::NotEq);
}

Box* listLt(BoxedList* self, Box* rhs) {
    if (!PyList_Check(rhs)) {
        return NotImplemented;
    }

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::Lt);
}

Box* listLe(BoxedList* self, Box* rhs) {
    if (!PyList_Check(rhs)) {
        return NotImplemented;
    }

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::LtE);
}

Box* listGt(BoxedList* self, Box* rhs) {
    if (!PyList_Check(rhs)) {
        return NotImplemented;
    }

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::Gt);
}

Box* listGe(BoxedList* self, Box* rhs) {
    if (!PyList_Check(rhs)) {
        return NotImplemented;
    }

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::GtE);
}

extern "C" PyObject* _PyList_Extend(PyListObject* self, PyObject* b) noexcept {
    BoxedList* l = (BoxedList*)self;
    assert(PyList_Check(l));

    try {
        return listIAdd(l, b);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyList_SetSlice(PyObject* a, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject* v) noexcept {
    if (!PyList_Check(a)) {
        PyErr_BadInternalCall();
        return -1;
    }

    BoxedList* l = (BoxedList*)a;
    ASSERT(PyList_Check(l), "%s", l->cls->tp_name);

    try {
        BoxedSlice* slice = (BoxedSlice*)createSlice(boxInt(ilow), boxInt(ihigh), None);
        if (v)
            listSetitemSlice(l, slice, v);
        else
            listDelitemSlice(l, slice);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

template <ExceptionStyle S> Box* listiterNext(Box* s) noexcept(S == CAPI) {
    Box* rtn = listiter_next(s);
    if (!rtn) {
        if (S == CAPI) {
            PyErr_SetObject(StopIteration, None);
            return NULL;
        } else
            raiseExcHelper(StopIteration, (const char*)NULL);
    }
    return rtn;
}

// force instantiation:
template Box* listiterNext<CAPI>(Box*) noexcept;
template Box* listiterNext<CXX>(Box*);

void BoxedListIterator::gcHandler(GCVisitor* v, Box* b) {
    Box::gcHandler(v, b);
    BoxedListIterator* it = (BoxedListIterator*)b;
    v->visit(&it->l);
}

void BoxedList::gcHandler(GCVisitor* v, Box* b) {
    assert(PyList_Check(b));

    Box::gcHandler(v, b);

    BoxedList* l = (BoxedList*)b;
    int size = l->size;
    int capacity = l->capacity;
    assert(capacity >= size);
    if (capacity)
        v->visit(&l->elts);
    if (size)
        v->visitRange(&l->elts->elts[0], &l->elts->elts[size]);
}

void setupList() {
    static PySequenceMethods list_as_sequence;
    list_cls->tp_as_sequence = &list_as_sequence;
    static PyMappingMethods list_as_mapping;
    list_cls->tp_as_mapping = &list_as_mapping;

    list_iterator_cls = BoxedClass::create(type_cls, object_cls, &BoxedListIterator::gcHandler, 0, 0,
                                           sizeof(BoxedListIterator), false, "listiterator");
    list_reverse_iterator_cls = BoxedClass::create(type_cls, object_cls, &BoxedListIterator::gcHandler, 0, 0,
                                                   sizeof(BoxedListIterator), false, "listreverseiterator");
    list_iterator_cls->instances_are_nonzero = list_reverse_iterator_cls->instances_are_nonzero = true;

    list_cls->giveAttr("__len__", new BoxedFunction(FunctionMetadata::create((void*)listLen, BOXED_INT, 1)));

    FunctionMetadata* getitem = new FunctionMetadata(2, false, false);
    getitem->addVersion((void*)listGetitemInt, UNKNOWN, std::vector<ConcreteCompilerType*>{ LIST, BOXED_INT });
    getitem->addVersion((void*)listGetitemSlice<CXX>, LIST, std::vector<ConcreteCompilerType*>{ LIST, SLICE }, CXX);
    getitem->addVersion((void*)listGetitem<CXX>, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN }, CXX);
    getitem->addVersion((void*)listGetitemSlice<CAPI>, LIST, std::vector<ConcreteCompilerType*>{ LIST, SLICE }, CAPI);
    getitem->addVersion((void*)listGetitem<CAPI>, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN },
                        CAPI);
    list_cls->giveAttr("__getitem__", new BoxedFunction(getitem));

    list_cls->giveAttr("__getslice__", new BoxedFunction(FunctionMetadata::create((void*)listGetslice, LIST, 3)));

    FunctionMetadata* setitem = new FunctionMetadata(3, false, false);
    setitem->addVersion((void*)listSetitemInt, NONE, std::vector<ConcreteCompilerType*>{ LIST, BOXED_INT, UNKNOWN });
    setitem->addVersion((void*)listSetitemSlice, NONE, std::vector<ConcreteCompilerType*>{ LIST, SLICE, UNKNOWN });
    setitem->addVersion((void*)listSetitem, NONE, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN, UNKNOWN });
    list_cls->giveAttr("__setitem__", new BoxedFunction(setitem));

    list_cls->giveAttr("__setslice__", new BoxedFunction(FunctionMetadata::create((void*)listSetslice, NONE, 4)));

    FunctionMetadata* delitem = new FunctionMetadata(2, false, false);
    delitem->addVersion((void*)listDelitemInt, NONE, std::vector<ConcreteCompilerType*>{ LIST, BOXED_INT });
    delitem->addVersion((void*)listDelitemSlice, NONE, std::vector<ConcreteCompilerType*>{ LIST, SLICE });
    delitem->addVersion((void*)listDelitem, NONE, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN });
    list_cls->giveAttr("__delitem__", new BoxedFunction(delitem));

    list_cls->giveAttr("__delslice__", new BoxedFunction(FunctionMetadata::create((void*)listDelslice, NONE, 3)));

    list_cls->giveAttr(
        "__iter__", new BoxedFunction(FunctionMetadata::create((void*)listIter, typeFromClass(list_iterator_cls), 1)));

    list_cls->giveAttr("__reversed__", new BoxedFunction(FunctionMetadata::create(
                                           (void*)listReversed, typeFromClass(list_reverse_iterator_cls), 1)));

    list_cls->giveAttr("__eq__", new BoxedFunction(FunctionMetadata::create((void*)listEq, UNKNOWN, 2)));
    list_cls->giveAttr("__ne__", new BoxedFunction(FunctionMetadata::create((void*)listNe, UNKNOWN, 2)));
    list_cls->giveAttr("__lt__", new BoxedFunction(FunctionMetadata::create((void*)listLt, UNKNOWN, 2)));
    list_cls->giveAttr("__le__", new BoxedFunction(FunctionMetadata::create((void*)listLe, UNKNOWN, 2)));
    list_cls->giveAttr("__gt__", new BoxedFunction(FunctionMetadata::create((void*)listGt, UNKNOWN, 2)));
    list_cls->giveAttr("__ge__", new BoxedFunction(FunctionMetadata::create((void*)listGe, UNKNOWN, 2)));

    list_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)listRepr, STR, 1)));
    list_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)listNonzero, BOXED_BOOL, 1)));

    list_cls->giveAttr("pop",
                       new BoxedFunction(FunctionMetadata::create((void*)listPop, UNKNOWN, 2, false, false), { None }));

    list_cls->giveAttr("append", new BoxedFunction(FunctionMetadata::create((void*)listAppend, NONE, 2)));
    list_cls->giveAttr("extend", new BoxedFunction(FunctionMetadata::create((void*)listIAdd, UNKNOWN, 2)));

    list_cls->giveAttr("insert", new BoxedFunction(FunctionMetadata::create((void*)listInsert, NONE, 3)));
    list_cls->giveAttr("__mul__", new BoxedFunction(FunctionMetadata::create((void*)listMul, UNKNOWN, 2)));
    list_cls->giveAttr("__rmul__", new BoxedFunction(FunctionMetadata::create((void*)listMul, UNKNOWN, 2)));
    list_cls->giveAttr("__imul__", new BoxedFunction(FunctionMetadata::create((void*)listImul, UNKNOWN, 2)));

    list_cls->giveAttr("__iadd__", new BoxedFunction(FunctionMetadata::create((void*)listIAdd, UNKNOWN, 2)));
    list_cls->giveAttr("__add__", new BoxedFunction(FunctionMetadata::create((void*)listAdd, UNKNOWN, 2)));

    list_cls->giveAttr("sort",
                       new BoxedFunction(FunctionMetadata::create((void*)listSortFunc, NONE, 4, false, false,
                                                                  ParamNames({ "", "cmp", "key", "reverse" }, "", "")),
                                         { None, None, False }));
    list_cls->giveAttr("__contains__", new BoxedFunction(FunctionMetadata::create((void*)listContains, BOXED_BOOL, 2)));

    list_cls->giveAttr(
        "__init__", new BoxedFunction(FunctionMetadata::create((void*)listInit, UNKNOWN, 2, false, false), { NULL }));

    list_cls->giveAttr("count", new BoxedFunction(FunctionMetadata::create((void*)listCount, BOXED_INT, 2)));
    list_cls->giveAttr(
        "index",
        new BoxedFunction(FunctionMetadata::create((void*)listIndex, BOXED_INT, 4, false, false), { NULL, NULL }));
    list_cls->giveAttr("remove", new BoxedFunction(FunctionMetadata::create((void*)listRemove, NONE, 2)));
    list_cls->giveAttr("reverse", new BoxedFunction(FunctionMetadata::create((void*)listReverse, NONE, 1)));

    list_cls->giveAttr("__hash__", None);
    list_cls->freeze();
    list_cls->tp_iter = listIter;

    list_cls->tp_as_sequence->sq_length = list_length;
    list_cls->tp_as_sequence->sq_concat = (binaryfunc)list_concat;
    list_cls->tp_as_sequence->sq_item = (ssizeargfunc)list_item;
    list_cls->tp_as_sequence->sq_slice = list_slice;
    list_cls->tp_as_sequence->sq_ass_item = (ssizeobjargproc)list_ass_item;
    list_cls->tp_as_sequence->sq_ass_slice = (ssizessizeobjargproc)list_ass_slice;
    list_cls->tp_as_sequence->sq_contains = (objobjproc)list_contains;
    list_cls->tp_as_sequence->sq_repeat = (ssizeargfunc)list_repeat;

    FunctionMetadata* hasnext = FunctionMetadata::create((void*)listiterHasnextUnboxed, BOOL, 1);
    hasnext->addVersion((void*)listiterHasnext, BOXED_BOOL);
    list_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    list_iterator_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                                (void*)listIterIter, typeFromClass(list_iterator_cls), 1)));

    FunctionMetadata* listiter_next_func = FunctionMetadata::create((void*)listiterNext<CXX>, UNKNOWN, 1);
    listiter_next_func->addVersion((void*)listiterNext<CAPI>, UNKNOWN, CAPI);
    list_iterator_cls->giveAttr("next", new BoxedFunction(listiter_next_func));

    list_iterator_cls->freeze();
    list_iterator_cls->tpp_hasnext = listiterHasnextUnboxed;
    list_iterator_cls->tp_iternext = listiter_next;
    list_iterator_cls->tp_iter = PyObject_SelfIter;

    list_reverse_iterator_cls->giveAttr("__name__", boxString("listreverseiterator"));

    hasnext = FunctionMetadata::create((void*)listreviterHasnextUnboxed, BOOL, 1);
    hasnext->addVersion((void*)listreviterHasnext, BOXED_BOOL);
    list_reverse_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    list_reverse_iterator_cls->giveAttr(
        "__iter__",
        new BoxedFunction(FunctionMetadata::create((void*)listIterIter, typeFromClass(list_reverse_iterator_cls), 1)));
    list_reverse_iterator_cls->giveAttr(
        "next", new BoxedFunction(FunctionMetadata::create((void*)listreviterNext, UNKNOWN, 1)));

    list_reverse_iterator_cls->freeze();
    list_reverse_iterator_cls->tp_iternext = listreviter_next;
    list_reverse_iterator_cls->tp_iter = PyObject_SelfIter;
}

void teardownList() {
    // TODO do clearattrs?
    // decref(list_iterator_cls);
    // decref(list_reverse_iterator_cls);
}
}
