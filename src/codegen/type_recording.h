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

#ifndef PYSTON_CODEGEN_TYPERECORDING_H
#define PYSTON_CODEGEN_TYPERECORDING_H

#include <cstdint>

namespace pyston {

class AST;
class Box;
class BoxedClass;

class TypeRecorder;
// Have this be a non-function-scoped friend function;
// the benefit of doing it this way, as opposed to being a member function,
// is that we can extern "C" and get a guaranteed ABI, for calling from the patchpoints.
// (I think most compilers pass "this" as the argument 0 and then shift the rest of the
// arguments, but I'd rather not depend on that behavior since I can't find where that's
// specified.)
// The return value of this function is 'obj' for ease of use.
extern "C" Box* recordType(TypeRecorder* recorder, Box* obj);
class TypeRecorder {
public:
    BoxedClass* last_seen;
    int64_t last_count;

    constexpr TypeRecorder() : last_seen(nullptr), last_count(0) {}

    BoxedClass* predict();

    friend Box* recordType(TypeRecorder*, Box*);
};

TypeRecorder* getTypeRecorderForNode(AST* node);

BoxedClass* predictClassFor(AST* node);
}

#endif
