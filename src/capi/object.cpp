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

extern "C" PyObject* PyObject_Unicode(PyObject* v) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* _PyObject_Str(PyObject* v) noexcept {
    if (v == NULL)
        return boxStrConstant("<NULL>");

    if (v->cls == str_cls)
        return v;

    try {
        return str(v);
    } catch (Box* b) {
        PyErr_SetObject(b->cls, b);
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
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_GenericSetAttr(PyObject* obj, PyObject* name, PyObject* value) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_GetAttrString(PyObject* o, const char* attr) noexcept {
    // TODO do something like this?  not sure if this is safe; will people expect that calling into a known function
    // won't end up doing a GIL check?
    // threading::GLDemoteRegion _gil_demote;

    try {
        return getattr(o, attr);
    } catch (Box* b) {
        PyErr_SetObject(b->cls, b);
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
}
