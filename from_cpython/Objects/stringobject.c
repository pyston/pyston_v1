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

