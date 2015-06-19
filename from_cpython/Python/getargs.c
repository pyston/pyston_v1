// This file is originally from CPython 2.7, with modifications for Pyston

/* New getargs implementation */

#include "Python.h"

#include <ctype.h>


#ifdef __cplusplus
extern "C" {
#endif
int PyArg_Parse(PyObject *, const char *, ...);
int PyArg_ParseTuple(PyObject *, const char *, ...);
int PyArg_VaParse(PyObject *, const char *, va_list);

int PyArg_ParseTupleAndKeywords(PyObject *, PyObject *,
                                const char *, char **, ...);
int PyArg_VaParseTupleAndKeywords(PyObject *, PyObject *,
                                const char *, char **, va_list);

#ifdef HAVE_DECLSPEC_DLL
/* Export functions */
PyAPI_FUNC(int) _PyArg_Parse_SizeT(PyObject *, char *, ...);
PyAPI_FUNC(int) _PyArg_ParseTuple_SizeT(PyObject *, char *, ...);
PyAPI_FUNC(int) _PyArg_ParseTupleAndKeywords_SizeT(PyObject *, PyObject *,
                                                  const char *, char **, ...);
PyAPI_FUNC(PyObject *) _Py_BuildValue_SizeT(const char *, ...);
PyAPI_FUNC(int) _PyArg_VaParse_SizeT(PyObject *, char *, va_list);
PyAPI_FUNC(int) _PyArg_VaParseTupleAndKeywords_SizeT(PyObject *, PyObject *,
                                              const char *, char **, va_list);
#endif

#define FLAG_COMPAT 1
#define FLAG_SIZE_T 2


/* Forward */
static int vgetargs1(PyObject *, const char *, va_list *, int);
static void seterror(int, const char *, int *, const char *, const char *);
static char *convertitem(PyObject *, const char **, va_list *, int, int *,
                         char *, size_t, PyObject **);
static char *converttuple(PyObject *, const char **, va_list *, int,
                          int *, char *, size_t, int, PyObject **);
static char *convertsimple(PyObject *, const char **, va_list *, int, char *,
                           size_t, PyObject **);
static Py_ssize_t convertbuffer(PyObject *, void **p, char **);
static int getbuffer(PyObject *, Py_buffer *, char**);

static int vgetargskeywords(PyObject *, PyObject *,
                            const char *, char **, va_list *, int);
static char *skipitem(const char **, va_list *, int);

int
PyArg_Parse(PyObject *args, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = vgetargs1(args, format, &va, FLAG_COMPAT);
    va_end(va);
    return retval;
}

int
_PyArg_Parse_SizeT(PyObject *args, char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = vgetargs1(args, format, &va, FLAG_COMPAT|FLAG_SIZE_T);
    va_end(va);
    return retval;
}


int
PyArg_ParseTuple(PyObject *args, const char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = vgetargs1(args, format, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseTuple_SizeT(PyObject *args, char *format, ...)
{
    int retval;
    va_list va;

    va_start(va, format);
    retval = vgetargs1(args, format, &va, FLAG_SIZE_T);
    va_end(va);
    return retval;
}


int
PyArg_VaParse(PyObject *args, const char *format, va_list va)
{
    va_list lva;

#ifdef VA_LIST_IS_ARRAY
    memcpy(lva, va, sizeof(va_list));
#else
#ifdef __va_copy
    __va_copy(lva, va);
#else
    lva = va;
#endif
#endif

    return vgetargs1(args, format, &lva, 0);
}

int
_PyArg_VaParse_SizeT(PyObject *args, char *format, va_list va)
{
    va_list lva;

#ifdef VA_LIST_IS_ARRAY
    memcpy(lva, va, sizeof(va_list));
#else
#ifdef __va_copy
    __va_copy(lva, va);
#else
    lva = va;
#endif
#endif

    return vgetargs1(args, format, &lva, FLAG_SIZE_T);
}


/* Handle cleanup of allocated memory in case of exception */

#define GETARGS_CAPSULE_NAME_CLEANUP_PTR "getargs.cleanup_ptr"
#define GETARGS_CAPSULE_NAME_CLEANUP_BUFFER "getargs.cleanup_buffer"

static void
cleanup_ptr(PyObject *self)
{
    void *ptr = PyCapsule_GetPointer(self, GETARGS_CAPSULE_NAME_CLEANUP_PTR);
    if (ptr) {
      PyMem_FREE(ptr);
    }
}

static void
cleanup_buffer(PyObject *self)
{
    Py_buffer *ptr = (Py_buffer *)PyCapsule_GetPointer(self, GETARGS_CAPSULE_NAME_CLEANUP_BUFFER);
    if (ptr) {
        PyBuffer_Release(ptr);
    }
}

static int
addcleanup(void *ptr, PyObject **freelist, PyCapsule_Destructor destr)
{
    PyObject *cobj;
    const char *name;

    if (!*freelist) {
        *freelist = PyList_New(0);
        if (!*freelist) {
            destr(ptr);
            return -1;
        }
    }

    if (destr == cleanup_ptr) {
        name = GETARGS_CAPSULE_NAME_CLEANUP_PTR;
    } else if (destr == cleanup_buffer) {
        name = GETARGS_CAPSULE_NAME_CLEANUP_BUFFER;
    } else {
        return -1;
    }
    cobj = PyCapsule_New(ptr, name, destr);
    if (!cobj) {
        destr(ptr);
        return -1;
    }
    if (PyList_Append(*freelist, cobj)) {
        Py_DECREF(cobj);
        return -1;
    }
    Py_DECREF(cobj);
    return 0;
}

static int
cleanreturn(int retval, PyObject *freelist)
{
    if (freelist && retval != 0) {
        /* We were successful, reset the destructors so that they
           don't get called. */
        Py_ssize_t len = PyList_GET_SIZE(freelist), i;
        for (i = 0; i < len; i++)
            PyCapsule_SetDestructor(PyList_GET_ITEM(freelist, i), NULL);
    }
    Py_XDECREF(freelist);
    return retval;
}


static int
vgetargs1(PyObject *args, const char *format, va_list *p_va, int flags)
{
    char msgbuf[256];
    int levels[32];
    const char *fname = NULL;
    const char *message = NULL;
    int min = -1;
    int max = 0;
    int level = 0;
    int endfmt = 0;
    const char *formatsave = format;
    Py_ssize_t i, len;
    char *msg;
    PyObject *freelist = NULL;
    int compat = flags & FLAG_COMPAT;

    assert(compat || (args != (PyObject*)NULL));
    flags = flags & ~FLAG_COMPAT;

    while (endfmt == 0) {
        int c = *format++;
        switch (c) {
        case '(':
            if (level == 0)
                max++;
            level++;
            if (level >= 30)
                Py_FatalError("too many tuple nesting levels "
                              "in argument format string");
            break;
        case ')':
            if (level == 0)
                Py_FatalError("excess ')' in getargs format");
            else
                level--;
            break;
        case '\0':
            endfmt = 1;
            break;
        case ':':
            fname = format;
            endfmt = 1;
            break;
        case ';':
            message = format;
            endfmt = 1;
            break;
        default:
            if (level == 0) {
                if (c == 'O')
                    max++;
                else if (isalpha(Py_CHARMASK(c))) {
                    if (c != 'e') /* skip encoded */
                        max++;
                } else if (c == '|')
                    min = max;
            }
            break;
        }
    }

    if (level != 0)
        Py_FatalError(/* '(' */ "missing ')' in getargs format");

    if (min < 0)
        min = max;

    format = formatsave;

    if (compat) {
        if (max == 0) {
            if (args == NULL)
                return 1;
            PyOS_snprintf(msgbuf, sizeof(msgbuf),
                          "%.200s%s takes no arguments",
                          fname==NULL ? "function" : fname,
                          fname==NULL ? "" : "()");
            PyErr_SetString(PyExc_TypeError, msgbuf);
            return 0;
        }
        else if (min == 1 && max == 1) {
            if (args == NULL) {
                PyOS_snprintf(msgbuf, sizeof(msgbuf),
                      "%.200s%s takes at least one argument",
                          fname==NULL ? "function" : fname,
                          fname==NULL ? "" : "()");
                PyErr_SetString(PyExc_TypeError, msgbuf);
                return 0;
            }
            msg = convertitem(args, &format, p_va, flags, levels,
                              msgbuf, sizeof(msgbuf), &freelist);
            if (msg == NULL)
                return cleanreturn(1, freelist);
            seterror(levels[0], msg, levels+1, fname, message);
            return cleanreturn(0, freelist);
        }
        else {
            PyErr_SetString(PyExc_SystemError,
                "old style getargs format uses new features");
            return 0;
        }
    }

    if (!PyTuple_Check(args)) {
        PyErr_SetString(PyExc_SystemError,
            "new style getargs format but argument is not a tuple");
        return 0;
    }

    len = PyTuple_GET_SIZE(args);

    if (len < min || max < len) {
        if (message == NULL) {
            PyOS_snprintf(msgbuf, sizeof(msgbuf),
                          "%.150s%s takes %s %d argument%s "
                          "(%ld given)",
                          fname==NULL ? "function" : fname,
                          fname==NULL ? "" : "()",
                          min==max ? "exactly"
                          : len < min ? "at least" : "at most",
                          len < min ? min : max,
                          (len < min ? min : max) == 1 ? "" : "s",
                          Py_SAFE_DOWNCAST(len, Py_ssize_t, long));
            message = msgbuf;
        }
        PyErr_SetString(PyExc_TypeError, message);
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (*format == '|')
            format++;
        msg = convertitem(PyTuple_GET_ITEM(args, i), &format, p_va,
                          flags, levels, msgbuf,
                          sizeof(msgbuf), &freelist);
        if (msg) {
            seterror(i+1, msg, levels, fname, msg);
            return cleanreturn(0, freelist);
        }
    }

    if (*format != '\0' && !isalpha(Py_CHARMASK(*format)) &&
        *format != '(' &&
        *format != '|' && *format != ':' && *format != ';') {
        PyErr_Format(PyExc_SystemError,
                     "bad format string: %.200s", formatsave);
        return cleanreturn(0, freelist);
    }

    return cleanreturn(1, freelist);
}



static void
seterror(int iarg, const char *msg, int *levels, const char *fname,
         const char *message)
{
    char buf[512];
    int i;
    char *p = buf;

    if (PyErr_Occurred())
        return;
    else if (message == NULL) {
        if (fname != NULL) {
            PyOS_snprintf(p, sizeof(buf), "%.200s() ", fname);
            p += strlen(p);
        }
        if (iarg != 0) {
            PyOS_snprintf(p, sizeof(buf) - (p - buf),
                          "argument %d", iarg);
            i = 0;
            p += strlen(p);
            while (levels[i] > 0 && i < 32 && (int)(p-buf) < 220) {
                PyOS_snprintf(p, sizeof(buf) - (p - buf),
                              ", item %d", levels[i]-1);
                p += strlen(p);
                i++;
            }
        }
        else {
            PyOS_snprintf(p, sizeof(buf) - (p - buf), "argument");
            p += strlen(p);
        }
        PyOS_snprintf(p, sizeof(buf) - (p - buf), " %.256s", msg);
        message = buf;
    }
    PyErr_SetString(PyExc_TypeError, message);
}


/* Convert a tuple argument.
   On entry, *p_format points to the character _after_ the opening '('.
   On successful exit, *p_format points to the closing ')'.
   If successful:
      *p_format and *p_va are updated,
      *levels and *msgbuf are untouched,
      and NULL is returned.
   If the argument is invalid:
      *p_format is unchanged,
      *p_va is undefined,
      *levels is a 0-terminated list of item numbers,
      *msgbuf contains an error message, whose format is:
     "must be <typename1>, not <typename2>", where:
        <typename1> is the name of the expected type, and
        <typename2> is the name of the actual type,
      and msgbuf is returned.
*/

static char *
converttuple(PyObject *arg, const char **p_format, va_list *p_va, int flags,
             int *levels, char *msgbuf, size_t bufsize, int toplevel,
             PyObject **freelist)
{
    int level = 0;
    int n = 0;
    const char *format = *p_format;
    int i;

    for (;;) {
        int c = *format++;
        if (c == '(') {
            if (level == 0)
                n++;
            level++;
        }
        else if (c == ')') {
            if (level == 0)
                break;
            level--;
        }
        else if (c == ':' || c == ';' || c == '\0')
            break;
        else if (level == 0 && isalpha(Py_CHARMASK(c)))
            n++;
    }

    if (!PySequence_Check(arg) || PyString_Check(arg)) {
        levels[0] = 0;
        PyOS_snprintf(msgbuf, bufsize,
                      toplevel ? "expected %d arguments, not %.50s" :
                      "must be %d-item sequence, not %.50s",
                  n,
                  arg == Py_None ? "None" : arg->ob_type->tp_name);
        return msgbuf;
    }

    if ((i = PySequence_Size(arg)) != n) {
        levels[0] = 0;
        PyOS_snprintf(msgbuf, bufsize,
                      toplevel ? "expected %d arguments, not %d" :
                     "must be sequence of length %d, not %d",
                  n, i);
        return msgbuf;
    }

    format = *p_format;
    for (i = 0; i < n; i++) {
        char *msg;
        PyObject *item;
        item = PySequence_GetItem(arg, i);
        if (item == NULL) {
            PyErr_Clear();
            levels[0] = i+1;
            levels[1] = 0;
            strncpy(msgbuf, "is not retrievable", bufsize);
            return msgbuf;
        }
        msg = convertitem(item, &format, p_va, flags, levels+1,
                          msgbuf, bufsize, freelist);
        /* PySequence_GetItem calls tp->sq_item, which INCREFs */
        Py_XDECREF(item);
        if (msg != NULL) {
            levels[0] = i+1;
            return msg;
        }
    }

    *p_format = format;
    return NULL;
}


/* Convert a single item. */

static char *
convertitem(PyObject *arg, const char **p_format, va_list *p_va, int flags,
            int *levels, char *msgbuf, size_t bufsize, PyObject **freelist)
{
    char *msg;
    const char *format = *p_format;

    if (*format == '(' /* ')' */) {
        format++;
        msg = converttuple(arg, &format, p_va, flags, levels, msgbuf,
                           bufsize, 0, freelist);
        if (msg == NULL)
            format++;
    }
    else {
        msg = convertsimple(arg, &format, p_va, flags,
                            msgbuf, bufsize, freelist);
        if (msg != NULL)
            levels[0] = 0;
    }
    if (msg == NULL)
        *p_format = format;
    return msg;
}



#define UNICODE_DEFAULT_ENCODING(arg) \
    _PyUnicode_AsDefaultEncodedString(arg, NULL)

/* Format an error message generated by convertsimple(). */

static char *
converterr(const char *expected, PyObject *arg, char *msgbuf, size_t bufsize)
{
    assert(expected != NULL);
    assert(arg != NULL);
    PyOS_snprintf(msgbuf, bufsize,
                  "must be %.50s, not %.50s", expected,
                  arg == Py_None ? "None" : arg->ob_type->tp_name);
    return msgbuf;
}

#define CONV_UNICODE "(unicode conversion error)"

/* explicitly check for float arguments when integers are expected.  For now
 * signal a warning.  Returns true if an exception was raised. */
static int
float_argument_warning(PyObject *arg)
{
    if (PyFloat_Check(arg) &&
        PyErr_Warn(PyExc_DeprecationWarning,
                   "integer argument expected, got float" ))
        return 1;
    else
        return 0;
}

/* explicitly check for float arguments when integers are expected.  Raises
   TypeError and returns true for float arguments. */
static int
float_argument_error(PyObject *arg)
{
    if (PyFloat_Check(arg)) {
        PyErr_SetString(PyExc_TypeError,
                        "integer argument expected, got float");
        return 1;
    }
    else
        return 0;
}

/* Convert a non-tuple argument.  Return NULL if conversion went OK,
   or a string with a message describing the failure.  The message is
   formatted as "must be <desired type>, not <actual type>".
   When failing, an exception may or may not have been raised.
   Don't call if a tuple is expected.

   When you add new format codes, please don't forget poor skipitem() below.
*/

static char *
convertsimple(PyObject *arg, const char **p_format, va_list *p_va, int flags,
              char *msgbuf, size_t bufsize, PyObject **freelist)
{
    /* For # codes */
#define FETCH_SIZE      int *q=NULL;Py_ssize_t *q2=NULL;\
    if (flags & FLAG_SIZE_T) q2=va_arg(*p_va, Py_ssize_t*); \
    else q=va_arg(*p_va, int*);
#define STORE_SIZE(s)   \
    if (flags & FLAG_SIZE_T) \
        *q2=s; \
    else { \
        if (INT_MAX < s) { \
            PyErr_SetString(PyExc_OverflowError, \
                "size does not fit in an int"); \
            return converterr("", arg, msgbuf, bufsize); \
        } \
        *q=s; \
    }
#define BUFFER_LEN      ((flags & FLAG_SIZE_T) ? *q2:*q)

    const char *format = *p_format;
    char c = *format++;
#ifdef Py_USING_UNICODE
    PyObject *uarg;
#endif

    switch (c) {

    case 'b': { /* unsigned byte -- very short int */
        char *p = va_arg(*p_va, char *);
        long ival;
        if (float_argument_error(arg))
            return converterr("integer<b>", arg, msgbuf, bufsize);
        ival = PyInt_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<b>", arg, msgbuf, bufsize);
        else if (ival < 0) {
            PyErr_SetString(PyExc_OverflowError,
            "unsigned byte integer is less than minimum");
            return converterr("integer<b>", arg, msgbuf, bufsize);
        }
        else if (ival > UCHAR_MAX) {
            PyErr_SetString(PyExc_OverflowError,
            "unsigned byte integer is greater than maximum");
            return converterr("integer<b>", arg, msgbuf, bufsize);
        }
        else
            *p = (unsigned char) ival;
        break;
    }

    case 'B': {/* byte sized bitfield - both signed and unsigned
                  values allowed */
        char *p = va_arg(*p_va, char *);
        long ival;
        if (float_argument_error(arg))
            return converterr("integer<B>", arg, msgbuf, bufsize);
        ival = PyInt_AsUnsignedLongMask(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<B>", arg, msgbuf, bufsize);
        else
            *p = (unsigned char) ival;
        break;
    }

    case 'h': {/* signed short int */
        short *p = va_arg(*p_va, short *);
        long ival;
        if (float_argument_error(arg))
            return converterr("integer<h>", arg, msgbuf, bufsize);
        ival = PyInt_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<h>", arg, msgbuf, bufsize);
        else if (ival < SHRT_MIN) {
            PyErr_SetString(PyExc_OverflowError,
            "signed short integer is less than minimum");
            return converterr("integer<h>", arg, msgbuf, bufsize);
        }
        else if (ival > SHRT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
            "signed short integer is greater than maximum");
            return converterr("integer<h>", arg, msgbuf, bufsize);
        }
        else
            *p = (short) ival;
        break;
    }

    case 'H': { /* short int sized bitfield, both signed and
                   unsigned allowed */
        unsigned short *p = va_arg(*p_va, unsigned short *);
        long ival;
        if (float_argument_error(arg))
            return converterr("integer<H>", arg, msgbuf, bufsize);
        ival = PyInt_AsUnsignedLongMask(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<H>", arg, msgbuf, bufsize);
        else
            *p = (unsigned short) ival;
        break;
    }

    case 'i': {/* signed int */
        int *p = va_arg(*p_va, int *);
        long ival;
        if (float_argument_error(arg))
            return converterr("integer<i>", arg, msgbuf, bufsize);
        ival = PyInt_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<i>", arg, msgbuf, bufsize);
        else if (ival > INT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                "signed integer is greater than maximum");
            return converterr("integer<i>", arg, msgbuf, bufsize);
        }
        else if (ival < INT_MIN) {
            PyErr_SetString(PyExc_OverflowError,
                "signed integer is less than minimum");
            return converterr("integer<i>", arg, msgbuf, bufsize);
        }
        else
            *p = ival;
        break;
    }

    case 'I': { /* int sized bitfield, both signed and
                   unsigned allowed */
        unsigned int *p = va_arg(*p_va, unsigned int *);
        unsigned int ival;
        if (float_argument_error(arg))
            return converterr("integer<I>", arg, msgbuf, bufsize);
        ival = (unsigned int)PyInt_AsUnsignedLongMask(arg);
        if (ival == (unsigned int)-1 && PyErr_Occurred())
            return converterr("integer<I>", arg, msgbuf, bufsize);
        else
            *p = ival;
        break;
    }

    case 'n': /* Py_ssize_t */
#if SIZEOF_SIZE_T != SIZEOF_LONG
    {
        Py_ssize_t *p = va_arg(*p_va, Py_ssize_t *);
        Py_ssize_t ival;
        if (float_argument_error(arg))
            return converterr("integer<n>", arg, msgbuf, bufsize);
        ival = PyInt_AsSsize_t(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<n>", arg, msgbuf, bufsize);
        *p = ival;
        break;
    }
#endif
    /* Fall through from 'n' to 'l' if Py_ssize_t is int */
    case 'l': {/* long int */
        long *p = va_arg(*p_va, long *);
        long ival;
        if (float_argument_error(arg))
            return converterr("integer<l>", arg, msgbuf, bufsize);
        ival = PyInt_AsLong(arg);
        if (ival == -1 && PyErr_Occurred())
            return converterr("integer<l>", arg, msgbuf, bufsize);
        else
            *p = ival;
        break;
    }

    case 'k': { /* long sized bitfield */
        unsigned long *p = va_arg(*p_va, unsigned long *);
        unsigned long ival;
        if (PyInt_Check(arg))
            ival = PyInt_AsUnsignedLongMask(arg);
        else if (PyLong_Check(arg))
            ival = PyLong_AsUnsignedLongMask(arg);
        else
            return converterr("integer<k>", arg, msgbuf, bufsize);
        *p = ival;
        break;
    }

#ifdef HAVE_LONG_LONG
    case 'L': {/* PY_LONG_LONG */
        PY_LONG_LONG *p = va_arg( *p_va, PY_LONG_LONG * );
        PY_LONG_LONG ival;
        if (float_argument_warning(arg))
            return converterr("long<L>", arg, msgbuf, bufsize);
        ival = PyLong_AsLongLong(arg);
        if (ival == (PY_LONG_LONG)-1 && PyErr_Occurred() ) {
            return converterr("long<L>", arg, msgbuf, bufsize);
        } else {
            *p = ival;
        }
        break;
    }

    case 'K': { /* long long sized bitfield */
        unsigned PY_LONG_LONG *p = va_arg(*p_va, unsigned PY_LONG_LONG *);
        unsigned PY_LONG_LONG ival;
        if (PyInt_Check(arg))
            ival = PyInt_AsUnsignedLongMask(arg);
        else if (PyLong_Check(arg))
            ival = PyLong_AsUnsignedLongLongMask(arg);
        else
            return converterr("integer<K>", arg, msgbuf, bufsize);
        *p = ival;
        break;
    }
#endif

    case 'f': {/* float */
        float *p = va_arg(*p_va, float *);
        double dval = PyFloat_AsDouble(arg);
        if (PyErr_Occurred())
            return converterr("float<f>", arg, msgbuf, bufsize);
        else
            *p = (float) dval;
        break;
    }

    case 'd': {/* double */
        double *p = va_arg(*p_va, double *);
        double dval = PyFloat_AsDouble(arg);
        if (PyErr_Occurred())
            return converterr("float<d>", arg, msgbuf, bufsize);
        else
            *p = dval;
        break;
    }

#ifndef WITHOUT_COMPLEX
    case 'D': {/* complex double */
        Py_complex *p = va_arg(*p_va, Py_complex *);
        Py_complex cval;
        cval = PyComplex_AsCComplex(arg);
        if (PyErr_Occurred())
            return converterr("complex<D>", arg, msgbuf, bufsize);
        else
            *p = cval;
        break;
    }
#endif /* WITHOUT_COMPLEX */

    case 'c': {/* char */
        char *p = va_arg(*p_va, char *);
        if (PyString_Check(arg) && PyString_Size(arg) == 1)
            *p = PyString_AS_STRING(arg)[0];
        else
            return converterr("char", arg, msgbuf, bufsize);
        break;
    }

    case 's': {/* string */
        if (*format == '*') {
            Py_buffer *p = (Py_buffer *)va_arg(*p_va, Py_buffer *);

            if (PyString_Check(arg)) {
                PyBuffer_FillInfo(p, arg,
                                  PyString_AS_STRING(arg), PyString_GET_SIZE(arg),
                                  1, 0);
            }
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(arg)) {
                uarg = UNICODE_DEFAULT_ENCODING(arg);
                if (uarg == NULL)
                    return converterr(CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                PyBuffer_FillInfo(p, arg,
                                  PyString_AS_STRING(uarg), PyString_GET_SIZE(uarg),
                                  1, 0);
            }
#endif
            else { /* any buffer-like object */
                char *buf;
                if (getbuffer(arg, p, &buf) < 0)
                    return converterr(buf, arg, msgbuf, bufsize);
            }
            if (addcleanup(p, freelist, cleanup_buffer)) {
                return converterr(
                    "(cleanup problem)",
                    arg, msgbuf, bufsize);
            }
            format++;
        } else if (*format == '#') {
            void **p = (void **)va_arg(*p_va, char **);
            FETCH_SIZE;

            if (PyString_Check(arg)) {
                *p = PyString_AS_STRING(arg);
                STORE_SIZE(PyString_GET_SIZE(arg));
            }
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(arg)) {
                uarg = UNICODE_DEFAULT_ENCODING(arg);
                if (uarg == NULL)
                    return converterr(CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                *p = PyString_AS_STRING(uarg);
                STORE_SIZE(PyString_GET_SIZE(uarg));
            }
#endif
            else { /* any buffer-like object */
                char *buf;
                Py_ssize_t count = convertbuffer(arg, p, &buf);
                if (count < 0)
                    return converterr(buf, arg, msgbuf, bufsize);
                STORE_SIZE(count);
            }
            format++;
        } else {
            char **p = va_arg(*p_va, char **);

            if (PyString_Check(arg))
                *p = PyString_AS_STRING(arg);
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(arg)) {
                uarg = UNICODE_DEFAULT_ENCODING(arg);
                if (uarg == NULL)
                    return converterr(CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                *p = PyString_AS_STRING(uarg);
            }
#endif
            else
                return converterr("string", arg, msgbuf, bufsize);
            if ((Py_ssize_t)strlen(*p) != PyString_Size(arg))
                return converterr("string without null bytes",
                                  arg, msgbuf, bufsize);
        }
        break;
    }

    case 'z': {/* string, may be NULL (None) */
        if (*format == '*') {
            Py_buffer *p = (Py_buffer *)va_arg(*p_va, Py_buffer *);

            if (arg == Py_None)
                PyBuffer_FillInfo(p, NULL, NULL, 0, 1, 0);
            else if (PyString_Check(arg)) {
                PyBuffer_FillInfo(p, arg,
                                  PyString_AS_STRING(arg), PyString_GET_SIZE(arg),
                                  1, 0);
            }
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(arg)) {
                uarg = UNICODE_DEFAULT_ENCODING(arg);
                if (uarg == NULL)
                    return converterr(CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                PyBuffer_FillInfo(p, arg,
                                  PyString_AS_STRING(uarg), PyString_GET_SIZE(uarg),
                                  1, 0);
            }
#endif
            else { /* any buffer-like object */
                char *buf;
                if (getbuffer(arg, p, &buf) < 0)
                    return converterr(buf, arg, msgbuf, bufsize);
            }
            if (addcleanup(p, freelist, cleanup_buffer)) {
                return converterr(
                    "(cleanup problem)",
                    arg, msgbuf, bufsize);
            }
            format++;
        } else if (*format == '#') { /* any buffer-like object */
            void **p = (void **)va_arg(*p_va, char **);
            FETCH_SIZE;

            if (arg == Py_None) {
                *p = 0;
                STORE_SIZE(0);
            }
            else if (PyString_Check(arg)) {
                *p = PyString_AS_STRING(arg);
                STORE_SIZE(PyString_GET_SIZE(arg));
            }
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(arg)) {
                uarg = UNICODE_DEFAULT_ENCODING(arg);
                if (uarg == NULL)
                    return converterr(CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                *p = PyString_AS_STRING(uarg);
                STORE_SIZE(PyString_GET_SIZE(uarg));
            }
#endif
            else { /* any buffer-like object */
                char *buf;
                Py_ssize_t count = convertbuffer(arg, p, &buf);
                if (count < 0)
                    return converterr(buf, arg, msgbuf, bufsize);
                STORE_SIZE(count);
            }
            format++;
        } else {
            char **p = va_arg(*p_va, char **);

            if (arg == Py_None)
                *p = 0;
            else if (PyString_Check(arg))
                *p = PyString_AS_STRING(arg);
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(arg)) {
                uarg = UNICODE_DEFAULT_ENCODING(arg);
                if (uarg == NULL)
                    return converterr(CONV_UNICODE,
                                      arg, msgbuf, bufsize);
                *p = PyString_AS_STRING(uarg);
            }
#endif
            else
                return converterr("string or None",
                                  arg, msgbuf, bufsize);
            if (*format == '#') {
                FETCH_SIZE;
                assert(0); /* XXX redundant with if-case */
                if (arg == Py_None) {
                    STORE_SIZE(0);
                } else {
                    STORE_SIZE(PyString_Size(arg));
                }
                format++;
            }
            else if (*p != NULL &&
                     (Py_ssize_t)strlen(*p) != PyString_Size(arg))
                return converterr(
                    "string without null bytes or None",
                    arg, msgbuf, bufsize);
        }
        break;
    }

    case 'e': {/* encoded string */
        char **buffer;
        const char *encoding;
        PyObject *s;
        Py_ssize_t size;
        int recode_strings;

        /* Get 'e' parameter: the encoding name */
        encoding = (const char *)va_arg(*p_va, const char *);
#ifdef Py_USING_UNICODE
        if (encoding == NULL)
            encoding = PyUnicode_GetDefaultEncoding();
#endif

        /* Get output buffer parameter:
           's' (recode all objects via Unicode) or
           't' (only recode non-string objects)
        */
        if (*format == 's')
            recode_strings = 1;
        else if (*format == 't')
            recode_strings = 0;
        else
            return converterr(
                "(unknown parser marker combination)",
                arg, msgbuf, bufsize);
        buffer = (char **)va_arg(*p_va, char **);
        format++;
        if (buffer == NULL)
            return converterr("(buffer is NULL)",
                              arg, msgbuf, bufsize);

        /* Encode object */
        if (!recode_strings && PyString_Check(arg)) {
            s = arg;
            Py_INCREF(s);
        }
        else {
#ifdef Py_USING_UNICODE
            PyObject *u;

            /* Convert object to Unicode */
            u = PyUnicode_FromObject(arg);
            if (u == NULL)
                return converterr(
                    "string or unicode or text buffer",
                    arg, msgbuf, bufsize);

            /* Encode object; use default error handling */
            s = PyUnicode_AsEncodedString(u,
                                          encoding,
                                          NULL);
            Py_DECREF(u);
            if (s == NULL)
                return converterr("(encoding failed)",
                                  arg, msgbuf, bufsize);
            if (!PyString_Check(s)) {
                Py_DECREF(s);
                return converterr(
                    "(encoder failed to return a string)",
                    arg, msgbuf, bufsize);
            }
#else
            return converterr("string<e>", arg, msgbuf, bufsize);
#endif
        }
        size = PyString_GET_SIZE(s);

        /* Write output; output is guaranteed to be 0-terminated */
        if (*format == '#') {
            /* Using buffer length parameter '#':

               - if *buffer is NULL, a new buffer of the
               needed size is allocated and the data
               copied into it; *buffer is updated to point
               to the new buffer; the caller is
               responsible for PyMem_Free()ing it after
               usage

               - if *buffer is not NULL, the data is
               copied to *buffer; *buffer_len has to be
               set to the size of the buffer on input;
               buffer overflow is signalled with an error;
               buffer has to provide enough room for the
               encoded string plus the trailing 0-byte

               - in both cases, *buffer_len is updated to
               the size of the buffer /excluding/ the
               trailing 0-byte

            */
            FETCH_SIZE;

            format++;
            if (q == NULL && q2 == NULL) {
                Py_DECREF(s);
                return converterr(
                    "(buffer_len is NULL)",
                    arg, msgbuf, bufsize);
            }
            if (*buffer == NULL) {
                *buffer = PyMem_NEW(char, size + 1);
                if (*buffer == NULL) {
                    Py_DECREF(s);
                    return converterr(
                        "(memory error)",
                        arg, msgbuf, bufsize);
                }
                if (addcleanup(*buffer, freelist, cleanup_ptr)) {
                    Py_DECREF(s);
                    return converterr(
                        "(cleanup problem)",
                        arg, msgbuf, bufsize);
                }
            } else {
                if (size + 1 > BUFFER_LEN) {
                    Py_DECREF(s);
                    return converterr(
                        "(buffer overflow)",
                        arg, msgbuf, bufsize);
                }
            }
            memcpy(*buffer,
                   PyString_AS_STRING(s),
                   size + 1);
            STORE_SIZE(size);
        } else {
            /* Using a 0-terminated buffer:

               - the encoded string has to be 0-terminated
               for this variant to work; if it is not, an
               error raised

               - a new buffer of the needed size is
               allocated and the data copied into it;
               *buffer is updated to point to the new
               buffer; the caller is responsible for
               PyMem_Free()ing it after usage

            */
            if ((Py_ssize_t)strlen(PyString_AS_STRING(s))
                                                    != size) {
                Py_DECREF(s);
                return converterr(
                    "encoded string without NULL bytes",
                    arg, msgbuf, bufsize);
            }
            *buffer = PyMem_NEW(char, size + 1);
            if (*buffer == NULL) {
                Py_DECREF(s);
                return converterr("(memory error)",
                                  arg, msgbuf, bufsize);
            }
            if (addcleanup(*buffer, freelist, cleanup_ptr)) {
                Py_DECREF(s);
                return converterr("(cleanup problem)",
                                arg, msgbuf, bufsize);
            }
            memcpy(*buffer,
                   PyString_AS_STRING(s),
                   size + 1);
        }
        Py_DECREF(s);
        break;
    }

#ifdef Py_USING_UNICODE
    case 'u': {/* raw unicode buffer (Py_UNICODE *) */
        if (*format == '#') { /* any buffer-like object */
            void **p = (void **)va_arg(*p_va, char **);
            FETCH_SIZE;
            if (PyUnicode_Check(arg)) {
                *p = PyUnicode_AS_UNICODE(arg);
                STORE_SIZE(PyUnicode_GET_SIZE(arg));
            }
            else {
                return converterr("cannot convert raw buffers",
                                  arg, msgbuf, bufsize);
            }
            format++;
        } else {
            Py_UNICODE **p = va_arg(*p_va, Py_UNICODE **);
            if (PyUnicode_Check(arg))
                *p = PyUnicode_AS_UNICODE(arg);
            else
                return converterr("unicode", arg, msgbuf, bufsize);
        }
        break;
    }
#endif

    case 'S': { /* string object */
        PyObject **p = va_arg(*p_va, PyObject **);
        if (PyString_Check(arg))
            *p = arg;
        else
            return converterr("string", arg, msgbuf, bufsize);
        break;
    }

#ifdef Py_USING_UNICODE
    case 'U': { /* Unicode object */
        PyObject **p = va_arg(*p_va, PyObject **);
        if (PyUnicode_Check(arg))
            *p = arg;
        else
            return converterr("unicode", arg, msgbuf, bufsize);
        break;
    }
#endif

    case 'O': { /* object */
        PyTypeObject *type;
        PyObject **p;
        if (*format == '!') {
            type = va_arg(*p_va, PyTypeObject*);
            p = va_arg(*p_va, PyObject **);
            format++;
            if (PyType_IsSubtype(arg->ob_type, type))
                *p = arg;
            else
                return converterr(type->tp_name, arg, msgbuf, bufsize);

        }
        else if (*format == '?') {
            inquiry pred = va_arg(*p_va, inquiry);
            p = va_arg(*p_va, PyObject **);
            format++;
            if ((*pred)(arg))
                *p = arg;
            else
                return converterr("(unspecified)",
                                  arg, msgbuf, bufsize);

        }
        else if (*format == '&') {
            typedef int (*converter)(PyObject *, void *);
            converter convert = va_arg(*p_va, converter);
            void *addr = va_arg(*p_va, void *);
            format++;
            if (! (*convert)(arg, addr))
                return converterr("(unspecified)",
                                  arg, msgbuf, bufsize);
        }
        else {
            p = va_arg(*p_va, PyObject **);
            *p = arg;
        }
        break;
    }


    case 'w': { /* memory buffer, read-write access */
        void **p = va_arg(*p_va, void **);
        void *res;
        PyBufferProcs *pb = arg->ob_type->tp_as_buffer;
        Py_ssize_t count;

        if (pb && pb->bf_releasebuffer && *format != '*')
            /* Buffer must be released, yet caller does not use
               the Py_buffer protocol. */
            return converterr("pinned buffer", arg, msgbuf, bufsize);

        if (pb && pb->bf_getbuffer && *format == '*') {
            /* Caller is interested in Py_buffer, and the object
               supports it directly. */
            format++;
            if (pb->bf_getbuffer(arg, (Py_buffer*)p, PyBUF_WRITABLE) < 0) {
                PyErr_Clear();
                return converterr("read-write buffer", arg, msgbuf, bufsize);
            }
            if (addcleanup(p, freelist, cleanup_buffer)) {
                return converterr(
                    "(cleanup problem)",
                    arg, msgbuf, bufsize);
            }
            if (!PyBuffer_IsContiguous((Py_buffer*)p, 'C'))
                return converterr("contiguous buffer", arg, msgbuf, bufsize);
            break;
        }

        if (pb == NULL ||
            pb->bf_getwritebuffer == NULL ||
            pb->bf_getsegcount == NULL)
            return converterr("read-write buffer", arg, msgbuf, bufsize);
        if ((*pb->bf_getsegcount)(arg, NULL) != 1)
            return converterr("single-segment read-write buffer",
                              arg, msgbuf, bufsize);
        if ((count = pb->bf_getwritebuffer(arg, 0, &res)) < 0)
            return converterr("(unspecified)", arg, msgbuf, bufsize);
        if (*format == '*') {
            PyBuffer_FillInfo((Py_buffer*)p, arg, res, count, 1, 0);
            format++;
        }
        else {
            *p = res;
            if (*format == '#') {
                FETCH_SIZE;
                STORE_SIZE(count);
                format++;
            }
        }
        break;
    }

    case 't': { /* 8-bit character buffer, read-only access */
        char **p = va_arg(*p_va, char **);
        PyBufferProcs *pb = arg->ob_type->tp_as_buffer;
        Py_ssize_t count;

        if (*format++ != '#')
            return converterr(
                "invalid use of 't' format character",
                arg, msgbuf, bufsize);
        if (!PyType_HasFeature(arg->ob_type,
                               Py_TPFLAGS_HAVE_GETCHARBUFFER) ||
            pb == NULL || pb->bf_getcharbuffer == NULL ||
            pb->bf_getsegcount == NULL)
            return converterr(
                "string or read-only character buffer",
                arg, msgbuf, bufsize);

        if (pb->bf_getsegcount(arg, NULL) != 1)
            return converterr(
                "string or single-segment read-only buffer",
                arg, msgbuf, bufsize);

        if (pb->bf_releasebuffer)
            return converterr(
                "string or pinned buffer",
                arg, msgbuf, bufsize);

        count = pb->bf_getcharbuffer(arg, 0, p);
        if (count < 0)
            return converterr("(unspecified)", arg, msgbuf, bufsize);
        {
            FETCH_SIZE;
            STORE_SIZE(count);
        }
        break;
    }

    default:
        return converterr("impossible<bad format char>", arg, msgbuf, bufsize);

    }

    *p_format = format;
    return NULL;
}

static Py_ssize_t
convertbuffer(PyObject *arg, void **p, char **errmsg)
{
    PyBufferProcs *pb = arg->ob_type->tp_as_buffer;
    Py_ssize_t count;
    if (pb == NULL ||
        pb->bf_getreadbuffer == NULL ||
        pb->bf_getsegcount == NULL ||
        pb->bf_releasebuffer != NULL) {
        *errmsg = "string or read-only buffer";
        return -1;
    }
    if ((*pb->bf_getsegcount)(arg, NULL) != 1) {
        *errmsg = "string or single-segment read-only buffer";
        return -1;
    }
    if ((count = (*pb->bf_getreadbuffer)(arg, 0, p)) < 0) {
        *errmsg = "(unspecified)";
    }
    return count;
}

static int
getbuffer(PyObject *arg, Py_buffer *view, char **errmsg)
{
    void *buf;
    Py_ssize_t count;
    PyBufferProcs *pb = arg->ob_type->tp_as_buffer;
    if (pb == NULL) {
        *errmsg = "string or buffer";
        return -1;
    }
    if (pb->bf_getbuffer) {
        if (pb->bf_getbuffer(arg, view, 0) < 0) {
            *errmsg = "convertible to a buffer";
            return -1;
        }
        if (!PyBuffer_IsContiguous(view, 'C')) {
            *errmsg = "contiguous buffer";
            return -1;
        }
        return 0;
    }

    count = convertbuffer(arg, &buf, errmsg);
    if (count < 0) {
        *errmsg = "convertible to a buffer";
        return count;
    }
    PyBuffer_FillInfo(view, arg, buf, count, 1, 0);
    return 0;
}

/* Support for keyword arguments donated by
   Geoff Philbrick <philbric@delphi.hks.com> */

/* Return false (0) for error, else true. */
int
PyArg_ParseTupleAndKeywords(PyObject *args,
                            PyObject *keywords,
                            const char *format,
                            char **kwlist, ...)
{
    int retval;
    va_list va;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_start(va, kwlist);
    retval = vgetargskeywords(args, keywords, format, kwlist, &va, 0);
    va_end(va);
    return retval;
}

int
_PyArg_ParseTupleAndKeywords_SizeT(PyObject *args,
                                  PyObject *keywords,
                                  const char *format,
                                  char **kwlist, ...)
{
    int retval;
    va_list va;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

    va_start(va, kwlist);
    retval = vgetargskeywords(args, keywords, format,
                              kwlist, &va, FLAG_SIZE_T);
    va_end(va);
    return retval;
}


int
PyArg_VaParseTupleAndKeywords(PyObject *args,
                              PyObject *keywords,
                              const char *format,
                              char **kwlist, va_list va)
{
    int retval;
    va_list lva;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

#ifdef VA_LIST_IS_ARRAY
    memcpy(lva, va, sizeof(va_list));
#else
#ifdef __va_copy
    __va_copy(lva, va);
#else
    lva = va;
#endif
#endif

    retval = vgetargskeywords(args, keywords, format, kwlist, &lva, 0);
    return retval;
}

int
_PyArg_VaParseTupleAndKeywords_SizeT(PyObject *args,
                                    PyObject *keywords,
                                    const char *format,
                                    char **kwlist, va_list va)
{
    int retval;
    va_list lva;

    if ((args == NULL || !PyTuple_Check(args)) ||
        (keywords != NULL && !PyDict_Check(keywords)) ||
        format == NULL ||
        kwlist == NULL)
    {
        PyErr_BadInternalCall();
        return 0;
    }

#ifdef VA_LIST_IS_ARRAY
    memcpy(lva, va, sizeof(va_list));
#else
#ifdef __va_copy
    __va_copy(lva, va);
#else
    lva = va;
#endif
#endif

    retval = vgetargskeywords(args, keywords, format,
                              kwlist, &lva, FLAG_SIZE_T);
    return retval;
}

#define IS_END_OF_FORMAT(c) (c == '\0' || c == ';' || c == ':')

static int
vgetargskeywords(PyObject *args, PyObject *keywords, const char *format,
                 char **kwlist, va_list *p_va, int flags)
{
    char msgbuf[512];
    int levels[32];
    const char *fname, *msg, *custom_msg, *keyword;
    int min = INT_MAX;
    int i, len, nargs, nkeywords;
    PyObject *freelist = NULL, *current_arg;

    assert(args != NULL && PyTuple_Check(args));
    assert(keywords == NULL || PyDict_Check(keywords));
    assert(format != NULL);
    assert(kwlist != NULL);
    assert(p_va != NULL);

    /* grab the function name or custom error msg first (mutually exclusive) */
    fname = strchr(format, ':');
    if (fname) {
        fname++;
        custom_msg = NULL;
    }
    else {
        custom_msg = strchr(format,';');
        if (custom_msg)
            custom_msg++;
    }

    /* scan kwlist and get greatest possible nbr of args */
    for (len=0; kwlist[len]; len++)
        continue;

    nargs = PyTuple_GET_SIZE(args);
    nkeywords = (keywords == NULL) ? 0 : PyDict_Size(keywords);
    if (nargs + nkeywords > len) {
        PyErr_Format(PyExc_TypeError, "%s%s takes at most %d "
                     "argument%s (%d given)",
                     (fname == NULL) ? "function" : fname,
                     (fname == NULL) ? "" : "()",
                     len,
                     (len == 1) ? "" : "s",
                     nargs + nkeywords);
        return 0;
    }

    /* convert tuple args and keyword args in same loop, using kwlist to drive process */
    for (i = 0; i < len; i++) {
        keyword = kwlist[i];
        if (*format == '|') {
            min = i;
            format++;
        }
        if (IS_END_OF_FORMAT(*format)) {
            PyErr_Format(PyExc_RuntimeError,
                         "More keyword list entries (%d) than "
                         "format specifiers (%d)", len, i);
            return cleanreturn(0, freelist);
        }
        current_arg = NULL;
        if (nkeywords) {
            current_arg = PyDict_GetItemString(keywords, keyword);
        }
        if (current_arg) {
            --nkeywords;
            if (i < nargs) {
                /* arg present in tuple and in dict */
                PyErr_Format(PyExc_TypeError,
                             "Argument given by name ('%s') "
                             "and position (%d)",
                             keyword, i+1);
                return cleanreturn(0, freelist);
            }
        }
        else if (nkeywords && PyErr_Occurred())
            return cleanreturn(0, freelist);
        else if (i < nargs)
            current_arg = PyTuple_GET_ITEM(args, i);

        if (current_arg) {
            msg = convertitem(current_arg, &format, p_va, flags,
                levels, msgbuf, sizeof(msgbuf), &freelist);
            if (msg) {
                seterror(i+1, msg, levels, fname, custom_msg);
                return cleanreturn(0, freelist);
            }
            continue;
        }

        if (i < min) {
            PyErr_Format(PyExc_TypeError, "Required argument "
                         "'%s' (pos %d) not found",
                         keyword, i+1);
            return cleanreturn(0, freelist);
        }
        /* current code reports success when all required args
         * fulfilled and no keyword args left, with no further
         * validation. XXX Maybe skip this in debug build ?
         */
        if (!nkeywords)
            return cleanreturn(1, freelist);

        /* We are into optional args, skip thru to any remaining
         * keyword args */
        msg = skipitem(&format, p_va, flags);
        if (msg) {
            PyErr_Format(PyExc_RuntimeError, "%s: '%s'", msg,
                         format);
            return cleanreturn(0, freelist);
        }
    }

    if (!IS_END_OF_FORMAT(*format) && *format != '|') {
        PyErr_Format(PyExc_RuntimeError,
            "more argument specifiers than keyword list entries "
            "(remaining format:'%s')", format);
        return cleanreturn(0, freelist);
    }

    /* make sure there are no extraneous keyword arguments */
    if (nkeywords > 0) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(keywords, &pos, &key, &value)) {
            int match = 0;
            char *ks;
            if (!PyString_Check(key)) {
                PyErr_SetString(PyExc_TypeError,
                                "keywords must be strings");
                return cleanreturn(0, freelist);
            }
            ks = PyString_AsString(key);
            for (i = 0; i < len; i++) {
                if (!strcmp(ks, kwlist[i])) {
                    match = 1;
                    break;
                }
            }
            if (!match) {
                PyErr_Format(PyExc_TypeError,
                             "'%s' is an invalid keyword "
                             "argument for this function",
                             ks);
                return cleanreturn(0, freelist);
            }
        }
    }

    return cleanreturn(1, freelist);
}


static char *
skipitem(const char **p_format, va_list *p_va, int flags)
{
    const char *format = *p_format;
    char c = *format++;

    switch (c) {

    /* simple codes
     * The individual types (second arg of va_arg) are irrelevant */

    case 'b': /* byte -- very short int */
    case 'B': /* byte as bitfield */
    case 'h': /* short int */
    case 'H': /* short int as bitfield */
    case 'i': /* int */
    case 'I': /* int sized bitfield */
    case 'l': /* long int */
    case 'k': /* long int sized bitfield */
#ifdef HAVE_LONG_LONG
    case 'L': /* PY_LONG_LONG */
    case 'K': /* PY_LONG_LONG sized bitfield */
#endif
    case 'f': /* float */
    case 'd': /* double */
#ifndef WITHOUT_COMPLEX
    case 'D': /* complex double */
#endif
    case 'c': /* char */
        {
            (void) va_arg(*p_va, void *);
            break;
        }

    case 'n': /* Py_ssize_t */
        {
            (void) va_arg(*p_va, Py_ssize_t *);
            break;
        }

    /* string codes */

    case 'e': /* string with encoding */
        {
            (void) va_arg(*p_va, const char *);
            if (!(*format == 's' || *format == 't'))
                /* after 'e', only 's' and 't' is allowed */
                goto err;
            format++;
            /* explicit fallthrough to string cases */
        }

    case 's': /* string */
    case 'z': /* string or None */
#ifdef Py_USING_UNICODE
    case 'u': /* unicode string */
#endif
    case 't': /* buffer, read-only */
    case 'w': /* buffer, read-write */
        {
            (void) va_arg(*p_va, char **);
            if (*format == '#') {
                if (flags & FLAG_SIZE_T)
                    (void) va_arg(*p_va, Py_ssize_t *);
                else
                    (void) va_arg(*p_va, int *);
                format++;
            } else if ((c == 's' || c == 'z') && *format == '*') {
                format++;
            }
            break;
        }

    /* object codes */

    case 'S': /* string object */
#ifdef Py_USING_UNICODE
    case 'U': /* unicode string object */
#endif
        {
            (void) va_arg(*p_va, PyObject **);
            break;
        }

    case 'O': /* object */
        {
            if (*format == '!') {
                format++;
                (void) va_arg(*p_va, PyTypeObject*);
                (void) va_arg(*p_va, PyObject **);
            }
            else if (*format == '&') {
                typedef int (*converter)(PyObject *, void *);
                (void) va_arg(*p_va, converter);
                (void) va_arg(*p_va, void *);
                format++;
            }
            else {
                (void) va_arg(*p_va, PyObject **);
            }
            break;
        }

    case '(':           /* bypass tuple, not handled at all previously */
        {
            char *msg;
            for (;;) {
                if (*format==')')
                    break;
                if (IS_END_OF_FORMAT(*format))
                    return "Unmatched left paren in format "
                           "string";
                msg = skipitem(&format, p_va, flags);
                if (msg)
                    return msg;
            }
            format++;
            break;
        }

    case ')':
        return "Unmatched right paren in format string";

    default:
err:
        return "impossible<bad format char>";

    }

    *p_format = format;
    return NULL;
}


int
PyArg_UnpackTuple(PyObject *args, const char *name, Py_ssize_t min, Py_ssize_t max, ...)
{
    Py_ssize_t i, l;
    PyObject **o;
    va_list vargs;

#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, max);
#else
    va_start(vargs);
#endif

    assert(min >= 0);
    assert(min <= max);
    if (!PyTuple_Check(args)) {
        va_end(vargs);
        PyErr_SetString(PyExc_SystemError,
            "PyArg_UnpackTuple() argument list is not a tuple");
        return 0;
    }
    l = PyTuple_GET_SIZE(args);
    if (l < min) {
        if (name != NULL)
            PyErr_Format(
                PyExc_TypeError,
                "%s expected %s%zd arguments, got %zd",
                name, (min == max ? "" : "at least "), min, l);
        else
            PyErr_Format(
                PyExc_TypeError,
                "unpacked tuple should have %s%zd elements,"
                " but has %zd",
                (min == max ? "" : "at least "), min, l);
        va_end(vargs);
        return 0;
    }
    if (l > max) {
        if (name != NULL)
            PyErr_Format(
                PyExc_TypeError,
                "%s expected %s%zd arguments, got %zd",
                name, (min == max ? "" : "at most "), max, l);
        else
            PyErr_Format(
                PyExc_TypeError,
                "unpacked tuple should have %s%zd elements,"
                " but has %zd",
                (min == max ? "" : "at most "), max, l);
        va_end(vargs);
        return 0;
    }
    for (i = 0; i < l; i++) {
        o = va_arg(vargs, PyObject **);
        *o = PyTuple_GET_ITEM(args, i);
    }
    va_end(vargs);
    return 1;
}


/* For type constructors that don't take keyword args
 *
 * Sets a TypeError and returns 0 if the kwds dict is
 * not empty, returns 1 otherwise
 */
int
_PyArg_NoKeywords(const char *funcname, PyObject *kw)
{
    if (kw == NULL)
        return 1;
    if (!PyDict_CheckExact(kw)) {
        PyErr_BadInternalCall();
        return 0;
    }
    if (PyDict_Size(kw) == 0)
        return 1;

    PyErr_Format(PyExc_TypeError, "%s does not take keyword arguments",
                    funcname);
    return 0;
}
#ifdef __cplusplus
};
#endif
