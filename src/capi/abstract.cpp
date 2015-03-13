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
        // Don't support PyErr_SetString yet:
        assert(0);
        // PyErr_SetString(PyExc_BufferError, "Object is not writable.");
        // return -1;
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
        args = Py_VaBuildValue(format, va);
        va_end(va);
    } else
        args = PyTuple_New(0);

    retval = call_function_tail(func, args);

exit:
    /* args gets consumed in call_function_tail */
    Py_XDECREF(func);

    return retval;
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

extern "C" int PyNumber_Check(PyObject* obj) noexcept {
    assert(obj && obj->cls);

    // Our check, since we don't currently fill in tp_as_number:
    if (isSubclass(obj->cls, int_cls) || isSubclass(obj->cls, long_cls))
        return true;

    // The CPython check:
    return obj->cls->tp_as_number && (obj->cls->tp_as_number->nb_int || obj->cls->tp_as_number->nb_float);
}

extern "C" PyObject* PyNumber_Add(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Add);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyNumber_Subtract(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Sub);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Multiply(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Mult);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Divide(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Div);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_FloorDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_TrueDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Remainder(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::Mod);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Divmod(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::DivMod);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Power(PyObject*, PyObject*, PyObject* o3) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Negative(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Positive(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Absolute(PyObject* o) noexcept {
    try {
        return abs_(o);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Invert(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Lshift(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Rshift(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::RShift);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_And(PyObject* lhs, PyObject* rhs) noexcept {
    try {
        return binop(lhs, rhs, AST_TYPE::BitAnd);
    } catch (ExcInfo e) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Xor(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Or(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceAdd(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceSubtract(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceMultiply(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceFloorDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceTrueDivide(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceRemainder(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlacePower(PyObject*, PyObject*, PyObject* o3) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceLshift(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceRshift(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceAnd(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceXor(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceOr(PyObject*, PyObject*) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyNumber_Coerce(PyObject**, PyObject**) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyNumber_CoerceEx(PyObject**, PyObject**) noexcept {
    Py_FatalError("unimplemented");
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

    Py_FatalError("unimplemented __trunc__ and string -> int conversion");
// the remainder of PyNumber_Int deals with __trunc__ usage, and converting from unicode/string to int
#if 0
    PyObject* trunc_func = getattr(o, "__trunc__");
    if (trunc_func) {
      PyObject *truncated = PyEval_CallObject(trunc_func, NULL);
      Py_DECREF(trunc_func);
      /* __trunc__ is specified to return an Integral type, but
     int() needs to return an int. */
      return _PyNumber_ConvertIntegralToInt(
                      truncated,
                      "__trunc__ returned non-Integral (type %.200s)");
    }
    PyErr_Clear();  /* It's not an error if  o.__trunc__ doesn't exist. */

    if (PyString_Check(o))
      return int_from_string(PyString_AS_STRING(o),
                 PyString_GET_SIZE(o));
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(o))
      return PyInt_FromUnicode(PyUnicode_AS_UNICODE(o),
                   PyUnicode_GET_SIZE(o),
                   10);
#endif
    if (!PyObject_AsCharBuffer(o, &buffer, &buffer_len))
      return int_from_string((char*)buffer, buffer_len);

    return type_error("int() argument must be a string or a "
              "number, not '%.200s'", o);
#endif
}

extern "C" PyObject* PyNumber_Long(PyObject* o) noexcept {
    // This method should do quite a bit more, including checking tp_as_number->nb_long or calling __trunc__

    if (o->cls == long_cls)
        return o;

    if (o->cls == float_cls)
        return PyLong_FromDouble(PyFloat_AsDouble(o));

    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Float(PyObject* o) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Index(PyObject* o) noexcept {
    PyObject* result = NULL;
    if (o == NULL)
        return null_error();
    if (PyInt_Check(o) || PyLong_Check(o)) {
        return o;
    }

    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_ToBase(PyObject* n, int base) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyNumber_AsSsize_t(PyObject* o, PyObject* exc) noexcept {

    if (isSubclass(o->cls, int_cls)) {
        int64_t n = static_cast<BoxedInt*>(o)->n;
        static_assert(sizeof(n) == sizeof(Py_ssize_t), "");
        return n;
    } else if (isSubclass(o->cls, long_cls)) {
        return PyLong_AsSsize_t(o);
    }

    Py_FatalError("unimplemented");
}
}
