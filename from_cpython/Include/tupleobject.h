// This file is originally from CPython 2.7, with modifications for Pyston

/* Tuple object interface */

#ifndef Py_TUPLEOBJECT_H
#define Py_TUPLEOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/*
Another generally useful object type is a tuple of object pointers.
For Python, this is an immutable type.  C code can change the tuple items
(but not their number), and even use tuples are general-purpose arrays of
object references, but in general only brand new tuples should be mutated,
not ones that might already have been exposed to Python code.

*** WARNING *** PyTuple_SetItem does not increment the new item's reference
count, but does decrement the reference count of the item it replaces,
if not nil.  It does *decrement* the reference count if it is *not*
inserted in the tuple.  Similarly, PyTuple_GetItem does not increment the
returned item's reference count.
*/

// Pyston change: this is not the format we're using (but maybe it should be)
#if 0
typedef struct {
    PyObject_VAR_HEAD
    PyObject *ob_item[1];

    /* ob_item contains space for 'ob_size' elements.
     * Items must normally not be NULL, except during construction when
     * the tuple is not yet visible outside the function that builds it.
     */
} PyTupleObject;
#endif
struct _PyTupleObject;
typedef struct _PyTupleObject PyTupleObject;

// Pyston change: this is no longer a static object
PyAPI_DATA(PyTypeObject*) tuple_cls;
#define PyTuple_Type (*tuple_cls)

// Pyston changes: these aren't direct macros any more [they potentially could be though]
PyAPI_FUNC(bool) PyTuple_Check(PyObject*);
#if 0
#define PyTuple_Check(op) \
                 PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_TUPLE_SUBCLASS)
#endif
#define PyTuple_CheckExact(op) (Py_TYPE(op) == &PyTuple_Type)

PyAPI_FUNC(PyObject *) PyTuple_New(Py_ssize_t size);
PyAPI_FUNC(Py_ssize_t) PyTuple_Size(PyObject *);
PyAPI_FUNC(PyObject *) PyTuple_GetItem(PyObject *, Py_ssize_t);
PyAPI_FUNC(int) PyTuple_SetItem(PyObject *, Py_ssize_t, PyObject *);
PyAPI_FUNC(PyObject *) PyTuple_GetSlice(PyObject *, Py_ssize_t, Py_ssize_t);
PyAPI_FUNC(int) _PyTuple_Resize(PyObject **, Py_ssize_t);
PyAPI_FUNC(PyObject *) PyTuple_Pack(Py_ssize_t, ...);
PyAPI_FUNC(void) _PyTuple_MaybeUntrack(PyObject *);

/* Macro, trading safety for speed */
// Pyston changes: these aren't direct macros any more [they potentially could be though]
#define PyTuple_GET_ITEM(op, i) PyTuple_GetItem(op, i)
#define PyTuple_GET_SIZE(op)    PyTuple_Size(op)
//#define PyTuple_GET_ITEM(op, i) (((PyTupleObject *)(op))->ob_item[i])
//#define PyTuple_GET_SIZE(op)    Py_SIZE(op)
/* Macro, *only* to be used to fill in brand new tuples */
#define PyTuple_SET_ITEM(op, i, v) PyTuple_SetItem((PyObject*)op, i, v)
//#define PyTuple_SET_ITEM(op, i, v) (((PyTupleObject *)(op))->ob_item[i] = v)

PyAPI_FUNC(int) PyTuple_ClearFreeList(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_TUPLEOBJECT_H */

