// Pyston change: this is just a shim to import stuff from stringlib/

#include "Python.h"

#include "stringlib/stringdefs.h"
#include "stringlib/fastsearch.h"

#include "stringlib/count.h"
#include "stringlib/find.h"
#include "stringlib/split.h"

#define _Py_InsertThousandsGrouping _PyString_InsertThousandsGrouping
#include "stringlib/localeutil.h"

#include "stringlib/string_format.h"

// do_string_format needs to be declared as a static function, since it's used by both stringobject.c
// and unicodeobject.c.  We want to access it from str.cpp, though, so just use this little forwarding
// function.
// We could also potentially have tried to modifie string_format.h to choose whether to mark the function
// as static or not.
PyObject * _do_string_format(PyObject *self, PyObject *args, PyObject *kwargs) {
    return do_string_format(self, args, kwargs);
}

PyObject * string_split(PyStringObject *self, PyObject *args)
{
    Py_ssize_t len = PyString_GET_SIZE(self), n;
    Py_ssize_t maxsplit = -1;
    const char *s = PyString_AS_STRING(self), *sub;
    PyObject *subobj = Py_None;

    if (!PyArg_ParseTuple(args, "|On:split", &subobj, &maxsplit))
        return NULL;
    if (maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;
    if (subobj == Py_None)
        return stringlib_split_whitespace((PyObject*) self, s, len, maxsplit);
    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        n = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_Split((PyObject *)self, subobj, maxsplit);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &n))
        return NULL;

    return stringlib_split((PyObject*) self, s, len, sub, n, maxsplit);
}

PyObject* string_rsplit(PyStringObject* self, PyObject* args) {
    Py_ssize_t len = PyString_GET_SIZE(self), n;
    Py_ssize_t maxsplit = -1;
    const char* s = PyString_AS_STRING(self), *sub;
    PyObject* subobj = Py_None;

    if (!PyArg_ParseTuple(args, "|On:rsplit", &subobj, &maxsplit))
        return NULL;
    if (maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;
    if (subobj == Py_None)
        return stringlib_rsplit_whitespace((PyObject*)self, s, len, maxsplit);
    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        n = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_RSplit((PyObject*)self, subobj, maxsplit);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &n))
        return NULL;

    return stringlib_rsplit((PyObject*)self, s, len, sub, n, maxsplit);
}

Py_LOCAL_INLINE(Py_ssize_t)
string_find_internal(PyStringObject *self, PyObject *args, int dir)
{
    PyObject *subobj;
    const char *sub;
    Py_ssize_t sub_len;
    Py_ssize_t start=0, end=PY_SSIZE_T_MAX;

    if (!stringlib_parse_args_finds("find/rfind/index/rindex",
                                    args, &subobj, &start, &end))
        return -2;

    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        sub_len = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_Find(
            (PyObject *)self, subobj, start, end, dir);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &sub_len))
        /* XXX - the "expected a character buffer object" is pretty
           confusing for a non-expert.  remap to something else ? */
        return -2;

    if (dir > 0)
        return stringlib_find_slice(
            PyString_AS_STRING(self), PyString_GET_SIZE(self),
            sub, sub_len, start, end);
    else
        return stringlib_rfind_slice(
            PyString_AS_STRING(self), PyString_GET_SIZE(self),
            sub, sub_len, start, end);
}

PyObject *
string_rfind(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, -1);
    if (result == -2)
        return NULL;
    return PyInt_FromSsize_t(result);
}

PyObject *
string_find(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, +1);
    if (result == -2)
        return NULL;
    return PyInt_FromSsize_t(result);
}
