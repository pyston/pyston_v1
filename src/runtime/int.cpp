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

#include <sstream>

#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

#include "runtime/inline/boxing.h"

#include "gc/collector.h"

#include "codegen/compvars.h"

namespace pyston {

BoxedInt* interned_ints[NUM_INTERNED_INTS];

// Could add this to the others, but the inliner should be smart enough
// that this isn't needed:
extern "C" i64 add_i64_i64(i64 lhs, i64 rhs) {
    return lhs + rhs;
}

extern "C" i64 sub_i64_i64(i64 lhs, i64 rhs) {
    return lhs - rhs;
}

extern "C" i64 div_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        fprintf(stderr, "ZeroDivisionError: integer division or modulo by zero\n");
        raiseExc();
    }
    if (lhs < 0 && rhs > 0)
        return (lhs - rhs + 1) / rhs;
    if (lhs > 0 && rhs < 0)
        return (lhs - rhs - 1) / rhs;
    return lhs / rhs;
}

extern "C" i64 mod_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        fprintf(stderr, "ZeroDivisionError: integer division or modulo by zero\n");
        raiseExc();
    }
    if (lhs < 0 && rhs > 0)
        return ((lhs + 1) % rhs) + (rhs - 1);
    if (lhs > 0 && rhs < 0)
        return ((lhs - 1) % rhs) + (rhs + 1);
    return lhs % rhs;
}

extern "C" i64 pow_i64_i64(i64 lhs, i64 rhs) {
    // TODO overflow very possible
    i64 rtn = 1, curpow = lhs;
    RELEASE_ASSERT(rhs >= 0, "");
    while (rhs) {
        if (rhs & 1)
            rtn *= curpow;
        curpow *= curpow;
        rhs >>= 1;
    }
    return rtn;
}

extern "C" i64 mul_i64_i64(i64 lhs, i64 rhs) {
    return lhs * rhs;
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


extern "C" Box* intAddInt(BoxedInt* lhs, BoxedInt *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n + rhs->n);
}

extern "C" Box* intAddFloat(BoxedInt* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n + rhs->d);
}

extern "C" Box* intAdd(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(lhs->n + rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n + rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intAnd(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n & rhs_int->n);
}

extern "C" Box* intDivInt(BoxedInt* lhs, BoxedInt *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(div_i64_i64(lhs->n, rhs->n));
}

extern "C" Box* intDivFloat(BoxedInt* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);

    if (rhs->d == 0) {
        fprintf(stderr, "float divide by zero\n");
        raiseExc();
    }
    return boxFloat(lhs->n / rhs->d);
}

extern "C" Box* intDiv(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        return intDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return intDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intEq(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n == rhs_int->n);
}

extern "C" Box* intNe(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n != rhs_int->n);
}

extern "C" Box* intLt(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n < rhs_int->n);
}

extern "C" Box* intLe(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n <= rhs_int->n);
}

extern "C" Box* intGt(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n > rhs_int->n);
}

extern "C" Box* intGe(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n >= rhs_int->n);
}

extern "C" Box* intLShift(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n << rhs_int->n);
}

extern "C" Box* intMod(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);

    return boxInt(mod_i64_i64(lhs->n, rhs_int->n));
}

extern "C" Box* intMulInt(BoxedInt* lhs, BoxedInt *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n * rhs->n);
}

extern "C" Box* intMulFloat(BoxedInt* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n * rhs->d);
}

extern "C" Box* intMul(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(lhs->n * rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n * rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intPow(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(pow_i64_i64(lhs->n, rhs_int->n));
}

extern "C" Box* intRShift(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n >> rhs_int->n);
}

extern "C" Box* intSubInt(BoxedInt* lhs, BoxedInt *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n - rhs->n);
}

extern "C" Box* intSubFloat(BoxedInt* lhs, BoxedFloat *rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n - rhs->d);
}

extern "C" Box* intSub(BoxedInt* lhs, Box *rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        BoxedInt *rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(lhs->n - rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat *rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n - rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intInvert(BoxedInt* v) {
    assert(v->cls == int_cls);
    return boxInt(~v->n);
}

extern "C" Box* intPos(BoxedInt* v) {
    assert(v->cls == int_cls);
    return v;
}

extern "C" Box* intNeg(BoxedInt* v) {
    assert(v->cls == int_cls);
    return boxInt(-v->n);
}

extern "C" Box* intNonzero(BoxedInt* v) {
    assert(v->cls == int_cls);
    return boxBool(v->n != 0);
}

extern "C" BoxedString* intRepr(BoxedInt* v) {
    assert(v->cls == int_cls);
    char buf[80];
    int len = snprintf(buf, 80, "%ld", v->n);
    return new BoxedString(std::string(buf, len));
}

extern "C" Box* intHash(BoxedInt* self) {
    assert(self->cls == int_cls);
    return self;
}

extern "C" Box* intNew1(Box* cls) {
    assert(cls == int_cls);
    return new BoxedInt(0);
}

extern "C" Box* intNew2(Box* cls, Box* val) {
    assert(cls == int_cls);

    if (val->cls == int_cls) {
        return val;
    } else if (val->cls == str_cls) {
        BoxedString *s = static_cast<BoxedString*>(val);

        std::istringstream ss(s->s);
        int64_t n;
        ss >> n;
        return boxInt(n);
    } else if (val->cls == float_cls) {
        double d = static_cast<BoxedFloat*>(val)->d;

        return boxInt(d);
    } else {
        fprintf(stderr, "int() argument must be a string or a number, not '%s'\n", getTypeName(val)->c_str());
        raiseExc();
    }
}

extern "C" Box* intInit1(BoxedInt* self) {
    assert(self->cls == int_cls);

    return None;
}

extern "C" Box* intInit2(BoxedInt* self, Box* val) {
    assert(self->cls == int_cls);

    return None;
}

static void _addFunc(const char* name, void* int_func, void* float_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_if, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(BOXED_INT); v_ii.push_back(BOXED_INT);
    v_if.push_back(BOXED_INT); v_if.push_back(BOXED_FLOAT);
    v_iu.push_back(BOXED_INT); v_iu.push_back(NULL);

    CLFunction *cl = createRTFunction();
    addRTFunction(cl, int_func, NULL, v_ii, false);
    addRTFunction(cl, float_func, NULL, v_if, false);
    addRTFunction(cl, boxed_func, NULL, v_iu, false);
    int_cls->giveAttr(name, new BoxedFunction(cl));
}

void setupInt() {
    int_cls->giveAttr("__name__", boxStrConstant("int"));

    //int_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)intAdd, NULL, 2, false)));
    _addFunc("__add__", (void*)intAddInt, (void*)intAddFloat, (void*)intAdd);
    int_cls->giveAttr("__and__", new BoxedFunction(boxRTFunction((void*)intAnd, NULL, 2, false)));
    _addFunc("__div__", (void*)intDivInt, (void*)intDivFloat, (void*)intDiv);
    //int_cls->giveAttr("__div__", new BoxedFunction(boxRTFunction((void*)intDiv, NULL, 2, false)));
    int_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)intEq, NULL, 2, false)));
    int_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)intNe, NULL, 2, false)));
    int_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)intLt, NULL, 2, false)));
    int_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)intLe, NULL, 2, false)));
    int_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)intGt, NULL, 2, false)));
    int_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)intGe, NULL, 2, false)));
    int_cls->giveAttr("__lshift__", new BoxedFunction(boxRTFunction((void*)intLShift, NULL, 2, false)));
    int_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)intMod, NULL, 2, false)));
    _addFunc("__mul__", (void*)intMulInt, (void*)intMulFloat, (void*)intMul);
    //int_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)intMul, NULL, 2, false)));
    int_cls->giveAttr("__pow__", new BoxedFunction(boxRTFunction((void*)intPow, NULL, 2, false)));
    int_cls->giveAttr("__rshift__", new BoxedFunction(boxRTFunction((void*)intRShift, NULL, 2, false)));
    _addFunc("__sub__", (void*)intSubInt, (void*)intSubFloat, (void*)intSub);
    //int_cls->giveAttr("__sub__", new BoxedFunction(boxRTFunction((void*)intSub, NULL, 2, false)));

    int_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)intInvert, NULL, 1, false)));
    int_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)intPos, NULL, 1, false)));
    int_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)intNeg, NULL, 1, false)));
    int_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)intNonzero, NULL, 1, false)));
    int_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)intRepr, NULL, 1, false)));
    int_cls->setattr("__str__", int_cls->peekattr("__repr__"), NULL, NULL);
    int_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)intHash, NULL, 1, false)));

    CLFunction *__new__ = boxRTFunction((void*)intNew1, NULL, 1, false);
    addRTFunction(__new__, (void*)intNew2, NULL, 2, false);
    int_cls->giveAttr("__new__", new BoxedFunction(__new__));

    CLFunction *__init__ = boxRTFunction((void*)intInit1, NULL, 1, false);
    addRTFunction(__init__, (void*)intInit2, NULL, 2, false);
    int_cls->giveAttr("__init__", new BoxedFunction(__init__));

    int_cls->freeze();

    for (int i = 0; i < NUM_INTERNED_INTS; i++) {
        interned_ints[i] = new BoxedInt(i);
        gc::registerStaticRootObj(interned_ints[i]);
    }
}

void teardownInt() {
}

}
