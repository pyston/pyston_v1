// This file is originally from CPython 2.7, with modifications for Pyston


#ifndef Py_INTRCHECK_H
#define Py_INTRCHECK_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(int) PyOS_InterruptOccurred(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyOS_InitInterrupts(void) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyOS_AfterFork(void) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTRCHECK_H */
