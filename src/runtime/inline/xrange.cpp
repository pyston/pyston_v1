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

#include "core/types.h"

#include "runtime/types.h"
#include "runtime/objmodel.h"
#include "runtime/gc_runtime.h"

#include "codegen/compvars.h"

namespace pyston {

extern "C" const ObjectFlavor xrange_flavor(&boxGCHandler, NULL);

extern "C" const ObjectFlavor xrange_iterator_flavor;
BoxedClass *xrange_cls, *xrange_iterator_cls;

class BoxedXrangeIterator;
class BoxedXrange : public Box {
    private:
        const int64_t start, stop, step;

    public:
        BoxedXrange(i64 start, i64 stop, i64 step) : Box(&xrange_flavor, xrange_cls), start(start), stop(stop), step(step) {}

        friend class BoxedXrangeIterator;
};

class BoxedXrangeIterator : public Box {
    private:
        BoxedXrange * const xrange;
        int64_t cur;

    public:
        BoxedXrangeIterator(BoxedXrange *xrange) : Box(&xrange_iterator_flavor, xrange_iterator_cls), xrange(xrange), cur(xrange->start) {
        }

        static void xrangeIteratorDtor(Box *s) __attribute__((visibility("default"))) {
            assert(s->cls == xrange_iterator_cls);
            BoxedXrangeIterator *self = static_cast<BoxedXrangeIterator*>(s);
        }

        static Box* xrangeIteratorHasnext(Box *s) __attribute__((visibility("default"))) {
            assert(s->cls == xrange_iterator_cls);
            BoxedXrangeIterator *self = static_cast<BoxedXrangeIterator*>(s);
            assert(self->xrange->step > 0);
            return boxBool(self->cur < self->xrange->stop);
        }

        static bool xrangeIteratorHasnextUnboxed(Box *s) __attribute__((visibility("default"))) {
            assert(s->cls == xrange_iterator_cls);
            BoxedXrangeIterator *self = static_cast<BoxedXrangeIterator*>(s);
            assert(self->xrange->step > 0);
            return self->cur < self->xrange->stop;
        }

        static Box* xrangeIteratorNext(Box *s) __attribute__((visibility("default"))) {
            assert(s->cls == xrange_iterator_cls);
            BoxedXrangeIterator *self = static_cast<BoxedXrangeIterator*>(s);

            i64 rtn = self->cur;
            self->cur += self->xrange->step;
            return boxInt(rtn);
        }

        static i64 xrangeIteratorNextUnboxed(Box *s) __attribute__((visibility("default"))) {
            assert(s->cls == xrange_iterator_cls);
            BoxedXrangeIterator *self = static_cast<BoxedXrangeIterator*>(s);

            i64 rtn = self->cur;
            self->cur += self->xrange->step;
            return rtn;
        }

        static void xrangeIteratorGCHandler(GCVisitor *v, void* p) {
            boxGCHandler(v, p);

            BoxedXrangeIterator *it = (BoxedXrangeIterator*)p;
            v->visit(it->xrange);
        }
};
extern "C" const ObjectFlavor xrange_iterator_flavor(&BoxedXrangeIterator::xrangeIteratorGCHandler, NULL);

Box* xrange1(Box* cls, Box* stop) {
    assert(cls == xrange_cls);
    RELEASE_ASSERT(stop->cls == int_cls, "%s", getTypeName(stop)->c_str());

    i64 istop = static_cast<BoxedInt*>(stop)->n;
    return new BoxedXrange(0, istop, 1);
}

Box* xrange2(Box* cls, Box* start, Box* stop) {
    assert(cls == xrange_cls);
    RELEASE_ASSERT(start->cls == int_cls, "%s", getTypeName(start)->c_str());
    RELEASE_ASSERT(stop->cls == int_cls, "%s", getTypeName(stop)->c_str());

    i64 istart = static_cast<BoxedInt*>(start)->n;
    i64 istop = static_cast<BoxedInt*>(stop)->n;
    return new BoxedXrange(istart, istop, 1);
}

Box* xrange3(Box* cls, Box* start, Box* stop, Box** args) {
    Box* step = args[0];

    assert(cls == xrange_cls);
    RELEASE_ASSERT(start->cls == int_cls, "%s", getTypeName(start)->c_str());
    RELEASE_ASSERT(stop->cls == int_cls, "%s", getTypeName(stop)->c_str());
    RELEASE_ASSERT(step->cls == int_cls, "%s", getTypeName(step)->c_str());

    i64 istart = static_cast<BoxedInt*>(start)->n;
    i64 istop = static_cast<BoxedInt*>(stop)->n;
    i64 istep = static_cast<BoxedInt*>(step)->n;
    RELEASE_ASSERT(istep != 0, "step can't be 0");
    return new BoxedXrange(istart, istop, istep);
}

Box* xrangeIter(Box* self) {
    assert(self->cls == xrange_cls);

    Box* rtn = new BoxedXrangeIterator(static_cast<BoxedXrange*>(self));
    return rtn;
}

void setupXrange() {
    xrange_cls = new BoxedClass(false, NULL);
    xrange_cls->giveAttr("__name__", boxStrConstant("xrange"));
    xrange_iterator_cls = new BoxedClass(false, (BoxedClass::Dtor)BoxedXrangeIterator::xrangeIteratorDtor);
    xrange_iterator_cls->giveAttr("__name__", boxStrConstant("rangeiterator"));

    CLFunction *xrange_clf = boxRTFunction((void*)xrange1, NULL, 2, false);
    addRTFunction(xrange_clf, (void*)xrange2, NULL, 3, false);
    addRTFunction(xrange_clf, (void*)xrange3, NULL, 4, false);
    xrange_cls->giveAttr("__new__", new BoxedFunction(xrange_clf));
    xrange_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)xrangeIter, typeFromClass(xrange_iterator_cls), 1, false)));

    CLFunction *hasnext = boxRTFunction((void*)BoxedXrangeIterator::xrangeIteratorHasnextUnboxed, BOOL, 1, false);
    addRTFunction(hasnext, (void*)BoxedXrangeIterator::xrangeIteratorHasnext, BOXED_BOOL, 1, false);
    xrange_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));

    CLFunction *next = boxRTFunction((void*)BoxedXrangeIterator::xrangeIteratorNextUnboxed, INT, 1, false);
    addRTFunction(next, (void*)BoxedXrangeIterator::xrangeIteratorNext, BOXED_INT, 1, false);
    xrange_iterator_cls->giveAttr("next", new BoxedFunction(next));

    // TODO this is pretty hacky, but stuff the iterator cls into xrange to make sure it gets decref'd at the end
    xrange_cls->giveAttr("__iterator_cls__", xrange_iterator_cls);

    xrange_cls->freeze();
    xrange_iterator_cls->freeze();
}

}
