// This file is based off of CPython's pyport.h, though it has been heavily modified/cut down,
// largely due to the lack of a way to generate pyconfig.h at the moment.

#ifndef Py_PYPORT_H
#define Py_PYPORT_H

#include <stdint.h>

#ifdef __cplusplus
#if __cplusplus > 199711L
#define PYSTON_NOEXCEPT noexcept
#else
#define PYSTON_NOEXCEPT
#endif
#else
#define PYSTON_NOEXCEPT
#endif

// HACK: we should set this manually rather than cluing off of the C++ version
#ifdef __cplusplus
#if __cplusplus > 199711L
#define _PYSTON_API
#endif
#endif

#define Py_PROTO(x) x

// Pyston change: these are just hard-coded for now:
typedef ssize_t         Py_ssize_t;
#define Py_FORMAT_PARSETUPLE(func,p1,p2)
#define Py_GCC_ATTRIBUTE(x) __attribute__(x)



// Pyston change: the rest of these have just been copied from CPython's pyport.h, in an arbitrary order:

#if defined(__GNUC__) && __GNUC__ >= 3
#define Py_ALIGNED(x) __attribute__((aligned(x)))
#else
#define Py_ALIGNED(x)
#endif

#if defined(_MSC_VER)
#if defined(PY_LOCAL_AGGRESSIVE)
/* enable more aggressive optimization for visual studio */
#pragma optimize("agtw", on)
#endif
/* ignore warnings if the compiler decides not to inline a function */
#pragma warning(disable: 4710)
/* fastest possible local call under MSVC */
#define Py_LOCAL(type) static type __fastcall
#define Py_LOCAL_INLINE(type) static __inline type __fastcall
#elif defined(USE_INLINE)
#define Py_LOCAL(type) static type
#define Py_LOCAL_INLINE(type) static inline type
#else
#define Py_LOCAL(type) static type
#define Py_LOCAL_INLINE(type) static type
#endif

/*******************************
 * stat() and fstat() fiddling *
 *******************************/

/* We expect that stat and fstat exist on most systems.
 *  It's confirmed on Unix, Mac and Windows.
 *  If you don't have them, add
 *      #define DONT_HAVE_STAT
 * and/or
 *      #define DONT_HAVE_FSTAT
 * to your pyconfig.h. Python code beyond this should check HAVE_STAT and
 * HAVE_FSTAT instead.
 * Also
 *      #define HAVE_SYS_STAT_H
 * if <sys/stat.h> exists on your platform, and
 *      #define HAVE_STAT_H
 * if <stat.h> does.
 */
#ifndef DONT_HAVE_STAT
#define HAVE_STAT
#endif

#ifndef DONT_HAVE_FSTAT
#define HAVE_FSTAT
#endif

#ifdef RISCOS
#include <sys/types.h>
#include "unixstuff.h"
#endif

#ifdef HAVE_SYS_STAT_H
#if defined(PYOS_OS2) && defined(PYCC_GCC)
#include <sys/types.h>
#endif
#include <sys/stat.h>
#elif defined(HAVE_STAT_H)
#include <stat.h>
#endif


/*  The functions _Py_dg_strtod and _Py_dg_dtoa in Python/dtoa.c (which are
 *  required to support the short float repr introduced in Python 3.1) require
 *  that the floating-point unit that's being used for arithmetic operations
 *  on C doubles is set to use 53-bit precision.  It also requires that the
 *  FPU rounding mode is round-half-to-even, but that's less often an issue.
 *
 *  If your FPU isn't already set to 53-bit precision/round-half-to-even, and
 *  you want to make use of _Py_dg_strtod and _Py_dg_dtoa, then you should
 *
 *     #define HAVE_PY_SET_53BIT_PRECISION 1
 *
 *  and also give appropriate definitions for the following three macros:
 *
 *    _PY_SET_53BIT_PRECISION_START : store original FPU settings, and
 *        set FPU to 53-bit precision/round-half-to-even
 *    _PY_SET_53BIT_PRECISION_END : restore original FPU settings
 *    _PY_SET_53BIT_PRECISION_HEADER : any variable declarations needed to
 *        use the two macros above.
 *
 * The macros are designed to be used within a single C function: see
 * Python/pystrtod.c for an example of their use.
 */

/* get and set x87 control word for gcc/x86 */
#ifdef HAVE_GCC_ASM_FOR_X87
#define HAVE_PY_SET_53BIT_PRECISION 1
/* _Py_get/set_387controlword functions are defined in Python/pymath.c */
#define _Py_SET_53BIT_PRECISION_HEADER                          \
    unsigned short old_387controlword, new_387controlword
#define _Py_SET_53BIT_PRECISION_START                                   \
    do {                                                                \
        old_387controlword = _Py_get_387controlword();                  \
        new_387controlword = (old_387controlword & ~0x0f00) | 0x0200; \
        if (new_387controlword != old_387controlword)                   \
            _Py_set_387controlword(new_387controlword);                 \
    } while (0)
#define _Py_SET_53BIT_PRECISION_END                             \
    if (new_387controlword != old_387controlword)               \
        _Py_set_387controlword(old_387controlword)
#endif

/* get and set x87 control word for VisualStudio/x86 */
#if defined(_MSC_VER) && !defined(_WIN64) /* x87 not supported in 64-bit */
#define HAVE_PY_SET_53BIT_PRECISION 1
#define _Py_SET_53BIT_PRECISION_HEADER \
    unsigned int old_387controlword, new_387controlword, out_387controlword
/* We use the __control87_2 function to set only the x87 control word.
   The SSE control word is unaffected. */
#define _Py_SET_53BIT_PRECISION_START                                   \
    do {                                                                \
        __control87_2(0, 0, &old_387controlword, NULL);                 \
        new_387controlword =                                            \
          (old_387controlword & ~(_MCW_PC | _MCW_RC)) | (_PC_53 | _RC_NEAR); \
        if (new_387controlword != old_387controlword)                   \
            __control87_2(new_387controlword, _MCW_PC | _MCW_RC,        \
                          &out_387controlword, NULL);                   \
    } while (0)
#define _Py_SET_53BIT_PRECISION_END                                     \
    do {                                                                \
        if (new_387controlword != old_387controlword)                   \
            __control87_2(old_387controlword, _MCW_PC | _MCW_RC,        \
                          &out_387controlword, NULL);                   \
    } while (0)
#endif

/* default definitions are empty */
#ifndef HAVE_PY_SET_53BIT_PRECISION
#define _Py_SET_53BIT_PRECISION_HEADER
#define _Py_SET_53BIT_PRECISION_START
#define _Py_SET_53BIT_PRECISION_END
#endif

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

/* Largest positive value of type Py_ssize_t. */
#define PY_SSIZE_T_MAX ((Py_ssize_t)(((size_t)-1)>>1))
/* Smallest negative value of type Py_ssize_t. */
#define PY_SSIZE_T_MIN (-PY_SSIZE_T_MAX-1)

#ifdef uint32_t
#define HAVE_UINT32_T 1
#endif

#ifdef HAVE_UINT32_T
#ifndef PY_UINT32_T
#define PY_UINT32_T uint32_t
#endif
#endif

/* Macros for a 64-bit unsigned integer type; used for type 'twodigits' in the
 * long integer implementation, when 30-bit digits are enabled.
 */
#ifdef uint64_t
#define HAVE_UINT64_T 1
#endif

#ifdef HAVE_UINT64_T
#ifndef PY_UINT64_T
#define PY_UINT64_T uint64_t
#endif
#endif

/* Signed variants of the above */
#ifdef int32_t
#define HAVE_INT32_T 1
#endif

#ifdef HAVE_INT32_T
#ifndef PY_INT32_T
#define PY_INT32_T int32_t
#endif
#endif

#ifdef int64_t
#define HAVE_INT64_T 1
#endif

#ifdef HAVE_INT64_T
#ifndef PY_INT64_T
#define PY_INT64_T int64_t
#endif
#endif

/* uintptr_t is the C9X name for an unsigned integral type such that a
 * legitimate void* can be cast to uintptr_t and then back to void* again
 * without loss of information.  Similarly for intptr_t, wrt a signed
 * integral type.
 */
#ifdef HAVE_UINTPTR_T
typedef uintptr_t       Py_uintptr_t;
typedef intptr_t        Py_intptr_t;

#elif SIZEOF_VOID_P <= SIZEOF_INT
typedef unsigned int    Py_uintptr_t;
typedef int             Py_intptr_t;

#elif SIZEOF_VOID_P <= SIZEOF_LONG
typedef unsigned long   Py_uintptr_t;
typedef long            Py_intptr_t;

#elif defined(HAVE_LONG_LONG) && (SIZEOF_VOID_P <= SIZEOF_LONG_LONG)
typedef unsigned PY_LONG_LONG   Py_uintptr_t;
typedef PY_LONG_LONG            Py_intptr_t;

#else
#   error "Python needs a typedef for Py_uintptr_t in pyport.h."
#endif /* HAVE_UINTPTR_T */

#if defined(_MSC_VER)
#define Py_MEMCPY(target, source, length) do {                          \
        size_t i_, n_ = (length);                                       \
        char *t_ = (void*) (target);                                    \
        const char *s_ = (void*) (source);                              \
        if (n_ >= 16)                                                   \
            memcpy(t_, s_, n_);                                         \
        else                                                            \
            for (i_ = 0; i_ < n_; i_++)                                 \
                t_[i_] = s_[i_];                                        \
    } while (0)
#else
#define Py_MEMCPY memcpy
#endif

/********************************************
 * WRAPPER FOR <time.h> and/or <sys/time.h> *
 ********************************************/
#ifdef TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else /* !TIME_WITH_SYS_TIME */
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else /* !HAVE_SYS_TIME_H */
#include <time.h>
#endif /* !HAVE_SYS_TIME_H */
#endif /* !TIME_WITH_SYS_TIME */

#ifdef SIZE_MAX
#define PY_SIZE_MAX SIZE_MAX
#else
#define PY_SIZE_MAX ((size_t)-1)
#endif

/* Py_ARITHMETIC_RIGHT_SHIFT
 * C doesn't define whether a right-shift of a signed integer sign-extends
 * or zero-fills.  Here a macro to force sign extension:
 * Py_ARITHMETIC_RIGHT_SHIFT(TYPE, I, J)
 *    Return I >> J, forcing sign extension.  Arithmetically, return the
 *    floor of I/2**J.
 * Requirements:
 *    I should have signed integer type.  In the terminology of C99, this can
 *    be either one of the five standard signed integer types (signed char,
 *    short, int, long, long long) or an extended signed integer type.
 *    J is an integer >= 0 and strictly less than the number of bits in the
 *    type of I (because C doesn't define what happens for J outside that
 *    range either).
 *    TYPE used to specify the type of I, but is now ignored.  It's been left
 *    in for backwards compatibility with versions <= 2.6 or 3.0.
 * Caution:
 *    I may be evaluated more than once.
 */
#ifdef SIGNED_RIGHT_SHIFT_ZERO_FILLS
#define Py_ARITHMETIC_RIGHT_SHIFT(TYPE, I, J) \
    ((I) < 0 ? -1-((-1-(I)) >> (J)) : (I) >> (J))
#else
#define Py_ARITHMETIC_RIGHT_SHIFT(TYPE, I, J) ((I) >> (J))
#endif

#endif /* Py_PYPORT_H */

/* Py_ADJUST_ERANGE1(x)
 * Py_ADJUST_ERANGE2(x, y)
 * Set errno to 0 before calling a libm function, and invoke one of these
 * macros after, passing the function result(s) (Py_ADJUST_ERANGE2 is useful
 * for functions returning complex results).  This makes two kinds of
 * adjustments to errno:  (A) If it looks like the platform libm set
 * errno=ERANGE due to underflow, clear errno. (B) If it looks like the
 * platform libm overflowed but didn't set errno, force errno to ERANGE.  In
 * effect, we're trying to force a useful implementation of C89 errno
 * behavior.
 * Caution:
 *    This isn't reliable.  See Py_OVERFLOWED comments.
 *    X and Y may be evaluated more than once.
 */
#define Py_ADJUST_ERANGE1(X)                                            \
    do {                                                                \
        if (errno == 0) {                                               \
            if ((X) == Py_HUGE_VAL || (X) == -Py_HUGE_VAL)              \
                errno = ERANGE;                                         \
        }                                                               \
        else if (errno == ERANGE && (X) == 0.0)                         \
            errno = 0;                                                  \
    } while(0)

#define Py_ADJUST_ERANGE2(X, Y)                                         \
    do {                                                                \
        if ((X) == Py_HUGE_VAL || (X) == -Py_HUGE_VAL ||                \
            (Y) == Py_HUGE_VAL || (Y) == -Py_HUGE_VAL) {                \
                        if (errno == 0)                                 \
                                errno = ERANGE;                         \
        }                                                               \
        else if (errno == ERANGE)                                       \
            errno = 0;                                                  \
    } while(0)

/* If we can't guarantee 53-bit precision, don't use the code
   in Python/dtoa.c, but fall back to standard code.  This
   means that repr of a float will be long (17 sig digits).

   Realistically, there are two things that could go wrong:

   (1) doubles aren't IEEE 754 doubles, or
   (2) we're on x86 with the rounding precision set to 64-bits
       (extended precision), and we don't know how to change
       the rounding precision.
 */

#if !defined(DOUBLE_IS_LITTLE_ENDIAN_IEEE754) && \
    !defined(DOUBLE_IS_BIG_ENDIAN_IEEE754) && \
    !defined(DOUBLE_IS_ARM_MIXED_ENDIAN_IEEE754)
#define PY_NO_SHORT_FLOAT_REPR
#endif

/* double rounding is symptomatic of use of extended precision on x86.  If
   we're seeing double rounding, and we don't have any mechanism available for
   changing the FPU rounding precision, then don't use Python/dtoa.c. */
#if defined(X87_DOUBLE_ROUNDING) && !defined(HAVE_PY_SET_53BIT_PRECISION)
#define PY_NO_SHORT_FLOAT_REPR
#endif
