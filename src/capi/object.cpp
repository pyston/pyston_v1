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

static int merge_class_dict(PyObject* dict, PyObject* aclass) noexcept {
    PyObject* classdict;
    PyObject* bases;

    assert(PyDict_Check(dict));
    assert(aclass);

    /* Merge in the type's dict (if any). */
    classdict = PyObject_GetAttrString(aclass, "__dict__");
    if (classdict == NULL)
        PyErr_Clear();
    else {
        int status = PyDict_Update(dict, classdict);
        Py_DECREF(classdict);
        if (status < 0)
            return -1;
    }

    /* Recursively merge in the base types' (if any) dicts. */
    bases = PyObject_GetAttrString(aclass, "__bases__");
    if (bases == NULL)
        PyErr_Clear();
    else {
        /* We have no guarantee that bases is a real tuple */
        Py_ssize_t i, n;
        n = PySequence_Size(bases); /* This better be right */
        if (n < 0)
            PyErr_Clear();
        else {
            for (i = 0; i < n; i++) {
                int status;
                PyObject* base = PySequence_GetItem(bases, i);
                if (base == NULL) {
                    Py_DECREF(bases);
                    return -1;
                }
                status = merge_class_dict(dict, base);
                Py_DECREF(base);
                if (status < 0) {
                    Py_DECREF(bases);
                    return -1;
                }
            }
        }
        Py_DECREF(bases);
    }
    return 0;
}

static int merge_list_attr(PyObject* dict, PyObject* obj, const char* attrname) {
    PyObject* list;
    int result = 0;

    assert(PyDict_Check(dict));
    assert(obj);
    assert(attrname);

    list = PyObject_GetAttrString(obj, attrname);
    if (list == NULL)
        PyErr_Clear();

    else if (PyList_Check(list)) {
        int i;
        for (i = 0; i < PyList_GET_SIZE(list); ++i) {
            PyObject* item = PyList_GET_ITEM(list, i);
            if (PyString_Check(item)) {
                result = PyDict_SetItem(dict, item, Py_None);
                if (result < 0)
                    break;
            }
        }
        if (Py_Py3kWarningFlag && (strcmp(attrname, "__members__") == 0 || strcmp(attrname, "__methods__") == 0)) {
            if (PyErr_WarnEx(PyExc_DeprecationWarning, "__members__ and __methods__ not "
                                                       "supported in 3.x",
                             1) < 0) {
                Py_XDECREF(list);
                return -1;
            }
        }
    }

    Py_XDECREF(list);
    return result;
}

/* Helper for PyObject_Dir without arguments: returns the local scope. */
static PyObject* _dir_locals(void) {
    PyObject* names;
    PyObject* locals = PyEval_GetLocals();

    if (locals == NULL) {
        PyErr_SetString(PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    names = PyMapping_Keys(locals);
    if (!names)
        return NULL;
    if (!PyList_Check(names)) {
        PyErr_Format(PyExc_TypeError, "dir(): expected keys() of locals to be a list, "
                                      "not '%.200s'",
                     Py_TYPE(names)->tp_name);
        Py_DECREF(names);
        return NULL;
    }
    /* the locals don't need to be DECREF'd */
    return names;
}

/* Helper for PyObject_Dir of type objects: returns __dict__ and __bases__.
   We deliberately don't suck up its __class__, as methods belonging to the
   metaclass would probably be more confusing than helpful.
*/
static PyObject* _specialized_dir_type(PyObject* obj) noexcept {
    PyObject* result = NULL;
    PyObject* dict = PyDict_New();

    if (dict != NULL && merge_class_dict(dict, obj) == 0)
        result = PyDict_Keys(dict);

    Py_XDECREF(dict);
    return result;
}

/* Helper for PyObject_Dir of module objects: returns the module's __dict__. */
static PyObject* _specialized_dir_module(PyObject* obj) noexcept {
    PyObject* result = NULL;
    PyObject* dict = PyObject_GetAttrString(obj, "__dict__");

    if (dict != NULL) {
        if (PyDict_Check(dict))
            result = PyDict_Keys(dict);
        else if (dict->cls == attrwrapper_cls)
            result = attrwrapperKeys(dict);
        else {
            char* name = PyModule_GetName(obj);
            if (name)
                PyErr_Format(PyExc_TypeError, "%.200s.__dict__ is not a dictionary", name);
        }
    }

    Py_XDECREF(dict);
    return result;
}

/* Helper for PyObject_Dir of generic objects: returns __dict__, __class__,
   and recursively up the __class__.__bases__ chain.
*/
static PyObject* _generic_dir(PyObject* obj) noexcept {
    PyObject* result = NULL;
    PyObject* dict = NULL;
    PyObject* itsclass = NULL;

    /* Get __dict__ (which may or may not be a real dict...) */
    dict = PyObject_GetAttrString(obj, "__dict__");
    if (dict == NULL) {
        PyErr_Clear();
        dict = PyDict_New();
    } else if (dict->cls == attrwrapper_cls) {
        auto new_dict = PyDict_New();
        PyDict_Update(new_dict, dict);
        dict = new_dict;
    } else if (!PyDict_Check(dict)) {
        Py_DECREF(dict);
        dict = PyDict_New();
    } else {
        /* Copy __dict__ to avoid mutating it. */
        PyObject* temp = PyDict_Copy(dict);
        Py_DECREF(dict);
        dict = temp;
    }

    if (dict == NULL)
        goto error;

    /* Merge in __members__ and __methods__ (if any).
     * This is removed in Python 3000. */
    if (merge_list_attr(dict, obj, "__members__") < 0)
        goto error;
    if (merge_list_attr(dict, obj, "__methods__") < 0)
        goto error;

    /* Merge in attrs reachable from its class. */
    itsclass = PyObject_GetAttrString(obj, "__class__");
    if (itsclass == NULL)
        /* XXX(tomer): Perhaps fall back to obj->ob_type if no
                       __class__ exists? */
        PyErr_Clear();
    else {
        if (merge_class_dict(dict, itsclass) != 0)
            goto error;
    }

    result = PyDict_Keys(dict);
/* fall through */
error:
    Py_XDECREF(itsclass);
    Py_XDECREF(dict);
    return result;
}

/* Helper for PyObject_Dir: object introspection.
   This calls one of the above specialized versions if no __dir__ method
   exists. */
static PyObject* _dir_object(PyObject* obj) noexcept {
    PyObject* result = NULL;
    static PyObject* dir_str = NULL;
    PyObject* dirfunc;

    assert(obj);
    if (PyInstance_Check(obj)) {
        dirfunc = PyObject_GetAttrString(obj, "__dir__");
        if (dirfunc == NULL) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError))
                PyErr_Clear();
            else
                return NULL;
        }
    } else {
        dirfunc = _PyObject_LookupSpecial(obj, "__dir__", &dir_str);
        if (PyErr_Occurred())
            return NULL;
    }
    if (dirfunc == NULL) {
        /* use default implementation */
        if (PyModule_Check(obj))
            result = _specialized_dir_module(obj);
        else if (PyType_Check(obj) || PyClass_Check(obj))
            result = _specialized_dir_type(obj);
        else
            result = _generic_dir(obj);
    } else {
        /* use __dir__ */
        result = PyObject_CallFunctionObjArgs(dirfunc, NULL);
        Py_DECREF(dirfunc);
        if (result == NULL)
            return NULL;

        /* result must be a list */
        /* XXX(gbrandl): could also check if all items are strings */
        if (!PyList_Check(result)) {
            PyErr_Format(PyExc_TypeError, "__dir__() must return a list, not %.200s", Py_TYPE(result)->tp_name);
            Py_DECREF(result);
            result = NULL;
        }
    }

    return result;
}

/* Implementation of dir() -- if obj is NULL, returns the names in the current
   (local) scope.  Otherwise, performs introspection of the object: returns a
   sorted list of attribute names (supposedly) accessible from the object
*/
extern "C" PyObject* PyObject_Dir(PyObject* obj) noexcept {
    PyObject* result;

    if (obj == NULL)
        /* no object -- introspect the locals */
        result = _dir_locals();
    else
        /* object -- introspect the object */
        result = _dir_object(obj);

    assert(result == NULL || PyList_Check(result));

    if (result != NULL && PyList_Sort(result) != 0) {
        /* sorting the list failed */
        Py_DECREF(result);
        result = NULL;
    }

    return result;
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
        if (value == NULL)
            delattrGeneric(obj, std::string(static_cast<BoxedString*>(name)->s), NULL);
        else
            setattrGeneric(obj, std::string(static_cast<BoxedString*>(name)->s), value, NULL);
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
