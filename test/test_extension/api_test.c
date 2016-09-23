#include <Python.h>

static PyObject *
set_size(PyObject *self, PyObject *so)
{
    return Py_BuildValue("i", PySet_Size(so));
}

static PyObject*
test_attrwrapper_parse(PyObject *self, PyObject* args) {
    PyObject* d;
    int r = PyArg_ParseTuple(args, "O!", &PyDict_Type, &d);
    if (!r)
        return NULL;
    Py_RETURN_NONE;
}

static PyMethodDef TestMethods[] = {
    {"set_size",  set_size, METH_O, "Get set size by PySet_Size." },
    {"test_attrwrapper_parse",  test_attrwrapper_parse, METH_VARARGS, "Test PyArg_ParseTuple for attrwrappers." },
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
