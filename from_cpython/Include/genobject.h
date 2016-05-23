// This file is originally from CPython 2.7, with modifications for Pyston

/* Generator object interface */

#ifndef Py_GENOBJECT_H
#define Py_GENOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

struct _frame; /* Avoid including frameobject.h */

// Pyston change: this is not our object format
#if 0
typedef struct {
	PyObject_HEAD
	/* The gi_ prefix is intended to remind of generator-iterator. */

	/* Note: gi_frame can be NULL if the generator is "finished" */
	struct _frame *gi_frame;

	/* True if generator is being executed. */
	int gi_running;
    
	/* The code object backing the generator */
	PyObject *gi_code;

	/* List of weak reference. */
	PyObject *gi_weakreflist;
} PyGenObject;
#endif
typedef struct _PyGenObject PyGenObject;

// Pyston change: not a static object
//PyAPI_DATA(PyTypeObject) PyGen_Type;
PyAPI_DATA(PyTypeObject*) generator_cls;
#define PyGen_Type (*generator_cls)

#define PyGen_Check(op) PyObject_TypeCheck(op, &PyGen_Type)
#define PyGen_CheckExact(op) (Py_TYPE(op) == &PyGen_Type)

PyAPI_FUNC(PyObject *) PyGen_New(struct _frame *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyGen_NeedsFinalizing(PyGenObject *) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_GENOBJECT_H */
