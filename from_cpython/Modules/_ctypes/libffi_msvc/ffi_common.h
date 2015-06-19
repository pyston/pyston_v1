/* -----------------------------------------------------------------------
   ffi_common.h - Copyright (c) 1996  Red Hat, Inc.

   Common internal definitions and macros. Only necessary for building
   libffi.
   ----------------------------------------------------------------------- */

#ifndef FFI_COMMON_H
#define FFI_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <fficonfig.h>
#include <malloc.h>

/* Check for the existence of memcpy. */
#if STDC_HEADERS
# include <string.h>
#else
# ifndef HAVE_MEMCPY
#  define memcpy(d, s, n) bcopy ((s), (d), (n))
# endif
#endif

#if defined(FFI_DEBUG) 
#include <stdio.h>
#endif

#ifdef FFI_DEBUG
/*@exits@*/ void ffi_assert(/*@temp@*/ char *expr, /*@temp@*/ char *file, int line);
void ffi_stop_here(void);
void ffi_type_test(/*@temp@*/ /*@out@*/ ffi_type *a, /*@temp@*/ char *file, int line);

#define FFI_ASSERT(x) ((x) ? (void)0 : ffi_assert(#x, __FILE__,__LINE__))
#define FFI_ASSERT_AT(x, f, l) ((x) ? 0 : ffi_assert(#x, (f), (l)))
#define FFI_ASSERT_VALID_TYPE(x) ffi_type_test (x, __FILE__, __LINE__)
#else
#define FFI_ASSERT(x) 
#define FFI_ASSERT_AT(x, f, l)
#define FFI_ASSERT_VALID_TYPE(x)
#endif

#define ALIGN(v, a)  (((((size_t) (v))-1) | ((a)-1))+1)

/* Perform machine dependent cif processing */
ffi_status ffi_prep_cif_machdep(ffi_cif *cif);

/* Extended cif, used in callback from assembly routine */
typedef struct
{
  /*@dependent@*/ ffi_cif *cif;
  /*@dependent@*/ void *rvalue;
  /*@dependent@*/ void **avalue;
} extended_cif;

/* Terse sized type definitions.  */
typedef unsigned int UINT8  __attribute__((__mode__(__QI__)));
typedef signed int   SINT8  __attribute__((__mode__(__QI__)));
typedef unsigned int UINT16 __attribute__((__mode__(__HI__)));
typedef signed int   SINT16 __attribute__((__mode__(__HI__)));
typedef unsigned int UINT32 __attribute__((__mode__(__SI__)));
typedef signed int   SINT32 __attribute__((__mode__(__SI__)));
typedef unsigned int UINT64 __attribute__((__mode__(__DI__)));
typedef signed int   SINT64 __attribute__((__mode__(__DI__)));

typedef float FLOAT32;


#ifdef __cplusplus
}
#endif

#endif


