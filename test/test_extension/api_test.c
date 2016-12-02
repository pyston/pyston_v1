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

static PyObject*
dict_API_test(PyObject* self, PyObject* args) {
    PyObject *inst;
    if (!PyArg_ParseTuple(args, "O", &inst))
        return NULL;

#if defined(PYSTON_VERSION)
    // Get `name` field in inst.
    PyObject* dict = PyObject_GetDictCopy(inst);
    if (dict == Py_None)
        return NULL;
    PyObject* name = PyDict_GetItem(dict, PyString_FromString("name"));

    // Clear the object's dict.
    PyObject_ClearDict(inst);
    PyObject* old_dict = PyObject_GetDictCopy(inst);
    // Set new items for object's dict.
    PyObject* new_dict = PyDict_New();
    PyDict_SetItem(new_dict, PyString_FromString("value"), PyInt_FromLong(42));
    PyObject_UpdateDict(inst, new_dict);

    // The return values should be 'Pyston', {}
    return Py_BuildValue("OO", name, old_dict);
#endif
    return Py_BuildValue("OO", Py_None, Py_None);
}

static PyMethodDef TestMethods[] = {
    {"set_size",  set_size, METH_O, "Get set size by PySet_Size." },
    {"test_attrwrapper_parse",  test_attrwrapper_parse, METH_VARARGS, "Test PyArg_ParseTuple for attrwrappers." },
    {"change_self",  change_self, METH_VARARGS, "A function which the self point to its base class."},
    {"dict_API_test",  dict_API_test, METH_VARARGS, ""},
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
