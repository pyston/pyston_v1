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

static PyObject*
change_self(PyObject* self, PyObject* args) {
    PyObject *im, *inst;
    if (!PyArg_ParseTuple(args, "OO", &im, &inst))
        return NULL;

#if defined(PYSTON_VERSION)
    if (!PyMethod_SetSelf(im, inst))
        return NULL;
#else
    Py_XDECREF(((PyMethodObject*)im)->im_self);
    Py_INCREF(inst);
    ((PyMethodObject*)im)->im_self = inst;
#endif
    Py_RETURN_NONE;
}

static PyMethodDef TestMethods[] = {
    {"set_size",  set_size, METH_O, "Get set size by PySet_Size." },
    {"test_attrwrapper_parse",  test_attrwrapper_parse, METH_VARARGS, "Test PyArg_ParseTuple for attrwrappers." },
    {"change_self",  change_self, METH_VARARGS,
     "A function which the self point to its base class."},
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
