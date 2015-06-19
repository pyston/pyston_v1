#include "Python.h"

#include "code.h"
#include "compile.h"
#include "Python-ast.h"
#include "symtable.h"

static PyObject *
symtable_symtable(PyObject *self, PyObject *args)
{
    struct symtable *st;
    PyObject *t;

    char *str;
    char *filename;
    char *startstr;
    int start;

    if (!PyArg_ParseTuple(args, "sss:symtable", &str, &filename,
                          &startstr))
        return NULL;
    if (strcmp(startstr, "exec") == 0)
        start = Py_file_input;
    else if (strcmp(startstr, "eval") == 0)
        start = Py_eval_input;
    else if (strcmp(startstr, "single") == 0)
        start = Py_single_input;
    else {
        PyErr_SetString(PyExc_ValueError,
           "symtable() arg 3 must be 'exec' or 'eval' or 'single'");
        return NULL;
    }
    st = Py_SymtableString(str, filename, start);
    if (st == NULL)
        return NULL;
    t = (PyObject *)st->st_top;
    Py_INCREF(t);
    PyMem_Free((void *)st->st_future);
    PySymtable_Free(st);
    return t;
}

static PyMethodDef symtable_methods[] = {
    {"symtable",        symtable_symtable,      METH_VARARGS,
     PyDoc_STR("Return symbol and scope dictionaries"
               " used internally by compiler.")},
    {NULL,              NULL}           /* sentinel */
};

PyMODINIT_FUNC
init_symtable(void)
{
    PyObject *m;

    if (PyType_Ready(&PySTEntry_Type) < 0)
        return;

    m = Py_InitModule("_symtable", symtable_methods);
    if (m == NULL)
        return;
    PyModule_AddIntConstant(m, "USE", USE);
    PyModule_AddIntConstant(m, "DEF_GLOBAL", DEF_GLOBAL);
    PyModule_AddIntConstant(m, "DEF_LOCAL", DEF_LOCAL);
    PyModule_AddIntConstant(m, "DEF_PARAM", DEF_PARAM);
    PyModule_AddIntConstant(m, "DEF_FREE", DEF_FREE);
    PyModule_AddIntConstant(m, "DEF_FREE_CLASS", DEF_FREE_CLASS);
    PyModule_AddIntConstant(m, "DEF_IMPORT", DEF_IMPORT);
    PyModule_AddIntConstant(m, "DEF_BOUND", DEF_BOUND);

    PyModule_AddIntConstant(m, "TYPE_FUNCTION", FunctionBlock);
    PyModule_AddIntConstant(m, "TYPE_CLASS", ClassBlock);
    PyModule_AddIntConstant(m, "TYPE_MODULE", ModuleBlock);

    PyModule_AddIntConstant(m, "OPT_IMPORT_STAR", OPT_IMPORT_STAR);
    PyModule_AddIntConstant(m, "OPT_EXEC", OPT_EXEC);
    PyModule_AddIntConstant(m, "OPT_BARE_EXEC", OPT_BARE_EXEC);

    PyModule_AddIntConstant(m, "LOCAL", LOCAL);
    PyModule_AddIntConstant(m, "GLOBAL_EXPLICIT", GLOBAL_EXPLICIT);
    PyModule_AddIntConstant(m, "GLOBAL_IMPLICIT", GLOBAL_IMPLICIT);
    PyModule_AddIntConstant(m, "FREE", FREE);
    PyModule_AddIntConstant(m, "CELL", CELL);

    PyModule_AddIntConstant(m, "SCOPE_OFF", SCOPE_OFF);
    PyModule_AddIntConstant(m, "SCOPE_MASK", SCOPE_MASK);
}
