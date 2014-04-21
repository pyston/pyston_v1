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

#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/types.h"
#include "runtime/util.h"
#include "runtime/inline/boxing.h"

namespace pyston {

BoxedModule* math_module;

static double _extractFloat(Box* b) {
    if (b->cls != int_cls && b->cls != float_cls) {
        fprintf(stderr, "TypeError: a float is required\n");
        raiseExc();
    }

    if (b->cls == int_cls)
        return static_cast<BoxedInt*>(b)->n;
    else
        return static_cast<BoxedFloat*>(b)->d;
}

Box* mathSqrt(Box* b) {
    double d = _extractFloat(b);
    if (d < 0) {
        fprintf(stderr, "ValueError: math domain error\n");
        raiseExc();
    }

    double r = sqrt(d);

    return boxFloat(r);
}

Box* mathTan(Box* b) {
    double d = _extractFloat(b);
    return boxFloat(tan(d));
}

void setupMath() {
    std::string name("math");
    std::string fn("__builtin__");
    math_module = new BoxedModule(&name, &fn);

    math_module->giveAttr("pi", boxFloat(M_PI));

    math_module->giveAttr("sqrt", new BoxedFunction(boxRTFunction((void*)mathSqrt, NULL, 1, false)));
    math_module->giveAttr("tan", new BoxedFunction(boxRTFunction((void*)mathTan, NULL, 1, false)));
}

}
