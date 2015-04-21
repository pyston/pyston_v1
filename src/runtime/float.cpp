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

#include <cmath>
#include <cstring>

#include "core/types.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" PyObject* PyFloat_FromDouble(double d) noexcept {
    return boxFloat(d);
}

extern "C" PyObject* PyFloat_FromString(PyObject* v, char** pend) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" double PyFloat_AsDouble(PyObject* o) noexcept {
    if (o->cls == float_cls)
        return static_cast<BoxedFloat*>(o)->d;
    else if (isSubclass(o->cls, int_cls))
        return static_cast<BoxedInt*>(o)->n;
    Py_FatalError("unimplemented");
    return 0.0;
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

extern "C" double pow_float_float(double lhs, double rhs) {
    return pow(lhs, rhs);
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

extern "C" Box* floatPowFloat(BoxedFloat* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(pow(lhs->d, rhs->d));
}

extern "C" Box* floatPowInt(BoxedFloat* lhs, BoxedInt* rhs) {
    assert(lhs->cls == float_cls);
    assert(isSubclass(rhs->cls, int_cls));
    return boxFloat(pow(lhs->d, rhs->n));
}

extern "C" Box* floatPow(BoxedFloat* lhs, Box* rhs) {
    assert(lhs->cls == float_cls);
    if (isSubclass(rhs->cls, int_cls)) {
        return floatPowInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return floatPowFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == long_cls) {
        return boxFloat(pow(lhs->d, PyLong_AsDouble(rhs)));
    } else {
        return NotImplemented;
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
        const std::string& s = static_cast<BoxedString*>(a)->s;
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
        const char* startptr = s.c_str();
        double r = strtod(startptr, &endptr);
        if (endptr != startptr + s.size())
            raiseExcHelper(ValueError, "could not convert string to float: %s", s.c_str());
        return new BoxedFloat(r);
    } else {
        static const std::string float_str("__float__");
        Box* r = callattr(a, &float_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = true }),
                          ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);

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

void setupFloat() {
    _addFunc("__add__", BOXED_FLOAT, (void*)floatAddFloat, (void*)floatAddInt, (void*)floatAdd);
    float_cls->giveAttr("__radd__", float_cls->getattr("__add__"));

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
    float_cls->giveAttr("__rmul__", float_cls->getattr("__mul__"));

    _addFunc("__pow__", BOXED_FLOAT, (void*)floatPowFloat, (void*)floatPowInt, (void*)floatPow);
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

    float_cls->giveAttr("__trunc__", new BoxedFunction(boxRTFunction((void*)floatTrunc, BOXED_INT, 1)));

    float_cls->freeze();
}

void teardownFloat() {
}
}
