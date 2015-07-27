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

#include "codegen/compvars.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {
BoxedClass* wrapperdescr_cls, *wrapperobject_cls;
}

static Box* memberGet(BoxedMemberDescriptor* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == member_descriptor_cls, "");

    if (inst == None)
        return self;

    if (self->type == BoxedMemberDescriptor::OBJECT) {
        Box* rtn = *(Box**)(((char*)inst) + self->offset);
        if (!rtn)
            rtn = None;
        return rtn;
    }

    Py_FatalError("unimplemented");
}

static void propertyDocCopy(BoxedProperty* prop, Box* fget) {
    assert(prop);
    assert(fget);
    Box* get_doc;

    static BoxedString* doc_str = internStringImmortal("__doc__");
    try {
        get_doc = getattrInternal<ExceptionStyle::CXX>(fget, doc_str, NULL);
    } catch (ExcInfo e) {
        if (!e.matches(Exception)) {
            throw e;
        }
        get_doc = NULL;
    }

    if (get_doc) {
        if (prop->cls == property_cls) {
            prop->prop_doc = get_doc;
        } else {
            /* If this is a property subclass, put __doc__
            in dict of the subclass instance instead,
            otherwise it gets shadowed by __doc__ in the
            class's dict. */
            setattr(prop, doc_str, get_doc);
        }
        prop->getter_doc = true;
    }
}

static Box* propertyInit(Box* _self, Box* fget, Box* fset, Box** args) {
    RELEASE_ASSERT(isSubclass(_self->cls, property_cls), "");
    Box* fdel = args[0];
    Box* doc = args[1];

    BoxedProperty* self = static_cast<BoxedProperty*>(_self);
    self->prop_get = fget;
    self->prop_set = fset;
    self->prop_del = fdel;
    self->prop_doc = doc;
    self->getter_doc = false;

    /* if no docstring given and the getter has one, use that one */
    if ((doc == NULL || doc == None) && fget != NULL) {
        propertyDocCopy(self, fget);
    }

    return None;
}

static Box* propertyGet(Box* self, Box* obj, Box* type) {
    RELEASE_ASSERT(isSubclass(self->cls, property_cls), "");

    BoxedProperty* prop = static_cast<BoxedProperty*>(self);
    if (obj == NULL || obj == None) {
        return self;
    }

    if (prop->prop_get == NULL) {
        raiseExcHelper(AttributeError, "unreadable attribute");
    }

    return runtimeCall(prop->prop_get, ArgPassSpec(1), obj, NULL, NULL, NULL, NULL);
}

static Box* propertySet(Box* self, Box* obj, Box* val) {
    RELEASE_ASSERT(isSubclass(self->cls, property_cls), "");

    BoxedProperty* prop = static_cast<BoxedProperty*>(self);
    Box* func;
    if (val == NULL) {
        func = prop->prop_del;
    } else {
        func = prop->prop_set;
    }

    if (func == NULL) {
        raiseExcHelper(AttributeError, val == NULL ? "can't delete attribute" : "can't set attribute");
    }

    if (val == NULL) {
        runtimeCall(func, ArgPassSpec(1), obj, NULL, NULL, NULL, NULL);
    } else {
        runtimeCall(func, ArgPassSpec(2), obj, val, NULL, NULL, NULL);
    }
    return None;
}

static Box* propertyDel(Box* self, Box* obj) {
    return propertySet(self, obj, NULL);
}

static Box* property_copy(BoxedProperty* old, Box* get, Box* set, Box* del) {
    RELEASE_ASSERT(isSubclass(old->cls, property_cls), "");

    if (!get || get == None)
        get = old->prop_get;
    if (!set || set == None)
        set = old->prop_set;
    if (!del || del == None)
        del = old->prop_del;

    // Optimization for the case when the old propery is not subclassed
    if (old->cls == property_cls) {
        BoxedProperty* prop = new BoxedProperty(get, set, del, old->prop_doc);

        prop->getter_doc = false;
        if ((old->getter_doc && get != None) || !old->prop_doc)
            propertyDocCopy(prop, get);

        return prop;
    } else {
        Box* doc;
        if ((old->getter_doc && get != None) || !old->prop_doc)
            doc = None;
        else
            doc = old->prop_doc;

        return runtimeCall(old->cls, ArgPassSpec(4), get, set, del, &doc, NULL);
    }
}

static Box* propertyGetter(Box* self, Box* obj) {
    RELEASE_ASSERT(isSubclass(self->cls, property_cls), "");
    BoxedProperty* prop = static_cast<BoxedProperty*>(self);
    return property_copy(prop, obj, NULL, NULL);
}

static Box* propertySetter(Box* self, Box* obj) {
    RELEASE_ASSERT(isSubclass(self->cls, property_cls), "");
    BoxedProperty* prop = static_cast<BoxedProperty*>(self);
    return property_copy(prop, NULL, obj, NULL);
}

static Box* propertyDeleter(Box* self, Box* obj) {
    RELEASE_ASSERT(isSubclass(self->cls, property_cls), "");
    BoxedProperty* prop = static_cast<BoxedProperty*>(self);
    return property_copy(prop, NULL, NULL, obj);
}

static Box* staticmethodInit(Box* _self, Box* f) {
    RELEASE_ASSERT(_self->cls == staticmethod_cls, "");
    BoxedStaticmethod* self = static_cast<BoxedStaticmethod*>(_self);
    self->sm_callable = f;

    return None;
}

static Box* staticmethodGet(Box* self, Box* obj, Box* type) {
    RELEASE_ASSERT(self->cls == staticmethod_cls, "");

    BoxedStaticmethod* sm = static_cast<BoxedStaticmethod*>(self);

    if (sm->sm_callable == NULL) {
        raiseExcHelper(RuntimeError, "uninitialized staticmethod object");
    }

    return sm->sm_callable;
}

extern "C" PyObject* PyClassMethod_New(PyObject* callable) noexcept {
    return new BoxedClassmethod(callable);
}

static Box* classmethodInit(Box* _self, Box* f) {
    RELEASE_ASSERT(_self->cls == classmethod_cls, "");
    BoxedClassmethod* self = static_cast<BoxedClassmethod*>(_self);
    self->cm_callable = f;

    return None;
}

static Box* classmethodGet(Box* self, Box* obj, Box* type) {
    RELEASE_ASSERT(self->cls == classmethod_cls, "");

    BoxedClassmethod* cm = static_cast<BoxedClassmethod*>(self);

    if (cm->cm_callable == NULL) {
        raiseExcHelper(RuntimeError, "uninitialized classmethod object");
    }

    if (type == NULL) {
        type = obj->cls;
    }

    return new BoxedInstanceMethod(type, cm->cm_callable, type);
}

// TODO this should be auto-generated as a slot wrapper:
Box* BoxedMethodDescriptor::__call__(BoxedMethodDescriptor* self, Box* obj, BoxedTuple* varargs, Box** _args) {
    BoxedDict* kwargs = static_cast<BoxedDict*>(_args[0]);
    return BoxedMethodDescriptor::tppCall(self, NULL, ArgPassSpec(1, 0, true, true), obj, varargs, kwargs, NULL, NULL);
}

Box* BoxedMethodDescriptor::tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                                    Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    STAT_TIMER(t0, "us_timer_boxedmethoddescriptor__call__", 10);

    assert(_self->cls == method_cls);
    BoxedMethodDescriptor* self = static_cast<BoxedMethodDescriptor*>(_self);

    int ml_flags = self->method->ml_flags;
    int call_flags = ml_flags & (~METH_CLASS);

    if (rewrite_args && !rewrite_args->func_guarded) {
        rewrite_args->obj->addAttrGuard(offsetof(BoxedMethodDescriptor, method), (intptr_t)self->method);
    }

    ParamReceiveSpec paramspec(0, 0, false, false);
    Box** defaults = NULL;
    if (call_flags == METH_NOARGS) {
        paramspec = ParamReceiveSpec(1, 0, false, false);
    } else if (call_flags == METH_VARARGS) {
        paramspec = ParamReceiveSpec(1, 0, true, false);
    } else if (call_flags == (METH_VARARGS | METH_KEYWORDS)) {
        paramspec = ParamReceiveSpec(1, 0, true, true);
    } else if (call_flags == METH_O) {
        paramspec = ParamReceiveSpec(2, 0, false, false);
    } else if ((call_flags & ~(METH_O3 | METH_D3)) == 0) {
        int num_args = 0;
        if (call_flags & METH_O)
            num_args++;
        if (call_flags & METH_O2)
            num_args += 2;

        int num_defaults = 0;
        if (call_flags & METH_D1)
            num_defaults++;
        if (call_flags & METH_D2)
            num_defaults += 2;

        paramspec = ParamReceiveSpec(1 + num_args, num_defaults, false, false);
        if (num_defaults) {
            static Box* _defaults[] = { NULL, NULL, NULL };
            assert(num_defaults <= 3);
            defaults = _defaults;
        }
    } else {
        RELEASE_ASSERT(0, "0x%x", call_flags);
    }

    Box* oarg1 = NULL;
    Box* oarg2 = NULL;
    Box* oarg3 = NULL;
    Box** oargs = NULL;

    Box* oargs_array[1];
    if (paramspec.totalReceived() >= 3) {
        assert((paramspec.totalReceived() - 3) <= sizeof(oargs_array) / sizeof(oargs_array[0]));
        oargs = oargs_array;
    }

    bool rewrite_success = false;
    rearrangeArguments(paramspec, NULL, self->method->ml_name, defaults, rewrite_args, rewrite_success, argspec, arg1,
                       arg2, arg3, args, keyword_names, oarg1, oarg2, oarg3, oargs);

    if (!rewrite_success)
        rewrite_args = NULL;

    if (ml_flags & METH_CLASS) {
        rewrite_args = NULL;
        if (!isSubclass(oarg1->cls, type_cls))
            raiseExcHelper(TypeError, "descriptor '%s' requires a type but received a '%s'", self->method->ml_name,
                           getFullTypeName(oarg1).c_str());
    } else {
        if (!isSubclass(oarg1->cls, self->type))
            raiseExcHelper(TypeError, "descriptor '%s' requires a '%s' oarg1 but received a '%s'",
                           self->method->ml_name, getFullNameOfClass(self->type).c_str(),
                           getFullTypeName(oarg1).c_str());
    }

    if (rewrite_args) {
        rewrite_args->arg1->addAttrGuard(offsetof(Box, cls), (intptr_t)oarg1->cls);
    }

    Box* rtn;
    if (call_flags == METH_NOARGS) {
        {
            UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_builtins");
            rtn = (Box*)self->method->ml_meth(oarg1, NULL);
        }
        if (rewrite_args)
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, rewrite_args->arg1,
                                               rewrite_args->rewriter->loadConst(0, Location::forArg(1)));
    } else if (call_flags == METH_VARARGS) {
        {
            UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_builtins");
            rtn = (Box*)self->method->ml_meth(oarg1, oarg2);
        }
        if (rewrite_args)
            rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, rewrite_args->arg1,
                                                                 rewrite_args->arg2);
    } else if (call_flags == (METH_VARARGS | METH_KEYWORDS)) {
        {
            UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_builtins");
            rtn = (Box*)((PyCFunctionWithKeywords)self->method->ml_meth)(oarg1, oarg2, oarg3);
        }
        if (rewrite_args)
            rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, rewrite_args->arg1,
                                                                 rewrite_args->arg2, rewrite_args->arg3);
    } else if (call_flags == METH_O) {
        {
            UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_builtins");
            rtn = (Box*)self->method->ml_meth(oarg1, oarg2);
        }
        if (rewrite_args)
            rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, rewrite_args->arg1,
                                                                 rewrite_args->arg2);
    } else if ((call_flags & ~(METH_O3 | METH_D3)) == 0) {
        {
            UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_builtins");
            rtn = ((Box * (*)(Box*, Box*, Box*, Box**))self->method->ml_meth)(oarg1, oarg2, oarg3, oargs);
        }
        if (rewrite_args) {
            if (paramspec.totalReceived() == 2)
                rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth,
                                                                     rewrite_args->arg1, rewrite_args->arg2);
            else if (paramspec.totalReceived() == 3)
                rewrite_args->out_rtn = rewrite_args->rewriter->call(
                    true, (void*)self->method->ml_meth, rewrite_args->arg1, rewrite_args->arg2, rewrite_args->arg3);
            else if (paramspec.totalReceived() > 3)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)self->method->ml_meth, rewrite_args->arg1,
                                                   rewrite_args->arg2, rewrite_args->arg3, rewrite_args->args);
            else
                abort();
        }
    } else {
        RELEASE_ASSERT(0, "0x%x", call_flags);
    }

    if (!rtn)
        throwCAPIException();

    if (rewrite_args) {
        rewrite_args->rewriter->call(false, (void*)checkAndThrowCAPIException);
        rewrite_args->out_success = true;
    }

    return rtn;
}

static Box* methodGetDoc(Box* b, void*) {
    assert(b->cls == method_cls);
    const char* s = static_cast<BoxedMethodDescriptor*>(b)->method->ml_doc;
    if (s)
        return boxString(s);
    return None;
}

Box* BoxedMethodDescriptor::__get__(BoxedMethodDescriptor* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == method_cls, "");

    // CPython handles this differently: they create the equivalent of different BoxedMethodDescriptor
    // objects but with different class objects, which define different __get__ and __call__ methods.
    if (self->method->ml_flags & METH_CLASS)
        return boxInstanceMethod(owner, self, self->type);

    if (self->method->ml_flags & METH_STATIC)
        Py_FatalError("unimplemented");
    if (self->method->ml_flags & METH_COEXIST)
        Py_FatalError("unimplemented");

    if (inst == None)
        return self;
    else
        return boxInstanceMethod(inst, self, self->type);
}

void BoxedMethodDescriptor::gcHandler(GCVisitor* v, Box* _o) {
    assert(_o->cls == method_cls);
    BoxedMethodDescriptor* o = static_cast<BoxedMethodDescriptor*>(_o);

    boxGCHandler(v, o);
    v->visit(o->type);
}

Box* BoxedWrapperDescriptor::__get__(BoxedWrapperDescriptor* self, Box* inst, Box* owner) {
    STAT_TIMER(t0, "us_timer_boxedwrapperdescriptor_get", 20);

    RELEASE_ASSERT(self->cls == wrapperdescr_cls, "");

    if (inst == None)
        return self;

    if (!isSubclass(inst->cls, self->type))
        raiseExcHelper(TypeError, "Descriptor '' for '%s' objects doesn't apply to '%s' object",
                       getFullNameOfClass(self->type).c_str(), getFullTypeName(inst).c_str());

    return new BoxedWrapperObject(self, inst);
}

Box* BoxedWrapperDescriptor::descr_get(Box* _self, Box* inst, Box* owner) noexcept {
    RELEASE_ASSERT(_self->cls == wrapperdescr_cls, "");
    BoxedWrapperDescriptor* self = static_cast<BoxedWrapperDescriptor*>(_self);

    if (inst == None)
        return self;

    if (!isSubclass(inst->cls, self->type)) {
        PyErr_Format(TypeError, "Descriptor '' for '%s' objects doesn't apply to '%s' object",
                     getFullNameOfClass(self->type).c_str(), getFullTypeName(inst).c_str());
        return NULL;
    }

    return new BoxedWrapperObject(self, inst);
}

Box* BoxedWrapperDescriptor::__call__(BoxedWrapperDescriptor* descr, PyObject* self, BoxedTuple* args, Box** _args) {
    RELEASE_ASSERT(descr->cls == wrapperdescr_cls, "");

    BoxedDict* kw = static_cast<BoxedDict*>(_args[0]);

    if (!isSubclass(self->cls, descr->type))
        raiseExcHelper(TypeError, "descriptor '' requires a '%s' object but received a '%s'",
                       getFullNameOfClass(descr->type).c_str(), getFullTypeName(self).c_str());

    auto wrapper = new BoxedWrapperObject(descr, self);
    return BoxedWrapperObject::__call__(wrapper, args, kw);
}

void BoxedWrapperDescriptor::gcHandler(GCVisitor* v, Box* _o) {
    assert(_o->cls == wrapperdescr_cls);
    BoxedWrapperDescriptor* o = static_cast<BoxedWrapperDescriptor*>(_o);

    boxGCHandler(v, o);
    v->visit(o->type);
}

static Box* wrapperdescrGetDoc(Box* b, void*) {
    assert(b->cls == wrapperdescr_cls);
    auto s = static_cast<BoxedWrapperDescriptor*>(b)->wrapper->doc;
    assert(s.size());
    return boxString(s);
}

Box* BoxedWrapperObject::__call__(BoxedWrapperObject* self, Box* args, Box* kwds) {
    STAT_TIMER(t0, "us_timer_boxedwrapperobject_call", (self->cls->is_user_defined ? 10 : 20));

    assert(self->cls == wrapperobject_cls);
    assert(args->cls == tuple_cls);
    assert(!kwds || kwds->cls == dict_cls);

    int flags = self->descr->wrapper->flags;
    wrapperfunc wrapper = self->descr->wrapper->wrapper;
    assert(self->descr->wrapper->offset > 0);

    Box* rtn;
    if (flags == PyWrapperFlag_KEYWORDS) {
        wrapperfunc_kwds wk = (wrapperfunc_kwds)wrapper;
        rtn = (*wk)(self->obj, args, self->descr->wrapped, kwds);
    } else if (flags == PyWrapperFlag_PYSTON || flags == 0) {
        rtn = (*wrapper)(self->obj, args, self->descr->wrapped);
    } else {
        RELEASE_ASSERT(0, "%d", flags);
    }

    checkAndThrowCAPIException();
    assert(rtn && "should have set + thrown an exception!");
    return rtn;
}

Box* BoxedWrapperObject::tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                                 Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    STAT_TIMER(t0, "us_timer_boxedwrapperobject_call", (_self->cls->is_user_defined ? 10 : 20));

    assert(_self->cls == wrapperobject_cls);
    BoxedWrapperObject* self = static_cast<BoxedWrapperObject*>(_self);

    int flags = self->descr->wrapper->flags;
    wrapperfunc wrapper = self->descr->wrapper->wrapper;
    assert(self->descr->wrapper->offset > 0);

    if (rewrite_args && !rewrite_args->func_guarded) {
        rewrite_args->obj->addAttrGuard(offsetof(BoxedWrapperObject, descr), (intptr_t)self->descr);
    }

    ParamReceiveSpec paramspec(0, 0, true, false);
    if (flags == PyWrapperFlag_KEYWORDS) {
        paramspec = ParamReceiveSpec(0, 0, true, true);
    } else if (flags == PyWrapperFlag_PYSTON || flags == 0) {
        paramspec = ParamReceiveSpec(0, 0, true, false);
    } else {
        RELEASE_ASSERT(0, "%d", flags);
    }

    Box* oarg1 = NULL;
    Box* oarg2 = NULL;
    Box* oarg3 = NULL;
    Box** oargs = NULL;

    bool rewrite_success = false;
    rearrangeArguments(paramspec, NULL, self->descr->wrapper->name.data(), NULL, rewrite_args, rewrite_success, argspec,
                       arg1, arg2, arg3, args, keyword_names, oarg1, oarg2, oarg3, oargs);

    assert(oarg1 && oarg1->cls == tuple_cls);
    if (!paramspec.takes_kwargs)
        assert(oarg2 == NULL);
    assert(oarg3 == NULL);
    assert(oargs == NULL);

    if (!rewrite_success)
        rewrite_args = NULL;

    Box* rtn;
    if (flags == PyWrapperFlag_KEYWORDS) {
        wrapperfunc_kwds wk = (wrapperfunc_kwds)wrapper;
        rtn = (*wk)(self->obj, oarg1, self->descr->wrapped, oarg2);

        if (rewrite_args) {
            auto rewriter = rewrite_args->rewriter;
            auto r_obj = rewrite_args->obj->getAttr(offsetof(BoxedWrapperObject, obj), Location::forArg(0));
            rewrite_args->out_rtn = rewriter->call(
                true, (void*)wk, r_obj, rewrite_args->arg1,
                rewriter->loadConst((intptr_t)self->descr->wrapped, Location::forArg(2)), rewrite_args->arg2);
            rewriter->call(false, (void*)checkAndThrowCAPIException);
            rewrite_args->out_success = true;
        }
    } else if (flags == PyWrapperFlag_PYSTON || flags == 0) {
        rtn = (*wrapper)(self->obj, oarg1, self->descr->wrapped);

        if (rewrite_args) {
            auto rewriter = rewrite_args->rewriter;
            auto r_obj = rewrite_args->obj->getAttr(offsetof(BoxedWrapperObject, obj), Location::forArg(0));
            rewrite_args->out_rtn
                = rewriter->call(true, (void*)wrapper, r_obj, rewrite_args->arg1,
                                 rewriter->loadConst((intptr_t)self->descr->wrapped, Location::forArg(2)));
            rewriter->call(false, (void*)checkAndThrowCAPIException);
            rewrite_args->out_success = true;
        }
    } else {
        RELEASE_ASSERT(0, "%d", flags);
    }

    checkAndThrowCAPIException();
    assert(rtn && "should have set + thrown an exception!");
    return rtn;
}

void BoxedWrapperObject::gcHandler(GCVisitor* v, Box* _o) {
    assert(_o->cls == wrapperobject_cls);
    BoxedWrapperObject* o = static_cast<BoxedWrapperObject*>(_o);

    boxGCHandler(v, o);
    v->visit(o->obj);
}

void setupDescr() {
    member_descriptor_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)memberGet, UNKNOWN, 3)));
    member_descriptor_cls->freeze();

    property_cls->giveAttr("__init__",
                           new BoxedFunction(boxRTFunction((void*)propertyInit, UNKNOWN, 5, 4, false, false,
                                                           ParamNames({ "", "fget", "fset", "fdel", "doc" }, "", "")),
                                             { NULL, NULL, NULL, NULL }));
    property_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)propertyGet, UNKNOWN, 3)));
    property_cls->giveAttr("__set__", new BoxedFunction(boxRTFunction((void*)propertySet, UNKNOWN, 3)));
    property_cls->giveAttr("__delete__", new BoxedFunction(boxRTFunction((void*)propertyDel, UNKNOWN, 2)));
    property_cls->giveAttr("getter", new BoxedFunction(boxRTFunction((void*)propertyGetter, UNKNOWN, 2)));
    property_cls->giveAttr("setter", new BoxedFunction(boxRTFunction((void*)propertySetter, UNKNOWN, 2)));
    property_cls->giveAttr("deleter", new BoxedFunction(boxRTFunction((void*)propertyDeleter, UNKNOWN, 2)));
    property_cls->giveAttr("fget",
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedProperty, prop_get)));
    property_cls->giveAttr("fset",
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedProperty, prop_set)));
    property_cls->giveAttr("fdel",
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedProperty, prop_del)));
    property_cls->giveAttr("__doc__",
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedProperty, prop_doc)));
    property_cls->freeze();

    staticmethod_cls->giveAttr("__init__",
                               new BoxedFunction(boxRTFunction((void*)staticmethodInit, UNKNOWN, 5, 4, false, false),
                                                 { None, None, None, None }));
    staticmethod_cls->giveAttr(
        "__get__", new BoxedFunction(boxRTFunction((void*)staticmethodGet, UNKNOWN, 3, 1, false, false), { None }));
    staticmethod_cls->freeze();


    classmethod_cls->giveAttr("__init__",
                              new BoxedFunction(boxRTFunction((void*)classmethodInit, UNKNOWN, 5, 4, false, false),
                                                { None, None, None, None }));
    classmethod_cls->giveAttr(
        "__get__", new BoxedFunction(boxRTFunction((void*)classmethodGet, UNKNOWN, 3, 1, false, false), { None }));
    classmethod_cls->freeze();

    method_cls->giveAttr("__get__",
                         new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__get__, UNKNOWN, 3)));
    CLFunction* method_call_cl = boxRTFunction((void*)BoxedMethodDescriptor::__call__, UNKNOWN, 2, 0, true, true);
    method_cls->giveAttr("__call__", new BoxedFunction(method_call_cl));
    method_cls->tpp_call = BoxedMethodDescriptor::tppCall;
    method_cls->giveAttr("__doc__", new (pyston_getset_cls) BoxedGetsetDescriptor(methodGetDoc, NULL, NULL));
    method_cls->freeze();

    wrapperdescr_cls->giveAttr("__get__",
                               new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__get__, UNKNOWN, 3)));
    wrapperdescr_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__call__,
                                                                           UNKNOWN, 2, 0, true, true)));
    wrapperdescr_cls->giveAttr("__doc__",
                               new (pyston_getset_cls) BoxedGetsetDescriptor(wrapperdescrGetDoc, NULL, NULL));
    wrapperdescr_cls->freeze();
    wrapperdescr_cls->tp_descr_get = BoxedWrapperDescriptor::descr_get;

    wrapperobject_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperObject::__call__, UNKNOWN, 1, 0, true, true)));
    wrapperobject_cls->tpp_call = BoxedWrapperObject::tppCall;
    wrapperobject_cls->freeze();
}

void teardownDescr() {
}
}
