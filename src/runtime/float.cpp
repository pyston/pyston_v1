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

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <gmp.h>

#include "capi/types.h"
#include "core/types.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

extern "C" PyObject* float_hex(PyObject* v) noexcept;
extern "C" PyObject* float_fromhex(PyObject* cls, PyObject* arg) noexcept;
extern "C" PyObject* float_as_integer_ratio(PyObject* v, PyObject* unused) noexcept;
extern "C" PyObject* float_is_integer(PyObject* v) noexcept;
extern "C" PyObject* float__format__(PyObject* v) noexcept;
extern "C" PyObject* float_pow(PyObject* v, PyObject* w, PyObject* z) noexcept;
extern "C" int float_pow_unboxed(double iv, double iw, double* res) noexcept;

namespace pyston {

extern "C" PyObject* PyFloat_FromDouble(double d) noexcept {
    return boxFloat(d);
}

extern "C" PyObject* PyFloat_FromString(PyObject* v, char** pend) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" double PyFloat_AsDouble(PyObject* o) noexcept {
    assert(o);

    if (PyFloat_CheckExact(o))
        return static_cast<BoxedFloat*>(o)->d;

    // special case: int (avoids all the boxing below, and int_cls->tp_as_number
    // isn't implemented at the time of this writing)
    // This is an exact check.
    if (o->cls == int_cls || o->cls == bool_cls)
        return (double)static_cast<BoxedInt*>(o)->n;
    // special case: long
    if (o->cls == long_cls)
        return mpz_get_d(static_cast<BoxedLong*>(o)->n);

    // implementation from cpython:
    PyNumberMethods* nb;
    BoxedFloat* fo;
    double val;

    if ((nb = Py_TYPE(o)->tp_as_number) == NULL || nb->nb_float == NULL) {
        PyErr_SetString(PyExc_TypeError, "a float is required");
        return -1;
    };
    fo = (BoxedFloat*)(*nb->nb_float)(o);
    if (fo == NULL)
        return -1;
    if (!PyFloat_Check(fo)) {
        PyErr_SetString(PyExc_TypeError, "nb_float should return float object");
        return -1;
    }

    return static_cast<BoxedFloat*>(fo)->d;
}

template <typename T> static inline void raiseDivZeroExcIfZero(T var) {
    if (var == 0) {
        raiseExcHelper(ZeroDivisionError, "float division by zero");
    }
}

extern "C" double mod_float_float(double lhs, double rhs) {
    raiseDivZeroExcIfZero(rhs);
    double r = fmod(lhs, rhs);
    // Have to be careful here with signed zeroes:
    if (std::signbit(r) != std::signbit(rhs)) {
        if (r == 0)
            r *= -1;
        else
            r += rhs;
    }
    return r;
}

extern "C" double div_float_float(double lhs, double rhs) {
    raiseDivZeroExcIfZero(rhs);
    return lhs / rhs;
}

extern "C" double floordiv_float_float(double lhs, double rhs) {
    raiseDivZeroExcIfZero(rhs);
    return floor(lhs / rhs);
}

extern "C" Box* floatAddFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->d + rhs->d);
}

extern "C" Box* floatAddInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(lhs->d + rhs->n);
}

extern "C" Box* floatAdd(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatAddInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatAddFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(lhs->d + PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatDivFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    raiseDivZeroExcIfZero(rhs->d);
    return boxFloat(lhs->d / rhs->d);
}

extern "C" Box* floatDivInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    raiseDivZeroExcIfZero(rhs->n);
    return boxFloat(lhs->d / rhs->n);
}

extern "C" Box* floatDiv(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(lhs->d / PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatTruediv(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(lhs->d / PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatRDivFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    raiseDivZeroExcIfZero(lhs->d);
    return boxFloat(rhs->d / lhs->d);
}

extern "C" Box* floatRDivInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    raiseDivZeroExcIfZero(lhs->d);
    return boxFloat(rhs->n / lhs->d);
}

extern "C" Box* floatRDiv(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatRDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatRDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(PyLong_AsDouble(rhs) / lhs->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatFloorDivFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    raiseDivZeroExcIfZero(rhs->d);
    return boxFloat(floor(lhs->d / rhs->d));
}

extern "C" Box* floatFloorDivInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    raiseDivZeroExcIfZero(rhs->n);
    return boxFloat(floor(lhs->d / rhs->n));
}

extern "C" Box* floatFloorDiv(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatFloorDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatFloorDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatEqFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxBool(lhs->d == rhs->d);
}

extern "C" Box* floatEqInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxBool(lhs->d == rhs->n);
}

extern "C" Box* floatEq(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatEqInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatEqFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxBool(lhs->d == PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatNeFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxBool(lhs->d != rhs->d);
}

extern "C" Box* floatNeInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxBool(lhs->d != rhs->n);
}

extern "C" Box* floatNe(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatNeInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatNeFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxBool(lhs->d != PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatLtFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxBool(lhs->d < rhs->d);
}

extern "C" Box* floatLtInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxBool(lhs->d < rhs->n);
}

extern "C" Box* floatLt(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatLtInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatLtFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxBool(lhs->d < PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatLeFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxBool(lhs->d <= rhs->d);
}

extern "C" Box* floatLeInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxBool(lhs->d <= rhs->n);
}

extern "C" Box* floatLe(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatLeInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatLeFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxBool(lhs->d <= PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatGtFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxBool(lhs->d > rhs->d);
}

extern "C" Box* floatGtInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxBool(lhs->d > rhs->n);
}

extern "C" Box* floatGt(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatGtInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatGtFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxBool(lhs->d > PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatGeFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxBool(lhs->d >= rhs->d);
}

extern "C" Box* floatGeInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxBool(lhs->d >= rhs->n);
}

extern "C" Box* floatGe(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatGeInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatGeFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxBool(lhs->d >= PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatModFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(mod_float_float(lhs->d, rhs->d));
}

extern "C" Box* floatModInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(mod_float_float(lhs->d, rhs->n));
}

extern "C" Box* floatMod(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatModInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatModFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(mod_float_float(lhs->d, PyLong_AsDouble(rhs)));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatRModFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(mod_float_float(rhs->d, lhs->d));
}

extern "C" Box* floatRModInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(mod_float_float(rhs->n, lhs->d));
}

extern "C" Box* floatRMod(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatRModInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatRModFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(mod_float_float(PyLong_AsDouble(rhs), lhs->d));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatPow(BoxedFloat* lhs, Box* rhs, Box* mod) {
    Box* res = float_pow(lhs, rhs, mod);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

extern "C" Box* floatPowFloat(BoxedFloat* lhs, BoxedFloat* rhs, Box* mod = None) {
    // TODO to specialize this, need to account for all the special cases in float_pow
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return floatPow(lhs, rhs, mod);
}

extern "C" Box* floatPowInt(BoxedFloat* lhs, BoxedInt* rhs, Box* mod = None) {
    // TODO to specialize this, need to account for all the special cases in float_pow
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return floatPow(lhs, rhs, mod);
}

extern "C" double pow_float_float(double lhs, double rhs) {
    double res;
    int err = float_pow_unboxed(lhs, rhs, &res);
    if (err) {
        throwCAPIException();
    } else {
        return res;
    }
}

extern "C" Box* floatMulFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->d * rhs->d);
}

extern "C" Box* floatMulInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(lhs->d * rhs->n);
}

extern "C" Box* floatMul(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatMulInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatMulFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(lhs->d * PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatSubFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->d - rhs->d);
}

extern "C" Box* floatSubInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(lhs->d - rhs->n);
}

extern "C" Box* floatSub(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatSubInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatSubFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(lhs->d - PyLong_AsDouble(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatRSubFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(rhs->d - lhs->d);
}

extern "C" Box* floatRSubInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(rhs->n - lhs->d);
}

extern "C" Box* floatRSub(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatRSubInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatRSubFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(PyLong_AsDouble(rhs) - lhs->d);
    } else {
        return NotImplemented;
    }
}

Box* floatNeg(BoxedFloat* self) {
    assert(self->cls == float_cls);
    return boxFloat(-self->d);
}

bool floatNonzeroUnboxed(BoxedFloat* self) {
    assert(self->cls == float_cls);
    return self->d != 0.0;
}

Box* floatNonzero(BoxedFloat* self) {
    return boxBool(floatNonzeroUnboxed(self));
}

std::string floatFmt(double x, int precision, char code) {
    char fmt[5] = "%.*g";
    fmt[3] = code;

    if (isnan(x)) {
        return "nan";
    }
    if (isinf(x)) {
        if (x > 0)
            return "inf";
        return "-inf";
    }

    char buf[40];
    int n = snprintf(buf, 40, fmt, precision, x);

    int dot = -1;
    int exp = -1;
    int first = -1;
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '.') {
            dot = i;
        } else if (c == 'e') {
            exp = i;
        } else if (first == -1 && c >= '0' && c <= '9') {
            first = i;
        }
    }

    if (dot == -1 && exp == -1) {
        if (n == precision) {
            memmove(buf + first + 2, buf + first + 1, (n - first - 1));
            buf[first + 1] = '.';
            exp = n + 1;
            int exp_digs = snprintf(buf + n + 1, 5, "e%+.02d", (n - first - 1));
            n += exp_digs + 1;
            dot = 1;
        } else {
            buf[n] = '.';
            buf[n + 1] = '0';
            n += 2;

            return std::string(buf, n);
        }
    }

    if (exp != -1 && dot == -1) {
        return std::string(buf, n);
    }

    assert(dot != -1);

    int start, end;
    if (exp) {
        start = exp - 1;
        end = dot;
    } else {
        start = n - 1;
        end = dot + 2;
    }
    for (int i = start; i >= end; i--) {
        if (buf[i] == '0') {
            memmove(buf + i, buf + i + 1, n - i - 1);
            n--;
        } else if (buf[i] == '.') {
            memmove(buf + i, buf + i + 1, n - i - 1);
            n--;
            break;
        } else {
            break;
        }
    }
    return std::string(buf, n);
}

BoxedFloat* _floatNew(Box* a) {
    // FIXME CPython uses PyUnicode_EncodeDecimal:
    a = coerceUnicodeToStr(a);

    if (a->cls == float_cls) {
        return static_cast<BoxedFloat*>(a);
    } else if (isSubclass(a->cls, float_cls)) {
        return new BoxedFloat(static_cast<BoxedFloat*>(a)->d);
    } else if (isSubclass(a->cls, int_cls)) {
        return new BoxedFloat(static_cast<BoxedInt*>(a)->n);
    } else if (a->cls == str_cls) {
        llvm::StringRef s = static_cast<BoxedString*>(a)->s();
        if (s == "nan")
            return new BoxedFloat(NAN);
        if (s == "-nan")
            return new BoxedFloat(-NAN);
        if (s == "inf")
            return new BoxedFloat(INFINITY);
        if (s == "-inf")
            return new BoxedFloat(-INFINITY);

        // TODO this should just use CPython's implementation:
        char* endptr;
        assert(s.data()[s.size()] == '\0');
        const char* startptr = s.data();
        double r = strtod(startptr, &endptr);
        if (endptr != startptr + s.size())
            raiseExcHelper(ValueError, "could not convert string to float: %s", s.data());
        return new BoxedFloat(r);
    } else {
        static BoxedString* float_str = internStringImmortal("__float__");
        CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
        Box* r = callattr(a, float_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);

        if (!r) {
            fprintf(stderr, "TypeError: float() argument must be a string or a number, not '%s'\n", getTypeName(a));
            raiseExcHelper(TypeError, "");
        }

        if (!isSubclass(r->cls, float_cls)) {
            raiseExcHelper(TypeError, "__float__ returned non-float (type %s)", r->cls->tp_name);
        }
        return static_cast<BoxedFloat*>(r);
    }
}

Box* floatNew(BoxedClass* _cls, Box* a) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "float.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, float_cls))
        raiseExcHelper(TypeError, "float.__new__(%s): %s is not a subtype of float", getNameOfClass(cls),
                       getNameOfClass(cls));


    if (cls == float_cls)
        return _floatNew(a);

    BoxedFloat* f = _floatNew(a);

    return new (cls) BoxedFloat(f->d);
}

Box* floatStr(BoxedFloat* self) {
    if (!isSubclass(self->cls, float_cls))
        raiseExcHelper(TypeError, "descriptor '__str__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    return boxString(floatFmt(self->d, 12, 'g'));
}

Box* floatRepr(BoxedFloat* self) {
    assert(self->cls == float_cls);
    return boxString(floatFmt(self->d, 16, 'g'));
}

Box* floatTrunc(BoxedFloat* self) {
    if (!isSubclass(self->cls, float_cls))
        raiseExcHelper(TypeError, "descriptor '__trunc__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    double wholepart; /* integral portion of x, rounded toward 0 */

    (void)modf(self->d, &wholepart);
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

Box* floatHash(BoxedFloat* self) {
    if (!isSubclass(self->cls, float_cls))
        raiseExcHelper(TypeError, "descriptor '__hash__' requires a 'float' object but received a '%s'",
                       getTypeName(self));

    return boxInt(_Py_HashDouble(self->d));
}

extern "C" void printFloat(double d) {
    std::string s = floatFmt(d, 12, 'g');
    printf("%s", s.c_str());
}

static void _addFunc(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* int_func,
                     void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ff, v_fi, v_fu;
    v_ff.push_back(BOXED_FLOAT);
    v_ff.push_back(BOXED_FLOAT);
    v_fi.push_back(BOXED_FLOAT);
    v_fi.push_back(BOXED_INT);
    v_fu.push_back(BOXED_FLOAT);
    v_fu.push_back(UNKNOWN);

    CLFunction* cl = createRTFunction(2, 0, false, false);
    addRTFunction(cl, float_func, rtn_type, v_ff);
    addRTFunction(cl, int_func, rtn_type, v_fi);
    addRTFunction(cl, boxed_func, UNKNOWN, v_fu);
    float_cls->giveAttr(name, new BoxedFunction(cl));
}

static void _addFuncPow(const char* name, ConcreteCompilerType* rtn_type, void* float_func, void* int_func,
                        void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ffu{ BOXED_FLOAT, BOXED_FLOAT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_fiu{ BOXED_FLOAT, BOXED_INT, UNKNOWN };
    std::vector<ConcreteCompilerType*> v_fuu{ BOXED_FLOAT, UNKNOWN, UNKNOWN };

    CLFunction* cl = createRTFunction(3, 1, false, false);
    addRTFunction(cl, float_func, rtn_type, v_ffu);
    addRTFunction(cl, int_func, rtn_type, v_fiu);
    addRTFunction(cl, boxed_func, UNKNOWN, v_fuu);
    float_cls->giveAttr(name, new BoxedFunction(cl, { None }));
}

static Box* floatFloat(Box* b, void*) {
    if (b->cls == float_cls) {
        return b;
    } else {
        assert(PyFloat_Check(b));
        return boxFloat(static_cast<BoxedFloat*>(b)->d);
    }
}

static Box* float0(Box*, void*) {
    return boxFloat(0.0);
}

// __getformat__
// ported pretty directly from cpython Objects/floatobject.c

typedef enum { unknown_format, ieee_big_endian_format, ieee_little_endian_format } float_format_type;

static float_format_type double_format, float_format;
static float_format_type detected_double_format, detected_float_format;

static void floatFormatInit() {
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

    if (sizeof(double) == 8) {
        double x = 9006104071832581.0;
        if (memcmp(&x, "\x43\x3f\xff\x01\x02\x03\x04\x05", 8) == 0)
            detected_double_format = ieee_big_endian_format;
        else if (memcmp(&x, "\x05\x04\x03\x02\x01\xff\x3f\x43", 8) == 0)
            detected_double_format = ieee_little_endian_format;
        else
            detected_double_format = unknown_format;
    } else {
        detected_double_format = unknown_format;
    }

    if (sizeof(float) == 4) {
        float y = 16711938.0;
        if (memcmp(&y, "\x4b\x7f\x01\x02", 4) == 0)
            detected_float_format = ieee_big_endian_format;
        else if (memcmp(&y, "\x02\x01\x7f\x4b", 4) == 0)
            detected_float_format = ieee_little_endian_format;
        else
            detected_float_format = unknown_format;
    } else {
        detected_float_format = unknown_format;
    }

    double_format = detected_double_format;
    float_format = detected_float_format;
}

// ported pretty directly from cpython
Box* floatGetFormat(BoxedClass* v, Box* arg) {
    char* s;
    float_format_type r;

    if (!PyString_Check(arg)) {
        raiseExcHelper(TypeError, "__getformat__() argument must be string, not %s", arg->cls->tp_name);
    }
    s = PyString_AS_STRING(arg);
    if (strcmp(s, "double") == 0) {
        r = double_format;
    } else if (strcmp(s, "float") == 0) {
        r = float_format;
    } else {
        raiseExcHelper(ValueError, "__getformat__() argument 1 must be "
                                   "'double' or 'float'");
    }

    switch (r) {
        case unknown_format:
            return boxString("unknown");
        case ieee_little_endian_format:
            return boxString("IEEE, little-endian");
        case ieee_big_endian_format:
            return boxString("IEEE, big-endian");
        default:
            RELEASE_ASSERT(false, "insane float_format or double_format");
            return NULL;
    }
}

const char* floatGetFormatDoc = "float.__getformat__(typestr) -> string\n"
                                "\n"
                                "You probably don't want to use this function.  It exists mainly to be\n"
                                "used in Python's test suite.\n"
                                "\n"
                                "typestr must be 'double' or 'float'.  This function returns whichever of\n"
                                "'unknown', 'IEEE, big-endian' or 'IEEE, little-endian' best describes the\n"
                                "format of floating point numbers used by the C type named by typestr.";

static PyObject* float_setformat(PyTypeObject* v, PyObject* args) noexcept {
    char* typestr;
    char* format;
    float_format_type f;
    float_format_type detected;
    float_format_type* p;

    if (!PyArg_ParseTuple(args, "ss:__setformat__", &typestr, &format))
        return NULL;

    if (strcmp(typestr, "double") == 0) {
        p = &double_format;
        detected = detected_double_format;
    } else if (strcmp(typestr, "float") == 0) {
        p = &float_format;
        detected = detected_float_format;
    } else {
        PyErr_SetString(PyExc_ValueError, "__setformat__() argument 1 must "
                                          "be 'double' or 'float'");
        return NULL;
    }

    if (strcmp(format, "unknown") == 0) {
        f = unknown_format;
    } else if (strcmp(format, "IEEE, little-endian") == 0) {
        f = ieee_little_endian_format;
    } else if (strcmp(format, "IEEE, big-endian") == 0) {
        f = ieee_big_endian_format;
    } else {
        PyErr_SetString(PyExc_ValueError, "__setformat__() argument 2 must be "
                                          "'unknown', 'IEEE, little-endian' or "
                                          "'IEEE, big-endian'");
        return NULL;
    }

    if (f != unknown_format && f != detected) {
        PyErr_Format(PyExc_ValueError, "can only set %s format to 'unknown' or the "
                                       "detected platform value",
                     typestr);
        return NULL;
    }

    *p = f;
    Py_RETURN_NONE;
}

/*----------------------------------------------------------------------------
 * _PyFloat_{Pack,Unpack}{4,8}.  See floatobject.h.
 */
extern "C" int _PyFloat_Pack4(double x, unsigned char* p, int le) noexcept {
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
        } else
            sign = 0;

        f = frexp(x, &e);

        /* Normalize f to be in the range [1.0, 2.0) */
        if (0.5 <= f && f < 1.0) {
            f *= 2.0;
            e--;
        } else if (f == 0.0)
            e = 0;
        else {
            PyErr_SetString(PyExc_SystemError, "frexp() result out of range");
            return -1;
        }

        if (e >= 128)
            goto Overflow;
        else if (e < -126) {
            /* Gradual underflow */
            f = ldexp(f, 126 + e);
            e = 0;
        } else if (!(e == 0 && f == 0.0)) {
            e += 127;
            f -= 1.0; /* Get rid of leading 1 */
        }

        f *= 8388608.0;                  /* 2**23 */
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
        *p = (char)(((e & 1) << 7) | (fbits >> 16));
        p += incr;

        /* Third byte */
        *p = (fbits >> 8) & 0xFF;
        p += incr;

        /* Fourth byte */
        *p = fbits & 0xFF;

        /* Done */
        return 0;

    } else {
        float y = (float)x;
        const char* s = (char*)&y;
        int i, incr = 1;

        if (Py_IS_INFINITY(y) && !Py_IS_INFINITY(x))
            goto Overflow;

        if ((float_format == ieee_little_endian_format && !le) || (float_format == ieee_big_endian_format && le)) {
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
    PyErr_SetString(PyExc_OverflowError, "float too large to pack with f format");
    return -1;
}

extern "C" int _PyFloat_Pack8(double x, unsigned char* p, int le) noexcept {
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
        } else
            sign = 0;

        f = frexp(x, &e);

        /* Normalize f to be in the range [1.0, 2.0) */
        if (0.5 <= f && f < 1.0) {
            f *= 2.0;
            e--;
        } else if (f == 0.0)
            e = 0;
        else {
            PyErr_SetString(PyExc_SystemError, "frexp() result out of range");
            return -1;
        }

        if (e >= 1024)
            goto Overflow;
        else if (e < -1022) {
            /* Gradual underflow */
            f = ldexp(f, 1022 + e);
            e = 0;
        } else if (!(e == 0 && f == 0.0)) {
            e += 1023;
            f -= 1.0; /* Get rid of leading 1 */
        }

        /* fhi receives the high 28 bits; flo the low 24 bits (== 52 bits) */
        f *= 268435456.0;      /* 2**28 */
        fhi = (unsigned int)f; /* Truncate */
        assert(fhi < 268435456);

        f -= (double)fhi;
        f *= 16777216.0;               /* 2**24 */
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
        *p = (unsigned char)(((e & 0xF) << 4) | (fhi >> 24));
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
        PyErr_SetString(PyExc_OverflowError, "float too large to pack with d format");
        return -1;
    } else {
        const char* s = (char*)&x;
        int i, incr = 1;

        if ((double_format == ieee_little_endian_format && !le) || (double_format == ieee_big_endian_format && le)) {
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

extern "C" double _PyFloat_Unpack4(const unsigned char* p, int le) noexcept {
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
            PyErr_SetString(PyExc_ValueError, "can't unpack IEEE 754 special value "
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
    } else {
        float x;

        if ((float_format == ieee_little_endian_format && !le) || (float_format == ieee_big_endian_format && le)) {
            char buf[4];
            char* d = &buf[3];
            int i;

            for (i = 0; i < 4; i++) {
                *d-- = *p++;
            }
            memcpy(&x, buf, 4);
        } else {
            memcpy(&x, p, 4);
        }

        return x;
    }
}

extern "C" double _PyFloat_Unpack8(const unsigned char* p, int le) noexcept {
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
            PyErr_SetString(PyExc_ValueError, "can't unpack IEEE 754 special value "
                                              "on non-IEEE platform");
            return -1.0;
        }

        /* Third byte */
        fhi |= *p << 16;
        p += incr;

        /* Fourth byte */
        fhi |= *p << 8;
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
        x /= 268435456.0;                           /* 2**28 */

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
    } else {
        double x;

        if ((double_format == ieee_little_endian_format && !le) || (double_format == ieee_big_endian_format && le)) {
            char buf[8];
            char* d = &buf[7];
            int i;

            for (i = 0; i < 8; i++) {
                *d-- = *p++;
            }
            memcpy(&x, buf, 8);
        } else {
            memcpy(&x, p, 8);
        }

        return x;
    }
}

#if DBL_MANT_DIG == 53
#define FIVE_POW_LIMIT 22
#else
#error "C doubles do not appear to be IEEE 754 binary64 format"
#endif

extern "C" PyObject* _Py_double_round(double x, int ndigits) noexcept {

    double rounded, m;
    Py_ssize_t buflen, mybuflen = 100;
    char* buf, *buf_end, shortbuf[100], * mybuf = shortbuf;
    int decpt, sign, val, halfway_case;
    PyObject* result = NULL;
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
    assert(std::isfinite(x) && x != 0.0);

    /* find 2-valuation val of x */
    m = frexp(x, &val);
    while (m != floor(m)) {
        m *= 2.0;
        val--;
    }

    /* determine whether this is a halfway case */
    if (val == -ndigits - 1) {
        if (ndigits >= 0)
            halfway_case = 1;
        else if (ndigits >= -FIVE_POW_LIMIT) {
            double five_pow = 1.0;
            int i;
            for (i = 0; i < -ndigits; i++)
                five_pow *= 5.0;
            halfway_case = fmod(x, five_pow) == 0.0;
        } else
            halfway_case = 0;
    } else
        halfway_case = 0;

    /* round to a decimal string; use an extra place for halfway case */
    _Py_SET_53BIT_PRECISION_START;
    buf = _Py_dg_dtoa(x, 3, ndigits + halfway_case, &decpt, &sign, &buf_end);
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
        assert(buflen - decpt == ndigits + 1);
        assert(buf[buflen - 1] == '5');

        /* increment and shift right at the same time. */
        decpt += 1;
        carry = 1;
        for (i = buflen - 1; i-- > 0;) {
            carry += buf[i] - '0';
            buf[i + 1] = carry % 10 + '0';
            carry /= 10;
        }
        buf[0] = carry + '0';
    }

    /* Get new buffer if shortbuf is too small.  Space needed <= buf_end -
       buf + 8: (1 extra for '0', 1 for sign, 5 for exp, 1 for '\0'). */
    if (buflen + 8 > mybuflen) {
        mybuflen = buflen + 8;
        mybuf = (char*)PyMem_Malloc(mybuflen);
        if (mybuf == NULL) {
            PyErr_NoMemory();
            goto exit;
        }
    }
    /* copy buf to mybuf, adding exponent, sign and leading 0 */
    PyOS_snprintf(mybuf, mybuflen, "%s0%se%d", (sign ? "-" : ""), buf, decpt - (int)buflen);

    /* and convert the resulting string back to a double */
    errno = 0;
    _Py_SET_53BIT_PRECISION_START;
    rounded = _Py_dg_strtod(mybuf, NULL);
    _Py_SET_53BIT_PRECISION_END;
    if (errno == ERANGE && fabs(rounded) >= 1.)
        PyErr_SetString(PyExc_OverflowError, "rounded value too large to represent");
    else
        result = PyFloat_FromDouble(rounded);

    /* done computing value;  now clean up */
    if (mybuf != shortbuf)
        PyMem_Free(mybuf);
exit:
    _Py_dg_freedtoa(buf);
    return result;
}

static PyMethodDef float_methods[] = { { "hex", (PyCFunction)float_hex, METH_NOARGS, NULL },
                                       { "fromhex", (PyCFunction)float_fromhex, METH_O | METH_CLASS, NULL },
                                       { "as_integer_ratio", (PyCFunction)float_as_integer_ratio, METH_NOARGS, NULL },

                                       { "is_integer", (PyCFunction)float_is_integer, METH_NOARGS, NULL },
                                       { "__format__", (PyCFunction)float__format__, METH_VARARGS, NULL } };

void setupFloat() {
    _addFunc("__add__", BOXED_FLOAT, (void*)floatAddFloat, (void*)floatAddInt, (void*)floatAdd);
    float_cls->giveAttr("__radd__", float_cls->getattr(internStringMortal("__add__")));

    _addFunc("__div__", BOXED_FLOAT, (void*)floatDivFloat, (void*)floatDivInt, (void*)floatDiv);
    _addFunc("__rdiv__", BOXED_FLOAT, (void*)floatRDivFloat, (void*)floatRDivInt, (void*)floatRDiv);
    _addFunc("__floordiv__", BOXED_FLOAT, (void*)floatFloorDivFloat, (void*)floatFloorDivInt, (void*)floatFloorDiv);
    _addFunc("__truediv__", BOXED_FLOAT, (void*)floatDivFloat, (void*)floatDivInt, (void*)floatTruediv);

    _addFunc("__eq__", BOXED_BOOL, (void*)floatEqFloat, (void*)floatEqInt, (void*)floatEq);
    _addFunc("__ge__", BOXED_BOOL, (void*)floatGeFloat, (void*)floatGeInt, (void*)floatGe);
    _addFunc("__gt__", BOXED_BOOL, (void*)floatGtFloat, (void*)floatGtInt, (void*)floatGt);
    _addFunc("__le__", BOXED_BOOL, (void*)floatLeFloat, (void*)floatLeInt, (void*)floatLe);
    _addFunc("__lt__", BOXED_BOOL, (void*)floatLtFloat, (void*)floatLtInt, (void*)floatLt);
    _addFunc("__ne__", BOXED_BOOL, (void*)floatNeFloat, (void*)floatNeInt, (void*)floatNe);

    _addFunc("__mod__", BOXED_FLOAT, (void*)floatModFloat, (void*)floatModInt, (void*)floatMod);
    _addFunc("__rmod__", BOXED_FLOAT, (void*)floatRModFloat, (void*)floatRModInt, (void*)floatRMod);
    _addFunc("__mul__", BOXED_FLOAT, (void*)floatMulFloat, (void*)floatMulInt, (void*)floatMul);
    float_cls->giveAttr("__rmul__", float_cls->getattr(internStringMortal("__mul__")));

    _addFuncPow("__pow__", BOXED_FLOAT, (void*)floatPowFloat, (void*)floatPowInt, (void*)floatPow);
    _addFunc("__sub__", BOXED_FLOAT, (void*)floatSubFloat, (void*)floatSubInt, (void*)floatSub);
    _addFunc("__rsub__", BOXED_FLOAT, (void*)floatRSubFloat, (void*)floatRSubInt, (void*)floatRSub);

    float_cls->giveAttr(
        "__new__", new BoxedFunction(boxRTFunction((void*)floatNew, UNKNOWN, 2, 1, false, false), { boxFloat(0.0) }));

    float_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)floatNeg, BOXED_FLOAT, 1)));

    CLFunction* nonzero = boxRTFunction((void*)floatNonzeroUnboxed, BOOL, 1);
    addRTFunction(nonzero, (void*)floatNonzero, UNKNOWN);
    float_cls->giveAttr("__nonzero__", new BoxedFunction(nonzero));

    // float_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)floatNonzero, NULL, 1)));
    float_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)floatStr, STR, 1)));
    float_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)floatRepr, STR, 1)));

    float_cls->giveAttr("__trunc__", new BoxedFunction(boxRTFunction((void*)floatTrunc, UNKNOWN, 1)));
    float_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)floatHash, BOXED_INT, 1)));

    float_cls->giveAttr("real", new (pyston_getset_cls) BoxedGetsetDescriptor(floatFloat, NULL, NULL));
    float_cls->giveAttr("imag", new (pyston_getset_cls) BoxedGetsetDescriptor(float0, NULL, NULL));
    float_cls->giveAttr("conjugate", new BoxedFunction(boxRTFunction((void*)floatFloat, BOXED_FLOAT, 1)));

    float_cls->giveAttr("__getformat__",
                        new BoxedClassmethod(new BoxedBuiltinFunctionOrMethod(
                            boxRTFunction((void*)floatGetFormat, STR, 2), "__getformat__", floatGetFormatDoc)));

    for (auto& md : float_methods) {
        float_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, float_cls));
    }

    float_cls->freeze();

    floatFormatInit();

    float_cls->tp_as_number->nb_power = float_pow;
}

void teardownFloat() {
}
}
