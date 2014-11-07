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

#include "codegen/compvars.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static Box* memberGet(BoxedMemberDescriptor* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == member_cls, "");

    Py_FatalError("unimplemented");
}

static Box* propertyNew(Box* cls, Box* fget, Box* fset, Box** args) {
    RELEASE_ASSERT(cls == property_cls, "");
    Box* fdel = args[0];
    Box* doc = args[1];

    return new BoxedProperty(fget, fset, fdel, doc);
}

static Box* propertyGet(Box* self, Box* obj, Box* type) {
    RELEASE_ASSERT(self->cls == property_cls, "");

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
    RELEASE_ASSERT(self->cls == property_cls, "");

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

static Box* staticmethodNew(Box* cls, Box* f) {
    RELEASE_ASSERT(cls == staticmethod_cls, "");
    return new BoxedStaticmethod(f);
}

static Box* staticmethodGet(Box* self, Box* obj, Box* type) {
    RELEASE_ASSERT(self->cls == staticmethod_cls, "");

    BoxedStaticmethod* sm = static_cast<BoxedStaticmethod*>(self);

    if (sm->sm_callable == NULL) {
        raiseExcHelper(RuntimeError, "uninitialized staticmethod object");
    }

    return sm->sm_callable;
}

static Box* classmethodNew(Box* cls, Box* f) {
    RELEASE_ASSERT(cls == classmethod_cls, "");
    return new BoxedClassmethod(f);
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

    return new BoxedInstanceMethod(type, cm->cm_callable);
}

void setupDescr() {
    member_cls->giveAttr("__name__", boxStrConstant("member"));
    member_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)memberGet, UNKNOWN, 3)));
    member_cls->freeze();

    property_cls->giveAttr("__name__", boxStrConstant("property"));
    property_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)propertyNew, UNKNOWN, 5, 4, false, false),
                                                        { None, None, None, None }));
    property_cls->giveAttr("__get__",
                           new BoxedFunction(boxRTFunction((void*)propertyGet, UNKNOWN, 3, 0, false, false)));
    property_cls->giveAttr("__set__",
                           new BoxedFunction(boxRTFunction((void*)propertySet, UNKNOWN, 3, 0, false, false)));
    property_cls->freeze();

    staticmethod_cls->giveAttr("__name__", boxStrConstant("staticmethod"));
    staticmethod_cls->giveAttr("__new__",
                               new BoxedFunction(boxRTFunction((void*)staticmethodNew, UNKNOWN, 5, 4, false, false),
                                                 { None, None, None, None }));
    staticmethod_cls->giveAttr("__get__",
                               new BoxedFunction(boxRTFunction((void*)staticmethodGet, UNKNOWN, 3, 0, false, false)));
    staticmethod_cls->freeze();


    classmethod_cls->giveAttr("__name__", boxStrConstant("classmethod"));
    classmethod_cls->giveAttr("__new__",
                              new BoxedFunction(boxRTFunction((void*)classmethodNew, UNKNOWN, 5, 4, false, false),
                                                { None, None, None, None }));
    classmethod_cls->giveAttr("__get__",
                              new BoxedFunction(boxRTFunction((void*)classmethodGet, UNKNOWN, 3, 0, false, false)));
    classmethod_cls->freeze();
}

void teardownDescr() {
}
}
