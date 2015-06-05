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

#include "capi/types.h"
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

    BoxedSuper(BoxedClass* type, Box* obj, BoxedClass* obj_type) : type(type), obj(obj), obj_type(obj_type) {}

    DEFAULT_CLASS(super_cls);

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(isSubclass(_o->cls, super_cls));
        BoxedSuper* o = static_cast<BoxedSuper*>(_o);

        boxGCHandler(v, o);
        if (o->type)
            v->visit(o->type);
        if (o->obj)
            v->visit(o->obj);
        if (o->obj_type)
            v->visit(o->obj_type);
    }
};

static const char* class_str = "__class__";

Box* superGetattribute(Box* _s, Box* _attr) {
    RELEASE_ASSERT(isSubclass(_s->cls, super_cls), "");
    BoxedSuper* s = static_cast<BoxedSuper*>(_s);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    bool skip = s->obj_type == NULL;

    if (!skip) {
        // Looks like __class__ is supposed to be "super", not the class of the the proxied object.
        skip = (attr->s() == class_str);
    }

    if (!skip) {
        PyObject* mro, *res, *tmp, *dict;
        PyTypeObject* starttype;
        descrgetfunc f;
        Py_ssize_t i, n;

        starttype = s->obj_type;
        mro = starttype->tp_mro;

        if (mro == NULL)
            n = 0;
        else {
            assert(PyTuple_Check(mro));
            n = PyTuple_GET_SIZE(mro);
        }
        for (i = 0; i < n; i++) {
            if ((PyObject*)(s->type) == PyTuple_GET_ITEM(mro, i))
                break;
        }
        i++;
        res = NULL;
        for (; i < n; i++) {
            tmp = PyTuple_GET_ITEM(mro, i);

// Pyston change:
#if 0
            if (PyType_Check(tmp))
                dict = ((PyTypeObject *)tmp)->tp_dict;
            else if (PyClass_Check(tmp))
                dict = ((PyClassObject *)tmp)->cl_dict;
            else
                continue;
            res = PyDict_GetItem(dict, name);
#endif
            res = tmp->getattr(std::string(attr->s()));

            if (res != NULL) {
// Pyston change:
#if 0
                Py_INCREF(res);
                f = Py_TYPE(res)->tp_descr_get;
                if (f != NULL) {
                    tmp = f(res,
                        /* Only pass 'obj' param if
                           this is instance-mode sper
                           (See SF ID #743627)
                        */
                        (s->obj == (PyObject *)
                                    s->obj_type
                            ? (PyObject *)NULL
                            : s->obj),
                        (PyObject *)starttype);
                    Py_DECREF(res);
                    res = tmp;
                }
#endif
                return processDescriptor(res, (s->obj == s->obj_type ? None : s->obj), s->obj_type);
            }
        }
    }

    Box* r = getattrInternalGeneric(s, attr->s(), NULL, true, false, NULL, NULL);
    if (r)
        return processDescriptor(r, s, s->cls);
    raiseExcHelper(AttributeError, "'super' object has no attribute '%s'", attr->data());
}

Box* superRepr(Box* _s) {
    RELEASE_ASSERT(_s->cls == super_cls, "");
    BoxedSuper* s = static_cast<BoxedSuper*>(_s);

    if (s->obj_type) {
        return boxStringTwine(llvm::Twine("<super: <class '") + (s->type ? getNameOfClass(s->type) : "NULL") + "'>, <"
                              + getNameOfClass(s->obj_type) + " object>>");
    } else {
        return boxStringTwine(llvm::Twine("<super: <class '") + (s->type ? getNameOfClass(s->type) : "NULL")
                              + "'>, <NULL>>");
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

Box* superInit(Box* _self, Box* _type, Box* obj) {
    RELEASE_ASSERT(isSubclass(_self->cls, super_cls), "");
    BoxedSuper* self = static_cast<BoxedSuper*>(_self);

    if (!isSubclass(_type->cls, type_cls))
        raiseExcHelper(TypeError, "must be type, not %s", getTypeName(_type));
    BoxedClass* type = static_cast<BoxedClass*>(_type);

    BoxedClass* obj_type = NULL;
    if (obj == None)
        obj = NULL;
    if (obj != NULL)
        obj_type = supercheck(type, obj);

    self->type = type;
    self->obj = obj;
    self->obj_type = obj_type;

    return None;
}

static PyObject* superDescrGet(PyObject* self, PyObject* obj, PyObject* type) {
    BoxedSuper* su = static_cast<BoxedSuper*>(self);
    BoxedSuper* newobj;

    if (obj == NULL || obj == None || su->obj != NULL) {
        /* Not binding to an object, or already bound */
        Py_INCREF(self);
        return self;
    }
    if (su->cls != super_cls) {
        /* If su is an instance of a (strict) subclass of super,
           call its type */
        return runtimeCall(su->cls, ArgPassSpec(2, 0, false, false), su->type, obj, NULL, NULL, NULL);
    } else {
        /* Inline the common case */
        BoxedClass* obj_type = supercheck(su->type, obj);
        if (obj_type == NULL)
            return NULL;
        newobj = new (su->cls) BoxedSuper(NULL, NULL, NULL);
        if (newobj == NULL)
            return NULL;
        Py_INCREF(su->type);
        Py_INCREF(obj);
        newobj->type = su->type;
        newobj->obj = obj;
        newobj->obj_type = obj_type;
        return (PyObject*)newobj;
    }
}

void setupSuper() {
    super_cls = BoxedHeapClass::create(type_cls, object_cls, &BoxedSuper::gcHandler, 0, 0, sizeof(BoxedSuper), false,
                                       "super");

    super_cls->giveAttr("__getattribute__", new BoxedFunction(boxRTFunction((void*)superGetattribute, UNKNOWN, 2)));
    super_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)superRepr, STR, 1)));

    super_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)superDescrGet, UNKNOWN, 3)));

    super_cls->giveAttr("__init__",
                        new BoxedFunction(boxRTFunction((void*)superInit, UNKNOWN, 3, 1, false, false), { NULL }));

    super_cls->giveAttr("__thisclass__",
                        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSuper, type)));
    super_cls->giveAttr("__self__",
                        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSuper, obj)));
    super_cls->giveAttr("__self_class__",
                        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSuper, obj_type)));

    super_cls->freeze();
}
}
