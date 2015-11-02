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

#ifndef PYSTON_RUNTIME_CODE_H
#define PYSTON_RUNTIME_CODE_H

#include "Python.h"

#include "runtime/types.h"

namespace pyston {

class BoxedCode : public Box {
public:
    FunctionMetadata* f;

    BoxedCode(FunctionMetadata* f) : f(f) {}

    DEFAULT_CLASS(code_cls);

    static void gcHandler(GCVisitor* v, Box* b);

    // These need to be static functions rather than methods because function
    // pointers could point to them.
    static Box* name(Box* b, void*);
    static Box* filename(Box* b, void*);
    static Box* firstlineno(Box* b, void*);
    static Box* argcount(Box* b, void*);
    static Box* varnames(Box* b, void*);
    static Box* flags(Box* b, void*);
};
}

#endif
