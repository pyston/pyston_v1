// This file is originally from CPython 2.7, with modifications for Pyston

/* String (str/bytes) object interface */

#ifndef Py_STRINGOBJECT_H
#define Py_STRINGOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

/*
Type PyStringObject represents a character string.  An extra zero byte is
reserved at the end to ensure it is zero-terminated, but a size is
present so strings with null bytes in them can be represented.  This
is an immutable object type.

There are functions to create new string objects, to test
an object for string-ness, and to get the
string value.  The latter function returns a null pointer
if the object is not of the proper type.
There is a variant that takes an explicit size as well as a
variant that assumes a zero-terminated string.  Note that none of the
functions should be applied to nil objects.
*/

/* Caching the hash (ob_shash) saves recalculation of a string's hash value.
   Interning strings (ob_sstate) tries to ensure that only one string
   object with a given value exists, so equality tests can be one pointer
   comparison.  This is generally restricted to strings that "look like"
   Python identifiers, although the intern() builtin can be used to force
   interning of any string.
   Together, these sped the interpreter by up to 20%. */

// Pyston change: comment this out since this is not the format we're using
#if 0
typedef struct {
    PyObject_VAR_HEAD
    long ob_shash;
    int ob_sstate;
    char ob_sval[1];

    /* Invariants:
     *     ob_sval contains space for 'ob_size+1' elements.
     *     ob_sval[ob_size] == 0.
     *     ob_shash is the hash of the string or -1 if not computed yet.
     *     ob_sstate != 0 iff the string object is in stringobject.c's
     *       'interned' dictionary; in this case the two references
     *       from 'interned' to this object are *not counted* in ob_refcnt.
     */
} PyStringObject;
#endif
struct _PyStringObject;
typedef struct _PyStringObject PyStringObject;

#define SSTATE_NOT_INTERNED 0
#define SSTATE_INTERNED_MORTAL 1
#define SSTATE_INTERNED_IMMORTAL 2

// Pyston change: these are no longer a static object
PyAPI_DATA(PyTypeObject*) basestring_cls;
#define PyBaseString_Type (*basestring_cls)
PyAPI_DATA(PyTypeObject*) str_cls;
#define PyString_Type (*str_cls)

#define PyString_Check(op) \
                 PyType_FastSubclass(Py_TYPE(op), Py_TPFLAGS_STRING_SUBCLASS)
#define PyString_CheckExact(op) (Py_TYPE(op) == &PyString_Type)

PyAPI_FUNC(PyObject *) PyString_FromStringAndSize(const char *, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyString_FromString(const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyString_FromFormatV(const char*, va_list)
				PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 1, 0)));
PyAPI_FUNC(PyObject *) PyString_FromFormat(const char*, ...)
				PYSTON_NOEXCEPT Py_GCC_ATTRIBUTE((format(printf, 1, 2)));
PyAPI_FUNC(Py_ssize_t) PyString_Size(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(char *) PyString_AsString(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyString_Repr(PyObject *, int) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyString_Concat(PyObject **, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyString_ConcatAndDel(PyObject **, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyString_Resize(PyObject **, Py_ssize_t) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) _PyString_Eq(PyObject *, PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyString_Format(PyObject *, PyObject *) PYSTON_NOEXCEPT;
// Pyston change: added const
PyAPI_FUNC(PyObject *) _PyString_FormatLong(PyObject*, int, int,
						  int, const char**, int*) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyString_DecodeEscape(const char *, Py_ssize_t, 
						   const char *, Py_ssize_t,
						   const char *) PYSTON_NOEXCEPT;

PyAPI_FUNC(void) PyString_InternInPlace(PyObject **) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyString_InternImmortal(PyObject **) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyString_InternFromString(const char *) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) _Py_ReleaseInternedStrings(void) PYSTON_NOEXCEPT;

// Pyston addition:
PyAPI_FUNC(char) PyString_GetItem(PyObject *, Py_ssize_t) PYSTON_NOEXCEPT;

/* Use only if you know it's a string */
// Pyston changes: these aren't direct macros any more [they potentially could be though]
//#define PyString_CHECK_INTERNED(op) (((PyStringObject *)(op))->ob_sstate)
PyAPI_FUNC(int) _PyString_CheckInterned(PyObject *) PYSTON_NOEXCEPT;
#define PyString_CHECK_INTERNED(op) _PyString_CheckInterned((PyObject*)op)

/* Macro, trading safety for speed */
// Pyston changes: these aren't direct macros any more [they potentially could be though]
#define PyString_AS_STRING(op) PyString_AsString((PyObject*)op)
// Note: there are buggy extension modules (unicodedata.c) that rely on the fact that
// PyString_GET_SIZE does *not* have the same behavior as PyString_Size.  In particular,
// you can get away with calling PyString_GET_SIZE on a unicode object and getting the
// length of the unicode string, not the length of the bytes it encodes to in the default
// encoding.
// So, set up a different function for those callers to use.
//#define PyString_AS_STRING(op) (((PyStringObject *)(op))->ob_sval)
#define PyString_GET_SIZE(op)  Py_SIZE(op)

/* _PyString_Join(sep, x) is like sep.join(x).  sep must be PyStringObject*,
   x must be an iterable object. */
PyAPI_FUNC(PyObject *) _PyString_Join(PyObject *sep, PyObject *x) PYSTON_NOEXCEPT;

/* --- Generic Codecs ----------------------------------------------------- */

/* Create an object by decoding the encoded string s of the
   given size. */

PyAPI_FUNC(PyObject*) PyString_Decode(
    const char *s,              /* encoded string */
    Py_ssize_t size,            /* size of buffer */
    const char *encoding,       /* encoding */
    const char *errors          /* error handling */
    ) PYSTON_NOEXCEPT;

/* Encodes a char buffer of the given size and returns a 
   Python object. */

PyAPI_FUNC(PyObject*) PyString_Encode(
    const char *s,              /* string char buffer */
    Py_ssize_t size,            /* number of chars to encode */
    const char *encoding,       /* encoding */
    const char *errors          /* error handling */
    ) PYSTON_NOEXCEPT;

/* Encodes a string object and returns the result as Python 
   object. */

PyAPI_FUNC(PyObject*) PyString_AsEncodedObject(
    PyObject *str,	 	/* string object */
    const char *encoding,	/* encoding */
    const char *errors		/* error handling */
    ) PYSTON_NOEXCEPT;

/* Encodes a string object and returns the result as Python string
   object.   
   
   If the codec returns an Unicode object, the object is converted
   back to a string using the default encoding.

   DEPRECATED - use PyString_AsEncodedObject() instead. */

PyAPI_FUNC(PyObject*) PyString_AsEncodedString(
    PyObject *str,	 	/* string object */
    const char *encoding,	/* encoding */
    const char *errors		/* error handling */
    ) PYSTON_NOEXCEPT;

/* Decodes a string object and returns the result as Python 
   object. */

PyAPI_FUNC(PyObject*) PyString_AsDecodedObject(
    PyObject *str,	 	/* string object */
    const char *encoding,	/* encoding */
    const char *errors		/* error handling */
    ) PYSTON_NOEXCEPT;

/* Decodes a string object and returns the result as Python string
   object.  
   
   If the codec returns an Unicode object, the object is converted
   back to a string using the default encoding.

   DEPRECATED - use PyString_AsDecodedObject() instead. */

PyAPI_FUNC(PyObject*) PyString_AsDecodedString(
    PyObject *str,	 	/* string object */
    const char *encoding,	/* encoding */
    const char *errors		/* error handling */
    ) PYSTON_NOEXCEPT;

/* Provides access to the internal data buffer and size of a string
   object or the default encoded version of an Unicode object. Passing
   NULL as *len parameter will force the string buffer to be
   0-terminated (passing a string with embedded NULL characters will
   cause an exception).  */

PyAPI_FUNC(int) PyString_AsStringAndSize(
    register PyObject *obj,	/* string or Unicode object */
    register char **s,		/* pointer to buffer variable */
    register Py_ssize_t *len	/* pointer to length variable or NULL
				   (only possible for 0-terminated
				   strings) */
    ) PYSTON_NOEXCEPT;


/* Using the current locale, insert the thousands grouping
   into the string pointed to by buffer.  For the argument descriptions,
   see Objects/stringlib/localeutil.h */
PyAPI_FUNC(Py_ssize_t) _PyString_InsertThousandsGroupingLocale(char *buffer,
                                  Py_ssize_t n_buffer,
                                  char *digits,
                                  Py_ssize_t n_digits,
                                  Py_ssize_t min_width) PYSTON_NOEXCEPT;

/* Using explicit passed-in values, insert the thousands grouping
   into the string pointed to by buffer.  For the argument descriptions,
   see Objects/stringlib/localeutil.h */
PyAPI_FUNC(Py_ssize_t) _PyString_InsertThousandsGrouping(char *buffer,
                                  Py_ssize_t n_buffer,
                                  char *digits,
                                  Py_ssize_t n_digits,
                                  Py_ssize_t min_width,
                                  const char *grouping,
                                  const char *thousands_sep) PYSTON_NOEXCEPT;

/* Format the object based on the format_spec, as defined in PEP 3101
   (Advanced String Formatting). */
PyAPI_FUNC(PyObject *) _PyBytes_FormatAdvanced(PyObject *obj,
					       char *format_spec,
					       Py_ssize_t format_spec_len) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_STRINGOBJECT_H */
