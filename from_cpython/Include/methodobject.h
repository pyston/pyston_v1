// This file is originally from CPython 2.7, with modifications for Pyston

/* Method object interface */

#ifndef Py_METHODOBJECT_H
#define Py_METHODOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/* This is about the type 'builtin_function_or_method',
   not Python methods in user-defined classes.  See classobject.h
   for the latter. */

// Pyston change: this is no longer a static object
//PyAPI_DATA(PyTypeObject) PyCFunction_Type;
PyAPI_DATA(PyTypeObject*) capifunc_cls;
#define PyCFunction_Type (*capifunc_cls)

#define PyCFunction_Check(op) (Py_TYPE(op) == &PyCFunction_Type)

// Pyston change: expose our builtin_function_or_method type
PyAPI_DATA(PyTypeObject*) builtin_function_or_method_cls;
#define PyBuiltinFunction_Type (*builtin_function_or_method_cls)

typedef PyObject *(*PyCFunction)(PyObject *, PyObject *);
typedef PyObject *(*PyCFunctionWithKeywords)(PyObject *, PyObject *,
					     PyObject *);
typedef PyObject *(*PyNoArgsFunction)(PyObject *);

PyAPI_FUNC(PyCFunction) PyCFunction_GetFunction(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyCFunction_GetSelf(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyCFunction_GetFlags(PyObject *) PYSTON_NOEXCEPT;

// Pyston change: changed to function calls
#if 0
/* Macros for direct access to these values. Type checks are *not*
   done, so use with care. */
#define PyCFunction_GET_FUNCTION(func) \
        (((PyCFunctionObject *)func) -> m_ml -> ml_meth)
#define PyCFunction_GET_SELF(func) \
	(((PyCFunctionObject *)func) -> m_self)
#define PyCFunction_GET_FLAGS(func) \
	(((PyCFunctionObject *)func) -> m_ml -> ml_flags)
#endif
#define PyCFunction_GET_FUNCTION(func) \
        PyCFunction_GetFunction((PyObject*)(func))
#define PyCFunction_GET_SELF(func) \
        PyCFunction_GetSelf((PyObject*)(func))
#define PyCFunction_GET_FLAGS(func) \
        PyCFunction_GetFlags((PyObject*)(func))
PyAPI_FUNC(PyObject *) PyCFunction_Call(PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;

struct PyMethodDef {
    const char	*ml_name;	/* The name of the built-in function/method */
    PyCFunction  ml_meth;	/* The C function that implements it */
    int		 ml_flags;	/* Combination of METH_xxx flags, which mostly
				   describe the args expected by the C func */
    const char	*ml_doc;	/* The __doc__ attribute, or NULL */
};
typedef struct PyMethodDef PyMethodDef;

PyAPI_FUNC(PyObject *) Py_FindMethod(PyMethodDef[], PyObject *, const char *) PYSTON_NOEXCEPT;

#define PyCFunction_New(ML, SELF) PyCFunction_NewEx((ML), (SELF), NULL)
PyAPI_FUNC(PyObject *) PyCFunction_NewEx(PyMethodDef *, PyObject *, 
					 PyObject *) PYSTON_NOEXCEPT;

/* Flag passed to newmethodobject */
#define METH_OLDARGS  0x0000
#define METH_VARARGS  0x0001
#define METH_KEYWORDS 0x0002
/* METH_NOARGS and METH_O must not be combined with the flags above. */
#define METH_NOARGS   0x0004
#define METH_O        0x0008

/* METH_CLASS and METH_STATIC are a little different; these control
   the construction of methods for a class.  These cannot be used for
   functions in modules. */
#define METH_CLASS    0x0010
#define METH_STATIC   0x0020

/* METH_COEXIST allows a method to be entered eventhough a slot has
   already filled the entry.  When defined, the flag allows a separate
   method, "__contains__" for example, to coexist with a defined 
   slot like sq_contains. */

#define METH_COEXIST   0x0040

/* Pyston additions: */
#define METH_O2        0x0080
#define METH_O3        (METH_O | METH_O2)
// The number of defaults:
#define METH_D1        0x0200
#define METH_D2        0x0400
#define METH_D3        (METH_D1 | METH_D2)

typedef struct PyMethodChain {
    PyMethodDef *methods;		/* Methods of this type */
    struct PyMethodChain *link;	/* NULL or base type */
} PyMethodChain;

PyAPI_FUNC(PyObject *) Py_FindMethodInChain(PyMethodChain *, PyObject *,
                                            const char *) PYSTON_NOEXCEPT;

typedef struct {
    PyObject_HEAD
    PyMethodDef *m_ml; /* Description of the C function to call */
    PyObject    *m_self; /* Passed as 'self' arg to the C func, can be NULL */
    PyObject    *m_module; /* The __module__ attribute, can be anything */
} PyCFunctionObject;

PyAPI_FUNC(int) PyCFunction_ClearFreeList(void) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_METHODOBJECT_H */
