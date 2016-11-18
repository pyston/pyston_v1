// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/unwinding.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {
BoxedClass* classobj_cls, *instance_cls;
}

template <Rewritable rewritable>
static BORROWED(Box*) classLookup(BoxedClassobj* cls, BoxedString* attr, GetattrRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    if (rewrite_args)
        assert(!rewrite_args->isSuccessful());

    Box* r = cls->getattr<rewritable>(attr, rewrite_args);
    if (r) {
        if (rewrite_args)
            rewrite_args->assertReturnConvention(ReturnConvention::HAS_RETURN);
        return r;
    }

    if (cls->bases == EmptyTuple) {
        if (rewrite_args && rewrite_args->isSuccessful()) {
            rewrite_args->obj->addAttrGuard(offsetof(BoxedClassobj, bases), (uint64_t)EmptyTuple);
            rewrite_args->assertReturnConvention(ReturnConvention::NO_RETURN);
        }
        return NULL;
    }

    if (rewrite_args) {
        if (rewrite_args->isSuccessful()) {
            rewrite_args->getReturn(); // just to make the asserts happy
            rewrite_args->clearReturn();
        }
        rewrite_args = NULL;
    }

    for (auto b : *cls->bases) {
        RELEASE_ASSERT(b->cls == classobj_cls, "");
        Box* r = classLookup<NOT_REWRITABLE>(static_cast<BoxedClassobj*>(b), attr, rewrite_args);
        if (r)
            return r;
    }

    return NULL;
}

static BORROWED(Box*) classLookup(BoxedClassobj* cls, BoxedString* attr) {
    return classLookup<NOT_REWRITABLE>(cls, attr, NULL);
}

extern "C" BORROWED(PyObject*) _PyInstance_Lookup(PyObject* pinst, PyObject* pname) noexcept {
    RELEASE_ASSERT(PyInstance_Check(pinst), "");
    BoxedInstance* inst = (BoxedInstance*)pinst;

    RELEASE_ASSERT(PyString_Check(pname), "");
    BoxedString* name = (BoxedString*)pname;

    try {
        Py_INCREF(name);
        internStringMortalInplace(name);
        AUTO_DECREF(name);

        Box* v = inst->getattr(name);
        if (v == NULL)
            v = classLookup(inst->inst_cls, name);
        return v;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyInstance_NewRaw(PyObject* klass, PyObject* dict) noexcept {
    RELEASE_ASSERT(!dict, "not implemented");
    if (!PyClass_Check(klass)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return new BoxedInstance((BoxedClassobj*)klass);
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
    made->giveAttr("__doc__", incref(Py_None));

    for (const auto& p : *dict) {
        RELEASE_ASSERT(p.first->cls == str_cls, "");
        BoxedString* s = (BoxedString*)p.first;
        internStringMortalInplace(s);
        made->setattr(s, p.second, NULL);
    }

    return made;
}

Box* classobjCall(Box* _cls, Box* _args, Box* _kwargs) {
    assert(_cls->cls == classobj_cls);
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    assert(_args->cls == tuple_cls);
    BoxedTuple* args = static_cast<BoxedTuple*>(_args);

    assert(!_kwargs || _kwargs->cls == dict_cls);
    BoxedDict* kwargs = static_cast<BoxedDict*>(_kwargs);


    static BoxedString* init_str = getStaticString("__init__");
    Box* init_func = classLookup(cls, init_str);

    BoxedInstance* made = new BoxedInstance(cls);
    AUTO_DECREF(made);
    if (init_func) {
        Box* init_rtn = runtimeCall(init_func, ArgPassSpec(1, 0, true, true), made, args, kwargs, NULL, NULL);
        AUTO_DECREF(init_rtn);
        if (init_rtn != Py_None)
            raiseExcHelper(TypeError, "__init__() should return None");
    } else {
        if (args->size() || (kwargs && kwargs->d.size())) {
            raiseExcHelper(TypeError, "this constructor takes no arguments");
        }
    }
    return incref(made);
}

extern "C" PyObject* PyInstance_New(PyObject* klass, PyObject* arg, PyObject* kw) noexcept {
    try {
        return classobjCall(klass, arg, kw);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" BORROWED(PyObject*) PyInstance_Class(PyObject* _inst) noexcept {
    RELEASE_ASSERT(PyInstance_Check(_inst), "");
    BoxedInstance* inst = (BoxedInstance*)_inst;
    return inst->inst_cls;
}

static Box* classobjGetattribute(Box* _cls, Box* _attr) {
    RELEASE_ASSERT(_cls->cls == classobj_cls, "");
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // These are special cases in CPython as well:
    if (attr->data()[0] == '_' && attr->data()[1] == '_') {
        if (attr->s() == "__dict__")
            return incref(cls->getAttrWrapper());

        if (attr->s() == "__bases__")
            return incref(cls->bases);

        if (attr->s() == "__name__") {
            if (cls->name)
                return incref(cls->name);
            return incref(Py_None);
        }
    }

    Box* r = classLookup(cls, attr);
    if (!r)
        raiseExcHelper(AttributeError, "class %s has no attribute '%s'", cls->name->data(), attr->data());

    r = processDescriptor(r, Py_None, cls);
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

static const char* set_bases(BoxedClassobj* c, PyObject* v) {
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
    auto old_bases = c->bases;
    ((BoxedClassobj*)c)->bases = (BoxedTuple*)incref(v);
    Py_DECREF(old_bases);
    return "";
}

static void _classobjSetattr(Box* _cls, Box* _attr, Box* _value) {
    RELEASE_ASSERT(_cls->cls == classobj_cls, "");
    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_cls);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    if (attr->s() == "__bases__") {
        RELEASE_ASSERT(_value, "can't delete __bases__");

        const char* error_str = set_bases(cls, _value);
        if (error_str && error_str[0] != '\0')
            raiseExcHelper(TypeError, "%s", error_str);
        static BoxedString* bases_str = getStaticString("__bases__");
        cls->setattr(bases_str, _value, NULL);
        return;
    }
    int r = PyObject_GenericSetAttr(cls, _attr, _value);
    if (r)
        throwCAPIException();
}

static Box* classobjSetattr(Box* _cls, Box* _attr, Box* _value) {
    assert(_value);

    _classobjSetattr(_cls, _attr, _value);
    return incref(Py_None);
}

static Box* classobjDelattr(Box* _cls, Box* _attr) {
    _classobjSetattr(_cls, _attr, NULL);
    return incref(Py_None);
}

static int classobj_setattro(Box* cls, Box* attr, Box* value) noexcept {
    try {
        _classobjSetattr(cls, attr, value);
        return 0;
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

    static BoxedString* module_str = getStaticString("__module__");
    Box* _mod = cls->getattr(module_str);
    RELEASE_ASSERT(_mod, "");
    RELEASE_ASSERT(_mod->cls == str_cls, "");
    return boxStringTwine(llvm::Twine(static_cast<BoxedString*>(_mod)->s()) + "." + cls->name->s());
}

Box* classobjRepr(Box* _obj) {
    if (!isSubclass(_obj->cls, classobj_cls)) {
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'classobj' object but received an '%s'",
                       getTypeName(_obj));
    }

    BoxedClassobj* cls = static_cast<BoxedClassobj*>(_obj);

    static BoxedString* module_str = getStaticString("__module__");
    Box* mod = cls->getattr(module_str);

    const char* name;
    if (cls->name == NULL || !PyString_Check(cls->name))
        name = "?";
    else
        name = PyString_AsString(cls->name);
    if (mod == NULL || !PyString_Check(mod))
        return PyString_FromFormat("<class ?.%s at %p>", name, cls);
    else
        return PyString_FromFormat("<class %s.%s at %p>", PyString_AsString(mod), name, cls);
}

// Analogous to CPython's instance_getattr2
template <Rewritable rewritable>
static Box* instanceGetattributeSimple(BoxedInstance* inst, BoxedString* attr_str,
                                       GetattrRewriteArgs* rewrite_args = NULL) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    Box* r = inst->getattr<rewritable>(attr_str, rewrite_args);
    if (r) {
        if (rewrite_args)
            rewrite_args->assertReturnConvention(ReturnConvention::HAS_RETURN);
        return incref(r);
    }

    RewriterVar* r_inst = NULL;
    RewriterVar* r_inst_cls = NULL;
    if (rewrite_args) {
        if (!rewrite_args->isSuccessful())
            rewrite_args = NULL;
        else {
            rewrite_args->assertReturnConvention(ReturnConvention::NO_RETURN);
            rewrite_args->clearReturn();

            r_inst = rewrite_args->obj;
            r_inst_cls = r_inst->getAttr(offsetof(BoxedInstance, inst_cls));
        }
    }
    GetattrRewriteArgs grewriter_inst_args(rewrite_args ? rewrite_args->rewriter : NULL, r_inst_cls,
                                           rewrite_args ? rewrite_args->rewriter->getReturnDestination() : Location());
    r = classLookup<rewritable>(inst->inst_cls, attr_str, rewrite_args ? &grewriter_inst_args : NULL);
    if (!grewriter_inst_args.isSuccessful())
        rewrite_args = NULL;

    if (r) {
        Box* rtn = processDescriptor(r, inst, inst->inst_cls);
        if (rewrite_args) {
            RewriterVar* r_rtn
                = rewrite_args->rewriter->call(true, (void*)processDescriptor,
                                               grewriter_inst_args.getReturn(ReturnConvention::HAS_RETURN), r_inst,
                                               r_inst_cls)->setType(RefType::OWNED);
            rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
        }
        return rtn;
    }

    if (rewrite_args)
        grewriter_inst_args.assertReturnConvention(ReturnConvention::NO_RETURN);

    return NULL;
}

template <Rewritable rewritable>
static Box* instanceGetattributeWithFallback(BoxedInstance* inst, BoxedString* attr_str,
                                             GetattrRewriteArgs* rewrite_args = NULL) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    Box* attr_obj = instanceGetattributeSimple<rewritable>(inst, attr_str, rewrite_args);

    if (attr_obj) {
        if (rewrite_args && rewrite_args->isSuccessful())
            rewrite_args->assertReturnConvention(
                ReturnConvention::HAS_RETURN); // otherwise need to guard on the success
        return attr_obj;
    }

    if (rewrite_args) {
        if (!rewrite_args->isSuccessful())
            rewrite_args = NULL;
        else {
            rewrite_args->assertReturnConvention(ReturnConvention::NO_RETURN);
            rewrite_args->clearReturn();
        }

        // abort rewriting for now
        rewrite_args = NULL;
    }

    static BoxedString* getattr_str = getStaticString("__getattr__");
    Box* getattr = classLookup(inst->inst_cls, getattr_str);

    if (getattr) {
        getattr = processDescriptor(getattr, inst, inst->inst_cls);
        AUTO_DECREF(getattr);
        return runtimeCallInternal<CXX, NOT_REWRITABLE>(getattr, NULL, ArgPassSpec(1), attr_str, NULL, NULL, NULL,
                                                        NULL);
    }

    return NULL;
}

template <Rewritable rewritable>
static Box* _instanceGetattribute(Box* _inst, BoxedString* attr_str, bool raise_on_missing,
                                  GetattrRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    // These are special cases in CPython as well:
    if (attr_str->data()[0] == '_' && attr_str->data()[1] == '_') {
        if (attr_str->s() == "__dict__")
            return incref(inst->getAttrWrapper());

        if (attr_str->s() == "__class__")
            return incref(inst->inst_cls);
    }

    try {
        Box* attr = instanceGetattributeWithFallback<rewritable>(inst, attr_str, rewrite_args);
        if (attr)
            return attr;
    } catch (ExcInfo e) {
        if (!raise_on_missing && e.matches(AttributeError)) {
            e.clear();
            return NULL;
        }
        throw e;
    }
    if (!raise_on_missing) {
        return NULL;
    } else {
        raiseExcHelper(AttributeError, "%s instance has no attribute '%s'", inst->inst_cls->name->data(),
                       attr_str->data());
    }
}
static Box* _instanceGetattribute(Box* _inst, BoxedString* attr_str, bool raise_on_missing) {
    return _instanceGetattribute<NOT_REWRITABLE>(_inst, attr_str, raise_on_missing, NULL);
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
            return _instanceGetattribute<REWRITABLE>(cls, attr, true, rewrite_args);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    } else {
        return _instanceGetattribute<REWRITABLE>(cls, attr, true, rewrite_args);
    }
}

// Force instantiation of the template
template Box* instanceGetattroInternal<CAPI>(Box*, Box*, GetattrRewriteArgs*) noexcept;
template Box* instanceGetattroInternal<CXX>(Box*, Box*, GetattrRewriteArgs*);

void instanceSetattroInternal(Box* _inst, Box* _attr, STOLEN(Box*) value, SetattrRewriteArgs* rewrite_args) {
    STAT_TIMER(t0, "us_timer_instance_setattro", 0);

    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    assert(value);

    // These are special cases in CPython as well:
    if (attr->data()[0] == '_' && attr->data()[1] == '_') {
        if (attr->s() == "__dict__")
            Py_FatalError("unimplemented");

        if (attr->s() == "__class__") {
            if (value->cls != classobj_cls) {
                Py_DECREF(value);
                raiseExcHelper(TypeError, "__class__ must be set to a class");
            }

            auto old_cls = inst->inst_cls;
            inst->inst_cls = static_cast<BoxedClassobj*>(value);
            Py_DECREF(old_cls);
            return;
        }
    }

    AUTO_DECREF(value);

    static BoxedString* setattr_str = getStaticString("__setattr__");

    if (rewrite_args) {
        RewriterVar* inst_r = rewrite_args->obj->getAttr(offsetof(BoxedInstance, inst_cls));
        inst_r->addGuard((uint64_t)inst->inst_cls);
        rewrite_args->rewriter->addGCReference(inst->inst_cls);
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, inst_r,
                                         rewrite_args->rewriter->getReturnDestination());
        Box* setattr = classLookup<REWRITABLE>(inst->inst_cls, setattr_str, &grewrite_args);

        if (!grewrite_args.isSuccessful()) {
            assert(!rewrite_args->out_success);
            rewrite_args = NULL;
        }

        if (setattr) {
            setattr = processDescriptor(setattr, inst, inst->inst_cls);
            AUTO_DECREF(setattr);
            autoDecref(runtimeCall(setattr, ArgPassSpec(2), _attr, value, NULL, NULL, NULL));
            return;
        }

        if (rewrite_args)
            grewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);

        _inst->setattr(attr, value, rewrite_args);
        return;
    } else {
        Box* setattr = classLookup(inst->inst_cls, setattr_str);
        if (setattr) {
            setattr = processDescriptor(setattr, inst, inst->inst_cls);
            AUTO_DECREF(setattr);
            autoDecref(runtimeCall(setattr, ArgPassSpec(2), _attr, value, NULL, NULL, NULL));
            return;
        }

        _inst->setattr(attr, value, NULL);
        return;
    }
}

Box* instanceDelattr(Box* _inst, Box* _attr) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    RELEASE_ASSERT(_attr->cls == str_cls, "");
    BoxedString* attr = static_cast<BoxedString*>(_attr);

    // These are special cases in CPython as well:
    if (attr->data()[0] == '_' && attr->data()[1] == '_') {
        if (attr->s() == "__dict__")
            raiseExcHelper(TypeError, "__dict__ must be set to a dictionary");

        if (attr->s() == "__class__")
            raiseExcHelper(TypeError, "__class__ must be set to a class");
    }

    static BoxedString* delattr_str = getStaticString("__delattr__");
    Box* delattr = classLookup(inst->inst_cls, delattr_str);

    if (delattr) {
        delattr = processDescriptor(delattr, inst, inst->inst_cls);
        AUTO_DECREF(delattr);
        return runtimeCall(delattr, ArgPassSpec(1), _attr, NULL, NULL, NULL, NULL);
    }

    if (_inst->hasattr(attr))
        _inst->delattr(attr, NULL);
    else {
        BoxedClassobj* clsobj = (BoxedClassobj*)inst->inst_cls;
        RELEASE_ASSERT(PyClass_Check(clsobj), "");
        raiseExcHelper(AttributeError, "%.50s instance has no attribute '%.400s'", clsobj->name->c_str(),
                       attr->c_str());
    }
    return incref(Py_None);
}

int instance_setattro(Box* inst, Box* attr, Box* value) noexcept {
    try {
        if (value) {
            instanceSetattroInternal(inst, attr, value, NULL);
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

    static BoxedString* repr_str = getStaticString("__repr__");
    Box* repr_func = _instanceGetattribute(inst, repr_str, false);

    if (repr_func) {
        AUTO_DECREF(repr_func);
        return runtimeCall(repr_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        Box* class_str = classobjStr(inst->inst_cls);
        assert(class_str->cls == str_cls);
        AUTO_DECREF(class_str);

        char buf[80];
        snprintf(buf, 80, "<%s instance at %p>", static_cast<BoxedString*>(class_str)->data(), inst);
        return boxString(buf);
    }
}

Box* instanceStr(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* str_str = getStaticString("__str__");
    Box* str_func = _instanceGetattribute(inst, str_str, false);

    if (str_func) {
        AUTO_DECREF(str_func);
        return runtimeCall(str_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        return instanceRepr(inst);
    }
}

Box* instanceNonzero(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* nonzero_str = getStaticString("__nonzero__");

    Box* nonzero_func = NULL;
    try {
        nonzero_func = _instanceGetattribute(inst, nonzero_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
        e.clear();
    }

    if (nonzero_func == NULL) {
        static BoxedString* len_str = getStaticString("__len__");
        try {
            nonzero_func = _instanceGetattribute(inst, len_str, false);
        } catch (ExcInfo e) {
            if (!e.matches(AttributeError))
                throw e;
            e.clear();
        }
    }

    if (nonzero_func) {
        AUTO_DECREF(nonzero_func);
        return runtimeCall(nonzero_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    } else {
        Py_RETURN_TRUE;
    }
}

Box* instanceLen(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* len_str = getStaticString("__len__");
    Box* len_func = _instanceGetattribute(inst, len_str, true);
    AUTO_DECREF(len_func);
    return runtimeCall(len_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

// Analoguous to CPython's, used for sq_slots, mp_slots
static Py_ssize_t instance_length(PyObject* self) noexcept {
    PyObject* func;
    PyObject* res;
    Py_ssize_t outcome;

    static BoxedString* len_str = getStaticString("__len__");

    func = instance_getattro(self, len_str);
    if (func == NULL)
        return -1;

    res = runtimeCallCapi(func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    Py_DECREF(func);

    if (res == NULL)
        return -1;

    if (PyInt_Check(res)) {
        outcome = PyInt_AsSsize_t(res);
        if (outcome == -1 && PyErr_Occurred()) {
            Py_DECREF(res);
            return -1;
        }
#if SIZEOF_SIZE_T < SIZEOF_INT
        /* Overflow check -- range of PyInt is more than C int */
        if (outcome != (int)outcome) {
            PyErr_SetString(PyExc_OverflowError, "__len__() should return 0 <= outcome < 2**31");
            outcome = -1;
        } else
#endif
            if (outcome < 0) {
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
            outcome = -1;
        }

    } else {
        PyErr_SetString(PyExc_TypeError, "__len__() should return an int");
        outcome = -1;
    }
    Py_DECREF(res);
    return outcome;
}


// Analoguous to CPython's, used for mp_slots.
static int instance_ass_sub(Box* inst, Box* key, Box* value) noexcept {
    Box* func;
    Box* res;

    static BoxedString* setitem_str = getStaticString("__setitem__");
    static BoxedString* delitem_str = getStaticString("__delitem__");

    if (value == NULL) {
        func = instance_getattro(inst, delitem_str);
    } else {
        func = instance_getattro(inst, setitem_str);
    }

    if (func == NULL) {
        return -1;
    }

    if (value == NULL) {
        res = runtimeCallCapi(func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
    } else {
        res = runtimeCallCapi(func, ArgPassSpec(2), key, value, NULL, NULL, NULL);
    }

    Py_DECREF(func);
    if (res == NULL)
        return -1;

    Py_DECREF(res);
    return 0;
}

template <enum ExceptionStyle S> Box* instanceGetitem(Box* _inst, Box* key) noexcept(S == CAPI) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* getitem_str = getStaticString("__getitem__");

    Box* getitem_func;

    if (S == CAPI) {
        getitem_func = instance_getattro(inst, getitem_str);
        if (getitem_func == NULL) {
            return NULL;
        }
    } else {
        getitem_func = _instanceGetattribute(inst, getitem_str, true);
    }

    AUTO_DECREF(getitem_func);

    return runtimeCallInternal<S, NOT_REWRITABLE>(getitem_func, NULL, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
}

Box* instance_item(Box* self, Py_ssize_t i) noexcept {
    return instanceGetitem<CAPI>(self, autoDecref(boxInt(i)));
}

Box* instanceSetitem(Box* _inst, Box* key, Box* value) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* setitem_str = getStaticString("__setitem__");
    Box* setitem_func = _instanceGetattribute(inst, setitem_str, true);
    AUTO_DECREF(setitem_func);
    return runtimeCall(setitem_func, ArgPassSpec(2), key, value, NULL, NULL, NULL);
}

static int instance_ass_item(Box* inst, Py_ssize_t k, PyObject* item) noexcept {
    static BoxedString* delitem_str, *setitem_str;
    Box* func = NULL, * res = NULL;
    Box* key = boxInt(k);

    AUTO_DECREF(key);

    if (item == NULL) {
        delitem_str = getStaticString("__delitem__");

        func = instance_getattro(inst, delitem_str);
        if (func == NULL)
            return -1;

        res = runtimeCallCapi(func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
    } else {
        setitem_str = getStaticString("__setitem__");

        func = instance_getattro(inst, setitem_str);
        if (func == NULL)
            return -1;

        res = runtimeCallCapi(func, ArgPassSpec(2), key, (Box*)item, NULL, NULL, NULL);
    }

    Py_DECREF(func);

    if (res == NULL)
        return -1;

    Py_DECREF(res);
    return 0;
}

Box* instanceDelitem(Box* _inst, Box* key) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* delitem_str = getStaticString("__delitem__");
    Box* delitem_func = _instanceGetattribute(inst, delitem_str, true);
    AUTO_DECREF(delitem_func);
    return runtimeCall(delitem_func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
}

Box* instanceGetslice(Box* _inst, Box* i, Box* j) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* getslice_str = getStaticString("__getslice__");
    Box* getslice_func = NULL;

    try {
        getslice_func = _instanceGetattribute(inst, getslice_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
        e.clear();
    }

    if (getslice_func == NULL) {
        Box* slice = static_cast<Box*>(createSlice(i, j, Py_None));
        AUTO_DECREF(slice);
        return instanceGetitem<CXX>(inst, slice);
    }
    AUTO_DECREF(getslice_func);
    return runtimeCall(getslice_func, ArgPassSpec(2), i, j, NULL, NULL, NULL);
}

Box* instance_slice(PyObject* self, Py_ssize_t i, Py_ssize_t j) noexcept {
    try {
        return instanceGetslice(self, autoDecref(boxInt(i)), autoDecref((boxInt(j))));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return 0;
    }
}

Box* instanceSetslice(Box* _inst, Box* i, Box* j, Box** sequence) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* setslice_str = getStaticString("__setslice__");
    Box* setslice_func = NULL;

    try {
        setslice_func = _instanceGetattribute(inst, setslice_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
        e.clear();
    }

    if (setslice_func == NULL) {
        Box* slice = static_cast<Box*>(createSlice(i, j, Py_None));
        AUTO_DECREF(slice);
        return instanceSetitem(inst, slice, *sequence);
    }

    AUTO_DECREF(setslice_func);
    return runtimeCall(setslice_func, ArgPassSpec(3), i, j, *sequence, NULL, NULL);
}

Box* instanceDelslice(Box* _inst, Box* i, Box* j) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* delslice_str = getStaticString("__delslice__");
    Box* delslice_func = NULL;

    try {
        delslice_func = _instanceGetattribute(inst, delslice_str, false);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
        e.clear();
    }

    if (delslice_func == NULL) {
        Box* slice = static_cast<Box*>(createSlice(i, j, Py_None));
        AUTO_DECREF(slice);
        return instanceDelitem(inst, slice);
    }
    try {
        AUTO_DECREF(delslice_func);
        return runtimeCall(delslice_func, ArgPassSpec(2), i, j, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

// Analoguous to CPython's, used for sq_slots.
static int instance_ass_slice(Box* inst, Py_ssize_t i, Py_ssize_t j, PyObject* value) {
    static BoxedString* delslice_str, *setslice_str, *delitem_str, *setitem_str;
    Box* func = NULL, * res = NULL;
    Box* slice = NULL;
    Box* begin = boxInt(i);
    Box* end = boxInt(j);

    AUTO_DECREF(begin);
    AUTO_DECREF(end);

    if (value == NULL) {
        delslice_str = getStaticString("__delslice__");
        func = instance_getattro(inst, delslice_str);
        if (func == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return -1;
            PyErr_Clear();

            delitem_str = getStaticString("__delitem__");
            func = instance_getattro(inst, delitem_str);
            if (func == NULL)
                return -1;

            slice = static_cast<Box*>(createSlice(begin, end, Py_None));
            AUTO_DECREF(slice);
            res = runtimeCallCapi(func, ArgPassSpec(1), slice, NULL, NULL, NULL, NULL);
        } else {
            if (PyErr_WarnPy3k("in 3.x, __delslice__ has been removed; use __delitem__", 1) < 0) {
                Py_DECREF(func);
                return -1;
            }
            res = runtimeCallCapi(func, ArgPassSpec(2), begin, end, NULL, NULL, NULL);
        }
    } else {
        setslice_str = getStaticString("__setslice__");
        func = instance_getattro(inst, setslice_str);
        if (func == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return -1;
            PyErr_Clear();

            setitem_str = getStaticString("__setitem__");
            func = instance_getattro(inst, setitem_str);
            if (func == NULL)
                return -1;

            slice = static_cast<Box*>(createSlice(begin, end, Py_None));
            AUTO_DECREF(slice);
            res = runtimeCallCapi(func, ArgPassSpec(2), slice, (Box*)value, NULL, NULL, NULL);
        } else {
            if (PyErr_WarnPy3k("in 3.x, __setslice__ has been removed; use __setitem__", 1) < 0) {
                Py_DECREF(func);
                return -1;
            }
            res = runtimeCallCapi(func, ArgPassSpec(3), begin, end, (Box*)value, NULL, NULL);
        }
    }

    Py_DECREF(func);
    if (res == NULL)
        return -1;

    Py_DECREF(res);
    return 0;
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

    static BoxedString* cmp_str = getStaticString("__cmp__");
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

    AUTO_DECREF(cmp_func);

    args = PyTuple_Pack(1, w);
    if (args == NULL) {
        return -2;
    }

    result = PyEval_CallObject(cmp_func, args);
    Py_DECREF(args);

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
        return incref(NotImplemented);
    if (rtn == -2)
        throwCAPIException();
    return boxInt(rtn);
}

Box* instanceContains(Box* _inst, Box* key) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* contains_str = getStaticString("__contains__");
    Box* contains_func = _instanceGetattribute(inst, contains_str, false);

    if (!contains_func) {
        int result = _PySequence_IterSearch(inst, key, PY_ITERSEARCH_CONTAINS);
        if (result < 0)
            throwCAPIException();
        assert(result == 0 || result == 1);
        return boxBool(result);
    }

    AUTO_DECREF(contains_func);

    Box* r = runtimeCall(contains_func, ArgPassSpec(1), key, NULL, NULL, NULL, NULL);
    AUTO_DECREF(r);
    return boxBool(nonzero(r));
}

static int instance_contains(Box* inst, Box* key) noexcept {
    try {
        Box* res = instanceContains(inst, key);
        AUTO_DECREF(res);
        return nonzero(res);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

static Box* instanceHash(BoxedInstance* inst) {
    assert(inst->cls == instance_cls);

    PyObject* func;
    PyObject* res;

    static BoxedString* hash_str = getStaticString("__hash__");
    static BoxedString* eq_str = getStaticString("__eq__");
    static BoxedString* cmp_str = getStaticString("__cmp__");

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
            } else {
                Py_DECREF(func);
            }
        } else {
            Py_DECREF(func);
        }
        raiseExcHelper(TypeError, "unhashable instance");
    }
    AUTO_DECREF(func);

    res = runtimeCall(func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    AUTO_DECREF(res);
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

    static BoxedString* iter_str = getStaticString("__iter__");
    static BoxedString* getitem_str = getStaticString("__getitem__");
    if ((func = _instanceGetattribute(self, iter_str, false)) != NULL) {
        AUTO_DECREF(func);
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
    Py_DECREF(func);

    Box* r = PySeqIter_New((PyObject*)self);
    if (!r)
        throwCAPIException();
    return r;
}

static Box* instanceNext(BoxedInstance* inst) {
    assert(inst->cls == instance_cls);

    static BoxedString* next_str = getStaticString("next");
    Box* next_func = _instanceGetattribute(inst, next_str, false);

    if (!next_func) {
        // not 100% sure why this is a different error:
        raiseExcHelper(TypeError, "instance has no next() method");
    }
    AUTO_DECREF(next_func);

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
        coerce_obj = getStaticString("__coerce__");
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
    static BoxedString* pow_str = getStaticString("__pow__");
    static BoxedString* ipow_str = getStaticString("__ipow__");
    static BoxedString* rpow_str = getStaticString("__rpow__");
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
    static BoxedString* pow_str = getStaticString("__pow__");
    static BoxedString* ipow_str = getStaticString("__ipow__");
    static BoxedString* rpow_str = getStaticString("__rpow__");
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

    static BoxedString* index_str = getStaticString("__index__");
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

static Box* _instanceBinary(Box* _inst, Box* other, BoxedString* attr) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    Box* func = _instanceGetattribute(inst, attr, false);
    if (!func)
        return incref(NotImplemented);
    AUTO_DECREF(func);
    return runtimeCall(func, ArgPassSpec(1), other, NULL, NULL, NULL, NULL);
}

Box* instanceGt(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__gt__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceGe(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ge__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceLt(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__lt__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceLe(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__le__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceEq(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__eq__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceNe(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ne__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceAdd(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__add__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceSub(Box* _inst, Box* other) {

    static BoxedString* attr_str = getStaticString("__sub__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceMul(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__mul__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceFloordiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__floordiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceMod(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__mod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceDivMod(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__divmod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instancePow(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__pow__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceLshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__lshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceAnd(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__and__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceXor(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__xor__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceOr(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__or__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceDiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__div__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceTruediv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__truediv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRadd(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__radd__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRsub(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rsub__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRmul(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rmul__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRdiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rdiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRtruediv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rtruediv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRfloordiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rfloordiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRmod(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rmod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRdivmod(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rdivmod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRpow(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rpow__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRlshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rlshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRrshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rrshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRand(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rand__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRxor(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__rxor__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceRor(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ror__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIadd(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__iadd__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIsub(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__isub__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceImul(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__imul__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIdiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__idiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceItruediv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__itruediv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIfloordiv(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ifloordiv__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceImod(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__imod__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIpow(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ipow__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIlshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ilshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIrshift(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__irshift__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIand(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__iand__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIxor(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ixor__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIor(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__ior__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceNeg(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* neg_str = getStaticString("__neg__");
    Box* neg_func = _instanceGetattribute(inst, neg_str, true);
    AUTO_DECREF(neg_func);
    return runtimeCall(neg_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instancePos(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* pos_str = getStaticString("__pos__");
    Box* pos_func = _instanceGetattribute(inst, pos_str, true);
    AUTO_DECREF(pos_func);
    return runtimeCall(pos_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceAbs(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* abs_str = getStaticString("__abs__");
    Box* abs_func = _instanceGetattribute(inst, abs_str, true);
    AUTO_DECREF(abs_func);
    return runtimeCall(abs_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceInvert(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* invert_str = getStaticString("__invert__");
    Box* invert_func = _instanceGetattribute(inst, invert_str, true);
    AUTO_DECREF(invert_func);
    return runtimeCall(invert_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceTrunc(BoxedInstance* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* trunc_str = getStaticString("__trunc__");
    Box* trunc_func = _instanceGetattribute(inst, trunc_str, true);
    AUTO_DECREF(trunc_func);

    return runtimeCall(trunc_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceInt(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* int_str = getStaticString("__int__");
    if (PyObject_HasAttr((PyObject*)inst, int_str)) {
        Box* int_func = _instanceGetattribute(inst, int_str, true);
        AUTO_DECREF(int_func);
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

    static BoxedString* long_str = getStaticString("__long__");
    if (PyObject_HasAttr((PyObject*)inst, long_str)) {
        Box* long_func = _instanceGetattribute(inst, long_str, true);
        AUTO_DECREF(long_func);
        return runtimeCall(long_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
    }

    Box* res = instanceInt(inst);
    return res;
}

Box* instanceFloat(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* float_str = getStaticString("__float__");
    Box* float_func = _instanceGetattribute(inst, float_str, true);
    AUTO_DECREF(float_func);
    return runtimeCall(float_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceOct(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* oct_str = getStaticString("__oct__");
    Box* oct_func = _instanceGetattribute(inst, oct_str, true);
    AUTO_DECREF(oct_func);
    return runtimeCall(oct_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceHex(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* hex_str = getStaticString("__hex__");
    Box* hex_func = _instanceGetattribute(inst, hex_str, true);
    AUTO_DECREF(hex_func);
    return runtimeCall(hex_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceCoerce(Box* _inst, Box* other) {
    static BoxedString* attr_str = getStaticString("__coerce__");
    return _instanceBinary(_inst, other, attr_str);
}

Box* instanceIndex(Box* _inst) {
    RELEASE_ASSERT(_inst->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* index_str = getStaticString("__index__");
    Box* index_func = _instanceGetattribute(inst, index_str, true);
    AUTO_DECREF(index_func);
    return runtimeCall(index_func, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
}

Box* instanceCall(Box* _inst, Box* _args, Box* _kwargs) {
    assert(_inst->cls == instance_cls);
    BoxedInstance* inst = static_cast<BoxedInstance*>(_inst);

    static BoxedString* call_str = getStaticString("__call__");
    Box* call_func = _instanceGetattribute(inst, call_str, false);
    if (!call_func)
        raiseExcHelper(AttributeError, "%s instance has no __call__ method", inst->inst_cls->name->data());

    AUTO_DECREF(call_func);
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

extern "C" BORROWED(PyObject*) PyClass_Name(PyObject* _classobj) noexcept {
    RELEASE_ASSERT(PyClass_Check(_classobj), "");
    BoxedClassobj* classobj = (BoxedClassobj*)_classobj;
    return classobj->name;
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

extern "C" int PyMethod_SetSelf(PyObject* im, PyObject* instance) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return 0;
    }

    Box* old = ((BoxedInstanceMethod*)im)->obj;
    Py_INCREF(instance);
    ((BoxedInstanceMethod*)im)->obj = instance;
    Py_XDECREF(old);
    return 1;
}

extern "C" PyObject* PyMethod_Class(PyObject* im) noexcept {
    if (!PyMethod_Check(im)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((BoxedInstanceMethod*)im)->im_class;
}

extern "C" int PyMethod_ClearFreeList(void) noexcept {
    return 0; // number of entries cleared
}

void BoxedInstance::dealloc(Box* b) noexcept {
    RELEASE_ASSERT(b->cls == instance_cls, "");
    BoxedInstance* inst = static_cast<BoxedInstance*>(b);

    PyObject* error_type, *error_value, *error_traceback;
    PyObject* del;
    static PyObject* delstr;

    _PyObject_GC_UNTRACK(inst);
    if (inst->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject*)inst);

    /* Temporarily resurrect the object. */
    assert(inst->cls == &PyInstance_Type);
    assert(inst->ob_refcnt == 0);
    inst->ob_refcnt = 1;

    /* Save the current exception, if any. */
    PyErr_Fetch(&error_type, &error_value, &error_traceback);
    /* Execute __del__ method, if any. */
    if (delstr == NULL) {
        delstr = getStaticString("__del__");
        if (delstr == NULL)
            PyErr_WriteUnraisable((PyObject*)inst);
    }
    // if (delstr && (del = instance_getattr2(inst, delstr)) != NULL) {
    // TODO: not sure if this is the same as cpython's getattr2 (and the exception style might be different too?)
    if (delstr
        && (del = instanceGetattributeSimple<NOT_REWRITABLE>(inst, static_cast<BoxedString*>(delstr), NULL)) != NULL) {
        PyObject* res = PyEval_CallObject(del, (PyObject*)NULL);
        if (res == NULL)
            PyErr_WriteUnraisable(del);
        else
            Py_DECREF(res);
        Py_DECREF(del);
    }
    /* Restore the saved exception. */
    PyErr_Restore(error_type, error_value, error_traceback);

    /* Undo the temporary resurrection; can't use DECREF here, it would
     * cause a recursive call.
     */
    assert(inst->ob_refcnt > 0);
    if (--inst->ob_refcnt == 0) {

        /* New weakrefs could be created during the finalizer call.
            If this occurs, clear them out without calling their
            finalizers since they might rely on part of the object
            being finalized that has already been destroyed. */
        while (inst->weakreflist != NULL) {
            _PyWeakref_ClearRef((PyWeakReference*)(inst->weakreflist));
        }

        Py_DECREF(inst->inst_cls);
        inst->attrs.clearForDealloc();
        PyObject_GC_Del(inst);
    } else {
        Py_ssize_t refcnt = inst->ob_refcnt;
        /* __del__ resurrected it!  Make it look like the original
         * Py_DECREF never happened.
         */
        _Py_NewReference((PyObject*)inst);
        inst->ob_refcnt = refcnt;
        _PyObject_GC_TRACK(inst);
        /* If Py_REF_DEBUG, _Py_NewReference bumped _Py_RefTotal, so
         * we need to undo that. */
        _Py_DEC_REFTOTAL;
/* If Py_TRACE_REFS, _Py_NewReference re-added self to the
 * object chain, so no more to do there.
 * If COUNT_ALLOCS, the original decref bumped tp_frees, and
 * _Py_NewReference bumped tp_allocs: both of those need to be
 * undone.
 */
#ifdef COUNT_ALLOCS
        --inst->cls->tp_frees;
        --inst->cls->tp_allocs;
#endif
    }
}

int BoxedInstance::traverse(Box* o, visitproc visit, void* arg) noexcept {
    BoxedInstance* self = static_cast<BoxedInstance*>(o);

    Py_TRAVERSE(self->attrs);
    Py_VISIT(self->inst_cls);

    return 0;
}

int BoxedInstance::clear(Box* o) noexcept {
    BoxedInstance* self = static_cast<BoxedInstance*>(o);

    self->attrs.clearForDealloc();

    // I think it is ok to not clear this:
    // Py_CLEAR(self->inst_cls);

    return 0;
}

void BoxedClassobj::dealloc(Box* b) noexcept {
    _PyObject_GC_UNTRACK(b);

    BoxedClassobj* cl = static_cast<BoxedClassobj*>(b);

    if (cl->weakreflist)
        PyObject_ClearWeakRefs(cl);

    cl->clearAttrsForDealloc();

    Py_DECREF(cl->bases);
    Py_DECREF(cl->name);

    cl->cls->tp_free(cl);
}

int BoxedClassobj::traverse(Box* o, visitproc visit, void* arg) noexcept {
    BoxedClassobj* cl = static_cast<BoxedClassobj*>(o);

    Py_VISIT(cl->bases);
    Py_VISIT(cl->name);
    Py_TRAVERSE(cl->attrs);
    return 0;
}

int BoxedClassobj::clear(Box* self) noexcept {
    BoxedClassobj* cl = static_cast<BoxedClassobj*>(self);

    cl->attrs.clearForDealloc();

    // I think it is ok to not clear these:
    // Py_CLEAR(cl->bases);
    // Py_CLEAR(cl->name);

    return 0;
}

void setupClassobj() {
    classobj_cls
        = BoxedClass::create(type_cls, object_cls, offsetof(BoxedClassobj, attrs), offsetof(BoxedClassobj, weakreflist),
                             sizeof(BoxedClassobj), false, "classobj", false, (destructor)BoxedClassobj::dealloc, NULL,
                             true, (traverseproc)BoxedClassobj::traverse, (inquiry)BoxedClassobj::clear);
    instance_cls
        = BoxedClass::create(type_cls, object_cls, offsetof(BoxedInstance, attrs), offsetof(BoxedInstance, weakreflist),
                             sizeof(BoxedInstance), false, "instance", false, (destructor)BoxedInstance::dealloc, NULL,
                             true, (traverseproc)BoxedInstance::traverse, (inquiry)BoxedInstance::clear);

    classobj_cls->giveAttr("__new__", new BoxedFunction(BoxedCode::create((void*)classobjNew, UNKNOWN, 4, false, false,
                                                                          "classobj.__new__")));

    classobj_cls->giveAttr("__call__", new BoxedFunction(BoxedCode::create((void*)classobjCall, UNKNOWN, 1, true, true,
                                                                           "classobj.__call__")));

    classobj_cls->giveAttr("__getattribute__", new BoxedFunction(BoxedCode::create((void*)classobjGetattribute, UNKNOWN,
                                                                                   2, "classobj.__getattribute__")));
    classobj_cls->giveAttr("__setattr__", new BoxedFunction(BoxedCode::create((void*)classobjSetattr, UNKNOWN, 3,
                                                                              "classobj.__setattr__")));
    classobj_cls->giveAttr("__delattr__", new BoxedFunction(BoxedCode::create((void*)classobjDelattr, UNKNOWN, 2,
                                                                              "classobj.__delattr__")));
    classobj_cls->giveAttr("__str__",
                           new BoxedFunction(BoxedCode::create((void*)classobjStr, STR, 1, "classobj.__str__")));
    classobj_cls->giveAttr("__repr__",
                           new BoxedFunction(BoxedCode::create((void*)classobjRepr, STR, 1, "classobj.__repr__")));
    classobj_cls->giveAttrBorrowed("__dict__", dict_descr);

    classobj_cls->freeze();
    classobj_cls->tp_getattro = classobj_getattro;
    classobj_cls->tp_setattro = classobj_setattro;
    add_operators(classobj_cls);

    static PyNumberMethods instance_as_number;
    instance_cls->tp_as_number = &instance_as_number;
    static PySequenceMethods instance_as_sequence;
    instance_cls->tp_as_sequence = &instance_as_sequence;
    static PyMappingMethods instance_as_mapping;
    instance_cls->tp_as_mapping = &instance_as_mapping;

    instance_cls->giveAttr("__getattribute__",
                           new BoxedFunction(BoxedCode::create((void*)instanceGetattroInternal<CXX>, UNKNOWN, 2,
                                                               "instance.__getattribute__")));
    instance_cls->giveAttr("__delattr__", new BoxedFunction(BoxedCode::create((void*)instanceDelattr, UNKNOWN, 2,
                                                                              "instance.__delattr__")));
    instance_cls->giveAttr("__str__",
                           new BoxedFunction(BoxedCode::create((void*)instanceStr, UNKNOWN, 1, "instance.__str__")));
    instance_cls->giveAttr("__repr__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRepr, UNKNOWN, 1, "instance.__repr__")));
    instance_cls->giveAttr("__nonzero__", new BoxedFunction(BoxedCode::create((void*)instanceNonzero, UNKNOWN, 1,
                                                                              "instance.__nonzero__")));
    instance_cls->giveAttr("__len__",
                           new BoxedFunction(BoxedCode::create((void*)instanceLen, UNKNOWN, 1, "instance.__len__")));
    instance_cls->giveAttr("__getitem__", new BoxedFunction(BoxedCode::create((void*)instanceGetitem<CXX>, UNKNOWN, 2,
                                                                              "instance.__getitem__")));
    instance_cls->giveAttr("__setitem__", new BoxedFunction(BoxedCode::create((void*)instanceSetitem, UNKNOWN, 3,
                                                                              "instance.__setitem__")));
    instance_cls->giveAttr("__delitem__", new BoxedFunction(BoxedCode::create((void*)instanceDelitem, UNKNOWN, 2,
                                                                              "instance.__delitem__")));
    instance_cls->giveAttr("__getslice__", new BoxedFunction(BoxedCode::create((void*)instanceGetslice, UNKNOWN, 3,
                                                                               "instance.__getslice__")));
    instance_cls->giveAttr("__setslice__", new BoxedFunction(BoxedCode::create((void*)instanceSetslice, UNKNOWN, 4,
                                                                               "instance.__setslice__")));
    instance_cls->giveAttr("__delslice__", new BoxedFunction(BoxedCode::create((void*)instanceDelslice, UNKNOWN, 3,
                                                                               "instance.__delslice__")));
    instance_cls->giveAttr(
        "__cmp__", new BoxedFunction(BoxedCode::create((void*)instanceCompare, UNKNOWN, 2, "instance.__cmp__")));
    instance_cls->giveAttr("__contains__", new BoxedFunction(BoxedCode::create((void*)instanceContains, UNKNOWN, 2,
                                                                               "instance.__contains__")));
    instance_cls->giveAttr("__hash__",
                           new BoxedFunction(BoxedCode::create((void*)instanceHash, UNKNOWN, 1, "instance.__hash__")));
    instance_cls->giveAttr("__iter__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIter, UNKNOWN, 1, "instance.__iter__")));
    instance_cls->giveAttr("next",
                           new BoxedFunction(BoxedCode::create((void*)instanceNext, UNKNOWN, 1, "instance.next")));
    instance_cls->giveAttr("__call__", new BoxedFunction(BoxedCode::create((void*)instanceCall, UNKNOWN, 1, true, true,
                                                                           "instance.__call__")));
    instance_cls->giveAttr("__eq__",
                           new BoxedFunction(BoxedCode::create((void*)instanceEq, UNKNOWN, 2, "instance.__eq__")));
    instance_cls->giveAttr("__ne__",
                           new BoxedFunction(BoxedCode::create((void*)instanceNe, UNKNOWN, 2, "instance.__ne__")));
    instance_cls->giveAttr("__lt__",
                           new BoxedFunction(BoxedCode::create((void*)instanceLt, UNKNOWN, 2, "instance.__lt__")));
    instance_cls->giveAttr("__le__",
                           new BoxedFunction(BoxedCode::create((void*)instanceLe, UNKNOWN, 2, "instance.__le__")));
    instance_cls->giveAttr("__gt__",
                           new BoxedFunction(BoxedCode::create((void*)instanceGt, UNKNOWN, 2, "instance.__gt__")));
    instance_cls->giveAttr("__ge__",
                           new BoxedFunction(BoxedCode::create((void*)instanceGe, UNKNOWN, 2, "instance.__ge__")));
    instance_cls->giveAttr("__add__",
                           new BoxedFunction(BoxedCode::create((void*)instanceAdd, UNKNOWN, 2, "instance.__add__")));
    instance_cls->giveAttr("__sub__",
                           new BoxedFunction(BoxedCode::create((void*)instanceSub, UNKNOWN, 2, "instance.__sub__")));
    instance_cls->giveAttr("__mul__",
                           new BoxedFunction(BoxedCode::create((void*)instanceMul, UNKNOWN, 2, "instance.__mul__")));
    instance_cls->giveAttr("__floordiv__", new BoxedFunction(BoxedCode::create((void*)instanceFloordiv, UNKNOWN, 2,
                                                                               "instance.__floordiv__")));
    instance_cls->giveAttr("__mod__",
                           new BoxedFunction(BoxedCode::create((void*)instanceMod, UNKNOWN, 2, "instance.__mod__")));
    instance_cls->giveAttr(
        "__divmod__", new BoxedFunction(BoxedCode::create((void*)instanceDivMod, UNKNOWN, 2, "instance.__divmod__")));
    instance_cls->giveAttr("__pow__",
                           new BoxedFunction(BoxedCode::create((void*)instancePow, UNKNOWN, 2, "instance.__pow__")));
    instance_cls->giveAttr(
        "__lshift__", new BoxedFunction(BoxedCode::create((void*)instanceLshift, UNKNOWN, 2, "instance.__lshift__")));
    instance_cls->giveAttr(
        "__rshift__", new BoxedFunction(BoxedCode::create((void*)instanceRshift, UNKNOWN, 2, "instance.__rshift__")));
    instance_cls->giveAttr("__and__",
                           new BoxedFunction(BoxedCode::create((void*)instanceAnd, UNKNOWN, 2, "instance.__and__")));
    instance_cls->giveAttr("__xor__",
                           new BoxedFunction(BoxedCode::create((void*)instanceXor, UNKNOWN, 2, "instance.__xor__")));
    instance_cls->giveAttr("__or__",
                           new BoxedFunction(BoxedCode::create((void*)instanceOr, UNKNOWN, 2, "instance.__or__")));
    instance_cls->giveAttr("__div__",
                           new BoxedFunction(BoxedCode::create((void*)instanceDiv, UNKNOWN, 2, "instance.__div__")));
    instance_cls->giveAttr("__truediv__", new BoxedFunction(BoxedCode::create((void*)instanceTruediv, UNKNOWN, 2,
                                                                              "instance.__truediv__")));

    instance_cls->giveAttr("__radd__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRadd, UNKNOWN, 2, "instance.__radd__")));
    instance_cls->giveAttr("__rsub__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRsub, UNKNOWN, 2, "instance.__rsub__")));
    instance_cls->giveAttr("__rmul__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRmul, UNKNOWN, 2, "instance.__rmul__")));
    instance_cls->giveAttr("__rdiv__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRdiv, UNKNOWN, 2, "instance.__rdiv__")));
    instance_cls->giveAttr("__rtruediv__", new BoxedFunction(BoxedCode::create((void*)instanceRtruediv, UNKNOWN, 2,
                                                                               "instance.__rtruediv__")));
    instance_cls->giveAttr("__rfloordiv__", new BoxedFunction(BoxedCode::create((void*)instanceRfloordiv, UNKNOWN, 2,
                                                                                "instance.__rfloordiv__")));
    instance_cls->giveAttr("__rmod__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRmod, UNKNOWN, 2, "instance.__rmod__")));
    instance_cls->giveAttr("__rdivmod__", new BoxedFunction(BoxedCode::create((void*)instanceRdivmod, UNKNOWN, 2,
                                                                              "instance.__rdivmod__")));
    instance_cls->giveAttr("__rpow__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRpow, UNKNOWN, 2, "instance.__rpow__")));
    instance_cls->giveAttr("__rlshift__", new BoxedFunction(BoxedCode::create((void*)instanceRlshift, UNKNOWN, 2,
                                                                              "instance.__rlshift__")));
    instance_cls->giveAttr("__rrshift__", new BoxedFunction(BoxedCode::create((void*)instanceRrshift, UNKNOWN, 2,
                                                                              "instance.__rrshift__")));
    instance_cls->giveAttr("__rand__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRand, UNKNOWN, 2, "instance.__rand__")));
    instance_cls->giveAttr("__rxor__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRxor, UNKNOWN, 2, "instance.__rxor__")));
    instance_cls->giveAttr("__ror__",
                           new BoxedFunction(BoxedCode::create((void*)instanceRor, UNKNOWN, 2, "instance.__ror__")));

    instance_cls->giveAttr("__iadd__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIadd, UNKNOWN, 2, "instance.__iadd__")));
    instance_cls->giveAttr("__isub__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIsub, UNKNOWN, 2, "instance.__isub__")));
    instance_cls->giveAttr("__imul__",
                           new BoxedFunction(BoxedCode::create((void*)instanceImul, UNKNOWN, 2, "instance.__imul__")));
    instance_cls->giveAttr("__idiv__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIdiv, UNKNOWN, 2, "instance.__idiv__")));
    instance_cls->giveAttr("__itruediv__", new BoxedFunction(BoxedCode::create((void*)instanceItruediv, UNKNOWN, 2,
                                                                               "instance.__itruediv__")));
    instance_cls->giveAttr("__ifloordiv__", new BoxedFunction(BoxedCode::create((void*)instanceIfloordiv, UNKNOWN, 2,
                                                                                "instance.__ifloordiv__")));
    instance_cls->giveAttr("__imod__",
                           new BoxedFunction(BoxedCode::create((void*)instanceImod, UNKNOWN, 2, "instance.__imod__")));
    instance_cls->giveAttr("__ipow__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIpow, UNKNOWN, 2, "instance.__ipow__")));
    instance_cls->giveAttr("__ilshift__", new BoxedFunction(BoxedCode::create((void*)instanceIlshift, UNKNOWN, 2,
                                                                              "instance.__ilshift__")));
    instance_cls->giveAttr("__irshift__", new BoxedFunction(BoxedCode::create((void*)instanceIrshift, UNKNOWN, 2,
                                                                              "instance.__irshift__")));
    instance_cls->giveAttr("__iand__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIand, UNKNOWN, 2, "instance.__iand__")));
    instance_cls->giveAttr("__ixor__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIxor, UNKNOWN, 2, "instance.__ixor__")));
    instance_cls->giveAttr("__ior__",
                           new BoxedFunction(BoxedCode::create((void*)instanceIor, UNKNOWN, 2, "instance.__ior__")));

    instance_cls->giveAttr("__neg__",
                           new BoxedFunction(BoxedCode::create((void*)instanceNeg, UNKNOWN, 1, "instance.__neg__")));
    instance_cls->giveAttr("__pos__",
                           new BoxedFunction(BoxedCode::create((void*)instancePos, UNKNOWN, 1, "instance.__pos__")));
    instance_cls->giveAttr("__abs__",
                           new BoxedFunction(BoxedCode::create((void*)instanceAbs, UNKNOWN, 1, "instance.__abs__")));
    instance_cls->giveAttr(
        "__invert__", new BoxedFunction(BoxedCode::create((void*)instanceInvert, UNKNOWN, 1, "instance.__invert__")));
    instance_cls->giveAttr("__int__",
                           new BoxedFunction(BoxedCode::create((void*)instanceInt, UNKNOWN, 1, "instance.__int__")));
    instance_cls->giveAttr("__long__",
                           new BoxedFunction(BoxedCode::create((void*)instanceLong, UNKNOWN, 1, "instance.__long__")));
    instance_cls->giveAttr(
        "__float__", new BoxedFunction(BoxedCode::create((void*)instanceFloat, UNKNOWN, 1, "instance.__float__")));
    instance_cls->giveAttr("__oct__",
                           new BoxedFunction(BoxedCode::create((void*)instanceOct, UNKNOWN, 1, "instance.__oct__")));
    instance_cls->giveAttr("__hex__",
                           new BoxedFunction(BoxedCode::create((void*)instanceHex, UNKNOWN, 1, "instance.__hex__")));
    instance_cls->giveAttr(
        "__coerce__", new BoxedFunction(BoxedCode::create((void*)instanceCoerce, UNKNOWN, 2, "instance.__coerce__")));
    instance_cls->giveAttr(
        "__index__", new BoxedFunction(BoxedCode::create((void*)instanceIndex, UNKNOWN, 1, "instance.__index__")));

    instance_cls->freeze();
    instance_cls->tp_getattro = instance_getattro;
    instance_cls->tp_setattro = instance_setattro;
    instance_cls->tp_as_number->nb_index = instance_index;
    instance_cls->tp_as_number->nb_power = instance_pow;
    instance_cls->tp_as_number->nb_inplace_power = instance_ipow;
    instance_cls->tp_as_sequence->sq_item = (ssizeargfunc)instance_item;
    instance_cls->tp_as_sequence->sq_slice = instance_slice;
    instance_cls->tp_as_sequence->sq_ass_item = (ssizeobjargproc)instance_ass_item;
    instance_cls->tp_as_sequence->sq_ass_slice = (ssizessizeobjargproc)instance_ass_slice;
    instance_cls->tp_as_sequence->sq_length = (lenfunc)instance_length;
    instance_cls->tp_as_sequence->sq_contains = (objobjproc)instance_contains;

    instance_cls->tp_as_mapping->mp_length = (lenfunc)instance_length;
    instance_cls->tp_as_mapping->mp_subscript = (binaryfunc)instanceGetitem<CAPI>;
    instance_cls->tp_as_mapping->mp_ass_subscript = (objobjargproc)instance_ass_sub;
}
}
