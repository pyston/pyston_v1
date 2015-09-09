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
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {
BoxedClass* classobj_cls, *instance_cls;
}


static Box* classLookup(BoxedClassobj* cls, BoxedString* attr, GetattrRewriteArgs* rewrite_args = NULL) {
    if (rewrite_args)
        assert(!rewrite_args->out_success);

    Box* r = cls->getattr(attr, rewrite_args);
    if (r)
        return r;

    if (rewrite_args) {
        // abort rewriting because we currenly don't guard the particular 'bases' hierarchy
        rewrite_args->out_success = false;
        rewrite_args = NULL;
    }

    for (auto b : *cls->bases) {
        RELEASE_ASSERT(b->cls == classobj_cls, "");
        Box* r = classLookup(static_cast<BoxedClassobj*>(b), attr, rewrite_args);
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
    if (!PyType_Check(_cls))
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

    for (const auto& p : *dict) {
        RELEASE_ASSERT(p.first->cls == str_cls, "");
        BoxedString* s = (BoxedString*)p.first;
        internStringMortalInplace(s);
        made->setattr(s, p.second, NULL);
    }

    // Note: make sure to do this after assigning the attrs, since it will overwrite any defined __name__
    static BoxedString* name_str = internStringImmortal("__name__");
    static BoxedString* bases_str = internStringImmortal("__bases__");
    made->setattr(name_str, name, NULL);
    made->setattr(bases_str, bases, NULL);

    return made;
}

Box* classobjCall(Box* _cls, Box* _args, Box* _kwargs) {
    assert(_cls->cls == classobj_cls);
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    assert(_args->cls == tuple_cls);
    BoxedTuple* args = static_cast<BoxedTuple*>(_args);

    assert(!_kwargs || _kwargs->cls == dict_cls);
    BoxedDict* kwargs = static_cast<BoxedDict*>(_kwargs);

    BoxedInstance* made = new BoxedInstance(cls);

    static BoxedString* init_str = internStringImmortal("__init__");
    Box* init_func = classLookup(cls, init_str);

    if (init_func) {
        Box* init_rtn = runtimeCall(init_func, ArgPassSpec(1, 0, true, true), made, args, kwargs, NULL, NULL);
        if (init_rtn != None)
            raiseExcHelper(TypeError, "__init__() should return None");
    } else {
        if (args->size() || (kwargs && kwargs->d.size()))
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

static Box* classobjSetattr(Box* _cls, Box* _attr, Box* _value) {
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
        return None;
    }
    PyObject_GenericSetAttr(cls, _attr, _value);
    checkAndThrowCAPIException();
    return None;
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


// Analogous to CPython's instance_getattr2
static Box* instanceGetattributeSimple(BoxedInstance* inst, BoxedString* attr_str,
                                       GetattrRewriteArgs* rewriter_args = NULL) {
    Box* r = inst->getattr(attr_str, rewriter_args);
    if (r)
        return r;

    RewriterVar* r_inst = NULL;
    RewriterVar* r_inst_cls = NULL;
    if (rewriter_args) {
        if (!rewriter_args->out_success)
            rewriter_args = NULL;
        else {
            rewriter_args->out_success = false;
            r_inst = rewriter_args->obj;
            r_inst_cls = r_inst->getAttr(offsetof(BoxedInstance, inst_cls));
        }
    }
    GetattrRewriteArgs grewriter_inst_args(rewriter_args ? rewriter_args->rewriter : NULL, r_inst_cls,
                                           rewriter_args ? rewriter_args->rewriter->getReturnDestination()
                                                         : Location());
    r = classLookup(inst->inst_cls, attr_str, rewriter_args ? &grewriter_inst_args : NULL);
    if (!grewriter_inst_args.out_success)
        rewriter_args = NULL;

    if (r) {
        Box* rtn = processDescriptor(r, inst, inst->inst_cls);
        if (rewriter_args) {
            RewriterVar* r_rtn = rewriter_args->rewriter->call(true, (void*)processDescriptor,
                                                               grewriter_inst_args.out_rtn, r_inst, r_inst_cls);
            rewriter_args->out_rtn = r_rtn;
            rewriter_args->out_success = true;
            rewriter_args->out_return_convention = GetattrRewriteArgs::VALID_RETURN;
        }
        return rtn;
    }

    return NULL;
}

static Box* instanceGetattributeWithFallback(BoxedInstance* inst, BoxedString* attr_str,
                                             GetattrRewriteArgs* rewriter_args = NULL) {
    Box* attr_obj = instanceGetattributeSimple(inst, attr_str, rewriter_args);

    if (attr_obj) {
        return attr_obj;
    }

    if (rewriter_args) {
        if (!rewriter_args->out_success)
            rewriter_args = NULL;
        else
            rewriter_args->out_success = false;

        // abort rewriting for now
        rewriter_args = NULL;
    }

    static BoxedString* getattr_str = internStringImmortal("__getattr__");
    Box* getattr = classLookup(inst->inst_cls, getattr_str);

    if (getattr) {
        getattr = processDescriptor(getattr, inst, inst->inst_cls);
        return runtimeCallInternal<CXX>(getattr, NULL, ArgPassSpec(1), attr_str, NULL, NULL, NULL, NULL);
    }

    return NULL;
}

static Box* _instanceGetattribute(Box* _inst, BoxedString* attr_str, bool raise_on_missing,
                                  GetattrRewriteArgs* rewriter_args = NULL) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    // These are special cases in CPython as well:
    if (attr_str->s()[0] == '_' && attr_str->s()[1] == '_') {
        if (attr_str->s() == "__dict__")
            return inst->getAttrWrapper();

        if (attr_str->s() == "__class__")
            return inst->inst_cls;
    }

    Box* attr = instanceGetattributeWithFallback(inst, attr_str, rewriter_args);
    if (attr) {
        return attr;
    } else if (!raise_on_missing) {
        return NULL;
    } else {
        raiseExcHelper(AttributeError, "%s instance has no attribute '%s'", inst->inst_cls->name->data(),
                       attr_str->data());
    }
}

// Analogous to CPython's instance_getattr
Box* instance_getattro(Box* cls, Box* attr) noexcept {
    return instanceGetattroInternal<CAPI>(cls, attr, NULL);
}

template <ExceptionStyle S>
Box* instanceGetattroInternal(Box* cls, Box* _attr, GetattrRewriteArgs* rewrite_args) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_instance_getattro", 0);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    if (S == CAPI) {
        try {
            return _instanceGetattribute(cls, attr, true, rewrite_args);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    } else {
        return _instanceGetattribute(cls, attr, true, rewrite_args);
    }
}

// Force instantiation of the template
template Box* instanceGetattroInternal<CAPI>(Box*, Box*, GetattrRewriteArgs*) noexcept;
template Box* instanceGetattroInternal<CXX>(Box*, Box*, GetattrRewriteArgs*);

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

Box* instanceGetslice(Box* _inst, Box* i, Box* j) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* getslice_str = internStringImmortal("__getslice__");
    Box* getslice_func = NULL;

    try {
        getslice_func = _instanceGetattribute(inst, getslice_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
    }

    if (getslice_func == NULL) {
        Box* slice = static_cast<Box*>(createSlice(i, j, None));
        return instanceGetitem(inst, slice);
    }

    return runtimeCall(getslice_func, ArgPassSpec(2), i, j, NULL, NULL, NULL);
}

Box* instanceSetslice(Box* _inst, Box* i, Box* j, Box** sequence) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* setslice_str = internStringImmortal("__setslice__");
    Box* setslice_func = NULL;

    try {
        setslice_func = _instanceGetattribute(inst, setslice_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
    }

    if (setslice_func == NULL) {
        Box* slice = static_cast<Box*>(createSlice(i, j, None));
        return instanceSetitem(inst, slice, *sequence);
    }

    return runtimeCall(setslice_func, ArgPassSpec(3), i, j, *sequence, NULL, NULL);
}

Box* instanceDelslice(Box* _inst, Box* i, Box* j) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* delslice_str = internStringImmortal("__delslice__");
    Box* delslice_func = NULL;

    try {
        delslice_func = _instanceGetattribute(inst, delslice_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
    }

    if (delslice_func == NULL) {
        Box* slice = static_cast<Box*>(createSlice(i, j, None));
        return instanceDelitem(inst, slice);
    }
    try {
        return runtimeCall(delslice_func, ArgPassSpec(2), i, j, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
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

static PyObject* generic_binary_op(PyObject* v, PyObject* w, char* opname) {
    PyObject* result;
    PyObject* args;
    PyObject* func = PyObject_GetAttrString(v, opname);
    if (func == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return NULL;
        PyErr_Clear();
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    args = PyTuple_Pack(1, w);
    if (args == NULL) {
        Py_DECREF(func);
        return NULL;
    }
    result = PyEval_CallObject(func, args);
    Py_DECREF(args);
    Py_DECREF(func);
    return result;
}

static PyObject* coerce_obj;

/* Try one half of a binary operator involving a class instance. */
static PyObject* half_binop(PyObject* v, PyObject* w, char* opname, binaryfunc thisfunc, int swapped) noexcept {
    PyObject* args;
    PyObject* coercefunc;
    PyObject* coerced = NULL;
    PyObject* v1;
    PyObject* result;

    if (!PyInstance_Check(v)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    if (coerce_obj == NULL) {
        coerce_obj = PyString_InternFromString("__coerce__");
        if (coerce_obj == NULL)
            return NULL;
    }
    coercefunc = PyObject_GetAttr(v, coerce_obj);
    if (coercefunc == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return NULL;
        PyErr_Clear();
        return generic_binary_op(v, w, opname);
    }

    args = PyTuple_Pack(1, w);
    if (args == NULL) {
        Py_DECREF(coercefunc);
        return NULL;
    }
    coerced = PyEval_CallObject(coercefunc, args);
    Py_DECREF(args);
    Py_DECREF(coercefunc);
    if (coerced == NULL) {
        return NULL;
    }
    if (coerced == Py_None || coerced == Py_NotImplemented) {
        Py_DECREF(coerced);
        return generic_binary_op(v, w, opname);
    }
    if (!PyTuple_Check(coerced) || PyTuple_Size(coerced) != 2) {
        Py_DECREF(coerced);
        PyErr_SetString(PyExc_TypeError, "coercion should return None or 2-tuple");
        return NULL;
    }
    v1 = PyTuple_GetItem(coerced, 0);
    w = PyTuple_GetItem(coerced, 1);
    if (v1->cls == v->cls && PyInstance_Check(v)) {
        /* prevent recursion if __coerce__ returns self as the first
         * argument */
        result = generic_binary_op(v1, w, opname);
    } else {
        if (Py_EnterRecursiveCall(" after coercion"))
            return NULL;
        if (swapped)
            result = (thisfunc)(w, v1);
        else
            result = (thisfunc)(v1, w);
        Py_LeaveRecursiveCall();
    }
    Py_DECREF(coerced);
    return result;
}

/* Implement a binary operator involving at least one class instance. */
static PyObject* do_binop(PyObject* v, PyObject* w, char* opname, char* ropname, binaryfunc thisfunc) noexcept {
    PyObject* result = half_binop(v, w, opname, thisfunc, 0);
    if (result == Py_NotImplemented) {
        Py_DECREF(result);
        result = half_binop(w, v, ropname, thisfunc, 1);
    }
    return result;
}

static PyObject* do_binop_inplace(PyObject* v, PyObject* w, char* iopname, char* opname, char* ropname,
                                  binaryfunc thisfunc) noexcept {
    PyObject* result = half_binop(v, w, iopname, thisfunc, 0);
    if (result == Py_NotImplemented) {
        Py_DECREF(result);
        result = do_binop(v, w, opname, ropname, thisfunc);
    }
    return result;
}

static PyObject* bin_power(PyObject* v, PyObject* w) noexcept {
    return PyNumber_Power(v, w, Py_None);
}

/* This version is for ternary calls only (z != None) */
static PyObject* instance_pow(PyObject* v, PyObject* w, PyObject* z) noexcept {
    static BoxedString* pow_str = internStringImmortal("__pow__");
    static BoxedString* ipow_str = internStringImmortal("__ipow__");
    static BoxedString* rpow_str = internStringImmortal("__rpow__");
    if (z == Py_None) {
        return do_binop(v, w, pow_str->data(), rpow_str->data(), bin_power);
    } else {
        PyObject* func;
        PyObject* args;
        PyObject* result;

        /* XXX Doesn't do coercions... */
        func = PyObject_GetAttrString(v, pow_str->data());
        if (func == NULL)
            return NULL;
        args = PyTuple_Pack(2, w, z);
        if (args == NULL) {
            Py_DECREF(func);
            return NULL;
        }
        result = PyEval_CallObject(func, args);
        Py_DECREF(func);
        Py_DECREF(args);
        return result;
    }
}

static PyObject* bin_inplace_power(PyObject* v, PyObject* w) noexcept {
    return PyNumber_InPlacePower(v, w, Py_None);
}

static PyObject* instance_ipow(PyObject* v, PyObject* w, PyObject* z) noexcept {
    static BoxedString* pow_str = internStringImmortal("__pow__");
    static BoxedString* ipow_str = internStringImmortal("__ipow__");
    static BoxedString* rpow_str = internStringImmortal("__rpow__");
    if (z == Py_None) {
        return do_binop_inplace(v, w, ipow_str->data(), pow_str->data(), rpow_str->data(), bin_inplace_power);
    } else {
        /* XXX Doesn't do coercions... */
        PyObject* func;
        PyObject* args;
        PyObject* result;

        func = PyObject_GetAttrString(v, ipow_str->data());
        if (func == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return NULL;
            PyErr_Clear();
            return instance_pow(v, w, z);
        }
        args = PyTuple_Pack(2, w, z);
        if (args == NULL) {
            Py_DECREF(func);
            return NULL;
        }
        result = PyEval_CallObject(func, args);
        Py_DECREF(func);
        Py_DECREF(args);
        return result;
    }
}

static PyObject* instance_index(PyObject* self) noexcept {
    PyObject* func, *res;

    static BoxedString* index_str = internStringImmortal("__index__");
    if ((func = instance_getattro(self, index_str)) == NULL) {
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

static void instance_dealloc(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    // Note that trying to call __del__ as a finalizer does not fallback to
    // __getattr__ unlike other attributes (like __index__). This is CPython's behavior.
    static BoxedString* del_str = internStringImmortal("__del__");
    Box* func = instanceGetattributeSimple(inst, del_str);
    if (func)
        runtimeCall(func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
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

Box* instanceAdd(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__add__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceSub(Box* _inst, Box* other) {

    static BoxedString* attr_str = internStringImmortal("__sub__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceMul(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__mul__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceFloordiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__floordiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceMod(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__mod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceDivMod(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__divmod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instancePow(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__pow__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceLshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__lshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceAnd(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__and__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceXor(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__xor__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceOr(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__or__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceDiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__div__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceTruediv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__truediv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRadd(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__radd__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRsub(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rsub__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRmul(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rmul__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRdiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rdiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRtruediv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rtruediv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRfloordiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rfloordiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRmod(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rmod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRdivmod(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rdivmod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRpow(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rpow__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRlshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rlshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRrshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rrshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRand(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rand__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRxor(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__rxor__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRor(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ror__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIadd(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__iadd__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIsub(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__isub__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceImul(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__imul__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIdiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__idiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceItruediv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__itruediv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIfloordiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ifloordiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceImod(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__imod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIpow(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ipow__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIlshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ilshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIrshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__irshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIand(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__iand__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIxor(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ixor__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIor(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__ior__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceNeg(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* neg_str = internStringImmortal("__neg__");
    Box* neg_func = _instanceGetattribute(inst, neg_str, true);
    return runtimeCall(neg_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instancePos(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* pos_str = internStringImmortal("__pos__");
    Box* pos_func = _instanceGetattribute(inst, pos_str, true);
    return runtimeCall(pos_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceAbs(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* abs_str = internStringImmortal("__abs__");
    Box* abs_func = _instanceGetattribute(inst, abs_str, true);
    return runtimeCall(abs_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceInvert(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* invert_str = internStringImmortal("__invert__");
    Box* invert_func = _instanceGetattribute(inst, invert_str, true);
    return runtimeCall(invert_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceTrunc(BoxedInstance* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* trunc_str = internStringImmortal("__trunc__");
    Box* trunc_func = _instanceGetattribute(inst, trunc_str, true);

    return runtimeCall(trunc_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceInt(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* int_str = internStringImmortal("__int__");
    if (PyObject_HasAttr((PyObject*)inst, int_str)) {
        Box* int_func = _instanceGetattribute(inst, int_str, true);
        return runtimeCall(int_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    }

    Box* truncated = instanceTrunc(inst);
    /* __trunc__ is specified to return an Integral type, but
       int() needs to return an int. */
    Box* res = _PyNumber_ConvertIntegralToInt(truncated, "__trunc__ returned non-Integral (type %.200s)");
    if (!res)
        throwCAPIException();
    return res;
}

Box* instanceLong(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* long_str = internStringImmortal("__long__");
    Box* long_func = _instanceGetattribute(inst, long_str, true);
    return runtimeCall(long_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceFloat(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* float_str = internStringImmortal("__float__");
    Box* float_func = _instanceGetattribute(inst, float_str, true);
    return runtimeCall(float_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceOct(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* oct_str = internStringImmortal("__oct__");
    Box* oct_func = _instanceGetattribute(inst, oct_str, true);
    return runtimeCall(oct_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceHex(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* hex_str = internStringImmortal("__hex__");
    Box* hex_func = _instanceGetattribute(inst, hex_str, true);
    return runtimeCall(hex_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceCoerce(Box* _inst, Box* other) {
    static BoxedString* attr_str = internStringImmortal("__coerce__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIndex(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* index_str = internStringImmortal("__index__");
    Box* index_func = _instanceGetattribute(inst, index_str, true);
    return runtimeCall(index_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
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

    classobj_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)classobjNew, UNKNOWN, 4, false, false)));

    classobj_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)classobjCall, UNKNOWN, 1, true, true)));

    classobj_cls->giveAttr("__getattribute__",
                           new BoxedFunction(boxRTFunction((void*)classobjGetattribute, UNKNOWN, 2)));
    classobj_cls->giveAttr("__setattr__", new BoxedFunction(boxRTFunction((void*)classobjSetattr, UNKNOWN, 3)));
    classobj_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)classobjStr, STR, 1)));
    classobj_cls->giveAttr("__dict__", dict_descr);

    classobj_cls->freeze();
    classobj_cls->tp_getattro = classobj_getattro;
    classobj_cls->tp_setattro = classobj_setattro;


    instance_cls->giveAttr("__getattribute__",
                           new BoxedFunction(boxRTFunction((void*)instanceGetattroInternal<CXX>, UNKNOWN, 2)));
    instance_cls->giveAttr("__setattr__", new BoxedFunction(boxRTFunction((void*)instanceSetattr, UNKNOWN, 3)));
    instance_cls->giveAttr("__delattr__", new BoxedFunction(boxRTFunction((void*)instanceDelattr, UNKNOWN, 2)));
    instance_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)instanceStr, UNKNOWN, 1)));
    instance_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)instanceRepr, UNKNOWN, 1)));
    instance_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)instanceNonzero, UNKNOWN, 1)));
    instance_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)instanceLen, UNKNOWN, 1)));
    instance_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)instanceGetitem, UNKNOWN, 2)));
    instance_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)instanceSetitem, UNKNOWN, 3)));
    instance_cls->giveAttr("__delitem__", new BoxedFunction(boxRTFunction((void*)instanceDelitem, UNKNOWN, 2)));
    instance_cls->giveAttr("__getslice__", new BoxedFunction(boxRTFunction((void*)instanceGetslice, UNKNOWN, 3)));
    instance_cls->giveAttr("__setslice__", new BoxedFunction(boxRTFunction((void*)instanceSetslice, UNKNOWN, 4)));
    instance_cls->giveAttr("__delslice__", new BoxedFunction(boxRTFunction((void*)instanceDelslice, UNKNOWN, 3)));
    instance_cls->giveAttr("__cmp__", new BoxedFunction(boxRTFunction((void*)instanceCompare, UNKNOWN, 2)));
    instance_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)instanceContains, UNKNOWN, 2)));
    instance_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)instanceHash, UNKNOWN, 1)));
    instance_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)instanceIter, UNKNOWN, 1)));
    instance_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)instanceNext, UNKNOWN, 1)));
    instance_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)instanceCall, UNKNOWN, 1, true, true)));
    instance_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)instanceEq, UNKNOWN, 2)));
    instance_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)instanceNe, UNKNOWN, 2)));
    instance_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)instanceLt, UNKNOWN, 2)));
    instance_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)instanceLe, UNKNOWN, 2)));
    instance_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)instanceGt, UNKNOWN, 2)));
    instance_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)instanceGe, UNKNOWN, 2)));
    instance_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)instanceAdd, UNKNOWN, 2)));
    instance_cls->giveAttr("__sub__", new BoxedFunction(boxRTFunction((void*)instanceSub, UNKNOWN, 2)));
    instance_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)instanceMul, UNKNOWN, 2)));
    instance_cls->giveAttr("__floordiv__", new BoxedFunction(boxRTFunction((void*)instanceFloordiv, UNKNOWN, 2)));
    instance_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)instanceMod, UNKNOWN, 2)));
    instance_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)instanceDivMod, UNKNOWN, 2)));
    instance_cls->giveAttr("__pow__", new BoxedFunction(boxRTFunction((void*)instancePow, UNKNOWN, 2)));
    instance_cls->giveAttr("__lshift__", new BoxedFunction(boxRTFunction((void*)instanceLshift, UNKNOWN, 2)));
    instance_cls->giveAttr("__rshift__", new BoxedFunction(boxRTFunction((void*)instanceRshift, UNKNOWN, 2)));
    instance_cls->giveAttr("__and__", new BoxedFunction(boxRTFunction((void*)instanceAnd, UNKNOWN, 2)));
    instance_cls->giveAttr("__xor__", new BoxedFunction(boxRTFunction((void*)instanceXor, UNKNOWN, 2)));
    instance_cls->giveAttr("__or__", new BoxedFunction(boxRTFunction((void*)instanceOr, UNKNOWN, 2)));
    instance_cls->giveAttr("__div__", new BoxedFunction(boxRTFunction((void*)instanceDiv, UNKNOWN, 2)));
    instance_cls->giveAttr("__truediv__", new BoxedFunction(boxRTFunction((void*)instanceTruediv, UNKNOWN, 2)));

    instance_cls->giveAttr("__radd__", new BoxedFunction(boxRTFunction((void*)instanceRadd, UNKNOWN, 2)));
    instance_cls->giveAttr("__rsub__", new BoxedFunction(boxRTFunction((void*)instanceRsub, UNKNOWN, 2)));
    instance_cls->giveAttr("__rmul__", new BoxedFunction(boxRTFunction((void*)instanceRmul, UNKNOWN, 2)));
    instance_cls->giveAttr("__rdiv__", new BoxedFunction(boxRTFunction((void*)instanceRdiv, UNKNOWN, 2)));
    instance_cls->giveAttr("__rtruediv__", new BoxedFunction(boxRTFunction((void*)instanceRtruediv, UNKNOWN, 2)));
    instance_cls->giveAttr("__rfloordiv__", new BoxedFunction(boxRTFunction((void*)instanceRfloordiv, UNKNOWN, 2)));
    instance_cls->giveAttr("__rmod__", new BoxedFunction(boxRTFunction((void*)instanceRmod, UNKNOWN, 2)));
    instance_cls->giveAttr("__rdivmod__", new BoxedFunction(boxRTFunction((void*)instanceRdivmod, UNKNOWN, 2)));
    instance_cls->giveAttr("__rpow__", new BoxedFunction(boxRTFunction((void*)instanceRpow, UNKNOWN, 2)));
    instance_cls->giveAttr("__rlshift__", new BoxedFunction(boxRTFunction((void*)instanceRlshift, UNKNOWN, 2)));
    instance_cls->giveAttr("__rrshift__", new BoxedFunction(boxRTFunction((void*)instanceRrshift, UNKNOWN, 2)));
    instance_cls->giveAttr("__rand__", new BoxedFunction(boxRTFunction((void*)instanceRand, UNKNOWN, 2)));
    instance_cls->giveAttr("__rxor__", new BoxedFunction(boxRTFunction((void*)instanceRxor, UNKNOWN, 2)));
    instance_cls->giveAttr("__ror__", new BoxedFunction(boxRTFunction((void*)instanceRor, UNKNOWN, 2)));

    instance_cls->giveAttr("__iadd__", new BoxedFunction(boxRTFunction((void*)instanceIadd, UNKNOWN, 2)));
    instance_cls->giveAttr("__isub__", new BoxedFunction(boxRTFunction((void*)instanceIsub, UNKNOWN, 2)));
    instance_cls->giveAttr("__imul__", new BoxedFunction(boxRTFunction((void*)instanceImul, UNKNOWN, 2)));
    instance_cls->giveAttr("__idiv__", new BoxedFunction(boxRTFunction((void*)instanceIdiv, UNKNOWN, 2)));
    instance_cls->giveAttr("__itruediv__", new BoxedFunction(boxRTFunction((void*)instanceItruediv, UNKNOWN, 2)));
    instance_cls->giveAttr("__ifloordiv__", new BoxedFunction(boxRTFunction((void*)instanceIfloordiv, UNKNOWN, 2)));
    instance_cls->giveAttr("__imod__", new BoxedFunction(boxRTFunction((void*)instanceImod, UNKNOWN, 2)));
    instance_cls->giveAttr("__ipow__", new BoxedFunction(boxRTFunction((void*)instanceIpow, UNKNOWN, 2)));
    instance_cls->giveAttr("__ilshift__", new BoxedFunction(boxRTFunction((void*)instanceIlshift, UNKNOWN, 2)));
    instance_cls->giveAttr("__irshift__", new BoxedFunction(boxRTFunction((void*)instanceIrshift, UNKNOWN, 2)));
    instance_cls->giveAttr("__iand__", new BoxedFunction(boxRTFunction((void*)instanceIand, UNKNOWN, 2)));
    instance_cls->giveAttr("__ixor__", new BoxedFunction(boxRTFunction((void*)instanceIxor, UNKNOWN, 2)));
    instance_cls->giveAttr("__ior__", new BoxedFunction(boxRTFunction((void*)instanceIor, UNKNOWN, 2)));

    instance_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)instanceNeg, UNKNOWN, 1)));
    instance_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)instancePos, UNKNOWN, 1)));
    instance_cls->giveAttr("__abs__", new BoxedFunction(boxRTFunction((void*)instanceAbs, UNKNOWN, 1)));
    instance_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)instanceInvert, UNKNOWN, 1)));
    instance_cls->giveAttr("__int__", new BoxedFunction(boxRTFunction((void*)instanceInt, UNKNOWN, 1)));
    instance_cls->giveAttr("__long__", new BoxedFunction(boxRTFunction((void*)instanceLong, UNKNOWN, 1)));
    instance_cls->giveAttr("__float__", new BoxedFunction(boxRTFunction((void*)instanceFloat, UNKNOWN, 1)));
    instance_cls->giveAttr("__oct__", new BoxedFunction(boxRTFunction((void*)instanceOct, UNKNOWN, 1)));
    instance_cls->giveAttr("__hex__", new BoxedFunction(boxRTFunction((void*)instanceHex, UNKNOWN, 1)));
    instance_cls->giveAttr("__coerce__", new BoxedFunction(boxRTFunction((void*)instanceCoerce, UNKNOWN, 2)));
    instance_cls->giveAttr("__index__", new BoxedFunction(boxRTFunction((void*)instanceIndex, UNKNOWN, 1)));

    instance_cls->freeze();
    instance_cls->tp_getattro = instance_getattro;
    instance_cls->tp_setattro = instance_setattro;
    instance_cls->tp_as_number->nb_index = instance_index;
    instance_cls->tp_as_number->nb_power = instance_pow;
    instance_cls->tp_as_number->nb_inplace_power = instance_ipow;
    instance_cls->tp_dealloc = instance_dealloc;
    instance_cls->has_safe_tp_dealloc = false;
}
}
