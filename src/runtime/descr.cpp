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
#include "runtime/types.h"

namespace pyston {

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
    try {
        get_doc = getattrInternal(fget, "__doc__", NULL);
    } catch (ExcInfo e) {
        if (!e.matches(Exception)) {
            throw;
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
            setattr(prop, "__doc__", get_doc);
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
}

void teardownDescr() {
}
}
