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
#include "runtime/rewrite_args.h"

namespace pyston {

Box* BoxedMethodDescriptor::__call__(BoxedMethodDescriptor* self, Box* obj, BoxedTuple* varargs, Box** _args) {
    STAT_TIMER(t0, "us_timer_boxedmethoddescriptor__call__", 10);
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
        RELEASE_ASSERT(varargs->size() == 0, "");
        RELEASE_ASSERT(kwargs->d.size() == 0, "");
        rtn = (Box*)self->method->ml_meth(obj, NULL);
    } else if (call_flags == METH_VARARGS) {
        RELEASE_ASSERT(kwargs->d.size() == 0, "");
        rtn = (Box*)self->method->ml_meth(obj, varargs);
    } else if (call_flags == (METH_VARARGS | METH_KEYWORDS)) {
        rtn = (Box*)((PyCFunctionWithKeywords)self->method->ml_meth)(obj, varargs, kwargs);
    } else if (call_flags == METH_O) {
        RELEASE_ASSERT(kwargs->d.size() == 0, "");
        RELEASE_ASSERT(varargs->size() == 1, "");
        rtn = (Box*)self->method->ml_meth(obj, varargs->elts[0]);
    } else {
        RELEASE_ASSERT(0, "0x%x", call_flags);
    }

    checkAndThrowCAPIException();
    assert(rtn && "should have set + thrown an exception!");
    return rtn;
}

Box* BoxedMethodDescriptor::callInternal(BoxedFunctionBase* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec,
                                         Box* arg1, Box* arg2, Box* arg3, Box** args,
                                         const std::vector<BoxedString*>* keyword_names) {
    // TODO: could also handle cases where we have starargs but no positional args,
    // and similarly for kwargs but no keywords
    if (!rewrite_args || argspec.has_kwargs || argspec.has_starargs || argspec.num_keywords > 0 || argspec.num_args > 4)
        return callFunc(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    assert(argspec.num_args >= 2);
    int passed_varargs = argspec.num_args - 2;

    assert(arg1->cls == method_cls);
    BoxedMethodDescriptor* self = static_cast<BoxedMethodDescriptor*>(arg1);
    Box* obj = arg2;
    RewriterVar* r_obj = rewrite_args->arg2;

    // We could also guard on the fields of the method object, but lets just guard on the object itself
    // for now.
    // TODO: what if it gets GC'd?
    rewrite_args->arg1->addGuard((intptr_t)self);

    int ml_flags = self->method->ml_flags;
    RELEASE_ASSERT((ml_flags & METH_CLASS) == 0, "unimplemented");
    if (!isSubclass(obj->cls, self->type))
        raiseExcHelper(TypeError, "descriptor '%s' requires a '%s' object but received a '%s'", self->method->ml_name,
                       getFullNameOfClass(self->type).c_str(), getFullTypeName(obj).c_str());
    r_obj->addAttrGuard(offsetof(Box, cls), (intptr_t)obj->cls);
    int call_flags = ml_flags;

    Box* rtn;
    RewriterVar* r_rtn;
    if (call_flags == METH_NOARGS) {
        RELEASE_ASSERT(passed_varargs == 0, "");
        rtn = (Box*)(self->method->ml_meth)(obj, NULL);
        r_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, r_obj,
                                             rewrite_args->rewriter->loadConst(0, Location::forArg(1)));
    } else if (call_flags & METH_VARARGS) {
        RELEASE_ASSERT(call_flags == METH_VARARGS || call_flags == (METH_VARARGS | METH_KEYWORDS), "");

        Box* varargs;
        RewriterVar* r_varargs;

        if (passed_varargs == 0) {
            varargs = EmptyTuple;
            r_varargs = rewrite_args->rewriter->loadConst((intptr_t)EmptyTuple, Location::forArg(1));
        } else if (passed_varargs == 1) {
            varargs = BoxedTuple::create1(arg3);
            r_varargs = rewrite_args->rewriter->call(false, (void*)BoxedTuple::create1, rewrite_args->arg3);
        } else if (passed_varargs == 2) {
            varargs = BoxedTuple::create2(arg3, args[0]);
            r_varargs = rewrite_args->rewriter->call(false, (void*)BoxedTuple::create2, rewrite_args->arg3,
                                                     rewrite_args->args->getAttr(0, Location::forArg(1)));
        } else {
            RELEASE_ASSERT(0, "");
        }

        if (call_flags & METH_KEYWORDS) {
            Box* kwargs = NULL;
            RewriterVar* r_kwargs = rewrite_args->rewriter->loadConst(0);
            rtn = (Box*)((PyCFunctionWithKeywords)self->method->ml_meth)(obj, varargs, kwargs);
            r_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, r_obj, r_varargs, r_kwargs);
        } else {
            rtn = (Box*)(self->method->ml_meth)(obj, varargs);
            r_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, r_obj, r_varargs);
        }
    } else if (call_flags == METH_O) {
        RELEASE_ASSERT(passed_varargs == 1, "");
        rtn = (Box*)(self->method->ml_meth)(obj, arg3);
        r_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, r_obj, rewrite_args->arg3);
    } else {
        RELEASE_ASSERT(0, "0x%x", call_flags);
    }

    rewrite_args->rewriter->call(true, (void*)checkAndThrowCAPIException);
    rewrite_args->out_rtn = r_rtn;
    rewrite_args->out_success = true;
    return rtn;
}
}
