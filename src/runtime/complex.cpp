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

#include "runtime/complex.h"

#include "core/types.h"
#include "runtime/float.h"
#include "runtime/inline/boxing.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static inline void raiseDivZeroExc() {
    raiseExcHelper(ZeroDivisionError, "complex divide by zero");
}

extern "C" Box* createPureImaginary(double i) {
    return new BoxedComplex(0.0, i);
}

// addition

extern "C" Box* complexAddComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == complex_cls);
    return boxComplex(lhs->real + rhs->real, lhs->imag + rhs->imag);
}

extern "C" Box* complexAddFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    return boxComplex(lhs->real + rhs->d, lhs->imag);
}

extern "C" Box* complexAddInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == int_cls);
    return boxComplex(lhs->real + (double)rhs->n, lhs->imag);
}

extern "C" Box* complexAdd(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (rhs->cls == int_cls) {
        return complexAddInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexAddFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexAddComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

// subtraction

extern "C" Box* complexSubComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == complex_cls);
    return boxComplex(lhs->real - rhs->real, lhs->imag - rhs->imag);
}

extern "C" Box* complexSubFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    return boxComplex(lhs->real - rhs->d, lhs->imag);
}

extern "C" Box* complexSubInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == int_cls);
    return boxComplex(lhs->real - (double)rhs->n, lhs->imag);
}

extern "C" Box* complexSub(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (rhs->cls == int_cls) {
        return complexSubInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexSubFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexSubComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

// multiplication

extern "C" Box* complexMulComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == complex_cls);
    return boxComplex(lhs->real * rhs->real - lhs->imag * rhs->imag, lhs->real * rhs->imag + lhs->imag * rhs->real);
}

extern "C" Box* complexMulFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    return boxComplex(lhs->real * rhs->d, lhs->imag * rhs->d);
}

extern "C" Box* complexMulInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == int_cls);
    return boxComplex(lhs->real * (double)rhs->n, lhs->imag * (double)rhs->n);
}

extern "C" Box* complexMul(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (rhs->cls == int_cls) {
        return complexMulInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexMulFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexMulComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

// division

extern "C" Box* complexDivComplex(BoxedComplex* lhs, BoxedComplex* rhs) {
    // TODO implement this
    // NOTE: the "naive" implementation of complex division has numerical issues
    // see notes in CPython, Objects/complexobject.c, c_quot
    return NotImplemented;
}

extern "C" Box* complexDivFloat(BoxedComplex* lhs, BoxedFloat* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == float_cls);
    if (rhs->d == 0.0) {
        raiseDivZeroExc();
    }
    return boxComplex(lhs->real / rhs->d, lhs->imag / rhs->d);
}

extern "C" Box* complexDivInt(BoxedComplex* lhs, BoxedInt* rhs) {
    assert(lhs->cls == complex_cls);
    assert(rhs->cls == int_cls);
    if (rhs->n == 0) {
        raiseDivZeroExc();
    }
    return boxComplex(lhs->real / (double)rhs->n, lhs->imag / (double)rhs->n);
}

extern "C" Box* complexDiv(BoxedComplex* lhs, Box* rhs) {
    assert(lhs->cls == complex_cls);
    if (rhs->cls == int_cls) {
        return complexDivInt(lhs, static_cast<BoxedInt*>(rhs));
    } else if (rhs->cls == float_cls) {
        return complexDivFloat(lhs, static_cast<BoxedFloat*>(rhs));
    } else if (rhs->cls == complex_cls) {
        return complexDivComplex(lhs, static_cast<BoxedComplex*>(rhs));
    } else {
        return NotImplemented;
    }
}

// str and repr
// For now, just print the same way as ordinary doubles.
// TODO this is wrong, e.g. if real or imaginary part is an integer, there should
// be no decimal point, maybe some other differences. Need to dig deeper into
// how CPython formats floats and complex numbers.
// (complex_format in Objects/complexobject.c)
std::string complexFmt(double r, double i, int precision, char code) {
    if (r == 0. && copysign(1.0, r) == 1.0) {
        return floatFmt(i, precision, code) + "j";
    } else {
        return "(" + floatFmt(r, precision, code) + (isnan(i) || i >= 0.0 ? "+" : "") + floatFmt(i, precision, code)
               + "j)";
    }
}

static void _addFunc(const char* name, ConcreteCompilerType* rtn_type, void* complex_func, void* float_func,
                     void* int_func, void* boxed_func) {
    CLFunction* cl = createRTFunction(2, 0, false, false);
    addRTFunction(cl, complex_func, rtn_type, { BOXED_COMPLEX, BOXED_COMPLEX });
    addRTFunction(cl, float_func, rtn_type, { BOXED_COMPLEX, BOXED_FLOAT });
    addRTFunction(cl, int_func, rtn_type, { BOXED_COMPLEX, BOXED_INT });
    addRTFunction(cl, boxed_func, UNKNOWN, { BOXED_COMPLEX, UNKNOWN });
    complex_cls->giveAttr(name, new BoxedFunction(cl));
}

Box* complexStr(BoxedComplex* self) {
    assert(self->cls == complex_cls);
    return boxString(complexFmt(self->real, self->imag, 12, 'g'));
}

Box* complexRepr(BoxedComplex* self) {
    assert(self->cls == complex_cls);
    return boxString(complexFmt(self->real, self->imag, 16, 'g'));
}

Box* complexNew(Box* _cls, Box* real, Box* imag) {
    RELEASE_ASSERT(_cls == complex_cls, "");

    double real_f;
    if (real->cls == int_cls) {
        real_f = static_cast<BoxedInt*>(real)->n;
    } else if (real->cls == float_cls) {
        real_f = static_cast<BoxedFloat*>(real)->d;
    } else {
        // TODO: implement taking a string argument
        raiseExcHelper(TypeError, "complex() argument must be a string or number");
    }

    double imag_f;
    if (imag->cls == int_cls) {
        imag_f = static_cast<BoxedInt*>(imag)->n;
    } else if (imag->cls == float_cls) {
        imag_f = static_cast<BoxedFloat*>(imag)->d;
    } else if (imag->cls == str_cls) {
        raiseExcHelper(TypeError, "complex() second arg can't be a string");
    } else {
        raiseExcHelper(TypeError, "complex() argument must be a string or number");
    }

    return new BoxedComplex(real_f, imag_f);
}

void setupComplex() {
    complex_cls->giveAttr("__name__", boxStrConstant("complex"));

    complex_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)complexNew, UNKNOWN, 3, 2, false, false),
                                                       { boxInt(0), boxInt(0) }));

    _addFunc("__add__", BOXED_COMPLEX, (void*)complexAddComplex, (void*)complexAddFloat, (void*)complexAddInt,
             (void*)complexAdd);

    _addFunc("__sub__", BOXED_COMPLEX, (void*)complexSubComplex, (void*)complexSubFloat, (void*)complexSubInt,
             (void*)complexSub);

    _addFunc("__mul__", BOXED_COMPLEX, (void*)complexMulComplex, (void*)complexMulFloat, (void*)complexMulInt,
             (void*)complexMul);

    _addFunc("__div__", BOXED_COMPLEX, (void*)complexDivComplex, (void*)complexDivFloat, (void*)complexDivInt,
             (void*)complexDiv);

    complex_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)complexStr, STR, 1)));
    complex_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)complexRepr, STR, 1)));
    complex_cls->giveAttr("real",
                          new BoxedMemberDescriptor(BoxedMemberDescriptor::DOUBLE, offsetof(BoxedComplex, real)));
    complex_cls->giveAttr("imag",
                          new BoxedMemberDescriptor(BoxedMemberDescriptor::DOUBLE, offsetof(BoxedComplex, imag)));
    complex_cls->freeze();
}

void teardownComplex() {
}
}
