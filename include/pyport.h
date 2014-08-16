// This file is based off of CPython's pyport.h, though it has been heavily modified/cut down,
// largely due to the lack of a way to generate pyconfig.h at the moment.

#ifndef Py_PYPORT_H
#define Py_PYPORT_H

#include <stdint.h>

// Pyston change: these are just hard-coded for now:
#define SIZEOF_VOID_P 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_INT 4
typedef ssize_t         Py_ssize_t;
#define Py_FORMAT_PARSETUPLE(func,p1,p2)
#define Py_GCC_ATTRIBUTE(x) __attribute__(x)
#define HAVE_LONG_LONG 1
#define PY_LONG_LONG long long



// Pyston change: the rest of these have just been copied from CPython's pyport.h, in an arbitrary order:

/* Py_DEPRECATED(version)
 * Declare a variable, type, or function deprecated.
 * Usage:
 *    extern int old_var Py_DEPRECATED(2.3);
 *    typedef int T1 Py_DEPRECATED(2.4);
 *    extern int x() Py_DEPRECATED(2.5);
 */
#if defined(__GNUC__) && ((__GNUC__ >= 4) || \
              (__GNUC__ == 3) && (__GNUC_MINOR__ >= 1))
#define Py_DEPRECATED(VERSION_UNUSED) __attribute__((__deprecated__))
#else
#define Py_DEPRECATED(VERSION_UNUSED)
#endif

/* Declarations for symbol visibility.

  PyAPI_FUNC(type): Declares a public Python API function and return type
  PyAPI_DATA(type): Declares public Python data and its type
  PyMODINIT_FUNC:   A Python module init function.  If these functions are
                    inside the Python core, they are private to the core.
                    If in an extension module, it may be declared with
                    external linkage depending on the platform.

  As a number of platforms support/require "__declspec(dllimport/dllexport)",
  we support a HAVE_DECLSPEC_DLL macro to save duplication.
*/

/*
  All windows ports, except cygwin, are handled in PC/pyconfig.h.

  BeOS and cygwin are the only other autoconf platform requiring special
  linkage handling and both of these use __declspec().
*/
#if defined(__CYGWIN__) || defined(__BEOS__)
#       define HAVE_DECLSPEC_DLL
#endif

/* only get special linkage if built as shared or platform is Cygwin */
#if defined(Py_ENABLE_SHARED) || defined(__CYGWIN__)
#       if defined(HAVE_DECLSPEC_DLL)
#               ifdef Py_BUILD_CORE
#                       define PyAPI_FUNC(RTYPE) __declspec(dllexport) RTYPE
#                       define PyAPI_DATA(RTYPE) extern __declspec(dllexport) RTYPE
        /* module init functions inside the core need no external linkage */
        /* except for Cygwin to handle embedding (FIXME: BeOS too?) */
#                       if defined(__CYGWIN__)
#                               define PyMODINIT_FUNC __declspec(dllexport) void
#                       else /* __CYGWIN__ */
#                               define PyMODINIT_FUNC void
#                       endif /* __CYGWIN__ */
#               else /* Py_BUILD_CORE */
        /* Building an extension module, or an embedded situation */
        /* public Python functions and data are imported */
        /* Under Cygwin, auto-import functions to prevent compilation */
        /* failures similar to those described at the bottom of 4.1: */
        /* http://docs.python.org/extending/windows.html#a-cookbook-approach */
#                       if !defined(__CYGWIN__)
#                               define PyAPI_FUNC(RTYPE) __declspec(dllimport) RTYPE
#                       endif /* !__CYGWIN__ */
#                       define PyAPI_DATA(RTYPE) extern __declspec(dllimport) RTYPE
        /* module init functions outside the core must be exported */
#                       if defined(__cplusplus)
#                               define PyMODINIT_FUNC extern "C" __declspec(dllexport) void
#                       else /* __cplusplus */
#                               define PyMODINIT_FUNC __declspec(dllexport) void
#                       endif /* __cplusplus */
#               endif /* Py_BUILD_CORE */
#       endif /* HAVE_DECLSPEC */
#endif /* Py_ENABLE_SHARED */

/* If no external linkage macros defined by now, create defaults */
#ifndef PyAPI_FUNC
#       define PyAPI_FUNC(RTYPE) RTYPE
#endif
#ifndef PyAPI_DATA
#       define PyAPI_DATA(RTYPE) extern RTYPE
#endif
#ifndef PyMODINIT_FUNC
#       if defined(__cplusplus)
#               define PyMODINIT_FUNC extern "C" void
#       else /* __cplusplus */
#               define PyMODINIT_FUNC void
#       endif /* __cplusplus */
#endif

/* Deprecated DL_IMPORT and DL_EXPORT macros */
#if defined(Py_ENABLE_SHARED) && defined (HAVE_DECLSPEC_DLL)
#       if defined(Py_BUILD_CORE)
#               define DL_IMPORT(RTYPE) __declspec(dllexport) RTYPE
#               define DL_EXPORT(RTYPE) __declspec(dllexport) RTYPE
#       else
#               define DL_IMPORT(RTYPE) __declspec(dllimport) RTYPE
#               define DL_EXPORT(RTYPE) __declspec(dllexport) RTYPE
#       endif
#endif
#ifndef DL_EXPORT
#       define DL_EXPORT(RTYPE) RTYPE
#endif
#ifndef DL_IMPORT
#       define DL_IMPORT(RTYPE) RTYPE
#endif

#ifdef Py_DEBUG
#define Py_SAFE_DOWNCAST(VALUE, WIDE, NARROW) \
    (assert((WIDE)(NARROW)(VALUE) == (VALUE)), (NARROW)(VALUE))
#else
#define Py_SAFE_DOWNCAST(VALUE, WIDE, NARROW) (NARROW)(VALUE)
#endif

#ifndef Py_LL
#define Py_LL(x) x##LL
#endif

#ifndef Py_ULL
#define Py_ULL(x) Py_LL(x##U)
#endif

#endif /* Py_PYPORT_H */

