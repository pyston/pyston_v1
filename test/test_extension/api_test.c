#include <Python.h>

static PyObject *
set_size(PyObject *self, PyObject *so)
{
    return Py_BuildValue("i", PySet_Size(so));
}

static PyMethodDef TestMethods[] = {
    {"set_size",  set_size, METH_O, "Get set size by PySet_Size." },
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

PyMODINIT_FUNC
initapi_test(void)
{
    PyObject *m;

    m = Py_InitModule("api_test", TestMethods);
    if (m == NULL)
        return;
}
