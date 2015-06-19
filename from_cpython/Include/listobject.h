// This file is originally from CPython 2.7, with modifications for Pyston

/* List object interface */

/*
Another generally useful object type is an list of object pointers.
This is a mutable type: the list items can be changed, and items can be
added or removed.  Out-of-range indices or non-list objects are ignored.

*** WARNING *** PyList_SetItem does not increment the new item's reference
count, but does decrement the reference count of the item it replaces,
if not nil.  It does *decrement* the reference count if it is *not*
inserted in the list.  Similarly, PyList_GetItem does not increment the
returned item's reference count.
*/

#ifndef Py_LISTOBJECT_H
#define Py_LISTOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

// Pyston change: comment this out since this is not the format we're using
#if 0
typedef struct {
    PyObject_VAR_HEAD
    /* Vector of pointers to list elements.  list[0] is ob_item[0], etc. */
    PyObject **ob_item;

    /* ob_item contains space for 'allocated' elements.  The number
     * currently in use is ob_size.
     * Invariants:
     *     0 <= ob_size <= allocated
     *     len(list) == ob_size
     *     ob_item == NULL implies ob_size == allocated == 0
     * list.sort() temporarily sets allocated to -1 to detect mutations.
     *
     * Items must normally not be NULL, except during construction when
     * the list is not yet visible outside the function that builds it.
     */
    Py_ssize_t allocated;
} PyListObject;
#endif
struct _PyListObject;
typedef struct _PyListObject PyListObject;

// Pyston change: this is no longer a static object
PyAPI_DATA(PyTypeObject*) list_cls;
#define PyList_Type (*list_cls)

#define PyList_Check(op) \
		PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_LIST_SUBCLASS)
#define PyList_CheckExact(op) (Py_TYPE(op) == &PyList_Type)

PyAPI_FUNC(PyObject *) PyList_New(Py_ssize_t size) PYSTON_NOEXCEPT;
PyAPI_FUNC(Py_ssize_t) PyList_Size(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyList_GetItem(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyList_SetItem(PyObject *, Py_ssize_t, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyList_Insert(PyObject *, Py_ssize_t, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyList_Append(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyList_GetSlice(PyObject *, Py_ssize_t, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyList_SetSlice(PyObject *, Py_ssize_t, Py_ssize_t, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyList_Sort(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyList_Reverse(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyList_AsTuple(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyList_Extend(PyListObject *, PyObject *) PYSTON_NOEXCEPT;

// Pyston addition:
PyAPI_FUNC(PyObject **) PyList_Items(PyObject *) PYSTON_NOEXCEPT;

/* Macro, trading safety for speed */
// Pyston changes: these aren't direct macros any more [they potentially could be though]
#define PyList_GET_ITEM(op, i) PyList_GetItem((PyObject*)(op), (i))
#define PyList_SET_ITEM(op, i, v) PyList_SetItem((PyObject*)(op), (i), (v))
#define PyList_GET_SIZE(op)    PyList_Size((PyObject*)(op))
//#define PyList_GET_ITEM(op, i) (((PyListObject *)(op))->ob_item[i])
//#define PyList_SET_ITEM(op, i, v) (((PyListObject *)(op))->ob_item[i] = (v))
//#define PyList_GET_SIZE(op)    Py_SIZE(op)

#ifdef __cplusplus
}
#endif
#endif /* !Py_LISTOBJECT_H */
