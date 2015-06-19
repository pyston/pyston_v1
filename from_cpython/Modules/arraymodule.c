/* Array object implementation */

/* An array is a uniform list -- all items have the same type.
   The item type is restricted to simple C types like int or float */

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "structmember.h"

#ifdef STDC_HEADERS
#include <stddef.h>
#else /* !STDC_HEADERS */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>          /* For size_t */
#endif /* HAVE_SYS_TYPES_H */
#endif /* !STDC_HEADERS */

struct arrayobject; /* Forward */

/* All possible arraydescr values are defined in the vector "descriptors"
 * below.  That's defined later because the appropriate get and set
 * functions aren't visible yet.
 */
struct arraydescr {
    int typecode;
    int itemsize;
    PyObject * (*getitem)(struct arrayobject *, Py_ssize_t);
    int (*setitem)(struct arrayobject *, Py_ssize_t, PyObject *);
};

typedef struct arrayobject {
    PyObject_VAR_HEAD
    char *ob_item;
    Py_ssize_t allocated;
    struct arraydescr *ob_descr;
    PyObject *weakreflist; /* List of weak references */
} arrayobject;

static PyTypeObject Arraytype;

#define array_Check(op) PyObject_TypeCheck(op, &Arraytype)
#define array_CheckExact(op) (Py_TYPE(op) == &Arraytype)

static int
array_resize(arrayobject *self, Py_ssize_t newsize)
{
    char *items;
    size_t _new_size;

    /* Bypass realloc() when a previous overallocation is large enough
       to accommodate the newsize.  If the newsize is 16 smaller than the
       current size, then proceed with the realloc() to shrink the list.
    */

    if (self->allocated >= newsize &&
        Py_SIZE(self) < newsize + 16 &&
        self->ob_item != NULL) {
        Py_SIZE(self) = newsize;
        return 0;
    }

    /* This over-allocates proportional to the array size, making room
     * for additional growth.  The over-allocation is mild, but is
     * enough to give linear-time amortized behavior over a long
     * sequence of appends() in the presence of a poorly-performing
     * system realloc().
     * The growth pattern is:  0, 4, 8, 16, 25, 34, 46, 56, 67, 79, ...
     * Note, the pattern starts out the same as for lists but then
     * grows at a smaller rate so that larger arrays only overallocate
     * by about 1/16th -- this is done because arrays are presumed to be more
     * memory critical.
     */

    _new_size = (newsize >> 4) + (Py_SIZE(self) < 8 ? 3 : 7) + newsize;
    items = self->ob_item;
    /* XXX The following multiplication and division does not optimize away
       like it does for lists since the size is not known at compile time */
    if (_new_size <= ((~(size_t)0) / self->ob_descr->itemsize))
        PyMem_RESIZE(items, char, (_new_size * self->ob_descr->itemsize));
    else
        items = NULL;
    if (items == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    self->ob_item = items;
    Py_SIZE(self) = newsize;
    self->allocated = _new_size;
    return 0;
}

/****************************************************************************
Get and Set functions for each type.
A Get function takes an arrayobject* and an integer index, returning the
array value at that index wrapped in an appropriate PyObject*.
A Set function takes an arrayobject, integer index, and PyObject*; sets
the array value at that index to the raw C data extracted from the PyObject*,
and returns 0 if successful, else nonzero on failure (PyObject* not of an
appropriate type or value).
Note that the basic Get and Set functions do NOT check that the index is
in bounds; that's the responsibility of the caller.
****************************************************************************/

static PyObject *
c_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyString_FromStringAndSize(&((char *)ap->ob_item)[i], 1);
}

static int
c_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    char x;
    if (!PyArg_Parse(v, "c;array item must be char", &x))
        return -1;
    if (i >= 0)
        ((char *)ap->ob_item)[i] = x;
    return 0;
}

static PyObject *
b_getitem(arrayobject *ap, Py_ssize_t i)
{
    long x = ((char *)ap->ob_item)[i];
    if (x >= 128)
        x -= 256;
    return PyInt_FromLong(x);
}

static int
b_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    short x;
    /* PyArg_Parse's 'b' formatter is for an unsigned char, therefore
       must use the next size up that is signed ('h') and manually do
       the overflow checking */
    if (!PyArg_Parse(v, "h;array item must be integer", &x))
        return -1;
    else if (x < -128) {
        PyErr_SetString(PyExc_OverflowError,
            "signed char is less than minimum");
        return -1;
    }
    else if (x > 127) {
        PyErr_SetString(PyExc_OverflowError,
            "signed char is greater than maximum");
        return -1;
    }
    if (i >= 0)
        ((char *)ap->ob_item)[i] = (char)x;
    return 0;
}

static PyObject *
BB_getitem(arrayobject *ap, Py_ssize_t i)
{
    long x = ((unsigned char *)ap->ob_item)[i];
    return PyInt_FromLong(x);
}

static int
BB_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    unsigned char x;
    /* 'B' == unsigned char, maps to PyArg_Parse's 'b' formatter */
    if (!PyArg_Parse(v, "b;array item must be integer", &x))
        return -1;
    if (i >= 0)
        ((char *)ap->ob_item)[i] = x;
    return 0;
}

#ifdef Py_USING_UNICODE
static PyObject *
u_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyUnicode_FromUnicode(&((Py_UNICODE *) ap->ob_item)[i], 1);
}

static int
u_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    Py_UNICODE *p;
    Py_ssize_t len;

    if (!PyArg_Parse(v, "u#;array item must be unicode character", &p, &len))
        return -1;
    if (len != 1) {
        PyErr_SetString(PyExc_TypeError,
                        "array item must be unicode character");
        return -1;
    }
    if (i >= 0)
        ((Py_UNICODE *)ap->ob_item)[i] = p[0];
    return 0;
}
#endif

static PyObject *
h_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((short *)ap->ob_item)[i]);
}

static int
h_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    short x;
    /* 'h' == signed short, maps to PyArg_Parse's 'h' formatter */
    if (!PyArg_Parse(v, "h;array item must be integer", &x))
        return -1;
    if (i >= 0)
                 ((short *)ap->ob_item)[i] = x;
    return 0;
}

static PyObject *
HH_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((unsigned short *)ap->ob_item)[i]);
}

static int
HH_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    int x;
    /* PyArg_Parse's 'h' formatter is for a signed short, therefore
       must use the next size up and manually do the overflow checking */
    if (!PyArg_Parse(v, "i;array item must be integer", &x))
        return -1;
    else if (x < 0) {
        PyErr_SetString(PyExc_OverflowError,
            "unsigned short is less than minimum");
        return -1;
    }
    else if (x > USHRT_MAX) {
        PyErr_SetString(PyExc_OverflowError,
            "unsigned short is greater than maximum");
        return -1;
    }
    if (i >= 0)
        ((short *)ap->ob_item)[i] = (short)x;
    return 0;
}

static PyObject *
i_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyInt_FromLong((long) ((int *)ap->ob_item)[i]);
}

static int
i_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    int x;
    /* 'i' == signed int, maps to PyArg_Parse's 'i' formatter */
    if (!PyArg_Parse(v, "i;array item must be integer", &x))
        return -1;
    if (i >= 0)
                 ((int *)ap->ob_item)[i] = x;
    return 0;
}

static PyObject *
II_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyLong_FromUnsignedLong(
        (unsigned long) ((unsigned int *)ap->ob_item)[i]);
}

static int
II_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    unsigned long x;
    if (PyLong_Check(v)) {
        x = PyLong_AsUnsignedLong(v);
        if (x == (unsigned long) -1 && PyErr_Occurred())
            return -1;
    }
    else {
        long y;
        if (!PyArg_Parse(v, "l;array item must be integer", &y))
            return -1;
        if (y < 0) {
            PyErr_SetString(PyExc_OverflowError,
                "unsigned int is less than minimum");
            return -1;
        }
        x = (unsigned long)y;

    }
    if (x > UINT_MAX) {
        PyErr_SetString(PyExc_OverflowError,
            "unsigned int is greater than maximum");
        return -1;
    }

    if (i >= 0)
        ((unsigned int *)ap->ob_item)[i] = (unsigned int)x;
    return 0;
}

static PyObject *
l_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyInt_FromLong(((long *)ap->ob_item)[i]);
}

static int
l_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    long x;
    if (!PyArg_Parse(v, "l;array item must be integer", &x))
        return -1;
    if (i >= 0)
                 ((long *)ap->ob_item)[i] = x;
    return 0;
}

static PyObject *
LL_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyLong_FromUnsignedLong(((unsigned long *)ap->ob_item)[i]);
}

static int
LL_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    unsigned long x;
    if (PyLong_Check(v)) {
        x = PyLong_AsUnsignedLong(v);
        if (x == (unsigned long) -1 && PyErr_Occurred())
            return -1;
    }
    else {
        long y;
        if (!PyArg_Parse(v, "l;array item must be integer", &y))
            return -1;
        if (y < 0) {
            PyErr_SetString(PyExc_OverflowError,
                "unsigned long is less than minimum");
            return -1;
        }
        x = (unsigned long)y;

    }
    if (x > ULONG_MAX) {
        PyErr_SetString(PyExc_OverflowError,
            "unsigned long is greater than maximum");
        return -1;
    }

    if (i >= 0)
        ((unsigned long *)ap->ob_item)[i] = x;
    return 0;
}

static PyObject *
f_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyFloat_FromDouble((double) ((float *)ap->ob_item)[i]);
}

static int
f_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    float x;
    if (!PyArg_Parse(v, "f;array item must be float", &x))
        return -1;
    if (i >= 0)
                 ((float *)ap->ob_item)[i] = x;
    return 0;
}

static PyObject *
d_getitem(arrayobject *ap, Py_ssize_t i)
{
    return PyFloat_FromDouble(((double *)ap->ob_item)[i]);
}

static int
d_setitem(arrayobject *ap, Py_ssize_t i, PyObject *v)
{
    double x;
    if (!PyArg_Parse(v, "d;array item must be float", &x))
        return -1;
    if (i >= 0)
                 ((double *)ap->ob_item)[i] = x;
    return 0;
}

/* Description of types */
static struct arraydescr descriptors[] = {
    {'c', sizeof(char), c_getitem, c_setitem},
    {'b', sizeof(char), b_getitem, b_setitem},
    {'B', sizeof(char), BB_getitem, BB_setitem},
#ifdef Py_USING_UNICODE
    {'u', sizeof(Py_UNICODE), u_getitem, u_setitem},
#endif
    {'h', sizeof(short), h_getitem, h_setitem},
    {'H', sizeof(short), HH_getitem, HH_setitem},
    {'i', sizeof(int), i_getitem, i_setitem},
    {'I', sizeof(int), II_getitem, II_setitem},
    {'l', sizeof(long), l_getitem, l_setitem},
    {'L', sizeof(long), LL_getitem, LL_setitem},
    {'f', sizeof(float), f_getitem, f_setitem},
    {'d', sizeof(double), d_getitem, d_setitem},
    {'\0', 0, 0, 0} /* Sentinel */
};

/****************************************************************************
Implementations of array object methods.
****************************************************************************/

static PyObject *
newarrayobject(PyTypeObject *type, Py_ssize_t size, struct arraydescr *descr)
{
    arrayobject *op;
    size_t nbytes;

    if (size < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }

    nbytes = size * descr->itemsize;
    /* Check for overflow */
    if (nbytes / descr->itemsize != (size_t)size) {
        return PyErr_NoMemory();
    }
    op = (arrayobject *) type->tp_alloc(type, 0);
    if (op == NULL) {
        return NULL;
    }
    op->ob_descr = descr;
    op->allocated = size;
    op->weakreflist = NULL;
    Py_SIZE(op) = size;
    if (size <= 0) {
        op->ob_item = NULL;
    }
    else {
        op->ob_item = PyMem_NEW(char, nbytes);
        if (op->ob_item == NULL) {
            Py_DECREF(op);
            return PyErr_NoMemory();
        }
    }
    return (PyObject *) op;
}

static PyObject *
getarrayitem(PyObject *op, Py_ssize_t i)
{
    register arrayobject *ap;
    assert(array_Check(op));
    ap = (arrayobject *)op;
    assert(i>=0 && i<Py_SIZE(ap));
    return (*ap->ob_descr->getitem)(ap, i);
}

static int
ins1(arrayobject *self, Py_ssize_t where, PyObject *v)
{
    char *items;
    Py_ssize_t n = Py_SIZE(self);
    if (v == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if ((*self->ob_descr->setitem)(self, -1, v) < 0)
        return -1;

    if (array_resize(self, n+1) == -1)
        return -1;
    items = self->ob_item;
    if (where < 0) {
        where += n;
        if (where < 0)
            where = 0;
    }
    if (where > n)
        where = n;
    /* appends don't need to call memmove() */
    if (where != n)
        memmove(items + (where+1)*self->ob_descr->itemsize,
            items + where*self->ob_descr->itemsize,
            (n-where)*self->ob_descr->itemsize);
    return (*self->ob_descr->setitem)(self, where, v);
}

/* Methods */

static void
array_dealloc(arrayobject *op)
{
    if (op->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) op);
    if (op->ob_item != NULL)
        PyMem_DEL(op->ob_item);
    Py_TYPE(op)->tp_free((PyObject *)op);
}

static PyObject *
array_richcompare(PyObject *v, PyObject *w, int op)
{
    arrayobject *va, *wa;
    PyObject *vi = NULL;
    PyObject *wi = NULL;
    Py_ssize_t i, k;
    PyObject *res;

    if (!array_Check(v) || !array_Check(w)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    va = (arrayobject *)v;
    wa = (arrayobject *)w;

    if (Py_SIZE(va) != Py_SIZE(wa) && (op == Py_EQ || op == Py_NE)) {
        /* Shortcut: if the lengths differ, the arrays differ */
        if (op == Py_EQ)
            res = Py_False;
        else
            res = Py_True;
        Py_INCREF(res);
        return res;
    }

    /* Search for the first index where items are different */
    k = 1;
    for (i = 0; i < Py_SIZE(va) && i < Py_SIZE(wa); i++) {
        vi = getarrayitem(v, i);
        wi = getarrayitem(w, i);
        if (vi == NULL || wi == NULL) {
            Py_XDECREF(vi);
            Py_XDECREF(wi);
            return NULL;
        }
        k = PyObject_RichCompareBool(vi, wi, Py_EQ);
        if (k == 0)
            break; /* Keeping vi and wi alive! */
        Py_DECREF(vi);
        Py_DECREF(wi);
        if (k < 0)
            return NULL;
    }

    if (k) {
        /* No more items to compare -- compare sizes */
        Py_ssize_t vs = Py_SIZE(va);
        Py_ssize_t ws = Py_SIZE(wa);
        int cmp;
        switch (op) {
        case Py_LT: cmp = vs <  ws; break;
        case Py_LE: cmp = vs <= ws; break;
        case Py_EQ: cmp = vs == ws; break;
        case Py_NE: cmp = vs != ws; break;
        case Py_GT: cmp = vs >  ws; break;
        case Py_GE: cmp = vs >= ws; break;
        default: return NULL; /* cannot happen */
        }
        if (cmp)
            res = Py_True;
        else
            res = Py_False;
        Py_INCREF(res);
        return res;
    }

    /* We have an item that differs.  First, shortcuts for EQ/NE */
    if (op == Py_EQ) {
        Py_INCREF(Py_False);
        res = Py_False;
    }
    else if (op == Py_NE) {
        Py_INCREF(Py_True);
        res = Py_True;
    }
    else {
        /* Compare the final item again using the proper operator */
        res = PyObject_RichCompare(vi, wi, op);
    }
    Py_DECREF(vi);
    Py_DECREF(wi);
    return res;
}

static Py_ssize_t
array_length(arrayobject *a)
{
    return Py_SIZE(a);
}

static PyObject *
array_item(arrayobject *a, Py_ssize_t i)
{
    if (i < 0 || i >= Py_SIZE(a)) {
        PyErr_SetString(PyExc_IndexError, "array index out of range");
        return NULL;
    }
    return getarrayitem((PyObject *)a, i);
}

static PyObject *
array_slice(arrayobject *a, Py_ssize_t ilow, Py_ssize_t ihigh)
{
    arrayobject *np;
    if (ilow < 0)
        ilow = 0;
    else if (ilow > Py_SIZE(a))
        ilow = Py_SIZE(a);
    if (ihigh < 0)
        ihigh = 0;
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > Py_SIZE(a))
        ihigh = Py_SIZE(a);
    np = (arrayobject *) newarrayobject(&Arraytype, ihigh - ilow, a->ob_descr);
    if (np == NULL)
        return NULL;
    memcpy(np->ob_item, a->ob_item + ilow * a->ob_descr->itemsize,
           (ihigh-ilow) * a->ob_descr->itemsize);
    return (PyObject *)np;
}

static PyObject *
array_copy(arrayobject *a, PyObject *unused)
{
    return array_slice(a, 0, Py_SIZE(a));
}

PyDoc_STRVAR(copy_doc,
"copy(array)\n\
\n\
 Return a copy of the array.");

static PyObject *
array_concat(arrayobject *a, PyObject *bb)
{
    Py_ssize_t size;
    arrayobject *np;
    if (!array_Check(bb)) {
        PyErr_Format(PyExc_TypeError,
             "can only append array (not \"%.200s\") to array",
                 Py_TYPE(bb)->tp_name);
        return NULL;
    }
#define b ((arrayobject *)bb)
    if (a->ob_descr != b->ob_descr) {
        PyErr_BadArgument();
        return NULL;
    }
    if (Py_SIZE(a) > PY_SSIZE_T_MAX - Py_SIZE(b)) {
        return PyErr_NoMemory();
    }
    size = Py_SIZE(a) + Py_SIZE(b);
    np = (arrayobject *) newarrayobject(&Arraytype, size, a->ob_descr);
    if (np == NULL) {
        return NULL;
    }
    memcpy(np->ob_item, a->ob_item, Py_SIZE(a)*a->ob_descr->itemsize);
    memcpy(np->ob_item + Py_SIZE(a)*a->ob_descr->itemsize,
           b->ob_item, Py_SIZE(b)*b->ob_descr->itemsize);
    return (PyObject *)np;
#undef b
}

static PyObject *
array_repeat(arrayobject *a, Py_ssize_t n)
{
    Py_ssize_t i;
    Py_ssize_t size;
    arrayobject *np;
    char *p;
    Py_ssize_t nbytes;
    if (n < 0)
        n = 0;
    if ((Py_SIZE(a) != 0) && (n > PY_SSIZE_T_MAX / Py_SIZE(a))) {
        return PyErr_NoMemory();
    }
    size = Py_SIZE(a) * n;
    np = (arrayobject *) newarrayobject(&Arraytype, size, a->ob_descr);
    if (np == NULL)
        return NULL;
    p = np->ob_item;
    nbytes = Py_SIZE(a) * a->ob_descr->itemsize;
    for (i = 0; i < n; i++) {
        memcpy(p, a->ob_item, nbytes);
        p += nbytes;
    }
    return (PyObject *) np;
}

static int
array_ass_slice(arrayobject *a, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject *v)
{
    char *item;
    Py_ssize_t n; /* Size of replacement array */
    Py_ssize_t d; /* Change in size */
#define b ((arrayobject *)v)
    if (v == NULL)
        n = 0;
    else if (array_Check(v)) {
        n = Py_SIZE(b);
        if (a == b) {
            /* Special case "a[i:j] = a" -- copy b first */
            int ret;
            v = array_slice(b, 0, n);
            if (!v)
                return -1;
            ret = array_ass_slice(a, ilow, ihigh, v);
            Py_DECREF(v);
            return ret;
        }
        if (b->ob_descr != a->ob_descr) {
            PyErr_BadArgument();
            return -1;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
         "can only assign array (not \"%.200s\") to array slice",
                         Py_TYPE(v)->tp_name);
        return -1;
    }
    if (ilow < 0)
        ilow = 0;
    else if (ilow > Py_SIZE(a))
        ilow = Py_SIZE(a);
    if (ihigh < 0)
        ihigh = 0;
    if (ihigh < ilow)
        ihigh = ilow;
    else if (ihigh > Py_SIZE(a))
        ihigh = Py_SIZE(a);
    item = a->ob_item;
    d = n - (ihigh-ilow);
    if (d < 0) { /* Delete -d items */
        memmove(item + (ihigh+d)*a->ob_descr->itemsize,
            item + ihigh*a->ob_descr->itemsize,
            (Py_SIZE(a)-ihigh)*a->ob_descr->itemsize);
        Py_SIZE(a) += d;
        PyMem_RESIZE(item, char, Py_SIZE(a)*a->ob_descr->itemsize);
                                        /* Can't fail */
        a->ob_item = item;
        a->allocated = Py_SIZE(a);
    }
    else if (d > 0) { /* Insert d items */
        PyMem_RESIZE(item, char,
                     (Py_SIZE(a) + d)*a->ob_descr->itemsize);
        if (item == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        memmove(item + (ihigh+d)*a->ob_descr->itemsize,
            item + ihigh*a->ob_descr->itemsize,
            (Py_SIZE(a)-ihigh)*a->ob_descr->itemsize);
        a->ob_item = item;
        Py_SIZE(a) += d;
        a->allocated = Py_SIZE(a);
    }
    if (n > 0)
        memcpy(item + ilow*a->ob_descr->itemsize, b->ob_item,
               n*b->ob_descr->itemsize);
    return 0;
#undef b
}

static int
array_ass_item(arrayobject *a, Py_ssize_t i, PyObject *v)
{
    if (i < 0 || i >= Py_SIZE(a)) {
        PyErr_SetString(PyExc_IndexError,
                         "array assignment index out of range");
        return -1;
    }
    if (v == NULL)
        return array_ass_slice(a, i, i+1, v);
    return (*a->ob_descr->setitem)(a, i, v);
}

static int
setarrayitem(PyObject *a, Py_ssize_t i, PyObject *v)
{
    assert(array_Check(a));
    return array_ass_item((arrayobject *)a, i, v);
}

static int
array_iter_extend(arrayobject *self, PyObject *bb)
{
    PyObject *it, *v;

    it = PyObject_GetIter(bb);
    if (it == NULL)
        return -1;

    while ((v = PyIter_Next(it)) != NULL) {
        if (ins1(self, Py_SIZE(self), v) != 0) {
            Py_DECREF(v);
            Py_DECREF(it);
            return -1;
        }
        Py_DECREF(v);
    }
    Py_DECREF(it);
    if (PyErr_Occurred())
        return -1;
    return 0;
}

static int
array_do_extend(arrayobject *self, PyObject *bb)
{
    Py_ssize_t size;
    char *old_item;

    if (!array_Check(bb))
        return array_iter_extend(self, bb);
#define b ((arrayobject *)bb)
    if (self->ob_descr != b->ob_descr) {
        PyErr_SetString(PyExc_TypeError,
                     "can only extend with array of same kind");
        return -1;
    }
    if ((Py_SIZE(self) > PY_SSIZE_T_MAX - Py_SIZE(b)) ||
        ((Py_SIZE(self) + Py_SIZE(b)) > PY_SSIZE_T_MAX / self->ob_descr->itemsize)) {
        PyErr_NoMemory();
        return -1;
    }
    size = Py_SIZE(self) + Py_SIZE(b);
    old_item = self->ob_item;
    PyMem_RESIZE(self->ob_item, char, size*self->ob_descr->itemsize);
    if (self->ob_item == NULL) {
        self->ob_item = old_item;
        PyErr_NoMemory();
        return -1;
    }
    memcpy(self->ob_item + Py_SIZE(self)*self->ob_descr->itemsize,
           b->ob_item, Py_SIZE(b)*b->ob_descr->itemsize);
    Py_SIZE(self) = size;
    self->allocated = size;

    return 0;
#undef b
}

static PyObject *
array_inplace_concat(arrayobject *self, PyObject *bb)
{
    if (!array_Check(bb)) {
        PyErr_Format(PyExc_TypeError,
            "can only extend array with array (not \"%.200s\")",
            Py_TYPE(bb)->tp_name);
        return NULL;
    }
    if (array_do_extend(self, bb) == -1)
        return NULL;
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
array_inplace_repeat(arrayobject *self, Py_ssize_t n)
{
    char *items, *p;
    Py_ssize_t size, i;

    if (Py_SIZE(self) > 0) {
        if (n < 0)
            n = 0;
        items = self->ob_item;
        if ((self->ob_descr->itemsize != 0) &&
            (Py_SIZE(self) > PY_SSIZE_T_MAX / self->ob_descr->itemsize)) {
            return PyErr_NoMemory();
        }
        size = Py_SIZE(self) * self->ob_descr->itemsize;
        if (n == 0) {
            PyMem_FREE(items);
            self->ob_item = NULL;
            Py_SIZE(self) = 0;
            self->allocated = 0;
        }
        else {
            if (size > PY_SSIZE_T_MAX / n) {
                return PyErr_NoMemory();
            }
            PyMem_RESIZE(items, char, n * size);
            if (items == NULL)
                return PyErr_NoMemory();
            p = items;
            for (i = 1; i < n; i++) {
                p += size;
                memcpy(p, items, size);
            }
            self->ob_item = items;
            Py_SIZE(self) *= n;
            self->allocated = Py_SIZE(self);
        }
    }
    Py_INCREF(self);
    return (PyObject *)self;
}


static PyObject *
ins(arrayobject *self, Py_ssize_t where, PyObject *v)
{
    if (ins1(self, where, v) != 0)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
array_count(arrayobject *self, PyObject *v)
{
    Py_ssize_t count = 0;
    Py_ssize_t i;

    for (i = 0; i < Py_SIZE(self); i++) {
        PyObject *selfi = getarrayitem((PyObject *)self, i);
        int cmp = PyObject_RichCompareBool(selfi, v, Py_EQ);
        Py_DECREF(selfi);
        if (cmp > 0)
            count++;
        else if (cmp < 0)
            return NULL;
    }
    return PyInt_FromSsize_t(count);
}

PyDoc_STRVAR(count_doc,
"count(x)\n\
\n\
Return number of occurrences of x in the array.");

static PyObject *
array_index(arrayobject *self, PyObject *v)
{
    Py_ssize_t i;

    for (i = 0; i < Py_SIZE(self); i++) {
        PyObject *selfi = getarrayitem((PyObject *)self, i);
        int cmp = PyObject_RichCompareBool(selfi, v, Py_EQ);
        Py_DECREF(selfi);
        if (cmp > 0) {
            return PyInt_FromLong((long)i);
        }
        else if (cmp < 0)
            return NULL;
    }
    PyErr_SetString(PyExc_ValueError, "array.index(x): x not in list");
    return NULL;
}

PyDoc_STRVAR(index_doc,
"index(x)\n\
\n\
Return index of first occurrence of x in the array.");

static int
array_contains(arrayobject *self, PyObject *v)
{
    Py_ssize_t i;
    int cmp;

    for (i = 0, cmp = 0 ; cmp == 0 && i < Py_SIZE(self); i++) {
        PyObject *selfi = getarrayitem((PyObject *)self, i);
        cmp = PyObject_RichCompareBool(selfi, v, Py_EQ);
        Py_DECREF(selfi);
    }
    return cmp;
}

static PyObject *
array_remove(arrayobject *self, PyObject *v)
{
    int i;

    for (i = 0; i < Py_SIZE(self); i++) {
        PyObject *selfi = getarrayitem((PyObject *)self,i);
        int cmp = PyObject_RichCompareBool(selfi, v, Py_EQ);
        Py_DECREF(selfi);
        if (cmp > 0) {
            if (array_ass_slice(self, i, i+1,
                               (PyObject *)NULL) != 0)
                return NULL;
            Py_INCREF(Py_None);
            return Py_None;
        }
        else if (cmp < 0)
            return NULL;
    }
    PyErr_SetString(PyExc_ValueError, "array.remove(x): x not in list");
    return NULL;
}

PyDoc_STRVAR(remove_doc,
"remove(x)\n\
\n\
Remove the first occurrence of x in the array.");

static PyObject *
array_pop(arrayobject *self, PyObject *args)
{
    Py_ssize_t i = -1;
    PyObject *v;
    if (!PyArg_ParseTuple(args, "|n:pop", &i))
        return NULL;
    if (Py_SIZE(self) == 0) {
        /* Special-case most common failure cause */
        PyErr_SetString(PyExc_IndexError, "pop from empty array");
        return NULL;
    }
    if (i < 0)
        i += Py_SIZE(self);
    if (i < 0 || i >= Py_SIZE(self)) {
        PyErr_SetString(PyExc_IndexError, "pop index out of range");
        return NULL;
    }
    v = getarrayitem((PyObject *)self,i);
    if (array_ass_slice(self, i, i+1, (PyObject *)NULL) != 0) {
        Py_DECREF(v);
        return NULL;
    }
    return v;
}

PyDoc_STRVAR(pop_doc,
"pop([i])\n\
\n\
Return the i-th element and delete it from the array. i defaults to -1.");

static PyObject *
array_extend(arrayobject *self, PyObject *bb)
{
    if (array_do_extend(self, bb) == -1)
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(extend_doc,
"extend(array or iterable)\n\
\n\
 Append items to the end of the array.");

static PyObject *
array_insert(arrayobject *self, PyObject *args)
{
    Py_ssize_t i;
    PyObject *v;
    if (!PyArg_ParseTuple(args, "nO:insert", &i, &v))
        return NULL;
    return ins(self, i, v);
}

PyDoc_STRVAR(insert_doc,
"insert(i,x)\n\
\n\
Insert a new item x into the array before position i.");


static PyObject *
array_buffer_info(arrayobject *self, PyObject *unused)
{
    PyObject* retval = NULL;
    retval = PyTuple_New(2);
    if (!retval)
        return NULL;

    PyTuple_SET_ITEM(retval, 0, PyLong_FromVoidPtr(self->ob_item));
    PyTuple_SET_ITEM(retval, 1, PyInt_FromLong((long)(Py_SIZE(self))));

    return retval;
}

PyDoc_STRVAR(buffer_info_doc,
"buffer_info() -> (address, length)\n\
\n\
Return a tuple (address, length) giving the current memory address and\n\
the length in items of the buffer used to hold array's contents\n\
The length should be multiplied by the itemsize attribute to calculate\n\
the buffer length in bytes.");


static PyObject *
array_append(arrayobject *self, PyObject *v)
{
    return ins(self, Py_SIZE(self), v);
}

PyDoc_STRVAR(append_doc,
"append(x)\n\
\n\
Append new value x to the end of the array.");


static PyObject *
array_byteswap(arrayobject *self, PyObject *unused)
{
    char *p;
    Py_ssize_t i;

    switch (self->ob_descr->itemsize) {
    case 1:
        break;
    case 2:
        for (p = self->ob_item, i = Py_SIZE(self); --i >= 0; p += 2) {
            char p0 = p[0];
            p[0] = p[1];
            p[1] = p0;
        }
        break;
    case 4:
        for (p = self->ob_item, i = Py_SIZE(self); --i >= 0; p += 4) {
            char p0 = p[0];
            char p1 = p[1];
            p[0] = p[3];
            p[1] = p[2];
            p[2] = p1;
            p[3] = p0;
        }
        break;
    case 8:
        for (p = self->ob_item, i = Py_SIZE(self); --i >= 0; p += 8) {
            char p0 = p[0];
            char p1 = p[1];
            char p2 = p[2];
            char p3 = p[3];
            p[0] = p[7];
            p[1] = p[6];
            p[2] = p[5];
            p[3] = p[4];
            p[4] = p3;
            p[5] = p2;
            p[6] = p1;
            p[7] = p0;
        }
        break;
    default:
        PyErr_SetString(PyExc_RuntimeError,
                   "don't know how to byteswap this array type");
        return NULL;
    }
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(byteswap_doc,
"byteswap()\n\
\n\
Byteswap all items of the array.  If the items in the array are not 1, 2,\n\
4, or 8 bytes in size, RuntimeError is raised.");

static PyObject *
array_reverse(arrayobject *self, PyObject *unused)
{
    register Py_ssize_t itemsize = self->ob_descr->itemsize;
    register char *p, *q;
    /* little buffer to hold items while swapping */
    char tmp[256];      /* 8 is probably enough -- but why skimp */
    assert((size_t)itemsize <= sizeof(tmp));

    if (Py_SIZE(self) > 1) {
        for (p = self->ob_item,
             q = self->ob_item + (Py_SIZE(self) - 1)*itemsize;
             p < q;
             p += itemsize, q -= itemsize) {
            /* memory areas guaranteed disjoint, so memcpy
             * is safe (& memmove may be slower).
             */
            memcpy(tmp, p, itemsize);
            memcpy(p, q, itemsize);
            memcpy(q, tmp, itemsize);
        }
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(reverse_doc,
"reverse()\n\
\n\
Reverse the order of the items in the array.");

static PyObject *
array_fromfile(arrayobject *self, PyObject *args)
{
    PyObject *f;
    Py_ssize_t n;
    FILE *fp;
    if (!PyArg_ParseTuple(args, "On:fromfile", &f, &n))
        return NULL;
    fp = PyFile_AsFile(f);
    if (fp == NULL) {
        PyErr_SetString(PyExc_TypeError, "arg1 must be open file");
        return NULL;
    }
    if (n > 0) {
        char *item = self->ob_item;
        Py_ssize_t itemsize = self->ob_descr->itemsize;
        size_t nread;
        Py_ssize_t newlength;
        size_t newbytes;
        /* Be careful here about overflow */
        if ((newlength = Py_SIZE(self) + n) <= 0 ||
            (newbytes = newlength * itemsize) / itemsize !=
            (size_t)newlength)
            goto nomem;
        PyMem_RESIZE(item, char, newbytes);
        if (item == NULL) {
          nomem:
            PyErr_NoMemory();
            return NULL;
        }
        self->ob_item = item;
        Py_SIZE(self) += n;
        self->allocated = Py_SIZE(self);
        nread = fread(item + (Py_SIZE(self) - n) * itemsize,
                      itemsize, n, fp);
        if (nread < (size_t)n) {
          Py_SIZE(self) -= (n - nread);
            PyMem_RESIZE(item, char, Py_SIZE(self)*itemsize);
            self->ob_item = item;
            self->allocated = Py_SIZE(self);
            if (ferror(fp)) {
                PyErr_SetFromErrno(PyExc_IOError);
                clearerr(fp);
            }
            else {
                PyErr_SetString(PyExc_EOFError,
                                "not enough items in file");
            }
            return NULL;
        }
    }
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(fromfile_doc,
"fromfile(f, n)\n\
\n\
Read n objects from the file object f and append them to the end of the\n\
array.  Also called as read.");


static PyObject *
array_fromfile_as_read(arrayobject *self, PyObject *args)
{
    if (PyErr_WarnPy3k("array.read() not supported in 3.x; "
                       "use array.fromfile()", 1) < 0)
        return NULL;
    return array_fromfile(self, args);
}


static PyObject *
array_tofile(arrayobject *self, PyObject *f)
{
    FILE *fp;

    fp = PyFile_AsFile(f);
    if (fp == NULL) {
        PyErr_SetString(PyExc_TypeError, "arg must be open file");
        return NULL;
    }
    if (self->ob_size > 0) {
        if (fwrite(self->ob_item, self->ob_descr->itemsize,
                   self->ob_size, fp) != (size_t)self->ob_size) {
            PyErr_SetFromErrno(PyExc_IOError);
            clearerr(fp);
            return NULL;
        }
    }
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(tofile_doc,
"tofile(f)\n\
\n\
Write all items (as machine values) to the file object f.  Also called as\n\
write.");


static PyObject *
array_tofile_as_write(arrayobject *self, PyObject *f)
{
    if (PyErr_WarnPy3k("array.write() not supported in 3.x; "
                       "use array.tofile()", 1) < 0)
        return NULL;
    return array_tofile(self, f);
}


static PyObject *
array_fromlist(arrayobject *self, PyObject *list)
{
    Py_ssize_t n;
    Py_ssize_t itemsize = self->ob_descr->itemsize;

    if (!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "arg must be list");
        return NULL;
    }
    n = PyList_Size(list);
    if (n > 0) {
        char *item = self->ob_item;
        Py_ssize_t i;
        PyMem_RESIZE(item, char, (Py_SIZE(self) + n) * itemsize);
        if (item == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        self->ob_item = item;
        Py_SIZE(self) += n;
        self->allocated = Py_SIZE(self);
        for (i = 0; i < n; i++) {
            PyObject *v = PyList_GetItem(list, i);
            if ((*self->ob_descr->setitem)(self,
                            Py_SIZE(self) - n + i, v) != 0) {
                Py_SIZE(self) -= n;
                if (itemsize && (self->ob_size > PY_SSIZE_T_MAX / itemsize)) {
                    return PyErr_NoMemory();
                }
                PyMem_RESIZE(item, char,
                                  Py_SIZE(self) * itemsize);
                self->ob_item = item;
                self->allocated = Py_SIZE(self);
                return NULL;
            }
        }
    }
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(fromlist_doc,
"fromlist(list)\n\
\n\
Append items to array from list.");


static PyObject *
array_tolist(arrayobject *self, PyObject *unused)
{
    PyObject *list = PyList_New(Py_SIZE(self));
    Py_ssize_t i;

    if (list == NULL)
        return NULL;
    for (i = 0; i < Py_SIZE(self); i++) {
        PyObject *v = getarrayitem((PyObject *)self, i);
        if (v == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SetItem(list, i, v);
    }
    return list;
}

PyDoc_STRVAR(tolist_doc,
"tolist() -> list\n\
\n\
Convert array to an ordinary list with the same items.");


static PyObject *
array_fromstring(arrayobject *self, PyObject *args)
{
    char *str;
    Py_ssize_t n;
    int itemsize = self->ob_descr->itemsize;
    if (!PyArg_ParseTuple(args, "s#:fromstring", &str, &n))
        return NULL;
    if (n % itemsize != 0) {
        PyErr_SetString(PyExc_ValueError,
                   "string length not a multiple of item size");
        return NULL;
    }
    n = n / itemsize;
    if (n > 0) {
        char *item = self->ob_item;
        if ((n > PY_SSIZE_T_MAX - Py_SIZE(self)) ||
            ((Py_SIZE(self) + n) > PY_SSIZE_T_MAX / itemsize)) {
                return PyErr_NoMemory();
        }
        PyMem_RESIZE(item, char, (Py_SIZE(self) + n) * itemsize);
        if (item == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        self->ob_item = item;
        Py_SIZE(self) += n;
        self->allocated = Py_SIZE(self);
        memcpy(item + (Py_SIZE(self) - n) * itemsize,
               str, itemsize*n);
    }
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(fromstring_doc,
"fromstring(string)\n\
\n\
Appends items from the string, interpreting it as an array of machine\n\
values,as if it had been read from a file using the fromfile() method).");


static PyObject *
array_tostring(arrayobject *self, PyObject *unused)
{
    if (self->ob_size <= PY_SSIZE_T_MAX / self->ob_descr->itemsize) {
        return PyString_FromStringAndSize(self->ob_item,
                            Py_SIZE(self) * self->ob_descr->itemsize);
    } else {
        return PyErr_NoMemory();
    }
}

PyDoc_STRVAR(tostring_doc,
"tostring() -> string\n\
\n\
Convert the array to an array of machine values and return the string\n\
representation.");



#ifdef Py_USING_UNICODE
static PyObject *
array_fromunicode(arrayobject *self, PyObject *args)
{
    Py_UNICODE *ustr;
    Py_ssize_t n;

    if (!PyArg_ParseTuple(args, "u#:fromunicode", &ustr, &n))
        return NULL;
    if (self->ob_descr->typecode != 'u') {
        PyErr_SetString(PyExc_ValueError,
            "fromunicode() may only be called on "
            "type 'u' arrays");
        return NULL;
    }
    if (n > 0) {
        Py_UNICODE *item = (Py_UNICODE *) self->ob_item;
        if (Py_SIZE(self) > PY_SSIZE_T_MAX - n) {
            return PyErr_NoMemory();
        }
        PyMem_RESIZE(item, Py_UNICODE, Py_SIZE(self) + n);
        if (item == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        self->ob_item = (char *) item;
        Py_SIZE(self) += n;
        self->allocated = Py_SIZE(self);
        memcpy(item + Py_SIZE(self) - n,
               ustr, n * sizeof(Py_UNICODE));
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(fromunicode_doc,
"fromunicode(ustr)\n\
\n\
Extends this array with data from the unicode string ustr.\n\
The array must be a type 'u' array; otherwise a ValueError\n\
is raised.  Use array.fromstring(ustr.decode(...)) to\n\
append Unicode data to an array of some other type.");


static PyObject *
array_tounicode(arrayobject *self, PyObject *unused)
{
    if (self->ob_descr->typecode != 'u') {
        PyErr_SetString(PyExc_ValueError,
            "tounicode() may only be called on type 'u' arrays");
        return NULL;
    }
    return PyUnicode_FromUnicode((Py_UNICODE *) self->ob_item, Py_SIZE(self));
}

PyDoc_STRVAR(tounicode_doc,
"tounicode() -> unicode\n\
\n\
Convert the array to a unicode string.  The array must be\n\
a type 'u' array; otherwise a ValueError is raised.  Use\n\
array.tostring().decode() to obtain a unicode string from\n\
an array of some other type.");

#endif /* Py_USING_UNICODE */

static PyObject *
array_reduce(arrayobject *array)
{
    PyObject *dict, *result, *list;

    dict = PyObject_GetAttrString((PyObject *)array, "__dict__");
    if (dict == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return NULL;
        PyErr_Clear();
        dict = Py_None;
        Py_INCREF(dict);
    }
    /* Unlike in Python 3.x, we never use the more efficient memory
     * representation of an array for pickling.  This is unfortunately
     * necessary to allow array objects to be unpickled by Python 3.x,
     * since str objects from 2.x are always decoded to unicode in
     * Python 3.x.
     */
    list = array_tolist(array, NULL);
    if (list == NULL) {
        Py_DECREF(dict);
        return NULL;
    }
    result = Py_BuildValue(
        "O(cO)O", Py_TYPE(array), array->ob_descr->typecode, list, dict);
    Py_DECREF(list);
    Py_DECREF(dict);
    return result;
}

PyDoc_STRVAR(reduce_doc, "Return state information for pickling.");

static PyObject *
array_sizeof(arrayobject *self, PyObject *unused)
{
    Py_ssize_t res;
    res = sizeof(arrayobject) + self->allocated * self->ob_descr->itemsize;
    return PyLong_FromSsize_t(res);
}

PyDoc_STRVAR(sizeof_doc,
"__sizeof__() -> int\n\
\n\
Size of the array in memory, in bytes.");

static PyObject *
array_get_typecode(arrayobject *a, void *closure)
{
    char tc = a->ob_descr->typecode;
    return PyString_FromStringAndSize(&tc, 1);
}

static PyObject *
array_get_itemsize(arrayobject *a, void *closure)
{
    return PyInt_FromLong((long)a->ob_descr->itemsize);
}

static PyGetSetDef array_getsets [] = {
    {"typecode", (getter) array_get_typecode, NULL,
     "the typecode character used to create the array"},
    {"itemsize", (getter) array_get_itemsize, NULL,
     "the size, in bytes, of one array item"},
    {NULL}
};

static PyMethodDef array_methods[] = {
    {"append",          (PyCFunction)array_append,      METH_O,
     append_doc},
    {"buffer_info", (PyCFunction)array_buffer_info, METH_NOARGS,
     buffer_info_doc},
    {"byteswap",        (PyCFunction)array_byteswap,    METH_NOARGS,
     byteswap_doc},
    {"__copy__",        (PyCFunction)array_copy,        METH_NOARGS,
     copy_doc},
    {"count",           (PyCFunction)array_count,       METH_O,
     count_doc},
    {"__deepcopy__",(PyCFunction)array_copy,            METH_O,
     copy_doc},
    {"extend",      (PyCFunction)array_extend,          METH_O,
     extend_doc},
    {"fromfile",        (PyCFunction)array_fromfile,    METH_VARARGS,
     fromfile_doc},
    {"fromlist",        (PyCFunction)array_fromlist,    METH_O,
     fromlist_doc},
    {"fromstring",      (PyCFunction)array_fromstring,  METH_VARARGS,
     fromstring_doc},
#ifdef Py_USING_UNICODE
    {"fromunicode",     (PyCFunction)array_fromunicode, METH_VARARGS,
     fromunicode_doc},
#endif
    {"index",           (PyCFunction)array_index,       METH_O,
     index_doc},
    {"insert",          (PyCFunction)array_insert,      METH_VARARGS,
     insert_doc},
    {"pop",             (PyCFunction)array_pop,         METH_VARARGS,
     pop_doc},
    {"read",            (PyCFunction)array_fromfile_as_read,    METH_VARARGS,
     fromfile_doc},
    {"__reduce__",      (PyCFunction)array_reduce,      METH_NOARGS,
     reduce_doc},
    {"remove",          (PyCFunction)array_remove,      METH_O,
     remove_doc},
    {"reverse",         (PyCFunction)array_reverse,     METH_NOARGS,
     reverse_doc},
/*      {"sort",        (PyCFunction)array_sort,        METH_VARARGS,
    sort_doc},*/
    {"tofile",          (PyCFunction)array_tofile,      METH_O,
     tofile_doc},
    {"tolist",          (PyCFunction)array_tolist,      METH_NOARGS,
     tolist_doc},
    {"tostring",        (PyCFunction)array_tostring,    METH_NOARGS,
     tostring_doc},
#ifdef Py_USING_UNICODE
    {"tounicode",   (PyCFunction)array_tounicode,       METH_NOARGS,
     tounicode_doc},
#endif
    {"write",           (PyCFunction)array_tofile_as_write,     METH_O,
     tofile_doc},
    {"__sizeof__",      (PyCFunction)array_sizeof,      METH_NOARGS,
     sizeof_doc},
    {NULL,              NULL}           /* sentinel */
};

static PyObject *
array_repr(arrayobject *a)
{
    char buf[256], typecode;
    PyObject *s, *t, *v = NULL;
    Py_ssize_t len;

    len = Py_SIZE(a);
    typecode = a->ob_descr->typecode;
    if (len == 0) {
        PyOS_snprintf(buf, sizeof(buf), "array('%c')", typecode);
        return PyString_FromString(buf);
    }

    if (typecode == 'c')
        v = array_tostring(a, NULL);
#ifdef Py_USING_UNICODE
    else if (typecode == 'u')
        v = array_tounicode(a, NULL);
#endif
    else
        v = array_tolist(a, NULL);
    t = PyObject_Repr(v);
    Py_XDECREF(v);

    PyOS_snprintf(buf, sizeof(buf), "array('%c', ", typecode);
    s = PyString_FromString(buf);
    PyString_ConcatAndDel(&s, t);
    PyString_ConcatAndDel(&s, PyString_FromString(")"));
    return s;
}

static PyObject*
array_subscr(arrayobject* self, PyObject* item)
{
    if (PyIndex_Check(item)) {
        Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (i==-1 && PyErr_Occurred()) {
            return NULL;
        }
        if (i < 0)
            i += Py_SIZE(self);
        return array_item(self, i);
    }
    else if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, slicelength, cur, i;
        PyObject* result;
        arrayobject* ar;
        int itemsize = self->ob_descr->itemsize;

        if (PySlice_GetIndicesEx((PySliceObject*)item, Py_SIZE(self),
                         &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }

        if (slicelength <= 0) {
            return newarrayobject(&Arraytype, 0, self->ob_descr);
        }
        else if (step == 1) {
            PyObject *result = newarrayobject(&Arraytype,
                                    slicelength, self->ob_descr);
            if (result == NULL)
                return NULL;
            memcpy(((arrayobject *)result)->ob_item,
                   self->ob_item + start * itemsize,
                   slicelength * itemsize);
            return result;
        }
        else {
            result = newarrayobject(&Arraytype, slicelength, self->ob_descr);
            if (!result) return NULL;

            ar = (arrayobject*)result;

            for (cur = start, i = 0; i < slicelength;
                 cur += step, i++) {
                memcpy(ar->ob_item + i*itemsize,
                       self->ob_item + cur*itemsize,
                       itemsize);
            }

            return result;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "array indices must be integers");
        return NULL;
    }
}

static int
array_ass_subscr(arrayobject* self, PyObject* item, PyObject* value)
{
    Py_ssize_t start, stop, step, slicelength, needed;
    arrayobject* other;
    int itemsize;

    if (PyIndex_Check(item)) {
        Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);

        if (i == -1 && PyErr_Occurred())
            return -1;
        if (i < 0)
            i += Py_SIZE(self);
        if (i < 0 || i >= Py_SIZE(self)) {
            PyErr_SetString(PyExc_IndexError,
                "array assignment index out of range");
            return -1;
        }
        if (value == NULL) {
            /* Fall through to slice assignment */
            start = i;
            stop = i + 1;
            step = 1;
            slicelength = 1;
        }
        else
            return (*self->ob_descr->setitem)(self, i, value);
    }
    else if (PySlice_Check(item)) {
        if (PySlice_GetIndicesEx((PySliceObject *)item,
                                 Py_SIZE(self), &start, &stop,
                                 &step, &slicelength) < 0) {
            return -1;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "array indices must be integer");
        return -1;
    }
    if (value == NULL) {
        other = NULL;
        needed = 0;
    }
    else if (array_Check(value)) {
        other = (arrayobject *)value;
        needed = Py_SIZE(other);
        if (self == other) {
            /* Special case "self[i:j] = self" -- copy self first */
            int ret;
            value = array_slice(other, 0, needed);
            if (value == NULL)
                return -1;
            ret = array_ass_subscr(self, item, value);
            Py_DECREF(value);
            return ret;
        }
        if (other->ob_descr != self->ob_descr) {
            PyErr_BadArgument();
            return -1;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
         "can only assign array (not \"%.200s\") to array slice",
                         Py_TYPE(value)->tp_name);
        return -1;
    }
    itemsize = self->ob_descr->itemsize;
    /* for 'a[2:1] = ...', the insertion point is 'start', not 'stop' */
    if ((step > 0 && stop < start) ||
        (step < 0 && stop > start))
        stop = start;
    if (step == 1) {
        if (slicelength > needed) {
            memmove(self->ob_item + (start + needed) * itemsize,
                self->ob_item + stop * itemsize,
                (Py_SIZE(self) - stop) * itemsize);
            if (array_resize(self, Py_SIZE(self) +
                             needed - slicelength) < 0)
                return -1;
        }
        else if (slicelength < needed) {
            if (array_resize(self, Py_SIZE(self) +
                             needed - slicelength) < 0)
                return -1;
            memmove(self->ob_item + (start + needed) * itemsize,
                self->ob_item + stop * itemsize,
                (Py_SIZE(self) - start - needed) * itemsize);
        }
        if (needed > 0)
            memcpy(self->ob_item + start * itemsize,
                   other->ob_item, needed * itemsize);
        return 0;
    }
    else if (needed == 0) {
        /* Delete slice */
        size_t cur;
        Py_ssize_t i;

        if (step < 0) {
            stop = start + 1;
            start = stop + step * (slicelength - 1) - 1;
            step = -step;
        }
        for (cur = start, i = 0; i < slicelength;
             cur += step, i++) {
            Py_ssize_t lim = step - 1;

            if (cur + step >= (size_t)Py_SIZE(self))
                lim = Py_SIZE(self) - cur - 1;
            memmove(self->ob_item + (cur - i) * itemsize,
                self->ob_item + (cur + 1) * itemsize,
                lim * itemsize);
        }
        cur = start + slicelength * step;
        if (cur < (size_t)Py_SIZE(self)) {
            memmove(self->ob_item + (cur-slicelength) * itemsize,
                self->ob_item + cur * itemsize,
                (Py_SIZE(self) - cur) * itemsize);
        }
        if (array_resize(self, Py_SIZE(self) - slicelength) < 0)
            return -1;
        return 0;
    }
    else {
        Py_ssize_t cur, i;

        if (needed != slicelength) {
            PyErr_Format(PyExc_ValueError,
                "attempt to assign array of size %zd "
                "to extended slice of size %zd",
                needed, slicelength);
            return -1;
        }
        for (cur = start, i = 0; i < slicelength;
             cur += step, i++) {
            memcpy(self->ob_item + cur * itemsize,
                   other->ob_item + i * itemsize,
                   itemsize);
        }
        return 0;
    }
}

static PyMappingMethods array_as_mapping = {
    (lenfunc)array_length,
    (binaryfunc)array_subscr,
    (objobjargproc)array_ass_subscr
};

static const void *emptybuf = "";

static Py_ssize_t
array_buffer_getreadbuf(arrayobject *self, Py_ssize_t index, const void **ptr)
{
    if ( index != 0 ) {
        PyErr_SetString(PyExc_SystemError,
                        "Accessing non-existent array segment");
        return -1;
    }
    *ptr = (void *)self->ob_item;
    if (*ptr == NULL)
        *ptr = emptybuf;
    return Py_SIZE(self)*self->ob_descr->itemsize;
}

static Py_ssize_t
array_buffer_getwritebuf(arrayobject *self, Py_ssize_t index, const void **ptr)
{
    if ( index != 0 ) {
        PyErr_SetString(PyExc_SystemError,
                        "Accessing non-existent array segment");
        return -1;
    }
    *ptr = (void *)self->ob_item;
    if (*ptr == NULL)
        *ptr = emptybuf;
    return Py_SIZE(self)*self->ob_descr->itemsize;
}

static Py_ssize_t
array_buffer_getsegcount(arrayobject *self, Py_ssize_t *lenp)
{
    if ( lenp )
        *lenp = Py_SIZE(self)*self->ob_descr->itemsize;
    return 1;
}

static PySequenceMethods array_as_sequence = {
    (lenfunc)array_length,                      /*sq_length*/
    (binaryfunc)array_concat,               /*sq_concat*/
    (ssizeargfunc)array_repeat,                 /*sq_repeat*/
    (ssizeargfunc)array_item,                           /*sq_item*/
    (ssizessizeargfunc)array_slice,             /*sq_slice*/
    (ssizeobjargproc)array_ass_item,                    /*sq_ass_item*/
    (ssizessizeobjargproc)array_ass_slice,      /*sq_ass_slice*/
    (objobjproc)array_contains,                 /*sq_contains*/
    (binaryfunc)array_inplace_concat,           /*sq_inplace_concat*/
    (ssizeargfunc)array_inplace_repeat          /*sq_inplace_repeat*/
};

static PyBufferProcs array_as_buffer = {
    (readbufferproc)array_buffer_getreadbuf,
    (writebufferproc)array_buffer_getwritebuf,
    (segcountproc)array_buffer_getsegcount,
    NULL,
};

static PyObject *
array_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    char c;
    PyObject *initial = NULL, *it = NULL;
    struct arraydescr *descr;

    if (type == &Arraytype && !_PyArg_NoKeywords("array.array()", kwds))
        return NULL;

    if (!PyArg_ParseTuple(args, "c|O:array", &c, &initial))
        return NULL;

    if (!(initial == NULL || PyList_Check(initial)
          || PyString_Check(initial) || PyTuple_Check(initial)
          || (c == 'u' && PyUnicode_Check(initial)))) {
        it = PyObject_GetIter(initial);
        if (it == NULL)
            return NULL;
        /* We set initial to NULL so that the subsequent code
           will create an empty array of the appropriate type
           and afterwards we can use array_iter_extend to populate
           the array.
        */
        initial = NULL;
    }
    for (descr = descriptors; descr->typecode != '\0'; descr++) {
        if (descr->typecode == c) {
            PyObject *a;
            Py_ssize_t len;

            if (initial == NULL || !(PyList_Check(initial)
                || PyTuple_Check(initial)))
                len = 0;
            else
                len = PySequence_Size(initial);

            a = newarrayobject(type, len, descr);
            if (a == NULL)
                return NULL;

            if (len > 0) {
                Py_ssize_t i;
                for (i = 0; i < len; i++) {
                    PyObject *v =
                        PySequence_GetItem(initial, i);
                    if (v == NULL) {
                        Py_DECREF(a);
                        return NULL;
                    }
                    if (setarrayitem(a, i, v) != 0) {
                        Py_DECREF(v);
                        Py_DECREF(a);
                        return NULL;
                    }
                    Py_DECREF(v);
                }
            } else if (initial != NULL && PyString_Check(initial)) {
                PyObject *t_initial, *v;
                t_initial = PyTuple_Pack(1, initial);
                if (t_initial == NULL) {
                    Py_DECREF(a);
                    return NULL;
                }
                v = array_fromstring((arrayobject *)a,
                                         t_initial);
                Py_DECREF(t_initial);
                if (v == NULL) {
                    Py_DECREF(a);
                    return NULL;
                }
                Py_DECREF(v);
#ifdef Py_USING_UNICODE
            } else if (initial != NULL && PyUnicode_Check(initial))  {
                Py_ssize_t n = PyUnicode_GET_DATA_SIZE(initial);
                if (n > 0) {
                    arrayobject *self = (arrayobject *)a;
                    char *item = self->ob_item;
                    item = (char *)PyMem_Realloc(item, n);
                    if (item == NULL) {
                        PyErr_NoMemory();
                        Py_DECREF(a);
                        return NULL;
                    }
                    self->ob_item = item;
                    Py_SIZE(self) = n / sizeof(Py_UNICODE);
                    memcpy(item, PyUnicode_AS_DATA(initial), n);
                    self->allocated = Py_SIZE(self);
                }
#endif
            }
            if (it != NULL) {
                if (array_iter_extend((arrayobject *)a, it) == -1) {
                    Py_DECREF(it);
                    Py_DECREF(a);
                    return NULL;
                }
                Py_DECREF(it);
            }
            return a;
        }
    }
    PyErr_SetString(PyExc_ValueError,
        "bad typecode (must be c, b, B, u, h, H, i, I, l, L, f or d)");
    return NULL;
}


PyDoc_STRVAR(module_doc,
"This module defines an object type which can efficiently represent\n\
an array of basic values: characters, integers, floating point\n\
numbers.  Arrays are sequence types and behave very much like lists,\n\
except that the type of objects stored in them is constrained.  The\n\
type is specified at object creation time by using a type code, which\n\
is a single character.  The following type codes are defined:\n\
\n\
    Type code   C Type             Minimum size in bytes \n\
    'c'         character          1 \n\
    'b'         signed integer     1 \n\
    'B'         unsigned integer   1 \n\
    'u'         Unicode character  2 \n\
    'h'         signed integer     2 \n\
    'H'         unsigned integer   2 \n\
    'i'         signed integer     2 \n\
    'I'         unsigned integer   2 \n\
    'l'         signed integer     4 \n\
    'L'         unsigned integer   4 \n\
    'f'         floating point     4 \n\
    'd'         floating point     8 \n\
\n\
The constructor is:\n\
\n\
array(typecode [, initializer]) -- create a new array\n\
");

PyDoc_STRVAR(arraytype_doc,
"array(typecode [, initializer]) -> array\n\
\n\
Return a new array whose items are restricted by typecode, and\n\
initialized from the optional initializer value, which must be a list,\n\
string or iterable over elements of the appropriate type.\n\
\n\
Arrays represent basic values and behave very much like lists, except\n\
the type of objects stored in them is constrained.\n\
\n\
Methods:\n\
\n\
append() -- append a new item to the end of the array\n\
buffer_info() -- return information giving the current memory info\n\
byteswap() -- byteswap all the items of the array\n\
count() -- return number of occurrences of an object\n\
extend() -- extend array by appending multiple elements from an iterable\n\
fromfile() -- read items from a file object\n\
fromlist() -- append items from the list\n\
fromstring() -- append items from the string\n\
index() -- return index of first occurrence of an object\n\
insert() -- insert a new item into the array at a provided position\n\
pop() -- remove and return item (default last)\n\
read() -- DEPRECATED, use fromfile()\n\
remove() -- remove first occurrence of an object\n\
reverse() -- reverse the order of the items in the array\n\
tofile() -- write all items to a file object\n\
tolist() -- return the array converted to an ordinary list\n\
tostring() -- return the array converted to a string\n\
write() -- DEPRECATED, use tofile()\n\
\n\
Attributes:\n\
\n\
typecode -- the typecode character used to create the array\n\
itemsize -- the length in bytes of one array item\n\
");

static PyObject *array_iter(arrayobject *ao);

static PyTypeObject Arraytype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "array.array",
    sizeof(arrayobject),
    0,
    (destructor)array_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    (reprfunc)array_repr,                       /* tp_repr */
    0,                                          /* tp_as_number*/
    &array_as_sequence,                         /* tp_as_sequence*/
    &array_as_mapping,                          /* tp_as_mapping*/
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    &array_as_buffer,                           /* tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_WEAKREFS,  /* tp_flags */
    arraytype_doc,                              /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    array_richcompare,                          /* tp_richcompare */
    offsetof(arrayobject, weakreflist),         /* tp_weaklistoffset */
    (getiterfunc)array_iter,                    /* tp_iter */
    0,                                          /* tp_iternext */
    array_methods,                              /* tp_methods */
    0,                                          /* tp_members */
    array_getsets,                              /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    array_new,                                  /* tp_new */
    PyObject_Del,                               /* tp_free */
};


/*********************** Array Iterator **************************/

typedef struct {
    PyObject_HEAD
    Py_ssize_t                          index;
    arrayobject                 *ao;
    PyObject                    * (*getitem)(struct arrayobject *, Py_ssize_t);
} arrayiterobject;

static PyTypeObject PyArrayIter_Type;

#define PyArrayIter_Check(op) PyObject_TypeCheck(op, &PyArrayIter_Type)

static PyObject *
array_iter(arrayobject *ao)
{
    arrayiterobject *it;

    if (!array_Check(ao)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    it = PyObject_GC_New(arrayiterobject, &PyArrayIter_Type);
    if (it == NULL)
        return NULL;

    Py_INCREF(ao);
    it->ao = ao;
    it->index = 0;
    it->getitem = ao->ob_descr->getitem;
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static PyObject *
arrayiter_next(arrayiterobject *it)
{
    assert(PyArrayIter_Check(it));
    if (it->index < Py_SIZE(it->ao))
        return (*it->getitem)(it->ao, it->index++);
    return NULL;
}

static void
arrayiter_dealloc(arrayiterobject *it)
{
    PyObject_GC_UnTrack(it);
    Py_XDECREF(it->ao);
    PyObject_GC_Del(it);
}

static int
arrayiter_traverse(arrayiterobject *it, visitproc visit, void *arg)
{
    Py_VISIT(it->ao);
    return 0;
}

static PyTypeObject PyArrayIter_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "arrayiterator",                        /* tp_name */
    sizeof(arrayiterobject),                /* tp_basicsize */
    0,                                      /* tp_itemsize */
    /* methods */
    (destructor)arrayiter_dealloc,              /* tp_dealloc */
    0,                                      /* tp_print */
    0,                                      /* tp_getattr */
    0,                                      /* tp_setattr */
    0,                                      /* tp_compare */
    0,                                      /* tp_repr */
    0,                                      /* tp_as_number */
    0,                                      /* tp_as_sequence */
    0,                                      /* tp_as_mapping */
    0,                                      /* tp_hash */
    0,                                      /* tp_call */
    0,                                      /* tp_str */
    PyObject_GenericGetAttr,                /* tp_getattro */
    0,                                      /* tp_setattro */
    0,                                      /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                      /* tp_doc */
    (traverseproc)arrayiter_traverse,           /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                      /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)arrayiter_next,               /* tp_iternext */
    0,                                          /* tp_methods */
};


/*********************** Install Module **************************/

/* No functions in array module. */
static PyMethodDef a_methods[] = {
    {NULL, NULL, 0, NULL}        /* Sentinel */
};


PyMODINIT_FUNC
initarray(void)
{
    PyObject *m;

    Arraytype.ob_type = &PyType_Type;
    PyArrayIter_Type.ob_type = &PyType_Type;

    // Pyston change: let the GC know about our type
    if (PyType_Ready(&Arraytype))
        return;
    if (PyType_Ready(&PyArrayIter_Type))
        return;

    m = Py_InitModule3("array", a_methods, module_doc);
    if (m == NULL)
        return;

    Py_INCREF((PyObject *)&Arraytype);
    PyModule_AddObject(m, "ArrayType", (PyObject *)&Arraytype);
    Py_INCREF((PyObject *)&Arraytype);
    PyModule_AddObject(m, "array", (PyObject *)&Arraytype);
    /* No need to check the error here, the caller will do that */
}
