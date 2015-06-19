
/* Float object implementation */

/* XXX There should be overflow checks here, but it's hard to check
   for any kind of float exception without losing portability. */

#include "Python.h"
#include "structseq.h"

#include <ctype.h>
#include <float.h>

#undef MAX
#undef MIN
#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#ifdef _OSF_SOURCE
/* OSF1 5.1 doesn't make this available with XOPEN_SOURCE_EXTENDED defined */
extern int finite(double);
#endif

/* Special free list -- see comments for same code in intobject.c. */
#define BLOCK_SIZE      1000    /* 1K less typical malloc overhead */
#define BHEAD_SIZE      8       /* Enough for a 64-bit pointer */
#define N_FLOATOBJECTS  ((BLOCK_SIZE - BHEAD_SIZE) / sizeof(PyFloatObject))

struct _floatblock {
    struct _floatblock *next;
    PyFloatObject objects[N_FLOATOBJECTS];
};

typedef struct _floatblock PyFloatBlock;

static PyFloatBlock *block_list = NULL;
static PyFloatObject *free_list = NULL;

static PyFloatObject *
fill_free_list(void)
{
    PyFloatObject *p, *q;
    /* XXX Float blocks escape the object heap. Use PyObject_MALLOC ??? */
    p = (PyFloatObject *) PyMem_MALLOC(sizeof(PyFloatBlock));
    if (p == NULL)
        return (PyFloatObject *) PyErr_NoMemory();
    ((PyFloatBlock *)p)->next = block_list;
    block_list = (PyFloatBlock *)p;
    p = &((PyFloatBlock *)p)->objects[0];
    q = p + N_FLOATOBJECTS;
    while (--q > p)
        Py_TYPE(q) = (struct _typeobject *)(q-1);
    Py_TYPE(q) = NULL;
    return p + N_FLOATOBJECTS - 1;
}

double
PyFloat_GetMax(void)
{
    return DBL_MAX;
}

double
PyFloat_GetMin(void)
{
    return DBL_MIN;
}

static PyTypeObject FloatInfoType = {0, 0, 0, 0, 0, 0};

PyDoc_STRVAR(floatinfo__doc__,
"sys.float_info\n\
\n\
A structseq holding information about the float type. It contains low level\n\
information about the precision and internal representation. Please study\n\
your system's :file:`float.h` for more information.");

static PyStructSequence_Field floatinfo_fields[] = {
    {"max",             "DBL_MAX -- maximum representable finite float"},
    {"max_exp",         "DBL_MAX_EXP -- maximum int e such that radix**(e-1) "
                    "is representable"},
    {"max_10_exp",      "DBL_MAX_10_EXP -- maximum int e such that 10**e "
                    "is representable"},
    {"min",             "DBL_MIN -- Minimum positive normalizer float"},
    {"min_exp",         "DBL_MIN_EXP -- minimum int e such that radix**(e-1) "
                    "is a normalized float"},
    {"min_10_exp",      "DBL_MIN_10_EXP -- minimum int e such that 10**e is "
                    "a normalized"},
    {"dig",             "DBL_DIG -- digits"},
    {"mant_dig",        "DBL_MANT_DIG -- mantissa digits"},
    {"epsilon",         "DBL_EPSILON -- Difference between 1 and the next "
                    "representable float"},
    {"radix",           "FLT_RADIX -- radix of exponent"},
    {"rounds",          "FLT_ROUNDS -- addition rounds"},
    {0}
};

static PyStructSequence_Desc floatinfo_desc = {
    "sys.float_info",           /* name */
    floatinfo__doc__,           /* doc */
    floatinfo_fields,           /* fields */
    11
};

PyObject *
PyFloat_GetInfo(void)
{
    PyObject* floatinfo;
    int pos = 0;

    floatinfo = PyStructSequence_New(&FloatInfoType);
    if (floatinfo == NULL) {
        return NULL;
    }

#define SetIntFlag(flag) \
    PyStructSequence_SET_ITEM(floatinfo, pos++, PyInt_FromLong(flag))
#define SetDblFlag(flag) \
    PyStructSequence_SET_ITEM(floatinfo, pos++, PyFloat_FromDouble(flag))

    SetDblFlag(DBL_MAX);
    SetIntFlag(DBL_MAX_EXP);
    SetIntFlag(DBL_MAX_10_EXP);
    SetDblFlag(DBL_MIN);
    SetIntFlag(DBL_MIN_EXP);
    SetIntFlag(DBL_MIN_10_EXP);
    SetIntFlag(DBL_DIG);
    SetIntFlag(DBL_MANT_DIG);
    SetDblFlag(DBL_EPSILON);
    SetIntFlag(FLT_RADIX);
    SetIntFlag(FLT_ROUNDS);
#undef SetIntFlag
#undef SetDblFlag

    if (PyErr_Occurred()) {
        Py_CLEAR(floatinfo);
        return NULL;
    }
    return floatinfo;
}

// pyston change: comment this out
#if 0
PyObject *
PyFloat_FromDouble(double fval)
{
    register PyFloatObject *op;
    if (free_list == NULL) {
        if ((free_list = fill_free_list()) == NULL)
            return NULL;
    }
    /* Inline PyObject_New */
    op = free_list;
    free_list = (PyFloatObject *)Py_TYPE(op);
    PyObject_INIT(op, &PyFloat_Type);
    op->ob_fval = fval;
    return (PyObject *) op;
}
#endif

/**************************************************************************
RED_FLAG 22-Sep-2000 tim
PyFloat_FromString's pend argument is braindead.  Prior to this RED_FLAG,

1.  If v was a regular string, *pend was set to point to its terminating
    null byte.  That's useless (the caller can find that without any
    help from this function!).

2.  If v was a Unicode string, or an object convertible to a character
    buffer, *pend was set to point into stack trash (the auto temp
    vector holding the character buffer).  That was downright dangerous.

Since we can't change the interface of a public API function, pend is
still supported but now *officially* useless:  if pend is not NULL,
*pend is set to NULL.
**************************************************************************/
// pyston change: comment this out
#if 0
PyObject *
PyFloat_FromString(PyObject *v, char **pend)
{
    const char *s, *last, *end;
    double x;
    char buffer[256]; /* for errors */
#ifdef Py_USING_UNICODE
    char *s_buffer = NULL;
#endif
    Py_ssize_t len;
    PyObject *result = NULL;

    if (pend)
        *pend = NULL;
    if (PyString_Check(v)) {
        s = PyString_AS_STRING(v);
        len = PyString_GET_SIZE(v);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(v)) {
        s_buffer = (char *)PyMem_MALLOC(PyUnicode_GET_SIZE(v)+1);
        if (s_buffer == NULL)
            return PyErr_NoMemory();
        if (PyUnicode_EncodeDecimal(PyUnicode_AS_UNICODE(v),
                                    PyUnicode_GET_SIZE(v),
                                    s_buffer,
                                    NULL))
            goto error;
        s = s_buffer;
        len = strlen(s);
    }
#endif
    else if (PyObject_AsCharBuffer(v, &s, &len)) {
        PyErr_SetString(PyExc_TypeError,
            "float() argument must be a string or a number");
        return NULL;
    }
    last = s + len;

    while (Py_ISSPACE(*s))
        s++;
    /* We don't care about overflow or underflow.  If the platform
     * supports them, infinities and signed zeroes (on underflow) are
     * fine. */
    x = PyOS_string_to_double(s, (char **)&end, NULL);
    if (x == -1.0 && PyErr_Occurred())
        goto error;
    while (Py_ISSPACE(*end))
        end++;
    if (end == last)
        result = PyFloat_FromDouble(x);
    else {
        PyOS_snprintf(buffer, sizeof(buffer),
                      "invalid literal for float(): %.200s", s);
        PyErr_SetString(PyExc_ValueError, buffer);
        result = NULL;
    }

  error:
#ifdef Py_USING_UNICODE
    if (s_buffer)
        PyMem_FREE(s_buffer);
#endif
    return result;
}
#endif

static void
float_dealloc(PyFloatObject *op)
{
    if (PyFloat_CheckExact(op)) {
        Py_TYPE(op) = (struct _typeobject *)free_list;
        free_list = op;
    }
    else
        Py_TYPE(op)->tp_free((PyObject *)op);
}

// pyston change: comment this out
#if 0
double
PyFloat_AsDouble(PyObject *op)
{
    PyNumberMethods *nb;
    PyFloatObject *fo;
    double val;

    if (op && PyFloat_Check(op))
        return PyFloat_AS_DOUBLE((PyFloatObject*) op);

    if (op == NULL) {
        PyErr_BadArgument();
        return -1;
    }

    if ((nb = Py_TYPE(op)->tp_as_number) == NULL || nb->nb_float == NULL) {
        PyErr_SetString(PyExc_TypeError, "a float is required");
        return -1;
    }

    fo = (PyFloatObject*) (*nb->nb_float) (op);
    if (fo == NULL)
        return -1;
    if (!PyFloat_Check(fo)) {
        PyErr_SetString(PyExc_TypeError,
                        "nb_float should return float object");
        return -1;
    }

    val = PyFloat_AS_DOUBLE(fo);
    Py_DECREF(fo);

    return val;
}
#endif

/* Methods */

/* Macro and helper that convert PyObject obj to a C double and store
   the value in dbl; this replaces the functionality of the coercion
   slot function.  If conversion to double raises an exception, obj is
   set to NULL, and the function invoking this macro returns NULL.  If
   obj is not of float, int or long type, Py_NotImplemented is incref'ed,
   stored in obj, and returned from the function invoking this macro.
*/
#define CONVERT_TO_DOUBLE(obj, dbl)                     \
    if (PyFloat_Check(obj))                             \
        dbl = PyFloat_AS_DOUBLE(obj);                   \
    else if (convert_to_double(&(obj), &(dbl)) < 0)     \
        return obj;

static int
convert_to_double(PyObject **v, double *dbl)
{
    register PyObject *obj = *v;

    if (PyInt_Check(obj)) {
        *dbl = (double)PyInt_AS_LONG(obj);
    }
    else if (PyLong_Check(obj)) {
        *dbl = PyLong_AsDouble(obj);
        if (*dbl == -1.0 && PyErr_Occurred()) {
            *v = NULL;
            return -1;
        }
    }
    else {
        Py_INCREF(Py_NotImplemented);
        *v = Py_NotImplemented;
        return -1;
    }
    return 0;
}

/* XXX PyFloat_AsString and PyFloat_AsReprString are deprecated:
   XXX they pass a char buffer without passing a length.
*/
void
PyFloat_AsString(char *buf, PyFloatObject *v)
{
    char *tmp = PyOS_double_to_string(v->ob_fval, 'g',
                    PyFloat_STR_PRECISION,
                    Py_DTSF_ADD_DOT_0, NULL);
    strcpy(buf, tmp);
    PyMem_Free(tmp);
}

void
PyFloat_AsReprString(char *buf, PyFloatObject *v)
{
    char * tmp = PyOS_double_to_string(v->ob_fval, 'r', 0,
                    Py_DTSF_ADD_DOT_0, NULL);
    strcpy(buf, tmp);
    PyMem_Free(tmp);
}

/* ARGSUSED */
static int
float_print(PyFloatObject *v, FILE *fp, int flags)
{
    char *buf;
    if (flags & Py_PRINT_RAW)
        buf = PyOS_double_to_string(v->ob_fval,
                                    'g', PyFloat_STR_PRECISION,
                                    Py_DTSF_ADD_DOT_0, NULL);
    else
        buf = PyOS_double_to_string(v->ob_fval,
                            'r', 0, Py_DTSF_ADD_DOT_0, NULL);
    Py_BEGIN_ALLOW_THREADS
    fputs(buf, fp);
    Py_END_ALLOW_THREADS
    PyMem_Free(buf);
    return 0;
}

static PyObject *
float_str_or_repr(PyFloatObject *v, int precision, char format_code)
{
    PyObject *result;
    char *buf = PyOS_double_to_string(PyFloat_AS_DOUBLE(v),
                                  format_code, precision,
                                  Py_DTSF_ADD_DOT_0,
                                  NULL);
    if (!buf)
        return PyErr_NoMemory();
    result = PyString_FromString(buf);
    PyMem_Free(buf);
    return result;
}

static PyObject *
float_repr(PyFloatObject *v)
{
    return float_str_or_repr(v, 0, 'r');
}

static PyObject *
float_str(PyFloatObject *v)
{
    return float_str_or_repr(v, PyFloat_STR_PRECISION, 'g');
}

/* Comparison is pretty much a nightmare.  When comparing float to float,
 * we do it as straightforwardly (and long-windedly) as conceivable, so
 * that, e.g., Python x == y delivers the same result as the platform
 * C x == y when x and/or y is a NaN.
 * When mixing float with an integer type, there's no good *uniform* approach.
 * Converting the double to an integer obviously doesn't work, since we
 * may lose info from fractional bits.  Converting the integer to a double
 * also has two failure modes:  (1) a long int may trigger overflow (too
 * large to fit in the dynamic range of a C double); (2) even a C long may have
 * more bits than fit in a C double (e.g., on a a 64-bit box long may have
 * 63 bits of precision, but a C double probably has only 53), and then
 * we can falsely claim equality when low-order integer bits are lost by
 * coercion to double.  So this part is painful too.
 */

// Pyston change: don't need this for now
#if 0
static PyObject*
float_richcompare(PyObject *v, PyObject *w, int op)
{
    double i, j;
    int r = 0;

    assert(PyFloat_Check(v));
    i = PyFloat_AS_DOUBLE(v);

    /* Switch on the type of w.  Set i and j to doubles to be compared,
     * and op to the richcomp to use.
     */
    if (PyFloat_Check(w))
        j = PyFloat_AS_DOUBLE(w);

    else if (!Py_IS_FINITE(i)) {
        if (PyInt_Check(w) || PyLong_Check(w))
            /* If i is an infinity, its magnitude exceeds any
             * finite integer, so it doesn't matter which int we
             * compare i with.  If i is a NaN, similarly.
             */
            j = 0.0;
        else
            goto Unimplemented;
    }

    else if (PyInt_Check(w)) {
        long jj = PyInt_AS_LONG(w);
        /* In the worst realistic case I can imagine, C double is a
         * Cray single with 48 bits of precision, and long has 64
         * bits.
         */
#if SIZEOF_LONG > 6
        unsigned long abs = (unsigned long)(jj < 0 ? -jj : jj);
        if (abs >> 48) {
            /* Needs more than 48 bits.  Make it take the
             * PyLong path.
             */
            PyObject *result;
            PyObject *ww = PyLong_FromLong(jj);

            if (ww == NULL)
                return NULL;
            result = float_richcompare(v, ww, op);
            Py_DECREF(ww);
            return result;
        }
#endif
        j = (double)jj;
        assert((long)j == jj);
    }

    else if (PyLong_Check(w)) {
        int vsign = i == 0.0 ? 0 : i < 0.0 ? -1 : 1;
        int wsign = _PyLong_Sign(w);
        size_t nbits;
        int exponent;

        if (vsign != wsign) {
            /* Magnitudes are irrelevant -- the signs alone
             * determine the outcome.
             */
            i = (double)vsign;
            j = (double)wsign;
            goto Compare;
        }
        /* The signs are the same. */
        /* Convert w to a double if it fits.  In particular, 0 fits. */
        nbits = _PyLong_NumBits(w);
        if (nbits == (size_t)-1 && PyErr_Occurred()) {
            /* This long is so large that size_t isn't big enough
             * to hold the # of bits.  Replace with little doubles
             * that give the same outcome -- w is so large that
             * its magnitude must exceed the magnitude of any
             * finite float.
             */
            PyErr_Clear();
            i = (double)vsign;
            assert(wsign != 0);
            j = wsign * 2.0;
            goto Compare;
        }
        if (nbits <= 48) {
            j = PyLong_AsDouble(w);
            /* It's impossible that <= 48 bits overflowed. */
            assert(j != -1.0 || ! PyErr_Occurred());
            goto Compare;
        }
        assert(wsign != 0); /* else nbits was 0 */
        assert(vsign != 0); /* if vsign were 0, then since wsign is
                             * not 0, we would have taken the
                             * vsign != wsign branch at the start */
        /* We want to work with non-negative numbers. */
        if (vsign < 0) {
            /* "Multiply both sides" by -1; this also swaps the
             * comparator.
             */
            i = -i;
            op = _Py_SwappedOp[op];
        }
        assert(i > 0.0);
        (void) frexp(i, &exponent);
        /* exponent is the # of bits in v before the radix point;
         * we know that nbits (the # of bits in w) > 48 at this point
         */
        if (exponent < 0 || (size_t)exponent < nbits) {
            i = 1.0;
            j = 2.0;
            goto Compare;
        }
        if ((size_t)exponent > nbits) {
            i = 2.0;
            j = 1.0;
            goto Compare;
        }
        /* v and w have the same number of bits before the radix
         * point.  Construct two longs that have the same comparison
         * outcome.
         */
        {
            double fracpart;
            double intpart;
            PyObject *result = NULL;
            PyObject *one = NULL;
            PyObject *vv = NULL;
            PyObject *ww = w;

            if (wsign < 0) {
                ww = PyNumber_Negative(w);
                if (ww == NULL)
                    goto Error;
            }
            else
                Py_INCREF(ww);

            fracpart = modf(i, &intpart);
            vv = PyLong_FromDouble(intpart);
            if (vv == NULL)
                goto Error;

            if (fracpart != 0.0) {
                /* Shift left, and or a 1 bit into vv
                 * to represent the lost fraction.
                 */
                PyObject *temp;

                one = PyInt_FromLong(1);
                if (one == NULL)
                    goto Error;

                temp = PyNumber_Lshift(ww, one);
                if (temp == NULL)
                    goto Error;
                Py_DECREF(ww);
                ww = temp;

                temp = PyNumber_Lshift(vv, one);
                if (temp == NULL)
                    goto Error;
                Py_DECREF(vv);
                vv = temp;

                temp = PyNumber_Or(vv, one);
                if (temp == NULL)
                    goto Error;
                Py_DECREF(vv);
                vv = temp;
            }

            r = PyObject_RichCompareBool(vv, ww, op);
            if (r < 0)
                goto Error;
            result = PyBool_FromLong(r);
         Error:
            Py_XDECREF(vv);
            Py_XDECREF(ww);
            Py_XDECREF(one);
            return result;
        }
    } /* else if (PyLong_Check(w)) */

    else        /* w isn't float, int, or long */
        goto Unimplemented;

 Compare:
    PyFPE_START_PROTECT("richcompare", return NULL)
    switch (op) {
    case Py_EQ:
        r = i == j;
        break;
    case Py_NE:
        r = i != j;
        break;
    case Py_LE:
        r = i <= j;
        break;
    case Py_GE:
        r = i >= j;
        break;
    case Py_LT:
        r = i < j;
        break;
    case Py_GT:
        r = i > j;
        break;
    }
    PyFPE_END_PROTECT(r)
    return PyBool_FromLong(r);

 Unimplemented:
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
}
#endif

static long
float_hash(PyFloatObject *v)
{
    return _Py_HashDouble(v->ob_fval);
}

static PyObject *
float_add(PyObject *v, PyObject *w)
{
    double a,b;
    CONVERT_TO_DOUBLE(v, a);
    CONVERT_TO_DOUBLE(w, b);
    PyFPE_START_PROTECT("add", return 0)
    a = a + b;
    PyFPE_END_PROTECT(a)
    return PyFloat_FromDouble(a);
}

static PyObject *
float_sub(PyObject *v, PyObject *w)
{
    double a,b;
    CONVERT_TO_DOUBLE(v, a);
    CONVERT_TO_DOUBLE(w, b);
    PyFPE_START_PROTECT("subtract", return 0)
    a = a - b;
    PyFPE_END_PROTECT(a)
    return PyFloat_FromDouble(a);
}

static PyObject *
float_mul(PyObject *v, PyObject *w)
{
    double a,b;
    CONVERT_TO_DOUBLE(v, a);
    CONVERT_TO_DOUBLE(w, b);
    PyFPE_START_PROTECT("multiply", return 0)
    a = a * b;
    PyFPE_END_PROTECT(a)
    return PyFloat_FromDouble(a);
}

static PyObject *
float_div(PyObject *v, PyObject *w)
{
    double a,b;
    CONVERT_TO_DOUBLE(v, a);
    CONVERT_TO_DOUBLE(w, b);
#ifdef Py_NAN
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                        "float division by zero");
        return NULL;
    }
#endif
    PyFPE_START_PROTECT("divide", return 0)
    a = a / b;
    PyFPE_END_PROTECT(a)
    return PyFloat_FromDouble(a);
}

// Pyston change: don't need this for now
#if 0
static PyObject *
float_classic_div(PyObject *v, PyObject *w)
{
    double a,b;
    CONVERT_TO_DOUBLE(v, a);
    CONVERT_TO_DOUBLE(w, b);
    if (Py_DivisionWarningFlag >= 2 &&
        PyErr_Warn(PyExc_DeprecationWarning, "classic float division") < 0)
        return NULL;
#ifdef Py_NAN
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                        "float division by zero");
        return NULL;
    }
#endif
    PyFPE_START_PROTECT("divide", return 0)
    a = a / b;
    PyFPE_END_PROTECT(a)
    return PyFloat_FromDouble(a);
}
#endif

static PyObject *
float_rem(PyObject *v, PyObject *w)
{
    double vx, wx;
    double mod;
    CONVERT_TO_DOUBLE(v, vx);
    CONVERT_TO_DOUBLE(w, wx);
#ifdef Py_NAN
    if (wx == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                        "float modulo");
        return NULL;
    }
#endif
    PyFPE_START_PROTECT("modulo", return 0)
    mod = fmod(vx, wx);
    if (mod) {
        /* ensure the remainder has the same sign as the denominator */
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
        }
    }
    else {
        /* the remainder is zero, and in the presence of signed zeroes
           fmod returns different results across platforms; ensure
           it has the same sign as the denominator; we'd like to do
           "mod = wx * 0.0", but that may get optimized away */
        mod *= mod;  /* hide "mod = +0" from optimizer */
        if (wx < 0.0)
            mod = -mod;
    }
    PyFPE_END_PROTECT(mod)
    return PyFloat_FromDouble(mod);
}

static PyObject *
float_divmod(PyObject *v, PyObject *w)
{
    double vx, wx;
    double div, mod, floordiv;
    CONVERT_TO_DOUBLE(v, vx);
    CONVERT_TO_DOUBLE(w, wx);
    if (wx == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, "float divmod()");
        return NULL;
    }
    PyFPE_START_PROTECT("divmod", return 0)
    mod = fmod(vx, wx);
    /* fmod is typically exact, so vx-mod is *mathematically* an
       exact multiple of wx.  But this is fp arithmetic, and fp
       vx - mod is an approximation; the result is that div may
       not be an exact integral value after the division, although
       it will always be very close to one.
    */
    div = (vx - mod) / wx;
    if (mod) {
        /* ensure the remainder has the same sign as the denominator */
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
            div -= 1.0;
        }
    }
    else {
        /* the remainder is zero, and in the presence of signed zeroes
           fmod returns different results across platforms; ensure
           it has the same sign as the denominator; we'd like to do
           "mod = wx * 0.0", but that may get optimized away */
        mod *= mod;  /* hide "mod = +0" from optimizer */
        if (wx < 0.0)
            mod = -mod;
    }
    /* snap quotient to nearest integral value */
    if (div) {
        floordiv = floor(div);
        if (div - floordiv > 0.5)
            floordiv += 1.0;
    }
    else {
        /* div is zero - get the same sign as the true quotient */
        div *= div;             /* hide "div = +0" from optimizers */
        floordiv = div * vx / wx; /* zero w/ sign of vx/wx */
    }
    PyFPE_END_PROTECT(floordiv)
    return Py_BuildValue("(dd)", floordiv, mod);
}

static PyObject *
float_floor_div(PyObject *v, PyObject *w)
{
    PyObject *t, *r;

    t = float_divmod(v, w);
    if (t == NULL || t == Py_NotImplemented)
        return t;
    assert(PyTuple_CheckExact(t));
    r = PyTuple_GET_ITEM(t, 0);
    Py_INCREF(r);
    Py_DECREF(t);
    return r;
}

/* determine whether x is an odd integer or not;  assumes that
   x is not an infinity or nan. */
#define DOUBLE_IS_ODD_INTEGER(x) (fmod(fabs(x), 2.0) == 1.0)

inline int float_pow_unboxed(double iv, double iw, double* res);

// pyston change: split this up into float_pow and and float_pow_unboxed
PyObject *
float_pow(PyObject *v, PyObject *w, PyObject *z)
{
    double iv, iw, res;
    int err;

    if ((PyObject *)z != Py_None) {
        PyErr_SetString(PyExc_TypeError, "pow() 3rd argument not "
            "allowed unless all arguments are integers");
        return NULL;
    }

    CONVERT_TO_DOUBLE(v, iv);
    CONVERT_TO_DOUBLE(w, iw);

    err = float_pow_unboxed(iv, iw, &res);
    if (err) {
        return NULL;
    } else {
        return PyFloat_FromDouble(res);
    }
}

int
float_pow_unboxed(double iv, double iw, double* res)
{
    double ix;
    int negate_result = 0;

    /* Sort out special cases here instead of relying on pow() */
    if (iw == 0) {              /* v**0 is 1, even 0**0 */
        *res = 1.0;
        return 0;
    }
    if (Py_IS_NAN(iv)) {        /* nan**w = nan, unless w == 0 */
        *res = iv;
        return 0;
    }
    if (Py_IS_NAN(iw)) {        /* v**nan = nan, unless v == 1; 1**nan = 1 */
        *res = (iv == 1.0 ? 1.0 : iw);
        return 0;
    }
    if (Py_IS_INFINITY(iw)) {
        /* v**inf is: 0.0 if abs(v) < 1; 1.0 if abs(v) == 1; inf if
         *     abs(v) > 1 (including case where v infinite)
         *
         * v**-inf is: inf if abs(v) < 1; 1.0 if abs(v) == 1; 0.0 if
         *     abs(v) > 1 (including case where v infinite)
         */
        iv = fabs(iv);
        if (iv == 1.0) {
            *res = 1.0;
            return 0;
        } else if ((iw > 0.0) == (iv > 1.0)) {
            *res = fabs(iw);
            return 0;
        } else {
            *res = 0.0;
            return 0;
        }
    }
    if (Py_IS_INFINITY(iv)) {
        /* (+-inf)**w is: inf for w positive, 0 for w negative; in
         *     both cases, we need to add the appropriate sign if w is
         *     an odd integer.
         */
        int iw_is_odd = DOUBLE_IS_ODD_INTEGER(iw);
        if (iw > 0.0) {
            *res = (iw_is_odd ? iv : fabs(iv));
            return 0;
        } else {
            *res = (iw_is_odd ?  copysign(0.0, iv) : 0.0);
            return 0;
        }
    }
    if (iv == 0.0) {  /* 0**w is: 0 for w positive, 1 for w zero
                         (already dealt with above), and an error
                         if w is negative. */
        int iw_is_odd = DOUBLE_IS_ODD_INTEGER(iw);
        if (iw < 0.0) {
            PyErr_SetString(PyExc_ZeroDivisionError,
                            "0.0 cannot be raised to a "
                            "negative power");
            return 1;
        }
        /* use correct sign if iw is odd */
        *res = (iw_is_odd ? iv : 0.0);
        return 0;
    }

    if (iv < 0.0) {
        /* Whether this is an error is a mess, and bumps into libm
         * bugs so we have to figure it out ourselves.
         */
        if (iw != floor(iw)) {
            PyErr_SetString(PyExc_ValueError, "negative number "
                "cannot be raised to a fractional power");
            return 1;
        }
        /* iw is an exact integer, albeit perhaps a very large
         * one.  Replace iv by its absolute value and remember
         * to negate the pow result if iw is odd.
         */
        iv = -iv;
        negate_result = DOUBLE_IS_ODD_INTEGER(iw);
    }

    if (iv == 1.0) { /* 1**w is 1, even 1**inf and 1**nan */
        /* (-1) ** large_integer also ends up here.  Here's an
         * extract from the comments for the previous
         * implementation explaining why this special case is
         * necessary:
         *
         * -1 raised to an exact integer should never be exceptional.
         * Alas, some libms (chiefly glibc as of early 2003) return
         * NaN and set EDOM on pow(-1, large_int) if the int doesn't
         * happen to be representable in a *C* integer.  That's a
         * bug.
         */
        *res = (negate_result ? -1.0 : 1.0);
        return 0;
    }

    /* Now iv and iw are finite, iw is nonzero, and iv is
     * positive and not equal to 1.0.  We finally allow
     * the platform pow to step in and do the rest.
     */
    errno = 0;
    PyFPE_START_PROTECT("pow", return 1)
    ix = pow(iv, iw);
    PyFPE_END_PROTECT(ix)
    Py_ADJUST_ERANGE1(ix);
    if (negate_result)
        ix = -ix;

    if (errno != 0) {
        /* We don't expect any errno value other than ERANGE, but
         * the range of libm bugs appears unbounded.
         */
        PyErr_SetFromErrno(errno == ERANGE ? PyExc_OverflowError :
                             PyExc_ValueError);
        return 1;
    }
    *res = ix;
    return 0;
}

#undef DOUBLE_IS_ODD_INTEGER

static PyObject *
float_neg(PyFloatObject *v)
{
    return PyFloat_FromDouble(-v->ob_fval);
}

static PyObject *
float_abs(PyFloatObject *v)
{
    return PyFloat_FromDouble(fabs(v->ob_fval));
}

static int
float_nonzero(PyFloatObject *v)
{
    return v->ob_fval != 0.0;
}

static int
float_coerce(PyObject **pv, PyObject **pw)
{
    if (PyInt_Check(*pw)) {
        long x = PyInt_AsLong(*pw);
        *pw = PyFloat_FromDouble((double)x);
        Py_INCREF(*pv);
        return 0;
    }
    else if (PyLong_Check(*pw)) {
        double x = PyLong_AsDouble(*pw);
        if (x == -1.0 && PyErr_Occurred())
            return -1;
        *pw = PyFloat_FromDouble(x);
        Py_INCREF(*pv);
        return 0;
    }
    else if (PyFloat_Check(*pw)) {
        Py_INCREF(*pv);
        Py_INCREF(*pw);
        return 0;
    }
    return 1; /* Can't do it */
}

// pyston change: make not static
PyObject *
float_is_integer(PyObject *v)
{
    double x = PyFloat_AsDouble(v);
    PyObject *o;

    if (x == -1.0 && PyErr_Occurred())
        return NULL;
    if (!Py_IS_FINITE(x))
        Py_RETURN_FALSE;
    errno = 0;
    PyFPE_START_PROTECT("is_integer", return NULL)
    o = (floor(x) == x) ? Py_True : Py_False;
    PyFPE_END_PROTECT(x)
    if (errno != 0) {
        PyErr_SetFromErrno(errno == ERANGE ? PyExc_OverflowError :
                             PyExc_ValueError);
        return NULL;
    }
    Py_INCREF(o);
    return o;
}

#if 0
static PyObject *
float_is_inf(PyObject *v)
{
    double x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred())
        return NULL;
    return PyBool_FromLong((long)Py_IS_INFINITY(x));
}

static PyObject *
float_is_nan(PyObject *v)
{
    double x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred())
        return NULL;
    return PyBool_FromLong((long)Py_IS_NAN(x));
}

static PyObject *
float_is_finite(PyObject *v)
{
    double x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred())
        return NULL;
    return PyBool_FromLong((long)Py_IS_FINITE(x));
}
#endif

static PyObject *
float_trunc(PyObject *v)
{
    double x = PyFloat_AsDouble(v);
    double wholepart;           /* integral portion of x, rounded toward 0 */

    (void)modf(x, &wholepart);
    /* Try to get out cheap if this fits in a Python int.  The attempt
     * to cast to long must be protected, as C doesn't define what
     * happens if the double is too big to fit in a long.  Some rare
     * systems raise an exception then (RISCOS was mentioned as one,
     * and someone using a non-default option on Sun also bumped into
     * that).  Note that checking for <= LONG_MAX is unsafe: if a long
     * has more bits of precision than a double, casting LONG_MAX to
     * double may yield an approximation, and if that's rounded up,
     * then, e.g., wholepart=LONG_MAX+1 would yield true from the C
     * expression wholepart<=LONG_MAX, despite that wholepart is
     * actually greater than LONG_MAX.  However, assuming a two's complement
     * machine with no trap representation, LONG_MIN will be a power of 2 (and
     * hence exactly representable as a double), and LONG_MAX = -1-LONG_MIN, so
     * the comparisons with (double)LONG_MIN below should be safe.
     */
    if ((double)LONG_MIN <= wholepart && wholepart < -(double)LONG_MIN) {
        const long aslong = (long)wholepart;
        return PyInt_FromLong(aslong);
    }
    return PyLong_FromDouble(wholepart);
}

static PyObject *
float_long(PyObject *v)
{
    double x = PyFloat_AsDouble(v);
    return PyLong_FromDouble(x);
}

/* _Py_double_round: rounds a finite nonzero double to the closest multiple of
   10**-ndigits; here ndigits is within reasonable bounds (typically, -308 <=
   ndigits <= 323).  Returns a Python float, or sets a Python error and
   returns NULL on failure (OverflowError and memory errors are possible). */

#ifndef PY_NO_SHORT_FLOAT_REPR
/* version of _Py_double_round that uses the correctly-rounded string<->double
   conversions from Python/dtoa.c */

/* FIVE_POW_LIMIT is the largest k such that 5**k is exactly representable as
   a double.  Since we're using the code in Python/dtoa.c, it should be safe
   to assume that C doubles are IEEE 754 binary64 format.  To be on the safe
   side, we check this. */
#if DBL_MANT_DIG == 53
#define FIVE_POW_LIMIT 22
#else
#error "C doubles do not appear to be IEEE 754 binary64 format"
#endif

// pyston change: comment this out
#if 0
PyObject *
_Py_double_round(double x, int ndigits) {

    double rounded, m;
    Py_ssize_t buflen, mybuflen=100;
    char *buf, *buf_end, shortbuf[100], *mybuf=shortbuf;
    int decpt, sign, val, halfway_case;
    PyObject *result = NULL;
    _Py_SET_53BIT_PRECISION_HEADER;

    /* Easy path for the common case ndigits == 0. */
    if (ndigits == 0) {
        rounded = round(x);
        if (fabs(rounded - x) == 0.5)
            /* halfway between two integers; use round-away-from-zero */
            rounded = x + (x > 0.0 ? 0.5 : -0.5);
        return PyFloat_FromDouble(rounded);
    }

    /* The basic idea is very simple: convert and round the double to a
       decimal string using _Py_dg_dtoa, then convert that decimal string
       back to a double with _Py_dg_strtod.  There's one minor difficulty:
       Python 2.x expects round to do round-half-away-from-zero, while
       _Py_dg_dtoa does round-half-to-even.  So we need some way to detect
       and correct the halfway cases.

       Detection: a halfway value has the form k * 0.5 * 10**-ndigits for
       some odd integer k.  Or in other words, a rational number x is
       exactly halfway between two multiples of 10**-ndigits if its
       2-valuation is exactly -ndigits-1 and its 5-valuation is at least
       -ndigits.  For ndigits >= 0 the latter condition is automatically
       satisfied for a binary float x, since any such float has
       nonnegative 5-valuation.  For 0 > ndigits >= -22, x needs to be an
       integral multiple of 5**-ndigits; we can check this using fmod.
       For -22 > ndigits, there are no halfway cases: 5**23 takes 54 bits
       to represent exactly, so any odd multiple of 0.5 * 10**n for n >=
       23 takes at least 54 bits of precision to represent exactly.

       Correction: a simple strategy for dealing with halfway cases is to
       (for the halfway cases only) call _Py_dg_dtoa with an argument of
       ndigits+1 instead of ndigits (thus doing an exact conversion to
       decimal), round the resulting string manually, and then convert
       back using _Py_dg_strtod.
    */

    /* nans, infinities and zeros should have already been dealt
       with by the caller (in this case, builtin_round) */
    assert(Py_IS_FINITE(x) && x != 0.0);

    /* find 2-valuation val of x */
    m = frexp(x, &val);
    while (m != floor(m)) {
        m *= 2.0;
        val--;
    }

    /* determine whether this is a halfway case */
    if (val == -ndigits-1) {
        if (ndigits >= 0)
            halfway_case = 1;
        else if (ndigits >= -FIVE_POW_LIMIT) {
            double five_pow = 1.0;
            int i;
            for (i=0; i < -ndigits; i++)
                five_pow *= 5.0;
            halfway_case = fmod(x, five_pow) == 0.0;
        }
        else
            halfway_case = 0;
    }
    else
        halfway_case = 0;

    /* round to a decimal string; use an extra place for halfway case */
    _Py_SET_53BIT_PRECISION_START;
    buf = _Py_dg_dtoa(x, 3, ndigits+halfway_case, &decpt, &sign, &buf_end);
    _Py_SET_53BIT_PRECISION_END;
    if (buf == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    buflen = buf_end - buf;

    /* in halfway case, do the round-half-away-from-zero manually */
    if (halfway_case) {
        int i, carry;
        /* sanity check: _Py_dg_dtoa should not have stripped
           any zeros from the result: there should be exactly
           ndigits+1 places following the decimal point, and
           the last digit in the buffer should be a '5'.*/
        assert(buflen - decpt == ndigits+1);
        assert(buf[buflen-1] == '5');

        /* increment and shift right at the same time. */
        decpt += 1;
        carry = 1;
        for (i=buflen-1; i-- > 0;) {
            carry += buf[i] - '0';
            buf[i+1] = carry % 10 + '0';
            carry /= 10;
        }
        buf[0] = carry + '0';
    }

    /* Get new buffer if shortbuf is too small.  Space needed <= buf_end -
       buf + 8: (1 extra for '0', 1 for sign, 5 for exp, 1 for '\0'). */
    if (buflen + 8 > mybuflen) {
        mybuflen = buflen+8;
        mybuf = (char *)PyMem_Malloc(mybuflen);
        if (mybuf == NULL) {
            PyErr_NoMemory();
            goto exit;
        }
    }
    /* copy buf to mybuf, adding exponent, sign and leading 0 */
    PyOS_snprintf(mybuf, mybuflen, "%s0%se%d", (sign ? "-" : ""),
                  buf, decpt - (int)buflen);

    /* and convert the resulting string back to a double */
    errno = 0;
    _Py_SET_53BIT_PRECISION_START;
    rounded = _Py_dg_strtod(mybuf, NULL);
    _Py_SET_53BIT_PRECISION_END;
    if (errno == ERANGE && fabs(rounded) >= 1.)
        PyErr_SetString(PyExc_OverflowError,
                        "rounded value too large to represent");
    else
        result = PyFloat_FromDouble(rounded);

    /* done computing value;  now clean up */
    if (mybuf != shortbuf)
        PyMem_Free(mybuf);
  exit:
    _Py_dg_freedtoa(buf);
    return result;
}
#endif

#undef FIVE_POW_LIMIT

#else /* PY_NO_SHORT_FLOAT_REPR */

/* fallback version, to be used when correctly rounded binary<->decimal
   conversions aren't available */

PyObject *
_Py_double_round(double x, int ndigits) {
    double pow1, pow2, y, z;
    if (ndigits >= 0) {
        if (ndigits > 22) {
            /* pow1 and pow2 are each safe from overflow, but
               pow1*pow2 ~= pow(10.0, ndigits) might overflow */
            pow1 = pow(10.0, (double)(ndigits-22));
            pow2 = 1e22;
        }
        else {
            pow1 = pow(10.0, (double)ndigits);
            pow2 = 1.0;
        }
        y = (x*pow1)*pow2;
        /* if y overflows, then rounded value is exactly x */
        if (!Py_IS_FINITE(y))
            return PyFloat_FromDouble(x);
    }
    else {
        pow1 = pow(10.0, (double)-ndigits);
        pow2 = 1.0; /* unused; silences a gcc compiler warning */
        y = x / pow1;
    }

    z = round(y);
    if (fabs(y-z) == 0.5)
        /* halfway between two integers; use round-away-from-zero */
        z = y + copysign(0.5, y);

    if (ndigits >= 0)
        z = (z / pow2) / pow1;
    else
        z *= pow1;

    /* if computation resulted in overflow, raise OverflowError */
    if (!Py_IS_FINITE(z)) {
        PyErr_SetString(PyExc_OverflowError,
                        "overflow occurred during round");
        return NULL;
    }

    return PyFloat_FromDouble(z);
}

#endif /* PY_NO_SHORT_FLOAT_REPR */

static PyObject *
float_float(PyObject *v)
{
    if (PyFloat_CheckExact(v))
        Py_INCREF(v);
    else
        v = PyFloat_FromDouble(((PyFloatObject *)v)->ob_fval);
    return v;
}

/* turn ASCII hex characters into integer values and vice versa */

static char
char_from_hex(int x)
{
    assert(0 <= x && x < 16);
    return "0123456789abcdef"[x];
}

static int
hex_from_char(char c) {
    int x;
    switch(c) {
    case '0':
        x = 0;
        break;
    case '1':
        x = 1;
        break;
    case '2':
        x = 2;
        break;
    case '3':
        x = 3;
        break;
    case '4':
        x = 4;
        break;
    case '5':
        x = 5;
        break;
    case '6':
        x = 6;
        break;
    case '7':
        x = 7;
        break;
    case '8':
        x = 8;
        break;
    case '9':
        x = 9;
        break;
    case 'a':
    case 'A':
        x = 10;
        break;
    case 'b':
    case 'B':
        x = 11;
        break;
    case 'c':
    case 'C':
        x = 12;
        break;
    case 'd':
    case 'D':
        x = 13;
        break;
    case 'e':
    case 'E':
        x = 14;
        break;
    case 'f':
    case 'F':
        x = 15;
        break;
    default:
        x = -1;
        break;
    }
    return x;
}

/* convert a float to a hexadecimal string */

/* TOHEX_NBITS is DBL_MANT_DIG rounded up to the next integer
   of the form 4k+1. */
#define TOHEX_NBITS DBL_MANT_DIG + 3 - (DBL_MANT_DIG+2)%4

// pyston change: make this not static
PyObject *
float_hex(PyObject *v)
{
    double x, m;
    int e, shift, i, si, esign;
    /* Space for 1+(TOHEX_NBITS-1)/4 digits, a decimal point, and the
       trailing NUL byte. */
    char s[(TOHEX_NBITS-1)/4+3];

    CONVERT_TO_DOUBLE(v, x);

    if (Py_IS_NAN(x) || Py_IS_INFINITY(x))
        return float_str((PyFloatObject *)v);

    if (x == 0.0) {
        if (copysign(1.0, x) == -1.0)
            return PyString_FromString("-0x0.0p+0");
        else
            return PyString_FromString("0x0.0p+0");
    }

    m = frexp(fabs(x), &e);
    shift = 1 - MAX(DBL_MIN_EXP - e, 0);
    m = ldexp(m, shift);
    e -= shift;

    si = 0;
    s[si] = char_from_hex((int)m);
    si++;
    m -= (int)m;
    s[si] = '.';
    si++;
    for (i=0; i < (TOHEX_NBITS-1)/4; i++) {
        m *= 16.0;
        s[si] = char_from_hex((int)m);
        si++;
        m -= (int)m;
    }
    s[si] = '\0';

    if (e < 0) {
        esign = (int)'-';
        e = -e;
    }
    else
        esign = (int)'+';

    if (x < 0.0)
        return PyString_FromFormat("-0x%sp%c%d", s, esign, e);
    else
        return PyString_FromFormat("0x%sp%c%d", s, esign, e);
}

PyDoc_STRVAR(float_hex_doc,
"float.hex() -> string\n\
\n\
Return a hexadecimal representation of a floating-point number.\n\
>>> (-0.1).hex()\n\
'-0x1.999999999999ap-4'\n\
>>> 3.14159.hex()\n\
'0x1.921f9f01b866ep+1'");

/* Case-insensitive locale-independent string match used for nan and inf
   detection. t should be lower-case and null-terminated.  Return a nonzero
   result if the first strlen(t) characters of s match t and 0 otherwise. */

static int
case_insensitive_match(const char *s, const char *t)
{
    while(*t && Py_TOLOWER(*s) == *t) {
        s++;
        t++;
    }
    return *t ? 0 : 1;
}

/* Convert a hexadecimal string to a float. */

// pyston change: make this not static
PyObject *
float_fromhex(PyObject *cls, PyObject *arg)
{
    PyObject *result_as_float, *result;
    double x;
    long exp, top_exp, lsb, key_digit;
    char *s, *coeff_start, *s_store, *coeff_end, *exp_start, *s_end;
    int half_eps, digit, round_up, sign=1;
    Py_ssize_t length, ndigits, fdigits, i;

    /*
     * For the sake of simplicity and correctness, we impose an artificial
     * limit on ndigits, the total number of hex digits in the coefficient
     * The limit is chosen to ensure that, writing exp for the exponent,
     *
     *   (1) if exp > LONG_MAX/2 then the value of the hex string is
     *   guaranteed to overflow (provided it's nonzero)
     *
     *   (2) if exp < LONG_MIN/2 then the value of the hex string is
     *   guaranteed to underflow to 0.
     *
     *   (3) if LONG_MIN/2 <= exp <= LONG_MAX/2 then there's no danger of
     *   overflow in the calculation of exp and top_exp below.
     *
     * More specifically, ndigits is assumed to satisfy the following
     * inequalities:
     *
     *   4*ndigits <= DBL_MIN_EXP - DBL_MANT_DIG - LONG_MIN/2
     *   4*ndigits <= LONG_MAX/2 + 1 - DBL_MAX_EXP
     *
     * If either of these inequalities is not satisfied, a ValueError is
     * raised.  Otherwise, write x for the value of the hex string, and
     * assume x is nonzero.  Then
     *
     *   2**(exp-4*ndigits) <= |x| < 2**(exp+4*ndigits).
     *
     * Now if exp > LONG_MAX/2 then:
     *
     *   exp - 4*ndigits >= LONG_MAX/2 + 1 - (LONG_MAX/2 + 1 - DBL_MAX_EXP)
     *                    = DBL_MAX_EXP
     *
     * so |x| >= 2**DBL_MAX_EXP, which is too large to be stored in C
     * double, so overflows.  If exp < LONG_MIN/2, then
     *
     *   exp + 4*ndigits <= LONG_MIN/2 - 1 + (
     *                      DBL_MIN_EXP - DBL_MANT_DIG - LONG_MIN/2)
     *                    = DBL_MIN_EXP - DBL_MANT_DIG - 1
     *
     * and so |x| < 2**(DBL_MIN_EXP-DBL_MANT_DIG-1), hence underflows to 0
     * when converted to a C double.
     *
     * It's easy to show that if LONG_MIN/2 <= exp <= LONG_MAX/2 then both
     * exp+4*ndigits and exp-4*ndigits are within the range of a long.
     */

    if (PyString_AsStringAndSize(arg, &s, &length))
        return NULL;
    s_end = s + length;

    /********************
     * Parse the string *
     ********************/

    /* leading whitespace and optional sign */
    while (Py_ISSPACE(*s))
        s++;
    if (*s == '-') {
        s++;
        sign = -1;
    }
    else if (*s == '+')
        s++;

    /* infinities and nans */
    if (*s == 'i' || *s == 'I') {
        if (!case_insensitive_match(s+1, "nf"))
            goto parse_error;
        s += 3;
        x = Py_HUGE_VAL;
        if (case_insensitive_match(s, "inity"))
            s += 5;
        goto finished;
    }
    if (*s == 'n' || *s == 'N') {
        if (!case_insensitive_match(s+1, "an"))
            goto parse_error;
        s += 3;
        x = Py_NAN;
        goto finished;
    }

    /* [0x] */
    s_store = s;
    if (*s == '0') {
        s++;
        if (*s == 'x' || *s == 'X')
            s++;
        else
            s = s_store;
    }

    /* coefficient: <integer> [. <fraction>] */
    coeff_start = s;
    while (hex_from_char(*s) >= 0)
        s++;
    s_store = s;
    if (*s == '.') {
        s++;
        while (hex_from_char(*s) >= 0)
            s++;
        coeff_end = s-1;
    }
    else
        coeff_end = s;

    /* ndigits = total # of hex digits; fdigits = # after point */
    ndigits = coeff_end - coeff_start;
    fdigits = coeff_end - s_store;
    if (ndigits == 0)
        goto parse_error;
    if (ndigits > MIN(DBL_MIN_EXP - DBL_MANT_DIG - LONG_MIN/2,
                      LONG_MAX/2 + 1 - DBL_MAX_EXP)/4)
        goto insane_length_error;

    /* [p <exponent>] */
    if (*s == 'p' || *s == 'P') {
        s++;
        exp_start = s;
        if (*s == '-' || *s == '+')
            s++;
        if (!('0' <= *s && *s <= '9'))
            goto parse_error;
        s++;
        while ('0' <= *s && *s <= '9')
            s++;
        exp = strtol(exp_start, NULL, 10);
    }
    else
        exp = 0;

/* for 0 <= j < ndigits, HEX_DIGIT(j) gives the jth most significant digit */
#define HEX_DIGIT(j) hex_from_char(*((j) < fdigits ?            \
                     coeff_end-(j) :                                    \
                     coeff_end-1-(j)))

    /*******************************************
     * Compute rounded value of the hex string *
     *******************************************/

    /* Discard leading zeros, and catch extreme overflow and underflow */
    while (ndigits > 0 && HEX_DIGIT(ndigits-1) == 0)
        ndigits--;
    if (ndigits == 0 || exp < LONG_MIN/2) {
        x = 0.0;
        goto finished;
    }
    if (exp > LONG_MAX/2)
        goto overflow_error;

    /* Adjust exponent for fractional part. */
    exp = exp - 4*((long)fdigits);

    /* top_exp = 1 more than exponent of most sig. bit of coefficient */
    top_exp = exp + 4*((long)ndigits - 1);
    for (digit = HEX_DIGIT(ndigits-1); digit != 0; digit /= 2)
        top_exp++;

    /* catch almost all nonextreme cases of overflow and underflow here */
    if (top_exp < DBL_MIN_EXP - DBL_MANT_DIG) {
        x = 0.0;
        goto finished;
    }
    if (top_exp > DBL_MAX_EXP)
        goto overflow_error;

    /* lsb = exponent of least significant bit of the *rounded* value.
       This is top_exp - DBL_MANT_DIG unless result is subnormal. */
    lsb = MAX(top_exp, (long)DBL_MIN_EXP) - DBL_MANT_DIG;

    x = 0.0;
    if (exp >= lsb) {
        /* no rounding required */
        for (i = ndigits-1; i >= 0; i--)
            x = 16.0*x + HEX_DIGIT(i);
        x = ldexp(x, (int)(exp));
        goto finished;
    }
    /* rounding required.  key_digit is the index of the hex digit
       containing the first bit to be rounded away. */
    half_eps = 1 << (int)((lsb - exp - 1) % 4);
    key_digit = (lsb - exp - 1) / 4;
    for (i = ndigits-1; i > key_digit; i--)
        x = 16.0*x + HEX_DIGIT(i);
    digit = HEX_DIGIT(key_digit);
    x = 16.0*x + (double)(digit & (16-2*half_eps));

    /* round-half-even: round up if bit lsb-1 is 1 and at least one of
       bits lsb, lsb-2, lsb-3, lsb-4, ... is 1. */
    if ((digit & half_eps) != 0) {
        round_up = 0;
        if ((digit & (3*half_eps-1)) != 0 ||
            (half_eps == 8 && (HEX_DIGIT(key_digit+1) & 1) != 0))
            round_up = 1;
        else
            for (i = key_digit-1; i >= 0; i--)
                if (HEX_DIGIT(i) != 0) {
                    round_up = 1;
                    break;
                }
        if (round_up == 1) {
            x += 2*half_eps;
            if (top_exp == DBL_MAX_EXP &&
                x == ldexp((double)(2*half_eps), DBL_MANT_DIG))
                /* overflow corner case: pre-rounded value <
                   2**DBL_MAX_EXP; rounded=2**DBL_MAX_EXP. */
                goto overflow_error;
        }
    }
    x = ldexp(x, (int)(exp+4*key_digit));

  finished:
    /* optional trailing whitespace leading to the end of the string */
    while (Py_ISSPACE(*s))
        s++;
    if (s != s_end)
        goto parse_error;
    result_as_float = Py_BuildValue("(d)", sign * x);
    if (result_as_float == NULL)
        return NULL;
    result = PyObject_CallObject(cls, result_as_float);
    Py_DECREF(result_as_float);
    return result;

  overflow_error:
    PyErr_SetString(PyExc_OverflowError,
                    "hexadecimal value too large to represent as a float");
    return NULL;

  parse_error:
    PyErr_SetString(PyExc_ValueError,
                    "invalid hexadecimal floating-point string");
    return NULL;

  insane_length_error:
    PyErr_SetString(PyExc_ValueError,
                    "hexadecimal string too long to convert");
    return NULL;
}

PyDoc_STRVAR(float_fromhex_doc,
"float.fromhex(string) -> float\n\
\n\
Create a floating-point number from a hexadecimal string.\n\
>>> float.fromhex('0x1.ffffp10')\n\
2047.984375\n\
>>> float.fromhex('-0x1p-1074')\n\
-4.9406564584124654e-324");


// pyston change: make not static
PyObject *
float_as_integer_ratio(PyObject *v, PyObject *unused)
{
    double self;
    double float_part;
    int exponent;
    int i;

    PyObject *prev;
    PyObject *py_exponent = NULL;
    PyObject *numerator = NULL;
    PyObject *denominator = NULL;
    PyObject *result_pair = NULL;
    PyNumberMethods *long_methods = PyLong_Type.tp_as_number;

#define INPLACE_UPDATE(obj, call) \
    prev = obj; \
    obj = call; \
    Py_DECREF(prev); \

    CONVERT_TO_DOUBLE(v, self);

    if (Py_IS_INFINITY(self)) {
      PyErr_SetString(PyExc_OverflowError,
                      "Cannot pass infinity to float.as_integer_ratio.");
      return NULL;
    }
#ifdef Py_NAN
    if (Py_IS_NAN(self)) {
      PyErr_SetString(PyExc_ValueError,
                      "Cannot pass NaN to float.as_integer_ratio.");
      return NULL;
    }
#endif

    PyFPE_START_PROTECT("as_integer_ratio", goto error);
    float_part = frexp(self, &exponent);        /* self == float_part * 2**exponent exactly */
    PyFPE_END_PROTECT(float_part);

    for (i=0; i<300 && float_part != floor(float_part) ; i++) {
        float_part *= 2.0;
        exponent--;
    }
    /* self == float_part * 2**exponent exactly and float_part is integral.
       If FLT_RADIX != 2, the 300 steps may leave a tiny fractional part
       to be truncated by PyLong_FromDouble(). */

    numerator = PyLong_FromDouble(float_part);
    if (numerator == NULL) goto error;

    /* fold in 2**exponent */
    denominator = PyLong_FromLong(1);
    py_exponent = PyLong_FromLong(labs((long)exponent));
    if (py_exponent == NULL) goto error;
    INPLACE_UPDATE(py_exponent,
                   long_methods->nb_lshift(denominator, py_exponent));
    if (py_exponent == NULL) goto error;
    if (exponent > 0) {
        INPLACE_UPDATE(numerator,
                       long_methods->nb_multiply(numerator, py_exponent));
        if (numerator == NULL) goto error;
    }
    else {
        Py_DECREF(denominator);
        denominator = py_exponent;
        py_exponent = NULL;
    }

    /* Returns ints instead of longs where possible */
    INPLACE_UPDATE(numerator, PyNumber_Int(numerator));
    if (numerator == NULL) goto error;
    INPLACE_UPDATE(denominator, PyNumber_Int(denominator));
    if (denominator == NULL) goto error;

    result_pair = PyTuple_Pack(2, numerator, denominator);

#undef INPLACE_UPDATE
error:
    Py_XDECREF(py_exponent);
    Py_XDECREF(denominator);
    Py_XDECREF(numerator);
    return result_pair;
}

PyDoc_STRVAR(float_as_integer_ratio_doc,
"float.as_integer_ratio() -> (int, int)\n"
"\n"
"Return a pair of integers, whose ratio is exactly equal to the original\n"
"float and with a positive denominator.\n"
"Raise OverflowError on infinities and a ValueError on NaNs.\n"
"\n"
">>> (10.0).as_integer_ratio()\n"
"(10, 1)\n"
">>> (0.0).as_integer_ratio()\n"
"(0, 1)\n"
">>> (-.25).as_integer_ratio()\n"
"(-1, 4)");


static PyObject *
float_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

static PyObject *
float_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *x = Py_False; /* Integer zero */
    static char *kwlist[] = {"x", 0};

    if (type != &PyFloat_Type)
        return float_subtype_new(type, args, kwds); /* Wimp out */
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:float", kwlist, &x))
        return NULL;
    /* If it's a string, but not a string subclass, use
       PyFloat_FromString. */
    if (PyString_CheckExact(x))
        return PyFloat_FromString(x, NULL);
    return PyNumber_Float(x);
}

/* Wimpy, slow approach to tp_new calls for subtypes of float:
   first create a regular float from whatever arguments we got,
   then allocate a subtype instance and initialize its ob_fval
   from the regular float.  The regular float is then thrown away.
*/
static PyObject *
float_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *tmp, *newobj;

    assert(PyType_IsSubtype(type, &PyFloat_Type));
    tmp = float_new(&PyFloat_Type, args, kwds);
    if (tmp == NULL)
        return NULL;
    assert(PyFloat_CheckExact(tmp));
    newobj = type->tp_alloc(type, 0);
    if (newobj == NULL) {
        Py_DECREF(tmp);
        return NULL;
    }
    ((PyFloatObject *)newobj)->ob_fval = ((PyFloatObject *)tmp)->ob_fval;
    Py_DECREF(tmp);
    return newobj;
}

static PyObject *
float_getnewargs(PyFloatObject *v)
{
    return Py_BuildValue("(d)", v->ob_fval);
}

/* this is for the benefit of the pack/unpack routines below */

typedef enum {
    unknown_format, ieee_big_endian_format, ieee_little_endian_format
} float_format_type;

static float_format_type double_format, float_format;
static float_format_type detected_double_format, detected_float_format;

static PyObject *
float_getformat(PyTypeObject *v, PyObject* arg)
{
    char* s;
    float_format_type r;

    if (!PyString_Check(arg)) {
        PyErr_Format(PyExc_TypeError,
         "__getformat__() argument must be string, not %.500s",
                         Py_TYPE(arg)->tp_name);
        return NULL;
    }
    s = PyString_AS_STRING(arg);
    if (strcmp(s, "double") == 0) {
        r = double_format;
    }
    else if (strcmp(s, "float") == 0) {
        r = float_format;
    }
    else {
        PyErr_SetString(PyExc_ValueError,
                        "__getformat__() argument 1 must be "
                        "'double' or 'float'");
        return NULL;
    }

    switch (r) {
    case unknown_format:
        return PyString_FromString("unknown");
    case ieee_little_endian_format:
        return PyString_FromString("IEEE, little-endian");
    case ieee_big_endian_format:
        return PyString_FromString("IEEE, big-endian");
    default:
        Py_FatalError("insane float_format or double_format");
        return NULL;
    }
}

PyDoc_STRVAR(float_getformat_doc,
"float.__getformat__(typestr) -> string\n"
"\n"
"You probably don't want to use this function.  It exists mainly to be\n"
"used in Python's test suite.\n"
"\n"
"typestr must be 'double' or 'float'.  This function returns whichever of\n"
"'unknown', 'IEEE, big-endian' or 'IEEE, little-endian' best describes the\n"
"format of floating point numbers used by the C type named by typestr.");

static PyObject *
float_setformat(PyTypeObject *v, PyObject* args)
{
    char* typestr;
    char* format;
    float_format_type f;
    float_format_type detected;
    float_format_type *p;

    if (!PyArg_ParseTuple(args, "ss:__setformat__", &typestr, &format))
        return NULL;

    if (strcmp(typestr, "double") == 0) {
        p = &double_format;
        detected = detected_double_format;
    }
    else if (strcmp(typestr, "float") == 0) {
        p = &float_format;
        detected = detected_float_format;
    }
    else {
        PyErr_SetString(PyExc_ValueError,
                        "__setformat__() argument 1 must "
                        "be 'double' or 'float'");
        return NULL;
    }

    if (strcmp(format, "unknown") == 0) {
        f = unknown_format;
    }
    else if (strcmp(format, "IEEE, little-endian") == 0) {
        f = ieee_little_endian_format;
    }
    else if (strcmp(format, "IEEE, big-endian") == 0) {
        f = ieee_big_endian_format;
    }
    else {
        PyErr_SetString(PyExc_ValueError,
                        "__setformat__() argument 2 must be "
                        "'unknown', 'IEEE, little-endian' or "
                        "'IEEE, big-endian'");
        return NULL;

    }

    if (f != unknown_format && f != detected) {
        PyErr_Format(PyExc_ValueError,
                     "can only set %s format to 'unknown' or the "
                     "detected platform value", typestr);
        return NULL;
    }

    *p = f;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(float_setformat_doc,
"float.__setformat__(typestr, fmt) -> None\n"
"\n"
"You probably don't want to use this function.  It exists mainly to be\n"
"used in Python's test suite.\n"
"\n"
"typestr must be 'double' or 'float'.  fmt must be one of 'unknown',\n"
"'IEEE, big-endian' or 'IEEE, little-endian', and in addition can only be\n"
"one of the latter two if it appears to match the underlying C reality.\n"
"\n"
"Override the automatic determination of C-level floating point type.\n"
"This affects how floats are converted to and from binary strings.");

static PyObject *
float_getzero(PyObject *v, void *closure)
{
    return PyFloat_FromDouble(0.0);
}

// pyston change: make not static
PyObject *
float__format__(PyObject *self, PyObject *args)
{
    PyObject *format_spec;

    if (!PyArg_ParseTuple(args, "O:__format__", &format_spec))
        return NULL;
    if (PyBytes_Check(format_spec))
        return _PyFloat_FormatAdvanced(self,
                                       PyBytes_AS_STRING(format_spec),
                                       PyBytes_GET_SIZE(format_spec));
    if (PyUnicode_Check(format_spec)) {
        /* Convert format_spec to a str */
        PyObject *result;
        PyObject *str_spec = PyObject_Str(format_spec);

        if (str_spec == NULL)
            return NULL;

        result = _PyFloat_FormatAdvanced(self,
                                         PyBytes_AS_STRING(str_spec),
                                         PyBytes_GET_SIZE(str_spec));

        Py_DECREF(str_spec);
        return result;
    }
    PyErr_SetString(PyExc_TypeError, "__format__ requires str or unicode");
    return NULL;
}

PyDoc_STRVAR(float__format__doc,
"float.__format__(format_spec) -> string\n"
"\n"
"Formats the float according to format_spec.");


static PyMethodDef float_methods[] = {
    {"conjugate",       (PyCFunction)float_float,       METH_NOARGS,
     "Return self, the complex conjugate of any float."},
    {"__trunc__",       (PyCFunction)float_trunc, METH_NOARGS,
     "Return the Integral closest to x between 0 and x."},
    {"as_integer_ratio", (PyCFunction)float_as_integer_ratio, METH_NOARGS,
     float_as_integer_ratio_doc},
    {"fromhex", (PyCFunction)float_fromhex,
     METH_O|METH_CLASS, float_fromhex_doc},
    {"hex", (PyCFunction)float_hex,
     METH_NOARGS, float_hex_doc},
    {"is_integer",      (PyCFunction)float_is_integer,  METH_NOARGS,
     "Return True if the float is an integer."},
#if 0
    {"is_inf",          (PyCFunction)float_is_inf,      METH_NOARGS,
     "Return True if the float is positive or negative infinite."},
    {"is_finite",       (PyCFunction)float_is_finite,   METH_NOARGS,
     "Return True if the float is finite, neither infinite nor NaN."},
    {"is_nan",          (PyCFunction)float_is_nan,      METH_NOARGS,
     "Return True if the float is not a number (NaN)."},
#endif
    {"__getnewargs__",          (PyCFunction)float_getnewargs,  METH_NOARGS},
    {"__getformat__",           (PyCFunction)float_getformat,
     METH_O|METH_CLASS,                 float_getformat_doc},
    {"__setformat__",           (PyCFunction)float_setformat,
     METH_VARARGS|METH_CLASS,           float_setformat_doc},
    {"__format__",          (PyCFunction)float__format__,
     METH_VARARGS,                  float__format__doc},
    {NULL,              NULL}           /* sentinel */
};

static PyGetSetDef float_getset[] = {
    {"real",
     (getter)float_float, (setter)NULL,
     "the real part of a complex number",
     NULL},
    {"imag",
     (getter)float_getzero, (setter)NULL,
     "the imaginary part of a complex number",
     NULL},
    {NULL}  /* Sentinel */
};

PyDoc_STRVAR(float_doc,
"float(x) -> floating point number\n\
\n\
Convert a string or number to a floating point number, if possible.");


// Pyston change: don't need this for now
#if 0
static PyNumberMethods float_as_number = {
    float_add,          /*nb_add*/
    float_sub,          /*nb_subtract*/
    float_mul,          /*nb_multiply*/
    float_classic_div, /*nb_divide*/
    float_rem,          /*nb_remainder*/
    float_divmod,       /*nb_divmod*/
    float_pow,          /*nb_power*/
    (unaryfunc)float_neg, /*nb_negative*/
    (unaryfunc)float_float, /*nb_positive*/
    (unaryfunc)float_abs, /*nb_absolute*/
    (inquiry)float_nonzero, /*nb_nonzero*/
    0,                  /*nb_invert*/
    0,                  /*nb_lshift*/
    0,                  /*nb_rshift*/
    0,                  /*nb_and*/
    0,                  /*nb_xor*/
    0,                  /*nb_or*/
    float_coerce,       /*nb_coerce*/
    float_trunc,        /*nb_int*/
    float_long,         /*nb_long*/
    float_float,        /*nb_float*/
    0,                  /* nb_oct */
    0,                  /* nb_hex */
    0,                  /* nb_inplace_add */
    0,                  /* nb_inplace_subtract */
    0,                  /* nb_inplace_multiply */
    0,                  /* nb_inplace_divide */
    0,                  /* nb_inplace_remainder */
    0,                  /* nb_inplace_power */
    0,                  /* nb_inplace_lshift */
    0,                  /* nb_inplace_rshift */
    0,                  /* nb_inplace_and */
    0,                  /* nb_inplace_xor */
    0,                  /* nb_inplace_or */
    float_floor_div, /* nb_floor_divide */
    float_div,          /* nb_true_divide */
    0,                  /* nb_inplace_floor_divide */
    0,                  /* nb_inplace_true_divide */
};
#endif

// pyston change: don't need this
#if 0
PyTypeObject PyFloat_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "float",
    sizeof(PyFloatObject),
    0,
    (destructor)float_dealloc,                  /* tp_dealloc */
    (printfunc)float_print,                     /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    (reprfunc)float_repr,                       /* tp_repr */
    &float_as_number,                           /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    (hashfunc)float_hash,                       /* tp_hash */
    0,                                          /* tp_call */
    (reprfunc)float_str,                        /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_CHECKTYPES |
        Py_TPFLAGS_BASETYPE,                    /* tp_flags */
    float_doc,                                  /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    float_richcompare,                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    float_methods,                              /* tp_methods */
    0,                                          /* tp_members */
    float_getset,                               /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    float_new,                                  /* tp_new */
};

void
_PyFloat_Init(void)
{
    /* We attempt to determine if this machine is using IEEE
       floating point formats by peering at the bits of some
       carefully chosen values.  If it looks like we are on an
       IEEE platform, the float packing/unpacking routines can
       just copy bits, if not they resort to arithmetic & shifts
       and masks.  The shifts & masks approach works on all finite
       values, but what happens to infinities, NaNs and signed
       zeroes on packing is an accident, and attempting to unpack
       a NaN or an infinity will raise an exception.

       Note that if we're on some whacked-out platform which uses
       IEEE formats but isn't strictly little-endian or big-
       endian, we will fall back to the portable shifts & masks
       method. */

#if SIZEOF_DOUBLE == 8
    {
        double x = 9006104071832581.0;
        if (memcmp(&x, "\x43\x3f\xff\x01\x02\x03\x04\x05", 8) == 0)
            detected_double_format = ieee_big_endian_format;
        else if (memcmp(&x, "\x05\x04\x03\x02\x01\xff\x3f\x43", 8) == 0)
            detected_double_format = ieee_little_endian_format;
        else
            detected_double_format = unknown_format;
    }
#else
    detected_double_format = unknown_format;
#endif

#if SIZEOF_FLOAT == 4
    {
        float y = 16711938.0;
        if (memcmp(&y, "\x4b\x7f\x01\x02", 4) == 0)
            detected_float_format = ieee_big_endian_format;
        else if (memcmp(&y, "\x02\x01\x7f\x4b", 4) == 0)
            detected_float_format = ieee_little_endian_format;
        else
            detected_float_format = unknown_format;
    }
#else
    detected_float_format = unknown_format;
#endif

    double_format = detected_double_format;
    float_format = detected_float_format;

    /* Init float info */
    if (FloatInfoType.tp_name == 0)
        PyStructSequence_InitType(&FloatInfoType, &floatinfo_desc);
}

int
PyFloat_ClearFreeList(void)
{
    PyFloatObject *p;
    PyFloatBlock *list, *next;
    int i;
    int u;                      /* remaining unfreed ints per block */
    int freelist_size = 0;

    list = block_list;
    block_list = NULL;
    free_list = NULL;
    while (list != NULL) {
        u = 0;
        for (i = 0, p = &list->objects[0];
             i < N_FLOATOBJECTS;
             i++, p++) {
            if (PyFloat_CheckExact(p) && Py_REFCNT(p) != 0)
                u++;
        }
        next = list->next;
        if (u) {
            list->next = block_list;
            block_list = list;
            for (i = 0, p = &list->objects[0];
                 i < N_FLOATOBJECTS;
                 i++, p++) {
                if (!PyFloat_CheckExact(p) ||
                    Py_REFCNT(p) == 0) {
                    Py_TYPE(p) = (struct _typeobject *)
                        free_list;
                    free_list = p;
                }
            }
        }
        else {
            PyMem_FREE(list);
        }
        freelist_size += u;
        list = next;
    }
    return freelist_size;
}

void
PyFloat_Fini(void)
{
    PyFloatObject *p;
    PyFloatBlock *list;
    int i;
    int u;                      /* total unfreed floats per block */

    u = PyFloat_ClearFreeList();

    if (!Py_VerboseFlag)
        return;
    fprintf(stderr, "# cleanup floats");
    if (!u) {
        fprintf(stderr, "\n");
    }
    else {
        fprintf(stderr,
            ": %d unfreed float%s\n",
            u, u == 1 ? "" : "s");
    }
    if (Py_VerboseFlag > 1) {
        list = block_list;
        while (list != NULL) {
            for (i = 0, p = &list->objects[0];
                 i < N_FLOATOBJECTS;
                 i++, p++) {
                if (PyFloat_CheckExact(p) &&
                    Py_REFCNT(p) != 0) {
                    char *buf = PyOS_double_to_string(
                        PyFloat_AS_DOUBLE(p), 'r',
                        0, 0, NULL);
                    if (buf) {
                        /* XXX(twouters) cast
                           refcount to long
                           until %zd is
                           universally
                           available
                        */
                        fprintf(stderr,
                 "#   <float at %p, refcnt=%ld, val=%s>\n",
                                    p, (long)Py_REFCNT(p), buf);
                                    PyMem_Free(buf);
                            }
                }
            }
            list = list->next;
        }
    }
}
#endif

// pyston change: comment this out
#if 0
/*----------------------------------------------------------------------------
 * _PyFloat_{Pack,Unpack}{4,8}.  See floatobject.h.
 */
int
_PyFloat_Pack4(double x, unsigned char *p, int le)
{
    if (float_format == unknown_format) {
        unsigned char sign;
        int e;
        double f;
        unsigned int fbits;
        int incr = 1;

        if (le) {
            p += 3;
            incr = -1;
        }

        if (x < 0) {
            sign = 1;
            x = -x;
        }
        else
            sign = 0;

        f = frexp(x, &e);

        /* Normalize f to be in the range [1.0, 2.0) */
        if (0.5 <= f && f < 1.0) {
            f *= 2.0;
            e--;
        }
        else if (f == 0.0)
            e = 0;
        else {
            PyErr_SetString(PyExc_SystemError,
                            "frexp() result out of range");
            return -1;
        }

        if (e >= 128)
            goto Overflow;
        else if (e < -126) {
            /* Gradual underflow */
            f = ldexp(f, 126 + e);
            e = 0;
        }
        else if (!(e == 0 && f == 0.0)) {
            e += 127;
            f -= 1.0; /* Get rid of leading 1 */
        }

        f *= 8388608.0; /* 2**23 */
        fbits = (unsigned int)(f + 0.5); /* Round */
        assert(fbits <= 8388608);
        if (fbits >> 23) {
            /* The carry propagated out of a string of 23 1 bits. */
            fbits = 0;
            ++e;
            if (e >= 255)
                goto Overflow;
        }

        /* First byte */
        *p = (sign << 7) | (e >> 1);
        p += incr;

        /* Second byte */
        *p = (char) (((e & 1) << 7) | (fbits >> 16));
        p += incr;

        /* Third byte */
        *p = (fbits >> 8) & 0xFF;
        p += incr;

        /* Fourth byte */
        *p = fbits & 0xFF;

        /* Done */
        return 0;

    }
    else {
        float y = (float)x;
        const char *s = (char*)&y;
        int i, incr = 1;

        if (Py_IS_INFINITY(y) && !Py_IS_INFINITY(x))
            goto Overflow;

        if ((float_format == ieee_little_endian_format && !le)
            || (float_format == ieee_big_endian_format && le)) {
            p += 3;
            incr = -1;
        }

        for (i = 0; i < 4; i++) {
            *p = *s++;
            p += incr;
        }
        return 0;
    }
  Overflow:
    PyErr_SetString(PyExc_OverflowError,
                    "float too large to pack with f format");
    return -1;
}

int
_PyFloat_Pack8(double x, unsigned char *p, int le)
{
    if (double_format == unknown_format) {
        unsigned char sign;
        int e;
        double f;
        unsigned int fhi, flo;
        int incr = 1;

        if (le) {
            p += 7;
            incr = -1;
        }

        if (x < 0) {
            sign = 1;
            x = -x;
        }
        else
            sign = 0;

        f = frexp(x, &e);

        /* Normalize f to be in the range [1.0, 2.0) */
        if (0.5 <= f && f < 1.0) {
            f *= 2.0;
            e--;
        }
        else if (f == 0.0)
            e = 0;
        else {
            PyErr_SetString(PyExc_SystemError,
                            "frexp() result out of range");
            return -1;
        }

        if (e >= 1024)
            goto Overflow;
        else if (e < -1022) {
            /* Gradual underflow */
            f = ldexp(f, 1022 + e);
            e = 0;
        }
        else if (!(e == 0 && f == 0.0)) {
            e += 1023;
            f -= 1.0; /* Get rid of leading 1 */
        }

        /* fhi receives the high 28 bits; flo the low 24 bits (== 52 bits) */
        f *= 268435456.0; /* 2**28 */
        fhi = (unsigned int)f; /* Truncate */
        assert(fhi < 268435456);

        f -= (double)fhi;
        f *= 16777216.0; /* 2**24 */
        flo = (unsigned int)(f + 0.5); /* Round */
        assert(flo <= 16777216);
        if (flo >> 24) {
            /* The carry propagated out of a string of 24 1 bits. */
            flo = 0;
            ++fhi;
            if (fhi >> 28) {
                /* And it also progagated out of the next 28 bits. */
                fhi = 0;
                ++e;
                if (e >= 2047)
                    goto Overflow;
            }
        }

        /* First byte */
        *p = (sign << 7) | (e >> 4);
        p += incr;

        /* Second byte */
        *p = (unsigned char) (((e & 0xF) << 4) | (fhi >> 24));
        p += incr;

        /* Third byte */
        *p = (fhi >> 16) & 0xFF;
        p += incr;

        /* Fourth byte */
        *p = (fhi >> 8) & 0xFF;
        p += incr;

        /* Fifth byte */
        *p = fhi & 0xFF;
        p += incr;

        /* Sixth byte */
        *p = (flo >> 16) & 0xFF;
        p += incr;

        /* Seventh byte */
        *p = (flo >> 8) & 0xFF;
        p += incr;

        /* Eighth byte */
        *p = flo & 0xFF;
        /* p += incr; Unneeded (for now) */

        /* Done */
        return 0;

      Overflow:
        PyErr_SetString(PyExc_OverflowError,
                        "float too large to pack with d format");
        return -1;
    }
    else {
        const char *s = (char*)&x;
        int i, incr = 1;

        if ((double_format == ieee_little_endian_format && !le)
            || (double_format == ieee_big_endian_format && le)) {
            p += 7;
            incr = -1;
        }

        for (i = 0; i < 8; i++) {
            *p = *s++;
            p += incr;
        }
        return 0;
    }
}

double
_PyFloat_Unpack4(const unsigned char *p, int le)
{
    if (float_format == unknown_format) {
        unsigned char sign;
        int e;
        unsigned int f;
        double x;
        int incr = 1;

        if (le) {
            p += 3;
            incr = -1;
        }

        /* First byte */
        sign = (*p >> 7) & 1;
        e = (*p & 0x7F) << 1;
        p += incr;

        /* Second byte */
        e |= (*p >> 7) & 1;
        f = (*p & 0x7F) << 16;
        p += incr;

        if (e == 255) {
            PyErr_SetString(
                PyExc_ValueError,
                "can't unpack IEEE 754 special value "
                "on non-IEEE platform");
            return -1;
        }

        /* Third byte */
        f |= *p << 8;
        p += incr;

        /* Fourth byte */
        f |= *p;

        x = (double)f / 8388608.0;

        /* XXX This sadly ignores Inf/NaN issues */
        if (e == 0)
            e = -126;
        else {
            x += 1.0;
            e -= 127;
        }
        x = ldexp(x, e);

        if (sign)
            x = -x;

        return x;
    }
    else {
        float x;

        if ((float_format == ieee_little_endian_format && !le)
            || (float_format == ieee_big_endian_format && le)) {
            char buf[4];
            char *d = &buf[3];
            int i;

            for (i = 0; i < 4; i++) {
                *d-- = *p++;
            }
            memcpy(&x, buf, 4);
        }
        else {
            memcpy(&x, p, 4);
        }

        return x;
    }
}

double
_PyFloat_Unpack8(const unsigned char *p, int le)
{
    if (double_format == unknown_format) {
        unsigned char sign;
        int e;
        unsigned int fhi, flo;
        double x;
        int incr = 1;

        if (le) {
            p += 7;
            incr = -1;
        }

        /* First byte */
        sign = (*p >> 7) & 1;
        e = (*p & 0x7F) << 4;

        p += incr;

        /* Second byte */
        e |= (*p >> 4) & 0xF;
        fhi = (*p & 0xF) << 24;
        p += incr;

        if (e == 2047) {
            PyErr_SetString(
                PyExc_ValueError,
                "can't unpack IEEE 754 special value "
                "on non-IEEE platform");
            return -1.0;
        }

        /* Third byte */
        fhi |= *p << 16;
        p += incr;

        /* Fourth byte */
        fhi |= *p  << 8;
        p += incr;

        /* Fifth byte */
        fhi |= *p;
        p += incr;

        /* Sixth byte */
        flo = *p << 16;
        p += incr;

        /* Seventh byte */
        flo |= *p << 8;
        p += incr;

        /* Eighth byte */
        flo |= *p;

        x = (double)fhi + (double)flo / 16777216.0; /* 2**24 */
        x /= 268435456.0; /* 2**28 */

        if (e == 0)
            e = -1022;
        else {
            x += 1.0;
            e -= 1023;
        }
        x = ldexp(x, e);

        if (sign)
            x = -x;

        return x;
    }
    else {
        double x;

        if ((double_format == ieee_little_endian_format && !le)
            || (double_format == ieee_big_endian_format && le)) {
            char buf[8];
            char *d = &buf[7];
            int i;

            for (i = 0; i < 8; i++) {
                *d-- = *p++;
            }
            memcpy(&x, buf, 8);
        }
        else {
            memcpy(&x, p, 8);
        }

        return x;
    }
}
#endif
