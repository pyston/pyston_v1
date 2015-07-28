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
#include <gmp.h>
#include <sstream>

#include "llvm/Support/raw_ostream.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
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
    boxGCHandler(v, b);

    BoxedLong* l = (BoxedLong*)b;

    v->visitPotentialRange((void**)&l->n, (void**)((&l->n) + 1));
}

extern "C" int _PyLong_Sign(PyObject* l) noexcept {
    return mpz_sgn(static_cast<BoxedLong*>(l)->n);
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
    RELEASE_ASSERT(pend == NULL, "unsupported");

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
    if (str[strlen(str) - 1] == 'L') {
        std::string without_l(str, strlen(str) - 1);
        int r = mpz_init_set_str(rtn->n, without_l.c_str(), base);
        RELEASE_ASSERT(r == 0, "");
    } else {
        int r = mpz_init_set_str(rtn->n, str, base);
        RELEASE_ASSERT(r == 0, "");
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

    double result = mpz_get_d(l->n);

    if (std::isinf(result)) {
        PyErr_SetString(PyExc_OverflowError, "long int too large to convert to float");
        return -1;
    }

    return result;
}

/* Convert the long to a string object with given base,
   appending a base prefix of 0[box] if base is 2, 8 or 16.
   Add a trailing "L" if addL is non-zero.
   If newstyle is zero, then use the pre-2.6 behavior of octal having
   a leading "0", instead of the prefix "0o" */
extern "C" PyAPI_FUNC(PyObject*) _PyLong_Format(PyObject* aa, int base, int addL, int newstyle) noexcept {
    BoxedLong* v = (BoxedLong*)aa;

    RELEASE_ASSERT(isSubclass(v->cls, long_cls), "");
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
    Py_FatalError("unimplemented");
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

BoxedLong* _longNew(Box* val, Box* _base) {
    BoxedLong* rtn = new BoxedLong();
    if (_base) {
        if (!isSubclass(_base->cls, int_cls))
            raiseExcHelper(TypeError, "an integer is required");
        int base = static_cast<BoxedInt*>(_base)->n;

        if (!isSubclass(val->cls, str_cls))
            raiseExcHelper(TypeError, "long() can't convert non-string with explicit base");
        BoxedString* s = static_cast<BoxedString*>(val);

        rtn = (BoxedLong*)PyLong_FromString(s->data(), NULL, base);
        checkAndThrowCAPIException();
    } else {
        if (isSubclass(val->cls, long_cls)) {
            BoxedLong* l = static_cast<BoxedLong*>(val);
            if (val->cls == long_cls)
                return l;
            BoxedLong* rtn = new BoxedLong();
            mpz_init_set(rtn->n, l->n);
            return rtn;
        } else if (isSubclass(val->cls, int_cls)) {
            mpz_init_set_si(rtn->n, static_cast<BoxedInt*>(val)->n);
        } else if (val->cls == str_cls) {
            llvm::StringRef s = static_cast<BoxedString*>(val)->s();
            assert(s.data()[s.size()] == '\0');
            int r = mpz_init_set_str(rtn->n, s.data(), 10);
            RELEASE_ASSERT(r == 0, "");
        } else if (val->cls == float_cls) {
            mpz_init_set_si(rtn->n, static_cast<BoxedFloat*>(val)->d);
        } else {
            static BoxedString* long_str = internStringImmortal("__long__");
            CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
            Box* r = callattr(val, long_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);

            if (!r) {
                fprintf(stderr, "TypeError: long() argument must be a string or a number, not '%s'\n",
                        getTypeName(val));
                raiseExcHelper(TypeError, "");
            }

            if (isSubclass(r->cls, int_cls)) {
                mpz_init_set_si(rtn->n, static_cast<BoxedInt*>(r)->n);
            } else if (!isSubclass(r->cls, long_cls)) {
                raiseExcHelper(TypeError, "__long__ returned non-long (type %s)", r->cls->tp_name);
            } else {
                return static_cast<BoxedLong*>(r);
            }
        }
    }
    return rtn;
}

extern "C" Box* longNew(Box* _cls, Box* val, Box* _base) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "long.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, long_cls))
        raiseExcHelper(TypeError, "long.__new__(%s): %s is not a subtype of long", getNameOfClass(cls),
                       getNameOfClass(cls));

    BoxedLong* l = _longNew(val, _base);
    if (cls == long_cls)
        return l;

    BoxedLong* rtn = new (cls) BoxedLong();

    mpz_init_set(rtn->n, l->n);
    return rtn;
}

Box* longInt(Box* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__int__' requires a 'long' object but received a '%s'", getTypeName(v));

    int overflow = 0;
    long n = PyLong_AsLongAndOverflow(v, &overflow);
    static_assert(sizeof(BoxedInt::n) == sizeof(long), "");
    if (overflow)
        return v;
    else
        return new BoxedInt(n);
}

Box* longFloat(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__float__' requires a 'long' object but received a '%s'",
                       getTypeName(v));

    double result = PyLong_AsDouble(v);

    if (result == -1)
        checkAndThrowCAPIException();

    return new BoxedFloat(result);
}

Box* longRepr(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 10, 1 /* add L */, 0);
}

Box* longStr(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 10, 0 /* no L */, 0);
}

Box* longHex(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__hex__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 16, 1 /* add L */, 0);
}

Box* longOct(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__oct__' requires a 'long' object but received a '%s'", getTypeName(v));
    return _PyLong_Format(v, 8, 1 /* add L */, 0);
}

Box* longNeg(BoxedLong* v1) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__neg__' requires a 'long' object but received a '%s'", getTypeName(v1));

    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_neg(r->n, v1->n);
    return r;
}

Box* longPos(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
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
    assert(isSubclass(v1->cls, long_cls));
    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_abs(r->n, v1->n);
    return r;
}

Box* longAdd(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_add(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__and__' requires a 'long' object but received a '%s'", getTypeName(v1));
    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_and(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__or__' requires a 'long' object but received a '%s'", getTypeName(v1));
    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_ior(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__xor__' requires a 'long' object but received a '%s'", getTypeName(v1));
    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_xor(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    RELEASE_ASSERT(isSubclass(_v1->cls, long_cls), "");
    BoxedLong* v1 = static_cast<BoxedLong*>(_v1);

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return convert_3way_to_object(op, mpz_cmp(v1->n, v2->n));
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return convert_3way_to_object(op, mpz_cmp_si(v1->n, v2->n));
    } else {
        return NotImplemented;
    }
}

Box* longLshift(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__lshift__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_sgn(v2->n) < 0)
            raiseExcHelper(ValueError, "negative shift count");

        uint64_t n = asUnsignedLong(v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mul_2exp(r->n, v1->n, n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);
        if (v2->n < 0)
            raiseExcHelper(ValueError, "negative shift count");

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mul_2exp(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longRshift(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rshift__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_sgn(v2->n) < 0)
            raiseExcHelper(ValueError, "negative shift count");

        uint64_t n = asUnsignedLong(v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_div_2exp(r->n, v1->n, n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);
        if (v2->n < 0)
            raiseExcHelper(ValueError, "negative shift count");

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_div_2exp(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
}

Box* longSub(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__sub__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_sub(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rsub__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    return longAdd(static_cast<BoxedLong*>(longNeg(v1)), _v2);
}

Box* longMul(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__mul__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mul(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_sgn(v2->n) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_fdiv_q(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__floordiv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));
    return longDiv(v1, _v2);
}

Box* longMod(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__mod__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_sgn(v2->n) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_mmod(r->n, v1->n, v2->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rmod__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    Box* lhs = _v2;
    BoxedLong* rhs = v1;
    if (isSubclass(lhs->cls, long_cls)) {
        return longMod((BoxedLong*)lhs, rhs);
    } else if (isSubclass(lhs->cls, int_cls)) {
        return longMod(boxLong(((BoxedInt*)lhs)->n), rhs);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* longDivmod(BoxedLong* lhs, Box* _rhs) {
    if (!isSubclass(lhs->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    if (isSubclass(_rhs->cls, long_cls)) {
        BoxedLong* rhs = static_cast<BoxedLong*>(_rhs);

        if (mpz_sgn(rhs->n) == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* q = new BoxedLong();
        BoxedLong* r = new BoxedLong();
        mpz_init(q->n);
        mpz_init(r->n);
        mpz_fdiv_qr(q->n, r->n, lhs->n, rhs->n);
        return BoxedTuple::create({ q, r });
    } else if (isSubclass(_rhs->cls, int_cls)) {
        BoxedInt* rhs = static_cast<BoxedInt*>(_rhs);

        if (rhs->n == 0)
            raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

        BoxedLong* q = new BoxedLong();
        BoxedLong* r = new BoxedLong();
        mpz_init(q->n);
        mpz_init_set_si(r->n, rhs->n);
        mpz_fdiv_qr(q->n, r->n, lhs->n, r->n);
        return BoxedTuple::create({ q, r });
    } else {
        return NotImplemented;
    }
}

Box* longRdiv(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rdiv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    if (mpz_sgn(v1->n) == 0)
        raiseExcHelper(ZeroDivisionError, "long division or modulo by zero");

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        mpz_fdiv_q(r->n, v2->n, v1->n);
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
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
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rfloordiv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    return longRdiv(v1, _v2);
}

Box* longTrueDiv(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__truediv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    // We only support args which fit into an int for now...
    int overflow = 0;
    long lhs = PyLong_AsLongAndOverflow(v1, &overflow);
    if (overflow)
        return NotImplemented;
    long rhs = PyLong_AsLongAndOverflow(_v2, &overflow);
    if (overflow)
        return NotImplemented;

    if (rhs == 0)
        raiseExcHelper(ZeroDivisionError, "division by zero");
    return boxFloat(lhs / (double)rhs);
}

Box* longRTrueDiv(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__rtruediv__' requires a 'long' object but received a '%s'",
                       getTypeName(v1));

    // We only support args which fit into an int for now...
    int overflow = 0;
    long lhs = PyLong_AsLongAndOverflow(_v2, &overflow);
    if (overflow)
        return NotImplemented;
    long rhs = PyLong_AsLongAndOverflow(v1, &overflow);
    if (overflow)
        return NotImplemented;

    if (rhs == 0)
        raiseExcHelper(ZeroDivisionError, "division by zero");
    return boxFloat(lhs / (double)rhs);
}

static void _addFuncPow(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* long_func) {
    std::vector<ConcreteCompilerType*> v_lfu{ UNKNOWN, BOXED_FLOAT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_uuu{ UNKNOWN, UNKNOWN, UNKNOWN };

    CLFunction* cl = createRTFunction(3, 1, false, false);
    addRTFunction(cl, float_func, UNKNOWN, v_lfu);
    addRTFunction(cl, long_func, UNKNOWN, v_uuu);
    long_cls->giveAttr(name, new BoxedFunction(cl, { None }));
}

extern "C" Box* longPowFloat(BoxedLong* lhs, BoxedFloat* rhs) {
    assert(isSubclass(lhs->cls, long_cls));
    assert(isSubclass(rhs->cls, float_cls));
    double lhs_float = static_cast<BoxedFloat*>(longFloat(lhs))->d;
    return boxFloat(pow_float_float(lhs_float, rhs->d));
}

Box* longPow(BoxedLong* lhs, Box* rhs, Box* mod) {
    if (!isSubclass(lhs->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(lhs));

    BoxedLong* mod_long = nullptr;
    if (mod != None) {
        if (isSubclass(mod->cls, long_cls)) {
            mod_long = static_cast<BoxedLong*>(mod);
        } else if (isSubclass(mod->cls, int_cls)) {
            mod_long = boxLong(static_cast<BoxedInt*>(mod)->n);
        } else {
            return NotImplemented;
        }
    }

    BoxedLong* rhs_long = nullptr;
    if (isSubclass(rhs->cls, long_cls)) {
        rhs_long = static_cast<BoxedLong*>(rhs);
    } else if (isSubclass(rhs->cls, int_cls)) {
        rhs_long = boxLong(static_cast<BoxedInt*>(rhs)->n);
    } else {
        return NotImplemented;
    }

    if (mod != None) {
        if (mpz_sgn(rhs_long->n) < 0)
            raiseExcHelper(TypeError, "pow() 2nd argument "
                                      "cannot be negative when 3rd argument specified");
        else if (mpz_sgn(mod_long->n) == 0)
            raiseExcHelper(ValueError, "pow() 3rd argument cannot be 0");
    }

    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);

    if (mpz_sgn(rhs_long->n) == -1) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(longFloat(rhs_long));
        BoxedFloat* lhs_float = static_cast<BoxedFloat*>(longFloat(lhs));
        return boxFloat(pow_float_float(lhs_float->d, rhs_float->d));
    }

    if (mod != None) {
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

extern "C" Box* longInvert(BoxedLong* v) {
    if (!isSubclass(v->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__invert__' requires a 'long' object but received a '%s'",
                       getTypeName(v));

    BoxedLong* r = new BoxedLong();
    mpz_init(r->n);
    mpz_com(r->n, v->n);
    return r;
}

Box* longNonzero(BoxedLong* self) {
    if (!isSubclass(self->cls, long_cls))
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
    if (!isSubclass(self->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(self));

    // If the long fits into an int we have to return the same hash in order that we can find the value in a dict.
    if (mpz_fits_slong_p(self->n))
        return boxInt(mpz_get_si(self->n));

    // Not sure if this is a good hash function or not;
    // simple, but only includes top bits:
    union {
        uint64_t n;
        double d;
    };
    d = mpz_get_d(self->n);
    return boxInt(n);
}

extern "C" Box* longTrunc(BoxedLong* self) {
    if (!isSubclass(self->cls, long_cls))
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

static Box* longLong(Box* b, void*) {
    if (b->cls == long_cls) {
        return b;
    } else {
        assert(PyLong_Check(b));
        BoxedLong* l = new BoxedLong();
        mpz_init_set(l->n, static_cast<BoxedLong*>(b)->n);
        return l;
    }
}

static Box* long0(Box* b, void*) {
    return boxLong(0);
}

static Box* long1(Box* b, void*) {
    return boxLong(1);
}

void setupLong() {
    mp_set_memory_functions(customised_allocation, customised_realloc, customised_free);

    _addFuncPow("__pow__", UNKNOWN, (void*)longPowFloat, (void*)longPow);
    long_cls->giveAttr(
        "__new__", new BoxedFunction(boxRTFunction((void*)longNew, UNKNOWN, 3, 2, false, false), { boxInt(0), NULL }));

    long_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)longMul, UNKNOWN, 2)));
    long_cls->giveAttr("__rmul__", long_cls->getattr(internStringMortal("__mul__")));

    long_cls->giveAttr("__div__", new BoxedFunction(boxRTFunction((void*)longDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rdiv__", new BoxedFunction(boxRTFunction((void*)longRdiv, UNKNOWN, 2)));
    long_cls->giveAttr("__floordiv__", new BoxedFunction(boxRTFunction((void*)longFloorDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rfloordiv__", new BoxedFunction(boxRTFunction((void*)longRfloorDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__truediv__", new BoxedFunction(boxRTFunction((void*)longTrueDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rtruediv__", new BoxedFunction(boxRTFunction((void*)longRTrueDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)longMod, UNKNOWN, 2)));
    long_cls->giveAttr("__rmod__", new BoxedFunction(boxRTFunction((void*)longRMod, UNKNOWN, 2)));

    long_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)longDivmod, UNKNOWN, 2)));

    long_cls->giveAttr("__sub__", new BoxedFunction(boxRTFunction((void*)longSub, UNKNOWN, 2)));
    long_cls->giveAttr("__rsub__", new BoxedFunction(boxRTFunction((void*)longRsub, UNKNOWN, 2)));

    long_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)longAdd, UNKNOWN, 2)));
    long_cls->giveAttr("__radd__", long_cls->getattr(internStringMortal("__add__")));
    long_cls->giveAttr("__and__", new BoxedFunction(boxRTFunction((void*)longAnd, UNKNOWN, 2)));
    long_cls->giveAttr("__rand__", long_cls->getattr(internStringMortal("__and__")));
    long_cls->giveAttr("__or__", new BoxedFunction(boxRTFunction((void*)longOr, UNKNOWN, 2)));
    long_cls->giveAttr("__ror__", long_cls->getattr(internStringMortal("__or__")));
    long_cls->giveAttr("__xor__", new BoxedFunction(boxRTFunction((void*)longXor, UNKNOWN, 2)));
    long_cls->giveAttr("__rxor__", long_cls->getattr(internStringMortal("__xor__")));

    // Note: CPython implements long comparisons using tp_compare
    long_cls->tp_richcompare = long_richcompare;

    long_cls->giveAttr("__lshift__", new BoxedFunction(boxRTFunction((void*)longLshift, UNKNOWN, 2)));
    long_cls->giveAttr("__rshift__", new BoxedFunction(boxRTFunction((void*)longRshift, UNKNOWN, 2)));

    long_cls->giveAttr("__int__", new BoxedFunction(boxRTFunction((void*)longInt, UNKNOWN, 1)));
    long_cls->giveAttr("__float__", new BoxedFunction(boxRTFunction((void*)longFloat, UNKNOWN, 1)));
    long_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)longRepr, STR, 1)));
    long_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)longStr, STR, 1)));
    long_cls->giveAttr("__hex__", new BoxedFunction(boxRTFunction((void*)longHex, STR, 1)));
    long_cls->giveAttr("__oct__", new BoxedFunction(boxRTFunction((void*)longOct, STR, 1)));

    long_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)longInvert, UNKNOWN, 1)));
    long_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)longNeg, UNKNOWN, 1)));
    long_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)longPos, UNKNOWN, 1)));
    long_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)longNonzero, BOXED_BOOL, 1)));
    long_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)longHash, BOXED_INT, 1)));

    long_cls->giveAttr("__trunc__", new BoxedFunction(boxRTFunction((void*)longTrunc, UNKNOWN, 1)));
    long_cls->giveAttr("__index__", new BoxedFunction(boxRTFunction((void*)longIndex, LONG, 1)));

    long_cls->giveAttr("real", new (pyston_getset_cls) BoxedGetsetDescriptor(longLong, NULL, NULL));
    long_cls->giveAttr("imag", new (pyston_getset_cls) BoxedGetsetDescriptor(long0, NULL, NULL));
    long_cls->giveAttr("conjugate", new BoxedFunction(boxRTFunction((void*)longLong, UNKNOWN, 1)));
    long_cls->giveAttr("numerator", new (pyston_getset_cls) BoxedGetsetDescriptor(longLong, NULL, NULL));
    long_cls->giveAttr("denominator", new (pyston_getset_cls) BoxedGetsetDescriptor(long1, NULL, NULL));

    add_operators(long_cls);
    long_cls->freeze();

    long_cls->tp_as_number->nb_power = long_pow;
}
}
