// Copyright (c) 2014-2016 Dropbox, Inc.
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
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

Box* True, *False;

extern "C" PyObject* PyBool_FromLong(long n) noexcept {
    return boxBool(n != 0);
}

extern "C" Box* boolNonzero(BoxedBool* v) {
    return v;
}

extern "C" Box* boolRepr(BoxedBool* v) {
    static BoxedString* true_str = internStringImmortal("True");
    static BoxedString* false_str = internStringImmortal("False");

    if (v == True)
        return true_str;
    return false_str;
}

size_t bool_hash(BoxedBool* v) {
    return v == True;
}

Box* boolHash(BoxedBool* v) {
    return boxInt(bool_hash(v));
}

extern "C" Box* boolNew(Box* cls, Box* val) {
    assert(cls == bool_cls);

    bool b = nonzero(val);
    return boxBool(b);
}

extern "C" Box* boolAnd(BoxedBool* lhs, BoxedBool* rhs) {
    if (!PyBool_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__and__' requires a 'bool' object but received a '%s'",
                       getTypeName(lhs));

    if (!PyBool_Check(rhs))
        return intAnd(lhs, rhs);

    return boxBool(lhs->n && rhs->n);
}

extern "C" Box* boolOr(BoxedBool* lhs, BoxedBool* rhs) {
    if (!PyBool_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__or__' requires a 'bool' object but received a '%s'", getTypeName(lhs));

    if (!PyBool_Check(rhs))
        return intOr(lhs, rhs);

    return boxBool(lhs->n || rhs->n);
}

extern "C" Box* boolXor(BoxedBool* lhs, BoxedBool* rhs) {
    if (!PyBool_Check(lhs))
        raiseExcHelper(TypeError, "descriptor '__xor__' requires a 'bool' object but received a '%s'",
                       getTypeName(lhs));

    if (!PyBool_Check(rhs))
        return intXor(lhs, rhs);

    return boxBool(lhs->n ^ rhs->n);
}


void setupBool() {
    static PyNumberMethods bool_as_number;
    bool_cls->tp_as_number = &bool_as_number;

    bool_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)boolNonzero, BOXED_BOOL, 1)));
    bool_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)boolRepr, STR, 1)));
    bool_cls->giveAttr("__hash__", new BoxedFunction(FunctionMetadata::create((void*)boolHash, BOXED_INT, 1)));

    bool_cls->giveAttr("__new__",
                       new BoxedFunction(FunctionMetadata::create((void*)boolNew, UNKNOWN, 2, false, false), { None }));

    // TODO: type specialize
    bool_cls->giveAttr("__and__", new BoxedFunction(FunctionMetadata::create((void*)boolAnd, UNKNOWN, 2)));
    bool_cls->giveAttr("__or__", new BoxedFunction(FunctionMetadata::create((void*)boolOr, UNKNOWN, 2)));
    bool_cls->giveAttr("__xor__", new BoxedFunction(FunctionMetadata::create((void*)boolXor, UNKNOWN, 2)));

    bool_cls->freeze();
    bool_cls->tp_hash = (hashfunc)bool_hash;
}

void teardownBool() {
}
}
