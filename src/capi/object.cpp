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
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" {
_Py_HashSecret_t _Py_HashSecret;
}

extern "C" PyObject* PyObject_Unicode(PyObject* v) noexcept {
    PyObject* res;
    PyObject* func;
    PyObject* str;
    int unicode_method_found = 0;
    static PyObject* unicodestr = NULL;

    if (v == NULL) {
        res = PyString_FromString("<NULL>");
        if (res == NULL)
            return NULL;
        str = PyUnicode_FromEncodedObject(res, NULL, "strict");
        Py_DECREF(res);
        return str;
    } else if (PyUnicode_CheckExact(v)) {
        Py_INCREF(v);
        return v;
    }

    if (PyInstance_Check(v)) {
        /* We're an instance of a classic class */
        /* Try __unicode__ from the instance -- alas we have no type */
        if (!unicodestr) {
            unicodestr = boxStrConstant("__unicode__");
            gc::registerPermanentRoot(unicodestr);
            if (!unicodestr)
                return NULL;
        }
        func = PyObject_GetAttr(v, unicodestr);
        if (func != NULL) {
            unicode_method_found = 1;
            res = PyObject_CallFunctionObjArgs(func, NULL);
            Py_DECREF(func);
        } else {
            PyErr_Clear();
        }
    } else {
        /* Not a classic class instance, try __unicode__. */
        func = _PyObject_LookupSpecial(v, "__unicode__", &unicodestr);
        if (func != NULL) {
            unicode_method_found = 1;
            res = PyObject_CallFunctionObjArgs(func, NULL);
            Py_DECREF(func);
        } else if (PyErr_Occurred())
            return NULL;
    }

    /* Didn't find __unicode__ */
    if (!unicode_method_found) {
        if (PyUnicode_Check(v)) {
            /* For a Unicode subtype that's didn't overwrite __unicode__,
               return a true Unicode object with the same data. */
            return PyUnicode_FromUnicode(PyUnicode_AS_UNICODE(v), PyUnicode_GET_SIZE(v));
        }
        if (PyString_CheckExact(v)) {
            Py_INCREF(v);
            res = v;
        } else {
            if (Py_TYPE(v)->tp_str != NULL)
                res = (*Py_TYPE(v)->tp_str)(v);
            else
                res = PyObject_Repr(v);
        }
    }

    if (res == NULL)
        return NULL;
    if (!PyUnicode_Check(res)) {
        str = PyUnicode_FromEncodedObject(res, NULL, "strict");
        Py_DECREF(res);
        res = str;
    }
    return res;
}

extern "C" PyObject* _PyObject_Str(PyObject* v) noexcept {
    if (v == NULL)
        return boxStrConstant("<NULL>");

    if (v->cls == str_cls)
        return v;

    try {
        return str(v);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyObject_Str(PyObject* v) noexcept {
    PyObject* res = _PyObject_Str(v);
    if (res == NULL)
        return NULL;
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(res)) {
        PyObject* str;
        str = PyUnicode_AsEncodedString(res, NULL, NULL);
        Py_DECREF(res);
        if (str)
            res = str;
        else
            return NULL;
    }
#endif
    assert(PyString_Check(res));
    return res;
}

extern "C" PyObject* PyObject_SelfIter(PyObject* obj) noexcept {
    return obj;
}

extern "C" int PyObject_GenericSetAttr(PyObject* obj, PyObject* name, PyObject* value) noexcept {
    try {
        setattrGeneric(obj, static_cast<BoxedString*>(name)->s.c_str(), value, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyObject_SetAttr(PyObject* v, PyObject* name, PyObject* value) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_SetAttrString(PyObject* v, const char* name, PyObject* w) noexcept {
    try {
        setattr(v, name, w);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" PyObject* PyObject_GetAttrString(PyObject* o, const char* attr) noexcept {
    // TODO do something like this?  not sure if this is safe; will people expect that calling into a known function
    // won't end up doing a GIL check?
    // threading::GLDemoteRegion _gil_demote;

    try {
        return getattr(o, attr);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyObject_HasAttrString(PyObject* v, const char* name) noexcept {
    PyObject* res = PyObject_GetAttrString(v, name);
    if (res != NULL) {
        Py_DECREF(res);
        return 1;
    }
    PyErr_Clear();
    return 0;
}

extern "C" int PyObject_AsWriteBuffer(PyObject* obj, void** buffer, Py_ssize_t* buffer_len) noexcept {
    Py_FatalError("unimplemented");
}

/* Return -1 if error; 1 if v op w; 0 if not (v op w). */
extern "C" int PyObject_RichCompareBool(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;
    int ok;

    /* Quick result when objects are the same.
       Guarantees that identity implies equality. */
    if (v == w) {
        if (op == Py_EQ)
            return 1;
        else if (op == Py_NE)
            return 0;
    }

    res = PyObject_RichCompare(v, w, op);
    if (res == NULL)
        return -1;
    if (PyBool_Check(res))
        ok = (res == Py_True);
    else
        ok = PyObject_IsTrue(res);
    Py_DECREF(res);
    return ok;
}

// I'm not sure how we can support this one:
extern "C" PyObject** _PyObject_GetDictPtr(PyObject* obj) noexcept {
    Py_FatalError("unimplemented");
}

/* These methods are used to control infinite recursion in repr, str, print,
   etc.  Container objects that may recursively contain themselves,
   e.g. builtin dictionaries and lists, should used Py_ReprEnter() and
   Py_ReprLeave() to avoid infinite recursion.

   Py_ReprEnter() returns 0 the first time it is called for a particular
   object and 1 every time thereafter.  It returns -1 if an exception
   occurred.  Py_ReprLeave() has no return value.

   See dictobject.c and listobject.c for examples of use.
*/

#define KEY "Py_Repr"

extern "C" int Py_ReprEnter(PyObject* obj) noexcept {
    PyObject* dict;
    PyObject* list;
    Py_ssize_t i;

    dict = PyThreadState_GetDict();
    if (dict == NULL)
        return 0;
    list = PyDict_GetItemString(dict, KEY);
    if (list == NULL) {
        list = PyList_New(0);
        if (list == NULL)
            return -1;
        if (PyDict_SetItemString(dict, KEY, list) < 0)
            return -1;
        Py_DECREF(list);
    }
    i = PyList_GET_SIZE(list);
    while (--i >= 0) {
        if (PyList_GET_ITEM(list, i) == obj)
            return 1;
    }
    PyList_Append(list, obj);
    return 0;
}

extern "C" void Py_ReprLeave(PyObject* obj) noexcept {
    PyObject* dict;
    PyObject* list;
    Py_ssize_t i;

    dict = PyThreadState_GetDict();
    if (dict == NULL)
        return;
    list = PyDict_GetItemString(dict, KEY);
    if (list == NULL || !PyList_Check(list))
        return;
    i = PyList_GET_SIZE(list);
    /* Count backwards because we always expect obj to be list[-1] */
    while (--i >= 0) {
        if (PyList_GET_ITEM(list, i) == obj) {
            PyList_SetSlice(list, i, i + 1, NULL);
            break;
        }
    }
}

extern "C" int PyObject_Compare(PyObject* o1, PyObject* o2) noexcept {
    Py_FatalError("unimplemented");
}
}
