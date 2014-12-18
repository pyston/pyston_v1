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

#include "runtime/classobj.h"

#include <sstream>

#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* classobj_cls, *instance_cls;

bool classobjIssubclass(BoxedClassobj* child, BoxedClassobj* parent) {
    if (child == parent)
        return true;

    for (auto e : child->bases->elts) {
        if (e->cls == classobj_cls && classobjIssubclass(static_cast<BoxedClassobj*>(e), parent))
            return true;
    }
    return false;
}

bool instanceIsinstance(BoxedInstance* obj, BoxedClassobj* cls) {
    return classobjIssubclass(obj->inst_cls, cls);
}

static Box* classLookup(BoxedClassobj* cls, const std::string& attr) {
    Box* r = cls->getattr(attr);
    if (r)
        return r;

    for (auto b : cls->bases->elts) {
        RELEASE_ASSERT(b->cls == classobj_cls, "");
        Box* r = classLookup(static_cast<BoxedClassobj*>(b), attr);
        if (r)
            return r;
    }

    return NULL;
}

Box* classobjNew(Box* _cls, Box* _name, Box* _bases, Box** _args) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "classobj.__new__(X): X is not a type object (%s)", getTypeName(_cls)->c_str());

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, classobj_cls))
        raiseExcHelper(TypeError, "classobj.__new__(%s): %s is not a subtype of classobj", getNameOfClass(cls)->c_str(),
                       getNameOfClass(cls)->c_str());

    if (_name->cls != str_cls)
        raiseExcHelper(TypeError, "argument 1 must be string, not %s", getTypeName(_name));
    BoxedString* name = static_cast<BoxedString*>(_name);

    Box* _dict = _args[0];
    if (_dict->cls != dict_cls)
        raiseExcHelper(TypeError, "PyClass_New: dict must be a dictionary");
    BoxedDict* dict = static_cast<BoxedDict*>(_dict);

    if (_bases->cls != tuple_cls)
        raiseExcHelper(TypeError, "PyClass_New: bases must be a tuple");
    BoxedTuple* bases = static_cast<BoxedTuple*>(_bases);

    BoxedClassobj* made = new BoxedClassobj(cls, name, bases);

    made->giveAttr("__module__", boxString(getCurrentModule()->name()));
    made->giveAttr("__doc__", None);

    for (auto& p : dict->d) {
        RELEASE_ASSERT(p.first->cls == str_cls, "");
        made->setattr(static_cast<BoxedString*>(p.first)->s, p.second, NULL);
    }

    // Note: make sure to do this after assigning the attrs, since it will overwrite any defined __name__
    made->setattr("__name__", name, NULL);
    made->setattr("__bases__", bases, NULL);

    return made;
}

Box* classobjCall(Box* _cls, Box* _args, Box* _kwargs) {
    assert(_cls->cls == classobj_cls);
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    assert(_args->cls == tuple_cls);
    BoxedTuple* args = static_cast<BoxedTuple*>(_args);

    assert(_kwargs->cls == dict_cls);
    BoxedDict* kwargs = static_cast<BoxedDict*>(_kwargs);

    BoxedInstance* made = new BoxedInstance(cls);

    static const std::string init_str("__init__");
    Box* init_func = classLookup(cls, init_str);

    if (init_func) {
        Box* init_rtn = runtimeCall(init_func, ArgPassSpec(1, 0, true, true), made, args, kwargs, NULL, NULL);
        if (init_rtn != None)
            raiseExcHelper(TypeError, "__init__() should return None");
    } else {
        if (args->elts.size() || kwargs->d.size())
            raiseExcHelper(TypeError, "this constructor takes no arguments");
    }
    return made;
}

Box* classobjStr(Box* _obj) {
    if (!isSubclass(_obj->cls, classobj_cls)) {
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'classobj' object but received an '%s'",
                       getTypeName(_obj)->c_str());
    }

    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_obj);

    Box* _mod = cls->getattr("__module__");
    RELEASE_ASSERT(_mod, "");
    RELEASE_ASSERT(_mod->cls == str_cls, "");
    return boxString(static_cast<BoxedString*>(_mod)->s + "." + cls->name->s);
}

static Box* _instanceGetattribute(Box* _inst, Box* _attr, bool raise_on_missing) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // TODO: special handling for accessing __dict__ and __class__
    if (attr->s[0] == '_' && attr->s[1] == '_') {
        if (attr->s == "__dict__")
            return makeAttrWrapper(inst);

        if (attr->s == "__class__")
            return inst->inst_cls;
    }

    Box* r = inst->getattr(attr->s);
    if (r)
        return r;

    r = classLookup(inst->inst_cls, attr->s);
    if (r) {
        return processDescriptor(r, inst, inst->inst_cls);
    }
    RELEASE_ASSERT(!r, "");

    static const std::string getattr_str("__getattr__");
    Box* getattr = classLookup(inst->inst_cls, getattr_str);

    if (getattr) {
        getattr = processDescriptor(getattr, inst, inst->inst_cls);
        return runtimeCall(getattr, ArgPassSpec(1), _attr, NULL, NULL, NULL, NULL);
    }

    if (!raise_on_missing)
        return NULL;

    raiseExcHelper(AttributeError, "%s instance has no attribute '%s'", inst->inst_cls->name->s.c_str(),
                   attr->s.c_str());
}

Box* instanceGetattribute(Box* _inst, Box* _attr) {
    return _instanceGetattribute(_inst, _attr, true);
}

Box* instanceRepr(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* repr_func = _instanceGetattribute(inst, boxStrConstant("__repr__"), false);

    if (repr_func) {
        return runtimeCall(repr_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        Box* class_str = classobjStr(inst->inst_cls);
        assert(class_str->cls == str_cls);

        char buf[80];
        snprintf(buf, 80, "<%s instance at %p>", static_cast<BoxedString*>(class_str)->s.c_str(), inst);
        return boxStrConstant(buf);
    }
}

Box* instanceStr(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* str_func = _instanceGetattribute(inst, boxStrConstant("__str__"), false);

    if (str_func) {
        return runtimeCall(str_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        return instanceRepr(inst);
        return objectStr(_inst);
    }
}

Box* instanceNonzero(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* nonzero_func = NULL;
    try {
        nonzero_func = _instanceGetattribute(inst, boxStrConstant("__nonzero__"), false);
    } catch (Box* b) {
        if (!isInstance(b, AttributeError))
            throw;
    }

    if (nonzero_func == NULL) {
        try {
            nonzero_func = _instanceGetattribute(inst, boxStrConstant("__len__"), false);
        } catch (Box* b) {
            if (!isInstance(b, AttributeError))
                throw;
        }
    }

    if (nonzero_func) {
        return runtimeCall(nonzero_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        return True;
    }
}

Box* instanceLen(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* len_func = _instanceGetattribute(inst, boxStrConstant("__len__"), true);
    return runtimeCall(len_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceGetitem(Box* _inst, Box* key) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* getitem_func = _instanceGetattribute(inst, boxStrConstant("__getitem__"), true);
    return runtimeCall(getitem_func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
}

Box* instanceSetitem(Box* _inst, Box* key, Box* value) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* setitem_func = _instanceGetattribute(inst, boxStrConstant("__setitem__"), true);
    return runtimeCall(setitem_func, ArgPassSpec(2), key, value, NULL, NULL, NULL);
}

void setupClassobj() {
    classobj_cls = new BoxedHeapClass(type_cls, object_cls, &BoxedClassobj::gcHandler, offsetof(BoxedClassobj, attrs),
                                      sizeof(BoxedClassobj), false);
    instance_cls = new BoxedHeapClass(type_cls, object_cls, &BoxedInstance::gcHandler, offsetof(BoxedInstance, attrs),
                                      sizeof(BoxedInstance), false);

    classobj_cls->giveAttr("__name__", boxStrConstant("classobj"));

    classobj_cls->giveAttr("__new__",
                           new BoxedFunction(boxRTFunction((void*)classobjNew, UNKNOWN, 4, 0, false, false)));

    classobj_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)classobjCall, UNKNOWN, 1, 0, true, true)));

    classobj_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)classobjStr, STR, 1)));

    classobj_cls->freeze();


    instance_cls->giveAttr("__name__", boxStrConstant("instance"));

    instance_cls->giveAttr("__getattribute__",
                           new BoxedFunction(boxRTFunction((void*)instanceGetattribute, UNKNOWN, 2)));
    instance_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)instanceStr, UNKNOWN, 1)));
    instance_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)instanceRepr, UNKNOWN, 1)));
    instance_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)instanceNonzero, UNKNOWN, 1)));
    instance_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)instanceLen, UNKNOWN, 1)));
    instance_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)instanceGetitem, UNKNOWN, 2)));
    instance_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)instanceSetitem, UNKNOWN, 3)));

    instance_cls->freeze();
}
}
