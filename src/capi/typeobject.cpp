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


#include "capi/typeobject.h"

#include "capi/types.h"
#include "runtime/classobj.h"
#include "runtime/hiddenclass.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"

namespace pyston {

typedef int (*update_callback)(PyTypeObject*, void*);

static PyObject* tp_new_wrapper(PyTypeObject* self, BoxedTuple* args, Box* kwds) noexcept;

extern "C" void conservativeGCHandler(GCVisitor* v, Box* b) noexcept {
    v->visitPotentialRange((void* const*)b, (void* const*)((char*)b + b->cls->tp_basicsize));
}

static int check_num_args(PyObject* ob, int n) noexcept {
    if (!PyTuple_CheckExact(ob)) {
        PyErr_SetString(PyExc_SystemError, "PyArg_UnpackTuple() argument list is not a tuple");
        return 0;
    }
    if (n == PyTuple_GET_SIZE(ob))
        return 1;
    PyErr_Format(PyExc_TypeError, "expected %d arguments, got %zd", n, PyTuple_GET_SIZE(ob));
    return 0;
}

/* Helper to check for object.__setattr__ or __delattr__ applied to a type.
   This is called the Carlo Verre hack after its discoverer. */
static int hackcheck(PyObject* self, setattrofunc func, const char* what) noexcept {
    PyTypeObject* type = Py_TYPE(self);
    while (type && type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        type = type->tp_base;
    /* If type is NULL now, this is a really weird type.
       In the spirit of backwards compatibility (?), just shut up. */
    if (type && type->tp_setattro != func) {
        PyErr_Format(PyExc_TypeError, "can't apply this %s to %s object", what, type->tp_name);
        return 0;
    }
    return 1;
}

#define WRAP_AVOIDABILITY(obj) ((obj)->cls->is_user_defined ? 10 : 20)

static PyObject* wrap_setattr(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_setattr", WRAP_AVOIDABILITY(self));
    setattrofunc func = (setattrofunc)wrapped;
    int res;
    PyObject* name, *value;

    if (!PyArg_UnpackTuple(args, "", 2, 2, &name, &value))
        return NULL;
    if (!hackcheck(self, func, "__setattr__"))
        return NULL;
    res = (*func)(self, name, value);
    if (res < 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_delattr(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_delattr", WRAP_AVOIDABILITY(self));
    setattrofunc func = (setattrofunc)wrapped;
    int res;
    PyObject* name;

    if (!check_num_args(args, 1))
        return NULL;
    name = PyTuple_GET_ITEM(args, 0);
    if (!hackcheck(self, func, "__delattr__"))
        return NULL;
    res = (*func)(self, name, NULL);
    if (res < 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_hashfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_hashfunc", WRAP_AVOIDABILITY(self));
    hashfunc func = (hashfunc)wrapped;
    long res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    return PyInt_FromLong(res);
}

static PyObject* wrap_call(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_call", WRAP_AVOIDABILITY(self));
    ternaryfunc func = (ternaryfunc)wrapped;

    return (*func)(self, args, kwds);
}

static PyObject* wrap_richcmpfunc(PyObject* self, PyObject* args, void* wrapped, int op) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_richcmpfunc", WRAP_AVOIDABILITY(self));
    richcmpfunc func = (richcmpfunc)wrapped;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    return (*func)(self, other, op);
}

#undef RICHCMP_WRAPPER
#define RICHCMP_WRAPPER(NAME, OP)                                                                                      \
    static PyObject* richcmp_##NAME(PyObject* self, PyObject* args, void* wrapped) {                                   \
        return wrap_richcmpfunc(self, args, wrapped, OP);                                                              \
    }

RICHCMP_WRAPPER(lt, Py_LT)
RICHCMP_WRAPPER(le, Py_LE)
RICHCMP_WRAPPER(eq, Py_EQ)
RICHCMP_WRAPPER(ne, Py_NE)
RICHCMP_WRAPPER(gt, Py_GT)
RICHCMP_WRAPPER(ge, Py_GE)

static PyObject* wrap_next(PyObject* self, PyObject* args, void* wrapped) {
    STAT_TIMER(t0, "us_timer_wrap_next", WRAP_AVOIDABILITY(self));
    unaryfunc func = (unaryfunc)wrapped;
    PyObject* res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == NULL && !PyErr_Occurred())
        PyErr_SetNone(PyExc_StopIteration);
    return res;
}

static PyObject* wrap_descr_get(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_descr_get", WRAP_AVOIDABILITY(self));
    descrgetfunc func = (descrgetfunc)wrapped;
    PyObject* obj;
    PyObject* type = NULL;

    if (!PyArg_UnpackTuple(args, "", 1, 2, &obj, &type))
        return NULL;
    if (obj == Py_None)
        obj = NULL;
    if (type == Py_None)
        type = NULL;
    if (type == NULL && obj == NULL) {
        PyErr_SetString(PyExc_TypeError, "__get__(None, None) is invalid");
        return NULL;
    }
    return (*func)(self, obj, type);
}

static PyObject* wrap_coercefunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_coercefunc", WRAP_AVOIDABILITY(self));
    coercion func = (coercion)wrapped;
    PyObject* other, *res;
    int ok;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    ok = func(&self, &other);
    if (ok < 0)
        return NULL;
    if (ok > 0) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    res = PyTuple_New(2);
    if (res == NULL) {
        Py_DECREF(self);
        Py_DECREF(other);
        return NULL;
    }
    PyTuple_SET_ITEM(res, 0, self);
    PyTuple_SET_ITEM(res, 1, other);
    return res;
}

static PyObject* wrap_ternaryfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_ternaryfunc", WRAP_AVOIDABILITY(self));
    ternaryfunc func = (ternaryfunc)wrapped;
    PyObject* other;
    PyObject* third = Py_None;

    /* Note: This wrapper only works for __pow__() */

    if (!PyArg_UnpackTuple(args, "", 1, 2, &other, &third))
        return NULL;
    return (*func)(self, other, third);
}

static PyObject* wrap_ternaryfunc_r(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_ternaryfunc_r", WRAP_AVOIDABILITY(self));
    ternaryfunc func = (ternaryfunc)wrapped;
    PyObject* other;
    PyObject* third = Py_None;

    /* Note: This wrapper only works for __pow__() */

    if (!PyArg_UnpackTuple(args, "", 1, 2, &other, &third))
        return NULL;
    return (*func)(other, self, third);
}

static PyObject* wrap_unaryfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_unaryfunc", WRAP_AVOIDABILITY(self));
    unaryfunc func = (unaryfunc)wrapped;

    if (!check_num_args(args, 0))
        return NULL;
    return (*func)(self);
}

static PyObject* wrap_inquirypred(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_inquirypred", WRAP_AVOIDABILITY(self));
    inquiry func = (inquiry)wrapped;
    int res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    return PyBool_FromLong((long)res);
}

static PyObject* wrapInquirypred(PyObject* self, PyObject* args, void* wrapped) {
    STAT_TIMER(t0, "us_timer_wrapInquirypred", WRAP_AVOIDABILITY(self));
    inquiry func = (inquiry)wrapped;
    int res;

    if (!check_num_args(args, 0))
        throwCAPIException();
    res = (*func)(self);
    assert(res == 0 || res == 1);
    assert(!PyErr_Occurred());
    return PyBool_FromLong((long)res);
}

static PyObject* wrap_binaryfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_binaryfunc", WRAP_AVOIDABILITY(self));
    binaryfunc func = (binaryfunc)wrapped;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    return (*func)(self, other);
}

static PyObject* wrap_binaryfunc_l(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_binaryfunc_l", WRAP_AVOIDABILITY(self));
    binaryfunc func = (binaryfunc)wrapped;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    if (!(self->cls->tp_flags & Py_TPFLAGS_CHECKTYPES) && !PyType_IsSubtype(other->cls, self->cls)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    return (*func)(self, other);
}

static PyObject* wrap_binaryfunc_r(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_binaryfunc_r", WRAP_AVOIDABILITY(self));
    binaryfunc func = (binaryfunc)wrapped;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    if (!(self->cls->tp_flags & Py_TPFLAGS_CHECKTYPES) && !PyType_IsSubtype(other->cls, self->cls)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    return (*func)(other, self);
}

static Py_ssize_t getindex(PyObject* self, PyObject* arg) noexcept {
    Py_ssize_t i;

    i = PyNumber_AsSsize_t(arg, PyExc_OverflowError);
    if (i == -1 && PyErr_Occurred())
        return -1;
    if (i < 0) {
        PySequenceMethods* sq = Py_TYPE(self)->tp_as_sequence;
        if (sq && sq->sq_length) {
            Py_ssize_t n = (*sq->sq_length)(self);
            if (n < 0)
                return -1;
            i += n;
        }
    }
    return i;
}

static PyObject* wrap_lenfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_lenfunc", WRAP_AVOIDABILITY(self));
    lenfunc func = (lenfunc)wrapped;
    Py_ssize_t res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    return PyInt_FromLong((long)res);
}

static PyObject* wrap_indexargfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_indexargfunc", WRAP_AVOIDABILITY(self));
    ssizeargfunc func = (ssizeargfunc)wrapped;
    PyObject* o;
    Py_ssize_t i;

    if (!PyArg_UnpackTuple(args, "", 1, 1, &o))
        return NULL;
    i = PyNumber_AsSsize_t(o, PyExc_OverflowError);
    if (i == -1 && PyErr_Occurred())
        return NULL;
    return (*func)(self, i);
}

static PyObject* wrap_sq_item(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_sq_item", WRAP_AVOIDABILITY(self));
    ssizeargfunc func = (ssizeargfunc)wrapped;
    PyObject* arg;
    Py_ssize_t i;

    if (PyTuple_GET_SIZE(args) == 1) {
        arg = PyTuple_GET_ITEM(args, 0);
        i = getindex(self, arg);
        if (i == -1 && PyErr_Occurred())
            return NULL;
        return (*func)(self, i);
    }
    check_num_args(args, 1);
    assert(PyErr_Occurred());
    return NULL;
}

static PyObject* wrap_ssizessizeargfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_ssizessizeargfunc", WRAP_AVOIDABILITY(self));
    ssizessizeargfunc func = (ssizessizeargfunc)wrapped;
    Py_ssize_t i, j;

    if (!PyArg_ParseTuple(args, "nn", &i, &j))
        return NULL;
    return (*func)(self, i, j);
}

static PyObject* wrap_sq_setitem(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_sq_setitem", WRAP_AVOIDABILITY(self));
    ssizeobjargproc func = (ssizeobjargproc)wrapped;
    Py_ssize_t i;
    int res;
    PyObject* arg, *value;

    if (!PyArg_UnpackTuple(args, "", 2, 2, &arg, &value))
        return NULL;
    i = getindex(self, arg);
    if (i == -1 && PyErr_Occurred())
        return NULL;
    res = (*func)(self, i, value);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_sq_delitem(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_sq_delitem", WRAP_AVOIDABILITY(self));
    ssizeobjargproc func = (ssizeobjargproc)wrapped;
    Py_ssize_t i;
    int res;
    PyObject* arg;

    if (!check_num_args(args, 1))
        return NULL;
    arg = PyTuple_GET_ITEM(args, 0);
    i = getindex(self, arg);
    if (i == -1 && PyErr_Occurred())
        return NULL;
    res = (*func)(self, i, NULL);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_ssizessizeobjargproc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_ssizessizeobjargproc", WRAP_AVOIDABILITY(self));
    ssizessizeobjargproc func = (ssizessizeobjargproc)wrapped;
    Py_ssize_t i, j;
    int res;
    PyObject* value;

    if (!PyArg_ParseTuple(args, "nnO", &i, &j, &value))
        return NULL;
    res = (*func)(self, i, j, value);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_delslice(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_delslice", WRAP_AVOIDABILITY(self));
    ssizessizeobjargproc func = (ssizessizeobjargproc)wrapped;
    Py_ssize_t i, j;
    int res;

    if (!PyArg_ParseTuple(args, "nn", &i, &j))
        return NULL;
    res = (*func)(self, i, j, NULL);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

/* XXX objobjproc is a misnomer; should be objargpred */
static PyObject* wrap_objobjproc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_objobjproc", WRAP_AVOIDABILITY(self));
    objobjproc func = (objobjproc)wrapped;
    int res;
    PyObject* value;

    if (!check_num_args(args, 1))
        return NULL;
    value = PyTuple_GET_ITEM(args, 0);
    res = (*func)(self, value);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    else
        return PyBool_FromLong(res);
}

static PyObject* wrap_objobjargproc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_objobjargproc", WRAP_AVOIDABILITY(self));
    objobjargproc func = (objobjargproc)wrapped;
    int res;
    PyObject* key, *value;

    if (!PyArg_UnpackTuple(args, "", 2, 2, &key, &value))
        return NULL;
    res = (*func)(self, key, value);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_delitem(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_delitem", WRAP_AVOIDABILITY(self));
    objobjargproc func = (objobjargproc)wrapped;
    int res;
    PyObject* key;

    if (!check_num_args(args, 1))
        return NULL;
    key = PyTuple_GET_ITEM(args, 0);
    res = (*func)(self, key, NULL);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* wrap_cmpfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_cmpfunc", WRAP_AVOIDABILITY(self));
    cmpfunc func = (cmpfunc)wrapped;
    int res;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    if (Py_TYPE(other)->tp_compare != func && !PyType_IsSubtype(Py_TYPE(other), Py_TYPE(self))) {
        PyErr_Format(PyExc_TypeError, "%s.__cmp__(x,y) requires y to be a '%s', not a '%s'", Py_TYPE(self)->tp_name,
                     Py_TYPE(self)->tp_name, Py_TYPE(other)->tp_name);
        return NULL;
    }
    res = (*func)(self, other);
    if (PyErr_Occurred())
        return NULL;
    return PyInt_FromLong((long)res);
}


static PyObject* wrap_init(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds) noexcept {
    STAT_TIMER(t0, "us_timer_wrap_init", WRAP_AVOIDABILITY(self));
    initproc func = (initproc)wrapped;

    if (func(self, args, kwds) < 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}



static PyObject* lookup_maybe(PyObject* self, const char* attrstr, PyObject** attrobj) noexcept {
    PyObject* res;

    if (*attrobj == NULL) {
        *attrobj = PyString_InternFromString(attrstr);
        if (*attrobj == NULL)
            return NULL;
    }

    Box* obj = typeLookup(self->cls, (BoxedString*)*attrobj, NULL);
    if (obj) {
        try {
            return processDescriptor(obj, self, self->cls);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    }
    return obj;
}

extern "C" PyObject* _PyObject_LookupSpecial(PyObject* self, const char* attrstr, PyObject** attrobj) noexcept {
    assert(!PyInstance_Check(self));
    return lookup_maybe(self, attrstr, attrobj);
}

static PyObject* lookup_method(PyObject* self, const char* attrstr, PyObject** attrobj) noexcept {
    PyObject* res = lookup_maybe(self, attrstr, attrobj);
    if (res == NULL && !PyErr_Occurred())
        PyErr_SetObject(PyExc_AttributeError, *attrobj);
    return res;
}

// Copied from CPython:
static PyObject* call_method(PyObject* o, const char* name, PyObject** nameobj, const char* format, ...) noexcept {
    va_list va;
    PyObject* args, * func = 0, *retval;
    va_start(va, format);

    func = lookup_maybe(o, name, nameobj);
    if (func == NULL) {
        va_end(va);
        if (!PyErr_Occurred())
            PyErr_SetObject(PyExc_AttributeError, *nameobj);
        return NULL;
    }

    if (format && *format)
        args = Py_VaBuildValue(format, va);
    else
        args = PyTuple_New(0);

    va_end(va);

    if (args == NULL)
        return NULL;

    assert(PyTuple_Check(args));
    retval = PyObject_Call(func, args, NULL);

    Py_DECREF(args);
    Py_DECREF(func);

    return retval;
}

/* Clone of call_method() that returns NotImplemented when the lookup fails. */

static PyObject* call_maybe(PyObject* o, const char* name, PyObject** nameobj, const char* format, ...) noexcept {
    va_list va;
    PyObject* args, * func = 0, *retval;
    va_start(va, format);

    func = lookup_maybe(o, name, nameobj);
    if (func == NULL) {
        va_end(va);
        if (!PyErr_Occurred()) {
            Py_INCREF(Py_NotImplemented);
            return Py_NotImplemented;
        }
        return NULL;
    }

    if (format && *format)
        args = Py_VaBuildValue(format, va);
    else
        args = PyTuple_New(0);

    va_end(va);

    if (args == NULL)
        return NULL;

    assert(PyTuple_Check(args));
    retval = PyObject_Call(func, args, NULL);

    Py_DECREF(args);
    Py_DECREF(func);

    return retval;
}

static int half_compare(PyObject* self, PyObject* other) noexcept {
    PyObject* func, *args, *res;
    static PyObject* cmp_str;
    Py_ssize_t c;

    func = lookup_method(self, "__cmp__", &cmp_str);
    if (func == NULL) {
        PyErr_Clear();
    } else {
        args = PyTuple_Pack(1, other);
        if (args == NULL)
            res = NULL;
        else {
            res = PyObject_Call(func, args, NULL);
            Py_DECREF(args);
        }
        Py_DECREF(func);
        if (res != Py_NotImplemented) {
            if (res == NULL)
                return -2;
            c = PyInt_AsLong(res);
            Py_DECREF(res);
            if (c == -1 && PyErr_Occurred())
                return -2;
            return (c < 0) ? -1 : (c > 0) ? 1 : 0;
        }
        Py_DECREF(res);
    }
    return 2;
}

/* This slot is published for the benefit of try_3way_compare in object.c */
extern "C" int _PyObject_SlotCompare(PyObject* self, PyObject* other) noexcept {
    int c;

    if (Py_TYPE(self)->tp_compare == _PyObject_SlotCompare) {
        c = half_compare(self, other);
        if (c <= 1)
            return c;
    }
    if (Py_TYPE(other)->tp_compare == _PyObject_SlotCompare) {
        c = half_compare(other, self);
        if (c < -1)
            return -2;
        if (c <= 1)
            return -c;
    }
    return (void*)self < (void*)other ? -1 : (void*)self > (void*)other ? 1 : 0;
}

#define SLOT_AVOIDABILITY(obj) ((obj)->cls->is_user_defined ? 10 : 20)

static PyObject* slot_tp_repr(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tprepr", SLOT_AVOIDABILITY(self));

    try {
        return repr(self);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static PyObject* slot_tp_str(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpstr", SLOT_AVOIDABILITY(self));

    try {
        return str(self);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static long slot_tp_hash(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tphash", SLOT_AVOIDABILITY(self));

    PyObject* func;
    static PyObject* hash_str, *eq_str, *cmp_str;
    long h;

    func = lookup_method(self, "__hash__", &hash_str);

    if (func != NULL && func != Py_None) {
        PyObject* res = PyEval_CallObject(func, NULL);
        Py_DECREF(func);
        if (res == NULL)
            return -1;
        if (PyLong_Check(res))
            h = PyLong_Type.tp_hash(res);
        else
            h = PyInt_AsLong(res);
        Py_DECREF(res);
    } else {
        Py_XDECREF(func); /* may be None */
        PyErr_Clear();
        func = lookup_method(self, "__eq__", &eq_str);
        if (func == NULL) {
            PyErr_Clear();
            func = lookup_method(self, "__cmp__", &cmp_str);
        }
        if (func != NULL) {
            Py_DECREF(func);
            return PyObject_HashNotImplemented(self);
        }
        PyErr_Clear();
        h = _Py_HashPointer((void*)self);
    }
    if (h == -1 && !PyErr_Occurred())
        h = -2;
    return h;
}

PyObject* slot_tp_call(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpcall", SLOT_AVOIDABILITY(self));

    try {
        Py_FatalError("this function is untested");

        // TODO: runtime ICs?
        return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwds, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static const char* name_op[] = {
    "__lt__", "__le__", "__eq__", "__ne__", "__gt__", "__ge__",
};

static PyObject* half_richcompare(PyObject* self, PyObject* other, int op) noexcept {
    PyObject* func, *args, *res;
    static PyObject* op_str[6];

    func = lookup_method(self, name_op[op], &op_str[op]);
    if (func == NULL) {
        PyErr_Clear();
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    args = PyTuple_Pack(1, other);
    if (args == NULL)
        res = NULL;
    else {
        res = PyObject_Call(func, args, NULL);
        Py_DECREF(args);
    }
    Py_DECREF(func);
    return res;
}

/* Pyston change: static*/ PyObject* slot_tp_richcompare(PyObject* self, PyObject* other, int op) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tprichcompare", SLOT_AVOIDABILITY(self));

    static StatCounter slowpath_richcompare("slowpath_richcompare");
    slowpath_richcompare.log();
#if 0
    std::string per_name_stat_name = "slowpath_richcompare." + std::string(self->cls->tp_name);
    int id = Stats::getStatId(per_name_stat_name);
    Stats::log(id);
#endif

    PyObject* res;

    if (Py_TYPE(self)->tp_richcompare == slot_tp_richcompare) {
        res = half_richcompare(self, other, op);
        if (res != Py_NotImplemented)
            return res;
        Py_DECREF(res);
    }
    if (Py_TYPE(other)->tp_richcompare == slot_tp_richcompare) {
        res = half_richcompare(other, self, _Py_SwappedOp[op]);
        if (res != Py_NotImplemented) {
            return res;
        }
        Py_DECREF(res);
    }
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
}

static PyObject* slot_tp_iter(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpiter", SLOT_AVOIDABILITY(self));

    PyObject* func, *res;
    static PyObject* iter_str, *getitem_str;

    func = lookup_method(self, "__iter__", &iter_str);
    if (func != NULL) {
        PyObject* args;
        args = res = PyTuple_New(0);
        if (args != NULL) {
            res = PyObject_Call(func, args, NULL);
            Py_DECREF(args);
        }
        Py_DECREF(func);
        return res;
    }
    PyErr_Clear();
    func = lookup_method(self, "__getitem__", &getitem_str);
    if (func == NULL) {
        PyErr_Format(PyExc_TypeError, "'%.200s' object is not iterable", Py_TYPE(self)->tp_name);
        return NULL;
    }
    Py_DECREF(func);
    return PySeqIter_New(self);
}

/* Pyston change: static */ PyObject* slot_tp_iternext(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpiternext", SLOT_AVOIDABILITY(self));

    static PyObject* next_str;
    return call_method(self, "next", &next_str, "()");
}

static bool slotTppHasnext(PyObject* self) {
    STAT_TIMER(t0, "us_timer_slot_tpphasnext", SLOT_AVOIDABILITY(self));

    static PyObject* hasnext_str;
    Box* r = self->hasnextOrNullIC();
    assert(r);
    return r->nonzeroIC();
}

static PyObject* slot_tp_descr_get(PyObject* self, PyObject* obj, PyObject* type) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpdescrget", SLOT_AVOIDABILITY(self));

    PyTypeObject* tp = Py_TYPE(self);
    PyObject* get;

    static BoxedString* get_str = internStringImmortal("__get__");
    get = typeLookup(tp, get_str, NULL);
    if (get == NULL) {
        /* Avoid further slowdowns */
        if (tp->tp_descr_get == slot_tp_descr_get)
            tp->tp_descr_get = NULL;
        Py_INCREF(self);
        return self;
    }
    if (obj == NULL)
        obj = Py_None;
    if (type == NULL)
        type = Py_None;
    return PyObject_CallFunctionObjArgs(get, self, obj, type, NULL);
}

static PyObject* slot_tp_tpp_descr_get(PyObject* self, PyObject* obj, PyObject* type) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tppdescrget", SLOT_AVOIDABILITY(self));

    assert(self->cls->tpp_descr_get);
    try {
        return self->cls->tpp_descr_get(self, obj, type);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static PyObject* slot_tp_getattro(PyObject* self, PyObject* name) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpgetattro", SLOT_AVOIDABILITY(self));

    static PyObject* getattribute_str = NULL;
    return call_method(self, "__getattribute__", &getattribute_str, "(O)", name);
}

static int slot_tp_setattro(PyObject* self, PyObject* name, PyObject* value) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpsetattro", SLOT_AVOIDABILITY(self));

    PyObject* res;
    static PyObject* delattr_str, *setattr_str;

    if (value == NULL)
        res = call_method(self, "__delattr__", &delattr_str, "(O)", name);
    else
        res = call_method(self, "__setattr__", &setattr_str, "(OO)", name, value);
    if (res == NULL)
        return -1;
    Py_DECREF(res);
    return 0;
}

static PyObject* call_attribute(PyObject* self, PyObject* attr, PyObject* name) noexcept {
    PyObject* res, * descr = NULL;
    descrgetfunc f = Py_TYPE(attr)->tp_descr_get;

    if (f != NULL) {
        descr = f(attr, self, (PyObject*)(Py_TYPE(self)));
        if (descr == NULL)
            return NULL;
        else
            attr = descr;
    }
    try {
        res = runtimeCall(attr, ArgPassSpec(1, 0, false, false), name, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        Py_XDECREF(descr);
        return NULL;
    }
    Py_XDECREF(descr);
    return res;
}

/* Pyston change: static */ PyObject* slot_tp_getattr_hook(PyObject* self, PyObject* name) noexcept {
    try {
        assert(name->cls == str_cls);
        return slotTpGetattrHookInternal(self, (BoxedString*)name, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* slotTpGetattrHookInternal(Box* self, BoxedString* name, GetattrRewriteArgs* rewrite_args) {
    STAT_TIMER(t0, "us_timer_slot_tpgetattrhook", SLOT_AVOIDABILITY(self));

    PyObject* getattr, *getattribute, * res = NULL;

    /* speed hack: we could use lookup_maybe, but that would resolve the
         method fully for each attribute lookup for classes with
         __getattr__, even when the attribute is present. So we use
         _PyType_Lookup and create the method only when needed, with
         call_attribute. */
    static BoxedString* _getattr_str = internStringImmortal("__getattr__");

    // Don't need to do this in the rewritten version; if a __getattr__ later gets removed:
    // - if we ever get to the "call __getattr__" portion of the rewrite, the guards will
    //   fail and we will end up back here
    // - if we never get to the "call __getattr__" portion and the "calling __getattribute__"
    //   portion still has its guards pass, then that section is still behaviorally correct, and
    //   I think should be close to as fast as the normal rewritten version we would generate.
    getattr = typeLookup(self->cls, _getattr_str, NULL);

    if (getattr == NULL) {
        assert(!rewrite_args || !rewrite_args->out_success);

        /* No __getattr__ hook: use a simpler dispatcher */
        self->cls->tp_getattro = slot_tp_getattro;
        return slot_tp_getattro(self, name);
    }

    /* speed hack: we could use lookup_maybe, but that would resolve the
         method fully for each attribute lookup for classes with
         __getattr__, even when self has the default __getattribute__
         method. So we use _PyType_Lookup and create the method only when
         needed, with call_attribute. */
    static BoxedString* _getattribute_str = internStringImmortal("__getattribute__");

    RewriterVar* r_getattribute = NULL;
    if (rewrite_args) {
        RewriterVar* r_obj_cls = rewrite_args->obj->getAttr(offsetof(Box, cls), Location::any());
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_obj_cls, Location::any());

        getattribute = typeLookup(self->cls, _getattribute_str, &grewrite_args);
        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else if (getattribute)
            r_getattribute = grewrite_args.out_rtn;
    } else {
        getattribute = typeLookup(self->cls, _getattribute_str, NULL);
    }
    // Not sure why CPython checks if getattribute is NULL since I don't think that should happen.
    // Is there some legacy way of creating types that don't inherit from object?  Anyway, I think we
    // have the right behavior even if getattribute was somehow NULL, but add an assert because that
    // case would still be very surprising to me:
    assert(getattribute);

    if (getattribute == NULL
        || (Py_TYPE(getattribute) == wrapperdescr_cls
            && ((BoxedWrapperDescriptor*)getattribute)->wrapped == (void*)PyObject_GenericGetAttr)) {

        assert(PyString_CHECK_INTERNED(name));
        if (rewrite_args) {
            // Fetching getattribute should have done the appropriate guarding on whether or not
            // getattribute exists.
            if (getattribute)
                r_getattribute->addGuard((intptr_t)getattribute);

            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
            try {
                res = getattrInternalGeneric(self, name, &grewrite_args, false, false, NULL, NULL);
            } catch (ExcInfo e) {
                if (!e.matches(AttributeError))
                    throw e;

                grewrite_args.out_success = false;
                res = NULL;
            }

            if (!grewrite_args.out_success)
                rewrite_args = NULL;
            else if (res)
                rewrite_args->out_rtn = grewrite_args.out_rtn;
        } else {
            try {
                res = getattrInternalGeneric(self, name, NULL, false, false, NULL, NULL);
            } catch (ExcInfo e) {
                if (!e.matches(AttributeError))
                    throw e;
                res = NULL;
            }
        }
    } else {
        rewrite_args = NULL;

        res = call_attribute(self, getattribute, name);
        if (res == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError))
                PyErr_Clear();
            else
                throwCAPIException();
        }
    }

    // At this point, CPython would have three cases: res is non-NULL and no exception was thrown,
    // or res is NULL and an exception was thrown and either it was an AttributeError (in which case
    // we call __getattr__) or it wan't (in which case it propagates).
    //
    // We handled it differently: if a non-AttributeError was thrown, we already would have propagated
    // it.  So there are only two cases: res is non-NULL if the attribute exists, or it is NULL if it
    // doesn't exist.

    if (res) {
        if (rewrite_args)
            rewrite_args->out_success = true;
        return res;
    }

    assert(!PyErr_Occurred());

    CallattrFlags callattr_flags = {.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(1) };
    if (rewrite_args) {
        // I was thinking at first that we could try to catch any AttributeErrors here and still
        // write out valid rewrite, but
        // - we need to let the original AttributeError propagate and not generate a new, potentially-different one
        // - we have no way of signalling that "we didn't get an attribute this time but that may be different
        //   in future executions through the IC".
        // I think this should only end up mattering anyway if the getattr site throws every single time.
        CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
        assert(PyString_CHECK_INTERNED(name) == SSTATE_INTERNED_IMMORTAL);
        crewrite_args.arg1 = rewrite_args->rewriter->loadConst((intptr_t)name, Location::forArg(1));

        res = callattrInternal(self, _getattr_str, LookupScope::CLASS_ONLY, &crewrite_args, ArgPassSpec(1), name, NULL,
                               NULL, NULL, NULL);
        assert(res);

        if (!crewrite_args.out_success)
            rewrite_args = NULL;
        else
            rewrite_args->out_rtn = crewrite_args.out_rtn;
    } else {
        // TODO: we already fetched the getattr attribute, it would be faster to call it rather than do
        // a second callattr.  My guess though is that the gains would be small, so I would prefer to keep
        // the rewrite_args and non-rewrite_args case the same.
        // Actually, we might have gotten to the point that doing a runtimeCall on an instancemethod is as
        // fast as a callattr, but that hasn't typically been the case.
        res = callattrInternal(self, _getattr_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(1), name, NULL, NULL,
                               NULL, NULL);
        assert(res);
    }

    if (rewrite_args)
        rewrite_args->out_success = true;
    return res;
}

/* Pyston change: static */ PyObject* slot_tp_new(PyTypeObject* self, PyObject* args, PyObject* kwds) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpnew", SLOT_AVOIDABILITY(self));

    try {
        // TODO: runtime ICs?
        static BoxedString* _new_str = internStringImmortal("__new__");
        Box* new_attr = typeLookup(self, _new_str, NULL);
        assert(new_attr);
        new_attr = processDescriptor(new_attr, None, self);

        return runtimeCall(new_attr, ArgPassSpec(1, 0, true, true), self, args, kwds, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static PyObject* slot_tp_del(PyObject* self) noexcept {
    static BoxedString* del_str = internStringImmortal("__del__");
    try {
        // TODO: runtime ICs?
        Box* del_attr = typeLookup(self->cls, del_str, NULL);
        assert(del_attr);

        CallattrFlags flags{.cls_only = false,
                            .null_on_nonexistent = true,
                            .argspec = ArgPassSpec(0, 0, false, false) };
        return callattr(self, del_str, flags, NULL, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        // Python does not support exceptions thrown inside finalizers. Instead, it just
        // prints a warning that an exception was throw to stderr but ignores it.
        setCAPIException(e);
        PyErr_WriteUnraisable(self);
        return NULL;
    }
}

static int slot_tp_init(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    STAT_TIMER(t0, "us_timer_slot_tpinit", SLOT_AVOIDABILITY(self));

    static PyObject* init_str;
    PyObject* meth = lookup_method(self, "__init__", &init_str);
    PyObject* res;

    if (meth == NULL)
        return -1;
    res = PyObject_Call(meth, args, kwds);
    Py_DECREF(meth);
    if (res == NULL)
        return -1;
    if (res != Py_None) {
        PyErr_Format(PyExc_TypeError, "__init__() should return None, not '%.200s'", Py_TYPE(res)->tp_name);
        Py_DECREF(res);
        return -1;
    }
    Py_DECREF(res);
    return 0;
}

PyObject* slot_sq_item(PyObject* self, Py_ssize_t i) noexcept {
    STAT_TIMER(t0, "us_timer_slot_sqitem", SLOT_AVOIDABILITY(self));

    try {
        return getitem(self, boxInt(i));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

/* Pyston change: static */ Py_ssize_t slot_sq_length(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_sqlength", SLOT_AVOIDABILITY(self));

    static PyObject* len_str;
    PyObject* res = call_method(self, "__len__", &len_str, "()");
    Py_ssize_t len;

    if (res == NULL)
        return -1;
    len = PyInt_AsSsize_t(res);
    Py_DECREF(res);
    if (len < 0) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_ValueError, "__len__() should return >= 0");
        return -1;
    }
    return len;
}

static PyObject* slot_sq_slice(PyObject* self, Py_ssize_t i, Py_ssize_t j) noexcept {
    STAT_TIMER(t0, "us_timer_slot_sqslice", SLOT_AVOIDABILITY(self));

    static PyObject* getslice_str;

    if (PyErr_WarnPy3k("in 3.x, __getslice__ has been removed; "
                       "use __getitem__",
                       1) < 0)
        return NULL;
    return call_method(self, "__getslice__", &getslice_str, "nn", i, j);
}

static int slot_sq_ass_item(PyObject* self, Py_ssize_t index, PyObject* value) noexcept {
    STAT_TIMER(t0, "us_timer_slot_sqassitem", SLOT_AVOIDABILITY(self));

    PyObject* res;
    static PyObject* delitem_str, *setitem_str;

    if (value == NULL)
        res = call_method(self, "__delitem__", &delitem_str, "(n)", index);
    else
        res = call_method(self, "__setitem__", &setitem_str, "(nO)", index, value);
    if (res == NULL)
        return -1;
    Py_DECREF(res);
    return 0;
}

static int slot_sq_ass_slice(PyObject* self, Py_ssize_t i, Py_ssize_t j, PyObject* value) noexcept {
    STAT_TIMER(t0, "us_timer_slot_sqassslice", SLOT_AVOIDABILITY(self));

    PyObject* res;
    static PyObject* delslice_str, *setslice_str;

    if (value == NULL) {
        if (PyErr_WarnPy3k("in 3.x, __delslice__ has been removed; "
                           "use __delitem__",
                           1) < 0)
            return -1;
        res = call_method(self, "__delslice__", &delslice_str, "(nn)", i, j);
    } else {
        if (PyErr_WarnPy3k("in 3.x, __setslice__ has been removed; "
                           "use __setitem__",
                           1) < 0)
            return -1;
        res = call_method(self, "__setslice__", &setslice_str, "(nnO)", i, j, value);
    }
    if (res == NULL)
        return -1;
    Py_DECREF(res);
    return 0;
}

/* Pyston change: static*/ int slot_sq_contains(PyObject* self, PyObject* value) noexcept {
    STAT_TIMER(t0, "us_timer_slot_sqcontains", SLOT_AVOIDABILITY(self));

    PyObject* func, *res, *args;
    int result = -1;

    static PyObject* contains_str;

    func = lookup_maybe(self, "__contains__", &contains_str);
    if (func != NULL) {
        args = PyTuple_Pack(1, value);
        if (args == NULL)
            res = NULL;
        else {
            res = PyObject_Call(func, args, NULL);
            Py_DECREF(args);
        }
        Py_DECREF(func);
        if (res != NULL) {
            result = PyObject_IsTrue(res);
            Py_DECREF(res);
        }
    } else if (!PyErr_Occurred()) {
        /* Possible results: -1 and 1 */
        Py_FatalError("unimplemented");
        // result = (int)_PySequence_IterSearch(self, value, PY_ITERSEARCH_CONTAINS);
    }
    return result;
}

// Copied from CPython:
#define SLOT0(FUNCNAME, OPSTR)                                                                                         \
    static PyObject* FUNCNAME(PyObject* self) noexcept {                                                               \
        STAT_TIMER(t0, "us_timer_" #FUNCNAME, SLOT_AVOIDABILITY(self));                                                \
        static PyObject* cache_str;                                                                                    \
        return call_method(self, OPSTR, &cache_str, "()");                                                             \
    }

#define SLOT1(FUNCNAME, OPSTR, ARG1TYPE, ARGCODES)                                                                     \
    /* Pyston change: static */ PyObject* FUNCNAME(PyObject* self, ARG1TYPE arg1) noexcept {                           \
        STAT_TIMER(t0, "us_timer_" #FUNCNAME, SLOT_AVOIDABILITY(self));                                                \
        static PyObject* cache_str;                                                                                    \
        return call_method(self, OPSTR, &cache_str, "(" ARGCODES ")", arg1);                                           \
    }

/* Boolean helper for SLOT1BINFULL().
   right.__class__ is a nontrivial subclass of left.__class__. */
static int method_is_overloaded(PyObject* left, PyObject* right, const char* name) noexcept {
    PyObject* a, *b;
    int ok;

    b = PyObject_GetAttrString((PyObject*)(Py_TYPE(right)), name);
    if (b == NULL) {
        PyErr_Clear();
        /* If right doesn't have it, it's not overloaded */
        return 0;
    }

    a = PyObject_GetAttrString((PyObject*)(Py_TYPE(left)), name);
    if (a == NULL) {
        PyErr_Clear();
        Py_DECREF(b);
        /* If right has it but left doesn't, it's overloaded */
        return 1;
    }

    ok = PyObject_RichCompareBool(a, b, Py_NE);
    Py_DECREF(a);
    Py_DECREF(b);
    if (ok < 0) {
        PyErr_Clear();
        return 0;
    }

    return ok;
}

#define SLOT1BINFULL(FUNCNAME, TESTFUNC, SLOTNAME, OPSTR, ROPSTR)                                                      \
    static PyObject* FUNCNAME(PyObject* self, PyObject* other) noexcept {                                              \
        static PyObject* cache_str, *rcache_str;                                                                       \
        int do_other = Py_TYPE(self) != Py_TYPE(other) && Py_TYPE(other)->tp_as_number != NULL                         \
                       && Py_TYPE(other)->tp_as_number->SLOTNAME == TESTFUNC;                                          \
        if (Py_TYPE(self)->tp_as_number != NULL && Py_TYPE(self)->tp_as_number->SLOTNAME == TESTFUNC) {                \
            PyObject* r;                                                                                               \
            if (do_other && PyType_IsSubtype(Py_TYPE(other), Py_TYPE(self))                                            \
                && method_is_overloaded(self, other, ROPSTR)) {                                                        \
                r = call_maybe(other, ROPSTR, &rcache_str, "(O)", self);                                               \
                if (r != Py_NotImplemented)                                                                            \
                    return r;                                                                                          \
                Py_DECREF(r);                                                                                          \
                do_other = 0;                                                                                          \
            }                                                                                                          \
            r = call_maybe(self, OPSTR, &cache_str, "(O)", other);                                                     \
            if (r != Py_NotImplemented || Py_TYPE(other) == Py_TYPE(self))                                             \
                return r;                                                                                              \
            Py_DECREF(r);                                                                                              \
        }                                                                                                              \
        if (do_other) {                                                                                                \
            return call_maybe(other, ROPSTR, &rcache_str, "(O)", self);                                                \
        }                                                                                                              \
        Py_INCREF(Py_NotImplemented);                                                                                  \
        return Py_NotImplemented;                                                                                      \
    }

#define SLOT1BIN(FUNCNAME, SLOTNAME, OPSTR, ROPSTR) SLOT1BINFULL(FUNCNAME, FUNCNAME, SLOTNAME, OPSTR, ROPSTR)

#define SLOT2(FUNCNAME, OPSTR, ARG1TYPE, ARG2TYPE, ARGCODES)                                                           \
    static PyObject* FUNCNAME(PyObject* self, ARG1TYPE arg1, ARG2TYPE arg2) {                                          \
        static PyObject* cache_str;                                                                                    \
        return call_method(self, OPSTR, &cache_str, "(" ARGCODES ")", arg1, arg2);                                     \
    }

#define slot_mp_length slot_sq_length

SLOT1(slot_mp_subscript, "__getitem__", PyObject*, "O")

static int slot_mp_ass_subscript(PyObject* self, PyObject* key, PyObject* value) noexcept {
    STAT_TIMER(t0, "us_timer_slot_mpasssubscript", SLOT_AVOIDABILITY(self));

    PyObject* res;
    static PyObject* delitem_str, *setitem_str;

    if (value == NULL)
        res = call_method(self, "__delitem__", &delitem_str, "(O)", key);
    else
        res = call_method(self, "__setitem__", &setitem_str, "(OO)", key, value);
    if (res == NULL)
        return -1;
    Py_DECREF(res);
    return 0;
}

SLOT1BIN(slot_nb_add, nb_add, "__add__", "__radd__")
SLOT1BIN(slot_nb_subtract, nb_subtract, "__sub__", "__rsub__")
SLOT1BIN(slot_nb_multiply, nb_multiply, "__mul__", "__rmul__")
SLOT1BIN(slot_nb_divide, nb_divide, "__div__", "__rdiv__")
SLOT1BIN(slot_nb_remainder, nb_remainder, "__mod__", "__rmod__")
SLOT1BIN(slot_nb_divmod, nb_divmod, "__divmod__", "__rdivmod__")

static PyObject* slot_nb_power(PyObject*, PyObject*, PyObject*) noexcept;

SLOT1BINFULL(slot_nb_power_binary, slot_nb_power, nb_power, "__pow__", "__rpow__")

static PyObject* slot_nb_power(PyObject* self, PyObject* other, PyObject* modulus) noexcept {
    STAT_TIMER(t0, "us_timer_slot_nbpower", SLOT_AVOIDABILITY(self));

    static PyObject* pow_str;

    if (modulus == Py_None)
        return slot_nb_power_binary(self, other);
    /* Three-arg power doesn't use __rpow__.  But ternary_op
       can call this when the second argument's type uses
       slot_nb_power, so check before calling self.__pow__. */
    if (Py_TYPE(self)->tp_as_number != NULL && Py_TYPE(self)->tp_as_number->nb_power == slot_nb_power) {
        return call_method(self, "__pow__", &pow_str, "(OO)", other, modulus);
    }
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
}

SLOT0(slot_nb_negative, "__neg__")
SLOT0(slot_nb_positive, "__pos__")
SLOT0(slot_nb_absolute, "__abs__")

static int slot_nb_nonzero(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_nbnonzero", SLOT_AVOIDABILITY(self));

    PyObject* func, *args;
    static PyObject* nonzero_str, *len_str;
    int result = -1;
    int using_len = 0;

    func = lookup_maybe(self, "__nonzero__", &nonzero_str);
    if (func == NULL) {
        if (PyErr_Occurred())
            return -1;
        func = lookup_maybe(self, "__len__", &len_str);
        if (func == NULL)
            return PyErr_Occurred() ? -1 : 1;
        using_len = 1;
    }
    args = PyTuple_New(0);
    if (args != NULL) {
        PyObject* temp = PyObject_Call(func, args, NULL);
        Py_DECREF(args);
        if (temp != NULL) {
            if (PyInt_CheckExact(temp) || PyBool_Check(temp))
                result = PyObject_IsTrue(temp);
            else {
                PyErr_Format(PyExc_TypeError, "%s should return "
                                              "bool or int, returned %s",
                             (using_len ? "__len__" : "__nonzero__"), temp->cls->tp_name);
                result = -1;
            }
            Py_DECREF(temp);
        }
    }
    Py_DECREF(func);
    return result;
}

static PyObject* slot_nb_index(PyObject* self) noexcept {
    STAT_TIMER(t0, "us_timer_slot_nbindex", SLOT_AVOIDABILITY(self));

    static PyObject* index_str;
    return call_method(self, "__index__", &index_str, "()");
}


SLOT0(slot_nb_invert, "__invert__")
SLOT1BIN(slot_nb_lshift, nb_lshift, "__lshift__", "__rlshift__")
SLOT1BIN(slot_nb_rshift, nb_rshift, "__rshift__", "__rrshift__")
SLOT1BIN(slot_nb_and, nb_and, "__and__", "__rand__")
SLOT1BIN(slot_nb_xor, nb_xor, "__xor__", "__rxor__")
SLOT1BIN(slot_nb_or, nb_or, "__or__", "__ror__")

static int slot_nb_coerce(PyObject** a, PyObject** b) noexcept {
    STAT_TIMER(t0, "us_timer_slot_nbcoerce", std::min(SLOT_AVOIDABILITY(*a), SLOT_AVOIDABILITY(*b)));

    static PyObject* coerce_str;
    PyObject* self = *a, * other = *b;

    if (self->cls->tp_as_number != NULL && self->cls->tp_as_number->nb_coerce == slot_nb_coerce) {
        PyObject* r;
        r = call_maybe(self, "__coerce__", &coerce_str, "(O)", other);
        if (r == NULL)
            return -1;
        if (r == Py_NotImplemented) {
            Py_DECREF(r);
        } else {
            if (!PyTuple_Check(r) || PyTuple_GET_SIZE(r) != 2) {
                PyErr_SetString(PyExc_TypeError, "__coerce__ didn't return a 2-tuple");
                Py_DECREF(r);
                return -1;
            }
            *a = PyTuple_GET_ITEM(r, 0);
            Py_INCREF(*a);
            *b = PyTuple_GET_ITEM(r, 1);
            Py_INCREF(*b);
            Py_DECREF(r);
            return 0;
        }
    }
    if (other->cls->tp_as_number != NULL && other->cls->tp_as_number->nb_coerce == slot_nb_coerce) {
        PyObject* r;
        r = call_maybe(other, "__coerce__", &coerce_str, "(O)", self);
        if (r == NULL)
            return -1;
        if (r == Py_NotImplemented) {
            Py_DECREF(r);
            return 1;
        }
        if (!PyTuple_Check(r) || PyTuple_GET_SIZE(r) != 2) {
            PyErr_SetString(PyExc_TypeError, "__coerce__ didn't return a 2-tuple");
            Py_DECREF(r);
            return -1;
        }
        *a = PyTuple_GET_ITEM(r, 1);
        Py_INCREF(*a);
        *b = PyTuple_GET_ITEM(r, 0);
        Py_INCREF(*b);
        Py_DECREF(r);
        return 0;
    }
    return 1;
}

SLOT0(slot_nb_int, "__int__")
SLOT0(slot_nb_long, "__long__")
SLOT0(slot_nb_float, "__float__")
SLOT0(slot_nb_oct, "__oct__")
SLOT0(slot_nb_hex, "__hex__")
SLOT1(slot_nb_inplace_add, "__iadd__", PyObject*, "O")
SLOT1(slot_nb_inplace_subtract, "__isub__", PyObject*, "O")
SLOT1(slot_nb_inplace_multiply, "__imul__", PyObject*, "O")
SLOT1(slot_nb_inplace_divide, "__idiv__", PyObject*, "O")
SLOT1(slot_nb_inplace_remainder, "__imod__", PyObject*, "O")
/* Can't use SLOT1 here, because nb_inplace_power is ternary */
static PyObject* slot_nb_inplace_power(PyObject* self, PyObject* arg1, PyObject* arg2) {
    STAT_TIMER(t0, "us_timer_slot_nbinplacepower", SLOT_AVOIDABILITY(self));

    static PyObject* cache_str;
    return call_method(self, "__ipow__", &cache_str, "("
                                                     "O"
                                                     ")",
                       arg1);
}
SLOT1(slot_nb_inplace_lshift, "__ilshift__", PyObject*, "O")
SLOT1(slot_nb_inplace_rshift, "__irshift__", PyObject*, "O")
SLOT1(slot_nb_inplace_and, "__iand__", PyObject*, "O")
SLOT1(slot_nb_inplace_xor, "__ixor__", PyObject*, "O")
SLOT1(slot_nb_inplace_or, "__ior__", PyObject*, "O")
SLOT1BIN(slot_nb_floor_divide, nb_floor_divide, "__floordiv__", "__rfloordiv__")
SLOT1BIN(slot_nb_true_divide, nb_true_divide, "__truediv__", "__rtruediv__")
SLOT1(slot_nb_inplace_floor_divide, "__ifloordiv__", PyObject*, "O")
SLOT1(slot_nb_inplace_true_divide, "__itruediv__", PyObject*, "O")

typedef wrapper_def slotdef;

static void** slotptr(BoxedClass* type, int offset) noexcept {
    // We use the index into PyHeapTypeObject as the canonical way to represent offsets, even though we are not
    // (currently) using that object representation

    // copied from CPython:
    /* Note: this depends on the order of the members of PyHeapTypeObject! */
    assert(offset >= 0);
    assert((size_t)offset < offsetof(PyHeapTypeObject, as_buffer));
    char* ptr;
    if ((size_t)offset >= offsetof(PyHeapTypeObject, as_sequence)) {
        ptr = (char*)type->tp_as_sequence;
        offset -= offsetof(PyHeapTypeObject, as_sequence);
    } else if ((size_t)offset >= offsetof(PyHeapTypeObject, as_mapping)) {
        ptr = (char*)type->tp_as_mapping;
        offset -= offsetof(PyHeapTypeObject, as_mapping);
    } else if ((size_t)offset >= offsetof(PyHeapTypeObject, as_number)) {
        ptr = (char*)type->tp_as_number;
        offset -= offsetof(PyHeapTypeObject, as_number);
    } else {
        ptr = (char*)type;
    }
    if (ptr != NULL)
        ptr += offset;
    return (void**)ptr;
}

// Copied from CPython:
#define TPSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), 0, NULL }
#define TPPSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                    \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), PyWrapperFlag_PYSTON, NULL }
#define FLSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC, FLAGS)                                                              \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), FLAGS, NULL }
#define ETSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    { NAME, offsetof(PyHeapTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), 0, NULL }
#define SQSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC) ETSLOT(NAME, as_sequence.SLOT, FUNCTION, WRAPPER, DOC)
#define MPSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC) ETSLOT(NAME, as_mapping.SLOT, FUNCTION, WRAPPER, DOC)
#define NBSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC) ETSLOT(NAME, as_number.SLOT, FUNCTION, WRAPPER, DOC)
#define UNSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, WRAPPER, "x." NAME "() <==> " DOC)
#define IBSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, WRAPPER, "x." NAME "(y) <==> x" DOC "y")
#define BINSLOT(NAME, SLOT, FUNCTION, DOC)                                                                             \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_l, "x." NAME "(y) <==> x" DOC "y")
#define RBINSLOT(NAME, SLOT, FUNCTION, DOC)                                                                            \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_r, "x." NAME "(y) <==> y" DOC "x")
#define BINSLOTNOTINFIX(NAME, SLOT, FUNCTION, DOC)                                                                     \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_l, "x." NAME "(y) <==> " DOC)
#define RBINSLOTNOTINFIX(NAME, SLOT, FUNCTION, DOC)                                                                    \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_r, "x." NAME "(y) <==> " DOC)

static slotdef slotdefs[]
    = { TPSLOT("__getattribute__", tp_getattr, NULL, NULL, ""),
        TPSLOT("__getattr__", tp_getattr, NULL, NULL, ""),
        TPSLOT("__setattr__", tp_setattr, NULL, NULL, ""),
        TPSLOT("__delattr__", tp_setattr, NULL, NULL, ""),
        TPSLOT("__cmp__", tp_compare, _PyObject_SlotCompare, wrap_cmpfunc, "x.__cmp__(y) <==> cmp(x,y)"),

        TPSLOT("__repr__", tp_repr, slot_tp_repr, wrap_unaryfunc, "x.__repr__() <==> repr(x)"),
        TPSLOT("__hash__", tp_hash, slot_tp_hash, wrap_hashfunc, "x.__hash__() <==> hash(x)"),
        FLSLOT("__call__", tp_call, slot_tp_call, (wrapperfunc)wrap_call, "x.__call__(...) <==> x(...)",
               PyWrapperFlag_KEYWORDS),
        TPSLOT("__str__", tp_str, slot_tp_str, wrap_unaryfunc, "x.__str__() <==> str(x)"),

        TPSLOT("__getattribute__", tp_getattro, slot_tp_getattr_hook, wrap_binaryfunc,
               "x.__getattribute__('name') <==> x.name"),
        TPSLOT("__getattr__", tp_getattro, slot_tp_getattr_hook, NULL, ""),
        TPSLOT("__setattr__", tp_setattro, slot_tp_setattro, wrap_setattr,
               "x.__setattr__('name', value) <==> x.name = value"),
        TPSLOT("__delattr__", tp_setattro, slot_tp_setattro, wrap_delattr, "x.__delattr__('name') <==> del x.name"),

        TPSLOT("__lt__", tp_richcompare, slot_tp_richcompare, richcmp_lt, "x.__lt__(y) <==> x<y"),
        TPSLOT("__le__", tp_richcompare, slot_tp_richcompare, richcmp_le, "x.__le__(y) <==> x<=y"),
        TPSLOT("__eq__", tp_richcompare, slot_tp_richcompare, richcmp_eq, "x.__eq__(y) <==> x==y"),
        TPSLOT("__ne__", tp_richcompare, slot_tp_richcompare, richcmp_ne, "x.__ne__(y) <==> x!=y"),
        TPSLOT("__gt__", tp_richcompare, slot_tp_richcompare, richcmp_gt, "x.__gt__(y) <==> x>y"),
        TPSLOT("__ge__", tp_richcompare, slot_tp_richcompare, richcmp_ge, "x.__ge__(y) <==> x>=y"),

        TPSLOT("__iter__", tp_iter, slot_tp_iter, wrap_unaryfunc, "x.__iter__() <==> iter(x)"),
        TPSLOT("next", tp_iternext, slot_tp_iternext, wrap_next, "x.next() -> the next value, or raise StopIteration"),
        TPSLOT("__get__", tp_descr_get, slot_tp_descr_get, wrap_descr_get, "descr.__get__(obj[, type]) -> value"),

        FLSLOT("__init__", tp_init, slot_tp_init, (wrapperfunc)wrap_init, "x.__init__(...) initializes x; "
                                                                          "see help(type(x)) for signature",
               PyWrapperFlag_KEYWORDS),
        TPSLOT("__new__", tp_new, slot_tp_new, NULL, ""),
        TPSLOT("__del__", tp_del, slot_tp_del, NULL, ""),
        FLSLOT("__class__", has___class__, NULL, NULL, "", PyWrapperFlag_BOOL),
        FLSLOT("__instancecheck__", has_instancecheck, NULL, NULL, "", PyWrapperFlag_BOOL),
        FLSLOT("__getattribute__", has_getattribute, NULL, NULL, "", PyWrapperFlag_BOOL),
        TPPSLOT("__hasnext__", tpp_hasnext, slotTppHasnext, wrapInquirypred, "hasnext"),

        BINSLOT("__add__", nb_add, slot_nb_add, "+"),                             // [force clang-format to line break]
        RBINSLOT("__radd__", nb_add, slot_nb_add, "+"),                           //
        BINSLOT("__sub__", nb_subtract, slot_nb_subtract, "-"),                   //
        RBINSLOT("__rsub__", nb_subtract, slot_nb_subtract, "-"),                 //
        BINSLOT("__mul__", nb_multiply, slot_nb_multiply, "*"),                   //
        RBINSLOT("__rmul__", nb_multiply, slot_nb_multiply, "*"),                 //
        BINSLOT("__div__", nb_divide, slot_nb_divide, "/"),                       //
        RBINSLOT("__rdiv__", nb_divide, slot_nb_divide, "/"),                     //
        BINSLOT("__mod__", nb_remainder, slot_nb_remainder, "%"),                 //
        RBINSLOT("__rmod__", nb_remainder, slot_nb_remainder, "%"),               //
        BINSLOTNOTINFIX("__divmod__", nb_divmod, slot_nb_divmod, "divmod(x, y)"), //
        RBINSLOTNOTINFIX("__rdivmod__", nb_divmod, slot_nb_divmod, "divmod(y, x)"),                                 //
        NBSLOT("__pow__", nb_power, slot_nb_power, wrap_ternaryfunc, "x.__pow__(y[, z]) <==> pow(x, y[, z])"),      //
        NBSLOT("__rpow__", nb_power, slot_nb_power, wrap_ternaryfunc_r, "y.__rpow__(x[, z]) <==> pow(x, y[, z])"),  //
        UNSLOT("__neg__", nb_negative, slot_nb_negative, wrap_unaryfunc, "-x"),                                     //
        UNSLOT("__pos__", nb_positive, slot_nb_positive, wrap_unaryfunc, "+x"),                                     //
        UNSLOT("__abs__", nb_absolute, slot_nb_absolute, wrap_unaryfunc, "abs(x)"),                                 //
        UNSLOT("__nonzero__", nb_nonzero, slot_nb_nonzero, wrap_inquirypred, "x != 0"),                             //
        UNSLOT("__invert__", nb_invert, slot_nb_invert, wrap_unaryfunc, "~x"),                                      //
        BINSLOT("__lshift__", nb_lshift, slot_nb_lshift, "<<"),                                                     //
        RBINSLOT("__rlshift__", nb_lshift, slot_nb_lshift, "<<"),                                                   //
        BINSLOT("__rshift__", nb_rshift, slot_nb_rshift, ">>"),                                                     //
        RBINSLOT("__rrshift__", nb_rshift, slot_nb_rshift, ">>"),                                                   //
        BINSLOT("__and__", nb_and, slot_nb_and, "&"),                                                               //
        RBINSLOT("__rand__", nb_and, slot_nb_and, "&"),                                                             //
        BINSLOT("__xor__", nb_xor, slot_nb_xor, "^"),                                                               //
        RBINSLOT("__rxor__", nb_xor, slot_nb_xor, "^"),                                                             //
        BINSLOT("__or__", nb_or, slot_nb_or, "|"),                                                                  //
        RBINSLOT("__ror__", nb_or, slot_nb_or, "|"),                                                                //
        NBSLOT("__coerce__", nb_coerce, slot_nb_coerce, wrap_coercefunc, "x.__coerce__(y) <==> coerce(x, y)"),      //
        UNSLOT("__int__", nb_int, slot_nb_int, wrap_unaryfunc, "int(x)"),                                           //
        UNSLOT("__long__", nb_long, slot_nb_long, wrap_unaryfunc, "long(x)"),                                       //
        UNSLOT("__float__", nb_float, slot_nb_float, wrap_unaryfunc, "float(x)"),                                   //
        UNSLOT("__oct__", nb_oct, slot_nb_oct, wrap_unaryfunc, "oct(x)"),                                           //
        UNSLOT("__hex__", nb_hex, slot_nb_hex, wrap_unaryfunc, "hex(x)"),                                           //
        IBSLOT("__iadd__", nb_inplace_add, slot_nb_inplace_add, wrap_binaryfunc, "+="),                             //
        IBSLOT("__isub__", nb_inplace_subtract, slot_nb_inplace_subtract, wrap_binaryfunc, "-="),                   //
        IBSLOT("__imul__", nb_inplace_multiply, slot_nb_inplace_multiply, wrap_binaryfunc, "*="),                   //
        IBSLOT("__idiv__", nb_inplace_divide, slot_nb_inplace_divide, wrap_binaryfunc, "/="),                       //
        IBSLOT("__imod__", nb_inplace_remainder, slot_nb_inplace_remainder, wrap_binaryfunc, "%="),                 //
        IBSLOT("__ipow__", nb_inplace_power, slot_nb_inplace_power, wrap_binaryfunc, "**="),                        //
        IBSLOT("__ilshift__", nb_inplace_lshift, slot_nb_inplace_lshift, wrap_binaryfunc, "<<="),                   //
        IBSLOT("__irshift__", nb_inplace_rshift, slot_nb_inplace_rshift, wrap_binaryfunc, ">>="),                   //
        IBSLOT("__iand__", nb_inplace_and, slot_nb_inplace_and, wrap_binaryfunc, "&="),                             //
        IBSLOT("__ixor__", nb_inplace_xor, slot_nb_inplace_xor, wrap_binaryfunc, "^="),                             //
        IBSLOT("__ior__", nb_inplace_or, slot_nb_inplace_or, wrap_binaryfunc, "|="),                                //
        BINSLOT("__floordiv__", nb_floor_divide, slot_nb_floor_divide, "//"),                                       //
        RBINSLOT("__rfloordiv__", nb_floor_divide, slot_nb_floor_divide, "//"),                                     //
        BINSLOT("__truediv__", nb_true_divide, slot_nb_true_divide, "/"),                                           //
        RBINSLOT("__rtruediv__", nb_true_divide, slot_nb_true_divide, "/"),                                         //
        IBSLOT("__ifloordiv__", nb_inplace_floor_divide, slot_nb_inplace_floor_divide, wrap_binaryfunc, "//"),      //
        IBSLOT("__itruediv__", nb_inplace_true_divide, slot_nb_inplace_true_divide, wrap_binaryfunc, "/"),          //
        NBSLOT("__index__", nb_index, slot_nb_index, wrap_unaryfunc, "x[y:z] <==> x[y.__index__():z.__index__()]"), //

        MPSLOT("__len__", mp_length, slot_mp_length, wrap_lenfunc, "x.__len__() <==> len(x)"),
        MPSLOT("__getitem__", mp_subscript, slot_mp_subscript, wrap_binaryfunc, "x.__getitem__(y) <==> x[y]"),
        MPSLOT("__setitem__", mp_ass_subscript, slot_mp_ass_subscript, wrap_objobjargproc,
               "x.__setitem__(i, y) <==> x[i]=y"),
        MPSLOT("__delitem__", mp_ass_subscript, slot_mp_ass_subscript, wrap_delitem, "x.__delitem__(y) <==> del x[y]"),

        SQSLOT("__len__", sq_length, slot_sq_length, wrap_lenfunc, "x.__len__() <==> len(x)"),
        /* Heap types defining __add__/__mul__ have sq_concat/sq_repeat == NULL.
           The logic in abstract.c always falls back to nb_add/nb_multiply in
           this case.  Defining both the nb_* and the sq_* slots to call the
           user-defined methods has unexpected side-effects, as shown by
           test_descr.notimplemented() */
        SQSLOT("__add__", sq_concat, NULL, wrap_binaryfunc, "x.__add__(y) <==> x+y"),
        SQSLOT("__mul__", sq_repeat, NULL, wrap_indexargfunc, "x.__mul__(n) <==> x*n"),
        SQSLOT("__rmul__", sq_repeat, NULL, wrap_indexargfunc, "x.__rmul__(n) <==> n*x"),
        SQSLOT("__getitem__", sq_item, slot_sq_item, wrap_sq_item, "x.__getitem__(y) <==> x[y]"),
        SQSLOT("__getslice__", sq_slice, slot_sq_slice, wrap_ssizessizeargfunc, "x.__getslice__(i, j) <==> x[i:j]\n\
           \n\
           Use of negative indices is not supported."),
        SQSLOT("__setitem__", sq_ass_item, slot_sq_ass_item, wrap_sq_setitem, "x.__setitem__(i, y) <==> x[i]=y"),
        SQSLOT("__delitem__", sq_ass_item, slot_sq_ass_item, wrap_sq_delitem, "x.__delitem__(y) <==> del x[y]"),
        SQSLOT("__setslice__", sq_ass_slice, slot_sq_ass_slice, wrap_ssizessizeobjargproc,
               "x.__setslice__(i, j, y) <==> x[i:j]=y\n\
           \n\
           Use  of negative indices is not supported."),
        SQSLOT("__delslice__", sq_ass_slice, slot_sq_ass_slice, wrap_delslice, "x.__delslice__(i, j) <==> del x[i:j]\n\
           \n\
           Use of negative indices is not supported."),
        SQSLOT("__contains__", sq_contains, slot_sq_contains, wrap_objobjproc, "x.__contains__(y) <==> y in x"),
        SQSLOT("__iadd__", sq_inplace_concat, NULL, wrap_binaryfunc, "x.__iadd__(y) <==> x+=y"),
        SQSLOT("__imul__", sq_inplace_repeat, NULL, wrap_indexargfunc, "x.__imul__(y) <==> x*=y"),
        { "", 0, NULL, NULL, "", 0, NULL } };

static void init_slotdefs() noexcept {
    static bool initialized = false;
    if (initialized)
        return;

    for (int i = 0; i < sizeof(slotdefs) / sizeof(slotdefs[0]); i++) {
        slotdefs[i].name_strobj = internStringImmortal(slotdefs[i].name.data());

        if (i > 0) {
            if (!slotdefs[i].name.size())
                continue;

#ifndef NDEBUG
            if (slotdefs[i - 1].offset > slotdefs[i].offset) {
                printf("slotdef for %s in the wrong place\n", slotdefs[i - 1].name.data());
                for (int j = i; j < sizeof(slotdefs) / sizeof(slotdefs[0]); j++) {
                    if (slotdefs[i - 1].offset <= slotdefs[j].offset) {
                        printf("Should go before %s\n", slotdefs[j].name.data());
                        break;
                    }
                }
            }
#endif
            ASSERT(slotdefs[i].offset >= slotdefs[i - 1].offset, "%d %s", i, slotdefs[i - 1].name.data());
        }
    }

    initialized = true;
}

/* Length of array of slotdef pointers used to store slots with the
   same __name__.  There should be at most MAX_EQUIV-1 slotdef entries with
   the same __name__, for any __name__. Since that's a static property, it is
   appropriate to declare fixed-size arrays for this. */
#define MAX_EQUIV 10

/* Return a slot pointer for a given name, but ONLY if the attribute has
   exactly one slot function.  The name must be an interned string. */
static void** resolve_slotdups(PyTypeObject* type, const std::string& name) noexcept {
    /* XXX Maybe this could be optimized more -- but is it worth it? */

    /* pname and ptrs act as a little cache */
    static std::string pname;
    static slotdef* ptrs[MAX_EQUIV];
    slotdef* p, **pp;
    void** res, **ptr;

    if (pname != name) {
        /* Collect all slotdefs that match name into ptrs. */
        pname = name;
        pp = ptrs;
        for (p = slotdefs; p->name.size() != 0; p++) {
            if (p->name == name)
                *pp++ = p;
        }
        *pp = NULL;
    }

    /* Look in all matching slots of the type; if exactly one of these has
       a filled-in slot, return its value.      Otherwise return NULL. */
    res = NULL;
    for (pp = ptrs; *pp; pp++) {
        ptr = slotptr(type, (*pp)->offset);
        if (ptr == NULL || *ptr == NULL)
            continue;
        if (res != NULL)
            return NULL;
        res = ptr;
    }
    return res;
}

static const slotdef* update_one_slot(BoxedClass* type, const slotdef* p) noexcept {
    assert(p->name.size() != 0);

    PyObject* descr;
    BoxedWrapperDescriptor* d;
    void* generic = NULL, * specific = NULL;
    int use_generic = 0;
    int offset = p->offset;
    void** ptr = slotptr(type, offset);

    if (ptr == NULL) {
        do {
            ++p;
        } while (p->offset == offset);
        return p;
    }

    do {
        descr = typeLookup(type, p->name_strobj, NULL);

        if (p->flags & PyWrapperFlag_BOOL) {
            // We are supposed to iterate over each slotdef; for now just assert that
            // there was only one:
            assert((p + 1)->offset > p->offset);

            static BoxedString* class_str = internStringImmortal("__class__");
            if (p->name_strobj == class_str) {
                if (descr == object_cls->getattr(class_str))
                    descr = NULL;
            }

            static BoxedString* getattribute_str = internStringImmortal("__getattribute__");
            if (p->name_strobj == getattribute_str) {
                if (descr && descr->cls == wrapperdescr_cls
                    && ((BoxedWrapperDescriptor*)descr)->wrapped == PyObject_GenericGetAttr)
                    descr = NULL;
            }

            *(bool*)ptr = (bool)descr;
            return p + 1;
        }

        if (descr == NULL) {
            if (ptr == (void**)&type->tp_iternext) {
                specific = (void*)_PyObject_NextNotImplemented;
            }
            continue;
        }
        if (Py_TYPE(descr) == wrapperdescr_cls
            && ((BoxedWrapperDescriptor*)descr)->wrapper->name == std::string(p->name)) {
            void** tptr = resolve_slotdups(type, p->name);
            if (tptr == NULL || tptr == ptr)
                generic = p->function;
            d = (BoxedWrapperDescriptor*)descr;
            if (d->wrapper->wrapper == p->wrapper && PyType_IsSubtype(type, d->type)
                && ((d->wrapper->flags & PyWrapperFlag_PYSTON) == (p->flags & PyWrapperFlag_PYSTON))) {
                if (specific == NULL || specific == d->wrapped)
                    specific = d->wrapped;
                else
                    use_generic = 1;
            }
        } else if (Py_TYPE(descr) == &PyCFunction_Type && PyCFunction_GET_FUNCTION(descr) == (PyCFunction)tp_new_wrapper
                   && ptr == (void**)&type->tp_new) {
            /* The __new__ wrapper is not a wrapper descriptor,
               so must be special-cased differently.
               If we don't do this, creating an instance will
               always use slot_tp_new which will look up
               __new__ in the MRO which will call tp_new_wrapper
               which will look through the base classes looking
               for a static base and call its tp_new (usually
               PyType_GenericNew), after performing various
               sanity checks and constructing a new argument
               list.  Cut all that nonsense short -- this speeds
               up instance creation tremendously. */
            specific = (void*)type->tp_new;
            /* XXX I'm not 100% sure that there isn't a hole
               in this reasoning that requires additional
               sanity checks.  I'll buy the first person to
               point out a bug in this reasoning a beer. */
        } else if (offset == offsetof(BoxedClass, tp_descr_get) && descr->cls == function_cls
                   && static_cast<BoxedFunction*>(descr)->f->always_use_version) {
            type->tpp_descr_get = (descrgetfunc) static_cast<BoxedFunction*>(descr)->f->always_use_version->code;
            specific = (void*)slot_tp_tpp_descr_get;
        } else if (descr == Py_None && ptr == (void**)&type->tp_hash) {
            /* We specifically allow __hash__ to be set to None
               to prevent inheritance of the default
               implementation from object.__hash__ */
            specific = (void*)PyObject_HashNotImplemented;
        } else {
            use_generic = 1;
            generic = p->function;
        }
    } while ((++p)->offset == offset);

    if (specific && !use_generic)
        *ptr = specific;
    else
        *ptr = generic;
    return p;
}

/* In the type, update the slots whose slotdefs are gathered in the pp array.
   This is a callback for update_subclasses(). */
static int update_slots_callback(PyTypeObject* type, void* data) noexcept {
    slotdef** pp = (slotdef**)data;

    for (; *pp; pp++)
        update_one_slot(type, *pp);
    return 0;
}

static int update_subclasses(PyTypeObject* type, PyObject* name, update_callback callback, void* data) noexcept;
static int recurse_down_subclasses(PyTypeObject* type, PyObject* name, update_callback callback, void* data) noexcept;

bool update_slot(BoxedClass* type, llvm::StringRef attr) noexcept {
    slotdef* ptrs[MAX_EQUIV];
    slotdef* p;
    slotdef** pp;
    int offset;

    /* Clear the VALID_VERSION flag of 'type' and all its
       subclasses.  This could possibly be unified with the
       update_subclasses() recursion below, but carefully:
       they each have their own conditions on which to stop
       recursing into subclasses. */
    PyType_Modified(type);

    init_slotdefs();
    pp = ptrs;
    for (p = slotdefs; p->name.size() != 0; p++) {
        /* XXX assume name is interned! */
        if (p->name == attr)
            *pp++ = p;
    }
    *pp = NULL;
    for (pp = ptrs; *pp; pp++) {
        p = *pp;
        offset = p->offset;
        while (p > slotdefs && (p - 1)->offset == offset)
            --p;
        *pp = p;
    }
    if (ptrs[0] == NULL)
        return false; /* Not an attribute that affects any slots */
    int r = update_subclasses(type, boxString(attr), update_slots_callback, (void*)ptrs);

    // TODO this is supposed to be a CAPI function!
    if (r)
        throwCAPIException();
    return true;
}

void fixup_slot_dispatchers(BoxedClass* self) noexcept {
    init_slotdefs();

    const slotdef* p = slotdefs;
    while (p->name.size() != 0)
        p = update_one_slot(self, p);
}

static int update_subclasses(PyTypeObject* type, PyObject* name, update_callback callback, void* data) noexcept {
    if (callback(type, data) < 0)
        return -1;
    return recurse_down_subclasses(type, name, callback, data);
}

static int recurse_down_subclasses(PyTypeObject* type, PyObject* name, update_callback callback, void* data) noexcept {
    PyTypeObject* subclass;
    PyObject* ref, *subclasses, *dict;
    Py_ssize_t i, n;

    subclasses = type->tp_subclasses;
    if (subclasses == NULL)
        return 0;
    assert(PyList_Check(subclasses));
    n = PyList_GET_SIZE(subclasses);
    for (i = 0; i < n; i++) {
        ref = PyList_GET_ITEM(subclasses, i);
        assert(PyWeakref_CheckRef(ref));
        subclass = (PyTypeObject*)PyWeakref_GET_OBJECT(ref);
        assert(subclass != NULL);
        if ((PyObject*)subclass == Py_None)
            continue;
        assert(PyType_Check(subclass));
        /* Avoid recursing down into unaffected classes */
        dict = subclass->tp_dict;
        if (dict != NULL && PyDict_Check(dict) && PyDict_GetItem(dict, name) != NULL)
            continue;
        if (update_subclasses(subclass, name, callback, data) < 0)
            return -1;
    }
    return 0;
}

static PyObject* tp_new_wrapper(PyTypeObject* self, BoxedTuple* args, Box* kwds) noexcept {
    RELEASE_ASSERT(isSubclass(self->cls, type_cls), "");

    // ASSERT(self->tp_new != Py_CallPythonNew, "going to get in an infinite loop");

    RELEASE_ASSERT(args->cls == tuple_cls, "");
    RELEASE_ASSERT(!kwds || kwds->cls == dict_cls, "");
    RELEASE_ASSERT(args->size() >= 1, "");

    BoxedClass* subtype = static_cast<BoxedClass*>(args->elts[0]);
    RELEASE_ASSERT(isSubclass(subtype->cls, type_cls), "");
    RELEASE_ASSERT(isSubclass(subtype, self), "");

    BoxedTuple* new_args = BoxedTuple::create(args->size() - 1, &args->elts[1]);

    return self->tp_new(subtype, new_args, kwds);
}

static struct PyMethodDef tp_new_methoddef[] = { { "__new__", (PyCFunction)tp_new_wrapper, METH_VARARGS | METH_KEYWORDS,
                                                   PyDoc_STR("T.__new__(S, ...) -> "
                                                             "a new object with type S, a subtype of T") },
                                                 { 0, 0, 0, 0 } };

static void add_tp_new_wrapper(BoxedClass* type) noexcept {
    static BoxedString* new_str = internStringImmortal("__new__");
    if (type->getattr(new_str))
        return;

    type->giveAttr(new_str, new BoxedCApiFunction(tp_new_methoddef, type));
}

void add_operators(BoxedClass* cls) noexcept {
    init_slotdefs();

    for (const slotdef& p : slotdefs) {
        if (!p.wrapper)
            continue;

        void** ptr = slotptr(cls, p.offset);

        if (!ptr || !*ptr)
            continue;
        if (cls->getattr(p.name_strobj))
            continue;

        if (*ptr == PyObject_HashNotImplemented) {
            cls->giveAttr(p.name_strobj, None);
        } else {
            cls->giveAttr(p.name_strobj, new BoxedWrapperDescriptor(&p, cls, *ptr));
        }
    }

    if (cls->tp_new)
        add_tp_new_wrapper(cls);
}

static void type_mro_modified(PyTypeObject* type, PyObject* bases) {
    /*
       Check that all base classes or elements of the mro of type are
       able to be cached.  This function is called after the base
       classes or mro of the type are altered.

       Unset HAVE_VERSION_TAG and VALID_VERSION_TAG if the type
       inherits from an old-style class, either directly or if it
       appears in the MRO of a new-style class.  No support either for
       custom MROs that include types that are not officially super
       types.

       Called from mro_internal, which will subsequently be called on
       each subclass when their mro is recursively updated.
     */
    Py_ssize_t i, n;
    int clear = 0;

    if (!PyType_HasFeature(type, Py_TPFLAGS_HAVE_VERSION_TAG))
        return;

    n = PyTuple_GET_SIZE(bases);
    for (i = 0; i < n; i++) {
        PyObject* b = PyTuple_GET_ITEM(bases, i);
        PyTypeObject* cls;

        if (!PyType_Check(b)) {
            clear = 1;
            break;
        }

        cls = (PyTypeObject*)b;

        if (!PyType_HasFeature(cls, Py_TPFLAGS_HAVE_VERSION_TAG) || !PyType_IsSubtype(type, cls)) {
            clear = 1;
            break;
        }
    }

    if (clear)
        type->tp_flags &= ~(Py_TPFLAGS_HAVE_VERSION_TAG | Py_TPFLAGS_VALID_VERSION_TAG);
}

static int extra_ivars(PyTypeObject* type, PyTypeObject* base) noexcept {
    size_t t_size = type->tp_basicsize;
    size_t b_size = base->tp_basicsize;

    assert(t_size >= b_size); /* Else type smaller than base! */
    if (type->tp_itemsize || base->tp_itemsize) {
        /* If itemsize is involved, stricter rules */
        return t_size != b_size || type->tp_itemsize != base->tp_itemsize;
    }
    if (type->tp_weaklistoffset && base->tp_weaklistoffset == 0 && type->tp_weaklistoffset + sizeof(PyObject*) == t_size
        && type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        t_size -= sizeof(PyObject*);
    if (type->tp_dictoffset && base->tp_dictoffset == 0 && type->tp_dictoffset + sizeof(PyObject*) == t_size
        && type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        t_size -= sizeof(PyObject*);

    // Pyston change:
    if (type->instancesHaveHCAttrs() && !base->instancesHaveHCAttrs())
        t_size -= sizeof(HCAttrs);

    return t_size != b_size;
}

static PyTypeObject* solid_base(PyTypeObject* type) noexcept {
    PyTypeObject* base;

    if (type->tp_base)
        base = solid_base(type->tp_base);
    else
        base = object_cls;
    if (extra_ivars(type, base))
        return type;
    else
        return base;
}

PyTypeObject* best_base(PyObject* bases) noexcept {
    Py_ssize_t i, n;
    PyTypeObject* base, *winner, *candidate, *base_i;
    PyObject* base_proto;

    assert(PyTuple_Check(bases));
    n = PyTuple_GET_SIZE(bases);
    assert(n > 0);
    base = NULL;
    winner = NULL;
    for (i = 0; i < n; i++) {
        base_proto = PyTuple_GET_ITEM(bases, i);
        if (PyClass_Check(base_proto))
            continue;
        if (!PyType_Check(base_proto)) {
            PyErr_SetString(PyExc_TypeError, "bases must be types");
            return NULL;
        }
        base_i = (PyTypeObject*)base_proto;

        // Pyston change: we require things are already ready
        if (base_i->tp_dict == NULL) {
            assert(base_i->is_pyston_class);
#if 0
            if (PyType_Ready(base_i) < 0)
                return NULL;
#endif
        }

        candidate = solid_base(base_i);
        if (winner == NULL) {
            winner = candidate;
            base = base_i;
        } else if (PyType_IsSubtype(winner, candidate))
            ;
        else if (PyType_IsSubtype(candidate, winner)) {
            winner = candidate;
            base = base_i;
        } else {
            PyErr_SetString(PyExc_TypeError, "multiple bases have "
                                             "instance lay-out conflict");
            return NULL;
        }
    }
    if (base == NULL)
        PyErr_SetString(PyExc_TypeError, "a new-style class can't have only classic bases");
    return base;
}

static int fill_classic_mro(PyObject* mro, PyObject* cls) {
    PyObject* bases, *base;
    Py_ssize_t i, n;

    assert(PyList_Check(mro));
    assert(PyClass_Check(cls));
    i = PySequence_Contains(mro, cls);
    if (i < 0)
        return -1;
    if (!i) {
        if (PyList_Append(mro, cls) < 0)
            return -1;
    }

    bases = ((BoxedClassobj*)cls)->bases;
    assert(bases && PyTuple_Check(bases));
    n = PyTuple_GET_SIZE(bases);
    for (i = 0; i < n; i++) {
        base = PyTuple_GET_ITEM(bases, i);
        if (fill_classic_mro(mro, base) < 0)
            return -1;
    }
    return 0;
}

static PyObject* classic_mro(PyObject* cls) {
    PyObject* mro;

    assert(PyClass_Check(cls));
    mro = PyList_New(0);
    if (mro != NULL) {
        if (fill_classic_mro(mro, cls) == 0)
            return mro;
        Py_DECREF(mro);
    }
    return NULL;
}

/*
    Method resolution order algorithm C3 described in
    "A Monotonic Superclass Linearization for Dylan",
    by Kim Barrett, Bob Cassel, Paul Haahr,
    David A. Moon, Keith Playford, and P. Tucker Withington.
    (OOPSLA 1996)

    Some notes about the rules implied by C3:

    No duplicate bases.
    It isn't legal to repeat a class in a list of base classes.

    The next three properties are the 3 constraints in "C3".

    Local precendece order.
    If A precedes B in C's MRO, then A will precede B in the MRO of all
    subclasses of C.

    Monotonicity.
    The MRO of a class must be an extension without reordering of the
    MRO of each of its superclasses.

    Extended Precedence Graph (EPG).
    Linearization is consistent if there is a path in the EPG from
    each class to all its successors in the linearization.  See
    the paper for definition of EPG.
 */

static int tail_contains(PyObject* list, int whence, PyObject* o) {
    Py_ssize_t j, size;
    size = PyList_GET_SIZE(list);

    for (j = whence + 1; j < size; j++) {
        if (PyList_GET_ITEM(list, j) == o)
            return 1;
    }
    return 0;
}

static PyObject* class_name(PyObject* cls) {
    PyObject* name = PyObject_GetAttrString(cls, "__name__");
    if (name == NULL) {
        PyErr_Clear();
        Py_XDECREF(name);
        name = PyObject_Repr(cls);
    }
    if (name == NULL)
        return NULL;
    if (!PyString_Check(name)) {
        Py_DECREF(name);
        return NULL;
    }
    return name;
}

static int check_duplicates(PyObject* list) {
    Py_ssize_t i, j, n;
    /* Let's use a quadratic time algorithm,
       assuming that the bases lists is short.
    */
    n = PyList_GET_SIZE(list);
    for (i = 0; i < n; i++) {
        PyObject* o = PyList_GET_ITEM(list, i);
        for (j = i + 1; j < n; j++) {
            if (PyList_GET_ITEM(list, j) == o) {
                o = class_name(o);
                PyErr_Format(PyExc_TypeError, "duplicate base class %s", o ? PyString_AS_STRING(o) : "?");
                Py_XDECREF(o);
                return -1;
            }
        }
    }
    return 0;
}

/* Raise a TypeError for an MRO order disagreement.

   It's hard to produce a good error message.  In the absence of better
   insight into error reporting, report the classes that were candidates
   to be put next into the MRO.  There is some conflict between the
   order in which they should be put in the MRO, but it's hard to
   diagnose what constraint can't be satisfied.
*/

static void set_mro_error(PyObject* to_merge, int* remain) noexcept {
    Py_ssize_t i, n, off, to_merge_size;
    char buf[1000];
    PyObject* k, *v;
    PyObject* set = PyDict_New();
    if (!set)
        return;

    to_merge_size = PyList_GET_SIZE(to_merge);
    for (i = 0; i < to_merge_size; i++) {
        PyObject* L = PyList_GET_ITEM(to_merge, i);
        if (remain[i] < PyList_GET_SIZE(L)) {
            PyObject* c = PyList_GET_ITEM(L, remain[i]);
            if (PyDict_SetItem(set, c, Py_None) < 0) {
                Py_DECREF(set);
                return;
            }
        }
    }
    n = PyDict_Size(set);

    off = PyOS_snprintf(buf, sizeof(buf), "Cannot create a \
consistent method resolution\norder (MRO) for bases");
    i = 0;
    while (PyDict_Next(set, &i, &k, &v) && (size_t)off < sizeof(buf)) {
        PyObject* name = class_name(k);
        off += PyOS_snprintf(buf + off, sizeof(buf) - off, " %s", name ? PyString_AS_STRING(name) : "?");
        Py_XDECREF(name);
        if (--n && (size_t)(off + 1) < sizeof(buf)) {
            buf[off++] = ',';
            buf[off] = '\0';
        }
    }
    PyErr_SetString(PyExc_TypeError, buf);
    Py_DECREF(set);
}

static int pmerge(PyObject* acc, PyObject* to_merge) noexcept {
    Py_ssize_t i, j, to_merge_size, empty_cnt;
    int* remain;
    int ok;

    to_merge_size = PyList_GET_SIZE(to_merge);

    /* remain stores an index into each sublist of to_merge.
       remain[i] is the index of the next base in to_merge[i]
       that is not included in acc.
    */
    remain = (int*)PyMem_MALLOC(SIZEOF_INT * to_merge_size);
    if (remain == NULL)
        return -1;
    for (i = 0; i < to_merge_size; i++)
        remain[i] = 0;

again:
    empty_cnt = 0;
    for (i = 0; i < to_merge_size; i++) {
        PyObject* candidate;

        PyObject* cur_list = PyList_GET_ITEM(to_merge, i);

        if (remain[i] >= PyList_GET_SIZE(cur_list)) {
            empty_cnt++;
            continue;
        }

        /* Choose next candidate for MRO.

           The input sequences alone can determine the choice.
           If not, choose the class which appears in the MRO
           of the earliest direct superclass of the new class.
        */

        candidate = PyList_GET_ITEM(cur_list, remain[i]);
        for (j = 0; j < to_merge_size; j++) {
            PyObject* j_lst = PyList_GET_ITEM(to_merge, j);
            if (tail_contains(j_lst, remain[j], candidate)) {
                goto skip; /* continue outer loop */
            }
        }
        ok = PyList_Append(acc, candidate);
        if (ok < 0) {
            PyMem_Free(remain);
            return -1;
        }
        for (j = 0; j < to_merge_size; j++) {
            PyObject* j_lst = PyList_GET_ITEM(to_merge, j);
            if (remain[j] < PyList_GET_SIZE(j_lst) && PyList_GET_ITEM(j_lst, remain[j]) == candidate) {
                remain[j]++;
            }
        }
        goto again;
    skip:
        ;
    }

    if (empty_cnt == to_merge_size) {
        PyMem_FREE(remain);
        return 0;
    }
    set_mro_error(to_merge, remain);
    PyMem_FREE(remain);
    return -1;
}

static PyObject* mro_implementation(PyTypeObject* type) noexcept {
    Py_ssize_t i, n;
    int ok;
    PyObject* bases, *result;
    PyObject* to_merge, *bases_aslist;

    // Pyston change: we require things are already ready
    if (type->tp_dict == NULL) {
        assert(type->is_pyston_class);
#if 0
        if (PyType_Ready(type) < 0)
            return NULL;
#endif
    }

    /* Find a superclass linearization that honors the constraints
       of the explicit lists of bases and the constraints implied by
       each base class.

       to_merge is a list of lists, where each list is a superclass
       linearization implied by a base class.  The last element of
       to_merge is the declared list of bases.
    */

    bases = type->tp_bases;
    assert(type->tp_bases);
    assert(type->tp_bases->cls == tuple_cls);
    n = PyTuple_GET_SIZE(bases);

    to_merge = PyList_New(n + 1);
    if (to_merge == NULL)
        return NULL;

    for (i = 0; i < n; i++) {
        PyObject* base = PyTuple_GET_ITEM(bases, i);
        PyObject* parentMRO;
        if (PyType_Check(base))
            parentMRO = PySequence_List(((PyTypeObject*)base)->tp_mro);
        else
            parentMRO = classic_mro(base);
        if (parentMRO == NULL) {
            Py_DECREF(to_merge);
            return NULL;
        }

        PyList_SET_ITEM(to_merge, i, parentMRO);
    }

    bases_aslist = PySequence_List(bases);
    if (bases_aslist == NULL) {
        Py_DECREF(to_merge);
        return NULL;
    }
    /* This is just a basic sanity check. */
    if (check_duplicates(bases_aslist) < 0) {
        Py_DECREF(to_merge);
        Py_DECREF(bases_aslist);
        return NULL;
    }
    PyList_SET_ITEM(to_merge, n, bases_aslist);

    result = Py_BuildValue("[O]", (PyObject*)type);
    if (result == NULL) {
        Py_DECREF(to_merge);
        return NULL;
    }

    ok = pmerge(result, to_merge);
    Py_DECREF(to_merge);
    if (ok < 0) {
        Py_DECREF(result);
        return NULL;
    }

    return result;
}

// Pyston change: made this non-static
PyObject* mro_external(PyObject* self) noexcept {
    PyTypeObject* type = (PyTypeObject*)self;

    return mro_implementation(type);
}

static int mro_internal(PyTypeObject* type) noexcept {
    PyObject* mro, *result, *tuple;
    int checkit = 0;

    if (Py_TYPE(type) == &PyType_Type) {
        result = mro_implementation(type);
    } else {
        static PyObject* mro_str;
        checkit = 1;
        mro = lookup_method((PyObject*)type, "mro", &mro_str);
        if (mro == NULL)
            return -1;
        result = PyObject_CallObject(mro, NULL);
        Py_DECREF(mro);
    }
    if (result == NULL)
        return -1;
    tuple = PySequence_Tuple(result);
    Py_DECREF(result);
    if (tuple == NULL)
        return -1;
    if (checkit) {
        Py_ssize_t i, len;
        PyObject* cls;
        PyTypeObject* solid;

        solid = solid_base(type);

        len = PyTuple_GET_SIZE(tuple);

        for (i = 0; i < len; i++) {
            PyTypeObject* t;
            cls = PyTuple_GET_ITEM(tuple, i);
            if (PyClass_Check(cls))
                continue;
            else if (!PyType_Check(cls)) {
                PyErr_Format(PyExc_TypeError, "mro() returned a non-class ('%.500s')", Py_TYPE(cls)->tp_name);
                Py_DECREF(tuple);
                return -1;
            }
            t = (PyTypeObject*)cls;
            if (!PyType_IsSubtype(solid, solid_base(t))) {
                PyErr_Format(PyExc_TypeError, "mro() returned base with unsuitable layout ('%.500s')", t->tp_name);
                Py_DECREF(tuple);
                return -1;
            }
        }
    }
    type->tp_mro = tuple;

    type_mro_modified(type, type->tp_mro);
    /* corner case: the old-style super class might have been hidden
       from the custom MRO */
    type_mro_modified(type, type->tp_bases);

    PyType_Modified(type);

    return 0;
}
extern "C" int PyType_IsSubtype(PyTypeObject* a, PyTypeObject* b) noexcept {
    PyObject* mro;

    if (!(a->tp_flags & Py_TPFLAGS_HAVE_CLASS))
        return b == a || b == &PyBaseObject_Type;

    mro = a->tp_mro;
    if (mro != NULL) {
        /* Deal with multiple inheritance without recursion
           by walking the MRO tuple */
        Py_ssize_t i, n;
        assert(PyTuple_Check(mro));
        n = PyTuple_GET_SIZE(mro);
        for (i = 0; i < n; i++) {
            if (PyTuple_GET_ITEM(mro, i) == (PyObject*)b)
                return 1;
        }
        return 0;
    } else {
        /* a is not completely initilized yet; follow tp_base */
        do {
            if (a == b)
                return 1;
            a = a->tp_base;
        } while (a != NULL);
        return b == &PyBaseObject_Type;
    }
}

#define BUFFER_FLAGS (Py_TPFLAGS_HAVE_GETCHARBUFFER | Py_TPFLAGS_HAVE_NEWBUFFER)

// This is copied from CPython with some modifications:
static void inherit_special(PyTypeObject* type, PyTypeObject* base) noexcept {
    Py_ssize_t oldsize, newsize;

    /* Special flag magic */
    if (!type->tp_as_buffer && base->tp_as_buffer) {
        type->tp_flags &= ~BUFFER_FLAGS;
        type->tp_flags |= base->tp_flags & BUFFER_FLAGS;
    }
    if (!type->tp_as_sequence && base->tp_as_sequence) {
        type->tp_flags &= ~Py_TPFLAGS_HAVE_SEQUENCE_IN;
        type->tp_flags |= base->tp_flags & Py_TPFLAGS_HAVE_SEQUENCE_IN;
    }
    if ((type->tp_flags & Py_TPFLAGS_HAVE_INPLACEOPS) != (base->tp_flags & Py_TPFLAGS_HAVE_INPLACEOPS)) {
        if ((!type->tp_as_number && base->tp_as_number) || (!type->tp_as_sequence && base->tp_as_sequence)) {
            type->tp_flags &= ~Py_TPFLAGS_HAVE_INPLACEOPS;
            if (!type->tp_as_number && !type->tp_as_sequence) {
                type->tp_flags |= base->tp_flags & Py_TPFLAGS_HAVE_INPLACEOPS;
            }
        }
        /* Wow */
    }
    if (!type->tp_as_number && base->tp_as_number) {
        type->tp_flags &= ~Py_TPFLAGS_CHECKTYPES;
        type->tp_flags |= base->tp_flags & Py_TPFLAGS_CHECKTYPES;
    }

    /* Copying basicsize is connected to the GC flags */
    oldsize = base->tp_basicsize;
    newsize = type->tp_basicsize ? type->tp_basicsize : oldsize;
    if (!(type->tp_flags & Py_TPFLAGS_HAVE_GC) && (base->tp_flags & Py_TPFLAGS_HAVE_GC)
        && (type->tp_flags & Py_TPFLAGS_HAVE_RICHCOMPARE /*GC slots exist*/)
        && (!type->tp_traverse && !type->tp_clear)) {
        type->tp_flags |= Py_TPFLAGS_HAVE_GC;
        if (type->tp_traverse == NULL)
            type->tp_traverse = base->tp_traverse;
        if (type->tp_clear == NULL)
            type->tp_clear = base->tp_clear;
    }
    if (type->tp_flags & base->tp_flags & Py_TPFLAGS_HAVE_CLASS) {
        /* The condition below could use some explanation.
           It appears that tp_new is not inherited for static types
           whose base class is 'object'; this seems to be a precaution
           so that old extension types don't suddenly become
           callable (object.__new__ wouldn't insure the invariants
           that the extension type's own factory function ensures).
           Heap types, of course, are under our control, so they do
           inherit tp_new; static extension types that specify some
           other built-in type as the default are considered
           new-style-aware so they also inherit object.__new__. */
        if (base != object_cls || (type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
            if (type->tp_new == NULL)
                type->tp_new = base->tp_new;
        }
    }
    type->tp_basicsize = newsize;

/* Copy other non-function slots */

#undef COPYVAL
#define COPYVAL(SLOT)                                                                                                  \
    if (type->SLOT == 0)                                                                                               \
    type->SLOT = base->SLOT

    COPYVAL(tp_itemsize);
    if (type->tp_flags & base->tp_flags & Py_TPFLAGS_HAVE_WEAKREFS) {
        COPYVAL(tp_weaklistoffset);
    }
    if (type->tp_flags & base->tp_flags & Py_TPFLAGS_HAVE_CLASS) {
        COPYVAL(tp_dictoffset);
    }

    /* Setup fast subclass flags */
    if (PyType_IsSubtype(base, (PyTypeObject*)PyExc_BaseException))
        type->tp_flags |= Py_TPFLAGS_BASE_EXC_SUBCLASS;
    else if (PyType_IsSubtype(base, &PyType_Type))
        type->tp_flags |= Py_TPFLAGS_TYPE_SUBCLASS;
    else if (PyType_IsSubtype(base, &PyInt_Type))
        type->tp_flags |= Py_TPFLAGS_INT_SUBCLASS;
    else if (PyType_IsSubtype(base, &PyLong_Type))
        type->tp_flags |= Py_TPFLAGS_LONG_SUBCLASS;
    else if (PyType_IsSubtype(base, &PyString_Type))
        type->tp_flags |= Py_TPFLAGS_STRING_SUBCLASS;
#ifdef Py_USING_UNICODE
    else if (PyType_IsSubtype(base, &PyUnicode_Type))
        type->tp_flags |= Py_TPFLAGS_UNICODE_SUBCLASS;
#endif
    else if (PyType_IsSubtype(base, &PyTuple_Type))
        type->tp_flags |= Py_TPFLAGS_TUPLE_SUBCLASS;
    else if (PyType_IsSubtype(base, &PyList_Type))
        type->tp_flags |= Py_TPFLAGS_LIST_SUBCLASS;
    else if (PyType_IsSubtype(base, &PyDict_Type))
        type->tp_flags |= Py_TPFLAGS_DICT_SUBCLASS;
}

static int overrides_name(PyTypeObject* type, const char* name) noexcept {
    PyObject* dict = type->tp_dict;

    assert(dict != NULL);
    if (PyDict_GetItemString(dict, name) != NULL) {
        return 1;
    }
    return 0;
}

#define OVERRIDES_HASH(x) overrides_name(x, "__hash__")
#define OVERRIDES_EQ(x) overrides_name(x, "__eq__")

static void inherit_slots(PyTypeObject* type, PyTypeObject* base) noexcept {
    // Pyston addition:
    if (base->tp_base == NULL)
        assert(base == object_cls);

    PyTypeObject* basebase;

#undef SLOTDEFINED
#undef COPYSLOT
#undef COPYNUM
#undef COPYSEQ
#undef COPYMAP
#undef COPYBUF

#define SLOTDEFINED(SLOT) (base->SLOT != 0 && (basebase == NULL || base->SLOT != basebase->SLOT))

#define COPYSLOT(SLOT)                                                                                                 \
    if (!type->SLOT && SLOTDEFINED(SLOT))                                                                              \
    type->SLOT = base->SLOT

#define COPYNUM(SLOT) COPYSLOT(tp_as_number->SLOT)
#define COPYSEQ(SLOT) COPYSLOT(tp_as_sequence->SLOT)
#define COPYMAP(SLOT) COPYSLOT(tp_as_mapping->SLOT)
#define COPYBUF(SLOT) COPYSLOT(tp_as_buffer->SLOT)

    /* This won't inherit indirect slots (from tp_as_number etc.)
       if type doesn't provide the space. */

    if (type->tp_as_number != NULL && base->tp_as_number != NULL) {
        basebase = base->tp_base;
        if (basebase->tp_as_number == NULL)
            basebase = NULL;
        COPYNUM(nb_add);
        COPYNUM(nb_subtract);
        COPYNUM(nb_multiply);
        COPYNUM(nb_divide);
        COPYNUM(nb_remainder);
        COPYNUM(nb_divmod);
        COPYNUM(nb_power);
        COPYNUM(nb_negative);
        COPYNUM(nb_positive);
        COPYNUM(nb_absolute);
        COPYNUM(nb_nonzero);
        COPYNUM(nb_invert);
        COPYNUM(nb_lshift);
        COPYNUM(nb_rshift);
        COPYNUM(nb_and);
        COPYNUM(nb_xor);
        COPYNUM(nb_or);
        COPYNUM(nb_coerce);
        COPYNUM(nb_int);
        COPYNUM(nb_long);
        COPYNUM(nb_float);
        COPYNUM(nb_oct);
        COPYNUM(nb_hex);
        COPYNUM(nb_inplace_add);
        COPYNUM(nb_inplace_subtract);
        COPYNUM(nb_inplace_multiply);
        COPYNUM(nb_inplace_divide);
        COPYNUM(nb_inplace_remainder);
        COPYNUM(nb_inplace_power);
        COPYNUM(nb_inplace_lshift);
        COPYNUM(nb_inplace_rshift);
        COPYNUM(nb_inplace_and);
        COPYNUM(nb_inplace_xor);
        COPYNUM(nb_inplace_or);
        if (base->tp_flags & Py_TPFLAGS_CHECKTYPES) {
            COPYNUM(nb_true_divide);
            COPYNUM(nb_floor_divide);
            COPYNUM(nb_inplace_true_divide);
            COPYNUM(nb_inplace_floor_divide);
        }
        if (base->tp_flags & Py_TPFLAGS_HAVE_INDEX) {
            COPYNUM(nb_index);
        }
    }

    if (type->tp_as_sequence != NULL && base->tp_as_sequence != NULL) {
        basebase = base->tp_base;
        if (basebase->tp_as_sequence == NULL)
            basebase = NULL;
        COPYSEQ(sq_length);
        COPYSEQ(sq_concat);
        COPYSEQ(sq_repeat);
        COPYSEQ(sq_item);
        COPYSEQ(sq_slice);
        COPYSEQ(sq_ass_item);
        COPYSEQ(sq_ass_slice);
        COPYSEQ(sq_contains);
        COPYSEQ(sq_inplace_concat);
        COPYSEQ(sq_inplace_repeat);
    }

    if (type->tp_as_mapping != NULL && base->tp_as_mapping != NULL) {
        basebase = base->tp_base;
        if (basebase->tp_as_mapping == NULL)
            basebase = NULL;
        COPYMAP(mp_length);
        COPYMAP(mp_subscript);
        COPYMAP(mp_ass_subscript);
    }

    if (type->tp_as_buffer != NULL && base->tp_as_buffer != NULL) {
        basebase = base->tp_base;
        if (basebase->tp_as_buffer == NULL)
            basebase = NULL;
        COPYBUF(bf_getreadbuffer);
        COPYBUF(bf_getwritebuffer);
        COPYBUF(bf_getsegcount);
        COPYBUF(bf_getcharbuffer);
        COPYBUF(bf_getbuffer);
        COPYBUF(bf_releasebuffer);
    }

    basebase = base->tp_base;

    COPYSLOT(tp_dealloc);
    COPYSLOT(tp_print);
    if (type->tp_getattr == NULL && type->tp_getattro == NULL) {
        type->tp_getattr = base->tp_getattr;
        type->tp_getattro = base->tp_getattro;
    }
    if (type->tp_setattr == NULL && type->tp_setattro == NULL) {
        type->tp_setattr = base->tp_setattr;
        type->tp_setattro = base->tp_setattro;
    }
    /* tp_compare see tp_richcompare */
    COPYSLOT(tp_repr);
    /* tp_hash see tp_richcompare */
    COPYSLOT(tp_call);
    COPYSLOT(tp_str);
    if (type->tp_flags & base->tp_flags & Py_TPFLAGS_HAVE_RICHCOMPARE) {
        if (type->tp_compare == NULL && type->tp_richcompare == NULL && type->tp_hash == NULL) {
            type->tp_compare = base->tp_compare;
            type->tp_richcompare = base->tp_richcompare;
            type->tp_hash = base->tp_hash;
            /* Check for changes to inherited methods in Py3k*/
            if (Py_Py3kWarningFlag) {
                if (base->tp_hash && (base->tp_hash != PyObject_HashNotImplemented) && !OVERRIDES_HASH(type)) {
                    if (OVERRIDES_EQ(type)) {
                        if (PyErr_WarnPy3k("Overriding "
                                           "__eq__ blocks inheritance "
                                           "of __hash__ in 3.x",
                                           1) < 0)
                            /* XXX This isn't right.  If the warning is turned
                               into an exception, we should be communicating
                               the error back to the caller, but figuring out
                               how to clean up in that case is tricky.  See
                               issue 8627 for more. */
                            PyErr_Clear();
                    }
                }
            }
        }
    } else {
        COPYSLOT(tp_compare);
    }
    if (type->tp_flags & base->tp_flags & Py_TPFLAGS_HAVE_ITER) {
        COPYSLOT(tp_iter);
        COPYSLOT(tp_iternext);
    }
    if (type->tp_flags & base->tp_flags & Py_TPFLAGS_HAVE_CLASS) {
        COPYSLOT(tp_descr_get);
        COPYSLOT(tp_descr_set);
        COPYSLOT(tp_dictoffset);
        COPYSLOT(tp_init);
        COPYSLOT(tp_alloc);
        COPYSLOT(tp_is_gc);
        if ((type->tp_flags & Py_TPFLAGS_HAVE_GC) == (base->tp_flags & Py_TPFLAGS_HAVE_GC)) {
            /* They agree about gc. */
            COPYSLOT(tp_free);
        } else if ((type->tp_flags & Py_TPFLAGS_HAVE_GC) && type->tp_free == NULL && base->tp_free == _PyObject_Del) {
            /* A bit of magic to plug in the correct default
             * tp_free function when a derived class adds gc,
             * didn't define tp_free, and the base uses the
             * default non-gc tp_free.
             */
            // Pyston change: don't do this:
            // type->tp_free = PyObject_GC_Del;
        }
        /* else they didn't agree about gc, and there isn't something
         * obvious to be done -- the type is on its own.
         */
    }
}

static int add_subclass(PyTypeObject* base, PyTypeObject* type) noexcept {
    Py_ssize_t i;
    int result;
    PyObject* list, *ref, *newobj;

    list = base->tp_subclasses;
    if (list == NULL) {
        base->tp_subclasses = list = PyList_New(0);
        if (list == NULL)
            return -1;
    }
    assert(PyList_Check(list));
    newobj = PyWeakref_NewRef((PyObject*)type, NULL);
    i = PyList_GET_SIZE(list);
    while (--i >= 0) {
        ref = PyList_GET_ITEM(list, i);
        assert(PyWeakref_CheckRef(ref));
        if (PyWeakref_GET_OBJECT(ref) == Py_None)
            return PyList_SetItem(list, i, newobj);
    }
    result = PyList_Append(list, newobj);
    Py_DECREF(newobj);
    return result;
}

static void remove_subclass(PyTypeObject* base, PyTypeObject* type) noexcept {
    Py_ssize_t i;
    PyObject* list, *ref;

    list = base->tp_subclasses;
    if (list == NULL) {
        return;
    }
    assert(PyList_Check(list));
    i = PyList_GET_SIZE(list);
    while (--i >= 0) {
        ref = PyList_GET_ITEM(list, i);
        assert(PyWeakref_CheckRef(ref));
        if (PyWeakref_GET_OBJECT(ref) == (PyObject*)type) {
            /* this can't fail, right? */
            PySequence_DelItem(list, i);
            return;
        }
    }
}

static int equiv_structs(PyTypeObject* a, PyTypeObject* b) noexcept {
    // Pyston change: added attrs_offset equality check
    // return a == b || (a != NULL && b != NULL && a->tp_basicsize == b->tp_basicsize
    //                   && a->tp_itemsize == b->tp_itemsize
    //                   && a->tp_dictoffset == b->tp_dictoffset && a->tp_weaklistoffset == b->tp_weaklistoffset
    //                   && ((a->tp_flags & Py_TPFLAGS_HAVE_GC) == (b->tp_flags & Py_TPFLAGS_HAVE_GC)));
    return a == b || (a != NULL && b != NULL && a->tp_basicsize == b->tp_basicsize && a->tp_itemsize == b->tp_itemsize
                      && a->tp_dictoffset == b->tp_dictoffset && a->tp_weaklistoffset == b->tp_weaklistoffset
                      && a->attrs_offset == b->attrs_offset
                      && ((a->tp_flags & Py_TPFLAGS_HAVE_GC) == (b->tp_flags & Py_TPFLAGS_HAVE_GC)));
}

static void update_all_slots(PyTypeObject* type) noexcept {
    slotdef* p;

    init_slotdefs();
    for (p = slotdefs; p->name.size() > 0; p++) {
        /* update_slot returns int but can't actually fail */
        update_slot(type, p->name);
    }
}

static int same_slots_added(PyTypeObject* a, PyTypeObject* b) noexcept {
    PyTypeObject* base = a->tp_base;
    Py_ssize_t size;
    PyObject* slots_a, *slots_b;

    assert(base == b->tp_base);
    size = base->tp_basicsize;
    if (a->tp_dictoffset == size && b->tp_dictoffset == size)
        size += sizeof(PyObject*);
    // Pyston change: have to check attrs_offset
    if (a->attrs_offset == size && b->attrs_offset == size)
        size += sizeof(HCAttrs);
    if (a->tp_weaklistoffset == size && b->tp_weaklistoffset == size)
        size += sizeof(PyObject*);

    /* Check slots compliance */
    slots_a = ((PyHeapTypeObject*)a)->ht_slots;
    slots_b = ((PyHeapTypeObject*)b)->ht_slots;
    if (slots_a && slots_b) {
        if (PyObject_Compare(slots_a, slots_b) != 0)
            return 0;
        size += sizeof(PyObject*) * PyTuple_GET_SIZE(slots_a);
    }
    return size == a->tp_basicsize && size == b->tp_basicsize;
}

static int compatible_for_assignment(PyTypeObject* oldto, PyTypeObject* newto, const char* attr) noexcept {
    PyTypeObject* newbase, *oldbase;

    if (newto->tp_dealloc != oldto->tp_dealloc || newto->tp_free != oldto->tp_free) {
        PyErr_Format(PyExc_TypeError, "%s assignment: "
                                      "'%s' deallocator differs from '%s'",
                     attr, newto->tp_name, oldto->tp_name);
        return 0;
    }
    newbase = newto;
    oldbase = oldto;
    while (equiv_structs(newbase, newbase->tp_base))
        newbase = newbase->tp_base;
    while (equiv_structs(oldbase, oldbase->tp_base))
        oldbase = oldbase->tp_base;
    if (newbase != oldbase && (newbase->tp_base != oldbase->tp_base || !same_slots_added(newbase, oldbase))) {
        PyErr_Format(PyExc_TypeError, "%s assignment: "
                                      "'%s' object layout differs from '%s'",
                     attr, newto->tp_name, oldto->tp_name);
        return 0;
    }

    return 1;
}

static int mro_subclasses(PyTypeObject* type, PyObject* temp) noexcept {
    PyTypeObject* subclass;
    PyObject* ref, *subclasses, *old_mro;
    Py_ssize_t i, n;

    subclasses = type->tp_subclasses;
    if (subclasses == NULL)
        return 0;
    assert(PyList_Check(subclasses));
    n = PyList_GET_SIZE(subclasses);
    for (i = 0; i < n; i++) {
        ref = PyList_GET_ITEM(subclasses, i);
        assert(PyWeakref_CheckRef(ref));
        subclass = (PyTypeObject*)PyWeakref_GET_OBJECT(ref);
        assert(subclass != NULL);
        if ((PyObject*)subclass == Py_None)
            continue;
        assert(PyType_Check(subclass));
        old_mro = subclass->tp_mro;
        if (mro_internal(subclass) < 0) {
            subclass->tp_mro = old_mro;
            return -1;
        } else {
            PyObject* tuple;
            tuple = PyTuple_Pack(2, subclass, old_mro);
            Py_DECREF(old_mro);
            if (!tuple)
                return -1;
            if (PyList_Append(temp, tuple) < 0)
                return -1;
            Py_DECREF(tuple);
        }
        if (mro_subclasses(subclass, temp) < 0)
            return -1;
    }
    return 0;
}

int type_set_bases(PyTypeObject* type, PyObject* value, void* context) noexcept {
    Py_ssize_t i;
    int r = 0;
    PyObject* ob, *temp;
    PyTypeObject* new_base, *old_base;
    PyObject* old_bases, *old_mro;

    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_Format(PyExc_TypeError, "can't set %s.__bases__", type->tp_name);
        return -1;
    }
    if (!value) {
        PyErr_Format(PyExc_TypeError, "can't delete %s.__bases__", type->tp_name);
        return -1;
    }
    if (!PyTuple_Check(value)) {
        PyErr_Format(PyExc_TypeError, "can only assign tuple to %s.__bases__, not %s", type->tp_name,
                     Py_TYPE(value)->tp_name);
        return -1;
    }
    if (PyTuple_GET_SIZE(value) == 0) {
        PyErr_Format(PyExc_TypeError, "can only assign non-empty tuple to %s.__bases__, not ()", type->tp_name);
        return -1;
    }
    for (i = 0; i < PyTuple_GET_SIZE(value); i++) {
        ob = PyTuple_GET_ITEM(value, i);
        if (!PyClass_Check(ob) && !PyType_Check(ob)) {
            PyErr_Format(PyExc_TypeError, "%s.__bases__ must be tuple of old- or new-style classes, not '%s'",
                         type->tp_name, Py_TYPE(ob)->tp_name);
            return -1;
        }
        if (PyType_Check(ob)) {
            if (PyType_IsSubtype((PyTypeObject*)ob, type)) {
                PyErr_SetString(PyExc_TypeError, "a __bases__ item causes an inheritance cycle");
                return -1;
            }
        }
    }

    new_base = best_base(value);

    if (!new_base) {
        return -1;
    }

    if (!compatible_for_assignment(type->tp_base, new_base, "__bases__"))
        return -1;

    Py_INCREF(new_base);
    Py_INCREF(value);

    old_bases = type->tp_bases;
    old_base = type->tp_base;
    old_mro = type->tp_mro;

    type->tp_bases = value;
    type->tp_base = new_base;

    if (mro_internal(type) < 0) {
        goto bail;
    }

    temp = PyList_New(0);
    if (!temp)
        goto bail;

    r = mro_subclasses(type, temp);

    if (r < 0) {
        for (i = 0; i < PyList_Size(temp); i++) {
            PyTypeObject* cls;
            PyObject* mro;
            PyArg_UnpackTuple(PyList_GET_ITEM(temp, i), "", 2, 2, &cls, &mro);
            Py_INCREF(mro);
            ob = cls->tp_mro;
            cls->tp_mro = mro;
            Py_DECREF(ob);
        }
        Py_DECREF(temp);
        goto bail;
    }

    Py_DECREF(temp);

    /* any base that was in __bases__ but now isn't, we
       need to remove |type| from its tp_subclasses.
       conversely, any class now in __bases__ that wasn't
       needs to have |type| added to its subclasses. */

    /* for now, sod that: just remove from all old_bases,
       add to all new_bases */

    for (i = PyTuple_GET_SIZE(old_bases) - 1; i >= 0; i--) {
        ob = PyTuple_GET_ITEM(old_bases, i);
        if (PyType_Check(ob)) {
            remove_subclass((PyTypeObject*)ob, type);
        }
    }

    for (i = PyTuple_GET_SIZE(value) - 1; i >= 0; i--) {
        ob = PyTuple_GET_ITEM(value, i);
        if (PyType_Check(ob)) {
            if (add_subclass((PyTypeObject*)ob, type) < 0)
                r = -1;
        }
    }

    update_all_slots(type);

    Py_DECREF(old_bases);
    Py_DECREF(old_base);
    Py_DECREF(old_mro);

    return r;

bail:
    Py_DECREF(type->tp_bases);
    Py_DECREF(type->tp_base);
    if (type->tp_mro != old_mro) {
        Py_DECREF(type->tp_mro);
    }

    type->tp_bases = old_bases;
    type->tp_base = old_base;
    type->tp_mro = old_mro;

    return -1;
}

// commonClassSetup is for the common code between PyType_Ready (which is just for extension classes)
// and our internal type-creation endpoints (BoxedClass::BoxedClass()).
// TODO: Move more of the duplicated logic into here.
void commonClassSetup(BoxedClass* cls) {
    if (cls->tp_bases == NULL) {
        if (cls->tp_base)
            cls->tp_bases = BoxedTuple::create({ cls->tp_base });
        else
            cls->tp_bases = BoxedTuple::create({});
    }

    /* Link into each base class's list of subclasses */
    for (PyObject* b : *static_cast<BoxedTuple*>(cls->tp_bases)) {
        if (PyType_Check(b) && add_subclass((PyTypeObject*)b, cls) < 0)
            throwCAPIException();
    }

    /* Calculate method resolution order */
    if (mro_internal(cls) < 0)
        throwCAPIException();

    if (cls->tp_base)
        inherit_special(cls, cls->tp_base);

    assert(cls->tp_mro);
    assert(cls->tp_mro->cls == tuple_cls);

    for (auto b : *static_cast<BoxedTuple*>(cls->tp_mro)) {
        if (b == cls)
            continue;
        if (PyType_Check(b))
            inherit_slots(cls, static_cast<BoxedClass*>(b));
    }

    assert(cls->tp_dict && cls->tp_dict->cls == attrwrapper_cls);
}

extern "C" void PyType_Modified(PyTypeObject* type) noexcept {
    // We don't cache anything yet that would need to be invalidated:
}

static Box* tppProxyToTpCall(Box* self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                             Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    ParamReceiveSpec paramspec(0, 0, true, true);
    if (!argspec.has_kwargs && argspec.num_keywords == 0) {
        paramspec.takes_kwargs = false;
    }

    bool rewrite_success = false;
    Box* oarg1, * oarg2 = NULL, *oarg3, ** oargs = NULL;
    rearrangeArguments(paramspec, NULL, "", NULL, rewrite_args, rewrite_success, argspec, arg1, arg2, arg3, args,
                       keyword_names, oarg1, oarg2, oarg3, oargs);

    if (!rewrite_success)
        rewrite_args = NULL;

    if (rewrite_args) {
        if (!paramspec.takes_kwargs)
            rewrite_args->arg2 = rewrite_args->rewriter->loadConst(0, Location::forArg(2));

        // Currently, guard that the value of tp_call didn't change, and then
        // emit a call to the current function address.
        // It might be better to just load the current value of tp_call and call it
        // (after guarding it's not null), or maybe not.  But the rewriter doesn't currently
        // support calling a RewriterVar (can only call fixed function addresses).
        RewriterVar* r_cls = rewrite_args->obj->getAttr(offsetof(Box, cls));
        r_cls->addAttrGuard(offsetof(BoxedClass, tp_call), (intptr_t)self->cls->tp_call);

        rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)self->cls->tp_call, rewrite_args->obj,
                                                             rewrite_args->arg1, rewrite_args->arg2);
        rewrite_args->rewriter->call(true, (void*)checkAndThrowCAPIException);
        rewrite_args->out_success = true;
    }

    Box* r = self->cls->tp_call(self, oarg1, oarg2);
    if (!r)
        throwCAPIException();
    return r;
}

extern "C" int PyType_Ready(PyTypeObject* cls) noexcept {
    ASSERT(!cls->is_pyston_class, "should not call this on Pyston classes");

    gc::registerNonheapRootObject(cls, sizeof(PyTypeObject));

    // unhandled fields:
    int ALLOWABLE_FLAGS = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_CHECKTYPES
                          | Py_TPFLAGS_HAVE_NEWBUFFER;
    ALLOWABLE_FLAGS |= Py_TPFLAGS_INT_SUBCLASS | Py_TPFLAGS_LONG_SUBCLASS | Py_TPFLAGS_LIST_SUBCLASS
                       | Py_TPFLAGS_TUPLE_SUBCLASS | Py_TPFLAGS_STRING_SUBCLASS | Py_TPFLAGS_UNICODE_SUBCLASS
                       | Py_TPFLAGS_DICT_SUBCLASS | Py_TPFLAGS_BASE_EXC_SUBCLASS | Py_TPFLAGS_TYPE_SUBCLASS;
    RELEASE_ASSERT((cls->tp_flags & ~ALLOWABLE_FLAGS) == 0, "");

    RELEASE_ASSERT(cls->tp_free == NULL || cls->tp_free == PyObject_Del || cls->tp_free == PyObject_GC_Del, "");
    RELEASE_ASSERT(cls->tp_is_gc == NULL, "");
    RELEASE_ASSERT(cls->tp_mro == NULL, "");
    RELEASE_ASSERT(cls->tp_cache == NULL, "");
    RELEASE_ASSERT(cls->tp_subclasses == NULL, "");
    RELEASE_ASSERT(cls->tp_weaklist == NULL, "");
    RELEASE_ASSERT(cls->tp_del == NULL, "");
    RELEASE_ASSERT(cls->tp_version_tag == 0, "");

    // I think it is safe to ignore these for for now:
    // RELEASE_ASSERT(cls->tp_weaklistoffset == 0, "");
    // RELEASE_ASSERT(cls->tp_traverse == NULL, "");
    // RELEASE_ASSERT(cls->tp_clear == NULL, "");

    assert(cls->attrs.hcls == NULL);
    new (&cls->attrs) HCAttrs(HiddenClass::makeSingleton());
#define INITIALIZE(a) new (&(a)) decltype(a)
#undef INITIALIZE

    BoxedClass* base = cls->tp_base;
    if (base == NULL)
        base = cls->tp_base = object_cls;
    if (!cls->cls)
        cls->cls = cls->tp_base->cls;
    cls->giveAttr("__base__", base);

    assert(cls->tp_dict == NULL);
    cls->tp_dict = cls->getAttrWrapper();

    assert(cls->tp_name);

    if (cls->tp_call)
        cls->tpp_call = tppProxyToTpCall;

    try {
        add_operators(cls);
    } catch (ExcInfo e) {
        abort();
    }

    for (PyMethodDef* method = cls->tp_methods; method && method->ml_name; ++method) {
        cls->setattr(internStringMortal(method->ml_name), new BoxedMethodDescriptor(method, cls), NULL);
    }

    for (PyMemberDef* member = cls->tp_members; member && member->name; ++member) {
        cls->giveAttr(internStringMortal(member->name), new BoxedMemberDescriptor(member));
    }

    for (PyGetSetDef* getset = cls->tp_getset; getset && getset->name; ++getset) {
        // TODO do something with __doc__
        cls->giveAttr(internStringMortal(getset->name),
                      new (capi_getset_cls) BoxedGetsetDescriptor(getset->get, (void (*)(Box*, Box*, void*))getset->set,
                                                                  getset->closure));
    }

    try {
        commonClassSetup(cls);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }

    static BoxedString* doc_str = internStringImmortal("__doc__");
    if (!cls->hasattr(doc_str)) {
        if (cls->tp_doc) {
            cls->giveAttr(doc_str, boxString(cls->tp_doc));
        } else {
            cls->giveAttr(doc_str, None);
        }
    }

    if (cls->tp_alloc == &PystonType_GenericAlloc)
        cls->tp_alloc = &PyType_GenericAlloc;

    cls->gc_visit = &conservativeGCHandler;
    cls->is_user_defined = true;


    if (!cls->instancesHaveHCAttrs() && cls->tp_base) {
        // These doesn't get copied in inherit_slots like other slots do.
        if (cls->tp_base->instancesHaveHCAttrs()) {
            cls->attrs_offset = cls->tp_base->attrs_offset;
        }

        // Example of when this code path could be reached and needs to be:
        // If this class is a metaclass defined in a C extension, chances are that some of its
        // instances may be hardcoded in the C extension as well. Those instances will call
        // PyType_Ready and expect their class (this metaclass) to have a place to put attributes.
        // e.g. CTypes does this.
        bool is_metaclass = PyType_IsSubtype(cls, type_cls);
        assert(!is_metaclass || cls->instancesHaveHCAttrs() || cls->instancesHaveDictAttrs());
    } else {
        // this should get automatically initialized to 0 on this path:
        assert(cls->attrs_offset == 0);
    }

    if (Py_TPFLAGS_BASE_EXC_SUBCLASS & cls->tp_flags) {
        exception_types.push_back(cls);
    }

    return 0;
}

extern "C" PyObject* PyType_GenericNew(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept {
    return type->tp_alloc(type, 0);
}

} // namespace pyston
