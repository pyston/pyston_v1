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

#ifndef PYSTON_CAPI_TYPES_H
#define PYSTON_CAPI_TYPES_H

#include "runtime/capi.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

typedef PyObject* (*wrapperfunc)(PyObject* self, PyObject* args, void* wrapped);
typedef PyObject* (*wrapperfunc_kwds)(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds);

class BoxedCApiFunction : public Box {
public:
    PyMethodDef* method_def;
    PyObject* passthrough;
    Box* module;

public:
    BoxedCApiFunction(PyMethodDef* method_def, Box* passthrough, Box* module = NULL)
        : method_def(method_def), passthrough(passthrough), module(module) {}

    DEFAULT_CLASS(capifunc_cls);

    PyCFunction getFunction() { return method_def->ml_meth; }

    static BoxedString* __repr__(BoxedCApiFunction* self) {
        assert(self->cls == capifunc_cls);
        return boxString(self->method_def->ml_name);
    }

    static Box* __call__(BoxedCApiFunction* self, BoxedTuple* varargs, BoxedDict* kwargs) {
        STAT_TIMER(t0, "us_timer_boxedcapifunction__call__", (self->cls->is_user_defined ? 10 : 20));
        assert(self->cls == capifunc_cls);
        assert(varargs->cls == tuple_cls);
        assert(kwargs->cls == dict_cls);

        threading::GLPromoteRegion _gil_lock;

        Box* rtn;

        int flags = self->method_def->ml_flags;
        auto func = self->method_def->ml_meth;
        if (flags == METH_VARARGS) {
            assert(kwargs->d.size() == 0);
            rtn = (Box*)func(self->passthrough, varargs);
        } else if (flags == (METH_VARARGS | METH_KEYWORDS)) {
            rtn = (Box*)((PyCFunctionWithKeywords)func)(self->passthrough, varargs, kwargs);
        } else if (flags == METH_NOARGS) {
            assert(kwargs->d.size() == 0);
            assert(varargs->size() == 0);
            rtn = (Box*)func(self->passthrough, NULL);
        } else if (flags == METH_O) {
            if (kwargs->d.size() != 0) {
                raiseExcHelper(TypeError, "%s() takes no keyword arguments", self->method_def->ml_name);
            }
            if (varargs->size() != 1) {
                raiseExcHelper(TypeError, "%s() takes exactly one argument (%d given)", self->method_def->ml_name,
                               varargs->size());
            }
            rtn = (Box*)func(self->passthrough, varargs->elts[0]);
        } else if (flags == METH_OLDARGS) {
            /* the really old style */
            if (kwargs == NULL || PyDict_Size(kwargs) == 0) {
                int size = PyTuple_GET_SIZE(varargs);
                Box* arg = varargs;
                if (size == 1)
                    arg = PyTuple_GET_ITEM(varargs, 0);
                else if (size == 0)
                    arg = NULL;
                rtn = func(self->passthrough, arg);
            } else {
                raiseExcHelper(TypeError, "%.200s() takes no keyword arguments", self->method_def->ml_name);
            }
        } else {
            RELEASE_ASSERT(0, "0x%x", flags);
        }

        checkAndThrowCAPIException();
        assert(rtn && "should have set + thrown an exception!");
        return rtn;
    }

    static Box* getname(Box* b, void*) {
        RELEASE_ASSERT(b->cls == capifunc_cls, "");
        const char* s = static_cast<BoxedCApiFunction*>(b)->method_def->ml_name;
        if (s)
            return boxString(s);
        return None;
    }

    static Box* callInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                             Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names);

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == capifunc_cls);
        BoxedCApiFunction* o = static_cast<BoxedCApiFunction*>(_o);

        boxGCHandler(v, o);
        v->visit(o->passthrough);
        v->visit(o->module);
    }
};
static_assert(sizeof(BoxedCApiFunction) == sizeof(PyCFunctionObject), "");
static_assert(offsetof(BoxedCApiFunction, method_def) == offsetof(PyCFunctionObject, m_ml), "");
static_assert(offsetof(BoxedCApiFunction, passthrough) == offsetof(PyCFunctionObject, m_self), "");
static_assert(offsetof(BoxedCApiFunction, module) == offsetof(PyCFunctionObject, m_module), "");

PyObject* convert_3way_to_object(int op, int c) noexcept;
int default_3way_compare(PyObject* v, PyObject* w);

} // namespace pyston

#endif
