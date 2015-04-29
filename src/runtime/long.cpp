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

#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/capi.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedClass* long_cls;

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
    Py_FatalError("unimplemented");
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

    // if this is ever true, we should raise a Python error, but I don't think we should hit it?
    assert(mpz_cmp_si(self->n, 0) >= 0);

    if (!mpz_fits_ulong_p(self->n))
        raiseExcHelper(OverflowError, "long int too large to convert to int");
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
        abort();
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
    return mpz_get_d(l->n);
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
    else if (base == 8)
        os << (newstyle ? "0o" : "0");
    else if (base == 16)
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
    Py_FatalError("unimplemented");
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

extern "C" Box* createLong(const std::string* s) {
    BoxedLong* rtn = new BoxedLong();
    int r = mpz_init_set_str(rtn->n, s->c_str(), 10);
    RELEASE_ASSERT(r == 0, "%d: '%s'", r, s->c_str());
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

        rtn = (BoxedLong*)PyLong_FromString(s->s.str().c_str(), NULL, base);
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
            const std::string& s = static_cast<BoxedString*>(val)->s;
            int r = mpz_init_set_str(rtn->n, s.c_str(), 10);
            RELEASE_ASSERT(r == 0, "");
        } else if (val->cls == float_cls) {
            mpz_init_set_si(rtn->n, static_cast<BoxedFloat*>(val)->d);
        } else {
            static const std::string long_str("__long__");
            Box* r = callattr(val, &long_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = true }),
                              ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);

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

// TODO reduce duplication between these 6 functions, and add double support
Box* longGt(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__gt__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) > 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) > 0);
    } else {
        return NotImplemented;
    }
}

Box* longGe(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__ge__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) >= 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) >= 0);
    } else {
        return NotImplemented;
    }
}

Box* longLt(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__lt__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) < 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) < 0);
    } else {
        return NotImplemented;
    }
}

Box* longLe(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__le__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) <= 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) <= 0);
    } else {
        return NotImplemented;
    }
}

Box* longEq(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) == 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) == 0);
    } else {
        return NotImplemented;
    }
}

Box* longNe(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__ne__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        return boxBool(mpz_cmp(v1->n, v2->n) != 0);
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);

        return boxBool(mpz_cmp_si(v1->n, v2->n) != 0);
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

        if (mpz_cmp_si(v2->n, 0) < 0)
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

        if (mpz_cmp_si(v2->n, 0) < 0)
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

        if (mpz_cmp_si(v2->n, 0) == 0)
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

Box* longMod(BoxedLong* v1, Box* _v2) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__mod__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);

        if (mpz_cmp_si(v2->n, 0) == 0)
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

        if (mpz_cmp_si(rhs->n, 0) == 0)
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
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'long' object but received a '%s'", getTypeName(v1));

    if (mpz_cmp_si(v1->n, 0) == 0)
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

Box* longPow(BoxedLong* v1, Box* _v2, Box* _v3) {
    if (!isSubclass(v1->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'", getTypeName(v1));

    BoxedLong* mod = nullptr;
    if (_v3 != None) {
        if (isSubclass(_v3->cls, int_cls))
            mod = boxLong(((BoxedInt*)_v3)->n);
        else {
            RELEASE_ASSERT(_v3->cls == long_cls, "");
            mod = (BoxedLong*)_v3;
        }
        RELEASE_ASSERT(mpz_sgn(mod->n) >= 0, "");
    }

    if (isSubclass(_v2->cls, long_cls)) {
        BoxedLong* v2 = static_cast<BoxedLong*>(_v2);
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);

        RELEASE_ASSERT(mpz_sgn(v2->n) >= 0, "");

        if (mod) {
            mpz_powm(r->n, v1->n, v2->n, mod->n);
        } else {
            RELEASE_ASSERT(mpz_fits_ulong_p(v2->n), "");
            uint64_t n2 = mpz_get_ui(v2->n);
            mpz_pow_ui(r->n, v1->n, n2);
        }
        return r;
    } else if (isSubclass(_v2->cls, int_cls)) {
        BoxedInt* v2 = static_cast<BoxedInt*>(_v2);
        RELEASE_ASSERT(v2->n >= 0, "");
        BoxedLong* r = new BoxedLong();
        mpz_init(r->n);
        if (mod)
            mpz_powm_ui(r->n, v1->n, v2->n, mod->n);
        else
            mpz_pow_ui(r->n, v1->n, v2->n);
        return r;
    } else {
        return NotImplemented;
    }
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

    if (mpz_cmp_si(self->n, 0) == 0)
        return False;
    return True;
}

Box* longHash(BoxedLong* self) {
    if (!isSubclass(self->cls, long_cls))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'long' object but received a '%s'",
                       getTypeName(self));

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
        abort();
    }
}

void setupLong() {
    mp_set_memory_functions(customised_allocation, customised_realloc, customised_free);

    long_cls->giveAttr(
        "__new__", new BoxedFunction(boxRTFunction((void*)longNew, UNKNOWN, 3, 2, false, false), { boxInt(0), NULL }));

    long_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)longMul, UNKNOWN, 2)));
    long_cls->giveAttr("__rmul__", long_cls->getattr("__mul__"));

    long_cls->giveAttr("__div__", new BoxedFunction(boxRTFunction((void*)longDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rdiv__", new BoxedFunction(boxRTFunction((void*)longRdiv, UNKNOWN, 2)));
    long_cls->giveAttr("__truediv__", new BoxedFunction(boxRTFunction((void*)longTrueDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__rtruediv__", new BoxedFunction(boxRTFunction((void*)longRTrueDiv, UNKNOWN, 2)));
    long_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)longMod, UNKNOWN, 2)));
    long_cls->giveAttr("__rmod__", new BoxedFunction(boxRTFunction((void*)longRMod, UNKNOWN, 2)));

    long_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)longDivmod, UNKNOWN, 2)));

    long_cls->giveAttr("__sub__", new BoxedFunction(boxRTFunction((void*)longSub, UNKNOWN, 2)));
    long_cls->giveAttr("__rsub__", new BoxedFunction(boxRTFunction((void*)longRsub, UNKNOWN, 2)));

    long_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)longAdd, UNKNOWN, 2)));
    long_cls->giveAttr("__radd__", long_cls->getattr("__add__"));

    long_cls->giveAttr("__pow__",
                       new BoxedFunction(boxRTFunction((void*)longPow, UNKNOWN, 3, 1, false, false), { None }));

    long_cls->giveAttr("__and__", new BoxedFunction(boxRTFunction((void*)longAnd, UNKNOWN, 2)));
    long_cls->giveAttr("__rand__", long_cls->getattr("__and__"));
    long_cls->giveAttr("__or__", new BoxedFunction(boxRTFunction((void*)longOr, UNKNOWN, 2)));
    long_cls->giveAttr("__ror__", long_cls->getattr("__or__"));
    long_cls->giveAttr("__xor__", new BoxedFunction(boxRTFunction((void*)longXor, UNKNOWN, 2)));
    long_cls->giveAttr("__rxor__", long_cls->getattr("__xor__"));

    long_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)longGt, UNKNOWN, 2)));
    long_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)longGe, UNKNOWN, 2)));
    long_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)longLt, UNKNOWN, 2)));
    long_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)longLe, UNKNOWN, 2)));
    long_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)longEq, UNKNOWN, 2)));
    long_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)longNe, UNKNOWN, 2)));

    long_cls->giveAttr("__lshift__", new BoxedFunction(boxRTFunction((void*)longLshift, UNKNOWN, 2)));
    long_cls->giveAttr("__rshift__", new BoxedFunction(boxRTFunction((void*)longRshift, UNKNOWN, 2)));

    long_cls->giveAttr("__int__", new BoxedFunction(boxRTFunction((void*)longInt, UNKNOWN, 1)));
    long_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)longRepr, STR, 1)));
    long_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)longStr, STR, 1)));
    long_cls->giveAttr("__hex__", new BoxedFunction(boxRTFunction((void*)longHex, STR, 1)));
    long_cls->giveAttr("__oct__", new BoxedFunction(boxRTFunction((void*)longOct, STR, 1)));

    long_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)longInvert, UNKNOWN, 1)));
    long_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)longNeg, UNKNOWN, 1)));
    long_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)longNonzero, BOXED_BOOL, 1)));
    long_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)longHash, BOXED_INT, 1)));

    long_cls->giveAttr("__trunc__", new BoxedFunction(boxRTFunction((void*)longTrunc, UNKNOWN, 1)));
    long_cls->giveAttr("__index__", new BoxedFunction(boxRTFunction((void*)longIndex, LONG, 1)));

    long_cls->freeze();

    long_cls->tp_as_number->nb_power = long_pow;
}
}
