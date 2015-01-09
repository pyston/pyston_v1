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

extern "C" PyObject* _PyObject_CallFunction_SizeT(PyObject* callable, char* format, ...) noexcept {
    Py_FatalError("unimplemented");
}
}
