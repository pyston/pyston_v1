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

#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/ast.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static PyObject* type_error(const char* msg, PyObject* obj) noexcept {
    PyErr_Format(PyExc_TypeError, msg, Py_TYPE(obj)->tp_name);
    return NULL;
}

static PyObject* null_error(void) noexcept {
    if (!PyErr_Occurred())
        PyErr_SetString(PyExc_SystemError, "null argument to internal routine");
    return NULL;
}

extern "C" int PyObject_Cmp(PyObject* o1, PyObject* o2, int* result) noexcept {
    int r;

    if (o1 == NULL || o2 == NULL) {
        null_error();
        return -1;
    }
    r = PyObject_Compare(o1, o2);
    if (PyErr_Occurred())
        return -1;
    *result = r;
    return 0;
}

extern "C" PyObject* PyObject_Type(PyObject* o) noexcept {
    if (o == NULL)
        return null_error();
    return incref(o->cls);
}

extern "C" Py_ssize_t _PyObject_LengthHint(PyObject* o, Py_ssize_t defaultvalue) noexcept {
    static PyObject* hintstrobj = NULL;
    PyObject* ro, *hintmeth;
    Py_ssize_t rv;

    /* try o.__len__() */
    rv = PyObject_Size(o);
    if (rv >= 0)
        return rv;
    if (PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_AttributeError))
            return -1;
        PyErr_Clear();
    }

    if (PyInstance_Check(o))
        return defaultvalue;
    /* try o.__length_hint__() */
    hintmeth = _PyObject_LookupSpecial(o, "__length_hint__", &hintstrobj);
    if (hintmeth == NULL) {
        if (PyErr_Occurred())
            return -1;
        else
            return defaultvalue;
    }
    ro = PyObject_CallFunctionObjArgs(hintmeth, NULL);
    Py_DECREF(hintmeth);
    if (ro == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_AttributeError))
            return -1;
        PyErr_Clear();
        return defaultvalue;
    }
    rv = PyNumber_Check(ro) ? PyInt_AsSsize_t(ro) : defaultvalue;
    Py_DECREF(ro);
    return rv;
}

static int _IsFortranContiguous(Py_buffer* view) {
    Py_ssize_t sd, dim;
    int i;

    if (view->ndim == 0)
        return 1;
    if (view->strides == NULL)
        return (view->ndim == 1);

    sd = view->itemsize;
    if (view->ndim == 1)
        return (view->shape[0] == 1 || sd == view->strides[0]);
    for (i = 0; i < view->ndim; i++) {
        dim = view->shape[i];
        if (dim == 0)
            return 1;
        if (view->strides[i] != sd)
            return 0;
        sd *= dim;
    }
    return 1;
}

static int _IsCContiguous(Py_buffer* view) {
    Py_ssize_t sd, dim;
    int i;

    if (view->ndim == 0)
        return 1;
    if (view->strides == NULL)
        return 1;

    sd = view->itemsize;
    if (view->ndim == 1)
        return (view->shape[0] == 1 || sd == view->strides[0]);
    for (i = view->ndim - 1; i >= 0; i--) {
        dim = view->shape[i];
        if (dim == 0)
            return 1;
        if (view->strides[i] != sd)
            return 0;
        sd *= dim;
    }
    return 1;
}

extern "C" int PyBuffer_IsContiguous(Py_buffer* view, char fort) noexcept {
    if (view->suboffsets != NULL)
        return 0;

    if (fort == 'C')
        return _IsCContiguous(view);
    else if (fort == 'F')
        return _IsFortranContiguous(view);
    else if (fort == 'A')
        return (_IsCContiguous(view) || _IsFortranContiguous(view));
    return 0;
}

/* view is not checked for consistency in either of these.  It is
   assumed that the size of the buffer is view->len in
   view->len / view->itemsize elements.
*/
extern "C" int PyBuffer_ToContiguous(void* buf, Py_buffer* view, Py_ssize_t len, char fort) noexcept {
    int k;
    void (*addone)(int, Py_ssize_t*, const Py_ssize_t*);
    Py_ssize_t* indices, elements;
    char* dest, *ptr;

    if (len > view->len) {
        len = view->len;
    }

    if (PyBuffer_IsContiguous(view, fort)) {
        /* simplest copy is all that is needed */
        memcpy(buf, view->buf, len);
        return 0;
    }

    /* Otherwise a more elaborate scheme is needed */

    /* XXX(nnorwitz): need to check for overflow! */
    indices = (Py_ssize_t*)PyMem_Malloc(sizeof(Py_ssize_t) * (view->ndim));
    if (indices == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (k = 0; k < view->ndim; k++) {
        indices[k] = 0;
    }

    if (fort == 'F') {
        addone = _Py_add_one_to_index_F;
    } else {
        addone = _Py_add_one_to_index_C;
    }
    dest = (char*)buf;
    /* XXX : This is not going to be the fastest code in the world
             several optimizations are possible.
     */
    elements = len / view->itemsize;
    while (elements--) {
        addone(view->ndim, indices, view->shape);
        ptr = (char*)PyBuffer_GetPointer(view, indices);
        memcpy(dest, ptr, view->itemsize);
        dest += view->itemsize;
    }
    PyMem_Free(indices);
    return 0;
}

extern "C" int PyBuffer_FillInfo(Py_buffer* view, PyObject* obj, void* buf, Py_ssize_t len, int readonly,
                                 int flags) noexcept {
    if (view == NULL)
        return 0;
    if (((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) && (readonly == 1)) {
        // https://docs.python.org/3/c-api/buffer.html#c.PyBuffer_FillInfo
        // '[On failure], raise PyExc_BufferError, set view->obj to NULL and return -1'
        view->obj = NULL;
        PyErr_SetString(PyExc_BufferError, "Object is not writable.");
        return -1;
    }

    view->obj = obj;
    if (obj)
        Py_INCREF(obj);
    view->buf = buf;
    view->len = len;
    view->readonly = readonly;
    view->itemsize = 1;
    view->format = NULL;
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT)
        view->format = "B";
    view->ndim = 1;
    view->shape = NULL;
    if ((flags & PyBUF_ND) == PyBUF_ND)
        view->shape = &(view->len);
    view->strides = NULL;
    if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES)
        view->strides = &(view->itemsize);
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

extern "C" void PyBuffer_Release(Py_buffer* view) noexcept {
    if (!view->buf) {
        assert(!view->obj);
        return;
    }

    PyObject* obj = view->obj;
    if (obj) {
        if (obj && Py_TYPE(obj)->tp_as_buffer && Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer)
            Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer(obj, view);
        Py_XDECREF(obj);
    }


    view->obj = NULL;
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

    static PyObject* __bases__ = NULL;
    if (__bases__ == NULL) {
        __bases__ = getStaticString("__bases__");
        if (__bases__ == NULL)
            return NULL;
    }

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

    static PyObject* __class__ = NULL;
    if (__class__ == NULL) {
        __class__ = getStaticString("__class__");
        if (__class__ == NULL)
            return -1;
    }

    if (PyClass_Check(cls) && PyInstance_Check(inst)) {
        PyObject* inclass = static_cast<BoxedInstance*>(inst)->inst_cls;
        retval = PyClass_IsSubclass(inclass, cls);
    } else if (PyType_Check(cls)) {
        retval = PyObject_TypeCheck(inst, (PyTypeObject*)cls);
        if (retval == 0) {
            PyObject* c = NULL;

            if (!inst->cls->has_getattribute) {
                assert(inst->cls->tp_getattr == object_cls->tp_getattr);
                assert(inst->cls->tp_getattro == object_cls->tp_getattro
                       || inst->cls->tp_getattro == slot_tp_getattr_hook);
            }
            // We don't need to worry about __getattr__, since the default __class__ will always resolve.
            bool has_custom_class = inst->cls->has___class__ || inst->cls->has_getattribute;
            if (!has_custom_class) {
                assert(autoDecref(PyObject_GetAttr(inst, __class__)) == inst->cls);
            } else {
                c = PyObject_GetAttr(inst, __class__);
                if (!c)
                    PyErr_Clear();
            }

            if (c) {
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

extern "C" int _PyObject_RealIsInstance(PyObject* inst, PyObject* cls) noexcept {
    return recursive_isinstance(inst, cls);
}

extern "C" int PyObject_IsInstance(PyObject* inst, PyObject* cls) noexcept {
    STAT_TIMER(t0, "us_timer_pyobject_isinstance", 20);

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
        PyObject* checker = NULL;
        if (cls->cls->has_instancecheck) {
            checker = _PyObject_LookupSpecial(cls, "__instancecheck__", &name);
            if (!checker && PyErr_Occurred())
                return -1;

            assert(checker);
        }

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
        }
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
    return PyEval_CallObjectWithKeywords(obj, args, NULL);
}

extern "C" int PyObject_AsCharBuffer(PyObject* obj, const char** buffer, Py_ssize_t* buffer_len) noexcept {
    PyBufferProcs* pb;
    char* pp;
    Py_ssize_t len;

    if (obj == NULL || buffer == NULL || buffer_len == NULL) {
        null_error();
        return -1;
    }
    pb = obj->cls->tp_as_buffer;
    if (pb == NULL || pb->bf_getcharbuffer == NULL || pb->bf_getsegcount == NULL) {
        PyErr_SetString(PyExc_TypeError, "expected a character buffer object");
        return -1;
    }
    if ((*pb->bf_getsegcount)(obj, NULL) != 1) {
        PyErr_SetString(PyExc_TypeError, "expected a single-segment buffer object");
        return -1;
    }
    len = (*pb->bf_getcharbuffer)(obj, 0, &pp);
    if (len < 0)
        return -1;
    *buffer = pp;
    *buffer_len = len;
    return 0;
}

extern "C" int PyObject_CheckReadBuffer(PyObject* obj) noexcept {
    PyBufferProcs* pb = obj->cls->tp_as_buffer;

    if (pb == NULL || pb->bf_getreadbuffer == NULL || pb->bf_getsegcount == NULL
        || (*pb->bf_getsegcount)(obj, NULL) != 1)
        return 0;
    return 1;
}

extern "C" int PyObject_AsReadBuffer(PyObject* obj, const void** buffer, Py_ssize_t* buffer_len) noexcept {
    PyBufferProcs* pb;
    void* pp;
    Py_ssize_t len;

    if (obj == NULL || buffer == NULL || buffer_len == NULL) {
        null_error();
        return -1;
    }
    pb = obj->cls->tp_as_buffer;
    if (pb == NULL || pb->bf_getreadbuffer == NULL || pb->bf_getsegcount == NULL) {
        PyErr_SetString(PyExc_TypeError, "expected a readable buffer object");
        return -1;
    }
    if ((*pb->bf_getsegcount)(obj, NULL) != 1) {
        PyErr_SetString(PyExc_TypeError, "expected a single-segment buffer object");
        return -1;
    }
    len = (*pb->bf_getreadbuffer)(obj, 0, &pp);
    if (len < 0)
        return -1;
    *buffer = pp;
    *buffer_len = len;
    return 0;
}

extern "C" int PyObject_AsWriteBuffer(PyObject* obj, void** buffer, Py_ssize_t* buffer_len) noexcept {
    PyBufferProcs* pb;
    void* pp;
    Py_ssize_t len;

    if (obj == NULL || buffer == NULL || buffer_len == NULL) {
        null_error();
        return -1;
    }
    pb = obj->cls->tp_as_buffer;
    if (pb == NULL || pb->bf_getwritebuffer == NULL || pb->bf_getsegcount == NULL) {
        PyErr_SetString(PyExc_TypeError, "expected a writeable buffer object");
        return -1;
    }
    if ((*pb->bf_getsegcount)(obj, NULL) != 1) {
        PyErr_SetString(PyExc_TypeError, "expected a single-segment buffer object");
        return -1;
    }
    len = (*pb->bf_getwritebuffer)(obj, 0, &pp);
    if (len < 0)
        return -1;
    *buffer = pp;
    *buffer_len = len;
    return 0;
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
    va_list va;
    PyObject* args = NULL;
    PyObject* retval = NULL;

    if (o == NULL || name == NULL)
        return null_error();

    ArgPassSpec argspec(0, 0, true, false);
    if (format && *format) {
        va_start(va, format);
        args = Py_VaBuildValue(format, va);
        va_end(va);

        if (!PyTuple_Check(args))
            argspec = ArgPassSpec(1);
    } else
        argspec = ArgPassSpec(0);
    AUTO_XDECREF(args);
    retval = callattrInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(
        o, autoDecref(internStringMortal(name)), CLASS_OR_INST, NULL, argspec, args, NULL, NULL, NULL, NULL);
    if (!retval && !PyErr_Occurred())
        PyErr_SetString(PyExc_AttributeError, name);

    return retval;
}

extern "C" PyObject* PyObject_CallMethodObjArgs(PyObject* callable, PyObject* name, ...) noexcept {
    PyObject* args, *tmp;
    va_list vargs;

    if (callable == NULL || name == NULL)
        return null_error();

    /* count the args */
    va_start(vargs, name);
    args = objargs_mktuple(vargs);
    va_end(vargs);
    if (args == NULL) {
        Py_DECREF(callable);
        return NULL;
    }

    BoxedString* attr = (BoxedString*)name;
    if (!PyString_Check(attr)) {
        if (PyUnicode_Check(attr)) {
            attr = (BoxedString*)_PyUnicode_AsDefaultEncodedString(attr, NULL);
            if (attr == NULL)
                return NULL;
        } else {
            PyErr_Format(TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr)->tp_name);
            return NULL;
        }
    }

    Py_INCREF(attr);
    internStringMortalInplace(attr);
    AUTO_DECREF(attr);
    tmp = callattrInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(
        callable, attr, CLASS_OR_INST, NULL, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
    if (!tmp && !PyErr_Occurred())
        PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", callable->cls->tp_name,
                     PyString_AS_STRING(attr));


    Py_DECREF(args);

    return tmp;
}


extern "C" PyObject* _PyObject_CallMethod_SizeT(PyObject* o, const char* name, const char* format, ...) noexcept {
    PyObject* args = NULL, *tmp;
    va_list vargs;

    if (o == NULL || name == NULL)
        return null_error();

    ArgPassSpec argspec(0, 0, true, false);

    /* count the args */
    if (format && *format) {
        va_start(vargs, format);
        args = _Py_VaBuildValue_SizeT(format, vargs);
        va_end(vargs);

        if (!PyTuple_Check(args))
            argspec = ArgPassSpec(1);
    } else
        argspec = ArgPassSpec(0);
    AUTO_XDECREF(args);

    tmp = callattrInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(o, autoDecref(internStringMortal(name)), CLASS_OR_INST,
                                                                 NULL, argspec, args, NULL, NULL, NULL, NULL);
    if (!tmp && !PyErr_Occurred())
        PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name, name);

    return tmp;
}

extern "C" Py_ssize_t PyObject_Size(PyObject* o) noexcept {
    BoxedInt* r = lenInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(o, NULL);
    if (!r)
        return -1;
    AUTO_DECREF(r);
    return r->n;
}

extern "C" PyObject* PyObject_GetIter(PyObject* o) noexcept {
    try {
        return getiter(o);
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

extern "C" int _PyObject_RealIsSubclass(PyObject* derived, PyObject* cls) noexcept {
    return recursive_issubclass(derived, cls);
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
        PyObject* checker = NULL;
        if (cls->cls->has_subclasscheck) {
            checker = _PyObject_LookupSpecial(cls, "__subclasscheck__", &name);
            if (!checker && PyErr_Occurred())
                return -1;

            assert(checker);
        }

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

static PyObject* binary_iop1(PyObject* v, PyObject* w, const int iop_slot, const int op_slot) {
    PyNumberMethods* mv = v->cls->tp_as_number;
    if (mv != NULL && PyType_HasFeature((v)->cls, Py_TPFLAGS_HAVE_INPLACEOPS)) {
        binaryfunc slot = NB_BINOP(mv, iop_slot);
        if (slot) {
            PyObject* x = (slot)(v, w);
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x);
        }
    }
    return binary_op1(v, w, op_slot);
}

static PyObject* binary_iop(PyObject* v, PyObject* w, const int iop_slot, const int op_slot, const char* op_name) {
    PyObject* result = binary_iop1(v, w, iop_slot, op_slot);
    if (result == Py_NotImplemented) {
        Py_DECREF(result);
        return binop_type_error(v, w, op_name);
    }
    return result;
}

/*
  Calling scheme used for ternary operations:

  *** In some cases, w.op is called before v.op; see binary_op1. ***

  v     w       z       Action
  -------------------------------------------------------------------
  new   new     new     v.op(v,w,z), w.op(v,w,z), z.op(v,w,z)
  new   old     new     v.op(v,w,z), z.op(v,w,z), coerce(v,w,z), v.op(v,w,z)
  old   new     new     w.op(v,w,z), z.op(v,w,z), coerce(v,w,z), v.op(v,w,z)
  old   old     new     z.op(v,w,z), coerce(v,w,z), v.op(v,w,z)
  new   new     old     v.op(v,w,z), w.op(v,w,z), coerce(v,w,z), v.op(v,w,z)
  new   old     old     v.op(v,w,z), coerce(v,w,z), v.op(v,w,z)
  old   new     old     w.op(v,w,z), coerce(v,w,z), v.op(v,w,z)
  old   old     old     coerce(v,w,z), v.op(v,w,z)

  Legend:
  -------
  * new == new style number
  * old == old style number
  * Action indicates the order in which operations are tried until either
    a valid result is produced or an error occurs.
  * coerce(v,w,z) actually does: coerce(v,w), coerce(v,z), coerce(w,z) and
    only if z != Py_None; if z == Py_None, then it is treated as absent
    variable and only coerce(v,w) is tried.

 */

static PyObject* ternary_op(PyObject* v, PyObject* w, PyObject* z, const int op_slot, const char* op_name) noexcept {
    PyNumberMethods* mv, *mw, *mz;
    PyObject* x = NULL;
    ternaryfunc slotv = NULL;
    ternaryfunc slotw = NULL;
    ternaryfunc slotz = NULL;

    mv = v->cls->tp_as_number;
    mw = w->cls->tp_as_number;
    if (mv != NULL && NEW_STYLE_NUMBER(v))
        slotv = NB_TERNOP(mv, op_slot);
    if (w->cls != v->cls && mw != NULL && NEW_STYLE_NUMBER(w)) {
        slotw = NB_TERNOP(mw, op_slot);
        if (slotw == slotv)
            slotw = NULL;
    }
    if (slotv) {
        if (slotw && PyType_IsSubtype(w->cls, v->cls)) {
            x = slotw(v, w, z);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x); /* can't do it */
            slotw = NULL;
        }
        x = slotv(v, w, z);
        if (x != Py_NotImplemented)
            return x;
        Py_DECREF(x); /* can't do it */
    }
    if (slotw) {
        x = slotw(v, w, z);
        if (x != Py_NotImplemented)
            return x;
        Py_DECREF(x); /* can't do it */
    }
    mz = z->cls->tp_as_number;
    if (mz != NULL && NEW_STYLE_NUMBER(z)) {
        slotz = NB_TERNOP(mz, op_slot);
        if (slotz == slotv || slotz == slotw)
            slotz = NULL;
        if (slotz) {
            x = slotz(v, w, z);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x); /* can't do it */
        }
    }

    if (!NEW_STYLE_NUMBER(v) || !NEW_STYLE_NUMBER(w) || (z != Py_None && !NEW_STYLE_NUMBER(z))) {
        /* we have an old style operand, coerce */
        PyObject* v1, *z1, *w2, *z2;
        int c;

        c = PyNumber_Coerce(&v, &w);
        if (c != 0)
            goto error3;

        /* Special case: if the third argument is None, it is
           treated as absent argument and not coerced. */
        if (z == Py_None) {
            if (v->cls->tp_as_number) {
                slotz = NB_TERNOP(v->cls->tp_as_number, op_slot);
                if (slotz)
                    x = slotz(v, w, z);
                else
                    c = -1;
            } else
                c = -1;
            goto error2;
        }
        v1 = v;
        z1 = z;
        c = PyNumber_Coerce(&v1, &z1);
        if (c != 0)
            goto error2;
        w2 = w;
        z2 = z1;
        c = PyNumber_Coerce(&w2, &z2);
        if (c != 0)
            goto error1;

        if (v1->cls->tp_as_number != NULL) {
            slotv = NB_TERNOP(v1->cls->tp_as_number, op_slot);
            if (slotv)
                x = slotv(v1, w2, z2);
            else
                c = -1;
        } else
            c = -1;

        Py_DECREF(w2);
        Py_DECREF(z2);
    error1:
        Py_DECREF(v1);
        Py_DECREF(z1);
    error2:
        Py_DECREF(v);
        Py_DECREF(w);
    error3:
        if (c >= 0)
            return x;
    }

    if (z == Py_None)
        PyErr_Format(PyExc_TypeError, "unsupported operand type(s) for ** or pow(): "
                                      "'%.100s' and '%.100s'",
                     v->cls->tp_name, w->cls->tp_name);
    else
        PyErr_Format(PyExc_TypeError, "unsupported operand type(s) for pow(): "
                                      "'%.100s', '%.100s', '%.100s'",
                     v->cls->tp_name, w->cls->tp_name, z->cls->tp_name);
    return NULL;
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

/* Iterate over seq.  Result depends on the operation:
   PY_ITERSEARCH_COUNT:  -1 if error, else # of times obj appears in seq.
   PY_ITERSEARCH_INDEX:  0-based index of first occurrence of obj in seq;
    set ValueError and return -1 if none found; also return -1 on error.
   Py_ITERSEARCH_CONTAINS:  return 1 if obj in seq, else 0; -1 on error.
*/
extern "C" Py_ssize_t _PySequence_IterSearch(PyObject* seq, PyObject* obj, int operation) noexcept {
    Py_ssize_t n;
    int wrapped;  /* for PY_ITERSEARCH_INDEX, true iff n wrapped around */
    PyObject* it; /* iter(seq) */

    if (seq == NULL || obj == NULL) {
        null_error();
        return -1;
    }

    it = PyObject_GetIter(seq);
    if (it == NULL) {
        type_error("argument of type '%.200s' is not iterable", seq);
        return -1;
    }

    n = wrapped = 0;
    for (;;) {
        int cmp;
        PyObject* item = PyIter_Next(it);
        if (item == NULL) {
            if (PyErr_Occurred())
                goto Fail;
            break;
        }

        cmp = PyObject_RichCompareBool(obj, item, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0)
            goto Fail;
        if (cmp > 0) {
            switch (operation) {
                case PY_ITERSEARCH_COUNT:
                    if (n == PY_SSIZE_T_MAX) {
                        PyErr_SetString(PyExc_OverflowError, "count exceeds C integer size");
                        goto Fail;
                    }
                    ++n;
                    break;

                case PY_ITERSEARCH_INDEX:
                    if (wrapped) {
                        PyErr_SetString(PyExc_OverflowError, "index exceeds C integer size");
                        goto Fail;
                    }
                    goto Done;

                case PY_ITERSEARCH_CONTAINS:
                    n = 1;
                    goto Done;

                default:
                    assert(!"unknown operation");
            }
        }

        if (operation == PY_ITERSEARCH_INDEX) {
            if (n == PY_SSIZE_T_MAX)
                wrapped = 1;
            ++n;
        }
    }

    if (operation != PY_ITERSEARCH_INDEX)
        goto Done;

    PyErr_SetString(PyExc_ValueError, "sequence.index(x): x not in sequence");
/* fall into failure code */
Fail:
    n = -1;
/* fall through */
Done:
    Py_DECREF(it);
    return n;
}

extern "C" int PySequence_Contains(PyObject* seq, PyObject* ob) noexcept {
    Py_ssize_t result;
    if (PyType_HasFeature(seq->cls, Py_TPFLAGS_HAVE_SEQUENCE_IN)) {
        PySequenceMethods* sqm = seq->cls->tp_as_sequence;
        if (sqm != NULL && sqm->sq_contains != NULL)
            return (*sqm->sq_contains)(seq, ob);
    }
    result = _PySequence_IterSearch(seq, ob, PY_ITERSEARCH_CONTAINS);
    return Py_SAFE_DOWNCAST(result, Py_ssize_t, int);
}

extern "C" PyObject* PySequence_Tuple(PyObject* v) noexcept {
    PyObject* it; /* iter(v) */
    Py_ssize_t n; /* guess for result tuple size */
    PyObject* result = NULL;
    Py_ssize_t j;

    if (v == NULL)
        return null_error();

    /* Special-case the common tuple and list cases, for efficiency. */
    if (PyTuple_CheckExact(v)) {
        /* Note that we can't know whether it's safe to return
           a tuple *subclass* instance as-is, hence the restriction
           to exact tuples here.  In contrast, lists always make
           a copy, so there's no need for exactness below. */
        Py_INCREF(v);
        return v;
    }
    if (PyList_Check(v))
        return PyList_AsTuple(v);

    /* Get iterator. */
    it = PyObject_GetIter(v);
    if (it == NULL)
        return NULL;

    /* Guess result size and allocate space. */
    n = _PyObject_LengthHint(v, 10);
    if (n == -1)
        goto Fail;
    result = PyTuple_New(n);
    if (result == NULL)
        goto Fail;

    /* Fill the tuple. */
    for (j = 0;; ++j) {
        PyObject* item = PyIter_Next(it);
        if (item == NULL) {
            if (PyErr_Occurred())
                goto Fail;
            break;
        }
        if (j >= n) {
            Py_ssize_t oldn = n;
            /* The over-allocation strategy can grow a bit faster
               than for lists because unlike lists the
               over-allocation isn't permanent -- we reclaim
               the excess before the end of this routine.
               So, grow by ten and then add 25%.
            */
            n += 10;
            n += n >> 2;
            if (n < oldn) {
                /* Check for overflow */
                PyErr_NoMemory();
                Py_DECREF(item);
                goto Fail;
            }
            if (_PyTuple_Resize(&result, n) != 0) {
                Py_DECREF(item);
                goto Fail;
            }
        }
        PyTuple_SET_ITEM(result, j, item);
    }

    /* Cut tuple back if guess was too large. */
    if (j < n && _PyTuple_Resize(&result, j) != 0)
        goto Fail;

    Py_DECREF(it);
    return result;

Fail:
    Py_XDECREF(result);
    Py_DECREF(it);
    return NULL;
}

extern "C" PyObject* PySequence_Repeat(PyObject* o, Py_ssize_t count) noexcept {
    PySequenceMethods* m;

    if (o == NULL)
        return null_error();

    m = o->cls->tp_as_sequence;
    if (m && m->sq_repeat)
        return m->sq_repeat(o, count);

    /* Instances of user classes defining a __mul__() method only
       have an nb_multiply slot, not an sq_repeat slot. so we fall back
       to nb_multiply if o appears to be a sequence. */
    if (PySequence_Check(o)) {
        PyObject* n, *result;
        n = PyInt_FromSsize_t(count);
        if (n == NULL)
            return NULL;
        result = binary_op1(o, n, NB_SLOT(nb_multiply));
        Py_DECREF(n);
        if (result != Py_NotImplemented)
            return result;
        Py_DECREF(result);
    }
    return type_error("'%.200s' object can't be repeated", o);
}

extern "C" PyObject* PySequence_InPlaceConcat(PyObject* s, PyObject* o) noexcept {
    PySequenceMethods* m;

    if (s == NULL || o == NULL)
        return null_error();

    m = s->cls->tp_as_sequence;
    if (m && PyType_HasFeature((s)->cls, Py_TPFLAGS_HAVE_INPLACEOPS) && m->sq_inplace_concat)
        return m->sq_inplace_concat(s, o);
    if (m && m->sq_concat)
        return m->sq_concat(s, o);

    if (PySequence_Check(s) && PySequence_Check(o)) {
        PyObject* result = binary_iop1(s, o, NB_SLOT(nb_inplace_add), NB_SLOT(nb_add));
        if (result != Py_NotImplemented)
            return result;
        Py_DECREF(result);
    }
    return type_error("'%.200s' object can't be concatenated", s);
}

extern "C" PyObject* PySequence_InPlaceRepeat(PyObject* o, Py_ssize_t count) noexcept {
    PySequenceMethods* m;

    if (o == NULL)
        return null_error();

    m = o->cls->tp_as_sequence;
    if (m && PyType_HasFeature((o)->cls, Py_TPFLAGS_HAVE_INPLACEOPS) && m->sq_inplace_repeat)
        return m->sq_inplace_repeat(o, count);
    if (m && m->sq_repeat)
        return m->sq_repeat(o, count);

    if (PySequence_Check(o)) {
        PyObject* n, *result;
        n = PyInt_FromSsize_t(count);
        if (n == NULL)
            return NULL;
        result = binary_iop1(o, n, NB_SLOT(nb_inplace_multiply), NB_SLOT(nb_multiply));
        Py_DECREF(n);
        if (result != Py_NotImplemented)
            return result;
        Py_DECREF(result);
    }
    return type_error("'%.200s' object can't be repeated", o);
}

extern "C" PyObject* PySequence_GetItem(PyObject* s, Py_ssize_t i) noexcept {
    PySequenceMethods* m;

    if (s == NULL)
        return null_error();

    m = s->cls->tp_as_sequence;
    if (m && m->sq_item) {
        if (i < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(s);
                if (l < 0)
                    return NULL;
                i += l;
            }
        }
        return m->sq_item(s, i);
    }

    return type_error("'%.200s' object does not support indexing", s);
}

PyObject* _PySlice_FromIndices(Py_ssize_t istart, Py_ssize_t istop) {
    PyObject* start, *end, *slice;
    start = PyInt_FromSsize_t(istart);
    if (!start)
        return NULL;
    end = PyInt_FromSsize_t(istop);
    if (!end) {
        Py_DECREF(start);
        return NULL;
    }

    slice = PySlice_New(start, end, NULL);
    Py_DECREF(start);
    Py_DECREF(end);
    return slice;
}

extern "C" PyObject* PySequence_GetSlice(PyObject* s, Py_ssize_t i1, Py_ssize_t i2) noexcept {
    PySequenceMethods* m;
    PyMappingMethods* mp;

    if (!s)
        return null_error();

    m = s->cls->tp_as_sequence;
    if (m && m->sq_slice) {
        if (i1 < 0 || i2 < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(s);
                if (l < 0)
                    return NULL;
                if (i1 < 0)
                    i1 += l;
                if (i2 < 0)
                    i2 += l;
            }
        }
        return m->sq_slice(s, i1, i2);
    } else if ((mp = s->cls->tp_as_mapping) && mp->mp_subscript) {
        PyObject* res;
        PyObject* slice = _PySlice_FromIndices(i1, i2);
        if (!slice)
            return NULL;
        res = mp->mp_subscript(s, slice);
        Py_DECREF(slice);
        return res;
    }

    return type_error("'%.200s' object is unsliceable", s);
}

extern "C" int PySequence_SetItem(PyObject* s, Py_ssize_t i, PyObject* o) noexcept {
    PySequenceMethods* m;

    if (s == NULL) {
        null_error();
        return -1;
    }

    m = s->cls->tp_as_sequence;
    if (m && m->sq_ass_item) {
        if (i < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(s);
                if (l < 0)
                    return -1;
                i += l;
            }
        }
        return m->sq_ass_item(s, i, o);
    }

    type_error("'%.200s' object does not support item assignment", s);
    return -1;
}

extern "C" int PySequence_DelItem(PyObject* s, Py_ssize_t i) noexcept {
    PySequenceMethods* m;

    if (s == NULL) {
        null_error();
        return -1;
    }

    m = s->cls->tp_as_sequence;
    if (m && m->sq_ass_item) {
        if (i < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(s);
                if (l < 0)
                    return -1;
                i += l;
            }
        }
        return m->sq_ass_item(s, i, (PyObject*)NULL);
    }

    type_error("'%.200s' object doesn't support item deletion", s);
    return -1;
}

extern "C" int PySequence_SetSlice(PyObject* s, Py_ssize_t i1, Py_ssize_t i2, PyObject* o) noexcept {
    PySequenceMethods* m;
    PyMappingMethods* mp;

    if (s == NULL) {
        null_error();
        return -1;
    }

    m = s->cls->tp_as_sequence;
    if (m && m->sq_ass_slice) {
        if (i1 < 0 || i2 < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(s);
                if (l < 0)
                    return -1;
                if (i1 < 0)
                    i1 += l;
                if (i2 < 0)
                    i2 += l;
            }
        }
        return m->sq_ass_slice(s, i1, i2, o);
    } else if ((mp = s->cls->tp_as_mapping) && mp->mp_ass_subscript) {
        int res;
        PyObject* slice = _PySlice_FromIndices(i1, i2);
        if (!slice)
            return -1;
        res = mp->mp_ass_subscript(s, slice, o);
        Py_DECREF(slice);
        return res;
    }

    type_error("'%.200s' object doesn't support slice assignment", s);
    return -1;
}

extern "C" int PySequence_DelSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) noexcept {
    PySequenceMethods* m;

    if (o == NULL) {
        null_error();
        return -1;
    }

    m = o->cls->tp_as_sequence;
    if (m && m->sq_ass_slice) {
        if (i1 < 0 || i2 < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(o);
                if (l < 0)
                    return -1;
                if (i1 < 0)
                    i1 += l;
                if (i2 < 0)
                    i2 += l;
            }
        }
        return m->sq_ass_slice(o, i1, i2, (PyObject*)NULL);
    }
    type_error("'%.200s' object doesn't support slice deletion", o);
    return -1;
}

extern "C" Py_ssize_t PySequence_Count(PyObject* o, PyObject* value) noexcept {
    return _PySequence_IterSearch(o, value, PY_ITERSEARCH_COUNT);
}

extern "C" Py_ssize_t PySequence_Index(PyObject* o, PyObject* value) noexcept {
    return _PySequence_IterSearch(o, value, PY_ITERSEARCH_INDEX);
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
    PyObject* v;

    v = PyMapping_GetItemString(o, key);
    if (v) {
        Py_DECREF(v);
        return 1;
    }
    PyErr_Clear();
    return 0;
}

extern "C" int PyMapping_HasKey(PyObject* o, PyObject* key) noexcept {
    PyObject* v;

    v = PyObject_GetItem(o, key);
    if (v) {
        Py_DECREF(v);
        return 1;
    }
    PyErr_Clear();
    return 0;
}

extern "C" PyObject* PyMapping_GetItemString(PyObject* o, const char* key) noexcept {
    PyObject* okey, *r;

    if (key == NULL)
        return null_error();

    okey = PyString_FromString(key);
    if (okey == NULL)
        return NULL;
    r = PyObject_GetItem(o, okey);
    Py_DECREF(okey);
    return r;
}

extern "C" int PyMapping_SetItemString(PyObject* o, const char* key, PyObject* value) noexcept {
    PyObject* okey;
    int r;

    if (key == NULL) {
        null_error();
        return -1;
    }

    okey = PyString_FromString(key);
    if (okey == NULL)
        return -1;
    r = PyObject_SetItem(o, okey, value);
    Py_DECREF(okey);
    return r;
}

extern "C" int PyNumber_Check(PyObject* obj) noexcept {
    assert(obj && obj->cls);

    // Our check, since we don't currently fill in tp_as_number:
    if (PyInt_Check(obj) || PyLong_Check(obj) || PyFloat_Check(obj) || PyComplex_Check(obj))
        return true;

    // The CPython check:
    return obj->cls->tp_as_number && (obj->cls->tp_as_number->nb_int || obj->cls->tp_as_number->nb_float);
}

extern "C" PyObject* PyNumber_Add(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Add);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Subtract(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Sub);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Multiply(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Mult);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Divide(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Div);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_FloorDivide(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::FloorDiv);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_TrueDivide(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::TrueDiv);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Remainder(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Mod);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Divmod(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::DivMod);
    } catch (ExcInfo e) {
        e.clear();
        fatalOrError(PyExc_NotImplementedError, "unimplemented");
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Power(PyObject* v, PyObject* w, PyObject* z) noexcept {
    return ternary_op(v, w, z, NB_SLOT(nb_power), "** or pow()");
}

extern "C" PyObject* PyNumber_Negative(PyObject* o) noexcept {
    PyNumberMethods* m;

    if (o == NULL)
        return null_error();
    m = o->cls->tp_as_number;
    if (m && m->nb_negative)
        return (*m->nb_negative)(o);

    return type_error("bad operand type for unary -: '%.200s'", o);
}

extern "C" PyObject* PyNumber_Positive(PyObject* o) noexcept {
    PyNumberMethods* m;

    if (o == NULL)
        return null_error();
    m = o->cls->tp_as_number;
    if (m && m->nb_positive)
        return (*m->nb_positive)(o);

    return type_error("bad operand type for unary +: '%.200s'", o);
}

extern "C" PyObject* PyNumber_Absolute(PyObject* o) noexcept {
    PyNumberMethods* m;

    if (o == NULL)
        return null_error();
    m = o->cls->tp_as_number;
    if (m && m->nb_absolute)
        return m->nb_absolute(o);

    return type_error("bad operand type for abs(): '%.200s'", o);
}

extern "C" PyObject* PyNumber_Invert(PyObject* o) noexcept {
    try {
        return unaryop(o, AST_TYPE::Invert);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Lshift(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::LShift);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Rshift(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::RShift);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_And(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::BitAnd);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Xor(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::BitXor);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_Or(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::BitOr);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return nullptr;
    }
}

extern "C" PyObject* PyNumber_InPlaceAdd(PyObject* v, PyObject* w) noexcept {
    PyObject* result = binary_iop1(v, w, NB_SLOT(nb_inplace_add), NB_SLOT(nb_add));
    if (result == Py_NotImplemented) {
        PySequenceMethods* m = v->cls->tp_as_sequence;
        Py_DECREF(result);
        if (m != NULL) {
            binaryfunc f = NULL;
            if (PyType_HasFeature((v)->cls, Py_TPFLAGS_HAVE_INPLACEOPS))
                f = m->sq_inplace_concat;
            if (f == NULL)
                f = m->sq_concat;
            if (f != NULL)
                return (*f)(v, w);
        }
        result = binop_type_error(v, w, "+=");
    }
    return result;
}

extern "C" PyObject* PyNumber_InPlaceSubtract(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_subtract), NB_SLOT(nb_subtract), "-=");
}

static PyObject* sequence_repeat(ssizeargfunc repeatfunc, PyObject* seq, PyObject* n) {
    Py_ssize_t count;
    if (PyIndex_Check(n)) {
        count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
        if (count == -1 && PyErr_Occurred())
            return NULL;
    } else {
        return type_error("can't multiply sequence by "
                          "non-int of type '%.200s'",
                          n);
    }
    return (*repeatfunc)(seq, count);
}

extern "C" PyObject* PyNumber_InPlaceMultiply(PyObject* v, PyObject* w) noexcept {
    PyObject* result = binary_iop1(v, w, NB_SLOT(nb_inplace_multiply), NB_SLOT(nb_multiply));
    if (result == Py_NotImplemented) {
        ssizeargfunc f = NULL;
        PySequenceMethods* mv = v->cls->tp_as_sequence;
        PySequenceMethods* mw = w->cls->tp_as_sequence;
        Py_DECREF(result);
        if (mv != NULL) {
            if (PyType_HasFeature((v)->cls, Py_TPFLAGS_HAVE_INPLACEOPS))
                f = mv->sq_inplace_repeat;
            if (f == NULL)
                f = mv->sq_repeat;
            if (f != NULL)
                return sequence_repeat(f, v, w);
        } else if (mw != NULL) {
            /* Note that the right hand operand should not be
             * mutated in this case so sq_inplace_repeat is not
             * used. */
            if (mw->sq_repeat)
                return sequence_repeat(mw->sq_repeat, w, v);
        }
        result = binop_type_error(v, w, "*=");
    }
    return result;
}

extern "C" PyObject* PyNumber_InPlaceDivide(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_divide), NB_SLOT(nb_divide), "/=");
}

extern "C" PyObject* PyNumber_InPlaceFloorDivide(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_floor_divide), NB_SLOT(nb_floor_divide), "//=");
}

extern "C" PyObject* PyNumber_InPlaceTrueDivide(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_true_divide), NB_SLOT(nb_true_divide), "/=");
}

extern "C" PyObject* PyNumber_InPlaceRemainder(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_remainder), NB_SLOT(nb_remainder), "%=");
}

extern "C" PyObject* PyNumber_InPlacePower(PyObject* v, PyObject* w, PyObject* z) noexcept {
    if (PyType_HasFeature((v)->cls, Py_TPFLAGS_HAVE_INPLACEOPS) && v->cls->tp_as_number
        && v->cls->tp_as_number->nb_inplace_power != NULL) {
        return ternary_op(v, w, z, NB_SLOT(nb_inplace_power), "**=");
    } else {
        return ternary_op(v, w, z, NB_SLOT(nb_power), "**=");
    }
}

extern "C" PyObject* PyNumber_InPlaceLshift(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_lshift), NB_SLOT(nb_lshift), "<<=");
}

extern "C" PyObject* PyNumber_InPlaceRshift(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_rshift), NB_SLOT(nb_rshift), ">>=");
}

extern "C" PyObject* PyNumber_InPlaceAnd(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_and), NB_SLOT(nb_and), "%=");
}

extern "C" PyObject* PyNumber_InPlaceXor(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_xor), NB_SLOT(nb_xor), "^=");
}

extern "C" PyObject* PyNumber_InPlaceOr(PyObject* v, PyObject* w) noexcept {
    return binary_iop(v, w, NB_SLOT(nb_inplace_or), NB_SLOT(nb_or), "|=");
}

extern "C" int PyNumber_Coerce(PyObject** pv, PyObject** pw) noexcept {
    int err = PyNumber_CoerceEx(pv, pw);
    if (err <= 0)
        return err;
    PyErr_SetString(PyExc_TypeError, "number coercion failed");
    return -1;
}

extern "C" int PyNumber_CoerceEx(PyObject** pv, PyObject** pw) noexcept {
    PyObject* v = *pv;
    PyObject* w = *pw;
    int res;

    /* Shortcut only for old-style types */
    if (v->cls == w->cls && !PyType_HasFeature(v->cls, Py_TPFLAGS_CHECKTYPES)) {
        Py_INCREF(v);
        Py_INCREF(w);
        return 0;
    }
    if (v->cls->tp_as_number && v->cls->tp_as_number->nb_coerce) {
        res = (*v->cls->tp_as_number->nb_coerce)(pv, pw);
        if (res <= 0)
            return res;
    }
    if (w->cls->tp_as_number && w->cls->tp_as_number->nb_coerce) {
        res = (*w->cls->tp_as_number->nb_coerce)(pw, pv);
        if (res <= 0)
            return res;
    }
    return 1;
}

extern "C" PyObject* _PyNumber_ConvertIntegralToInt(STOLEN(PyObject*) integral, const char* error_format) noexcept {
    const char* type_name;
    static PyObject* int_name = NULL;
    if (int_name == NULL) {
        int_name = getStaticString("__int__");
        if (int_name == NULL)
            return NULL;
    }

    if (integral && (!PyInt_Check(integral) && !PyLong_Check(integral))) {
        /* Don't go through tp_as_number->nb_int to avoid
           hitting the classic class fallback to __trunc__. */
        PyObject* int_func = PyObject_GetAttr(integral, int_name);
        if (int_func == NULL) {
            PyErr_Clear(); /* Raise a different error. */
            goto non_integral_error;
        }
        Py_DECREF(integral);
        integral = PyEval_CallObject(int_func, NULL);
        Py_DECREF(int_func);
        if (integral && (!PyInt_Check(integral) && !PyLong_Check(integral))) {
            goto non_integral_error;
        }
    }
    return integral;

non_integral_error:
    if (PyInstance_Check(integral)) {
        type_name = static_cast<BoxedInstance*>(integral)->inst_cls->name->data();
    } else {
        type_name = integral->cls->tp_name;
    }
    PyErr_Format(PyExc_TypeError, error_format, type_name);
    Py_DECREF(integral);
    return NULL;
}

/* Add a check for embedded NULL-bytes in the argument. */
static PyObject* int_from_string(const char* s, Py_ssize_t len) noexcept {
    char* end;
    PyObject* x;

    x = PyInt_FromString(s, &end, 10);
    if (x == NULL)
        return NULL;
    if (end != s + len) {
        PyErr_SetString(PyExc_ValueError, "null byte in argument for int()");
        Py_DECREF(x);
        return NULL;
    }
    return x;
}

extern "C" PyObject* PyNumber_Int(PyObject* o) noexcept {
    PyNumberMethods* m;
    const char* buffer;
    Py_ssize_t buffer_len;

    if (o == NULL) {
        PyErr_SetString(PyExc_SystemError, "null argument to internal routing");
        return NULL;
    }
    if (PyInt_CheckExact(o)) {
        Py_INCREF(o);
        return o;
    }
    m = o->cls->tp_as_number;
    if (m && m->nb_int) { /* This should include subclasses of int */
        /* Classic classes always take this branch. */
        PyObject* res = m->nb_int(o);
        if (res && (!PyInt_Check(res) && !PyLong_Check(res))) {
            PyErr_Format(PyExc_TypeError, "__int__ returned non-int (type %.200s)", res->cls->tp_name);
            Py_DECREF(res);
            return NULL;
        }
        return res;
    }
    if (PyInt_Check(o)) { /* A int subclass without nb_int */
        BoxedInt* io = (BoxedInt*)o;
        return PyInt_FromLong(io->n);
    }

    // Pyston change: this should be an optimization
    // PyObject* trunc_func = PyObject_GetAttrString(o, "__trunc__");
    static BoxedString* trunc_str = getStaticString("__trunc__");
    PyObject* trunc_func = getattrInternal<ExceptionStyle::CAPI>(o, trunc_str);

    if (trunc_func) {
        PyObject* truncated = PyEval_CallObject(trunc_func, NULL);
        Py_DECREF(trunc_func);
        /* __trunc__ is specified to return an Integral type, but
       int() needs to return an int. */

        return _PyNumber_ConvertIntegralToInt(truncated, "__trunc__ returned non-Integral (type %.200s)");
    }
    PyErr_Clear(); /* It's not an error if  o.__trunc__ doesn't exist. */

    if (PyString_Check(o))
        return int_from_string(PyString_AS_STRING(o), PyString_GET_SIZE(o));
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(o))
        return PyInt_FromUnicode(PyUnicode_AS_UNICODE(o), PyUnicode_GET_SIZE(o), 10);
#endif
    if (!PyObject_AsCharBuffer(o, &buffer, &buffer_len))
        return int_from_string(buffer, buffer_len);

    return type_error("int() argument must be a string or a "
                      "number, not '%.200s'",
                      o);
}

/* Add a check for embedded NULL-bytes in the argument. */
static PyObject* long_from_string(const char* s, Py_ssize_t len) noexcept {
    char* end;
    PyObject* x;

    x = PyLong_FromString(s, &end, 10);
    if (x == NULL)
        return NULL;
    if (end != s + len) {
        PyErr_SetString(PyExc_ValueError, "null byte in argument for long()");
        Py_DECREF(x);
        return NULL;
    }
    return x;
}

extern "C" PyObject* PyNumber_Long(PyObject* o) noexcept {
    PyNumberMethods* m;
    static PyObject* trunc_name = NULL;
    PyObject* trunc_func;
    const char* buffer;
    Py_ssize_t buffer_len;

    if (trunc_name == NULL) {
        trunc_name = getStaticString("__trunc__");
        if (trunc_name == NULL)
            return NULL;
    }

    if (o == NULL)
        return null_error();
    m = o->cls->tp_as_number;
    if (m && m->nb_long) { /* This should include subclasses of long */
        /* Classic classes always take this branch. */
        PyObject* res = m->nb_long(o);
        if (res == NULL)
            return NULL;
        if (PyInt_Check(res)) {
            long value = PyInt_AS_LONG(res);
            Py_DECREF(res);
            return PyLong_FromLong(value);
        } else if (!PyLong_Check(res)) {
            PyErr_Format(PyExc_TypeError, "__long__ returned non-long (type %.200s)", res->cls->tp_name);
            Py_DECREF(res);
            return NULL;
        }
        return res;
    }
    if (PyLong_Check(o)) { /* A long subclass without nb_long */
        return _PyLong_Copy((PyLongObject*)o);
    }
    trunc_func = PyObject_GetAttr(o, trunc_name);
    if (trunc_func) {
        PyObject* truncated = PyEval_CallObject(trunc_func, NULL);
        PyObject* int_instance;
        Py_DECREF(trunc_func);
        /* __trunc__ is specified to return an Integral type,
           but long() needs to return a long. */
        int_instance = _PyNumber_ConvertIntegralToInt(truncated, "__trunc__ returned non-Integral (type %.200s)");
        if (int_instance && PyInt_Check(int_instance)) {
            /* Make sure that long() returns a long instance. */
            long value = PyInt_AS_LONG(int_instance);
            Py_DECREF(int_instance);
            return PyLong_FromLong(value);
        }
        return int_instance;
    }
    PyErr_Clear(); /* It's not an error if  o.__trunc__ doesn't exist. */

    if (PyString_Check(o))
        /* need to do extra error checking that PyLong_FromString()
         * doesn't do.  In particular long('9.5') must raise an
         * exception, not truncate the float.
         */
        return long_from_string(PyString_AS_STRING(o), PyString_GET_SIZE(o));
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(o))
        /* The above check is done in PyLong_FromUnicode(). */
        return PyLong_FromUnicode(PyUnicode_AS_UNICODE(o), PyUnicode_GET_SIZE(o), 10);
#endif
    if (!PyObject_AsCharBuffer(o, &buffer, &buffer_len))
        return long_from_string(buffer, buffer_len);

    return type_error("long() argument must be a string or a "
                      "number, not '%.200s'",
                      o);
}

extern "C" PyObject* PyNumber_Float(PyObject* o) noexcept {
    if (o == NULL)
        return null_error();

    if (o->cls == float_cls) {
        Py_INCREF(o);
        return o;
    }

    PyNumberMethods* m;
    m = o->cls->tp_as_number;
    if (m && m->nb_float) { /* This should include subclasses of float */
        PyObject* res = m->nb_float(o);
        if (res && !PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError, "__float__ returned non-float (type %.200s)", res->cls->tp_name);
            Py_DECREF(res);
            return NULL;
        }
        return res;
    }

    if (PyInt_Check(o))
        return boxFloat(((BoxedInt*)o)->n);
    else if (PyLong_Check(o)) {
        double result = PyLong_AsDouble(o);
        if (result == -1.0 && PyErr_Occurred())
            return NULL;
        return boxFloat(result);
    }

    return PyFloat_FromString(o, NULL);
}

extern "C" PyObject* PyNumber_Index(PyObject* o) noexcept {
    PyObject* result = NULL;
    if (o == NULL)
        return null_error();
    if (PyInt_Check(o) || PyLong_Check(o)) {
        Py_INCREF(o);
        return o;
    }

    if (PyIndex_Check(o)) {
        result = o->cls->tp_as_number->nb_index(o);
        if (result && !PyInt_Check(result) && !PyLong_Check(result)) {
            PyErr_Format(PyExc_TypeError, "__index__ returned non-(int,long) "
                                          "(type %.200s)",
                         result->cls->tp_name);
            Py_DECREF(result);
            return NULL;
        }
    } else {
        PyErr_Format(PyExc_TypeError, "'%.200s' object cannot be interpreted "
                                      "as an index",
                     o->cls->tp_name);
    }
    return result;
}

extern "C" PyObject* PyNumber_ToBase(PyObject* n, int base) noexcept {
    PyObject* res = NULL;
    PyObject* index = PyNumber_Index(n);

    if (!index)
        return NULL;
    if (PyLong_Check(index))
        res = _PyLong_Format(index, base, 0, 1);
    else if (PyInt_Check(index))
        res = _PyInt_Format((PyIntObject*)index, base, 1);
    else
        /* It should not be possible to get here, as
           PyNumber_Index already has a check for the same
           condition */
        PyErr_SetString(PyExc_ValueError, "PyNumber_ToBase: index not int or long");
    Py_DECREF(index);
    return res;
}

extern "C" Py_ssize_t PyNumber_AsSsize_t(PyObject* item, PyObject* err) noexcept {
    Py_ssize_t result;
    PyObject* runerr;
    PyObject* value = PyNumber_Index(item);
    if (value == NULL)
        return -1;

    /* We're done if PyInt_AsSsize_t() returns without error. */
    result = PyInt_AsSsize_t(value);
    if (result != -1 || !(runerr = PyErr_Occurred()))
        goto finish;

    /* Error handling code -- only manage OverflowError differently */
    if (!PyErr_GivenExceptionMatches(runerr, PyExc_OverflowError))
        goto finish;

    PyErr_Clear();
    /* If no error-handling desired then the default clipping
       is sufficient.
     */
    if (!err) {
        assert(PyLong_Check(value));
        /* Whether or not it is less than or equal to
           zero is determined by the sign of ob_size
        */
        if (_PyLong_Sign(value) < 0)
            result = PY_SSIZE_T_MIN;
        else
            result = PY_SSIZE_T_MAX;
    } else {
        /* Otherwise replace the error with caller's error object. */
        PyErr_Format(err, "cannot fit '%.200s' into an index-sized integer", item->cls->tp_name);
    }

finish:
    Py_DECREF(value);
    return result;
}
}
