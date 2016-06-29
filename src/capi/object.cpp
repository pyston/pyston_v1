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
        Py_DECREF(dict);
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
            unicodestr = getStaticString("__unicode__");
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

extern "C" PyObject* PyObject_Repr(PyObject* v) noexcept {
    if (PyErr_CheckSignals())
        return NULL;
#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        PyErr_SetString(PyExc_MemoryError, "stack overflow");
        return NULL;
    }
#endif
    if (v == NULL)
        return PyString_FromString("<NULL>");
    else if (Py_TYPE(v)->tp_repr == NULL)
        return PyString_FromFormat("<%s object at %p>", Py_TYPE(v)->tp_name, v);
    else {
        PyObject* res;
        res = (*Py_TYPE(v)->tp_repr)(v);
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
        if (!PyString_Check(res)) {
            PyErr_Format(PyExc_TypeError, "__repr__ returned non-string (type %.200s)", Py_TYPE(res)->tp_name);
            Py_DECREF(res);
            return NULL;
        }
        return res;
    }
}

extern "C" PyObject* _PyObject_Str(PyObject* v) noexcept {
    PyObject* res;
    int type_ok;
    if (v == NULL)
        return PyString_FromString("<NULL>");
    if (PyString_CheckExact(v)) {
        Py_INCREF(v);
        return v;
    }
#ifdef Py_USING_UNICODE
    if (PyUnicode_CheckExact(v)) {
        Py_INCREF(v);
        return v;
    }
#endif
    if (Py_TYPE(v)->tp_str == NULL)
        return PyObject_Repr(v);

    /* It is possible for a type to have a tp_str representation that loops
       infinitely. */
    if (Py_EnterRecursiveCall(" while getting the str of an object"))
        return NULL;
    res = (*Py_TYPE(v)->tp_str)(v);
    Py_LeaveRecursiveCall();
    if (res == NULL)
        return NULL;
    type_ok = PyString_Check(res);
#ifdef Py_USING_UNICODE
    type_ok = type_ok || PyUnicode_Check(res);
#endif
    if (!type_ok) {
        PyErr_Format(PyExc_TypeError, "__str__ returned non-string (type %.200s)", Py_TYPE(res)->tp_name);
        Py_DECREF(res);
        return NULL;
    }
    return res;
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
    Py_INCREF(obj);
    return obj;
}

extern "C" int PyObject_GenericSetAttr(PyObject* obj, PyObject* name, PyObject* value) noexcept {
    if (!PyString_Check(name)) {
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return -1;
        } else {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return -1;
        }
    } else {
        Py_INCREF(name);
    }
    AUTO_DECREF(name);

    BoxedString* str = static_cast<BoxedString*>(name);
    incref(str);
    internStringMortalInplace(str);
    AUTO_DECREF(str);

    assert(PyString_Check(name));
    try {
        if (value == NULL)
            delattrGeneric(obj, str, NULL);
        else
            setattrGeneric<NOT_REWRITABLE>(obj, str, incref(value), NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyObject_SetAttr(PyObject* obj, PyObject* name, PyObject* value) noexcept {
    if (!PyString_Check(name)) {
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return -1;
        } else {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return -1;
        }
    }

    BoxedString* name_str = static_cast<BoxedString*>(name);
    Py_INCREF(name_str);
    internStringMortalInplace(name_str);
    AUTO_DECREF(name_str);

    assert(PyString_Check(name));
    try {
        if (value == NULL)
            delattr(obj, name_str);
        else
            setattr(obj, name_str, incref(value));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyObject_SetAttrString(PyObject* v, const char* name, PyObject* w) noexcept {
    try {
        setattr(v, autoDecref(internStringMortal(name)), incref(w));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" PyObject* PyObject_GetAttrString(PyObject* o, const char* attr) noexcept {
    try {
        Box* r = getattrInternal<ExceptionStyle::CXX>(o, autoDecref(internStringMortal(attr)));
        if (!r)
            PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name, attr);
        return r;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyObject_HasAttr(PyObject* v, PyObject* name) noexcept {
    PyObject* res = PyObject_GetAttr(v, name);
    if (res != NULL) {
        Py_DECREF(res);
        return 1;
    }
    PyErr_Clear();
    return 0;
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

// I'm not sure how we can support this one:
extern "C" PyObject** _PyObject_GetDictPtr(PyObject* obj) noexcept {
    Py_ssize_t dictoffset;
    PyTypeObject* tp = Py_TYPE(obj);

    if (!tp->instancesHaveHCAttrs()) {
        dictoffset = tp->tp_dictoffset;
        if (dictoffset == 0)
            return NULL;
        if (dictoffset < 0) {
            Py_ssize_t tsize;
            size_t size;

            tsize = ((PyVarObject*)obj)->ob_size;
            if (tsize < 0)
                tsize = -tsize;
            size = _PyObject_VAR_SIZE(tp, tsize);

            dictoffset += (long)size;
            assert(dictoffset > 0);
            assert(dictoffset % SIZEOF_VOID_P == 0);
        }
        return (PyObject**)((char*)obj + dictoffset);
    } else {
        fatalOrError(PyExc_NotImplementedError, "unimplemented for hcattrs");
        return nullptr;
    }
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

/* Helper to warn about deprecated tp_compare return values.  Return:
   -2 for an exception;
   -1 if v <  w;
    0 if v == w;
    1 if v  > w.
   (This function cannot return 2.)
*/
static int adjust_tp_compare(int c) {
    if (PyErr_Occurred()) {
        if (c != -1 && c != -2) {
            PyObject* t, *v, *tb;
            PyErr_Fetch(&t, &v, &tb);
            if (PyErr_Warn(PyExc_RuntimeWarning, "tp_compare didn't return -1 or -2 "
                                                 "for exception") < 0) {
                Py_XDECREF(t);
                Py_XDECREF(v);
                Py_XDECREF(tb);
            } else
                PyErr_Restore(t, v, tb);
        }
        return -2;
    } else if (c < -1 || c > 1) {
        if (PyErr_Warn(PyExc_RuntimeWarning, "tp_compare didn't return -1, 0 or 1") < 0)
            return -2;
        else
            return c < -1 ? -1 : 1;
    } else {
        assert(c >= -1 && c <= 1);
        return c;
    }
}


/* Macro to get the tp_richcompare field of a type if defined */
#define RICHCOMPARE(t) (PyType_HasFeature((t), Py_TPFLAGS_HAVE_RICHCOMPARE) ? (t)->tp_richcompare : NULL)

/* Map rich comparison operators to their swapped version, e.g. LT --> GT */
extern "C" {
int _Py_SwappedOp[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
}

/* Try a genuine rich comparison, returning an object.  Return:
   NULL for exception;
   NotImplemented if this particular rich comparison is not implemented or
     undefined;
   some object not equal to NotImplemented if it is implemented
     (this latter object may not be a Boolean).
*/
static PyObject* try_rich_compare(PyObject* v, PyObject* w, int op) {
    richcmpfunc f;
    PyObject* res;

    if (v->cls != w->cls && PyType_IsSubtype(w->cls, v->cls) && (f = RICHCOMPARE(w->cls)) != NULL) {
        res = (*f)(w, v, _Py_SwappedOp[op]);
        if (res != Py_NotImplemented)
            return res;
        Py_DECREF(res);
    }
    if ((f = RICHCOMPARE(v->cls)) != NULL) {
        res = (*f)(v, w, op);
        if (res != Py_NotImplemented)
            return res;
        Py_DECREF(res);
    }
    if ((f = RICHCOMPARE(w->cls)) != NULL) {
        return (*f)(w, v, _Py_SwappedOp[op]);
    }
    res = Py_NotImplemented;
    Py_INCREF(res);
    return res;
}

/* Try a genuine rich comparison, returning an int.  Return:
   -1 for exception (including the case where try_rich_compare() returns an
      object that's not a Boolean);
    0 if the outcome is false;
    1 if the outcome is true;
    2 if this particular rich comparison is not implemented or undefined.
*/
static int try_rich_compare_bool(PyObject* v, PyObject* w, int op) {
    PyObject* res;
    int ok;

    if (RICHCOMPARE(v->cls) == NULL && RICHCOMPARE(w->cls) == NULL)
        return 2; /* Shortcut, avoid INCREF+DECREF */
    res = try_rich_compare(v, w, op);
    if (res == NULL)
        return -1;
    if (res == Py_NotImplemented) {
        Py_DECREF(res);
        return 2;
    }
    ok = PyObject_IsTrue(res);
    Py_DECREF(res);
    return ok;
}

/* Try rich comparisons to determine a 3-way comparison.  Return:
   -2 for an exception;
   -1 if v  < w;
    0 if v == w;
    1 if v  > w;
    2 if this particular rich comparison is not implemented or undefined.
*/
static int try_rich_to_3way_compare(PyObject* v, PyObject* w) {
    static struct {
        int op;
        int outcome;
    } tries[3] = {
        /* Try this operator, and if it is true, use this outcome: */
        { Py_EQ, 0 },
        { Py_LT, -1 },
        { Py_GT, 1 },
    };
    int i;

    if (RICHCOMPARE(v->cls) == NULL && RICHCOMPARE(w->cls) == NULL)
        return 2; /* Shortcut */

    for (i = 0; i < 3; i++) {
        switch (try_rich_compare_bool(v, w, tries[i].op)) {
            case -1:
                return -2;
            case 1:
                return tries[i].outcome;
        }
    }

    return 2;
}

/* Try a 3-way comparison, returning an int.  Return:
   -2 for an exception;
   -1 if v <  w;
    0 if v == w;
    1 if v  > w;
    2 if this particular 3-way comparison is not implemented or undefined.
*/
static int try_3way_compare(PyObject* v, PyObject* w) {
    int c;
    cmpfunc f;

    /* Comparisons involving instances are given to instance_compare,
       which has the same return conventions as this function. */

    f = v->cls->tp_compare;
    if (PyInstance_Check(v))
        return (*f)(v, w);
    if (PyInstance_Check(w))
        return (*w->cls->tp_compare)(v, w);

    /* If both have the same (non-NULL) tp_compare, use it. */
    if (f != NULL && f == w->cls->tp_compare) {
        c = (*f)(v, w);
        return adjust_tp_compare(c);
    }

    /* If either tp_compare is _PyObject_SlotCompare, that's safe. */
    if (f == _PyObject_SlotCompare || w->cls->tp_compare == _PyObject_SlotCompare)
        return _PyObject_SlotCompare(v, w);

    /* If we're here, v and w,
        a) are not instances;
        b) have different types or a type without tp_compare; and
        c) don't have a user-defined tp_compare.
       tp_compare implementations in C assume that both arguments
       have their type, so we give up if the coercion fails or if
       it yields types which are still incompatible (which can
       happen with a user-defined nb_coerce).
    */
    c = PyNumber_CoerceEx(&v, &w);
    if (c < 0)
        return -2;
    if (c > 0)
        return 2;
    f = v->cls->tp_compare;
    if (f != NULL && f == w->cls->tp_compare) {
        c = (*f)(v, w);
        Py_DECREF(v);
        Py_DECREF(w);
        return adjust_tp_compare(c);
    }

    /* No comparison defined */
    Py_DECREF(v);
    Py_DECREF(w);
    return 2;
}

/* Final fallback 3-way comparison, returning an int.  Return:
   -2 if an error occurred;
   -1 if v <  w;
    0 if v == w;
    1 if v >  w.
*/
/* Pyston change: static*/ int default_3way_compare(PyObject* v, PyObject* w) {
    int c;
    const char* vname, *wname;

    if (v->cls == w->cls) {
        /* When comparing these pointers, they must be cast to
         * integer types (i.e. Py_uintptr_t, our spelling of C9X's
         * uintptr_t).  ANSI specifies that pointer compares other
         * than == and != to non-related structures are undefined.
         */
        Py_uintptr_t vv = (Py_uintptr_t)v;
        Py_uintptr_t ww = (Py_uintptr_t)w;
        return (vv < ww) ? -1 : (vv > ww) ? 1 : 0;
    }

    /* None is smaller than anything */
    if (v == Py_None)
        return -1;
    if (w == Py_None)
        return 1;

    /* different type: compare type names; numbers are smaller */
    if (PyNumber_Check(v))
        vname = "";
    else
        vname = v->cls->tp_name;
    if (PyNumber_Check(w))
        wname = "";
    else
        wname = w->cls->tp_name;
    c = strcmp(vname, wname);
    if (c < 0)
        return -1;
    if (c > 0)
        return 1;
    /* Same type name, or (more likely) incomparable numeric types */
    return ((Py_uintptr_t)(v->cls) < (Py_uintptr_t)(w->cls)) ? -1 : 1;
}

/* Do a 3-way comparison, by hook or by crook.  Return:
   -2 for an exception (but see below);
   -1 if v <  w;
    0 if v == w;
    1 if v >  w;
   BUT: if the object implements a tp_compare function, it returns
   whatever this function returns (whether with an exception or not).
*/
static int do_cmp(PyObject* v, PyObject* w) {
    int c;
    cmpfunc f;

    if (v->cls == w->cls && (f = v->cls->tp_compare) != NULL) {
        c = (*f)(v, w);
        if (PyInstance_Check(v)) {
            /* Instance tp_compare has a different signature.
               But if it returns undefined we fall through. */
            if (c != 2)
                return c;
            /* Else fall through to try_rich_to_3way_compare() */
        } else
            return adjust_tp_compare(c);
    }
    /* We only get here if one of the following is true:
       a) v and w have different types
       b) v and w have the same type, which doesn't have tp_compare
       c) v and w are instances, and either __cmp__ is not defined or
          __cmp__ returns NotImplemented
    */
    c = try_rich_to_3way_compare(v, w);
    if (c < 2)
        return c;
    c = try_3way_compare(v, w);
    if (c < 2)
        return c;
    return default_3way_compare(v, w);
}

/* Compare v to w.  Return
   -1 if v <  w or exception (PyErr_Occurred() true in latter case).
    0 if v == w.
    1 if v > w.
   XXX The docs (C API manual) say the return value is undefined in case
   XXX of error.
*/
extern "C" int PyObject_Compare(PyObject* v, PyObject* w) noexcept {
    int result;

    if (v == NULL || w == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (v == w)
        return 0;
    if (Py_EnterRecursiveCall(" in cmp"))
        return -1;
    result = do_cmp(v, w);
    Py_LeaveRecursiveCall();
    return result < 0 ? -1 : result;
}

/* Return (new reference to) Py_True or Py_False. */
/* Pyston change: static */ PyObject* convert_3way_to_object(int op, int c) noexcept {
    PyObject* result;
    switch (op) {
        case Py_LT:
            c = c < 0;
            break;
        case Py_LE:
            c = c <= 0;
            break;
        case Py_EQ:
            c = c == 0;
            break;
        case Py_NE:
            c = c != 0;
            break;
        case Py_GT:
            c = c > 0;
            break;
        case Py_GE:
            c = c >= 0;
            break;
    }
    result = c ? Py_True : Py_False;
    Py_INCREF(result);
    return result;
}

/* We want a rich comparison but don't have one.  Try a 3-way cmp instead.
   Return
   NULL      if error
   Py_True   if v op w
   Py_False  if not (v op w)
*/
/* static */ PyObject* try_3way_to_rich_compare(PyObject* v, PyObject* w, int op) noexcept {
    int c;

    c = try_3way_compare(v, w);
    if (c >= 2) {

        /* Py3K warning if types are not equal and comparison isn't == or !=  */
        if (Py_Py3kWarningFlag && v->cls != w->cls && op != Py_EQ && op != Py_NE
            && PyErr_WarnEx(PyExc_DeprecationWarning, "comparing unequal types not supported "
                                                      "in 3.x",
                            1) < 0) {
            return NULL;
        }

        c = default_3way_compare(v, w);
    }
    if (c <= -2)
        return NULL;
    return convert_3way_to_object(op, c);
}

/* Do rich comparison on v and w.  Return
   NULL      if error
   Else a new reference to an object other than Py_NotImplemented, usually(?):
   Py_True   if v op w
   Py_False  if not (v op w)
*/
static PyObject* do_richcmp(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;

    res = try_rich_compare(v, w, op);
    if (res != Py_NotImplemented)
        return res;
    Py_DECREF(res);

    return try_3way_to_rich_compare(v, w, op);
}

/* Return:
   NULL for exception;
   some object not equal to NotImplemented if it is implemented
     (this latter object may not be a Boolean).
*/
extern "C" PyObject* PyObject_RichCompare(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* res;

    assert(Py_LT <= op && op <= Py_GE);
    if (Py_EnterRecursiveCall(" in cmp"))
        return NULL;

    /* If the types are equal, and not old-style instances, try to
       get out cheap (don't bother with coercions etc.). */
    if (v->cls == w->cls && !PyInstance_Check(v)) {
        cmpfunc fcmp;
        richcmpfunc frich = RICHCOMPARE(v->cls);
        /* If the type has richcmp, try it first.  try_rich_compare
           tries it two-sided, which is not needed since we've a
           single type only. */
        if (frich != NULL) {
            res = (*frich)(v, w, op);
            if (res != Py_NotImplemented)
                goto Done;
            Py_DECREF(res);
        }
        /* No richcmp, or this particular richmp not implemented.
           Try 3-way cmp. */
        fcmp = v->cls->tp_compare;
        if (fcmp != NULL) {
            int c = (*fcmp)(v, w);
            c = adjust_tp_compare(c);
            if (c == -2) {
                res = NULL;
                goto Done;
            }
            res = convert_3way_to_object(op, c);
            goto Done;
        }
    }

    /* Fast path not taken, or couldn't deliver a useful result. */
    res = do_richcmp(v, w, op);
Done:
    Py_LeaveRecursiveCall();
    return res;
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

#ifdef Py_REF_DEBUG
extern "C" {
Py_ssize_t _Py_RefTotal;
}

extern "C" Py_ssize_t _Py_GetRefTotal(void) noexcept {
    PyObject* o;
    Py_ssize_t total = _Py_RefTotal;
/* ignore the references to the dummy object of the dicts and sets
 *        because they are not reliable and not useful (now that the
 *               hash table code is well-tested) */

// Pyston change: not sure what these are
#if 0
    o = _PyDict_Dummy();
    if (o != NULL)
        total -= o->ob_refcnt;
    o = _PySet_Dummy();
    if (o != NULL)
        total -= o->ob_refcnt;
#endif
    return total;
}
#endif /* Py_REF_DEBUG */

#ifdef Py_REF_DEBUG
/* Log a fatal error; doesn't return. */
extern "C" void _Py_NegativeRefcount(const char* fname, int lineno, PyObject* op) noexcept {
    char buf[300];

    PyOS_snprintf(buf, sizeof(buf), "%s:%i object at %p has negative ref count "
                                    "%" PY_FORMAT_SIZE_T "d.  \033[40mwatch -l *(long*)%p\033[0m",
                  fname, lineno, op, op->ob_refcnt, &op->ob_refcnt);
    Py_FatalError(buf);
}

#endif /* Py_REF_DEBUG */

extern "C" {
int _PyTrash_delete_nesting = 0;
PyObject* _PyTrash_delete_later = NULL;
}

extern "C" void _PyTrash_thread_deposit_object(PyObject* op) noexcept {
    PyThreadState* tstate = PyThreadState_GET();
    assert(PyObject_IS_GC(op));
    assert(_Py_AS_GC(op)->gc.gc_refs == _PyGC_REFS_UNTRACKED);
    assert(op->ob_refcnt == 0);
    _Py_AS_GC(op)->gc.gc_prev = (PyGC_Head*)tstate->trash_delete_later;
    tstate->trash_delete_later = op;
}

extern "C" void _PyTrash_thread_destroy_chain() noexcept {
    PyThreadState* tstate = PyThreadState_GET();
    while (tstate->trash_delete_later) {
        PyObject* op = tstate->trash_delete_later;
        destructor dealloc = Py_TYPE(op)->tp_dealloc;

        tstate->trash_delete_later = (PyObject*)_Py_AS_GC(op)->gc.gc_prev;

        /* Call the deallocator directly.  This used to try to
         * fool Py_DECREF into calling it indirectly, but
         * Py_DECREF was already called on this object, and in
         * assorted non-release builds calling Py_DECREF again ends
         * up distorting allocation statistics.
         */
        assert(op->ob_refcnt == 0);
        ++tstate->trash_delete_nesting;
        (*dealloc)(op);
        --tstate->trash_delete_nesting;
    }
}

#ifdef Py_TRACE_REFS
/* Head of circular doubly-linked list of all objects.  These are linked
 * together via the _ob_prev and _ob_next members of a PyObject, which
 * exist only in a Py_TRACE_REFS build.
 */
extern "C" {
// static PyObject refchain = { &refchain, &refchain };
PyObject refchain(Box::createRefchain());
}

/* Insert op at the front of the list of all objects.  If force is true,
 * op is added even if _ob_prev and _ob_next are non-NULL already.  If
 * force is false amd _ob_prev or _ob_next are non-NULL, do nothing.
 * force should be true if and only if op points to freshly allocated,
 * uninitialized memory, or you've unlinked op from the list and are
 * relinking it into the front.
 * Note that objects are normally added to the list via _Py_NewReference,
 * which is called by PyObject_Init.  Not all objects are initialized that
 * way, though; exceptions include statically allocated type objects, and
 * statically allocated singletons (like Py_True and Py_None).
 */
extern "C" void _Py_AddToAllObjects(PyObject* op, int force) noexcept {
#ifdef Py_DEBUG
    if (!force) {
        /* If it's initialized memory, op must be in or out of
         * the list unambiguously.
         */
        assert((op->_ob_prev == NULL) == (op->_ob_next == NULL));
    }
#endif
    if (force || op->_ob_prev == NULL) {
        op->_ob_next = refchain._ob_next;
        op->_ob_prev = &refchain;
        refchain._ob_next->_ob_prev = op;
        refchain._ob_next = op;
    }
}
#endif /* Py_TRACE_REFS */

#ifdef Py_TRACE_REFS

extern "C" void _Py_NewReference(PyObject* op) noexcept {
    _Py_INC_REFTOTAL;
    op->ob_refcnt = 1;
    _Py_AddToAllObjects(op, 1);
    _Py_INC_TPALLOCS(op);
}

extern "C" void _Py_ForgetReference(register PyObject* op) noexcept {
#ifdef SLOW_UNREF_CHECK
    register PyObject* p;
#endif
    if (op->ob_refcnt < 0)
        Py_FatalError("UNREF negative refcnt");
    if (op == &refchain || op->_ob_prev->_ob_next != op || op->_ob_next->_ob_prev != op)
        Py_FatalError("UNREF invalid object");
#ifdef SLOW_UNREF_CHECK
    for (p = refchain._ob_next; p != &refchain; p = p->_ob_next) {
        if (p == op)
            break;
    }
    if (p == &refchain) /* Not found */
        Py_FatalError("UNREF unknown object");
#endif
    op->_ob_next->_ob_prev = op->_ob_prev;
    op->_ob_prev->_ob_next = op->_ob_next;
    op->_ob_next = op->_ob_prev = NULL;
    _Py_INC_TPFREES(op);
}

extern "C" void _Py_Dealloc(PyObject* op) noexcept {
    destructor dealloc = Py_TYPE(op)->tp_dealloc;
    _Py_ForgetReference(op);
    (*dealloc)(op);
}

/* Print all live objects.  Because PyObject_Print is called, the
 * interpreter must be in a healthy state.
 */
extern "C" void _Py_PrintReferences(FILE* fp) noexcept {
    PyObject* op;
    fprintf(fp, "Remaining objects:\n");
    for (op = refchain._ob_next; op != &refchain; op = op->_ob_next) {
        fprintf(fp, "%p [%" PY_FORMAT_SIZE_T "d] ", op, op->ob_refcnt);
        if (PyObject_Print(op, fp, 0) != 0)
            PyErr_Clear();
        putc('\n', fp);
    }
}

/* Print the addresses of all live objects.  Unlike _Py_PrintReferences, this
 * doesn't make any calls to the Python C API, so is always safe to call.
 */
extern "C" void _Py_PrintReferenceAddresses(FILE* fp) noexcept {
    _Py_PrintReferenceAddressesCapped(fp, INT_MAX);
}

extern "C" void _Py_PrintReferenceAddressesCapped(FILE* fp, int max_to_print) noexcept {
    PyObject* op;
    fprintf(fp, "Remaining object addresses:\n");
    int found = 0;
    for (op = refchain._ob_next; op != &refchain; op = op->_ob_next) {
        found++;
        if (found <= max_to_print) {
            fprintf(fp, "%p [%" PY_FORMAT_SIZE_T "d] %s     \033[40mwatch -l ((PyObject*)%p)->ob_refcnt\033[0m\n", op,
                    op->ob_refcnt, Py_TYPE(op)->tp_name, op);
        }
    }
    if (found > max_to_print) {
        fprintf(fp, "%d more found (but not printed)\n", found - max_to_print);
    }
}

extern "C" PyObject* _Py_GetObjects(PyObject* self, PyObject* args) noexcept {
    int i, n;
    PyObject* t = NULL;
    PyObject* res, *op;

    if (!PyArg_ParseTuple(args, "i|O", &n, &t))
        return NULL;
    op = refchain._ob_next;
    res = PyList_New(0);
    if (res == NULL)
        return NULL;
    for (i = 0; (n == 0 || i < n) && op != &refchain; i++) {
        while (op == self || op == args || op == res || op == t || (t != NULL && Py_TYPE(op) != (PyTypeObject*)t)) {
            op = op->_ob_next;
            if (op == &refchain)
                return res;
        }
        if (PyList_Append(res, op) < 0) {
            Py_DECREF(res);
            return NULL;
        }
        op = op->_ob_next;
    }
    return res;
}

#endif
}
