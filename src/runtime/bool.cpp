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

#include "core/common.h"
#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

#include "gc/collector.h"

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

extern "C" Box* boolNew1(Box* cls) {
    assert(cls == bool_cls);
    return False;
}

extern "C" Box* boolNew2(Box* cls, Box* val) {
    assert(cls == bool_cls);

    bool b = nonzero(val);
    return boxBool(b);
}

void setupBool() {
    bool_cls->giveAttr("__name__", boxStrConstant("bool"));

    bool_cls->giveAttr("__invert__", new BoxedFunction(boxRTFunction((void*)boolInvert, NULL, 1, false)));
    bool_cls->giveAttr("__pos__", new BoxedFunction(boxRTFunction((void*)boolPos, NULL, 1, false)));
    bool_cls->giveAttr("__neg__", new BoxedFunction(boxRTFunction((void*)boolNeg, NULL, 1, false)));
    bool_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)boolNonzero, NULL, 1, false)));
    bool_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)boolRepr, NULL, 1, false)));
    bool_cls->setattr("__str__", bool_cls->peekattr("__repr__"), NULL, NULL);

    CLFunction* __new__ = boxRTFunction((void*)boolNew1, NULL, 1, false);
    addRTFunction(__new__, (void*)boolNew2, NULL, 2, false);
    bool_cls->giveAttr("__new__", new BoxedFunction(__new__));


    bool_cls->freeze();

    True = new BoxedBool(true);
    False = new BoxedBool(false);

    gc::registerStaticRootObj(True);
    gc::registerStaticRootObj(False);
}

void teardownBool() {
}
}
