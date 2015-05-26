// This file is originally from CPython 2.7, with modifications for Pyston
// The code is normally part of descrobject.c

#include "Python.h"

/* --- Readonly proxy for dictionaries (actually any mapping) --- */

/* This has no reason to be in this file except that adding new files is a
   bit of a pain */

typedef struct {
    PyObject_HEAD
    PyObject *dict;
} proxyobject;

static Py_ssize_t
proxy_len(proxyobject *pp)
{
    return PyObject_Size(pp->dict);
}

static PyObject *
proxy_getitem(proxyobject *pp, PyObject *key)
{
    return PyObject_GetItem(pp->dict, key);
}

static PyMappingMethods proxy_as_mapping = {
    (lenfunc)proxy_len,                         /* mp_length */
    (binaryfunc)proxy_getitem,                  /* mp_subscript */
    0,                                          /* mp_ass_subscript */
};

static int
proxy_contains(proxyobject *pp, PyObject *key)
{
    return PyDict_Contains(pp->dict, key);
}

static PySequenceMethods proxy_as_sequence = {
    0,                                          /* sq_length */
    0,                                          /* sq_concat */
    0,                                          /* sq_repeat */
    0,                                          /* sq_item */
    0,                                          /* sq_slice */
    0,                                          /* sq_ass_item */
    0,                                          /* sq_ass_slice */
    (objobjproc)proxy_contains,                 /* sq_contains */
    0,                                          /* sq_inplace_concat */
    0,                                          /* sq_inplace_repeat */
};

static PyObject *
proxy_has_key(proxyobject *pp, PyObject *key)
{
    int res = PyDict_Contains(pp->dict, key);
    if (res < 0)
        return NULL;
    return PyBool_FromLong(res);
}

static PyObject *
proxy_get(proxyobject *pp, PyObject *args)
{
    PyObject *key, *def = Py_None;

    if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &def))
        return NULL;
    return PyObject_CallMethod(pp->dict, "get", "(OO)", key, def);
}

static PyObject *
proxy_keys(proxyobject *pp)
{
    return PyMapping_Keys(pp->dict);
}

static PyObject *
proxy_values(proxyobject *pp)
{
    return PyMapping_Values(pp->dict);
}

static PyObject *
proxy_items(proxyobject *pp)
{
    return PyMapping_Items(pp->dict);
}

static PyObject *
proxy_iterkeys(proxyobject *pp)
{
    return PyObject_CallMethod(pp->dict, "iterkeys", NULL);
}

static PyObject *
proxy_itervalues(proxyobject *pp)
{
    return PyObject_CallMethod(pp->dict, "itervalues", NULL);
}

static PyObject *
proxy_iteritems(proxyobject *pp)
{
    return PyObject_CallMethod(pp->dict, "iteritems", NULL);
}
static PyObject *
proxy_copy(proxyobject *pp)
{
    return PyObject_CallMethod(pp->dict, "copy", NULL);
}

static PyMethodDef proxy_methods[] = {
    {"has_key",   (PyCFunction)proxy_has_key,    METH_O,
     PyDoc_STR("D.has_key(k) -> True if D has a key k, else False")},
    {"get",       (PyCFunction)proxy_get,        METH_VARARGS,
     PyDoc_STR("D.get(k[,d]) -> D[k] if D.has_key(k), else d."
                                    "  d defaults to None.")},
    {"keys",      (PyCFunction)proxy_keys,       METH_NOARGS,
     PyDoc_STR("D.keys() -> list of D's keys")},
    {"values",    (PyCFunction)proxy_values,     METH_NOARGS,
     PyDoc_STR("D.values() -> list of D's values")},
    {"items",     (PyCFunction)proxy_items,      METH_NOARGS,
     PyDoc_STR("D.items() -> list of D's (key, value) pairs, as 2-tuples")},
    {"iterkeys",  (PyCFunction)proxy_iterkeys,   METH_NOARGS,
     PyDoc_STR("D.iterkeys() -> an iterator over the keys of D")},
    {"itervalues",(PyCFunction)proxy_itervalues, METH_NOARGS,
     PyDoc_STR("D.itervalues() -> an iterator over the values of D")},
    {"iteritems", (PyCFunction)proxy_iteritems,  METH_NOARGS,
     PyDoc_STR("D.iteritems() ->"
               " an iterator over the (key, value) items of D")},
    {"copy",      (PyCFunction)proxy_copy,       METH_NOARGS,
     PyDoc_STR("D.copy() -> a shallow copy of D")},
    {0}
};

static void
proxy_dealloc(proxyobject *pp)
{
    _PyObject_GC_UNTRACK(pp);
    Py_DECREF(pp->dict);
    PyObject_GC_Del(pp);
}

static PyObject *
proxy_getiter(proxyobject *pp)
{
    return PyObject_GetIter(pp->dict);
}

static PyObject *
proxy_str(proxyobject *pp)
{
    return PyObject_Str(pp->dict);
}

static PyObject *
proxy_repr(proxyobject *pp)
{
    PyObject *dictrepr;
    PyObject *result;

    dictrepr = PyObject_Repr(pp->dict);
    if (dictrepr == NULL)
        return NULL;
    result = PyString_FromFormat("dict_proxy(%s)", PyString_AS_STRING(dictrepr));
    Py_DECREF(dictrepr);
    return result;
}

static int
proxy_traverse(PyObject *self, visitproc visit, void *arg)
{
    proxyobject *pp = (proxyobject *)self;
    Py_VISIT(pp->dict);
    return 0;
}

static int
proxy_compare(proxyobject *v, PyObject *w)
{
    return PyObject_Compare(v->dict, w);
}

static PyObject *
proxy_richcompare(proxyobject *v, PyObject *w, int op)
{
    return PyObject_RichCompare(v->dict, w, op);
}

PyTypeObject PyDictProxy_Type = {
    // Pyston change:
    // PyVarObject_HEAD_INIT(&PyType_Type, 0)
    PyVarObject_HEAD_INIT(NULL, 0)
    "dictproxy",                                /* tp_name */
    sizeof(proxyobject),                        /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)proxy_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    (cmpfunc)proxy_compare,                     /* tp_compare */
    (reprfunc)proxy_repr,                       /* tp_repr */
    0,                                          /* tp_as_number */
    &proxy_as_sequence,                         /* tp_as_sequence */
    &proxy_as_mapping,                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    (reprfunc)proxy_str,                        /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                          /* tp_doc */
    proxy_traverse,                             /* tp_traverse */
    0,                                          /* tp_clear */
    (richcmpfunc)proxy_richcompare,             /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)proxy_getiter,                 /* tp_iter */
    0,                                          /* tp_iternext */
    proxy_methods,                              /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
};

PyObject *
PyDictProxy_New(PyObject *dict)
{
    proxyobject *pp;

    pp = PyObject_GC_New(proxyobject, &PyDictProxy_Type);
    if (pp != NULL) {
        Py_INCREF(dict);
        pp->dict = dict;
        _PyObject_GC_TRACK(pp);
    }
    return (PyObject *)pp;
}

