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
#include "runtime/objmodel.h"

namespace pyston {

// FIXME duplicated with objmodel.cpp
static const std::string _new_str("__new__");

extern "C" void conservativeGCHandler(GCVisitor* v, Box* b) {
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

static PyObject* wrap_hashfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
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
    ternaryfunc func = (ternaryfunc)wrapped;

    return (*func)(self, args, kwds);
}

static PyObject* wrap_richcmpfunc(PyObject* self, PyObject* args, void* wrapped, int op) noexcept {
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

static PyObject* wrap_ternaryfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    ternaryfunc func = (ternaryfunc)wrapped;
    PyObject* other;
    PyObject* third = Py_None;

    /* Note: This wrapper only works for __pow__() */

    if (!PyArg_UnpackTuple(args, "", 1, 2, &other, &third))
        return NULL;
    return (*func)(self, other, third);
}

static PyObject* wrap_ternaryfunc_r(PyObject* self, PyObject* args, void* wrapped) noexcept {
    ternaryfunc func = (ternaryfunc)wrapped;
    PyObject* other;
    PyObject* third = Py_None;

    /* Note: This wrapper only works for __pow__() */

    if (!PyArg_UnpackTuple(args, "", 1, 2, &other, &third))
        return NULL;
    return (*func)(other, self, third);
}

static PyObject* wrap_unaryfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    unaryfunc func = (unaryfunc)wrapped;

    if (!check_num_args(args, 0))
        return NULL;
    return (*func)(self);
}

static PyObject* wrap_inquirypred(PyObject* self, PyObject* args, void* wrapped) noexcept {
    inquiry func = (inquiry)wrapped;
    int res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    return PyBool_FromLong((long)res);
}

static PyObject* wrap_binaryfunc(PyObject* self, PyObject* args, void* wrapped) noexcept {
    binaryfunc func = (binaryfunc)wrapped;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    return (*func)(self, other);
}

static PyObject* wrap_binaryfunc_l(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_binaryfunc_r(PyObject* self, PyObject* args, void* wrapped) {
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
    ssizessizeargfunc func = (ssizessizeargfunc)wrapped;
    Py_ssize_t i, j;

    if (!PyArg_ParseTuple(args, "nn", &i, &j))
        return NULL;
    return (*func)(self, i, j);
}

static PyObject* wrap_sq_setitem(PyObject* self, PyObject* args, void* wrapped) noexcept {
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

static PyObject* wrap_init(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds) noexcept {
    initproc func = (initproc)wrapped;

    if (func(self, args, kwds) < 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}



static PyObject* lookup_maybe(PyObject* self, const char* attrstr, PyObject** attrobj) noexcept {
    PyObject* res;

    // TODO: CPython uses the attrobj as a cache
    Box* obj = typeLookup(self->cls, attrstr, NULL);
    if (obj)
        return processDescriptor(obj, self, self->cls);
    return obj;
}

extern "C" PyObject* _PyObject_LookupSpecial(PyObject* self, const char* attrstr, PyObject** attrobj) {
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

PyObject* slot_tp_repr(PyObject* self) noexcept {
    try {
        return repr(self);
    } catch (Box* e) {
        PyErr_SetObject(e->cls, e);
        return NULL;
    }
}

PyObject* slot_tp_str(PyObject* self) noexcept {
    try {
        return str(self);
    } catch (Box* e) {
        PyErr_SetObject(e->cls, e);
        return NULL;
    }
}

static long slot_tp_hash(PyObject* self) noexcept {
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
    try {
        Py_FatalError("this function is untested");

        // TODO: runtime ICs?
        return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwds, NULL, NULL, NULL);
    } catch (Box* e) {
        PyErr_SetObject(e->cls, e);
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

static PyObject* slot_tp_richcompare(PyObject* self, PyObject* other, int op) noexcept {
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

PyObject* slot_tp_new(PyTypeObject* self, PyObject* args, PyObject* kwds) noexcept {
    try {
        // TODO: runtime ICs?
        Box* new_attr = typeLookup(self, _new_str, NULL);
        assert(new_attr);
        new_attr = processDescriptor(new_attr, None, self);

        return runtimeCall(new_attr, ArgPassSpec(1, 0, true, true), self, args, kwds, NULL, NULL);
    } catch (Box* e) {
        PyErr_SetObject(e->cls, e);
        return NULL;
    }
}

static int slot_tp_init(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
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
    try {
        return getitem(self, boxInt(i));
    } catch (Box* e) {
        PyErr_SetObject(e->cls, e);
        return NULL;
    }
}

static Py_ssize_t slot_sq_length(PyObject* self) noexcept {
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
    static PyObject* getslice_str;

    if (PyErr_WarnPy3k("in 3.x, __getslice__ has been removed; "
                       "use __getitem__",
                       1) < 0)
        return NULL;
    return call_method(self, "__getslice__", &getslice_str, "nn", i, j);
}

static int slot_sq_ass_item(PyObject* self, Py_ssize_t index, PyObject* value) {
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

static int slot_sq_ass_slice(PyObject* self, Py_ssize_t i, Py_ssize_t j, PyObject* value) {
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

static int slot_sq_contains(PyObject* self, PyObject* value) {
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
        static PyObject* cache_str;                                                                                    \
        return call_method(self, OPSTR, &cache_str, "()");                                                             \
    }

#define SLOT1(FUNCNAME, OPSTR, ARG1TYPE, ARGCODES)                                                                     \
    static PyObject* FUNCNAME(PyObject* self, ARG1TYPE arg1) noexcept {                                                \
        static PyObject* cache_str;                                                                                    \
        return call_method(self, OPSTR, &cache_str, "(" ARGCODES ")", arg1);                                           \
    }

/* Boolean helper for SLOT1BINFULL().
   right.__class__ is a nontrivial subclass of left.__class__. */
static int method_is_overloaded(PyObject* left, PyObject* right, const char* name) {
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
    static PyObject* FUNCNAME(PyObject* self, PyObject* other) {                                                       \
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

static int slot_mp_ass_subscript(PyObject* self, PyObject* key, PyObject* value) {
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

static PyObject* slot_nb_power(PyObject*, PyObject*, PyObject*);

SLOT1BINFULL(slot_nb_power_binary, slot_nb_power, nb_power, "__pow__", "__rpow__")

static PyObject* slot_nb_power(PyObject* self, PyObject* other, PyObject* modulus) {
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

SLOT0(slot_nb_invert, "__invert__")
SLOT1BIN(slot_nb_lshift, nb_lshift, "__lshift__", "__rlshift__")
SLOT1BIN(slot_nb_rshift, nb_rshift, "__rshift__", "__rrshift__")
SLOT1BIN(slot_nb_and, nb_and, "__and__", "__rand__")
SLOT1BIN(slot_nb_xor, nb_xor, "__xor__", "__rxor__")
SLOT1BIN(slot_nb_or, nb_or, "__or__", "__ror__")

static int slot_nb_coerce(PyObject** a, PyObject** b);

SLOT0(slot_nb_int, "__int__")
SLOT0(slot_nb_long, "__long__")
SLOT0(slot_nb_float, "__float__")
SLOT0(slot_nb_oct, "__oct__")
SLOT0(slot_nb_hex, "__hex__")

typedef wrapper_def slotdef;

static void** slotptr(BoxedClass* type, int offset) {
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

static void update_one_slot(BoxedClass* self, const slotdef& p) {
    // TODO: CPython version is significantly more sophisticated
    void** ptr = slotptr(self, p.offset);
    Box* attr = typeLookup(self, p.name, NULL);

    if (!ptr) {
        assert(!attr && "I don't think this case should happen? CPython handles it though");
        return;
    }

    if (attr) {
        if (attr == None && ptr == (void**)&self->tp_hash) {
            *ptr = (void*)&PyObject_HashNotImplemented;
        } else {
            *ptr = p.function;
        }
    } else {
        *ptr = NULL;
    }
}

// Copied from CPython:
#define TPSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), 0 }
#define FLSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC, FLAGS)                                                              \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), FLAGS }
#define ETSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    { NAME, offsetof(PyHeapTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), 0 }
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

static slotdef slotdefs[] = {
    TPSLOT("__repr__", tp_repr, slot_tp_repr, wrap_unaryfunc, "x.__repr__() <==> repr(x)"),
    TPSLOT("__hash__", tp_hash, slot_tp_hash, wrap_hashfunc, "x.__hash__() <==> hash(x)"),
    FLSLOT("__call__", tp_call, slot_tp_call, (wrapperfunc)wrap_call, "x.__call__(...) <==> x(...)",
           PyWrapperFlag_KEYWORDS),
    TPSLOT("__str__", tp_str, slot_tp_str, wrap_unaryfunc, "x.__str__() <==> str(x)"),
    TPSLOT("__lt__", tp_richcompare, slot_tp_richcompare, richcmp_lt, "x.__lt__(y) <==> x<y"),
    TPSLOT("__le__", tp_richcompare, slot_tp_richcompare, richcmp_le, "x.__le__(y) <==> x<=y"),
    TPSLOT("__eq__", tp_richcompare, slot_tp_richcompare, richcmp_eq, "x.__eq__(y) <==> x==y"),
    TPSLOT("__ne__", tp_richcompare, slot_tp_richcompare, richcmp_ne, "x.__ne__(y) <==> x!=y"),
    TPSLOT("__gt__", tp_richcompare, slot_tp_richcompare, richcmp_gt, "x.__gt__(y) <==> x>y"),
    TPSLOT("__ge__", tp_richcompare, slot_tp_richcompare, richcmp_ge, "x.__ge__(y) <==> x>=y"),

    FLSLOT("__init__", tp_init, slot_tp_init, (wrapperfunc)wrap_init, "x.__init__(...) initializes x; "
                                                                      "see help(type(x)) for signature",
           PyWrapperFlag_KEYWORDS),
    TPSLOT("__new__", tp_new, slot_tp_new, NULL, ""),

    BINSLOT("__add__", nb_add, slot_nb_add, "+"),               // [force clang-format to line break]
    RBINSLOT("__radd__", nb_add, slot_nb_add, "+"),             //
    BINSLOT("__sub__", nb_subtract, slot_nb_subtract, "-"),     //
    RBINSLOT("__rsub__", nb_subtract, slot_nb_subtract, "-"),   //
    BINSLOT("__mul__", nb_multiply, slot_nb_multiply, "*"),     //
    RBINSLOT("__rmul__", nb_multiply, slot_nb_multiply, "*"),   //
    BINSLOT("__div__", nb_divide, slot_nb_divide, "/"),         //
    RBINSLOT("__rdiv__", nb_divide, slot_nb_divide, "/"),       //
    BINSLOT("__mod__", nb_remainder, slot_nb_remainder, "%"),   //
    RBINSLOT("__rmod__", nb_remainder, slot_nb_remainder, "%"), //
    BINSLOTNOTINFIX("__divmod__", nb_divmod, slot_nb_divmod, "divmod(x, y)"),
    RBINSLOTNOTINFIX("__rdivmod__", nb_divmod, slot_nb_divmod, "divmod(y, x)"),
    NBSLOT("__pow__", nb_power, slot_nb_power, wrap_ternaryfunc, "x.__pow__(y[, z]) <==> pow(x, y[, z])"),
    NBSLOT("__rpow__", nb_power, slot_nb_power, wrap_ternaryfunc_r, "y.__rpow__(x[, z]) <==> pow(x, y[, z])"),
    UNSLOT("__neg__", nb_negative, slot_nb_negative, wrap_unaryfunc, "-x"),         //
    UNSLOT("__pos__", nb_positive, slot_nb_positive, wrap_unaryfunc, "+x"),         //
    UNSLOT("__abs__", nb_absolute, slot_nb_absolute, wrap_unaryfunc, "abs(x)"),     //
    UNSLOT("__nonzero__", nb_nonzero, slot_nb_nonzero, wrap_inquirypred, "x != 0"), //
    UNSLOT("__invert__", nb_invert, slot_nb_invert, wrap_unaryfunc, "~x"),          //
    BINSLOT("__lshift__", nb_lshift, slot_nb_lshift, "<<"),                         //
    RBINSLOT("__rlshift__", nb_lshift, slot_nb_lshift, "<<"),                       //
    BINSLOT("__rshift__", nb_rshift, slot_nb_rshift, ">>"),                         //
    RBINSLOT("__rrshift__", nb_rshift, slot_nb_rshift, ">>"),                       //
    BINSLOT("__and__", nb_and, slot_nb_and, "&"),                                   //
    RBINSLOT("__rand__", nb_and, slot_nb_and, "&"),                                 //
    BINSLOT("__xor__", nb_xor, slot_nb_xor, "^"),                                   //
    RBINSLOT("__rxor__", nb_xor, slot_nb_xor, "^"),                                 //
    BINSLOT("__or__", nb_or, slot_nb_or, "|"),                                      //
    RBINSLOT("__ror__", nb_or, slot_nb_or, "|"),                                    //
    UNSLOT("__int__", nb_int, slot_nb_int, wrap_unaryfunc, "int(x)"),               //
    UNSLOT("__long__", nb_long, slot_nb_long, wrap_unaryfunc, "long(x)"),           //
    UNSLOT("__float__", nb_float, slot_nb_float, wrap_unaryfunc, "float(x)"),       //
    UNSLOT("__oct__", nb_oct, slot_nb_oct, wrap_unaryfunc, "oct(x)"),               //
    UNSLOT("__hex__", nb_hex, slot_nb_hex, wrap_unaryfunc, "hex(x)"),               //

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
};

static void init_slotdefs() {
    static bool initialized = false;
    if (initialized)
        return;

    for (int i = 0; i < sizeof(slotdefs) / sizeof(slotdefs[0]); i++) {
        if (i > 0) {
#ifndef NDEBUG
            if (slotdefs[i - 1].offset > slotdefs[i].offset) {
                printf("slotdef for %s in the wrong place\n", slotdefs[i - 1].name);
                for (int j = i; j < sizeof(slotdefs) / sizeof(slotdefs[0]); j++) {
                    if (slotdefs[i - 1].offset <= slotdefs[j].offset) {
                        printf("Should go before %s\n", slotdefs[j].name);
                        break;
                    }
                }
            }
#endif
            ASSERT(slotdefs[i].offset >= slotdefs[i - 1].offset, "%d %s", i, slotdefs[i - 1].name);
            // CPython interns the name here
        }
    }

    initialized = true;
}

bool update_slot(BoxedClass* self, const std::string& attr) {
    bool updated = false;
    for (const slotdef& p : slotdefs) {
        if (p.name == attr) {
            // TODO update subclasses;
            update_one_slot(self, p);
            updated = true;
        }
    }
    return updated;
}

void fixup_slot_dispatchers(BoxedClass* self) {
    init_slotdefs();

    for (const slotdef& p : slotdefs) {
        update_one_slot(self, p);
    }

    // TODO: CPython handles this by having the __name__ attribute wrap (via a getset object)
    // the tp_name field, whereas we're (needlessly?) doing the opposite.
    if (!self->tp_name) {
        Box* b = self->getattr("__name__");
        assert(b);
        assert(b->cls == str_cls);
        self->tp_name = static_cast<BoxedString*>(b)->s.c_str();
    }
}

static PyObject* tp_new_wrapper(PyTypeObject* self, BoxedTuple* args, Box* kwds) {
    RELEASE_ASSERT(isSubclass(self->cls, type_cls), "");

    // ASSERT(self->tp_new != Py_CallPythonNew, "going to get in an infinite loop");

    RELEASE_ASSERT(args->cls == tuple_cls, "");
    RELEASE_ASSERT(kwds->cls == dict_cls, "");
    RELEASE_ASSERT(args->elts.size() >= 1, "");

    BoxedClass* subtype = static_cast<BoxedClass*>(args->elts[0]);
    RELEASE_ASSERT(isSubclass(subtype->cls, type_cls), "");
    RELEASE_ASSERT(isSubclass(subtype, self), "");

    BoxedTuple* new_args = new BoxedTuple(BoxedTuple::GCVector(args->elts.begin() + 1, args->elts.end()));

    return self->tp_new(subtype, new_args, kwds);
}

static void add_tp_new_wrapper(BoxedClass* type) {
    if (type->getattr("__new__"))
        return;

    type->giveAttr("__new__",
                   new BoxedCApiFunction(METH_VARARGS | METH_KEYWORDS, type, "__new__", (PyCFunction)tp_new_wrapper));
}

static void add_operators(BoxedClass* cls) {
    init_slotdefs();

    for (const slotdef& p : slotdefs) {
        if (!p.wrapper)
            continue;

        void** ptr = slotptr(cls, p.offset);

        if (!ptr || !*ptr)
            continue;
        if (cls->getattr(p.name))
            continue;

        if (*ptr == PyObject_HashNotImplemented) {
            cls->giveAttr(p.name, None);
        } else {
            cls->giveAttr(p.name, new BoxedWrapperDescriptor(&p, cls, *ptr));
        }
    }

    if (cls->tp_new)
        add_tp_new_wrapper(cls);
}

extern "C" int PyType_IsSubtype(PyTypeObject* a, PyTypeObject* b) {
    return isSubclass(a, b);
}

#define BUFFER_FLAGS (Py_TPFLAGS_HAVE_GETCHARBUFFER | Py_TPFLAGS_HAVE_NEWBUFFER)

// This is copied from CPython with some modifications:
static void inherit_special(PyTypeObject* type, PyTypeObject* base) {
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

// Pyston change: are not using these for now:
#if 0
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
#endif
}

static int overrides_name(PyTypeObject* type, const char* name) {
    PyObject* dict = type->tp_dict;

    assert(dict != NULL);
    if (PyDict_GetItemString(dict, name) != NULL) {
        return 1;
    }
    return 0;
}

#define OVERRIDES_HASH(x) overrides_name(x, "__hash__")
#define OVERRIDES_EQ(x) overrides_name(x, "__eq__")

static void inherit_slots(PyTypeObject* type, PyTypeObject* base) {
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
            type->tp_free = PyObject_GC_Del;
        }
        /* else they didn't agree about gc, and there isn't something
         * obvious to be done -- the type is on its own.
         */
    }
}

extern "C" int PyType_Ready(PyTypeObject* cls) {
    gc::registerNonheapRootObject(cls);

    // unhandled fields:
    RELEASE_ASSERT(cls->tp_print == NULL, "");
    RELEASE_ASSERT(cls->tp_getattr == NULL, "");
    RELEASE_ASSERT(cls->tp_setattr == NULL, "");
    RELEASE_ASSERT(cls->tp_compare == NULL, "");

    if (cls->tp_as_number) {
        auto num = cls->tp_as_number;
        // Members not added yet:
        assert(num->nb_coerce == NULL);
        assert(num->nb_inplace_add == NULL);
        assert(num->nb_inplace_subtract == NULL);
        assert(num->nb_inplace_multiply == NULL);
        assert(num->nb_inplace_divide == NULL);
        assert(num->nb_inplace_remainder == NULL);
        assert(num->nb_inplace_power == NULL);
        assert(num->nb_inplace_lshift == NULL);
        assert(num->nb_inplace_rshift == NULL);
        assert(num->nb_inplace_and == NULL);
        assert(num->nb_inplace_xor == NULL);
        assert(num->nb_inplace_or == NULL);
        assert(num->nb_floor_divide == NULL);
        assert(num->nb_true_divide == NULL);
        assert(num->nb_inplace_floor_divide == NULL);
        assert(num->nb_inplace_true_divide == NULL);
        assert(num->nb_index == NULL);
    }

    RELEASE_ASSERT(cls->tp_getattro == NULL || cls->tp_getattro == PyObject_GenericGetAttr, "");
    RELEASE_ASSERT(cls->tp_setattro == NULL || cls->tp_setattro == PyObject_GenericSetAttr, "");
    RELEASE_ASSERT(cls->tp_as_buffer == NULL, "");

    int ALLOWABLE_FLAGS = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_CHECKTYPES;
    RELEASE_ASSERT((cls->tp_flags & ~ALLOWABLE_FLAGS) == 0, "");
    if (cls->tp_as_number) {
        RELEASE_ASSERT(cls->tp_flags & Py_TPFLAGS_CHECKTYPES, "Pyston doesn't yet support non-checktypes behavior");
    }

    RELEASE_ASSERT(cls->tp_iter == NULL, "");
    RELEASE_ASSERT(cls->tp_iternext == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_get == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_set == NULL, "");
    RELEASE_ASSERT(cls->tp_alloc == NULL || cls->tp_alloc == PyType_GenericAlloc, "");
    RELEASE_ASSERT(cls->tp_free == NULL || cls->tp_free == PyObject_Del, "");
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

#define INITIALIZE(a) new (&(a)) decltype(a)
    INITIALIZE(cls->attrs);
    INITIALIZE(cls->dependent_icgetattrs);
#undef INITIALIZE

    BoxedClass* base = cls->tp_base;
    if (base == NULL)
        base = cls->tp_base = object_cls;
    if (!cls->cls)
        cls->cls = cls->tp_base->cls;

    assert(cls->tp_dict == NULL);
    cls->tp_dict = makeAttrWrapper(cls);

    assert(cls->tp_name);
    cls->giveAttr("__name__", boxStrConstant(cls->tp_name));
    // tp_name
    // tp_basicsize, tp_itemsize
    // tp_doc

    if (!cls->tp_new && base != object_cls)
        cls->tp_new = base->tp_new;

    if (!cls->tp_alloc) {
        cls->tp_alloc = reinterpret_cast<decltype(cls->tp_alloc)>(PyType_GenericAlloc);
    }

    try {
        add_operators(cls);
    } catch (Box* b) {
        abort();
    }

    for (PyMethodDef* method = cls->tp_methods; method && method->ml_name; ++method) {
        cls->giveAttr(method->ml_name, new BoxedMethodDescriptor(method, cls));
    }

    for (PyMemberDef* member = cls->tp_members; member && member->name; ++member) {
        cls->giveAttr(member->name, new BoxedMemberDescriptor(member));
    }

    if (cls->tp_getset) {
        if (VERBOSITY())
            printf("warning: ignoring tp_getset for now\n");
    }

    inherit_special(cls, cls->tp_base);

    // This is supposed to be over the MRO but we don't support multiple inheritance yet:
    BoxedClass* b = base;
    while (b) {
        // Not sure when this could fail; maybe not in Pyston right now but apparently it can in CPython:
        if (PyType_Check(b))
            inherit_slots(cls, b);

        b = b->tp_base;
    }

    cls->gc_visit = &conservativeGCHandler;
    cls->is_user_defined = true;

    // TODO not sure how we can handle extension types that manually
    // specify a dict...
    RELEASE_ASSERT(cls->tp_dictoffset == 0, "");
    // this should get automatically initialized to 0 on this path:
    assert(cls->attrs_offset == 0);

    return 0;
}

} // namespace pyston
