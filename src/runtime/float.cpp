// Copyright (c) 2014 Dropbox, Inc.
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

#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

#include "runtime/inline/boxing.h"

#include "codegen/compvars.h"

namespace pyston {

extern "C" double mod_float_float(double lhs, double rhs) {
    if (rhs == 0) {
        fprintf(stderr, "float divide by zero\n");
        raiseExc();
    }
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
    if (rhs == 0) {
        fprintf(stderr, "float divide by zero\n");
        raiseExc();
    }
    return lhs / rhs;
}

extern "C" Box* floatAddFloat(BoxedFloat* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
    return boxFloat(lhs->d + rhs_float->d);
}

extern "C" Box* floatAdd(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    //printf("floatAdd %p %p\n", lhs, rhs);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxFloat(lhs->d + rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->d + rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatDiv(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        if (rhs_int->n == 0) {
            fprintf(stderr, "float divide by zero\n");
            raiseExc();
        }
        return boxFloat(lhs->d / rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        if (rhs_float->d == 0) {
            fprintf(stderr, "float divide by zero\n");
            raiseExc();
        }
        return boxFloat(lhs->d / rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatRDiv(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (lhs->d == 0) {
        fprintf(stderr, "float divide by zero\n");
        raiseExc();
    }

    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxFloat(rhs_int->n / lhs->d);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(rhs_float->d / lhs->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatEq(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxBool(lhs->d == rhs_float->d);
    } else if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxBool(lhs->d == rhs_int->n);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatFloorDiv(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls != float_cls) {
        return NotImplemented;
    }
    BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
    if (rhs_float->d == 0) {
        fprintf(stderr, "float divide by zero\n");
        raiseExc();
    }
    return boxFloat(floor(lhs->d / rhs_float->d));
}

extern "C" Box* floatNe(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxBool(lhs->d != rhs_float->d);
    } else if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxBool(lhs->d != rhs_int->n);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatLt(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxBool(lhs->d < rhs_float->d);
    } else if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxBool(lhs->d < rhs_int->n);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatLe(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxBool(lhs->d <= rhs_float->d);
    } else if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxBool(lhs->d <= rhs_int->n);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatGt(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxBool(lhs->d > rhs_float->d);
    } else if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxBool(lhs->d > rhs_int->n);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatGe(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxBool(lhs->d >= rhs_float->d);
    } else if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxBool(lhs->d >= rhs_int->n);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatModFloat(BoxedFloat* lhs, Box *rhs) {
    assert(rhs->cls == float_cls);
    BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
    double drhs = rhs_float->d;

    if (drhs == 0) {
        fprintf(stderr, "float div by zero\n");
        raiseExc();
    }

    return boxFloat(mod_float_float(lhs->d, drhs));
}

extern "C" Box* floatMod(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    double drhs;
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        drhs = rhs_int->n;
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        drhs = rhs_float->d;
    } else {
        return NotImplemented;
    }

    if (drhs == 0) {
        fprintf(stderr, "float div by zero\n");
        raiseExc();
    }

    return boxFloat(mod_float_float(lhs->d, drhs));
}

extern "C" Box* floatRModFloat(BoxedFloat* lhs, Box *rhs) {
    assert(rhs->cls == float_cls);
    BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
    double drhs = rhs_float->d;

    if (lhs->d == 0) {
        fprintf(stderr, "float div by zero\n");
        raiseExc();
    }

    return boxFloat(mod_float_float(drhs, lhs->d));
}

extern "C" Box* floatRMod(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    double drhs;
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        drhs = rhs_int->n;
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        drhs = rhs_float->d;
    } else {
        return NotImplemented;
    }

    if (lhs->d == 0) {
        fprintf(stderr, "float div by zero\n");
        raiseExc();
    }

    return boxFloat(mod_float_float(drhs, lhs->d));
}

extern "C" Box* floatPow(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxFloat(pow(lhs->d, rhs_int->n));
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(pow(lhs->d, rhs_float->d));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatMulFloat(BoxedFloat* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
    return boxFloat(lhs->d * rhs_float->d);
}

extern "C" Box* floatMul(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxFloat(lhs->d * rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->d * rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatSubFloat(BoxedFloat* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == float_cls);
    assert(rhs->cls == float_cls);
    BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
    return boxFloat(lhs->d - rhs_float->d);
}

extern "C" Box* floatSub(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxFloat(lhs->d - rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->d - rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* floatRSub(BoxedFloat* lhs, Box *rhs) {
    assert(lhs->cls == float_cls);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxFloat(rhs_int->n - lhs->d);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(rhs_float->d - lhs->d);
    } else {
        return NotImplemented;
    }
}

Box* floatNeg(BoxedFloat *self) {
    assert(self->cls == float_cls);
    return boxFloat(-self->d);
}

bool floatNonzeroUnboxed(BoxedFloat *self) {
    assert(self->cls == float_cls);
    return self->d != 0.0;
}

Box* floatNonzero(BoxedFloat *self) {
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
            int exp_digs = snprintf(buf + n + 1, 5, "e%+.02d", (n-first-1));
            n += exp_digs + 1;
            dot = 1;
        } else {
            buf[n] = '.';
            buf[n+1] = '0';
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

Box* floatNew1(BoxedClass *cls) {
    assert(cls == float_cls);
    // TODO intern this?
    return boxFloat(0.0);
}

Box* floatNew2(BoxedClass *cls, Box* a) {
    assert(cls == float_cls);

    if (a->cls == float_cls) {
        return a;
    } else if (a->cls == str_cls) {
        const std::string &s = static_cast<BoxedString*>(a)->s;
        if (s == "nan")
            return boxFloat(NAN);
        if (s == "-nan")
            return boxFloat(-NAN);
        if (s == "inf")
            return boxFloat(INFINITY);
        if (s == "-inf")
            return boxFloat(-INFINITY);

        return boxFloat(strtod(s.c_str(), NULL));
    }
    RELEASE_ASSERT(0, "%s", getTypeName(a)->c_str());
}

Box* floatStr(BoxedFloat *self) {
    assert(self->cls == float_cls);
    return boxString(floatFmt(self->d, 12, 'g'));
}

Box* floatRepr(BoxedFloat *self) {
    assert(self->cls == float_cls);
    return boxString(floatFmt(self->d, 16, 'g'));
}

extern "C" void printFloat(double d) {
    std::string s = floatFmt(d, 12, 'g');
    printf("%s", s.c_str());
}

static void _addFunc(const char* name, void* float_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ff, v_fu;
    v_ff.push_back(BOXED_FLOAT); v_ff.push_back(BOXED_FLOAT);
    v_fu.push_back(BOXED_FLOAT); v_fu.push_back(NULL);

    CLFunction *cl = createRTFunction();
    addRTFunction(cl, float_func, BOXED_FLOAT, v_ff, false);
    addRTFunction(cl, boxed_func, NULL, v_fu, false);
    float_cls->giveAttr(name, new BoxedFunction(cl));
}

void setupFloat() {
    float_cls->giveAttr("__name__", boxStrConstant("float"));

    _addFunc("__add__", (void*)floatAddFloat, (void*)floatAdd);
    //float_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)floatAdd, NULL, 2, false)));
    float_cls->setattr("__radd__", float_cls->peekattr("__add__"), NULL, NULL);
    float_cls->giveAttr("__div__", new BoxedFunction(boxRTFunction((void*)floatDiv, NULL, 2, false)));
    float_cls->giveAttr("__rdiv__", new BoxedFunction(boxRTFunction((void*)floatRDiv, NULL, 2, false)));
    float_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)floatEq, NULL, 2, false)));
    float_cls->giveAttr("__floordiv__", new BoxedFunction(boxRTFunction((void*)floatFloorDiv, NULL, 2, false)));
    float_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)floatGe, NULL, 2, false)));
    float_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)floatGt, NULL, 2, false)));
    float_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)floatLe, NULL, 2, false)));
    float_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)floatLt, NULL, 2, false)));
    _addFunc("__mod__", (void*)floatModFloat, (void*)floatMod);
    _addFunc("__rmod__", (void*)floatRModFloat, (void*)floatRMod);
    _addFunc("__mul__", (void*)floatMulFloat, (void*)floatMul);
    float_cls->setattr("__rmul__", float_cls->peekattr("__mul__"), NULL, NULL);
    float_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)floatNe, NULL, 2, false)));
    float_cls->giveAttr("__pow__", new BoxedFunction(boxRTFunction((void*)floatPow, NULL, 2, false)));
    //float_cls->giveAttr("__sub__", new BoxedFunction(boxRTFunction((void*)floatSub, NULL, 2, false)));
    _addFunc("__sub__", (void*)floatSubFloat, (void*)floatSub);
    float_cls->giveAttr("__rsub__", new BoxedFunction(boxRTFunction((void*)floatRSub, NULL, 2, false)));

    CLFunction *__new__ = boxRTFunction((void*)floatNew1, NULL, 1, false);
    addRTFunction(__new__, (void*)floatNew2, NULL, 2, false);
    float_cls->giveAttr("__new__", new BoxedFunction(__new__));

    float_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)floatNeg, NULL, 1, false)));
    CLFunction *nonzero = boxRTFunction((void*)floatNonzeroUnboxed, BOOL, 1, false);
    addRTFunction(nonzero, (void*)floatNonzero, UNKNOWN, 1, false);
    float_cls->giveAttr("__nonzero__", new BoxedFunction(nonzero));
    //float_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)floatNonzero, NULL, 1, false)));
    float_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)floatStr, NULL, 1, false)));
    float_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)floatRepr, NULL, 1, false)));
    float_cls->freeze();
}

void teardownFloat() {
}

}
