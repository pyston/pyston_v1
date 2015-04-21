// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_TRACEBACK_H
#define Py_TRACEBACK_H
#ifdef __cplusplus
extern "C" {
#endif

struct _frame;

/* Traceback interface */

// Pyston change: not necessarily our object format
#if 0
typedef struct _traceback {
	PyObject_HEAD
	struct _traceback *tb_next;
	struct _frame *tb_frame;
	int tb_lasti;
	int tb_lineno;
} PyTracebackObject;
#endif
typedef struct _PyTracebackObject PyTracebackObject;

PyAPI_FUNC(int) PyTraceBack_Here(struct _frame *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyTraceBack_Print(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _Py_DisplaySourceLine(PyObject *, const char *, int, int) PYSTON_NOEXCEPT;

/* Reveal traceback type so we can typecheck traceback objects */
// Pyston change: not a static type any more
PyAPI_DATA(PyTypeObject*) traceback_cls;
#define PyTraceBack_Type (*traceback_cls)
// PyAPI_DATA(PyTypeObject) PyTraceBack_Type;
#define PyTraceBack_Check(v) (Py_TYPE(v) == &PyTraceBack_Type)

#ifdef __cplusplus
}
#endif
#endif /* !Py_TRACEBACK_H */
