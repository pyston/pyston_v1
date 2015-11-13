// This file is originally from CPython 2.7, with modifications for Pyston

/* Module object interface */

#ifndef Py_MODULEOBJECT_H
#define Py_MODULEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

// Pyston change: this is no longer a static object
//PyAPI_DATA(PyTypeObject) PyModule_Type;
PyAPI_DATA(PyTypeObject*) module_cls;
#define PyModule_Type (*module_cls)

#define PyModule_Check(op) PyObject_TypeCheck(op, &PyModule_Type)
#define PyModule_CheckExact(op) (Py_TYPE(op) == &PyModule_Type)

PyAPI_FUNC(PyObject *) PyModule_New(const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(BORROWED(PyObject *)) PyModule_GetDict(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(char *) PyModule_GetName(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(char *) PyModule_GetFilename(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyModule_Clear(PyObject *) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_MODULEOBJECT_H */
