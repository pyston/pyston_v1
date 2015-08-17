// This file is originally from CPython 2.7, with modifications for Pyston

/* Tuple object interface */

#ifndef Py_STRUCTSEQ_H
#define Py_STRUCTSEQ_H
#ifdef __cplusplus
extern "C" {
#endif

// Pyston changes: change char* to const char*
typedef struct PyStructSequence_Field {
	const char *name;
	const char *doc;
} PyStructSequence_Field;

typedef struct PyStructSequence_Desc {
	const char *name;
	const char *doc;
	struct PyStructSequence_Field *fields;
	int n_in_sequence;
} PyStructSequence_Desc;

extern char* PyStructSequence_UnnamedField;

PyAPI_FUNC(void) PyStructSequence_InitType(PyTypeObject *type,
					   PyStructSequence_Desc *desc) PYSTON_NOEXCEPT;

// Pyston change, change PyTypeObject to struct _typeobject
PyAPI_FUNC(PyObject *) PyStructSequence_New(struct _typeobject* type) PYSTON_NOEXCEPT;

typedef struct {
	PyObject_VAR_HEAD
	PyObject *ob_item[1];
} PyStructSequence;

/* Macro, *only* to be used to fill in brand new objects */
#define PyStructSequence_SET_ITEM(op, i, v) \
	(((PyStructSequence *)(op))->ob_item[i] = v)

#ifdef __cplusplus
}
#endif
#endif /* !Py_STRUCTSEQ_H */
