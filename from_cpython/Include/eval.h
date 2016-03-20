
/* Interface to execute compiled code */

#ifndef Py_EVAL_H
#define Py_EVAL_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(PyObject *) PyEval_EvalCode(PyCodeObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) PyEval_EvalCodeEx(PyCodeObject *co,
					PyObject *globals,
					PyObject *locals,
					PyObject **args, int argc,
					PyObject **kwds, int kwdc,
					PyObject **defs, int defc,
                    PyObject *closure) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) _PyEval_CallTracing(PyObject *func, PyObject *args) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_EVAL_H */
