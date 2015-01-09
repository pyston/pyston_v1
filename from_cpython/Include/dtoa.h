// PYSTON_NOEXCEPT This file is originally from CPython 2.7, with modifications for Pyston

#ifndef PY_NO_SHORT_FLOAT_REPR
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(double) _Py_dg_strtod(const char *str, char **ptr) PYSTON_NOEXCEPT;
PyAPI_FUNC(char *) _Py_dg_dtoa(double d, int mode, int ndigits,
                        int *decpt, int *sign, char **rve) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _Py_dg_freedtoa(char *s) PYSTON_NOEXCEPT;


#ifdef __cplusplus
}
#endif
#endif
