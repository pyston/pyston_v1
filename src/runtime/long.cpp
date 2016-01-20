// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/long.h"

#include <cmath>
#include <float.h>
#include <gmp.h>
#include <mpfr.h>
#include <sstream>

#include "llvm/Support/raw_ostream.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedClass* long_cls;

/* Table of digit values for 8-bit string -> integer conversion.
 * '0' maps to 0, ..., '9' maps to 9.
 * 'a' and 'A' map to 10, ..., 'z' and 'Z' map to 35.
 * All other indices map to 37.
 * Note that when converting a base B string, a char c is a legitimate
 * base B digit iff _PyLong_DigitValue[Py_CHARMASK(c)] < B.
 */
extern "C" {
int _PyLong_DigitValue[256] = {
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    37, 37, 37, 37, 37, 37, 37, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 37, 37, 37, 37, 37, 37, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
    29, 30, 31, 32, 33, 34, 35, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
    37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37,
};
}

#define IS_LITTLE_ENDIAN (int)*(unsigned char*)&one
#define PY_ABS_LLONG_MIN (0 - (unsigned PY_LONG_LONG)PY_LLONG_MIN)

void BoxedLong::gchandler(GCVisitor* v, Box* b) {
    Box::gcHandler(v, b);

    BoxedLong* l = (BoxedLong*)b;

    v->visitPotentialRange((void**)&l->n, (void**)((&l->n) + 1));
}

extern "C" int _PyLong_Sign(PyObject* l) noexcept {
    return mpz_sgn(static_cast<BoxedLong*>(l)->n);
}

extern "C" PyObject* _PyLong_Copy(PyLongObject* src) noexcept {
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set(rtn->n, ((BoxedLong*)src)->n);
    return rtn;
}

extern "C" unsigned PY_LONG_LONG PyLong_AsUnsignedLongLong(PyObject* vv) noexcept {
    unsigned PY_LONG_LONG bytes;
    int one = 1;
    int res;

    if (vv == NULL || !PyLong_Check(vv)) {
        PyErr_BadInternalCall();
        return (unsigned PY_LONG_LONG) - 1;
    }

    res = _PyLong_AsByteArray((PyLongObject*)vv, (unsigned char*)&bytes, SIZEOF_LONG_LONG, IS_LITTLE_ENDIAN, 0);

    /* Plan 9 can't handle PY_LONG_LONG in ? : expressions */
    if (res < 0)
        return (unsigned PY_LONG_LONG)res;
    else
        return bytes;
}

extern "C" int _PyLong_AsInt(PyObject* obj) noexcept {
    int overflow;
    long result = PyLong_AsLongAndOverflow(obj, &overflow);
    if (overflow || result > INT_MAX || result < INT_MIN) {
        /* XXX: could be cute and give a different
           message for overflow == -1 */
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C int");
        return -1;
    }
    return (int)result;
}

extern "C" unsigned long PyLong_AsUnsignedLongMask(PyObject* vv) noexcept {
    if (PyLong_Check(vv)) {
        BoxedLong* l = static_cast<BoxedLong*>(vv);
        return mpz_get_ui(l->n);
    }

    Py_FatalError("unimplemented");
}

extern "C" unsigned PY_LONG_LONG PyLong_AsUnsignedLongLongMask(PyObject* vv) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PY_LONG_LONG PyLong_AsLongLong(PyObject* vv) noexcept {
    PY_LONG_LONG bytes;
    int one = 1;
    int res;

    if (vv == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (!PyLong_Check(vv)) {
        PyNumberMethods* nb;
        PyObject* io;
        if (PyInt_Check(vv))
            return (PY_LONG_LONG)PyInt_AsLong(vv);
        if ((nb = vv->cls->tp_as_number) == NULL || nb->nb_int == NULL) {
            PyErr_SetString(PyExc_TypeError, "an integer is required");
            return -1;
        }
        io = (*nb->nb_int)(vv);
        if (io == NULL)
            return -1;
        if (PyInt_Check(io)) {
            bytes = PyInt_AsLong(io);
            Py_DECREF(io);
            return bytes;
        }
        if (PyLong_Check(io)) {
            bytes = PyLong_AsLongLong(io);
            Py_DECREF(io);
            return bytes;
        }
        Py_DECREF(io);
        PyErr_SetString(PyExc_TypeError, "integer conversion failed");
        return -1;
    }

    res = _PyLong_AsByteArray((PyLongObject*)vv, (unsigned char*)&bytes, SIZEOF_LONG_LONG, IS_LITTLE_ENDIAN, 1);

    /* Plan 9 can't handle PY_LONG_LONG in ? : expressions */
    if (res < 0)
        return (PY_LONG_LONG)-1;
    else
        return bytes;
}

extern "C" PY_LONG_LONG PyLong_AsLongLongAndOverflow(PyObject* obj, int* overflow) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyLong_FromString(const char* str, char** pend, int base) noexcept {
    int sign = 1;
    if ((base != 0 && base < 2) || base > 36) {
        PyErr_SetString(PyExc_ValueError, "long() arg 2 must be >= 2 and <= 36");
        return NULL;
    }
    while (*str != '\0' && isspace(Py_CHARMASK(*str)))
        str++;
    if (*str == '+')
        ++str;
    else if (*str == '-') {
        ++str;
        sign = -1;
    }
    while (*str != '\0' && isspace(Py_CHARMASK(*str)))
        str++;
    if (base == 0) {
        /* No base given.  Deduce the base from the contents
           of the string */
        if (str[0] != '0')
            base = 10;
        else if (str[1] == 'x' || str[1] == 'X')
            base = 16;
        else if (str[1] == 'o' || str[1] == 'O')
            base = 8;
        else if (str[1] == 'b' || str[1] == 'B')
            base = 2;
        else
            /* "old" (C-style) octal literal, still valid in
               2.x, although illegal in 3.x */
            base = 8;
    }
    /* Whether or not we were deducing the base, skip leading chars
       as needed */
    if (str[0] == '0'
        && ((base == 16 && (str[1] == 'x' || str[1] == 'X')) || (base == 8 && (str[1] == 'o' || str[1] == 'O'))
            || (base == 2 && (str[1] == 'b' || str[1] == 'B'))))
        str += 2;

    BoxedLong* rtn = new BoxedLong();
    int r = 0;
    if ((str[strlen(str) - 1] == 'L' || str[strlen(str) - 1] == 'l') && base < 22) {
        std::string without_l(str, strlen(str) - 1);
        r = mpz_init_set_str(rtn->n, without_l.c_str(), base);
    } else {
        // if base great than 22, 'l' or 'L' should count as a digit.
        r = mpz_init_set_str(rtn->n, str, base);
    }

    if (pend)
        *pend = const_cast<char*>(str) + strlen(str);
    if (r != 0) {
        PyErr_Format(PyExc_ValueError, "invalid literal for long() with base %d: '%s'", base, str);
        return NULL;
    }

    if (sign == -1)
        mpz_neg(rtn->n, rtn->n);

    return rtn;
}

static int64_t asSignedLong(BoxedLong* self) {
    assert(self->cls == long_cls);
    if (!mpz_fits_slong_p(self->n))
        raiseExcHelper(OverflowError, "long int too large to convert to int");
    return mpz_get_si(self->n);
}

static uint64_t asUnsignedLong(BoxedLong* self) {
    assert(self->cls == long_cls);

    if (mpz_sgn(self->n) == -1)
        raiseExcHelper(OverflowError, "can't convert negative value to unsigned long");

    if (!mpz_fits_ulong_p(self->n))
        raiseExcHelper(OverflowError, "long int too large to convert");
    return mpz_get_ui(self->n);
}

extern "C" unsigned long PyLong_AsUnsignedLong(PyObject* vv) noexcept {
    assert(vv);

    if (vv->cls == int_cls) {
        long val = PyInt_AsLong(vv);
        if (val < 0) {
            PyErr_SetString(PyExc_OverflowError, "can't convert negative value "
                                                 "to unsigned long");
            return (unsigned long)-1;
        }
        return val;
    }

    RELEASE_ASSERT(PyLong_Check(vv), "");
    BoxedLong* l = static_cast<BoxedLong*>(vv);

    try {
        return asUnsignedLong(l);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" long PyLong_AsLong(PyObject* vv) noexcept {
    int overflow;
    long result = PyLong_AsLongAndOverflow(vv, &overflow);
    if (overflow) {
        /* XXX: could be cute and give a different
           message for overflow == -1 */
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C long");
    }
    return result;
}

extern "C" Py_ssize_t PyLong_AsSsize_t(PyObject* vv) noexcept {
    RELEASE_ASSERT(PyLong_Check(vv), "");

    if (PyLong_Check(vv)) {
        BoxedLong* l = static_cast<BoxedLong*>(vv);
        if (mpz_fits_slong_p(l->n)) {
            return mpz_get_si(l->n);
        } else {
            PyErr_SetString(PyExc_OverflowError, "long int too large to convert to int");
            return -1;
        }
    }
    Py_FatalError("unimplemented");
}

extern "C" long PyLong_AsLongAndOverflow(Box* vv, int* overflow) noexcept {
    // Ported from CPython; original comment:
    /* This version by Tim Peters */

    *overflow = 0;
    if (vv == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }

    if (PyInt_Check(vv))
        return PyInt_AsLong(vv);

    if (!PyLong_Check(vv)) {
        PyNumberMethods* nb;
        nb = vv->cls->tp_as_number;
        if (nb == NULL || nb->nb_int == NULL) {
            PyErr_SetString(PyExc_TypeError, "an integer is required");
            return -1;
        }

        vv = (*nb->nb_int)(vv);
        if (vv == NULL)
            return -1;

        if (PyInt_Check(vv))
            return PyInt_AsLong(vv);

        if (!PyLong_Check(vv)) {
            PyErr_SetString(PyExc_TypeError, "nb_int should return int object");
            return -1;
        }
        // fallthrough: this has to be a long
    }

    BoxedLong* l = static_cast<BoxedLong*>(vv);
    if (mpz_fits_slong_p(l->n)) {
        return mpz_get_si(l->n);
    } else {
        *overflow = mpz_sgn(l->n);
        return -1;
    }
}

extern "C" double PyLong_AsDouble(PyObject* vv) noexcept {
    RELEASE_ASSERT(PyLong_Check(vv), "");
    BoxedLong* l = static_cast<BoxedLong*>(vv);
    mpfr_t result;
    mpfr_init(result);
    mpfr_init_set_z(result, l->n, MPFR_RNDN);

    double result_f = mpfr_get_d(result, MPFR_RNDN);
    if (isinf(result_f)) {
        PyErr_SetString(PyExc_OverflowError, "long int too large to convert to float");
        return -1;
    }

    return result_f;
}

/* Convert the long to a string object with given base,
   appending a base prefix of 0[box] if base is 2, 8 or 16.
   Add a trailing "L" if addL is non-zero.
   If newstyle is zero, then use the pre-2.6 behavior of octal having
   a leading "0", instead of the prefix "0o" */
extern "C" PyAPI_FUNC(PyObject*) _PyLong_Format(PyObject* aa, int base, int addL, int newstyle) noexcept {
    BoxedLong* v = (BoxedLong*)aa;

    RELEASE_ASSERT(PyLong_Check(v), "");
    RELEASE_ASSERT(base >= 2 && base <= 62, "");

    bool is_negative = mpz_sgn(v->n) == -1;

    int space_required = mpz_sizeinbase(v->n, base) + 2;
    char* buf = (char*)malloc(space_required);
    mpz_get_str(buf, base, v->n);

    std::string str;
    llvm::raw_string_ostream os(str);
    if (is_negative)
        os << '-';

    if (base == 2)
        os << "0b";
    else if (base == 8) {
        if (!(mpz_sgn(v->n) == 0)) {
            os << (newstyle ? "0o" : "0");
        }
    } else if (base == 16)
        os << "0x";

    if (is_negative)
        os << buf + 1; // +1 to remove sign
    else
        os << buf;

    if (addL)
        os << "L";

    os.flush();
    auto rtn = boxString(str);
    free(buf);
    return rtn;
}

extern "C" PyObject* PyLong_FromDouble(double v) noexcept {
    if (isnan(v)) {
        PyErr_SetString(PyExc_ValueError, "cannot convert float NaN to integer");
        return NULL;
    }
    if (isinf(v)) {
        PyErr_SetString(PyExc_OverflowError, "cannot convert float infinity to integer");
        return NULL;
    }

    BoxedLong* rtn = new BoxedLong();
    mpz_init_set_d(rtn->n, v);
    return rtn;
}

extern "C" PyObject* PyLong_FromLong(long ival) noexcept {
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set_si(rtn->n, ival);
    return rtn;
}

#ifdef Py_USING_UNICODE
extern "C" PyObject* PyLong_FromUnicode(Py_UNICODE* u, Py_ssize_t length, int base) noexcept {
    PyObject* result;
    char* buffer = (char*)PyMem_MALLOC(length + 1);

    if (buffer == NULL)
        return PyErr_NoMemory();

    if (PyUnicode_EncodeDecimal(u, length, buffer, NULL)) {
        PyMem_FREE(buffer);
        return NULL;
    }
    result = PyLong_FromString(buffer, NULL, base);
    PyMem_FREE(buffer);
    return result;
}
#endif

extern "C" PyObject* PyLong_FromUnsignedLong(unsigned long ival) noexcept {
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set_ui(rtn->n, ival);
    return rtn;
}

extern "C" PyObject* PyLong_FromSsize_t(Py_ssize_t ival) noexcept {
    Py_ssize_t bytes = ival;
    int one = 1;
    return _PyLong_FromByteArray((unsigned char*)&bytes, SIZEOF_SIZE_T, IS_LITTLE_ENDIAN, 1);
}

extern "C" PyObject* PyLong_FromSize_t(size_t ival) noexcept {
    size_t bytes = ival;
    int one = 1;
    return _PyLong_FromByteArray((unsigned char*)&bytes, SIZEOF_SIZE_T, IS_LITTLE_ENDIAN, 0);
}

#undef IS_LITTLE_ENDIAN

extern "C" double _PyLong_Frexp(PyLongObject* a, Py_ssize_t* e) noexcept {
    BoxedLong* v = (BoxedLong*)a;
    double result = mpz_get_d_2exp(e, v->n);
    static_assert(sizeof(Py_ssize_t) == 8, "need to add overflow checking");
    return result;
}

/* Create a new long (or int) object from a C pointer */

extern "C" PyObject* PyLong_FromVoidPtr(void* p) noexcept {
#if SIZEOF_VOID_P <= SIZEOF_LONG
    if ((long)p < 0)
        return PyLong_FromUnsignedLong((unsigned long)p);
    return PyInt_FromLong((long)p);
#else

#ifndef HAVE_LONG_LONG
#error "PyLong_FromVoidPtr: sizeof(void*) > sizeof(long), but no long long"
#endif
#if SIZEOF_LONG_LONG < SIZEOF_VOID_P
#error "PyLong_FromVoidPtr: sizeof(PY_LONG_LONG) < sizeof(void*)"
#endif
    /* optimize null pointers */
    if (p == NULL)
        return PyInt_FromLong(0);
    return PyLong_FromUnsignedLongLong((unsigned PY_LONG_LONG)p);

#endif /* SIZEOF_VOID_P <= SIZEOF_LONG */
}

/* Get a C pointer from a long object (or an int object in some cases) */

extern "C" void* PyLong_AsVoidPtr(PyObject* vv) noexcept {
/* This function will allow int or long objects. If vv is neither,
   then the PyLong_AsLong*() functions will raise the exception:
   PyExc_SystemError, "bad argument to internal function"
*/
#if SIZEOF_VOID_P <= SIZEOF_LONG
    long x;

    if (PyInt_Check(vv))
        x = PyInt_AS_LONG(vv);
    else if (PyLong_Check(vv) && _PyLong_Sign(vv) < 0)
        x = PyLong_AsLong(vv);
    else
        x = PyLong_AsUnsignedLong(vv);
#else

#ifndef HAVE_LONG_LONG
#error "PyLong_AsVoidPtr: sizeof(void*) > sizeof(long), but no long long"
#endif
#if SIZEOF_LONG_LONG < SIZEOF_VOID_P
#error "PyLong_AsVoidPtr: sizeof(PY_LONG_LONG) < sizeof(void*)"
#endif
    PY_LONG_LONG x;

    if (PyInt_Check(vv))
        x = PyInt_AS_LONG(vv);
    else if (PyLong_Check(vv) && _PyLong_Sign(vv) < 0)
        x = PyLong_AsLongLong(vv);
    else
        x = PyLong_AsUnsignedLongLong(vv);

#endif /* SIZEOF_VOID_P <= SIZEOF_LONG */

    if (x == -1 && PyErr_Occurred())
        return NULL;
    return (void*)x;
}

extern "C" int _PyLong_AsByteArray(PyLongObject* v, unsigned char* bytes, size_t n, int little_endian,
                                   int is_signed) noexcept {
    const mpz_t* op = &((BoxedLong*)v)->n;
    mpz_t modified;

    int sign = mpz_sgn(*op);
    // If the value is zero, then mpz_export won't touch any of the memory, so handle that here:
    if (sign == 0) {
        memset(bytes, 0, n);
        return 0;
    }

    size_t max_bits = n * 8;
    if (is_signed)
        max_bits--;
    size_t bits;

    if (sign == -1) {
        if (!is_signed) {
            PyErr_SetString(PyExc_OverflowError, "can't convert negative long to unsigned");
            return -1;
        }

        // GMP uses sign-magnitude representation, and mpz_export just returns the magnitude.
        // This is the easiest way I could think of to convert to two's complement.
        // Note: the common case for this function is n in 1/2/4/8, where we could potentially
        // just extract the value and then do the two's complement conversion ourselves.  But
        // then we would have to worry about endianness, which we don't right now.
        mpz_init(modified);
        mpz_com(modified, *op);
        bits = mpz_sizeinbase(modified, 2);
        for (int i = 0; i < 8 * n; i++) {
            mpz_combit(modified, i);
        }
        op = &modified;
    } else {
        bits = mpz_sizeinbase(*op, 2);
    }

    if (bits > max_bits) {
        if (sign == -1)
            mpz_clear(modified);
        PyErr_SetString(PyExc_OverflowError, "long too big to convert");
        return -1;
    }

    size_t count = 0;
    mpz_export(bytes, &count, 1, n, little_endian ? -1 : 1, 0, *op);
    ASSERT(count == 1, "overflow? (%ld %ld)", count, n);

    if (sign == -1)
        mpz_clear(modified);
    return 0;
}

extern "C" PyObject* _PyLong_FromByteArray(const unsigned char* bytes, size_t n, int little_endian,
                                           int is_signed) noexcept {
    if (n == 0)
        return PyLong_FromLong(0);

    if (!little_endian) {
        // TODO: check if the behaviour of mpz_import is right when big endian is specified.
        Py_FatalError("unimplemented");
        return 0;
    }

    BoxedLong* rtn = new BoxedLong();
    mpz_init(rtn->n);
    mpz_import(rtn->n, 1, 1, n, little_endian ? -1 : 1, 0, &bytes[0]);


    RELEASE_ASSERT(little_endian, "");
    if (is_signed && bytes[n - 1] >= 0x80) { // todo add big endian support
        mpz_t t;
        mpz_init(t);
        mpz_setbit(t, n * 8);
        mpz_sub(rtn->n, rtn->n, t);
        mpz_clear(t);
    }

    return rtn;
}

extern "C" void _PyLong_AsMPZ(PyObject* obj, _PyLongMPZ num) noexcept {
    RELEASE_ASSERT(obj->cls == long_cls, "needs a long argument");
    mpz_set((mpz_ptr)num, ((BoxedLong*)obj)->n);
}

extern "C" PyObject* _PyLong_FromMPZ(const _PyLongMPZ num) noexcept {
    BoxedLong* r = new BoxedLong();
    mpz_init_set(r->n, (mpz_srcptr)num);
    return r;
}

extern "C" Box* createLong(llvm::StringRef s) {
    BoxedLong* rtn = new BoxedLong();
    assert(s.data()[s.size()] == '\0');
    int r = mpz_init_set_str(rtn->n, s.data(), 10);
    RELEASE_ASSERT(r == 0, "%d: '%s'", r, s.data());
    return rtn;
}

extern "C" BoxedLong* boxLong(int64_t n) {
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set_si(rtn->n, n);
    return rtn;
}

extern "C" PyObject* PyLong_FromLongLong(long long ival) noexcept {
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set_si(rtn->n, ival);
    return rtn;
}

extern "C" PyObject* PyLong_FromUnsignedLongLong(unsigned long long ival) noexcept {
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set_ui(rtn->n, ival);
    return rtn;
}

template <ExceptionStyle S> Box* _longNew(Box* val, Box* _base) noexcept(S == CAPI) {
    int base = 10;
    if (_base) {
        if (S == CAPI) {
            if (!PyInt_Check(_base)) {
                PyErr_Format(PyExc_TypeError, "integer argument expected, got %s", getTypeName(_base));
                return NULL;
            }

            if (val == NULL) {
                PyErr_SetString(PyExc_TypeError, "long() missing string argument");
                return NULL;
            }

            if (!PyString_Check(val) && !PyUnicode_Check(val)) {
                PyErr_SetString(PyExc_TypeError, "long() can't convert non-string with explicit base");
                return NULL;
            }
        } else {
            if (!PyInt_Check(_base))
                raiseExcHelper(TypeError, "integer argument expected, got %s", getTypeName(_base));

            if (val == NULL)
                raiseExcHelper(TypeError, "long() missing string argument");

            if (!PyString_Check(val) && !PyUnicode_Check(val))
                raiseExcHelper(TypeError, "long() can't convert non-string with explicit base");
        }
        base = static_cast<BoxedInt*>(_base)->n;
    } else {
        if (val == NULL)
            return PyLong_FromLong(0L);

        Box* r = PyNumber_Long(val);
        if (!r) {
            if (S == CAPI) {
                return NULL;
            } else
                throwCAPIException();
        }
        return r;
    }

    if (PyString_Check(val)) {
        BoxedString* s = static_cast<BoxedString*>(val);

        if (s->size() != strlen(s->data())) {
            Box* srepr = PyObject_Repr(val);
            if (S == CAPI) {
                PyErr_Format(PyExc_ValueError, "invalid literal for long() with base %d: '%s'", base,
                             PyString_AS_STRING(srepr));
                return NULL;
            } else {
                raiseExcHelper(ValueError, "invalid literal for long() with base %d: '%s'", base,
                               PyString_AS_STRING(srepr));
            }
        }
        Box* r = PyLong_FromString(s->data(), NULL, base);
        if (!r) {
            if (S == CAPI)
                return NULL;
            else
                throwCAPIException();
        }
        return r;
    } else {
        // only for unicode and its subtype, other type will be filtered out in above
        Box* r = PyLong_FromUnicode(PyUnicode_AS_UNICODE(val), PyUnicode_GET_SIZE(val), base);
        if (!r) {
            if (S == CAPI)
                return NULL;
            else
                throwCAPIException();
        }
        return r;
    }
}

template <ExceptionStyle S> Box* longNew(Box* _cls, Box* val, Box* base) noexcept(S == CAPI) {
    if (!PyType_Check(_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "long.__new__(X): X is not a type object (%s)", getTypeName(_cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "long.__new__(X): X is not a type object (%s)", getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, long_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "long.__new__(%s): %s is not a subtype of long", getNameOfClass(cls),
                         getNameOfClass(cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "long.__new__(%s): %s is not a subtype of long", getNameOfClass(cls),
                           getNameOfClass(cls));
    }

    BoxedLong* l = (BoxedLong*)_longNew<S>(val, base);

    if (cls == long_cls)
        return l;

    BoxedLong* rtn = new (cls) BoxedLong();

    mpz_init_set(rtn->n, l->n);
    return rtn;
}

Box* longInt(Box* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__int__' requires a 'long' object but received a '%s'", getTypeName(v));

    int overflow = 0;
    long n = PyLong_AsLongAndOverflow(v, &overflow);
    static_assert(sizeof(BoxedInt::n) == sizeof(long), "");
    if (overflow) {
        BoxedLong* rtn = new BoxedLong();
        mpz_init_set(rtn->n, ((BoxedLong*)v)->n);
        return rtn;
    } else
        return boxInt(n);
}

Box* longToLong(Box* self) {
    if (self->cls == long_cls) {
        return self;
    } else {
        assert(PyLong_Check(self));
        BoxedLong* l = new BoxedLong();
        mpz_init_set(l->n, static_cast<BoxedLong*>(self)->n);
        return l;
    }
}

Box* longLong(BoxedLong* self) {
    if (!PyLong_Check(self))
        raiseExcHelper(TypeError, "descriptor '__long__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    return longToLong(self);
}

Box* longToFloat(BoxedLong* v) {
    double result = PyLong_AsDouble(v);

    if (result == -1.0 && PyErr_Occurred())
        throwCAPIException();

    return new BoxedFloat(result);
}

Box* longFloat(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__float__' requires a 'long' object but received a '%s'",
                       getTypeName(v));

    return longToFloat(v);
}

Box* longRepr(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 10, 1 /* add L */, 0);
}

Box* longStr(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 10, 0 /* no L */, 0);
}

Box* longBin(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__bin__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 2, 0 /* no L */, 0);
}

Box* longHex(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__hex__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 16, 1 /* add L */, 0);
}

Box* longOct(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__oct__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 8, 1 /* add L */, 0);
}

Box* longNeg(BoxedLong* v1) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__neg__' requires a 'long' object but received a '%s'", getTypeName(v1));

    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_neg(r->n, v1->n);
    return r;
}

Box* longPos(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__pos__' requires a 'long' object but received a '%s'", getTypeName(v));

    if (v->cls == long_cls) {
        return v;
    } else {
        BoxedLong* r = new BoxedLong();
        mpz_init_set(r->n, v->n);
        return r;
    }
}

Box* longAbs(BoxedLong* v1) {
    assert(PyLong_Check(v1));
    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_abs(r->n, v1->n);
    return r;
}

Box* longAdd(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_add(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        if (v2->n >= 0)
            mpz_add_ui(r->n, v1->n, v2->n);
        else
            mpz_sub_ui(r->n, v1->n, -v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

// TODO: split common code out into a helper function
extern "C" Box* longAnd(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__and__' requires a 'long' object but received a '%s'", getTypeName(v1));
    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_and(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2_int = static_cast<BoxedInt*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_t v2_long;
        mpz_init(v2_long);
        if (v2_int->n >= 0)
            mpz_init_set_ui(v2_long, v2_int->n);
        else
            mpz_init_set_si(v2_long, v2_int->n);

        mpz_and(r->n, v1->n, v2_long);
        return r;
    }
    return NotImplemented;
}

extern "C" Box* longOr(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__or__' requires a 'long' object but received a '%s'", getTypeName(v1));
    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_ior(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2_int = static_cast<BoxedInt*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_t v2_long;
        mpz_init(v2_long);
        if (v2_int->n >= 0)
            mpz_init_set_ui(v2_long, v2_int->n);
        else
            mpz_init_set_si(v2_long, v2_int->n);

        mpz_ior(r->n, v1->n, v2_long);
        return r;
    }
    return NotImplemented;
}

extern "C" Box* longXor(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__xor__' requires a 'long' object but received a '%s'", getTypeName(v1));
    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_xor(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2_int = static_cast<BoxedInt*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_t v2_long;
        mpz_init(v2_long);
        if (v2_int->n >= 0)
            mpz_init_set_ui(v2_long, v2_int->n);
        else
            mpz_init_set_si(v2_long, v2_int->n);

        mpz_xor(r->n, v1->n, v2_long);
        return r;
    }
    return NotImplemented;
}

static PyObject* long_richcompare(Box* _v1, Box* _v2, int op) noexcept {
    RELEASE_ASSERT(PyLong_Check(_v1), "");
    BoxedLong* v1 = static_cast<BoxedLong*>(_v1);

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return convert_3way_to_object(op, mpz_cmp(v1->n, v2->n));
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return convert_3way_to_object(op, mpz_cmp_si(v1->n, v2->n));
    } else {
        return NotImplemented;
    }
}

Box* convertToLong(Box* val) {
    if (PyLong_Check(val)) {
        return val;
    } else if (PyInt_Check(val)) {
        BoxedInt* val_int = static_cast<BoxedInt*>(val);
        BoxedLong* r = new BoxedLong();
        mpz_init_set_si(r->n, val_int->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longLShiftLong(BoxedLong* lhs, Box* _rhs) {
    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);

    // if (PyLong_Check(_v2)) {
    //     BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

    if (mpz_sgn(rhs_long->n) < 0)
        raiseExcHelper(ValueError, "negative shift count");

    uint64_t n = asUnsignedLong(rhs_long);
    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_mul_2exp(r->n, lhs->n, n);
    return r;
}

Box* longLShift(BoxedLong* lhs, Box* rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__lshift__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    return longLShiftLong(lhs, rhs);
}

Box* longRLShift(BoxedLong* lhs, Box* _rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rlshift__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);

    return longLShiftLong(rhs_long, lhs);
}

Box* longRShiftLong(BoxedLong* lhs, Box* _rhs) {
    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);

    if (mpz_sgn(rhs_long->n) < 0)
        raiseExcHelper(ValueError, "negative shift count");

    uint64_t n = asUnsignedLong(rhs_long);
    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_div_2exp(r->n, lhs->n, n);
    return r;
}

Box* longRShift(BoxedLong* lhs, Box* rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rshift__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    return longRShiftLong(lhs, rhs);
}

Box* longRRShift(BoxedLong* lhs, Box* _rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rrshift__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);

    return longRShiftLong(rhs_long, lhs);
}

Box* longCoerce(BoxedLong* lhs, Box* _rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__coerce__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = convertToLong(_rhs);

    if (!PyLong_Check(rhs))
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);
    return BoxedTuple::create({ lhs, rhs_long });
}

Box* longSub(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__sub__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_sub(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        if (v2->n >= 0)
            mpz_sub_ui(r->n, v1->n, v2->n);
        else
            mpz_add_ui(r->n, v1->n, -v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longRsub(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__rsub__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    return longAdd(static_cast<BoxedLong*>(longNeg(v1)), _v2);
}

Box* longMul(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__mul__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mul(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mul_si(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longDiv(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_sgn(v2->n) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_fdiv_q(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        if (v2->n == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong();
        mpz_init_set_si(r->n, v2->n);
        mpz_fdiv_q(r->n, v1->n, r->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longFloorDiv(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__floordiv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));
    return longDiv(v1, _v2);
}

Box* longMod(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__mod__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_sgn(v2->n) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mmod(r->n, v1->n, v2->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        if (v2->n == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong();
        mpz_init_set_si(r->n, v2->n);
        mpz_mmod(r->n, v1->n, r->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longRMod(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__rmod__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    Box* lhs = _v2;
    BoxedLong* rhs = v1;
    if (PyLong_Check(lhs)) {
        return longMod((BoxedLong*)lhs, rhs);
    } else if (PyInt_Check(lhs)) {
        return longMod(boxLong(((BoxedInt*)lhs)->n), rhs);
    } else {
        return NotImplemented;
    }
}

Box* longDivmodLong(BoxedLong* lhs, Box* _rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);

    if (mpz_sgn(rhs_long->n) == 0)
        raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

    BoxedLong* q = new BoxedLong();
    BoxedLong* r = new BoxedLong();
    mpz_init(q->n);
    mpz_init(r->n);
    mpz_fdiv_qr(q->n, r->n, lhs->n, rhs_long->n);
    return BoxedTuple::create({ q, r });
}

Box* longDivmod(BoxedLong* lhs, Box* rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    return longDivmodLong(lhs, rhs);
}

Box* longRDivmod(BoxedLong* lhs, Box* _rhs) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);

    return longDivmodLong(rhs_long, lhs);
}

Box* longRdiv(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__rdiv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    if (mpz_sgn(v1->n) == 0)
        raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

    if (PyLong_Check(_v2)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_fdiv_q(r->n, v2->n, v1->n);
        return r;
    } else if (PyInt_Check(_v2)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init_set_si(r->n, v2->n);
        mpz_fdiv_q(r->n, r->n, v1->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longRfloorDiv(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__rfloordiv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    return longRdiv(v1, _v2);
}

Box* longTrueDiv(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__truediv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    BoxedLong* v2;
    if (PyInt_Check(_v2) || PyLong_Check(_v2)) {
        v2 = (BoxedLong*)PyNumber_Long(_v2);
        if (!v2) {
            throwCAPIException();
        }
    } else {
        return NotImplemented;
    }

    if (mpz_sgn(v2->n) == 0) {
        raiseExcHelper(ZeroDivisionError, "division by zero");
    }

    mpfr_t lhs_f, rhs_f, result;
    mpfr_init(result);
    mpfr_init_set_z(lhs_f, v1->n, MPFR_RNDN);
    mpfr_init_set_z(rhs_f, v2->n, MPFR_RNDZ);
    mpfr_div(result, lhs_f, rhs_f, MPFR_RNDN);

    double result_f = mpfr_get_d(result, MPFR_RNDN);

    if (isinf(result_f)) {
        raiseExcHelper(OverflowError, "integer division result too large for a float");
    }
    return boxFloat(result_f);
}

Box* longRTrueDiv(BoxedLong* v1, Box* _v2) {
    if (!PyLong_Check(v1))
        raiseExcHelper(TypeError, "descriptor '__rtruediv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    BoxedLong* v2;
    if (PyInt_Check(_v2) || PyLong_Check(_v2)) {
        v2 = (BoxedLong*)PyNumber_Long(_v2);
    } else {
        return NotImplemented;
    }
    if (mpz_sgn(v2->n) == 0) {
        raiseExcHelper(ZeroDivisionError, "division by zero");
    }

    mpfr_t lhs_f, rhs_f, result;
    mpfr_init(result);
    mpfr_init_set_z(lhs_f, v2->n, MPFR_RNDN);
    mpfr_init_set_z(rhs_f, v1->n, MPFR_RNDZ);
    mpfr_div(result, lhs_f, rhs_f, MPFR_RNDN);

    double result_f = mpfr_get_d(result, MPFR_RNDZ);
    if (isinf(result_f)) {
        raiseExcHelper(OverflowError, "integer division result too large for a float");
    }
    return boxFloat(result_f);
}

static void _addFuncPow(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* long_func) {
    std::vector<ConcreteCompilerType*> v_lfu{ UNKNOWN, BOXED_FLOAT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_uuu{ UNKNOWN, UNKNOWN, UNKNOWN };

    FunctionMetadata* md = new FunctionMetadata(3, false, false);
    md->addVersion(float_func, UNKNOWN, v_lfu);
    md->addVersion(long_func, UNKNOWN, v_uuu);
    long_cls->giveAttr(name, new BoxedFunction(md, { None }));
}

extern "C" Box* longPowFloat(BoxedLong* lhs, BoxedFloat* rhs) {
    assert(PyLong_Check(lhs));
    assert(PyFloat_Check(rhs));
    double lhs_float = static_cast<BoxedFloat*>(longFloat(lhs))->d;
    return boxFloat(pow_float_float(lhs_float, rhs->d));
}

Box* longPowLong(BoxedLong* lhs, Box* _rhs, Box* _mod) {
    BoxedLong* mod_long = nullptr;
    if (_mod != None) {
        Box* mod = convertToLong(_mod);

        if (mod == NotImplemented)
            return NotImplemented;

        mod_long = static_cast<BoxedLong*>(mod);
    }

    BoxedLong* rhs_long = nullptr;
    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    rhs_long = static_cast<BoxedLong*>(rhs);

    if (_mod != None) {
        if (mpz_sgn(rhs_long->n) < 0)
            raiseExcHelper(TypeError, "pow() 2nd argument "
                                      "cannot be negative when 3rd argument specified");
        else if (mpz_sgn(mod_long->n) == 0)
            raiseExcHelper(ValueError, "pow() 3rd argument cannot be 0");
    }

    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);

    if (mpz_sgn(rhs_long->n) == -1) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(longToFloat(rhs_long));
        BoxedFloat* lhs_float = static_cast<BoxedFloat*>(longToFloat(lhs));
        return boxFloat(pow_float_float(lhs_float->d, rhs_float->d));
    }

    if (_mod != None) {
        mpz_powm(r->n, lhs->n, rhs_long->n, mod_long->n);
        if (mpz_sgn(r->n) == 0)
            return r;
        if (mpz_sgn(mod_long->n) < 0)
            return longAdd(r, mod_long);
    } else {
        if (mpz_fits_ulong_p(rhs_long->n)) {
            uint64_t n2 = mpz_get_ui(rhs_long->n);
            mpz_pow_ui(r->n, lhs->n, n2);
        } else {
            if (mpz_cmp_si(lhs->n, 1l) == 0) {
                mpz_set_ui(r->n, 1l);
            } else if (mpz_sgn(lhs->n) == 0) {
                mpz_set_ui(r->n, 0l);
            } else if (mpz_cmp_si(lhs->n, -1l) == 0) {
                long rl = mpz_even_p(rhs_long->n) ? 1l : -1l;
                mpz_set_si(r->n, rl);
            } else {
                raiseExcHelper(OverflowError, "the result is too large to convert to long");
            }
        }
    }
    return r;
}
Box* longPow(BoxedLong* lhs, Box* rhs, Box* mod) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    return longPowLong(lhs, rhs, mod);
}

Box* longRPow(BoxedLong* lhs, Box* _rhs, Box* mod) {
    if (!PyLong_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rpow__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    Box* rhs = convertToLong(_rhs);

    if (rhs == NotImplemented)
        return NotImplemented;

    BoxedLong* rhs_long = static_cast<BoxedLong*>(rhs);
    return longPowLong(rhs_long, lhs, mod);
}

extern "C" Box* longInvert(BoxedLong* v) {
    if (!PyLong_Check(v))
        raiseExcHelper(TypeError, "descriptor '__invert__' requires a 'long' object but received a '%s'",
                       getTypeName(v));

    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_com(r->n, v->n);
    return r;
}

Box* longNonzero(BoxedLong* self) {
    if (!PyLong_Check(self))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(self));

    if (mpz_sgn(self->n) == 0)
        return False;
    return True;
}

bool longNonzeroUnboxed(BoxedLong* self) {
    return mpz_sgn(self->n) != 0;
}

Box* longHash(BoxedLong* self) {
    if (!PyLong_Check(self))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(self));

    // If the long fits into an int we have to return the same hash in order that we can find the value in a dict.
    if (mpz_fits_slong_p(self->n))
        return boxInt(mpz_get_si(self->n));

    // CPython use the absolute value of self mod ULONG_MAX.
    unsigned long remainder = mpz_tdiv_ui(self->n, ULONG_MAX);
    if (remainder == 0)
        remainder = -1; // CPython compatibility -- ULONG_MAX mod ULONG_MAX is ULONG_MAX to them.

    remainder *= mpz_sgn(self->n);

    if (remainder == -1)
        remainder = -2;

    return boxInt(remainder);
}

extern "C" Box* longTrunc(BoxedLong* self) {
    if (!PyLong_Check(self))
        raiseExcHelper(TypeError, "descriptor '__trunc__' requires a 'long' object but received a '%s'",
                       getTypeName(self));

    return self;
}

void* customised_allocation(size_t alloc_size) {
    return gc::gc_alloc(alloc_size, gc::GCKind::CONSERVATIVE);
}

void* customised_realloc(void* ptr, size_t old_size, size_t new_size) {
    return gc::gc_realloc(ptr, new_size);
}

void customised_free(void* ptr, size_t size) {
    gc::gc_free(ptr);
}

extern "C" Box* longIndex(BoxedLong* v) noexcept {
    if (PyLong_CheckExact(v))
        return v;
    BoxedLong* rtn = new BoxedLong();
    mpz_init_set(rtn->n, v->n);
    return rtn;
}

extern "C" Box* longBitLength(BoxedLong* self) noexcept {
    if (!PyLong_Check(self))
        raiseExcHelper(TypeError, "descriptor 'bit_length' requires a 'long' object but received a '%s'",
                       getTypeName(self));

    if (mpz_sgn(self->n) == 0) {
        return boxLong(0);
    }
    size_t bits = mpz_sizeinbase(self->n, 2);
    return boxLong(bits);
}

static int convert_binop(PyObject* v, PyObject* w, PyLongObject** a, PyLongObject** b) noexcept {
    if (PyLong_Check(v)) {
        *a = (PyLongObject*)v;
        Py_INCREF(v);
    } else if (PyInt_Check(v)) {
        *a = (PyLongObject*)PyLong_FromLong(PyInt_AS_LONG(v));
    } else {
        return 0;
    }
    if (PyLong_Check(w)) {
        *b = (PyLongObject*)w;
        Py_INCREF(w);
    } else if (PyInt_Check(w)) {
        *b = (PyLongObject*)PyLong_FromLong(PyInt_AS_LONG(w));
    } else {
        Py_DECREF(*a);
        return 0;
    }
    return 1;
}

#define CONVERT_BINOP(v, w, a, b)                                                                                      \
    do {                                                                                                               \
        if (!convert_binop(v, w, a, b)) {                                                                              \
            Py_INCREF(Py_NotImplemented);                                                                              \
            return Py_NotImplemented;                                                                                  \
        }                                                                                                              \
    } while (0)

static PyObject* long_pow(PyObject* v, PyObject* w, PyObject* x) noexcept {
    try {
        PyLongObject* a, *b;
        CONVERT_BINOP(v, w, &a, &b);
        return longPow((BoxedLong*)a, (BoxedLong*)b, x);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static Box* longDesc(Box* b, void*) {
    return longToLong(b);
}

static Box* long0(Box* b, void*) {
    return boxLong(0);
}

static Box* long1(Box* b, void*) {
    return boxLong(1);
}

static PyObject* long_getnewargs(PyLongObject* v) noexcept {
    return Py_BuildValue("(N)", _PyLong_Copy(v));
}

void setupLong() {
    static PyNumberMethods long_as_number;
    long_cls->tp_as_number = &long_as_number;

    mp_set_memory_functions(customised_allocation, customised_realloc, customised_free);

    _addFuncPow("__pow__", UNKNOWN, (void*)longPowFloat, (void*)longPow);
    long_cls->giveAttr(
        "__rpow__", new BoxedFunction(FunctionMetadata::create((void*)longRPow, UNKNOWN, 3, false, false), { None }));
    auto long_new = FunctionMetadata::create((void*)longNew<CXX>, UNKNOWN, 3, false, false,
                                             ParamNames({ "", "x", "base" }, "", ""), CXX);
    long_new->addVersion((void*)longNew<CAPI>, UNKNOWN, CAPI);
    long_cls->giveAttr("__new__", new BoxedFunction(long_new, { NULL, NULL }));

    long_cls->giveAttr("__mul__", new BoxedFunction(FunctionMetadata::create((void*)longMul, UNKNOWN, 2)));
    long_cls->giveAttr("__rmul__", long_cls->getattr(internStringMortal("__mul__")));

    long_cls->giveAttr("__div__", new BoxedFunction(FunctionMetadata::create((void*)longDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rdiv__", new BoxedFunction(FunctionMetadata::create((void*)longRdiv, UNKNOWN, 2)));
    long_cls->giveAttr("__floordiv__", new BoxedFunction(FunctionMetadata::create((void*)longFloorDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rfloordiv__", new BoxedFunction(FunctionMetadata::create((void*)longRfloorDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__truediv__", new BoxedFunction(FunctionMetadata::create((void*)longTrueDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rtruediv__", new BoxedFunction(FunctionMetadata::create((void*)longRTrueDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__mod__", new BoxedFunction(FunctionMetadata::create((void*)longMod, UNKNOWN, 2)));
    long_cls->giveAttr("__rmod__", new BoxedFunction(FunctionMetadata::create((void*)longRMod, UNKNOWN, 2)));

    long_cls->giveAttr("__divmod__", new BoxedFunction(FunctionMetadata::create((void*)longDivmod, UNKNOWN, 2)));
    long_cls->giveAttr("__rdivmod__", new BoxedFunction(FunctionMetadata::create((void*)longRDivmod, UNKNOWN, 2)));

    long_cls->giveAttr("__sub__", new BoxedFunction(FunctionMetadata::create((void*)longSub, UNKNOWN, 2)));
    long_cls->giveAttr("__rsub__", new BoxedFunction(FunctionMetadata::create((void*)longRsub, UNKNOWN, 2)));

    long_cls->giveAttr("__add__", new BoxedFunction(FunctionMetadata::create((void*)longAdd, UNKNOWN, 2)));
    long_cls->giveAttr("__radd__", long_cls->getattr(internStringMortal("__add__")));
    long_cls->giveAttr("__and__", new BoxedFunction(FunctionMetadata::create((void*)longAnd, UNKNOWN, 2)));
    long_cls->giveAttr("__rand__", long_cls->getattr(internStringMortal("__and__")));
    long_cls->giveAttr("__or__", new BoxedFunction(FunctionMetadata::create((void*)longOr, UNKNOWN, 2)));
    long_cls->giveAttr("__ror__", long_cls->getattr(internStringMortal("__or__")));
    long_cls->giveAttr("__xor__", new BoxedFunction(FunctionMetadata::create((void*)longXor, UNKNOWN, 2)));
    long_cls->giveAttr("__rxor__", long_cls->getattr(internStringMortal("__xor__")));

    // Note: CPython implements long comparisons using tp_compare
    long_cls->tp_richcompare = long_richcompare;

    long_cls->giveAttr("__lshift__", new BoxedFunction(FunctionMetadata::create((void*)longLShift, UNKNOWN, 2)));
    long_cls->giveAttr("__rlshift__", new BoxedFunction(FunctionMetadata::create((void*)longRLShift, UNKNOWN, 2)));
    long_cls->giveAttr("__rshift__", new BoxedFunction(FunctionMetadata::create((void*)longRShift, UNKNOWN, 2)));
    long_cls->giveAttr("__rrshift__", new BoxedFunction(FunctionMetadata::create((void*)longRRShift, UNKNOWN, 2)));
    long_cls->giveAttr("__coerce__", new BoxedFunction(FunctionMetadata::create((void*)longCoerce, UNKNOWN, 2)));

    long_cls->giveAttr("__int__", new BoxedFunction(FunctionMetadata::create((void*)longInt, UNKNOWN, 1)));
    long_cls->giveAttr("__float__", new BoxedFunction(FunctionMetadata::create((void*)longFloat, UNKNOWN, 1)));
    long_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)longRepr, STR, 1)));
    long_cls->giveAttr("__str__", new BoxedFunction(FunctionMetadata::create((void*)longStr, STR, 1)));
    long_cls->giveAttr("__bin__", new BoxedFunction(FunctionMetadata::create((void*)longBin, STR, 1)));
    long_cls->giveAttr("__hex__", new BoxedFunction(FunctionMetadata::create((void*)longHex, STR, 1)));
    long_cls->giveAttr("__oct__", new BoxedFunction(FunctionMetadata::create((void*)longOct, STR, 1)));

    long_cls->giveAttr("__abs__", new BoxedFunction(FunctionMetadata::create((void*)longAbs, UNKNOWN, 1)));
    long_cls->giveAttr("__invert__", new BoxedFunction(FunctionMetadata::create((void*)longInvert, UNKNOWN, 1)));
    long_cls->giveAttr("__neg__", new BoxedFunction(FunctionMetadata::create((void*)longNeg, UNKNOWN, 1)));
    long_cls->giveAttr("__pos__", new BoxedFunction(FunctionMetadata::create((void*)longPos, UNKNOWN, 1)));
    long_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)longNonzero, BOXED_BOOL, 1)));
    long_cls->giveAttr("__hash__", new BoxedFunction(FunctionMetadata::create((void*)longHash, BOXED_INT, 1)));

    long_cls->giveAttr("__long__", new BoxedFunction(FunctionMetadata::create((void*)longLong, UNKNOWN, 1)));
    long_cls->giveAttr("__trunc__", new BoxedFunction(FunctionMetadata::create((void*)longTrunc, UNKNOWN, 1)));
    long_cls->giveAttr("__index__", new BoxedFunction(FunctionMetadata::create((void*)longIndex, LONG, 1)));

    long_cls->giveAttr("bit_length", new BoxedFunction(FunctionMetadata::create((void*)longBitLength, LONG, 1)));
    long_cls->giveAttrDescriptor("real", longDesc, NULL);
    long_cls->giveAttrDescriptor("imag", long0, NULL);
    long_cls->giveAttr("conjugate", new BoxedFunction(FunctionMetadata::create((void*)longDesc, UNKNOWN, 1)));
    long_cls->giveAttrDescriptor("numerator", longDesc, NULL);
    long_cls->giveAttrDescriptor("denominator", long1, NULL);

    long_cls->giveAttr("__getnewargs__", new BoxedFunction(FunctionMetadata::create((void*)long_getnewargs, UNKNOWN, 1,
                                                                                    ParamNames::empty(), CAPI)));

    long_cls->giveAttr("__doc__", boxString("long.bit_length() -> int or long\n"
                                            "\n"
                                            "Number of bits necessary to represent self in binary.\n"
                                            ">>> bin(37L)\n"
                                            "'0b100101'\n"
                                            ">>> (37L).bit_length()\n"
                                            "6"));

    add_operators(long_cls);
    long_cls->freeze();

    long_cls->tp_as_number->nb_power = long_pow;
}
}
