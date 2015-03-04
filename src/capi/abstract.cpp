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

#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" Py_ssize_t _PyObject_LengthHint(PyObject* o, Py_ssize_t defaultvalue) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyBuffer_ToContiguous(void* buf, Py_buffer* view, Py_ssize_t len, char fort) noexcept {
    Py_FatalError("unimplemented");
}

static PyObject* type_error(const char* msg, PyObject* obj) noexcept {
    PyErr_Format(PyExc_TypeError, msg, Py_TYPE(obj)->tp_name);
    return NULL;
}

static PyObject* null_error(void) noexcept {
    if (!PyErr_Occurred())
        PyErr_SetString(PyExc_SystemError, "null argument to internal routine");
    return NULL;
}

static PyObject* objargs_mktuple(va_list va) noexcept {
    int i, n = 0;
    va_list countva;
    PyObject* result, *tmp;

#ifdef VA_LIST_IS_ARRAY
    memcpy(countva, va, sizeof(va_list));
#else
#ifdef __va_copy
    __va_copy(countva, va);
#else
    countva = va;
#endif
#endif

    while (((PyObject*)va_arg(countva, PyObject*)) != NULL)
        ++n;
    result = PyTuple_New(n);
    if (result != NULL && n > 0) {
        for (i = 0; i < n; ++i) {
            tmp = (PyObject*)va_arg(va, PyObject*);
            PyTuple_SET_ITEM(result, i, tmp);
            Py_INCREF(tmp);
        }
    }
    return result;
}

/* isinstance(), issubclass() */

/* abstract_get_bases() has logically 4 return states, with a sort of 0th
 * state that will almost never happen.
 *
 * 0. creating the __bases__ static string could get a MemoryError
 * 1. getattr(cls, '__bases__') could raise an AttributeError
 * 2. getattr(cls, '__bases__') could raise some other exception
 * 3. getattr(cls, '__bases__') could return a tuple
 * 4. getattr(cls, '__bases__') could return something other than a tuple
 *
 * Only state #3 is a non-error state and only it returns a non-NULL object
 * (it returns the retrieved tuple).
 *
 * Any raised AttributeErrors are masked by clearing the exception and
 * returning NULL.  If an object other than a tuple comes out of __bases__,
 * then again, the return value is NULL.  So yes, these two situations
 * produce exactly the same results: NULL is returned and no error is set.
 *
 * If some exception other than AttributeError is raised, then NULL is also
 * returned, but the exception is not cleared.  That's because we want the
 * exception to be propagated along.
 *
 * Callers are expected to test for PyErr_Occurred() when the return value
 * is NULL to decide whether a valid exception should be propagated or not.
 * When there's no exception to propagate, it's customary for the caller to
 * set a TypeError.
 */
static PyObject* abstract_get_bases(PyObject* cls) noexcept {
    PyObject* bases;

    /*
    static PyObject* __bases__ = NULL;
    if (__bases__ == NULL) {
        __bases__ = PyString_InternFromString("__bases__");
        if (__bases__ == NULL)
            return NULL;
    }
    */
    PyObject* __bases__ = boxStrConstant("__bases__");

    bases = PyObject_GetAttr(cls, __bases__);
    if (bases == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError))
            PyErr_Clear();
        return NULL;
    }
    if (!PyTuple_Check(bases)) {
        Py_DECREF(bases);
        return NULL;
    }
    return bases;
}


static int abstract_issubclass(PyObject* derived, PyObject* cls) noexcept {
    PyObject* bases = NULL;
    Py_ssize_t i, n;
    int r = 0;

    while (1) {
        if (derived == cls)
            return 1;
        bases = abstract_get_bases(derived);
        if (bases == NULL) {
            if (PyErr_Occurred())
                return -1;
            return 0;
        }
        n = PyTuple_GET_SIZE(bases);
        if (n == 0) {
            Py_DECREF(bases);
            return 0;
        }
        /* Avoid recursivity in the single inheritance case */
        if (n == 1) {
            derived = PyTuple_GET_ITEM(bases, 0);
            Py_DECREF(bases);
            continue;
        }
        for (i = 0; i < n; i++) {
            r = abstract_issubclass(PyTuple_GET_ITEM(bases, i), cls);
            if (r != 0)
                break;
        }
        Py_DECREF(bases);
        return r;
    }
}


static int check_class(PyObject* cls, const char* error) noexcept {
    PyObject* bases = abstract_get_bases(cls);
    if (bases == NULL) {
        /* Do not mask errors. */
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_TypeError, error);
        return 0;
    }
    Py_DECREF(bases);
    return -1;
}

static int recursive_isinstance(PyObject* inst, PyObject* cls) noexcept {
    PyObject* icls;
    int retval = 0;

    /*
    static PyObject* __class__ = NULL;
    if (__class__ == NULL) {
        __class__ = PyString_InternFromString("__class__");
        if (__class__ == NULL)
            return -1;
    }
    */
    PyObject* __class__ = boxStrConstant("__class__");

    if (PyClass_Check(cls) && PyInstance_Check(inst)) {
        PyObject* inclass = static_cast<BoxedInstance*>(inst)->inst_cls;
        retval = PyClass_IsSubclass(inclass, cls);
    } else if (PyType_Check(cls)) {
        retval = PyObject_TypeCheck(inst, (PyTypeObject*)cls);
        if (retval == 0) {
            PyObject* c = PyObject_GetAttr(inst, __class__);
            if (c == NULL) {
                PyErr_Clear();
            } else {
                if (c != (PyObject*)(inst->cls) && PyType_Check(c))
                    retval = PyType_IsSubtype((PyTypeObject*)c, (PyTypeObject*)cls);
                Py_DECREF(c);
            }
        }
    } else {
        if (!check_class(cls, "isinstance() arg 2 must be a class, type,"
                              " or tuple of classes and types"))
            return -1;
        icls = PyObject_GetAttr(inst, __class__);
        if (icls == NULL) {
            PyErr_Clear();
            retval = 0;
        } else {
            retval = abstract_issubclass(icls, cls);
            Py_DECREF(icls);
        }
    }

    return retval;
}

extern "C" int PyObject_IsInstance(PyObject* inst, PyObject* cls) noexcept {
    static PyObject* name = NULL;

    /* Quick test for an exact match */
    if (Py_TYPE(inst) == (PyTypeObject*)cls)
        return 1;

    if (PyTuple_Check(cls)) {
        Py_ssize_t i;
        Py_ssize_t n;
        int r = 0;

        if (Py_EnterRecursiveCall(" in __instancecheck__"))
            return -1;
        n = PyTuple_GET_SIZE(cls);
        for (i = 0; i < n; ++i) {
            PyObject* item = PyTuple_GET_ITEM(cls, i);
            r = PyObject_IsInstance(inst, item);
            if (r != 0)
                /* either found it, or got an error */
                break;
        }
        Py_LeaveRecursiveCall();
        return r;
    }

    if (!(PyClass_Check(cls) || PyInstance_Check(cls))) {
        PyObject* checker;
        checker = _PyObject_LookupSpecial(cls, "__instancecheck__", &name);
        if (checker != NULL) {
            PyObject* res;
            int ok = -1;
            if (Py_EnterRecursiveCall(" in __instancecheck__")) {
                Py_DECREF(checker);
                return ok;
            }
            res = PyObject_CallFunctionObjArgs(checker, inst, NULL);
            Py_LeaveRecursiveCall();
            Py_DECREF(checker);
            if (res != NULL) {
                ok = PyObject_IsTrue(res);
                Py_DECREF(res);
            }
            return ok;
        } else if (PyErr_Occurred())
            return -1;
    }
    return recursive_isinstance(inst, cls);
}

extern "C" PyObject* PyObject_CallFunctionObjArgs(PyObject* callable, ...) noexcept {
    PyObject* args, *tmp;
    va_list vargs;

    if (callable == NULL)
        return null_error();

    /* count the args */
    va_start(vargs, callable);
    args = objargs_mktuple(vargs);
    va_end(vargs);
    if (args == NULL)
        return NULL;
    tmp = PyObject_Call(callable, args, NULL);
    Py_DECREF(args);

    return tmp;
}

extern "C" PyObject* PyObject_CallObject(PyObject* obj, PyObject* args) noexcept {
    // TODO do something like this?  not sure if this is safe; will people expect that calling into a known function
    // won't end up doing a GIL check?
    // threading::GLDemoteRegion _gil_demote;

    try {
        Box* r;
        if (args)
            r = runtimeCall(obj, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
        else
            r = runtimeCall(obj, ArgPassSpec(0, 0, false, false), NULL, NULL, NULL, NULL, NULL);
        return r;
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" int PyObject_AsReadBuffer(PyObject* obj, const void** buffer, Py_ssize_t* buffer_len) noexcept {
    Py_FatalError("unimplemented");
}

static PyObject* call_function_tail(PyObject* callable, PyObject* args) {
    PyObject* retval;

    if (args == NULL)
        return NULL;

    if (!PyTuple_Check(args)) {
        PyObject* a;

        a = PyTuple_New(1);
        if (a == NULL) {
            Py_DECREF(args);
            return NULL;
        }
        PyTuple_SET_ITEM(a, 0, args);
        args = a;
    }
    retval = PyObject_Call(callable, args, NULL);

    Py_DECREF(args);

    return retval;
}

extern "C" PyObject* PyObject_CallMethod(PyObject* o, const char* name, const char* format, ...) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_CallMethodObjArgs(PyObject* callable, PyObject* name, ...) noexcept {
    PyObject* args, *tmp;
    va_list vargs;

    if (callable == NULL || name == NULL)
        return null_error();

    callable = PyObject_GetAttr(callable, name);
    if (callable == NULL)
        return NULL;

    /* count the args */
    va_start(vargs, name);
    args = objargs_mktuple(vargs);
    va_end(vargs);
    if (args == NULL) {
        Py_DECREF(callable);
        return NULL;
    }
    tmp = PyObject_Call(callable, args, NULL);
    Py_DECREF(args);
    Py_DECREF(callable);

    return tmp;
}


extern "C" PyObject* _PyObject_CallMethod_SizeT(PyObject* o, const char* name, const char* format, ...) noexcept {
    // TODO it looks like this could be made much more efficient by calling our callattr(), but
    // I haven't taken the time to verify that that has the same behavior

    va_list va;
    PyObject* args;
    PyObject* func = NULL;
    PyObject* retval = NULL;

    if (o == NULL || name == NULL)
        return null_error();

    func = PyObject_GetAttrString(o, name);
    if (func == NULL) {
        PyErr_SetString(PyExc_AttributeError, name);
        return 0;
    }

    if (!PyCallable_Check(func)) {
        type_error("attribute of type '%.200s' is not callable", func);
        goto exit;
    }

    if (format && *format) {
        va_start(va, format);
        args = _Py_VaBuildValue_SizeT(format, va);
        va_end(va);
    } else
        args = PyTuple_New(0);

    retval = call_function_tail(func, args);

exit:
    /* args gets consumed in call_function_tail */
    Py_XDECREF(func);

    return retval;
}

extern "C" Py_ssize_t PyObject_Size(PyObject* o) noexcept {
    try {
        return len(o)->n;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" PyObject* PyObject_GetIter(PyObject* o) noexcept {
    try {
        return getiter(o);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyObject_Repr(PyObject* obj) noexcept {
    try {
        return repr(obj);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static int recursive_issubclass(PyObject* derived, PyObject* cls) noexcept {
    int retval;

    if (PyType_Check(cls) && PyType_Check(derived)) {
        /* Fast path (non-recursive) */
        return PyType_IsSubtype((PyTypeObject*)derived, (PyTypeObject*)cls);
    }
    if (!PyClass_Check(derived) || !PyClass_Check(cls)) {
        if (!check_class(derived, "issubclass() arg 1 must be a class"))
            return -1;

        if (!check_class(cls, "issubclass() arg 2 must be a class"
                              " or tuple of classes"))
            return -1;
        retval = abstract_issubclass(derived, cls);
    } else {
        /* shortcut */
        if (!(retval = (derived == cls)))
            retval = PyClass_IsSubclass(derived, cls);
    }

    return retval;
}

extern "C" int PyObject_IsSubclass(PyObject* derived, PyObject* cls) noexcept {
    static PyObject* name = NULL;

    if (PyTuple_Check(cls)) {
        Py_ssize_t i;
        Py_ssize_t n;
        int r = 0;

        if (Py_EnterRecursiveCall(" in __subclasscheck__"))
            return -1;
        n = PyTuple_GET_SIZE(cls);
        for (i = 0; i < n; ++i) {
            PyObject* item = PyTuple_GET_ITEM(cls, i);
            r = PyObject_IsSubclass(derived, item);
            if (r != 0)
                /* either found it, or got an error */
                break;
        }
        Py_LeaveRecursiveCall();
        return r;
    }
    if (!(PyClass_Check(cls) || PyInstance_Check(cls))) {
        PyObject* checker;
        checker = _PyObject_LookupSpecial(cls, "__subclasscheck__", &name);
        if (checker != NULL) {
            PyObject* res;
            int ok = -1;
            if (Py_EnterRecursiveCall(" in __subclasscheck__")) {
                Py_DECREF(checker);
                return ok;
            }
            res = PyObject_CallFunctionObjArgs(checker, derived, NULL);
            Py_LeaveRecursiveCall();
            Py_DECREF(checker);
            if (res != NULL) {
                ok = PyObject_IsTrue(res);
                Py_DECREF(res);
            }
            return ok;
        } else if (PyErr_Occurred()) {
            return -1;
        }
    }
    return recursive_issubclass(derived, cls);
}

extern "C" PyObject* _PyObject_CallFunction_SizeT(PyObject* callable, const char* format, ...) noexcept {
    va_list va;
    PyObject* args;

    if (callable == NULL)
        return null_error();

    if (format && *format) {
        va_start(va, format);
        args = _Py_VaBuildValue_SizeT(format, va);
        va_end(va);
    } else
        args = PyTuple_New(0);

    return call_function_tail(callable, args);
}

#define NEW_STYLE_NUMBER(o) PyType_HasFeature((o)->cls, Py_TPFLAGS_CHECKTYPES)

#define NB_SLOT(x) offsetof(PyNumberMethods, x)
#define NB_BINOP(nb_methods, slot) (*(binaryfunc*)(&((char*)nb_methods)[slot]))
#define NB_TERNOP(nb_methods, slot) (*(ternaryfunc*)(&((char*)nb_methods)[slot]))

extern "C" int PySequence_Check(PyObject* s) noexcept {
    if (s == NULL)
        return 0;
    if (PyInstance_Check(s))
        return PyObject_HasAttrString(s, "__getitem__");
    if (PyDict_Check(s))
        return 0;
    return s->cls->tp_as_sequence && s->cls->tp_as_sequence->sq_item != NULL;
}

extern "C" Py_ssize_t PySequence_Size(PyObject* s) noexcept {
    PySequenceMethods* m;

    if (s == NULL) {
        null_error();
        return -1;
    }

    m = s->cls->tp_as_sequence;
    if (m && m->sq_length)
        return m->sq_length(s);

    type_error("object of type '%.200s' has no len()", s);
    return -1;
}

extern "C" PyObject* PySequence_Fast(PyObject* v, const char* m) noexcept {
    PyObject* it;

    if (v == NULL)
        return null_error();

    if (PyList_CheckExact(v) || PyTuple_CheckExact(v)) {
        Py_INCREF(v);
        return v;
    }

    it = PyObject_GetIter(v);
    if (it == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError))
            PyErr_SetString(PyExc_TypeError, m);
        return NULL;
    }

    v = PySequence_List(it);
    Py_DECREF(it);

    return v;
}

extern "C" void* PyBuffer_GetPointer(Py_buffer* view, Py_ssize_t* indices) noexcept {
    char* pointer;
    int i;
    pointer = (char*)view->buf;
    for (i = 0; i < view->ndim; i++) {
        pointer += view->strides[i] * indices[i];
        if ((view->suboffsets != NULL) && (view->suboffsets[i] >= 0)) {
            pointer = *((char**)pointer) + view->suboffsets[i];
        }
    }
    return (void*)pointer;
}

extern "C" void _Py_add_one_to_index_F(int nd, Py_ssize_t* index, const Py_ssize_t* shape) noexcept {
    int k;

    for (k = 0; k < nd; k++) {
        if (index[k] < shape[k] - 1) {
            index[k]++;
            break;
        } else {
            index[k] = 0;
        }
    }
}

extern "C" void _Py_add_one_to_index_C(int nd, Py_ssize_t* index, const Py_ssize_t* shape) noexcept {
    int k;

    for (k = nd - 1; k >= 0; k--) {
        if (index[k] < shape[k] - 1) {
            index[k]++;
            break;
        } else {
            index[k] = 0;
        }
    }
}

extern "C" int PyObject_CopyData(PyObject* dest, PyObject* src) noexcept {
    Py_buffer view_dest, view_src;
    int k;
    Py_ssize_t* indices, elements;
    char* dptr, *sptr;

    if (!PyObject_CheckBuffer(dest) || !PyObject_CheckBuffer(src)) {
        PyErr_SetString(PyExc_TypeError, "both destination and source must have the "
                                         "buffer interface");
        return -1;
    }

    if (PyObject_GetBuffer(dest, &view_dest, PyBUF_FULL) != 0)
        return -1;
    if (PyObject_GetBuffer(src, &view_src, PyBUF_FULL_RO) != 0) {
        PyBuffer_Release(&view_dest);
        return -1;
    }

    if (view_dest.len < view_src.len) {
        PyErr_SetString(PyExc_BufferError, "destination is too small to receive data from source");
        PyBuffer_Release(&view_dest);
        PyBuffer_Release(&view_src);
        return -1;
    }

    if ((PyBuffer_IsContiguous(&view_dest, 'C') && PyBuffer_IsContiguous(&view_src, 'C'))
        || (PyBuffer_IsContiguous(&view_dest, 'F') && PyBuffer_IsContiguous(&view_src, 'F'))) {
        /* simplest copy is all that is needed */
        memcpy(view_dest.buf, view_src.buf, view_src.len);
        PyBuffer_Release(&view_dest);
        PyBuffer_Release(&view_src);
        return 0;
    }

    /* Otherwise a more elaborate copy scheme is needed */

    /* XXX(nnorwitz): need to check for overflow! */
    indices = (Py_ssize_t*)PyMem_Malloc(sizeof(Py_ssize_t) * view_src.ndim);
    if (indices == NULL) {
        PyErr_NoMemory();
        PyBuffer_Release(&view_dest);
        PyBuffer_Release(&view_src);
        return -1;
    }
    for (k = 0; k < view_src.ndim; k++) {
        indices[k] = 0;
    }
    elements = 1;
    for (k = 0; k < view_src.ndim; k++) {
        /* XXX(nnorwitz): can this overflow? */
        elements *= view_src.shape[k];
    }
    while (elements--) {
        _Py_add_one_to_index_C(view_src.ndim, indices, view_src.shape);
        dptr = (char*)PyBuffer_GetPointer(&view_dest, indices);
        sptr = (char*)PyBuffer_GetPointer(&view_src, indices);
        memcpy(dptr, sptr, view_src.itemsize);
    }
    PyMem_Free(indices);
    PyBuffer_Release(&view_dest);
    PyBuffer_Release(&view_src);
    return 0;
}

static PyObject* binary_op1(PyObject* v, PyObject* w, const int op_slot) {
    PyObject* x;
    binaryfunc slotv = NULL;
    binaryfunc slotw = NULL;

    if (v->cls->tp_as_number != NULL && NEW_STYLE_NUMBER(v))
        slotv = NB_BINOP(v->cls->tp_as_number, op_slot);
    if (w->cls != v->cls && w->cls->tp_as_number != NULL && NEW_STYLE_NUMBER(w)) {
        slotw = NB_BINOP(w->cls->tp_as_number, op_slot);
        if (slotw == slotv)
            slotw = NULL;
    }
    if (slotv) {
        if (slotw && PyType_IsSubtype(w->cls, v->cls)) {
            x = slotw(v, w);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x); /* can't do it */
            slotw = NULL;
        }
        x = slotv(v, w);
        if (x != Py_NotImplemented)
            return x;
        Py_DECREF(x); /* can't do it */
    }
    if (slotw) {
        x = slotw(v, w);
        if (x != Py_NotImplemented)
            return x;
        Py_DECREF(x); /* can't do it */
    }
    if (!NEW_STYLE_NUMBER(v) || !NEW_STYLE_NUMBER(w)) {
        int err = PyNumber_CoerceEx(&v, &w);
        if (err < 0) {
            return NULL;
        }
        if (err == 0) {
            PyNumberMethods* mv = v->cls->tp_as_number;
            if (mv) {
                binaryfunc slot;
                slot = NB_BINOP(mv, op_slot);
                if (slot) {
                    x = slot(v, w);
                    Py_DECREF(v);
                    Py_DECREF(w);
                    return x;
                }
            }
            /* CoerceEx incremented the reference counts */
            Py_DECREF(v);
            Py_DECREF(w);
        }
    }
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
}

static PyObject* binop_type_error(PyObject* v, PyObject* w, const char* op_name) {
    PyErr_Format(PyExc_TypeError, "unsupported operand type(s) for %.100s: "
                                  "'%.100s' and '%.100s'",
                 op_name, v->cls->tp_name, w->cls->tp_name);
    return NULL;
}

static PyObject* binary_op(PyObject* v, PyObject* w, const int op_slot, const char* op_name) {
    PyObject* result = binary_op1(v, w, op_slot);
    if (result == Py_NotImplemented) {
        Py_DECREF(result);
        return binop_type_error(v, w, op_name);
    }
    return result;
}

extern "C" PyObject* PySequence_Concat(PyObject* s, PyObject* o) noexcept {
    PySequenceMethods* m;

    if (s == NULL || o == NULL)
        return null_error();

    m = s->cls->tp_as_sequence;
    if (m && m->sq_concat)
        return m->sq_concat(s, o);

    /* Instances of user classes defining an __add__() method only
       have an nb_add slot, not an sq_concat slot.  So we fall back
       to nb_add if both arguments appear to be sequences. */
    if (PySequence_Check(s) && PySequence_Check(o)) {
        PyObject* result = binary_op1(s, o, NB_SLOT(nb_add));
        if (result != Py_NotImplemented)
            return result;
        Py_DECREF(result);
    }
    return type_error("'%.200s' object can't be concatenated", s);
}

extern "C" PyObject* PySequence_List(PyObject* v) noexcept {
    PyObject* result; /* result list */
    PyObject* rv;     /* return value from PyList_Extend */

    if (v == NULL)
        return null_error();

    result = PyList_New(0);
    if (result == NULL)
        return NULL;

    rv = _PyList_Extend((PyListObject*)result, v);
    if (rv == NULL) {
        Py_DECREF(result);
        return NULL;
    }
    Py_DECREF(rv);
    return result;
}

extern "C" PyObject* PyObject_CallFunction(PyObject* callable, const char* format, ...) noexcept {
    va_list va;
    PyObject* args;

    if (callable == NULL)
        return null_error();

    if (format && *format) {
        va_start(va, format);
        args = Py_VaBuildValue(format, va);
        va_end(va);
    } else
        args = PyTuple_New(0);

    return call_function_tail(callable, args);
}

extern "C" int PyMapping_Check(PyObject* o) noexcept {
    if (o && PyInstance_Check(o))
        return PyObject_HasAttrString(o, "__getitem__");

    return o && o->cls->tp_as_mapping && o->cls->tp_as_mapping->mp_subscript
           && !(o->cls->tp_as_sequence && o->cls->tp_as_sequence->sq_slice);
}

extern "C" Py_ssize_t PyMapping_Size(PyObject* o) noexcept {
    PyMappingMethods* m;

    if (o == NULL) {
        null_error();
        return -1;
    }

    m = o->cls->tp_as_mapping;
    if (m && m->mp_length)
        return m->mp_length(o);

    type_error("object of type '%.200s' has no len()", o);
    return -1;
}

extern "C" int PyMapping_HasKeyString(PyObject* o, char* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyMapping_HasKey(PyObject* o, PyObject* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyMapping_GetItemString(PyObject* o, char* key) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyMapping_SetItemString(PyObject* o, char* key, PyObject* v) noexcept {
    Py_FatalError("unimplemented");
}
}
