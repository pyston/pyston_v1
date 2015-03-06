// This file is originally from CPython 2.7, with modifications for Pyston

/* Interface for marshal.c */

#ifndef Py_MARSHAL_H
#define Py_MARSHAL_H
#ifdef __cplusplus
extern "C" {
#endif

#define Py_MARSHAL_VERSION 2

PyAPI_FUNC(void) PyMarshal_WriteLongToFile(long, FILE *, int) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyMarshal_WriteObjectToFile(PyObject *, FILE *, int) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMarshal_WriteObjectToString(PyObject *, int) PYSTON_NOEXCEPT;

PyAPI_FUNC(long) PyMarshal_ReadLongFromFile(FILE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyMarshal_ReadShortFromFile(FILE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMarshal_ReadObjectFromFile(FILE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMarshal_ReadLastObjectFromFile(FILE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMarshal_ReadObjectFromString(char *, Py_ssize_t) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_MARSHAL_H */
