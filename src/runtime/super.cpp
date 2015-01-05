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

#include "runtime/super.h"

#include <sstream>

#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* super_cls;

class BoxedSuper : public Box {
public:
    // These definitions+names are taken from CPython; not sure I understand it well enough yet
    BoxedClass* type;     // "the class invoking super()"
    Box* obj;             // "the instance invoking super(); make be None"
    BoxedClass* obj_type; // "the type of the instance invoking super(); may be None"

    BoxedSuper(BoxedClass* type, Box* obj, BoxedClass* obj_type)
        : Box(super_cls), type(type), obj(obj), obj_type(obj_type) {}

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == super_cls);
        BoxedSuper* o = static_cast<BoxedSuper*>(_o);

        boxGCHandler(v, o);
        v->visit(o->type);
        if (o->obj)
            v->visit(o->obj);
        if (o->obj_type)
            v->visit(o->obj_type);
    }
};

static const std::string class_str("__class__");

Box* superGetattribute(Box* _s, Box* _attr) {
    RELEASE_ASSERT(_s->cls == super_cls, "");
    BoxedSuper* s = static_cast<BoxedSuper*>(_s);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    bool skip = s->obj_type == NULL;

    if (!skip) {
        // Looks like __class__ is supposed to be "super", not the class of the the proxied object.
        skip = (attr->s == class_str);
    }

    if (!skip) {
        // We don't support multiple inheritance yet, so the lookup order is simple:
        Box* r = typeLookup(s->type->base, attr->s, NULL);

        if (r) {
            return processDescriptor(r, (s->obj == s->obj_type ? None : s->obj), s->obj_type);
        }
    }

    RELEASE_ASSERT(0, "should call the equivalent of objectGetattr here");
}

Box* superRepr(Box* _s) {
    RELEASE_ASSERT(_s->cls == super_cls, "");
    BoxedSuper* s = static_cast<BoxedSuper*>(_s);

    if (s->obj_type) {
        return boxString("<super: <class '" + (s->type ? *getNameOfClass(s->type) : "NULL") + "'>, <"
                         + *getNameOfClass(s->obj_type) + " object>>");
    } else {
        return boxString("<super: <class '" + (s->type ? *getNameOfClass(s->type) : "NULL") + "'>, <NULL>>");
    }
}


// Ported from the CPython version:
BoxedClass* supercheck(BoxedClass* type, Box* obj) {
    if (isSubclass(obj->cls, type_cls) && isSubclass(static_cast<BoxedClass*>(obj), type))
        return static_cast<BoxedClass*>(obj);

    if (isSubclass(obj->cls, type)) {
        return obj->cls;
    }

    Box* class_attr = obj->getattr(class_str);
    if (class_attr && isSubclass(class_attr->cls, type_cls) && class_attr != obj->cls) {
        Py_FatalError("warning: this path never tested"); // blindly copied from CPython
        return static_cast<BoxedClass*>(class_attr);
    }

    raiseExcHelper(TypeError, "super(type, obj): obj must be an instance or subtype of type");
}

// TODO This functionality is supposed to be in the __init__ function:
Box* superNew(Box* _cls, Box* _type, Box* obj) {
    RELEASE_ASSERT(_cls == super_cls, "");

    if (!isSubclass(_type->cls, type_cls))
        raiseExcHelper(TypeError, "must be type, not %s", getTypeName(_type)->c_str());
    BoxedClass* type = static_cast<BoxedClass*>(_type);

    BoxedClass* obj_type = NULL;
    if (obj == None)
        obj = NULL;
    if (obj != NULL)
        obj_type = supercheck(type, obj);

    return new BoxedSuper(type, obj, obj_type);
}

void setupSuper() {
    super_cls = new BoxedHeapClass(type_cls, object_cls, &BoxedSuper::gcHandler, 0, sizeof(BoxedSuper), false);

    super_cls->giveAttr("__name__", boxStrConstant("super"));

    super_cls->giveAttr("__getattribute__", new BoxedFunction(boxRTFunction((void*)superGetattribute, UNKNOWN, 2)));
    super_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)superRepr, STR, 1)));

    super_cls->giveAttr("__new__",
                        new BoxedFunction(boxRTFunction((void*)superNew, UNKNOWN, 3, 1, false, false), { NULL }));

    super_cls->freeze();
}
}
