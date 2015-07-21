// This file is originally from CPython 2.7, with modifications for Pyston

/* Descriptors */
#ifndef Py_DESCROBJECT_H
#define Py_DESCROBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

typedef PyObject *(*getter)(PyObject *, void *);
typedef int (*setter)(PyObject *, PyObject *, void *);

typedef struct PyGetSetDef {
    const char *name;
    getter get;
    setter set;
    const char *doc;
    void *closure;
} PyGetSetDef;

typedef PyObject *(*wrapperfunc)(PyObject *self, PyObject *args,
                                 void *wrapped);

typedef PyObject *(*wrapperfunc_kwds)(PyObject *self, PyObject *args,
                                      void *wrapped, PyObject *kwds);

struct wrapperbase {
    char *name;
    int offset;
    void *function;
    wrapperfunc wrapper;
    char *doc;
    int flags;
    PyObject *name_strobj;
};

/* Flags for above struct */
#define PyWrapperFlag_KEYWORDS 1 /* wrapper function takes keyword args */
#define PyWrapperFlag_PYSTON   2 /* wrapper function is a Pyston function */
#define PyWrapperFlag_BOOL     4 /* not really a wrapper, just set a bool field */

/* Various kinds of descriptor objects */

// Pyston change: these are not our object layouts
#if 0
#define PyDescr_COMMON \
    PyObject_HEAD \
    PyTypeObject *d_type; \
    PyObject *d_name

typedef struct {
    PyDescr_COMMON;
} PyDescrObject;

typedef struct {
    PyDescr_COMMON;
    PyMethodDef *d_method;
} PyMethodDescrObject;

typedef struct {
    PyDescr_COMMON;
    struct PyMemberDef *d_member;
} PyMemberDescrObject;

typedef struct {
    PyDescr_COMMON;
    PyGetSetDef *d_getset;
} PyGetSetDescrObject;

typedef struct {
    PyDescr_COMMON;
    struct wrapperbase *d_base;
    void *d_wrapped; /* This can be any function pointer */
} PyWrapperDescrObject;
#endif
// (Pyston TODO: add opaque definitions of those names)

// Pyston change: these are not static objects any more
#if 0
PyAPI_DATA(PyTypeObject) PyWrapperDescr_Type;
PyAPI_DATA(PyTypeObject) PyDictProxy_Type;
PyAPI_DATA(PyTypeObject) PyGetSetDescr_Type;
PyAPI_DATA(PyTypeObject) PyMemberDescr_Type;
#else
PyAPI_DATA(PyTypeObject) PyDictProxy_Type;
#endif
// (Pyston TODO: add #defines to our names)
PyAPI_DATA(PyTypeObject*) wrapperdescr_cls;
#define PyWrapperDescr_Type (*wrapperdescr_cls)

PyAPI_FUNC(PyObject *) PyDescr_NewMethod(PyTypeObject *, PyMethodDef *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyDescr_NewClassMethod(PyTypeObject *, PyMethodDef *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyDescr_NewMember(PyTypeObject *,
                                               struct PyMemberDef *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyDescr_NewGetSet(PyTypeObject *,
                                               struct PyGetSetDef *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyDescr_NewWrapper(PyTypeObject *,
                                                struct wrapperbase *, void *) PYSTON_NOEXCEPT;
#define PyDescr_IsData(d) (Py_TYPE(d)->tp_descr_set != NULL)

PyAPI_FUNC(PyObject *) PyDictProxy_New(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyWrapper_New(PyObject *, PyObject *) PYSTON_NOEXCEPT;


// Pyston change: this is no longer a static object
//PyAPI_DATA(PyTypeObject) PyProperty_Type;

#ifdef __cplusplus
}
#endif
#endif /* !Py_DESCROBJECT_H */

