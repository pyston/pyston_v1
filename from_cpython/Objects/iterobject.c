// This file is originally from CPython 2.7, with modifications for Pyston

// (actually it's just a subset of the CPython version)

#include "Python.h"

extern PyTypeObject PyCallIter_Type;
typedef struct {
    PyObject_HEAD
    PyObject *it_callable; /* Set to NULL when iterator is exhausted */
    PyObject *it_sentinel; /* Set to NULL when iterator is exhausted */
} calliterobject;

PyObject *
PyCallIter_New(PyObject *callable, PyObject *sentinel)
{
    calliterobject *it;
    it = PyObject_GC_New(calliterobject, &PyCallIter_Type);
    if (it == NULL)
        return NULL;
    Py_INCREF(callable);
    it->it_callable = callable;
    Py_INCREF(sentinel);
    it->it_sentinel = sentinel;
    _PyObject_GC_TRACK(it);
    return (PyObject *)it;
}
static void
calliter_dealloc(calliterobject *it)
{
    _PyObject_GC_UNTRACK(it);
    Py_XDECREF(it->it_callable);
    Py_XDECREF(it->it_sentinel);
    PyObject_GC_Del(it);
}

static int
calliter_traverse(calliterobject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->it_callable);
    Py_VISIT(it->it_sentinel);
    return 0;
}

static PyObject *
calliter_iternext(calliterobject *it)
{
    if (it->it_callable != NULL) {
        PyObject *args = PyTuple_New(0);
        PyObject *result;
        if (args == NULL)
            return NULL;
        result = PyObject_Call(it->it_callable, args, NULL);
        Py_DECREF(args);
        if (result != NULL) {
            int ok;
            ok = PyObject_RichCompareBool(result,
                                          it->it_sentinel,
                                          Py_EQ);
            if (ok == 0)
                return result; /* Common case, fast path */
            Py_DECREF(result);
            if (ok > 0) {
                Py_CLEAR(it->it_callable);
                Py_CLEAR(it->it_sentinel);
            }
        }
        else if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
            PyErr_Clear();
            Py_CLEAR(it->it_callable);
            Py_CLEAR(it->it_sentinel);
        }
    }
    return NULL;
}

PyTypeObject PyCallIter_Type = {
    // Pyston change:
    PyVarObject_HEAD_INIT(NULL /* &PyType_Type */, 0)
    "callable-iterator",                        /* tp_name */
    sizeof(calliterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)calliter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)calliter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)calliter_iternext,            /* tp_iternext */
    0,                                          /* tp_methods */
};

