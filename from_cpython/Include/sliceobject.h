// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_SLICEOBJECT_H
#define Py_SLICEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/* The unique ellipsis object "..." */

// Pyston change
// PyAPI_DATA(PyObject) _Py_EllipsisObject; /* Don't use this directly */
// #define Py_Ellipsis (&_Py_EllipsisObject)
PyAPI_DATA(PyObject) *Ellipsis; /* Don't use this directly */
#define Py_Ellipsis Ellipsis

/* Slice object interface */

/*

A slice object containing start, stop, and step data members (the
names are from range).  After much talk with Guido, it was decided to
let these be any arbitrary python type.  Py_None stands for omitted values.
*/

// Pyston change: comment this out since this is not the format we're using
#if 0
typedef struct {
    PyObject_HEAD
    PyObject *start, *stop, *step;	/* not NULL */
} PySliceObject;
#endif
struct _PySliceObject;
typedef struct _PySliceObject PySliceObject;

// Pyston change: these are no longer static objects
PyAPI_DATA(PyTypeObject*) slice_cls;
#define PySlice_Type (*slice_cls)
PyAPI_DATA(PyTypeObject*) ellipsis_cls;
#define PyEllipsis_Type (*ellipsis_cls)

// Pyston changes: these aren't direct macros any more [they potentially could be though]
//#define PySlice_Check(op) (Py_TYPE(op) == &PySlice_Type)
PyAPI_FUNC(bool) _PySlice_Check(PyObject*) PYSTON_NOEXCEPT;
#define PySlice_Check(op) _PySlice_Check((PyObject*)(op))

PyAPI_FUNC(PyObject *) PySlice_New(PyObject* start, PyObject* stop,
                                  PyObject* step) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PySlice_FromIndices(Py_ssize_t start, Py_ssize_t stop) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySlice_GetIndices(PySliceObject *r, Py_ssize_t length,
                                  Py_ssize_t *start, Py_ssize_t *stop, Py_ssize_t *step) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySlice_GetIndicesEx(PySliceObject *r, Py_ssize_t length,
				    Py_ssize_t *start, Py_ssize_t *stop, 
				    Py_ssize_t *step, Py_ssize_t *slicelength) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_SLICEOBJECT_H */
