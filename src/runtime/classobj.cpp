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

#include "runtime/classobj.h"

#include <sstream>

#include "capi/types.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {
BoxedClass* classobj_cls, *instance_cls;
}

static Box* classLookup(BoxedClassobj* cls, BoxedString* attr) {
    Box* r = cls->getattr(attr);
    if (r)
        return r;

    for (auto b : *cls->bases) {
        RELEASE_ASSERT(b->cls == classobj_cls, "");
        Box* r = classLookup(static_cast<BoxedClassobj*>(b), attr);
        if (r)
            return r;
    }

    return NULL;
}

extern "C" int PyClass_IsSubclass(PyObject* klass, PyObject* base) noexcept {
    Py_ssize_t i, n;
    if (klass == base)
        return 1;
    if (PyTuple_Check(base)) {
        n = PyTuple_GET_SIZE(base);
        for (i = 0; i < n; i++) {
            if (PyClass_IsSubclass(klass, PyTuple_GET_ITEM(base, i)))
                return 1;
        }
        return 0;
    }
    if (klass == NULL || !PyClass_Check(klass))
        return 0;
    BoxedClassobj* cp = (BoxedClassobj*)klass;
    n = PyTuple_Size(cp->bases);
    for (i = 0; i < n; i++) {
        if (PyClass_IsSubclass(PyTuple_GetItem(cp->bases, i), base))
            return 1;
    }
    return 0;
}

Box* classobjNew(Box* _cls, Box* _name, Box* _bases, Box** _args) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "classobj.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, classobj_cls))
        raiseExcHelper(TypeError, "classobj.__new__(%s): %s is not a subtype of classobj", getNameOfClass(cls),
                       getNameOfClass(cls));

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

    for (auto base : *bases) {
        if (!PyClass_Check(base) && PyCallable_Check(base->cls)) {
            Box* r = PyObject_CallFunctionObjArgs(base->cls, name, bases, dict, NULL);
            if (!r)
                throwCAPIException();
            return r;
        }
    }

    BoxedClassobj* made = new (cls) BoxedClassobj(name, bases);

    made->giveAttr("__module__", boxString(getCurrentModule()->name()));
    made->giveAttr("__doc__", None);

    for (auto& p : dict->d) {
        RELEASE_ASSERT(p.first->cls == str_cls, "");
        BoxedString* s = (BoxedString*)p.first;
        internStringMortalInplace(s);
        made->setattr(s, p.second, NULL);
    }

    // Note: make sure to do this after assigning the attrs, since it will overwrite any defined __name__
    made->setattr(internStringMortal("__name__"), name, NULL);
    made->setattr(internStringMortal("__bases__"), bases, NULL);

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

    static BoxedString* init_str = internStringImmortal("__init__");
    Box* init_func = classLookup(cls, init_str);

    if (init_func) {
        Box* init_rtn = runtimeCall(init_func, ArgPassSpec(1, 0, true, true), made, args, kwargs, NULL, NULL);
        if (init_rtn != None)
            raiseExcHelper(TypeError, "__init__() should return None");
    } else {
        if (args->size() || kwargs->d.size())
            raiseExcHelper(TypeError, "this constructor takes no arguments");
    }
    return made;
}

static Box* classobjGetattribute(Box* _cls, Box* _attr) {
    RELEASE_ASSERT(_cls->cls == classobj_cls, "");
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // These are special cases in CPython as well:
    if (attr->s()[0] == '_' && attr->s()[1] == '_') {
        if (attr->s() == "__dict__")
            return cls->getAttrWrapper();

        if (attr->s() == "__bases__")
            return cls->bases;

        if (attr->s() == "__name__") {
            if (cls->name)
                return cls->name;
            return None;
        }
    }

    Box* r = classLookup(cls, attr);
    if (!r)
        raiseExcHelper(AttributeError, "class %s has no attribute '%s'", cls->name->data(), attr->data());

    r = processDescriptor(r, None, cls);
    return r;
}

static Box* classobj_getattro(Box* cls, Box* attr) noexcept {
    try {
        return classobjGetattribute(cls, attr);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static const char* set_bases(PyClassObject* c, PyObject* v) {
    Py_ssize_t i, n;

    if (v == NULL || !PyTuple_Check(v))
        return "__bases__ must be a tuple object";
    n = PyTuple_Size(v);
    for (i = 0; i < n; i++) {
        PyObject* x = PyTuple_GET_ITEM(v, i);
        if (!PyClass_Check(x))
            return "__bases__ items must be classes";
        if (PyClass_IsSubclass(x, (PyObject*)c))
            return "a __bases__ item causes an inheritance cycle";
    }
    // Pyston change:
    // set_slot(&c->cl_bases, v);
    // set_attr_slots(c);
    ((BoxedClassobj*)c)->bases = (BoxedTuple*)v;
    return "";
}

static void classobjSetattr(Box* _cls, Box* _attr, Box* _value) {
    RELEASE_ASSERT(_cls->cls == classobj_cls, "");
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    if (attr->s() == "__bases__") {
        const char* error_str = set_bases((PyClassObject*)cls, _value);
        if (error_str && error_str[0] != '\0')
            raiseExcHelper(TypeError, "%s", error_str);
        static BoxedString* bases_str = internStringImmortal("__bases__");
        cls->setattr(bases_str, _value, NULL);
        return;
    }
    PyObject_GenericSetAttr(cls, _attr, _value);
    checkAndThrowCAPIException();
}

static int classobj_setattro(Box* cls, Box* attr, Box* value) noexcept {
    try {
        if (value) {
            classobjSetattr(cls, attr, value);
            return 0;
        } else {
            RELEASE_ASSERT(0, "");
        }
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* classobjStr(Box* _obj) {
    if (!isSubclass(_obj->cls, classobj_cls)) {
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'classobj' object but received an '%s'",
                       getTypeName(_obj));
    }

    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_obj);

    static BoxedString* module_str = internStringImmortal("__module__");
    Box* _mod = cls->getattr(module_str);
    RELEASE_ASSERT(_mod, "");
    RELEASE_ASSERT(_mod->cls == str_cls, "");
    return boxStringTwine(llvm::Twine(static_cast<BoxedString*>(_mod)->s()) + "." + cls->name->s());
}

static Box* _instanceGetattribute(Box* _inst, Box* _attr, bool raise_on_missing) {
    STAT_TIMER(t0, "us_timer_instance_getattribute", 0);

    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // These are special cases in CPython as well:
    if (attr->s()[0] == '_' && attr->s()[1] == '_') {
        if (attr->s() == "__dict__")
            return inst->getAttrWrapper();

        if (attr->s() == "__class__")
            return inst->inst_cls;
    }

    Box* r = inst->getattr(attr);
    if (r)
        return r;

    r = classLookup(inst->inst_cls, attr);
    if (r) {
        return processDescriptor(r, inst, inst->inst_cls);
    }
    RELEASE_ASSERT(!r, "");

    static BoxedString* getattr_str = internStringImmortal("__getattr__");
    Box* getattr = classLookup(inst->inst_cls, getattr_str);

    if (getattr) {
        getattr = processDescriptor(getattr, inst, inst->inst_cls);
        return runtimeCall(getattr, ArgPassSpec(1), _attr, NULL, NULL, NULL, NULL);
    }

    if (!raise_on_missing)
        return NULL;

    raiseExcHelper(AttributeError, "%s instance has no attribute '%s'", inst->inst_cls->name->data(), attr->data());
}

Box* instanceGetattribute(Box* _inst, Box* _attr) {
    return _instanceGetattribute(_inst, _attr, true);
}

static Box* instance_getattro(Box* cls, Box* attr) noexcept {
    try {
        return instanceGetattribute(cls, attr);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* instanceSetattr(Box* _inst, Box* _attr, Box* value) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    assert(value);

    // These are special cases in CPython as well:
    if (attr->s()[0] == '_' && attr->s()[1] == '_') {
        if (attr->s() == "__dict__")
            Py_FatalError("unimplemented");

        if (attr->s() == "__class__") {
            if (value->cls != classobj_cls)
                raiseExcHelper(TypeError, "__class__ must be set to a class");

            inst->inst_cls = static_cast<BoxedClassobj*>(value);
            return None;
        }
    }

    static BoxedString* setattr_str = internStringImmortal("__setattr__");
    Box* setattr = classLookup(inst->inst_cls, setattr_str);

    if (setattr) {
        setattr = processDescriptor(setattr, inst, inst->inst_cls);
        return runtimeCall(setattr, ArgPassSpec(2), _attr, value, NULL, NULL, NULL);
    }

    _inst->setattr(attr, value, NULL);
    return None;
}

Box* instanceDelattr(Box* _inst, Box* _attr) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // These are special cases in CPython as well:
    if (attr->s()[0] == '_' && attr->s()[1] == '_') {
        if (attr->s() == "__dict__")
            raiseExcHelper(TypeError, "__dict__ must be set to a dictionary");

        if (attr->s() == "__class__")
            raiseExcHelper(TypeError, "__class__ must be set to a class");
    }

    static BoxedString* delattr_str = internStringImmortal("__delattr__");
    Box* delattr = classLookup(inst->inst_cls, delattr_str);

    if (delattr) {
        delattr = processDescriptor(delattr, inst, inst->inst_cls);
        return runtimeCall(delattr, ArgPassSpec(1), _attr, NULL, NULL, NULL, NULL);
    }

    _inst->delattr(attr, NULL);
    return None;
}

static int instance_setattro(Box* cls, Box* attr, Box* value) noexcept {
    try {
        if (value) {
            instanceSetattr(cls, attr, value);
            return 0;
        } else {
            RELEASE_ASSERT(0, "");
        }
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* instanceRepr(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* repr_str = internStringImmortal("__repr__");
    Box* repr_func = _instanceGetattribute(inst, repr_str, false);

    if (repr_func) {
        return runtimeCall(repr_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        Box* class_str = classobjStr(inst->inst_cls);
        assert(class_str->cls == str_cls);

        char buf[80];
        snprintf(buf, 80, "<%s instance at %p>", static_cast<BoxedString*>(class_str)->data(), inst);
        return boxString(buf);
    }
}

Box* instanceStr(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* str_str = internStringImmortal("__str__");
    Box* str_func = _instanceGetattribute(inst, str_str, false);

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

    static BoxedString* nonzero_str = internStringImmortal("__nonzero__");

    Box* nonzero_func = NULL;
    try {
        nonzero_func = _instanceGetattribute(inst, nonzero_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
    }

    if (nonzero_func == NULL) {
        static BoxedString* len_str = internStringImmortal("__len__");
        try {
            nonzero_func = _instanceGetattribute(inst, len_str, false);
        } catch (ExcInfo e) {
            if (!e.matches(AttributeError))
                throw e;
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

    static BoxedString* len_str = internStringImmortal("__len__");
    Box* len_func = _instanceGetattribute(inst, len_str, true);
    return runtimeCall(len_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceGetitem(Box* _inst, Box* key) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* getitem_str = internStringImmortal("__getitem__");
    Box* getitem_func = _instanceGetattribute(inst, getitem_str, true);
    return runtimeCall(getitem_func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
}

Box* instanceSetitem(Box* _inst, Box* key, Box* value) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* setitem_str = internStringImmortal("__setitem__");
    Box* setitem_func = _instanceGetattribute(inst, setitem_str, true);
    return runtimeCall(setitem_func, ArgPassSpec(2), key, value, NULL, NULL, NULL);
}

Box* instanceDelitem(Box* _inst, Box* key) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* delitem_str = internStringImmortal("__delitem__");
    Box* delitem_func = _instanceGetattribute(inst, delitem_str, true);
    return runtimeCall(delitem_func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
}

/* Try a 3-way comparison, returning an int; v is an instance.  Return:
   -2 for an exception;
   -1 if v < w;
   0 if v == w;
   1 if v > w;
   2 if this particular 3-way comparison is not implemented or undefined.
*/
static int half_cmp(PyObject* v, PyObject* w) noexcept {
    // static PyObject* cmp_obj;
    PyObject* args;
    PyObject* cmp_func;
    PyObject* result;
    long l;

    assert(PyInstance_Check(v));

    static BoxedString* cmp_str = internStringImmortal("__cmp__");
// Pyston change:
#if 0
        if (cmp_obj == NULL) {
            cmp_obj = PyString_InternFromString("__cmp__");
            if (cmp_obj == NULL)
                return -2;
        }

        cmp_func = PyObject_GetAttr(v, cmp_obj);

        if (cmp_func == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return -2;
            PyErr_Clear();
            return 2;
        }
#else
    try {
        cmp_func = _instanceGetattribute(v, cmp_str, false);
        if (!cmp_func)
            return 2;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -2;
    }
#endif

    args = PyTuple_Pack(1, w);
    if (args == NULL) {
        Py_DECREF(cmp_func);
        return -2;
    }

    result = PyEval_CallObject(cmp_func, args);
    Py_DECREF(args);
    Py_DECREF(cmp_func);

    if (result == NULL)
        return -2;

    if (result == Py_NotImplemented) {
        Py_DECREF(result);
        return 2;
    }

    l = PyInt_AsLong(result);
    Py_DECREF(result);
    if (l == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "comparison did not return an int");
        return -2;
    }

    return l < 0 ? -1 : l > 0 ? 1 : 0;
}

/* Try a 3-way comparison, returning an int; either v or w is an instance.
   We first try a coercion.  Return:
   -2 for an exception;
   -1 if v < w;
   0 if v == w;
   1 if v > w;
   2 if this particular 3-way comparison is not implemented or undefined.
*/
static int instance_compare(PyObject* v, PyObject* w) noexcept {
    int c;

    c = PyNumber_CoerceEx(&v, &w);
    if (c < 0)
        return -2;
    if (c == 0) {
        /* If neither is now an instance, use regular comparison */
        if (!PyInstance_Check(v) && !PyInstance_Check(w)) {
            c = PyObject_Compare(v, w);
            Py_DECREF(v);
            Py_DECREF(w);
            if (PyErr_Occurred())
                return -2;
            return c < 0 ? -1 : c > 0 ? 1 : 0;
        }
    } else {
        /* The coercion didn't do anything.
           Treat this the same as returning v and w unchanged. */
        Py_INCREF(v);
        Py_INCREF(w);
    }

    if (PyInstance_Check(v)) {
        c = half_cmp(v, w);
        if (c <= 1) {
            Py_DECREF(v);
            Py_DECREF(w);
            return c;
        }
    }
    if (PyInstance_Check(w)) {
        c = half_cmp(w, v);
        if (c <= 1) {
            Py_DECREF(v);
            Py_DECREF(w);
            if (c >= -1)
                c = -c;
            return c;
        }
    }
    Py_DECREF(v);
    Py_DECREF(w);
    return 2;
}

Box* instanceCompare(Box* _inst, Box* other) {
    int rtn = instance_compare(_inst, other);
    if (rtn == 2)
        return NotImplemented;
    if (rtn == -2)
        throwCAPIException();
    return boxInt(rtn);
}

Box* instanceContains(Box* _inst, Box* key) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* contains_str = internStringImmortal("__contains__");
    Box* contains_func = _instanceGetattribute(inst, contains_str, false);

    if (!contains_func) {
        int result = _PySequence_IterSearch(inst, key, PY_ITERSEARCH_CONTAINS);
        if (result < 0)
            throwCAPIException();
        assert(result == 0 || result == 1);
        return boxBool(result);
    }

    Box* r = runtimeCall(contains_func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
    return boxBool(nonzero(r));
}

static Box* instanceHash(BoxedInstance* inst) {
    assert(inst->cls == instance_cls);

    PyObject* func;
    PyObject* res;

    static BoxedString* hash_str = internStringImmortal("__hash__");
    static BoxedString* eq_str = internStringImmortal("__eq__");
    static BoxedString* cmp_str = internStringImmortal("__cmp__");

    func = _instanceGetattribute(inst, hash_str, false);
    if (func == NULL) {
        /* If there is no __eq__ and no __cmp__ method, we hash on the
           address.  If an __eq__ or __cmp__ method exists, there must
           be a __hash__. */
        func = _instanceGetattribute(inst, eq_str, false);
        if (func == NULL) {
            func = _instanceGetattribute(inst, cmp_str, false);
            if (func == NULL) {
                return boxInt(_Py_HashPointer(inst));
            }
        }
        raiseExcHelper(TypeError, "unhashable instance");
    }

    res = runtimeCall(func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    if (PyInt_Check(res) || PyLong_Check(res)) {
        CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
        return callattr(res, hash_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
    } else {
        raiseExcHelper(TypeError, "__hash__() should return an int");
    }
}

static Box* instanceIter(BoxedInstance* self) {
    assert(self->cls == instance_cls);

    PyObject* func;

    static BoxedString* iter_str = internStringImmortal("__iter__");
    static BoxedString* getitem_str = internStringImmortal("__getitem__");
    if ((func = _instanceGetattribute(self, iter_str, false)) != NULL) {
        PyObject* res = PyEval_CallObject(func, (PyObject*)NULL);
        if (!res)
            throwCAPIException();

        if (!PyIter_Check(res))
            raiseExcHelper(TypeError, "__iter__ returned non-iterator of type '%.100s'", res->cls->tp_name);
        return res;
    }

    if ((func = _instanceGetattribute(self, getitem_str, false)) == NULL) {
        raiseExcHelper(TypeError, "iteration over non-sequence");
    }

    Box* r = PySeqIter_New((PyObject*)self);
    if (!r)
        throwCAPIException();
    return r;
}

static Box* instanceNext(BoxedInstance* inst) {
    assert(inst->cls == instance_cls);

    static BoxedString* next_str = internStringImmortal("next");
    Box* next_func = _instanceGetattribute(inst, next_str, false);

    if (!next_func) {
        // not 100% sure why this is a different error:
        raiseExcHelper(TypeError, "instance has no next() method");
    }

    Box* r = runtimeCall(next_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    return r;
}

static PyObject* instance_index(PyObject* self) noexcept {
    PyObject* func, *res;
    /*
    static PyObject* indexstr = NULL;

    if (indexstr == NULL) {
        indexstr = PyString_InternFromString("__index__");
        if (indexstr == NULL)
            return NULL;
    }
    */
    if ((func = instance_getattro(self, boxString("__index__"))) == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return NULL;
        PyErr_Clear();
        PyErr_SetString(PyExc_TypeError, "object cannot be interpreted as an index");
        return NULL;
    }
    res = PyEval_CallObject(func, (PyObject*)NULL);
    Py_DECREF(func);
    return res;
}

static Box* _instanceBinary(Box* _inst, Box* other, BoxedString* attr) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* func = _instanceGetattribute(inst, attr, false);
    if (!func)
        return NotImplemented;
    return runtimeCall(func, ArgPassSpec(1), other, NULL, NULL, NULL, NULL);
}

Box* instanceGt(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__gt__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceGe(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ge__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceLt(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__lt__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceLe(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__le__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceEq(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__eq__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceNe(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ne__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceCall(Box* _inst, Box* _args, Box* _kwargs) {
    assert(_inst->cls == instance_cls);
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* call_str = internStringImmortal("__call__");
    Box* call_func = _instanceGetattribute(inst, call_str, false);
    if (!call_func)
        raiseExcHelper(AttributeError, "%s instance has no __call__ method", inst->inst_cls->name->data());

    return runtimeCall(call_func, ArgPassSpec(0, 0, true, true), _args, _kwargs, NULL, NULL, NULL);
}

extern "C" PyObject* PyClass_New(PyObject* bases, PyObject* dict, PyObject* name) noexcept {
    try {
        if (name == NULL || !PyString_Check(name)) {
            PyErr_SetString(PyExc_TypeError, "PyClass_New: name must be a string");
            return NULL;
        }
        if (dict == NULL || !PyDict_Check(dict)) {
            PyErr_SetString(PyExc_TypeError, "PyClass_New: dict must be a dictionary");
            return NULL;
        }

        return runtimeCall(classobj_cls, ArgPassSpec(3), name, bases, dict, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyMethod_New(PyObject* func, PyObject* self, PyObject* klass) noexcept {
    try {
        return new BoxedInstanceMethod(self, func, klass);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyMethod_Function(PyObject* im) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((BoxedInstanceMethod*)im)->func;
}

extern "C" PyObject* PyMethod_Self(PyObject* im) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((BoxedInstanceMethod*)im)->obj;
}

extern "C" PyObject* PyMethod_Class(PyObject* im) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((BoxedInstanceMethod*)im)->im_class;
}

void setupClassobj() {
    classobj_cls = BoxedHeapClass::create(type_cls, object_cls, &BoxedClassobj::gcHandler,
                                          offsetof(BoxedClassobj, attrs), 0, sizeof(BoxedClassobj), false, "classobj");
    instance_cls
        = BoxedHeapClass::create(type_cls, object_cls, &BoxedInstance::gcHandler, offsetof(BoxedInstance, attrs),
                                 offsetof(BoxedInstance, weakreflist), sizeof(BoxedInstance), false, "instance");

    classobj_cls->giveAttr("__new__",
                           new BoxedFunction(boxRTFunction((void*)classobjNew, UNKNOWN, 4, 0, false, false)));

    classobj_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)classobjCall, UNKNOWN, 1, 0, true, true)));

    classobj_cls->giveAttr("__getattribute__",
                           new BoxedFunction(boxRTFunction((void*)classobjGetattribute, UNKNOWN, 2)));
    classobj_cls->giveAttr("__setattr__", new BoxedFunction(boxRTFunction((void*)classobjSetattr, UNKNOWN, 3)));
    classobj_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)classobjStr, STR, 1)));
    classobj_cls->giveAttr("__dict__", dict_descr);

    classobj_cls->freeze();
    classobj_cls->tp_getattro = classobj_getattro;
    classobj_cls->tp_setattro = classobj_setattro;


    instance_cls->giveAttr("__getattribute__",
                           new BoxedFunction(boxRTFunction((void*)instanceGetattribute, UNKNOWN, 2)));
    instance_cls->giveAttr("__setattr__", new BoxedFunction(boxRTFunction((void*)instanceSetattr, UNKNOWN, 3)));
    instance_cls->giveAttr("__delattr__", new BoxedFunction(boxRTFunction((void*)instanceDelattr, UNKNOWN, 2)));
    instance_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)instanceStr, UNKNOWN, 1)));
    instance_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)instanceRepr, UNKNOWN, 1)));
    instance_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)instanceNonzero, UNKNOWN, 1)));
    instance_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)instanceLen, UNKNOWN, 1)));
    instance_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)instanceGetitem, UNKNOWN, 2)));
    instance_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)instanceSetitem, UNKNOWN, 3)));
    instance_cls->giveAttr("__delitem__", new BoxedFunction(boxRTFunction((void*)instanceDelitem, UNKNOWN, 2)));
    instance_cls->giveAttr("__cmp__", new BoxedFunction(boxRTFunction((void*)instanceCompare, UNKNOWN, 2)));
    instance_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)instanceContains, UNKNOWN, 2)));
    instance_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)instanceHash, UNKNOWN, 1)));
    instance_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)instanceIter, UNKNOWN, 1)));
    instance_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)instanceNext, UNKNOWN, 1)));
    instance_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)instanceCall, UNKNOWN, 1, 0, true, true)));
    instance_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)instanceEq, UNKNOWN, 2)));
    instance_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)instanceNe, UNKNOWN, 2)));
    instance_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)instanceLt, UNKNOWN, 2)));
    instance_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)instanceLe, UNKNOWN, 2)));
    instance_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)instanceGt, UNKNOWN, 2)));
    instance_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)instanceGe, UNKNOWN, 2)));

    instance_cls->freeze();
    instance_cls->tp_getattro = instance_getattro;
    instance_cls->tp_setattro = instance_setattro;
    instance_cls->tp_as_number->nb_index = instance_index;
}
}
