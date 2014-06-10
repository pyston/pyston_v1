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

#include "runtime/int.h"

#include <cmath>
#include <sstream>

#include "codegen/compvars.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/gc_runtime.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

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
        raiseExcHelper(ZeroDivisionError, "integer division or modulo by zero");
    }
    if (lhs < 0 && rhs > 0)
        return (lhs - rhs + 1) / rhs;
    if (lhs > 0 && rhs < 0)
        return (lhs - rhs - 1) / rhs;
    return lhs / rhs;
}

extern "C" i64 mod_i64_i64(i64 lhs, i64 rhs) {
    if (rhs == 0) {
        raiseExcHelper(ZeroDivisionError, "integer division or modulo by zero");
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


extern "C" Box* intAddInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n + rhs->n);
}

extern "C" Box* intAddFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n + rhs->d);
}

extern "C" Box* intAdd(BoxedInt* lhs, Box* rhs) {
    if (!isSubclass(lhs->cls, int_cls))
        raiseExcHelper(TypeError, "descriptor '__add__' requires a 'int' object but received a '%s'",
                       getTypeName(rhs)->c_str());

    if (isSubclass(rhs->cls, int_cls)) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(lhs->n + rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n + rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intAndInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n & rhs->n);
}

extern "C" Box* intAnd(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n & rhs_int->n);
}

extern "C" Box* intDivInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(div_i64_i64(lhs->n, rhs->n));
}

extern "C" Box* intDivFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);

    if (rhs->d == 0) {
        raiseExcHelper(ZeroDivisionError, "float divide by zero");
    }
    return boxFloat(lhs->n / rhs->d);
}

extern "C" Box* intDiv(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        return intDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return intDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intEqInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxBool(lhs->n == rhs->n);
}

extern "C" Box* intEq(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n == rhs_int->n);
}

extern "C" Box* intNeInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxBool(lhs->n != rhs->n);
}

extern "C" Box* intNe(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n != rhs_int->n);
}

extern "C" Box* intLtInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxBool(lhs->n < rhs->n);
}

extern "C" Box* intLt(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n < rhs_int->n);
}

extern "C" Box* intLeInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxBool(lhs->n <= rhs->n);
}

extern "C" Box* intLe(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n <= rhs_int->n);
}

extern "C" Box* intGtInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxBool(lhs->n > rhs->n);
}

extern "C" Box* intGt(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n > rhs_int->n);
}

extern "C" Box* intGeInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxBool(lhs->n >= rhs->n);
}

extern "C" Box* intGe(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxBool(lhs->n >= rhs_int->n);
}

extern "C" Box* intLShiftInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n << rhs->n);
}

extern "C" Box* intLShift(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n << rhs_int->n);
}

extern "C" Box* intModInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(mod_i64_i64(lhs->n, rhs->n));
}

extern "C" Box* intMod(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(mod_i64_i64(lhs->n, rhs_int->n));
}

extern "C" Box* intDivmod(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
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
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n * rhs->n);
}

extern "C" Box* intMulFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n * rhs->d);
}

extern "C" Box* intMul(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(lhs->n * rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(lhs->n * rhs_float->d);
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intPowInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(pow_i64_i64(lhs->n, rhs_int->n));
}

extern "C" Box* intPowFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(pow(lhs->n, rhs->d));
}

extern "C" Box* intPow(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(pow_i64_i64(lhs->n, rhs_int->n));
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
        return boxFloat(pow(lhs->n, rhs_float->d));
    } else {
        return NotImplemented;
    }
}

extern "C" Box* intRShiftInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n >> rhs->n);
}

extern "C" Box* intRShift(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls != int_cls) {
        return NotImplemented;
    }
    BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
    return boxInt(lhs->n >> rhs_int->n);
}

extern "C" Box* intSubInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == int_cls);
    return boxInt(lhs->n - rhs->n);
}

extern "C" Box* intSubFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == int_cls);
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n - rhs->d);
}

extern "C" Box* intSub(BoxedInt* lhs, Box* rhs) {
    assert(lhs->cls == int_cls);
    if (rhs->cls == int_cls) {
        BoxedInt* rhs_int = static_cast<BoxedInt*>(rhs);
        return boxInt(lhs->n - rhs_int->n);
    } else if (rhs->cls == float_cls) {
        BoxedFloat* rhs_float = static_cast<BoxedFloat*>(rhs);
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
    if (!isSubclass(v->cls, int_cls))
        raiseExcHelper(TypeError, "descriptor '__pos__' requires a 'int' object but received a '%s'",
                       getTypeName(v)->c_str());

    if (v->cls == int_cls)
        return v;
    return boxInt(v->n);
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
    if (!isSubclass(v->cls, int_cls))
        raiseExcHelper(TypeError, "descriptor '__repr__' requires a 'int' object but received a '%s'",
                       getTypeName(v)->c_str());

    char buf[80];
    int len = snprintf(buf, 80, "%ld", v->n);
    return new BoxedString(std::string(buf, len));
}

extern "C" Box* intHash(BoxedInt* self) {
    assert(self->cls == int_cls);
    return self;
}

extern "C" Box* intNew(Box* _cls, Box* val) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "int.__new__(X): X is not a type object (%s)", getTypeName(_cls)->c_str());

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, int_cls))
        raiseExcHelper(TypeError, "int.__new__(%s): %s is not a subtype of int", getNameOfClass(cls)->c_str(),
                       getNameOfClass(cls)->c_str());

    assert(cls->instance_size >= sizeof(BoxedInt));
    void* mem = rt_alloc(cls->instance_size);
    BoxedInt* rtn = ::new (mem) BoxedInt(cls, 0);
    initUserAttrs(rtn, cls);

    if (val->cls == int_cls) {
        rtn->n = static_cast<BoxedInt*>(val)->n;
    } else if (val->cls == str_cls) {
        BoxedString* s = static_cast<BoxedString*>(val);

        std::istringstream ss(s->s);
        int64_t n;
        ss >> n;
        rtn->n = n;
    } else if (val->cls == float_cls) {
        double d = static_cast<BoxedFloat*>(val)->d;

        rtn->n = d;
    } else {
        fprintf(stderr, "TypeError: int() argument must be a string or a number, not '%s'\n",
                getTypeName(val)->c_str());
        raiseExcHelper(TypeError, "");
    }
    return rtn;
}

extern "C" Box* intInit(BoxedInt* self, Box* val, Box* args) {
    // int.__init__ will actually let you call it with anything
    return None;
}

static void _addFuncIntFloatUnknown(const char* name, void* int_func, void* float_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_if, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_if.push_back(BOXED_INT);
    v_if.push_back(BOXED_FLOAT);
    // Only the unknown version can accept non-ints (ex if you access the function directly ex via int.__add__)
    v_iu.push_back(UNKNOWN);
    v_iu.push_back(UNKNOWN);

    CLFunction* cl = createRTFunction(2, 0, false, false);
    addRTFunction(cl, int_func, BOXED_INT, v_ii);
    addRTFunction(cl, float_func, BOXED_FLOAT, v_if);
    addRTFunction(cl, boxed_func, UNKNOWN, v_iu);
    int_cls->giveAttr(name, new BoxedFunction(cl));
}

static void _addFuncIntUnknown(const char* name, ConcreteCompilerType* rtn_type, void* int_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_ii, v_iu;
    assert(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_ii.push_back(BOXED_INT);
    v_iu.push_back(BOXED_INT);
    v_iu.push_back(UNKNOWN);

    CLFunction* cl = createRTFunction(2, 0, false, false);
    addRTFunction(cl, int_func, rtn_type, v_ii);
    addRTFunction(cl, boxed_func, UNKNOWN, v_iu);
    int_cls->giveAttr(name, new BoxedFunction(cl));
}

void setupInt() {
    for (int i = 0; i < NUM_INTERNED_INTS; i++) {
        interned_ints[i] = new BoxedInt(int_cls, i);
        gc::registerStaticRootObj(interned_ints[i]);
    }

    int_cls->giveAttr("__name__", boxStrConstant("int"));

    _addFuncIntFloatUnknown("__add__", (void*)intAddInt, (void*)intAddFloat, (void*)intAdd);
    _addFuncIntUnknown("__and__", BOXED_INT, (void*)intAndInt, (void*)intAnd);
    _addFuncIntFloatUnknown("__sub__", (void*)intSubInt, (void*)intSubFloat, (void*)intSub);
    _addFuncIntFloatUnknown("__div__", (void*)intDivInt, (void*)intDivFloat, (void*)intDiv);
    _addFuncIntFloatUnknown("__mul__", (void*)intMulInt, (void*)intMulFloat, (void*)intMul);
    _addFuncIntUnknown("__mod__", BOXED_INT, (void*)intModInt, (void*)intMod);
    _addFuncIntFloatUnknown("__pow__", (void*)intPowInt, (void*)intPowFloat, (void*)intPow);

    _addFuncIntUnknown("__eq__", BOXED_BOOL, (void*)intEqInt, (void*)intEq);
    _addFuncIntUnknown("__ne__", BOXED_BOOL, (void*)intNeInt, (void*)intNe);
    _addFuncIntUnknown("__lt__", BOXED_BOOL, (void*)intLtInt, (void*)intLt);
    _addFuncIntUnknown("__le__", BOXED_BOOL, (void*)intLeInt, (void*)intLe);
    _addFuncIntUnknown("__gt__", BOXED_BOOL, (void*)intGtInt, (void*)intGt);
    _addFuncIntUnknown("__ge__", BOXED_BOOL, (void*)intGeInt, (void*)intGe);

    _addFuncIntUnknown("__lshift__", BOXED_INT, (void*)intLShiftInt, (void*)intLShift);
    _addFuncIntUnknown("__rshift__", BOXED_INT, (void*)intRShiftInt, (void*)intRShift);

    int_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)intInvert, BOXED_INT, 1)));
    int_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)intPos, BOXED_INT, 1)));
    int_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)intNeg, BOXED_INT, 1)));
    int_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)intNonzero, BOXED_BOOL, 1)));
    int_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)intRepr, STR, 1)));
    int_cls->giveAttr("__str__", int_cls->getattr("__repr__"));
    int_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)intHash, BOXED_INT, 1)));
    int_cls->giveAttr("__divmod__", new BoxedFunction(boxRTFunction((void*)intDivmod, BOXED_TUPLE, 2)));

    int_cls->giveAttr("__new__",
                      new BoxedFunction(boxRTFunction((void*)intNew, BOXED_INT, 2, 1, false, false), { boxInt(0) }));

    int_cls->giveAttr("__init__",
                      new BoxedFunction(boxRTFunction((void*)intInit, NONE, 2, 1, true, false), { boxInt(0) }));

    int_cls->freeze();
}

void teardownInt() {
}
}
