// This file is originally from CPython 2.7, with modifications for Pyston

/* Function object interface */

#ifndef Py_FUNCOBJECT_H
#define Py_FUNCOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/* Function objects and code objects should not be confused with each other:
 *
 * Function objects are created by the execution of the 'def' statement.
 * They reference a code object in their func_code attribute, which is a
 * purely syntactic object, i.e. nothing more than a compiled version of some
 * source code lines.  There is one code object per source code "fragment",
 * but each code object can be referenced by zero or many function objects
 * depending only on how many times the 'def' statement in the source was
 * executed so far.
 */

// Pyston change: not our object format
#if 0
typedef struct {
    PyObject_HEAD
    PyObject *func_code;	/* A code object */
    PyObject *func_globals;	/* A dictionary (other mappings won't do) */
    PyObject *func_defaults;	/* NULL or a tuple */
    PyObject *func_closure;	/* NULL or a tuple of cell objects */
    PyObject *func_doc;		/* The __doc__ attribute, can be anything */
    PyObject *func_name;	/* The __name__ attribute, a string object */
    PyObject *func_dict;	/* The __dict__ attribute, a dict or NULL */
    PyObject *func_weakreflist;	/* List of weak references */
    PyObject *func_module;	/* The __module__ attribute, can be anything */

    /* Invariant:
     *     func_closure contains the bindings for func_code->co_freevars, so
     *     PyTuple_Size(func_closure) == PyCode_GetNumFree(func_code)
     *     (func_closure may be NULL if PyCode_GetNumFree(func_code) == 0).
     */
} PyFunctionObject;
#endif

// Pyston change: not a static object any more
//PyAPI_DATA(PyTypeObject) PyFunction_Type;
PyAPI_DATA(PyTypeObject*) function_cls;
#define PyFunction_Type (*function_cls)

#define PyFunction_Check(op) (Py_TYPE(op) == &PyFunction_Type)

PyAPI_FUNC(PyObject *) PyFunction_New(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFunction_GetCode(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFunction_GetGlobals(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFunction_GetModule(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFunction_GetDefaults(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyFunction_SetDefaults(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyFunction_GetClosure(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyFunction_SetClosure(PyObject *, PyObject *) PYSTON_NOEXCEPT;

// Pyston change: no longer macros
#if 0
/* Macros for direct access to these values. Type checks are *not*
   done, so use with care. */
#define PyFunction_GET_CODE(func) \
        (((PyFunctionObject *)func) -> func_code)
#define PyFunction_GET_GLOBALS(func) \
	(((PyFunctionObject *)func) -> func_globals)
#define PyFunction_GET_MODULE(func) \
	(((PyFunctionObject *)func) -> func_module)
#define PyFunction_GET_DEFAULTS(func) \
	(((PyFunctionObject *)func) -> func_defaults)
#define PyFunction_GET_CLOSURE(func) \
	(((PyFunctionObject *)func) -> func_closure)
#endif
#define PyFunction_GET_CODE(func) (PyFunction_GetCode((PyObject *)(func)))
#define PyFunction_GET_GLOBALS(func) (PyFunction_GetGlobals((PyObject *)(func)))
#define PyFunction_GET_MODULE(func) (PyFunction_GetModule((PyObject *)(func)))
#define PyFunction_GET_DEFAULTS(func) (PyFunction_GetDefaults((PyObject *)(func)))
#define PyFunction_GET_CLOSURE(func) (PyFunction_GetClosure((PyObject *)(func)))

// Pyston change: not a static object any more
#if 0
/* The classmethod and staticmethod types lives here, too */
PyAPI_DATA(PyTypeObject) PyClassMethod_Type;
PyAPI_DATA(PyTypeObject) PyStaticMethod_Type;
#endif
PyAPI_DATA(PyTypeObject*) classmethod_cls;
#define PyClassMethod_Type (*classmethod_cls)
PyAPI_DATA(PyTypeObject*) staticmethod_cls;
#define PyStaticMethod_Type (*staticmethod_cls)

PyAPI_FUNC(PyObject *) PyClassMethod_New(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyStaticMethod_New(PyObject *) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_FUNCOBJECT_H */
