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

#include "runtime/iterobject.h"

#include <cmath>
#include <sstream>

#include "capi/types.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
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
    return incref(s);
}

static Box* seqiterHasnext_capi(Box* s) noexcept {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    if (!self->b) {
        Py_RETURN_FALSE;
    }

    if (self->len != -1) {
        if (self->idx >= self->len)
            Py_RETURN_FALSE;
        assert(!self->next);
        if (self->b->cls == str_cls)
            self->next = incref(characters[static_cast<BoxedString*>(self->b)->s()[self->idx] & UCHAR_MAX]);
        else
            self->next = PySequence_GetItem(self->b, self->idx);
        assert(self->next);
        self->idx++;
        Py_RETURN_TRUE;
    }

    Box* next = PySequence_GetItem(self->b, self->idx);
    if (!next) {
        if (PyErr_ExceptionMatches(IndexError) || PyErr_ExceptionMatches(StopIteration)) {
            PyErr_Clear();
            Py_CLEAR(self->b);
            Py_RETURN_FALSE;
        }
        return NULL;
    }

    self->idx++;
    RELEASE_ASSERT(!self->next, "");
    self->next = next;
    Py_RETURN_TRUE;
}

Box* seqiterHasnext(Box* s) {
    Box* rtn = seqiterHasnext_capi(s);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

llvm_compat_bool seqiterHasnextUnboxed(Box* s) {
    return unboxBool(autoDecref(seqiterHasnext(s)));
}

Box* seqreviterHasnext_capi(Box* s) noexcept {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    if (self->idx == -1 || !self->b)
        Py_RETURN_FALSE;

    Box* next = PySequence_GetItem(self->b, self->idx);
    if (!next) {
        if (PyErr_ExceptionMatches(IndexError) || PyErr_ExceptionMatches(StopIteration)) {
            PyErr_Clear();
            Py_CLEAR(self->b);
            Py_RETURN_FALSE;
        }
        return NULL;
    }
    self->idx--;
    RELEASE_ASSERT(!self->next, "");
    self->next = next;
    Py_RETURN_TRUE;
}

Box* seqreviterHasnext(Box* s) {
    Box* rtn = seqreviterHasnext_capi(s);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* seqiter_next(Box* s) noexcept {
    RELEASE_ASSERT(s->cls == seqiter_cls || s->cls == seqreviter_cls, "");
    BoxedSeqIter* self = static_cast<BoxedSeqIter*>(s);

    if (!self->next) {
        Box* hasnext = NULL;
        if (s->cls == seqiter_cls)
            hasnext = seqiterHasnext_capi(s);
        else if (s->cls == seqreviter_cls)
            hasnext = seqreviterHasnext_capi(s);
        else
            RELEASE_ASSERT(0, "");
        AUTO_XDECREF(hasnext);
        if (hasnext != Py_True)
            return NULL;
    }

    RELEASE_ASSERT(self->next, "");
    Box* r = self->next;
    self->next = NULL;
    return r;
}

Box* seqiterNext(Box* s) {
    Box* rtn = seqiter_next(s);
    if (!rtn)
        raiseExcHelper(StopIteration, (const char*)NULL);
    return rtn;
}

llvm_compat_bool iterwrapperHasnextUnboxed(Box* s) {
    RELEASE_ASSERT(s->cls == iterwrapper_cls, "");
    BoxedIterWrapper* self = static_cast<BoxedIterWrapper*>(s);

    Box* next = PyIter_Next(self->iter);
    RELEASE_ASSERT(!self->next, "");
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

Box* iterwrapper_next(Box* s) noexcept {
    RELEASE_ASSERT(s->cls == iterwrapper_cls, "");
    BoxedIterWrapper* self = static_cast<BoxedIterWrapper*>(s);

    if (!self->next)
        return NULL;
    Box* r = self->next;
    self->next = NULL;
    return r;
}

Box* iterwrapperNext(Box* s) {
    Box* rtn = iterwrapper_next(s);
    if (!rtn)
        raiseExcHelper(StopIteration, (const char*)NULL);
    return rtn;
}

extern "C" PyObject* PySeqIter_New(PyObject* seq) noexcept {
    try {
        return new BoxedSeqIter(seq, 0);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

llvm_compat_bool calliterHasnextUnboxed(Box* b) {
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
    seqiter_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedSeqIter), false, "iterator", false,
                                     (destructor)BoxedSeqIter::dealloc, NULL, true,
                                     (traverseproc)BoxedSeqIter::traverse, NOCLEAR);

    seqiter_cls->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)seqiterNext, UNKNOWN, 1)));
    seqiter_cls->giveAttr("__hasnext__",
                          new BoxedFunction(FunctionMetadata::create((void*)seqiterHasnext_capi, BOXED_BOOL, 1, ParamNames::empty(), CAPI)));
    seqiter_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create((void*)seqiterIter, UNKNOWN, 1)));

    seqiter_cls->freeze();
    seqiter_cls->tpp_hasnext = seqiterHasnextUnboxed;
    seqiter_cls->tp_iter = PyObject_SelfIter;
    seqiter_cls->tp_iternext = seqiter_next;

    seqreviter_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedSeqIter), false, "reversed", true,
                                        (destructor)BoxedSeqIter::dealloc, NULL, true,
                                        (traverseproc)BoxedSeqIter::traverse, NOCLEAR);

    seqreviter_cls->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)seqiterNext, UNKNOWN, 1)));
    seqreviter_cls->giveAttr("__hasnext__",
                             new BoxedFunction(FunctionMetadata::create((void*)seqreviterHasnext, BOXED_BOOL, 1)));
    seqreviter_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create((void*)seqiterIter, UNKNOWN, 1)));

    seqreviter_cls->freeze();
    seqreviter_cls->tp_iter = PyObject_SelfIter;
    seqreviter_cls->tp_iternext = seqiter_next;

    iterwrapper_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedIterWrapper), false, "iterwrapper",
                                         false, (destructor)BoxedIterWrapper::dealloc, NULL, true,
                                         (traverseproc)BoxedIterWrapper::traverse, NOCLEAR);

    iterwrapper_cls->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)iterwrapperNext, UNKNOWN, 1)));
    iterwrapper_cls->giveAttr("__hasnext__",
                              new BoxedFunction(FunctionMetadata::create((void*)iterwrapperHasnext, BOXED_BOOL, 1)));

    iterwrapper_cls->freeze();
    iterwrapper_cls->tpp_hasnext = iterwrapperHasnextUnboxed;
    iterwrapper_cls->tp_iternext = iterwrapper_next;
}
}
