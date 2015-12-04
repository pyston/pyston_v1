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

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* xrange_cls, *xrange_iterator_cls;

class BoxedXrangeIterator;
class BoxedXrange : public Box {
public:
    const int64_t start, stop, step;
    int64_t len;

    // from cpython
    /* Return number of items in range (lo, hi, step).  step != 0
     * required.  The result always fits in an unsigned long.
     */
    static unsigned long get_len_of_range(int64_t lo, int64_t hi, int64_t step) {
        /* -------------------------------------------------------------
        If step > 0 and lo >= hi, or step < 0 and lo <= hi, the range is empty.
        Else for step > 0, if n values are in the range, the last one is
        lo + (n-1)*step, which must be <= hi-1.  Rearranging,
        n <= (hi - lo - 1)/step + 1, so taking the floor of the RHS gives
        the proper value.  Since lo < hi in this case, hi-lo-1 >= 0, so
        the RHS is non-negative and so truncation is the same as the
        floor.  Letting M be the largest positive long, the worst case
        for the RHS numerator is hi=M, lo=-M-1, and then
        hi-lo-1 = M-(-M-1)-1 = 2*M.  Therefore unsigned long has enough
        precision to compute the RHS exactly.  The analysis for step < 0
        is similar.
        ---------------------------------------------------------------*/
        assert(step != 0);
        if (step > 0 && lo < hi)
            return 1UL + (hi - 1UL - lo) / step;
        else if (step < 0 && lo > hi)
            return 1UL + (lo - 1UL - hi) / (0UL - step);
        else
            return 0LL;
    }

    BoxedXrange(int64_t start, int64_t stop, int64_t step) : start(start), stop(stop), step(step) {
        len = get_len_of_range(start, stop, step);
    }

    friend class BoxedXrangeIterator;

    DEFAULT_CLASS(xrange_cls);
};

class BoxedXrangeIterator : public Box {
private:
    BoxedXrange* const xrange;
    int64_t cur;
    int64_t index, len, step;

public:
    BoxedXrangeIterator(BoxedXrange* xrange, bool reversed) : xrange(xrange) {
        int64_t start = xrange->start;

        step = xrange->step;

        len = xrange->len;
        index = 0;

        if (reversed) {
            start = xrange->start + (len - 1) * step;
            step = -step;
        }

        cur = start;
    }

    DEFAULT_CLASS(xrange_iterator_cls);

    static llvm_compat_bool xrangeIteratorHasnextUnboxed(Box* s) __attribute__((visibility("default"))) {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (self->index >= self->len) {
            return false;
        }
        return true;
    }

    static Box* xrangeIteratorHasnext(Box* s) __attribute__((visibility("default"))) {
        return boxBool(xrangeIteratorHasnextUnboxed(s));
    }

    static Box* xrangeIterator_next(Box* s) noexcept {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (!xrangeIteratorHasnextUnboxed(s))
            return NULL;

        i64 rtn = self->cur;
        self->cur += self->step;
        self->index++;
        return boxInt(rtn);
    }

    static i64 xrangeIteratorNextUnboxed(Box* s) __attribute__((visibility("default"))) {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (!xrangeIteratorHasnextUnboxed(s))
            raiseExcHelper(StopIteration, "");

        i64 rtn = self->cur;
        self->cur += self->step;
        self->index++;
        return rtn;
    }

    static Box* xrangeIteratorNext(Box* s) __attribute__((visibility("default"))) {
        return boxInt(xrangeIteratorNextUnboxed(s));
    }

    static void gcHandler(GCVisitor* v, Box* b) {
        Box::gcHandler(v, b);

        BoxedXrangeIterator* it = (BoxedXrangeIterator*)b;
        v->visit(const_cast<BoxedXrange**>(&it->xrange));
    }
};

Box* xrange(Box* cls, Box* start, Box* stop, Box** args) {
    assert(cls == xrange_cls);

    Box* step = args[0];

    if (stop == NULL) {
        i64 istop = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        return new BoxedXrange(0, istop, 1);
    } else if (step == NULL) {
        i64 istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        i64 istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        i64 n = BoxedXrange::get_len_of_range(istart, istop, 1);
        if (n > (unsigned long)LONG_MAX || (long)n > PY_SSIZE_T_MAX) {
            raiseExcHelper(OverflowError, "xrange() result has too many items");
        }
        return new BoxedXrange(istart, istop, 1);
    } else {
        i64 istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        i64 istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        i64 istep = PyLong_AsLong(step);
        checkAndThrowCAPIException();
        if (istep == 0)
            raiseExcHelper(ValueError, "xrange() arg 3 must not be zero");
        unsigned long n = BoxedXrange::get_len_of_range(istart, istop, istep);
        if (n > (unsigned long)LONG_MAX || (long)n > PY_SSIZE_T_MAX) {
            raiseExcHelper(OverflowError, "xrange() result has too many items");
        }
        return new BoxedXrange(istart, istop, istep);
    }
}

Box* xrangeIterIter(Box* self) {
    assert(self->cls == xrange_iterator_cls);
    return self;
}

Box* xrangeIter(Box* self) noexcept {
    assert(self->cls == xrange_cls);

    Box* rtn = new BoxedXrangeIterator(static_cast<BoxedXrange*>(self), false);
    return rtn;
}

Box* xrangeReversed(Box* self) {
    assert(self->cls == xrange_cls);

    Box* rtn = new BoxedXrangeIterator(static_cast<BoxedXrange*>(self), true);
    return rtn;
}

Box* xrangeGetitem(Box* self, Box* slice) {
    assert(isSubclass(self->cls, xrange_cls));
    BoxedXrange* r = static_cast<BoxedXrange*>(self);
    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i < 0) {
            i = i + r->len;
        }
        if (i < 0 || i >= r->len) {
            raiseExcHelper(IndexError, "xrange object index out of range");
        }
        /* do calculation entirely using unsigned longs, to avoid
           undefined behaviour due to signed overflow. */
        return PyInt_FromLong((long)(r->start + (unsigned long)i * r->step));
    } else {
        RELEASE_ASSERT(false, "unimplemented");
    }
}

Box* xrangeLen(Box* self) {
    assert(isSubclass(self->cls, xrange_cls));
    return boxInt(static_cast<BoxedXrange*>(self)->len);
}

Box* xrangeRepr(BoxedXrange* self) {
    Box* repr;

    if (self->start == 0 && self->step == 1)
        repr = PyString_FromFormat("xrange(%ld)", self->stop);

    else if (self->step == 1)
        repr = PyString_FromFormat("xrange(%ld, %ld)", self->start, self->stop);

    else
        repr = PyString_FromFormat("xrange(%ld, %ld, %ld)", self->start, self->stop, self->step);
    return repr;
}

Box* xrangeReduce(Box* self) {
    if (!self) {
        return None;
    }
    BoxedXrange* r = static_cast<BoxedXrange*>(self);
    BoxedTuple* range = BoxedTuple::create({ boxInt(r->start), boxInt(r->stop), boxInt(r->step) });

    return BoxedTuple::create({ r->cls, range });
}

void setupXrange() {
    xrange_cls = BoxedClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(BoxedXrange), false, "xrange");
    xrange_iterator_cls = BoxedClass::create(type_cls, object_cls, &BoxedXrangeIterator::gcHandler, 0, 0,
                                             sizeof(BoxedXrangeIterator), false, "rangeiterator");

    static PySequenceMethods xrange_as_sequence;
    xrange_cls->tp_as_sequence = &xrange_as_sequence;

    xrange_cls->giveAttr("__new__", new BoxedFunction(FunctionMetadata::create((void*)xrange, typeFromClass(xrange_cls),
                                                                               4, false, false),
                                                      { NULL, NULL }));
    xrange_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                         (void*)xrangeIter, typeFromClass(xrange_iterator_cls), 1)));
    xrange_cls->giveAttr("__reversed__", new BoxedFunction(FunctionMetadata::create(
                                             (void*)xrangeReversed, typeFromClass(xrange_iterator_cls), 1)));

    xrange_cls->giveAttr("__getitem__",
                         new BoxedFunction(FunctionMetadata::create((void*)xrangeGetitem, BOXED_INT, 2)));

    xrange_cls->giveAttr("__len__", new BoxedFunction(FunctionMetadata::create((void*)xrangeLen, BOXED_INT, 1)));
    xrange_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)xrangeRepr, STR, 1)));
    xrange_cls->giveAttr("__reduce__",
                         new BoxedFunction(FunctionMetadata::create((void*)xrangeReduce, BOXED_TUPLE, 1)));

    FunctionMetadata* hasnext
        = FunctionMetadata::create((void*)BoxedXrangeIterator::xrangeIteratorHasnextUnboxed, BOOL, 1);
    hasnext->addVersion((void*)BoxedXrangeIterator::xrangeIteratorHasnext, BOXED_BOOL);
    xrange_iterator_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                                  (void*)xrangeIterIter, typeFromClass(xrange_iterator_cls), 1)));
    xrange_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));

    FunctionMetadata* next
        = FunctionMetadata::create((void*)BoxedXrangeIterator::xrangeIteratorNextUnboxed, UNBOXED_INT, 1);
    next->addVersion((void*)BoxedXrangeIterator::xrangeIteratorNext, BOXED_INT);
    xrange_iterator_cls->giveAttr("next", new BoxedFunction(next));

    // TODO this is pretty hacky, but stuff the iterator cls into xrange to make sure it gets decref'd at the end
    xrange_cls->giveAttr("__iterator_cls__", xrange_iterator_cls);

    xrange_cls->freeze();
    xrange_cls->tp_iter = xrangeIter;

    xrange_iterator_cls->freeze();
    xrange_iterator_cls->tpp_hasnext = BoxedXrangeIterator::xrangeIteratorHasnextUnboxed;
    xrange_iterator_cls->tp_iternext = BoxedXrangeIterator::xrangeIterator_next;
    xrange_iterator_cls->tp_iter = PyObject_SelfIter;
}
}
