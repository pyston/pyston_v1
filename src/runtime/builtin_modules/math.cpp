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

#define _USE_MATH_DEFINES
#include <cmath>

#include "codegen/compvars.h"
#include "core/types.h"
#include "runtime/gc_runtime.h"
#include "runtime/inline/boxing.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* math_module;

static double _extractFloat(Box* b) {
    if (b->cls != int_cls && b->cls != float_cls) {
        raiseExcHelper(TypeError, "a float is required");
    }

    if (b->cls == int_cls)
        return static_cast<BoxedInt*>(b)->n;
    else
        return static_cast<BoxedFloat*>(b)->d;
}

Box* mathSqrtFloat(Box* b) {
    assert(b->cls == float_cls);
    double d = static_cast<BoxedFloat*>(b)->d;
    if (d < 0) {
        raiseExcHelper(ValueError, "math domain error");
    }
    return boxFloat(sqrt(d));
}

Box* mathSqrtInt(Box* b) {
    assert(b->cls == int_cls);
    double d = static_cast<BoxedInt*>(b)->n;
    if (d < 0) {
        raiseExcHelper(ValueError, "math domain error");
    }
    return boxFloat(sqrt(d));
}


Box* mathSqrt(Box* b) {
    double d = _extractFloat(b);
    if (d < 0) {
        raiseExcHelper(ValueError, "math domain error");
    }
    return boxFloat(sqrt(d));
}

Box* mathTanFloat(Box* b) {
    assert(b->cls == float_cls);
    double d = static_cast<BoxedFloat*>(b)->d;
    return boxFloat(tan(d));
}

Box* mathTanInt(Box* b) {
    assert(b->cls == int_cls);
    double d = static_cast<BoxedInt*>(b)->n;
    return boxFloat(tan(d));
}

Box* mathTan(Box* b) {
    double d = _extractFloat(b);
    return boxFloat(tan(d));
}


static void _addFunc(const char* name, void* int_func, void* float_func, void* boxed_func) {
    std::vector<ConcreteCompilerType*> v_i, v_f, v_u;
    assert(BOXED_INT);
    v_i.push_back(BOXED_INT);
    v_f.push_back(BOXED_FLOAT);
    v_u.push_back(UNKNOWN);

    CLFunction* cl = createRTFunction(1, 0, false, false);
    addRTFunction(cl, int_func, BOXED_FLOAT, v_i);
    addRTFunction(cl, float_func, BOXED_FLOAT, v_f);
    addRTFunction(cl, boxed_func, UNKNOWN, v_u);
    math_module->giveAttr(name, new BoxedFunction(cl));
}

void setupMath() {
    math_module = createModule("math", "__builtin__");
    math_module->giveAttr("pi", boxFloat(M_PI));

    _addFunc("sqrt", (void*)mathSqrtInt, (void*)mathSqrtFloat, (void*)mathSqrt);
    _addFunc("tan", (void*)mathTanInt, (void*)mathTanFloat, (void*)mathTan);
}
}
