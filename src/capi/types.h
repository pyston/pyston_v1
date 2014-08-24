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

#ifndef PYSTON_CAPI_TYPES_H
#define PYSTON_CAPI_TYPES_H

#include "runtime/types.h"

namespace pyston {

extern BoxedClass* capifunc_cls;
class BoxedCApiFunction : public Box {
private:
    int ml_flags;
    Box* passthrough;
    const char* name;
    PyCFunction func;

public:
    BoxedCApiFunction(int ml_flags, Box* passthrough, const char* name, PyCFunction func)
        : Box(capifunc_cls), ml_flags(ml_flags), passthrough(passthrough), name(name), func(func) {}

    static BoxedString* __repr__(BoxedCApiFunction* self) {
        assert(self->cls == capifunc_cls);
        return boxStrConstant(self->name);
    }

    static Box* __call__(BoxedCApiFunction* self, BoxedTuple* varargs, BoxedDict* kwargs) {
        assert(self->cls == capifunc_cls);
        assert(varargs->cls == tuple_cls);
        assert(kwargs->cls == dict_cls);

        threading::GLPromoteRegion _gil_lock;

        Box* rtn;
        if (self->ml_flags == METH_VARARGS) {
            assert(kwargs->d.size() == 0);
            rtn = (Box*)self->func(self->passthrough, varargs);
        } else if (self->ml_flags == (METH_VARARGS | METH_KEYWORDS)) {
            rtn = (Box*)((PyCFunctionWithKeywords)self->func)(self->passthrough, varargs, kwargs);
        } else if (self->ml_flags == METH_NOARGS) {
            assert(kwargs->d.size() == 0);
            assert(varargs->elts.size() == 0);
            rtn = (Box*)self->func(self->passthrough, NULL);
        } else if (self->ml_flags == METH_O) {
            assert(kwargs->d.size() == 0);
            assert(varargs->elts.size() == 1);
            rtn = (Box*)self->func(self->passthrough, varargs->elts[0]);
        } else {
            RELEASE_ASSERT(0, "0x%x", self->ml_flags);
        }
        assert(rtn);
        return rtn;
    }
};
}

#endif
