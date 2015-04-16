// This file is originally from CPython 2.7, with modifications for Pyston

/* Set object interface */

#ifndef Py_SETOBJECT_H
#define Py_SETOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

// Pyston change: comment this out since this is not the format we're using
#if 0
/*
There are three kinds of slots in the table:

1. Unused:  key == NULL
2. Active:  key != NULL and key != dummy
3. Dummy:   key == dummy

Note: .pop() abuses the hash field of an Unused or Dummy slot to
hold a search finger.  The hash field of Unused or Dummy slots has
no meaning otherwise.
*/

#define PySet_MINSIZE 8

typedef struct {
    long hash;      /* cached hash code for the entry key */
    PyObject *key;
} setentry;


/*
This data structure is shared by set and frozenset objects.
*/

typedef struct _setobject PySetObject;
struct _setobject {
    PyObject_HEAD

    Py_ssize_t fill;  /* # Active + # Dummy */
    Py_ssize_t used;  /* # Active */

    /* The table contains mask + 1 slots, and that's a power of 2.
     * We store the mask instead of the size because the mask is more
     * frequently needed.
     */
    Py_ssize_t mask;

    /* table points to smalltable for small tables, else to
     * additional malloc'ed memory.  table is never NULL!  This rule
     * saves repeated runtime null-tests.
     */
    setentry *table;
    setentry *(*lookup)(PySetObject *so, PyObject *key, long hash);
    setentry smalltable[PySet_MINSIZE];

    long hash;                  /* only used by frozenset objects */
    PyObject *weakreflist;      /* List of weak references */
};
#endif
struct _PySetObject;
typedef struct _PySetObject PySetObject;

// Pyston change: these are no longer static objects:
#if 0
PyAPI_DATA(PyTypeObject) PySet_Type;
PyAPI_DATA(PyTypeObject) PyFrozenSet_Type;
#endif
PyAPI_DATA(PyTypeObject*) set_cls;
#define PySet_Type (*set_cls)
PyAPI_DATA(PyTypeObject*) frozenset_cls;
#define PyFrozenSet_Type (*frozenset_cls)

/* Invariants for frozensets:
 *     data is immutable.
 *     hash is the hash of the frozenset or -1 if not computed yet.
 * Invariants for sets:
 *     hash is -1
 */

#define PyFrozenSet_CheckExact(ob) (Py_TYPE(ob) == &PyFrozenSet_Type)
#define PyAnySet_CheckExact(ob) \
    (Py_TYPE(ob) == &PySet_Type || Py_TYPE(ob) == &PyFrozenSet_Type)
#define PyAnySet_Check(ob) \
    (Py_TYPE(ob) == &PySet_Type || Py_TYPE(ob) == &PyFrozenSet_Type || \
      PyType_IsSubtype(Py_TYPE(ob), &PySet_Type) || \
      PyType_IsSubtype(Py_TYPE(ob), &PyFrozenSet_Type))
#define PySet_Check(ob) \
    (Py_TYPE(ob) == &PySet_Type || \
    PyType_IsSubtype(Py_TYPE(ob), &PySet_Type))
#define   PyFrozenSet_Check(ob) \
    (Py_TYPE(ob) == &PyFrozenSet_Type || \
      PyType_IsSubtype(Py_TYPE(ob), &PyFrozenSet_Type))

PyAPI_FUNC(PyObject *) PySet_New(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFrozenSet_New(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(Py_ssize_t) PySet_Size(PyObject *anyset) PYSTON_NOEXCEPT;

// Pyston change
//#define PySet_GET_SIZE(so) (((PySetObject *)(so))->used)
#define PySet_GET_SIZE(so) PySet_Size(so)

PyAPI_FUNC(int) PySet_Clear(PyObject *set) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySet_Contains(PyObject *anyset, PyObject *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySet_Discard(PyObject *set, PyObject *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySet_Add(PyObject *set, PyObject *key) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PySet_Next(PyObject *set, Py_ssize_t *pos, PyObject **key) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PySet_NextEntry(PyObject *set, Py_ssize_t *pos, PyObject **key, long *hash) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PySet_Pop(PyObject *set) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PySet_Update(PyObject *set, PyObject *iterable) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_SETOBJECT_H */
