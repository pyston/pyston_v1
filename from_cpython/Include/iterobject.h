// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_ITEROBJECT_H
#define Py_ITEROBJECT_H
/* Iterators (the basic kind, over a sequence) */
#ifdef __cplusplus
extern "C" {
#endif

// Pyston change: moved this from iterobject.c
typedef struct {
    PyObject_HEAD
    PyObject *it_callable; /* Set to NULL when iterator is exhausted */
    PyObject *it_sentinel; /* Set to NULL when iterator is exhausted */
    // Pyston changes:
    PyObject *it_nextvalue; /* Set to non-null when iterator is advanced in __hasnext__ */
} calliterobject;

// Pyston change: this is no longer a static object
//PyAPI_DATA(PyTypeObject) PySeqIter_Type;

#define PySeqIter_Check(op) (Py_TYPE(op) == &PySeqIter_Type)

PyAPI_FUNC(PyObject *) PySeqIter_New(PyObject *) PYSTON_NOEXCEPT;

PyAPI_DATA(PyTypeObject) PyCallIter_Type;

#define PyCallIter_Check(op) (Py_TYPE(op) == &PyCallIter_Type)

PyAPI_FUNC(PyObject *) PyCallIter_New(PyObject *, PyObject *) PYSTON_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif /* !Py_ITEROBJECT_H */

