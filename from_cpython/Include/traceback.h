// This file is originally from CPython 2.7, with modifications for Pyston

#ifndef Py_TRACEBACK_H
#define Py_TRACEBACK_H
#ifdef __cplusplus
extern "C" {
#endif

struct _frame;

/* Traceback interface */

typedef struct _traceback {
	PyObject_HEAD
	struct _traceback *tb_next;
	struct _frame *tb_frame;
	int tb_lasti;
	int tb_lineno;
} PyTracebackObject;

PyAPI_FUNC(int) PyTraceBack_Here(struct _frame *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyTraceBack_Print(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _Py_DisplaySourceLine(PyObject *, const char *, int, int) PYSTON_NOEXCEPT;

// Pyston change: this function does not modify curexc_traceback but instead sets the supplied tb
PyAPI_FUNC(int) PyTraceBack_Here_Tb(struct _frame *, PyTracebackObject **) PYSTON_NOEXCEPT;

/* Reveal traceback type so we can typecheck traceback objects */
PyAPI_DATA(PyTypeObject) PyTraceBack_Type;
#define PyTraceBack_Check(v) (Py_TYPE(v) == &PyTraceBack_Type)

#ifdef __cplusplus
}
#endif
#endif /* !Py_TRACEBACK_H */
