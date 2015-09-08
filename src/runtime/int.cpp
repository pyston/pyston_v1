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

#include "runtime/int.h"

#include <cmath>
#include <sstream>

#include "capi/typeobject.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" PyObject* float_pow(PyObject* v, PyObject* w, PyObject* z) noexcept;

namespace pyston {

extern "C" long PyInt_GetMax() noexcept {
    return LONG_MAX; /* To initialize sys.maxint */
}

extern "C" unsigned long PyInt_AsUnsignedLongMask(PyObject* op) noexcept {
    if (op && PyInt_Check(op))
        return PyInt_AS_LONG((PyIntObject*)op);
    if (op && PyLong_Check(op))
        return PyLong_AsUnsignedLongMask(op);
    Py_FatalError("unimplemented");
}

extern "C" long PyInt_AsLong(PyObject* op) noexcept {
    // This method should do quite a bit more, including checking tp_as_number->nb_int (or calling __int__?)

    if (PyInt_Check(op))
        return static_cast<BoxedInt*>(op)->n;

    if (op->cls == long_cls)
        return PyLong_AsLong(op);

    PyErr_SetString(PyExc_TypeError, "an integer is required");
    return -1;
}

extern "C" Py_ssize_t PyInt_AsSsize_t(PyObject* op) noexcept {
    if (op == NULL) {
        PyErr_SetString(PyExc_TypeError, "an integer is required");
        return -1;
    }

    if (PyInt_Check(op))
        return ((BoxedInt*)op)->n;
    if (PyLong_Check(op))
        return _PyLong_AsSsize_t(op);
#if SIZEOF_SIZE_T == SIZEOF_LONG
    return PyInt_AsLong(op);
#else
    RELEASE_ASSERT("not implemented", "");
#endif
}

extern "C" PyObject* PyInt_FromSize_t(size_t ival) noexcept {
    RELEASE_ASSERT(ival <= LONG_MAX, "");
    return boxInt(ival);
}

extern "C" PyObject* PyInt_FromSsize_t(Py_ssize_t ival) noexcept {
    return boxInt(ival);
}

extern "C" PyObject* PyInt_FromLong(long n) noexcept {
    return boxInt(n);
}

/* Convert an integer to a decimal string.  On many platforms, this
   will be significantly faster than the general arbitrary-base
   conversion machinery in _PyInt_Format, thanks to optimization
   opportunities offered by division by a compile-time constant. */
static Box* int_to_decimal_string(BoxedInt* v) noexcept {
    char buf[sizeof(long) * CHAR_BIT / 3 + 6], *p, *bufend;
    long n = v->n;
    unsigned long absn;
    p = bufend = buf + sizeof(buf);
    absn = n < 0 ? 0UL - n : n;
    do {
        *--p = '0' + (char)(absn % 10);
        absn /= 10;
    } while (absn);
    if (n < 0)
        *--p = '-';
    return PyString_FromStringAndSize(p, bufend - p);
}

extern "C" PyAPI_FUNC(PyObject*) _PyInt_Format(PyIntObject* v, int base, int newstyle) noexcept {
    BoxedInt* bint = reinterpret_cast<BoxedInt*>(v);
    RELEASE_ASSERT(PyInt_Check(bint), "");

    /* There are no doubt many, many ways to optimize this, using code
       similar to _PyLong_Format */
    long n = bint->n;
    int negative = n < 0;
    int is_zero = n == 0;

    /* For the reasoning behind this size, see
       http://c-faq.com/misc/hexio.html. Then, add a few bytes for
       the possible sign and prefix "0[box]" */
    char buf[sizeof(n) * CHAR_BIT + 6];

    /* Start by pointing to the end of the buffer.  We fill in from
       the back forward. */
    char* p = &buf[sizeof(buf)];

    assert(base >= 2 && base <= 36);

    /* Special case base 10, for speed */
    if (base == 10)
        return int_to_decimal_string(bint);

    do {
        /* I'd use i_divmod, except it doesn't produce the results
           I want when n is negative.  So just duplicate the salient
           part here. */
        long div = n / base;
        long mod = n - div * base;

        /* convert abs(mod) to the right character in [0-9, a-z] */
        char cdigit = (char)(mod < 0 ? -mod : mod);
        cdigit += (cdigit < 10) ? '0' : 'a' - 10;
        *--p = cdigit;

        n = div;
    } while (n);

    if (base == 2) {
        *--p = 'b';
        *--p = '0';
    } else if (base == 8) {
        if (newstyle) {
            *--p = 'o';
            *--p = '0';
        } else if (!is_zero)
            *--p = '0';
    } else if (base == 16) {
        *--p = 'x';
        *--p = '0';
    } else {
        *--p = '#';
        *--p = '0' + base % 10;
        if (base > 10)
            *--p = '0' + base / 10;
    }
    if (negative)
        *--p = '-';

    return PyString_FromStringAndSize(p, &buf[sizeof(buf)] - p);
}

extern "C" int _PyInt_AsInt(PyObject* obj) noexcept {
    long result = PyInt_AsLong(obj);
    if (result == -1 && PyErr_Occurred())
        return -1;
    if (result > INT_MAX || result < INT_MIN) {
        PyErr_SetString(PyExc_OverflowError, "Python int too large to convert to C int");
        return -1;
    }
    return (int)result;
}

#ifdef HAVE_LONG_LONG
extern "C" unsigned PY_LONG_LONG PyInt_AsUnsignedLongLongMask(register PyObject* op) noexcept {
    Py_FatalError("unimplemented");

    unsigned PY_LONG_LONG val = 0;

    return val;
}
#endif

extern "C" PyObject* PyInt_FromString(const char* s, char** pend, int base) noexcept {
    char* end;
    long x;
    Py_ssize_t slen;
    PyObject* sobj, *srepr;

    if ((base != 0 && base < 2) || base > 36) {
        PyErr_SetString(PyExc_ValueError, "int() base must be >= 2 and <= 36");
        return NULL;
    }

    while (*s && isspace(Py_CHARMASK(*s)))
        s++;
    errno = 0;
    if (base == 0 && s[0] == '0') {
        x = (long)PyOS_strtoul(const_cast<char*>(s), &end, base);
        if (x < 0)
            return PyLong_FromString(s, pend, base);
    } else
        x = PyOS_strtol(const_cast<char*>(s), &end, base);
    if (end == s || !isalnum(Py_CHARMASK(end[-1])))
        goto bad;
    while (*end && isspace(Py_CHARMASK(*end)))
        end++;
    if (*end != '\0') {
    bad:
        slen = strlen(s) < 200 ? strlen(s) : 200;

        // Pyston addition: try to avoid doing the string repr if possible.
        bool use_fast = true;
        for (int i = 0; i < slen; i++) {
            char c = s[i];
            if (c == '\'' || c == '\\' || c == '\t' || c == '\n' || c == '\r' || c < ' ' || c >= 0x7f) {
                use_fast = false;
                break;
            }
        }
        if (use_fast) {
            PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: '%.200s'", base, s);
            return NULL;
        }

        sobj = PyString_FromStringAndSize(s, slen);
        if (sobj == NULL)
            return NULL;
        srepr = PyObject_Repr(sobj);
        Py_DECREF(sobj);
        if (srepr == NULL)
            return NULL;
        PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: %s", base, PyString_AS_STRING(srepr));
        Py_DECREF(srepr);
        return NULL;
    } else if (errno != 0)
        return PyLong_FromString(s, pend, base);
    if (pend)
        *pend = end;
    return PyInt_FromLong(x);
}

#ifdef Py_USING_UNICODE
extern "C" PyObject* PyInt_FromUnicode(Py_UNICODE* s, Py_ssize_t length, int base) noexcept {
    PyObject* result;
    char* buffer = (char*)malloc(length + 1);

    if (buffer == NULL)
        return PyErr_NoMemory();

    if (PyUnicode_EncodeDecimal(s, length, buffer, NULL)) {
        free(buffer);
        return NULL;
    }
    result = PyInt_FromString(buffer, NULL, base);
    free(buffer);
    return result;
}
#endif

BoxedInt* interned_ints[NUM_INTERNED_INTS];

// If we don't have fast overflow-checking builtins, provide some slow variants:
#if !__has_builtin(__builtin_saddl_overflow)

#ifdef __clang__
#error "shouldn't be defining the slow versions of these for clang"
#endif

bool __builtin_saddl_overflow(i64 lhs, i64 rhs, i64* result) {
    __int128 r = (__int128)lhs + (__int128)rhs;
    if (r > (__int128)PYSTON_INT_MAX)
        return true;
    if (r < (__int128)PYSTON_INT_MIN)
        return true;
    *result = (i64)r;
    return false;
}
bool __builtin_ssubl_overflow(i64 lhs, i64 rhs, i64* result) {
    __int128 r = (__int128)lhs - (__int128)rhs;
    if (r > (__int128)PYSTON_INT_MAX)
        return true;
    if (r < (__int128)PYSTON_INT_MIN)
        return true;
    *result = (i64)r;
    return false;
}
bool __builtin_smull_overflow(i64 lhs, i64 rhs, i64* result) {
    __int128 r = (__int128)lhs * (__int128)rhs;
    if (r > (__int128)PYSTON_INT_MAX)
        return true;
    if (r < (__int128)PYSTON_INT_MIN)
        return true;
    *result = (i64)r;
    return false;
}

#endif

// Could add this to the others, but the inliner should be smart enough
// that this isn't needed:
extern "C" Box* add_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_saddl_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longAdd(boxLong(lhs), boxLong(rhs));
}

extern "C" Box* sub_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_ssubl_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longSub(boxLong(lhs), boxLong(rhs));
}

extern "C" Box* div_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        raiseExcHelper(ZeroDivisionError, "integer division or modulo by zero");
    }

// It's possible for division to overflow:
#if PYSTON_INT_MIN < -PYSTON_INT_MAX
    static_assert(PYSTON_INT_MIN == -PYSTON_INT_MAX - 1, "");

    if (lhs == PYSTON_INT_MIN && rhs == -1) {
        return longDiv(boxLong(lhs), boxLong(rhs));
    }
#endif

    i64 div_result, mod_result;
    div_result = lhs / rhs;
    /* div_result * rhs can overflow on platforms where lhs/rhs gives floor(lhs/rhs)
     * for lhs and rhs with differing signs. (This is unusual
     * behaviour, and C99 prohibits it, but it's allowed by C89;
     * for an example of overflow, take lhs = LONG_MIN, rhs = 5 or lhs =
     * LONG_MAX, rhs = -5.)  However, lhs - div_result*rhs is always
     * representable as a long, since it lies strictly between
     * -abs(rhs) and abs(rhs).  We add casts to avoid intermediate
     * overflow.
     */
    mod_result = (i64)(lhs - (unsigned long)div_result * rhs);
    /* If the signs of lhs and rhs differ, and the remainder is non-0,
     * C89 doesn't define whether div_result is now the floor or the
     * ceiling of the infinitely precise quotient.  We want the floor,
     * and we have it iff the remainder's sign matches rhs's.
     */
    if (mod_result && ((rhs ^ mod_result) < 0) /* i.e. and signs differ */) {
        mod_result += rhs;
        --div_result;
        assert(mod_result && ((rhs ^ mod_result) >= 0));
    }

    return boxInt(div_result);
}

extern "C" i64 mod_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        raiseExcHelper(ZeroDivisionError, "integer division or modulo by zero");
    }
    // I don't think this can overflow:
    if (lhs < 0 && rhs > 0)
        return ((lhs + 1) % rhs) + (rhs - 1);
    if (lhs > 0 && rhs < 0)
        return ((lhs - 1) % rhs) + (rhs + 1);
    return lhs % rhs;
}

extern "C" Box* pow_i64_i64(i64 lhs, i64 rhs, Box* mod) {
    i64 orig_rhs = rhs;
    i64 rtn = 1, curpow = lhs;

    if (rhs < 0)
        // already checked, rhs is a integer,
        // and mod will be None in this case.
        return boxFloat(pow_float_float(lhs, rhs));

    // let longPow do the checks.
    return longPow(boxLong(lhs), boxLong(rhs), mod);
}

extern "C" Box* mul_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_smull_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longMul(boxLong(lhs), boxLong(rhs));
}

extern "C" i1 eq_i64_i64(i64 lhs, i64 rhs) {
    return lhs == rhs;
}

extern "C" i1 ne_i64_i64(i64 lhs, i64 rhs) {
    return lhs != rhs;
}

extern "C" i1 lt_i64_i64(i64 lhs, i64 rhs) {
    return lhs < rhs;
}

extern "C" i1 le_i64_i64(i64 lhs, i64 rhs) {
    return lhs <= rhs;
}

extern "C" i1 gt_i64_i64(i64 lhs, i64 rhs) {
    return lhs > rhs;
}

extern "C" i1 ge_i64_i64(i64 lhs, i64 rhs) {
    return lhs >= rhs;
}


extern "C" Box* intAddInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return add_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intAddFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n + rhs->d);
}

extern "C" Box* intAdd(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return add_i64_i64(lhs->n, rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n + rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intAndInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(lhs->n & rhs->n);
}

extern "C" Box* intAnd(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__and__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (!PyInt_Check(rhs)) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n & rhs_int->n);
}

extern "C" Box* intOrInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(lhs->n | rhs->n);
}

extern "C" Box* intOr(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__or__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (!PyInt_Check(rhs)) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n | rhs_int->n);
}

extern "C" Box* intXorInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(lhs->n ^ rhs->n);
}

extern "C" Box* intXor(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__xor__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (!PyInt_Check(rhs)) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n ^ rhs_int->n);
}

extern "C" Box* intDivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return div_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intDivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "float divide by zero");
    }
    return boxFloat(lhs->n / rhs->d);
}

extern "C" Box* intDiv(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__div__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        return intDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return intDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intFloordivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return div_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intFloordivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "float divide by zero");
    }
    return boxFloat(floor(lhs->n / rhs->d));
}

extern "C" Box* intFloordiv(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__floordiv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        return intFloordivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return intFloordivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intTruedivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));

    if (rhs->n == 0) {
        raiseExcHelper(ZeroDivisionError, "division by zero");
    }
    return boxFloat(lhs->n / (double)rhs->n);
}

extern "C" Box* intTruedivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "division by zero");
    }
    return boxFloat(lhs->n / rhs->d);
}

extern "C" Box* intTruediv(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__truediv__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        return intTruedivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return intTruedivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intLShiftInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));

    if (rhs->n < 0)
        raiseExcHelper(ValueError, "negative shift count");

    bool undefined = rhs->n >= sizeof(rhs->n) * 8;
    if (!undefined) {
        int64_t res = lhs->n << rhs->n;
        if ((res >> rhs->n) == lhs->n)
            return boxInt(lhs->n << rhs->n);
    }
    return longLshift(boxLong(lhs->n), rhs);
}

extern "C" Box* intLShift(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__lshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));

    if (rhs->cls == long_cls)
        return longLshift(boxLong(lhs->n), rhs);

    if (!PyInt_Check(rhs)) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return intLShiftInt(lhs, rhs_int);
}

extern "C" Box* intModInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return boxInt(mod_i64_i64(lhs->n, rhs->n));
}

extern "C" Box* intMod(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__mod__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (!PyInt_Check(rhs)) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(mod_i64_i64(lhs->n, rhs_int->n));
}

extern "C" Box* intDivmod(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__divmod__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));

    Box* divResult = intDiv(lhs, rhs);

    if (divResult == NotImplemented) {
        return NotImplemented;
    }

    Box* modResult = intMod(lhs, rhs);

    if (modResult == NotImplemented) {
        return NotImplemented;
    }

    Box* arg[2] = { divResult, modResult };
    return createTuple(2, arg);
}


extern "C" Box* intMulInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return mul_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intMulFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n * rhs->d);
}

extern "C" Box* intMul(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__mul__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return intMulInt(lhs, rhs_int);
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return intMulFloat(lhs, rhs_float);
    } else {
        return NotImplemented;
    }
}

static void _addFuncPow(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* int_func) {
    std::vector<ConcreteCompilerType*> v_ifu{ BOXED_INT, BOXED_FLOAT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_uuu{ UNKNOWN, UNKNOWN, UNKNOWN };

    CLFunction* cl = createRTFunction(3, false, false);
    addRTFunction(cl, float_func, UNKNOWN, v_ifu);
    addRTFunction(cl, int_func, UNKNOWN, v_uuu);
    int_cls->giveAttr(name, new BoxedFunction(cl, { None }));
}

extern "C" Box* intPowLong(BoxedInt* lhs, BoxedLong* rhs, Box* mod) {
    assert(PyInt_Check(lhs));
    assert(PyLong_Check(rhs));
    BoxedLong* lhs_long = boxLong(lhs->n);
    return longPow(lhs_long, rhs, mod);
}

extern "C" Box* intPowFloat(BoxedInt* lhs, BoxedFloat* rhs, Box* mod) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);

    if (mod != None) {
        raiseExcHelper(TypeError, "pow() 3rd argument not allowed unless all arguments are integers");
    }
    return boxFloat(pow_float_float(lhs->n, rhs->d));
}

extern "C" Box* intPow(BoxedInt* lhs, Box* rhs, Box* mod) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__pow__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (PyLong_Check(rhs))
        return intPowLong(lhs, static_cast<BoxedLong*>(rhs), mod);
    else if (PyFloat_Check(rhs))
        return intPowFloat(lhs, static_cast<BoxedFloat*>(rhs), mod);
    else if (!PyInt_Check(rhs))
        return NotImplemented;

    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    BoxedInt* mod_int = static_cast<BoxedInt*>(mod);

    if (mod != None) {
        if (rhs_int->n < 0)
            raiseExcHelper(TypeError, "pow() 2nd argument "
                                      "cannot be negative when 3rd argument specified");
        if (!PyInt_Check(mod)) {
            return NotImplemented;
        } else if (mod_int->n == 0) {
            raiseExcHelper(ValueError, "pow() 3rd argument cannot be 0");
        }
    }

    Box* rtn = pow_i64_i64(lhs->n, rhs_int->n, mod);
    if (PyLong_Check(rtn))
        return longInt(rtn);
    return rtn;
}

extern "C" Box* intRShiftInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));

    if (rhs->n < 0)
        raiseExcHelper(ValueError, "negative shift count");

    return boxInt(lhs->n >> rhs->n);
}

extern "C" Box* intRShift(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__rshift__' requires a 'int' object but received a '%s'",
                       getTypeName(lhs));

    if (rhs->cls == long_cls)
        return longRshift(boxLong(lhs->n), rhs);

    if (!PyInt_Check(rhs)) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return intRShiftInt(lhs, rhs_int);
}

extern "C" Box* intSubInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return sub_i64_i64(lhs->n, rhs->n);
}

extern "C" Box* intSubFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n - rhs->d);
}

extern "C" Box* intSub(BoxedInt* lhs, Box* rhs) {
    if (!PyInt_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__sub__' requires a 'int' object but received a '%s'", getTypeName(lhs));

    if (PyInt_Check(rhs)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return intSubInt(lhs, rhs_int);
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return intSubFloat(lhs, rhs_float);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intInvert(BoxedInt* v) {
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__invert__' requires a 'int' object but received a '%s'",
                       getTypeName(v));

    return boxInt(~v->n);
}

extern "C" Box* intPos(BoxedInt* v) {
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__pos__' requires a 'int' object but received a '%s'", getTypeName(v));

    if (v->cls == int_cls)
        return v;
    return boxInt(v->n);
}

extern "C" Box* intNeg(BoxedInt* v) {
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__neg__' requires a 'int' object but received a '%s'", getTypeName(v));


// It's possible for this to overflow:
#if PYSTON_INT_MIN < -PYSTON_INT_MAX
    static_assert(PYSTON_INT_MIN == -PYSTON_INT_MAX - 1, "");

    if (v->n == PYSTON_INT_MIN) {
        return longNeg(boxLong(v->n));
    }
#endif

    return boxInt(-v->n);
}

extern "C" Box* intNonzero(BoxedInt* v) {
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__nonzero__' requires a 'int' object but received a '%s'",
                       getTypeName(v));

    return boxBool(v->n != 0);
}

extern "C" BoxedString* intRepr(BoxedInt* v) {
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'int' object but received a '%s'", getTypeName(v));

    char buf[80];
    int len = snprintf(buf, 80, "%ld", v->n);
    return static_cast<BoxedString*>(boxString(llvm::StringRef(buf, len)));
}

extern "C" Box* intHash(BoxedInt* self) {
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__hash__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    if (self->cls == int_cls)
        return self;
    return boxInt(self->n);
}

extern "C" Box* intBin(BoxedInt* self) {
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__bin__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    return _PyInt_Format(reinterpret_cast<PyIntObject*>(self), 2, 0);
}

extern "C" Box* intHex(BoxedInt* self) {
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__hex__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    char buf[80];
    int len = 0;
    bool is_negative = self->n < 0;
    if (is_negative)
        len = snprintf(buf, sizeof(buf), "-0x%lx", std::abs(self->n));
    else
        len = snprintf(buf, sizeof(buf), "0x%lx", self->n);
    return boxString(llvm::StringRef(buf, len));
}

extern "C" Box* intOct(BoxedInt* self) {
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__oct__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    char buf[80];
    int len = 0;
    bool is_negative = self->n < 0;
    if (is_negative)
        len = snprintf(buf, sizeof(buf), "-%#lo", std::abs(self->n));
    else
        len = snprintf(buf, sizeof(buf), "%#lo", self->n);
    return boxString(llvm::StringRef(buf, len));
}

extern "C" Box* intTrunc(BoxedInt* self) {
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__trunc__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    if (self->cls == int_cls)
        return self;
    return boxInt(self->n);
}

extern "C" Box* intInt(BoxedInt* self) {
    if (!PyInt_Check(self))
        raiseExcHelper(TypeError, "descriptor '__int__' requires a 'int' object but received a '%s'",
                       getTypeName(self));

    if (self->cls == int_cls)
        return self;
    return boxInt(self->n);
}

extern "C" Box* intIndex(BoxedInt* v) {
    if (PyInt_CheckExact(v))
        return v;
    return boxInt(v->n);
}

template <ExceptionStyle S> static Box* _intNew(Box* val, Box* base) noexcept(S == CAPI) {
    if (val->cls == int_cls) {
        if (base) {
            if (S == CAPI) {
                PyErr_Format(TypeError, "int() missing string argument");
                return NULL;
            } else
                raiseExcHelper(TypeError, "int() missing string argument");
        }
        BoxedInt* n = static_cast<BoxedInt*>(val);
        if (val->cls == int_cls)
            return n;
        return new BoxedInt(n->n);
    }
    if (!base) {
        Box* r = PyNumber_Int(val);
        if (!r) {
            if (S == CAPI) {
                return NULL;
            } else
                throwCAPIException();
        }
        return r;
    }

    if (PyString_Check(val)) {
        int base_n;
        if (!base)
            base_n = 10;
        else {
            RELEASE_ASSERT(base->cls == int_cls, "");
            base_n = static_cast<BoxedInt*>(base)->n;
        }
        BoxedString* s = static_cast<BoxedString*>(val);

        if (s->size() != strlen(s->data())) {
            Box* srepr = PyObject_Repr(val);
            if (S == CAPI) {
                PyErr_Format(PyExc_ValueError, "invalid literal for int() with base %d: %s", base_n,
                             PyString_AS_STRING(srepr));
                return NULL;
            } else {
                raiseExcHelper(ValueError, "invalid literal for int() with base %d: %s", base_n,
                               PyString_AS_STRING(srepr));
            }
        }
        Box* r = PyInt_FromString(s->data(), NULL, base_n);
        if (!r) {
            if (S == CAPI)
                return NULL;
            else
                throwCAPIException();
        }
        return r;
    } else if (isSubclass(val->cls, unicode_cls)) {
        int base_n;
        if (!base)
            base_n = 10;
        else {
            RELEASE_ASSERT(base->cls == int_cls, "");
            base_n = static_cast<BoxedInt*>(base)->n;
        }

        Box* r = PyInt_FromUnicode(PyUnicode_AS_UNICODE(val), PyUnicode_GET_SIZE(val), base_n);
        if (!r) {
            if (S == CAPI)
                return NULL;
            else
                throwCAPIException();
        }
        return r;
    } else if (val->cls == float_cls) {
        RELEASE_ASSERT(!base, "");

        // This is tricky -- code copied from CPython:

        double x = PyFloat_AsDouble(val);
        double wholepart; /* integral portion of x, rounded toward 0 */

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
    } else {
        if (base)
            raiseExcHelper(TypeError, "TypeError: int() can't convert non-string with explicit base");
        static BoxedString* int_str = internStringImmortal("__int__");
        Box* r = callattrInternal<S>(val, int_str, CLASS_ONLY, NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);

        if (!r) {
            if (S == CAPI) {
                if (!PyErr_Occurred())
                    PyErr_Format(TypeError, "int() argument must be a string or a number, not '%s'\n",
                                 getTypeName(val));
                return NULL;
            } else {
                raiseExcHelper(TypeError, "int() argument must be a string or a number, not '%s'\n", getTypeName(val));
            }
        }

        if (!PyInt_Check(r) && !PyLong_Check(r)) {
            if (S == CAPI) {
                PyErr_Format(TypeError, "__int__ returned non-int (type %s)", r->cls->tp_name);
                return NULL;
            } else
                raiseExcHelper(TypeError, "__int__ returned non-int (type %s)", r->cls->tp_name);
        }
        return r;
    }
}

template <ExceptionStyle S> Box* intNew(Box* _cls, Box* val, Box* base) noexcept(S == CAPI) {
    if (!PyType_Check(_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "int.__new__(X): X is not a type object (%s)", getTypeName(_cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "int.__new__(X): X is not a type object (%s)", getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, int_cls)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "int.__new__(%s): %s is not a subtype of int", getNameOfClass(cls),
                         getNameOfClass(cls));
            return NULL;
        } else
            raiseExcHelper(TypeError, "int.__new__(%s): %s is not a subtype of int", getNameOfClass(cls),
                           getNameOfClass(cls));
    }

    if (cls == int_cls)
        return _intNew<S>(val, base);

    BoxedInt* n = (BoxedInt*)_intNew<S>(val, base);
    if (!n) {
        assert(S == CAPI);
        return NULL;
    }

    if (n->cls == long_cls) {
        if (cls == int_cls)
            return n;

        if (S == CAPI) {
            PyErr_SetString(OverflowError, "Python int too large to convert to C long");
            return NULL;
        } else
            raiseExcHelper(OverflowError, "Python int too large to convert to C long");
    }
    return new (cls) BoxedInt(n->n);
}

static const unsigned char BitLengthTable[32]
    = { 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5 };

static int bits_in_ulong(unsigned long d) noexcept {
    int d_bits = 0;
    while (d >= 32) {
        d_bits += 6;
        d >>= 6;
    }
    d_bits += (int)BitLengthTable[d];
    return d_bits;
}

extern "C" Box* intBitLength(BoxedInt* v) {
    if (!PyInt_Check(v))
        raiseExcHelper(TypeError, "descriptor 'bit_length' requires a 'int' object but received a '%s'",
                       getTypeName(v));

    unsigned long n;
    if (v->n < 0)
        /* avoid undefined behaviour when v->n == -LONG_MAX-1 */
        n = 0U - (unsigned long)v->n;
    else
        n = (unsigned long)v->n;

    return PyInt_FromLong(bits_in_ulong(n));
}

static void _addFuncIntFloatUnknown(const char* name, void* int_func, void* float_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_if, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(UNKNOWN);
    v_ii.push_back(BOXED_INT);
    v_if.push_back(UNKNOWN);
    v_if.push_back(BOXED_FLOAT);
    // Only the unknown version can accept non-ints (ex if you access the function directly ex via int.__add__)
    v_iu.push_back(UNKNOWN);
    v_iu.push_back(UNKNOWN);

    CLFunction* cl = createRTFunction(2, false, false);
    addRTFunction(cl, int_func, UNKNOWN, v_ii);
    addRTFunction(cl, float_func, BOXED_FLOAT, v_if);
    addRTFunction(cl, boxed_func, UNKNOWN, v_iu);
    int_cls->giveAttr(name, new BoxedFunction(cl));
}

static void _addFuncIntUnknown(const char* name, ConcreteCompilerType* rtn_type, void* int_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(UNKNOWN);
    v_ii.push_back(BOXED_INT);
    v_iu.push_back(UNKNOWN);
    v_iu.push_back(UNKNOWN);

    CLFunction* cl = createRTFunction(2, false, false);
    addRTFunction(cl, int_func, rtn_type, v_ii);
    addRTFunction(cl, boxed_func, UNKNOWN, v_iu);
    int_cls->giveAttr(name, new BoxedFunction(cl));
}

static Box* intIntGetset(Box* b, void*) {
    if (b->cls == int_cls) {
        return b;
    } else {
        assert(PyInt_Check(b));
        return boxInt(static_cast<BoxedInt*>(b)->n);
    }
}

static Box* int0(Box*, void*) {
    return boxInt(0);
}

static Box* int1(Box*, void*) {
    return boxInt(1);
}

static int64_t int_hash(BoxedInt* o) noexcept {
    int64_t n = o->n;
    if (n == -1)
        return -2;
    return n;
}

static PyObject* int_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    if (!PyInt_Check(v) || !PyInt_Check(w)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    int64_t lhs = static_cast<BoxedInt*>(v)->n;
    int64_t rhs = static_cast<BoxedInt*>(w)->n;

    switch (op) {
        case Py_EQ:
            return boxBool(lhs == rhs);
        case Py_NE:
            return boxBool(lhs != rhs);
        case Py_LT:
            return boxBool(lhs < rhs);
        case Py_LE:
            return boxBool(lhs <= rhs);
        case Py_GT:
            return boxBool(lhs > rhs);
        case Py_GE:
            return boxBool(lhs >= rhs);
        default:
            RELEASE_ASSERT(0, "%d", op);
    }
}

void setupInt() {
    for (int i = 0; i < NUM_INTERNED_INTS; i++) {
        interned_ints[i] = new BoxedInt(i);
        gc::registerPermanentRoot(interned_ints[i]);
    }

    _addFuncIntFloatUnknown("__add__", (void*)intAddInt, (void*)intAddFloat, (void*)intAdd);
    _addFuncIntUnknown("__and__", BOXED_INT, (void*)intAndInt, (void*)intAnd);
    _addFuncIntUnknown("__or__", BOXED_INT, (void*)intOrInt, (void*)intOr);
    _addFuncIntUnknown("__xor__", BOXED_INT, (void*)intXorInt, (void*)intXor);
    _addFuncIntFloatUnknown("__sub__", (void*)intSubInt, (void*)intSubFloat, (void*)intSub);
    _addFuncIntFloatUnknown("__div__", (void*)intDivInt, (void*)intDivFloat, (void*)intDiv);
    _addFuncIntFloatUnknown("__floordiv__", (void*)intFloordivInt, (void*)intFloordivFloat, (void*)intFloordiv);
    _addFuncIntFloatUnknown("__truediv__", (void*)intTruedivInt, (void*)intTruedivFloat, (void*)intTruediv);
    _addFuncIntFloatUnknown("__mul__", (void*)intMulInt, (void*)intMulFloat, (void*)intMul);
    _addFuncIntUnknown("__mod__", BOXED_INT, (void*)intModInt, (void*)intMod);
    _addFuncPow("__pow__", BOXED_INT, (void*)intPowFloat, (void*)intPow);
    // Note: CPython implements int comparisons using tp_compare
    int_cls->tp_richcompare = int_richcompare;

    _addFuncIntUnknown("__lshift__", UNKNOWN, (void*)intLShiftInt, (void*)intLShift);
    _addFuncIntUnknown("__rshift__", UNKNOWN, (void*)intRShiftInt, (void*)intRShift);

    int_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)intInvert, BOXED_INT, 1)));
    int_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)intPos, BOXED_INT, 1)));
    int_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)intNeg, UNKNOWN, 1)));
    int_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)intNonzero, BOXED_BOOL, 1)));
    int_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)intRepr, STR, 1)));
    int_cls->tp_hash = (hashfunc)int_hash;
    int_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)intDivmod, UNKNOWN, 2)));

    int_cls->giveAttr("__bin__", new BoxedFunction(boxRTFunction((void*)intBin, STR, 1)));
    int_cls->giveAttr("__hex__", new BoxedFunction(boxRTFunction((void*)intHex, STR, 1)));
    int_cls->giveAttr("__oct__", new BoxedFunction(boxRTFunction((void*)intOct, STR, 1)));

    int_cls->giveAttr("__trunc__", new BoxedFunction(boxRTFunction((void*)intTrunc, BOXED_INT, 1)));
    int_cls->giveAttr("__index__", new BoxedFunction(boxRTFunction((void*)intIndex, BOXED_INT, 1)));
    int_cls->giveAttr("__int__", new BoxedFunction(boxRTFunction((void*)intInt, BOXED_INT, 1)));

    auto int_new
        = boxRTFunction((void*)intNew<CXX>, UNKNOWN, 3, false, false, ParamNames({ "", "x", "base" }, "", ""), CXX);
    addRTFunction(int_new, (void*)intNew<CAPI>, UNKNOWN, CAPI);
    int_cls->giveAttr("__new__", new BoxedFunction(int_new, { boxInt(0), NULL }));

    int_cls->giveAttr("bit_length", new BoxedFunction(boxRTFunction((void*)intBitLength, BOXED_INT, 1)));

    int_cls->giveAttr("real", new (pyston_getset_cls) BoxedGetsetDescriptor(intIntGetset, NULL, NULL));
    int_cls->giveAttr("imag", new (pyston_getset_cls) BoxedGetsetDescriptor(int0, NULL, NULL));
    int_cls->giveAttr("conjugate", new BoxedFunction(boxRTFunction((void*)intIntGetset, BOXED_INT, 1)));
    int_cls->giveAttr("numerator", new (pyston_getset_cls) BoxedGetsetDescriptor(intIntGetset, NULL, NULL));
    int_cls->giveAttr("denominator", new (pyston_getset_cls) BoxedGetsetDescriptor(int1, NULL, NULL));

    add_operators(int_cls);
    int_cls->freeze();

    int_cls->tp_repr = (reprfunc)int_to_decimal_string;
}

void teardownInt() {
}
}
