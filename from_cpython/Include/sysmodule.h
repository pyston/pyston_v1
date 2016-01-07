// This file is originally from CPython 2.7, with modifications for Pyston

/* System module interface */

#ifndef Py_SYSMODULE_H
#define Py_SYSMODULE_H
#ifdef __cplusplus
extern "C" {
#endif

// Pyston change: changed most of these to const char*
PyAPI_FUNC(PyObject *) PySys_GetObject(const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySys_SetObject(const char *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(FILE *) PySys_GetFile(char *, FILE *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_SetArgv(int, char **) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_SetArgvEx(int, char **, int) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_SetPath(char *) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PySys_WriteStdout(const char *format, ...)
			PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 1, 2)));
PyAPI_FUNC(void) PySys_WriteStderr(const char *format, ...)
			PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 1, 2)));

PyAPI_FUNC(void) PySys_ResetWarnOptions(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PySys_AddWarnOption(char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PySys_HasWarnOptions(void) PYSTON_NOEXCEPT;

// Pyston change: add this API to get sys modules dict
PyAPI_FUNC(PyObject *) PySys_GetModulesDict(void) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_SYSMODULE_H */

