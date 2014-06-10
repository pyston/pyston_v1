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

#include "codegen/compvars.h"
#include "core/common.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

Box* True, *False;

extern "C" Box* boolInvert(BoxedBool* v) {
    return boxInt(~v->b);
}

extern "C" Box* boolPos(BoxedBool* v) {
    return boxInt(v->b ? 1 : 0);
}

extern "C" Box* boolNeg(BoxedBool* v) {
    return boxInt(v->b ? -1 : 0);
}

extern "C" Box* boolNonzero(BoxedBool* v) {
    return v;
}

extern "C" Box* boolRepr(BoxedBool* v) {
    if (v->b)
        return boxStrConstant("True");
    return boxStrConstant("False");
}

extern "C" Box* boolNew(Box* cls, Box* val) {
    assert(cls == bool_cls);

    bool b = nonzero(val);
    return boxBool(b);
}

void setupBool() {
    bool_cls->giveAttr("__name__", boxStrConstant("bool"));

    bool_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)boolInvert, BOXED_INT, 1)));
    bool_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)boolPos, BOXED_INT, 1)));
    bool_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)boolNeg, BOXED_INT, 1)));
    bool_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)boolNonzero, BOXED_BOOL, 1)));
    bool_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)boolRepr, STR, 1)));
    bool_cls->giveAttr("__str__", bool_cls->getattr("__repr__"));

    bool_cls->giveAttr("__new__",
                       new BoxedFunction(boxRTFunction((void*)boolNew, UNKNOWN, 2, 1, false, false), { None }));


    bool_cls->freeze();

    True = new BoxedBool(true);
    False = new BoxedBool(false);

    gc::registerStaticRootObj(True);
    gc::registerStaticRootObj(False);
}

void teardownBool() {
}
}
