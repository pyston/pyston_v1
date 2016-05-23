// This file is originally from CPython 2.7, with modifications for Pyston

/* Module definition and import interface */

#ifndef Py_IMPORT_H
#define Py_IMPORT_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(long) PyImport_GetMagicNumber(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ExecCodeModule(const char *name, PyObject *co) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ExecCodeModuleEx(
    const char *name, PyObject *co, char *pathname) PYSTON_NOEXCEPT;
PyAPI_FUNC(BORROWED(PyObject *)) PyImport_GetModuleDict(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(BORROWED(PyObject *)) PyImport_AddModule(const char *name) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ImportModule(const char *name) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ImportModuleNoBlock(const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ImportModuleLevel(const char *name,
	PyObject *globals, PyObject *locals, PyObject *fromlist, int level) PYSTON_NOEXCEPT;

#define PyImport_ImportModuleEx(n, g, l, f) \
	PyImport_ImportModuleLevel(n, g, l, f, -1)

PyAPI_FUNC(PyObject *) PyImport_GetImporter(PyObject *path) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_Import(PyObject *name) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyImport_ReloadModule(PyObject *m) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyImport_Cleanup(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyImport_ImportFrozenModule(const char *) PYSTON_NOEXCEPT;

#ifdef WITH_THREAD
PyAPI_FUNC(void) _PyImport_AcquireLock(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_ReleaseLock(void) PYSTON_NOEXCEPT;
#else
#define _PyImport_AcquireLock()
#define _PyImport_ReleaseLock() 1
#endif

PyAPI_FUNC(struct filedescr *) _PyImport_FindModule(
	const char *, PyObject *, char *, size_t, FILE **, PyObject **) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyImport_IsScript(struct filedescr *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _PyImport_ReInitLock(void) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) _PyImport_FindExtension(char *, char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) _PyImport_FixupExtension(char *, char *) PYSTON_NOEXCEPT;

struct _inittab {
    const char *name;
    void (*initfunc)(void);
};

PyAPI_DATA(PyTypeObject) PyNullImporter_Type;
PyAPI_DATA(struct _inittab *) PyImport_Inittab;

PyAPI_FUNC(int) PyImport_AppendInittab(const char *name, void (*initfunc)(void)) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyImport_ExtendInittab(struct _inittab *newtab) PYSTON_NOEXCEPT;

struct _frozen {
    char *name;
    unsigned char *code;
    int size;
};

/* Embedding apps may change this pointer to point to their favorite
   collection of frozen modules: */

PyAPI_DATA(struct _frozen *) PyImport_FrozenModules;

#ifdef __cplusplus
}
#endif
#endif /* !Py_IMPORT_H */

