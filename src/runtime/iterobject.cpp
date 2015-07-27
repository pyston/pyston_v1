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

#include "runtime/iterobject.h"

#include <cmath>
#include <sstream>

#include "capi/types.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" PyObject* calliter_next(calliterobject* it);

namespace pyston {

BoxedClass* seqiter_cls;
BoxedClass* seqreviter_cls;
BoxedClass* iterwrapper_cls;

Box* seqiterIter(Box* s) {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    return s;
}

bool seqiterHasnextUnboxed(Box* s) {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    if (!self->b) {
        return false;
    }

    Box* next = getitemInternal<ExceptionStyle::CAPI>(self->b, boxInt(self->idx), NULL);
    if (!next) {
        if (PyErr_ExceptionMatches(IndexError) || PyErr_ExceptionMatches(StopIteration)) {
            PyErr_Clear();
            self->b = NULL;
            return false;
        } else {
            throwCAPIException();
        }
    }

    self->idx++;
    self->next = next;
    return true;
}

Box* seqiterHasnext(Box* s) {
    return boxBool(seqiterHasnextUnboxed(s));
}

Box* seqreviterHasnext(Box* s) {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    if (self->idx == -1 || !self->b)
        return False;
    Box* next;
    try {
        next = getitem(self->b, boxInt(self->idx));
    } catch (ExcInfo e) {
        if (e.matches(IndexError) || e.matches(StopIteration)) {
            self->b = NULL;
            return False;
        } else
            throw e;
    }
    self->idx--;
    self->next = next;
    return True;
}

Box* seqiterNext(Box* s) {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    if (!self->next) {
        Box* hasnext = NULL;
        if (s->cls == seqiter_cls)
            hasnext = seqiterHasnext(s);
        else if (s->cls == seqreviter_cls)
            hasnext = seqreviterHasnext(s);
        else
            RELEASE_ASSERT(0, "");
        if (hasnext == False)
            raiseExcHelper(StopIteration, "");
    }

    RELEASE_ASSERT(self->next, "");
    Box* r = self->next;
    self->next = NULL;
    return r;
}

static void seqiterGCVisit(GCVisitor* v, Box* b) {
    assert(b->cls == seqiter_cls || b->cls == seqreviter_cls);
    boxGCHandler(v, b);

    BoxedSeqIter* si = static_cast<BoxedSeqIter*>(b);
    v->visit(si->b);
    if (si->next)
        v->visit(si->next);
}

static void iterwrapperGCVisit(GCVisitor* v, Box* b) {
    assert(b->cls == iterwrapper_cls);
    boxGCHandler(v, b);

    BoxedIterWrapper* iw = static_cast<BoxedIterWrapper*>(b);
    v->visit(iw->iter);
    if (iw->next)
        v->visit(iw->next);
}

bool iterwrapperHasnextUnboxed(Box* s) {
    RELEASE_ASSERT(s->cls == iterwrapper_cls, "");
    BoxedIterWrapper* self = static_cast<BoxedIterWrapper*>(s);

    Box* next = PyIter_Next(self->iter);
    self->next = next;
    if (!next) {
        if (PyErr_Occurred() && !PyErr_ExceptionMatches(PyExc_StopIteration))
            throwCAPIException();
        PyErr_Clear();
    }
    return next != NULL;
}

Box* iterwrapperHasnext(Box* s) {
    return boxBool(iterwrapperHasnextUnboxed(s));
}

Box* iterwrapperNext(Box* s) {
    RELEASE_ASSERT(s->cls == iterwrapper_cls, "");
    BoxedIterWrapper* self = static_cast<BoxedIterWrapper*>(s);

    RELEASE_ASSERT(self->next, "");
    Box* r = self->next;
    self->next = NULL;
    return r;
}

extern "C" PyObject* PySeqIter_New(PyObject* seq) noexcept {
    try {
        return new BoxedSeqIter(seq, 0);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

bool calliter_hasnext(Box* b) {
    calliterobject* it = (calliterobject*)b;
    if (!it->it_nextvalue) {
        it->it_nextvalue = calliter_next(it);
        if (PyErr_Occurred()) {
            throwCAPIException();
        }
    }
    return it->it_nextvalue != NULL;
}


void setupIter() {
    seqiter_cls
        = BoxedHeapClass::create(type_cls, object_cls, seqiterGCVisit, 0, 0, sizeof(BoxedSeqIter), false, "iterator");

    seqiter_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)seqiterNext, UNKNOWN, 1)));
    seqiter_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)seqiterHasnext, BOXED_BOOL, 1)));
    seqiter_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)seqiterIter, UNKNOWN, 1)));

    seqiter_cls->freeze();
    seqiter_cls->tpp_hasnext = seqiterHasnextUnboxed;

    seqreviter_cls
        = BoxedHeapClass::create(type_cls, object_cls, seqiterGCVisit, 0, 0, sizeof(BoxedSeqIter), false, "reversed");

    seqreviter_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)seqiterNext, UNKNOWN, 1)));
    seqreviter_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)seqreviterHasnext, BOXED_BOOL, 1)));
    seqreviter_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)seqiterIter, UNKNOWN, 1)));

    seqreviter_cls->freeze();

    iterwrapper_cls = BoxedHeapClass::create(type_cls, object_cls, iterwrapperGCVisit, 0, 0, sizeof(BoxedIterWrapper),
                                             false, "iterwrapper");

    iterwrapper_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)iterwrapperNext, UNKNOWN, 1)));
    iterwrapper_cls->giveAttr("__hasnext__",
                              new BoxedFunction(boxRTFunction((void*)iterwrapperHasnext, BOXED_BOOL, 1)));

    iterwrapper_cls->freeze();
    iterwrapper_cls->tpp_hasnext = iterwrapperHasnextUnboxed;
}
}
