#include <Python.h>

#include <signal.h>

static PyObject* stored = NULL;
static PyObject *
test_store(PyObject *self, PyObject *args)
{
    PyObject* arg;

    //raise(SIGTRAP);
    if (!PyArg_ParseTuple(args, "O", &arg))
        return NULL;

    Py_INCREF(arg);
    stored = arg;
    return Py_BuildValue("");
}

void incref(PyObject *o) {
    Py_INCREF(o);
}

static PyObject *
test_load(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
        return NULL;

    assert(stored);
    Py_INCREF(stored);
    return stored;
}

static PyMethodDef TestMethods[] = {
    {"store",  test_store, METH_VARARGS, "Store."},
    {"load",  test_load, METH_VARARGS, "Load."},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

PyMODINIT_FUNC
initbasic_test(void)
{
    PyObject *m;

    m = Py_InitModule("basic_test", TestMethods);
    if (m == NULL)
        return;

#ifdef PYSTON_VERSION
    PyGC_RegisterStaticConstantLocation(&stored);
#endif
}
