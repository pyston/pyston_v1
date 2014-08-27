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

#include "codegen/compvars.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* classobj_cls, *instance_cls;

class BoxedClassobj : public Box {
public:
    HCAttrs attrs;

    BoxedTuple* bases;
    BoxedString* name;

    BoxedClassobj(BoxedClass* metaclass, BoxedString* name, BoxedTuple* bases)
        : Box(metaclass), bases(bases), name(name) {}

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == classobj_cls);
        BoxedClassobj* o = static_cast<BoxedClassobj*>(_o);

        boxGCHandler(v, o);
    }
};

class BoxedInstance : public Box {
public:
    HCAttrs attrs;

    BoxedClassobj* inst_cls;

    BoxedInstance(BoxedClassobj* inst_cls) : Box(instance_cls), inst_cls(inst_cls) {}

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == instance_cls);
        BoxedInstance* o = static_cast<BoxedInstance*>(_o);

        boxGCHandler(v, o);
    }
};

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

Box* instanceGetattribute(Box* _inst, Box* _attr) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // TODO: special handling for accessing __dict__ and __class__

    Box* r = inst->getattr(attr->s);
    if (r)
        return r;

    r = classLookup(inst->inst_cls, attr->s);
    if (r) {
        static const std::string get_str("__get__");
        Box* descr_r = callattrInternal(r, &get_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(2), inst,
                                        inst->inst_cls, NULL, NULL, NULL);
        if (descr_r)
            return descr_r;
        return r;
    }
    RELEASE_ASSERT(!r, "");

    static const std::string getattr_str("__getattr__");
    Box* getattr = classLookup(inst->inst_cls, getattr_str);
    RELEASE_ASSERT(getattr == NULL, "unimplemented");

    raiseExcHelper(AttributeError, "%s instance has no attribute '%s'", inst->inst_cls->name->s.c_str(),
                   attr->s.c_str());
}

Box* instanceStr(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* str_func = instanceGetattribute(inst, boxStrConstant("__str__"));

    if (str_func) {
        return runtimeCall(str_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        return objectStr(_inst);
    }
}

void setupClassobj() {
    classobj_cls = new BoxedClass(type_cls, object_cls, &BoxedClassobj::gcHandler, offsetof(BoxedClassobj, attrs),
                                  sizeof(BoxedClassobj), false);
    instance_cls = new BoxedClass(type_cls, object_cls, &BoxedInstance::gcHandler, offsetof(BoxedInstance, attrs),
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

    instance_cls->freeze();
}
}
