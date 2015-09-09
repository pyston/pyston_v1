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
    static int64_t get_len_of_range(int64_t lo, int64_t hi, int64_t step) {
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
        assert(step != 0LL);
        if (step > 0LL && lo < hi)
            return 1LL + (hi - 1LL - lo) / step;
        else if (step < 0 && lo > hi)
            return 1LL + (lo - 1LL - hi) / (0LL - step);
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
    int64_t stop, step;

public:
    BoxedXrangeIterator(BoxedXrange* xrange, bool reversed) : xrange(xrange) {
        int64_t start = xrange->start;
        int64_t len = xrange->len;

        stop = xrange->stop;
        step = xrange->step;

        if (reversed) {
            stop = xrange->start - step;
            start = xrange->start + (len - 1) * step;
            step = -step;
        }

        cur = start;
    }

    DEFAULT_CLASS(xrange_iterator_cls);

    static bool xrangeIteratorHasnextUnboxed(Box* s) __attribute__((visibility("default"))) {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (self->step > 0) {
            return self->cur < self->stop;
        } else {
            return self->cur > self->stop;
        }
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
        return boxInt(rtn);
    }

    static i64 xrangeIteratorNextUnboxed(Box* s) __attribute__((visibility("default"))) {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (!xrangeIteratorHasnextUnboxed(s))
            raiseExcHelper(StopIteration, "");

        i64 rtn = self->cur;
        self->cur += self->step;
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
        return new BoxedXrange(istart, istop, 1);
    } else {
        i64 istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        i64 istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        i64 istep = PyLong_AsLong(step);
        checkAndThrowCAPIException();
        RELEASE_ASSERT(istep != 0, "step can't be 0");
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

void setupXrange() {
    xrange_cls = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(BoxedXrange), false, "xrange");
    xrange_iterator_cls = BoxedHeapClass::create(type_cls, object_cls, &BoxedXrangeIterator::gcHandler, 0, 0,
                                                 sizeof(BoxedXrangeIterator), false, "rangeiterator");

    xrange_cls->giveAttr(
        "__new__",
        new BoxedFunction(boxRTFunction((void*)xrange, typeFromClass(xrange_cls), 4, false, false), { NULL, NULL }));
    xrange_cls->giveAttr("__iter__",
                         new BoxedFunction(boxRTFunction((void*)xrangeIter, typeFromClass(xrange_iterator_cls), 1)));
    xrange_cls->giveAttr(
        "__reversed__", new BoxedFunction(boxRTFunction((void*)xrangeReversed, typeFromClass(xrange_iterator_cls), 1)));

    xrange_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)xrangeGetitem, BOXED_INT, 2)));

    xrange_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)xrangeLen, BOXED_INT, 1)));

    CLFunction* hasnext = boxRTFunction((void*)BoxedXrangeIterator::xrangeIteratorHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)BoxedXrangeIterator::xrangeIteratorHasnext, BOXED_BOOL);
    xrange_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)xrangeIterIter, typeFromClass(xrange_iterator_cls), 1)));
    xrange_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));

    CLFunction* next = boxRTFunction((void*)BoxedXrangeIterator::xrangeIteratorNextUnboxed, INT, 1);
    addRTFunction(next, (void*)BoxedXrangeIterator::xrangeIteratorNext, BOXED_INT);
    xrange_iterator_cls->giveAttr("next", new BoxedFunction(next));

    // TODO this is pretty hacky, but stuff the iterator cls into xrange to make sure it gets decref'd at the end
    xrange_cls->giveAttr("__iterator_cls__", xrange_iterator_cls);

    xrange_cls->freeze();
    xrange_cls->tp_iter = xrangeIter;

    xrange_iterator_cls->freeze();
    xrange_iterator_cls->tpp_hasnext = BoxedXrangeIterator::xrangeIteratorHasnextUnboxed;
    xrange_iterator_cls->tp_iternext = BoxedXrangeIterator::xrangeIterator_next;
}
}
