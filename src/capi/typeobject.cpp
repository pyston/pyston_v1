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


#include "capi/typeobject.h"

#include "capi/types.h"
#include "runtime/objmodel.h"

namespace pyston {

// FIXME duplicated with objmodel.cpp
static const std::string _new_str("__new__");

extern "C" void conservativeGCHandler(GCVisitor* v, Box* b) {
    v->visitPotentialRange((void* const*)b, (void* const*)((char*)b + b->cls->tp_basicsize));
}

static int check_num_args(PyObject* ob, int n) {
    if (!PyTuple_CheckExact(ob)) {
        PyErr_SetString(PyExc_SystemError, "PyArg_UnpackTuple() argument list is not a tuple");
        return 0;
    }
    if (n == PyTuple_GET_SIZE(ob))
        return 1;
    PyErr_Format(PyExc_TypeError, "expected %d arguments, got %zd", n, PyTuple_GET_SIZE(ob));
    return 0;
}

static PyObject* wrap_hashfunc(PyObject* self, PyObject* args, void* wrapped) {
    hashfunc func = (hashfunc)wrapped;
    long res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    return PyInt_FromLong(res);
}

static PyObject* wrap_call(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds) {
    ternaryfunc func = (ternaryfunc)wrapped;

    return (*func)(self, args, kwds);
}

static PyObject* wrap_richcmpfunc(PyObject* self, PyObject* args, void* wrapped, int op) {
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

static PyObject* wrap_unaryfunc(PyObject* self, PyObject* args, void* wrapped) {
    unaryfunc func = (unaryfunc)wrapped;

    if (!check_num_args(args, 0))
        return NULL;
    return (*func)(self);
}

static PyObject* wrap_binaryfunc(PyObject* self, PyObject* args, void* wrapped) {
    binaryfunc func = (binaryfunc)wrapped;
    PyObject* other;

    if (!check_num_args(args, 1))
        return NULL;
    other = PyTuple_GET_ITEM(args, 0);
    return (*func)(self, other);
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

static PyObject* wrap_lenfunc(PyObject* self, PyObject* args, void* wrapped) {
    lenfunc func = (lenfunc)wrapped;
    Py_ssize_t res;

    if (!check_num_args(args, 0))
        return NULL;
    res = (*func)(self);
    if (res == -1 && PyErr_Occurred())
        return NULL;
    return PyInt_FromLong((long)res);
}

static PyObject* wrap_indexargfunc(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_ssizessizeargfunc(PyObject* self, PyObject* args, void* wrapped) {
    ssizessizeargfunc func = (ssizessizeargfunc)wrapped;
    Py_ssize_t i, j;

    if (!PyArg_ParseTuple(args, "nn", &i, &j))
        return NULL;
    return (*func)(self, i, j);
}

static PyObject* wrap_sq_setitem(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_sq_delitem(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_ssizessizeobjargproc(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_delslice(PyObject* self, PyObject* args, void* wrapped) {
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
static PyObject* wrap_objobjproc(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_objobjargproc(PyObject* self, PyObject* args, void* wrapped) {
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

static PyObject* wrap_delitem(PyObject* self, PyObject* args, void* wrapped) {
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


PyObject* slot_tp_repr(PyObject* self) noexcept {
    try {
        return repr(self);
    } catch (Box* e) {
        abort();
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
        abort();
    }
}

static const char* name_op[] = {
    "__lt__", "__le__", "__eq__", "__ne__", "__gt__", "__ge__",
};

static PyObject* half_richcompare(PyObject* self, PyObject* other, int op) {
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

static PyObject* slot_tp_richcompare(PyObject* self, PyObject* other, int op) {
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
        abort();
    }
}

PyObject* slot_sq_item(PyObject* self, Py_ssize_t i) noexcept {
    try {
        return getitem(self, boxInt(i));
    } catch (Box* e) {
        abort();
    }
}

static Py_ssize_t slot_sq_length(PyObject* self) {
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

static PyObject* slot_sq_slice(PyObject* self, Py_ssize_t i, Py_ssize_t j) {
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
    TPSLOT("__lt__", tp_richcompare, slot_tp_richcompare, richcmp_lt, "x.__lt__(y) <==> x<y"),
    TPSLOT("__le__", tp_richcompare, slot_tp_richcompare, richcmp_le, "x.__le__(y) <==> x<=y"),
    TPSLOT("__eq__", tp_richcompare, slot_tp_richcompare, richcmp_eq, "x.__eq__(y) <==> x==y"),
    TPSLOT("__ne__", tp_richcompare, slot_tp_richcompare, richcmp_ne, "x.__ne__(y) <==> x!=y"),
    TPSLOT("__gt__", tp_richcompare, slot_tp_richcompare, richcmp_gt, "x.__gt__(y) <==> x>y"),
    TPSLOT("__ge__", tp_richcompare, slot_tp_richcompare, richcmp_ge, "x.__ge__(y) <==> x>=y"),
    TPSLOT("__new__", tp_new, slot_tp_new, NULL, ""),

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

extern "C" int PyType_Ready(PyTypeObject* cls) {
    gc::registerNonheapRootObject(cls);

    // unhandled fields:
    RELEASE_ASSERT(cls->tp_print == NULL, "");
    RELEASE_ASSERT(cls->tp_getattr == NULL, "");
    RELEASE_ASSERT(cls->tp_setattr == NULL, "");
    RELEASE_ASSERT(cls->tp_compare == NULL, "");
    RELEASE_ASSERT(cls->tp_as_number == NULL, "");
    RELEASE_ASSERT(cls->tp_str == NULL, "");
    RELEASE_ASSERT(cls->tp_getattro == NULL || cls->tp_getattro == PyObject_GenericGetAttr, "");
    RELEASE_ASSERT(cls->tp_setattro == NULL, "");
    RELEASE_ASSERT(cls->tp_as_buffer == NULL, "");

    int ALLOWABLE_FLAGS = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC;
    RELEASE_ASSERT((cls->tp_flags & ~ALLOWABLE_FLAGS) == 0, "");

    RELEASE_ASSERT(cls->tp_iter == NULL, "");
    RELEASE_ASSERT(cls->tp_iternext == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_get == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_set == NULL, "");
    RELEASE_ASSERT(cls->tp_init == NULL, "");
    RELEASE_ASSERT(cls->tp_alloc == NULL, "");
    RELEASE_ASSERT(cls->tp_free == NULL || cls->tp_free == PyObject_Del, "");
    RELEASE_ASSERT(cls->tp_is_gc == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
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

    BoxedClass* base = cls->base = object_cls;
    if (!cls->cls)
        cls->cls = cls->base->cls;

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
