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

#include "capi/types.h"
#include "runtime/objmodel.h"

namespace pyston {

Box* BoxedMethodDescriptor::__call__(BoxedMethodDescriptor* self, Box* obj, BoxedTuple* varargs, Box** _args) {
    BoxedDict* kwargs = static_cast<BoxedDict*>(_args[0]);

    assert(self->cls == method_cls);
    assert(varargs->cls == tuple_cls);
    assert(kwargs->cls == dict_cls);

    int ml_flags = self->method->ml_flags;

    int call_flags;
    if (ml_flags & METH_CLASS) {
        if (!isSubclass(obj->cls, type_cls))
            raiseExcHelper(TypeError, "descriptor '%s' requires a type but received a '%s'", self->method->ml_name,
                           getFullTypeName(obj).c_str());
        call_flags = ml_flags & (~METH_CLASS);
    } else {
        if (!isSubclass(obj->cls, self->type))
            raiseExcHelper(TypeError, "descriptor '%s' requires a '%s' object but received a '%s'",
                           self->method->ml_name, getFullNameOfClass(self->type).c_str(), getFullTypeName(obj).c_str());
        call_flags = ml_flags;
    }

    threading::GLPromoteRegion _gil_lock;

    Box* rtn;
    if (call_flags == METH_NOARGS) {
        assert(varargs->elts.size() == 0);
        assert(kwargs->d.size() == 0);
        rtn = (Box*)self->method->ml_meth(obj, NULL);
    } else if (call_flags == METH_VARARGS) {
        assert(kwargs->d.size() == 0);
        rtn = (Box*)self->method->ml_meth(obj, varargs);
    } else if (call_flags == (METH_VARARGS | METH_KEYWORDS)) {
        rtn = (Box*)((PyCFunctionWithKeywords)self->method->ml_meth)(obj, varargs, kwargs);
    } else if (call_flags == METH_O) {
        assert(kwargs->d.size() == 0);
        assert(varargs->elts.size() == 1);
        rtn = (Box*)self->method->ml_meth(obj, varargs->elts[0]);
    } else {
        RELEASE_ASSERT(0, "0x%x", call_flags);
    }

    checkAndThrowCAPIException();
    assert(rtn && "should have set + thrown an exception!");
    return rtn;
}
}
