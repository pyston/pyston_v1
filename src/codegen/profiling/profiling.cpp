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

#include "codegen/profiling/profiling.h"

#include <csignal>
#include <cstdio>

namespace pyston {

typedef llvm::JITEventListener* (*Ctor)();
static Ctor ctors[16];
static int num_ctors = 0;
std::vector<llvm::JITEventListener*> makeJITEventListeners() {
    std::vector<llvm::JITEventListener*> rtn;
    for (int i = 0; i < num_ctors; i++) {
        rtn.push_back(ctors[i]());
    }
    return rtn;
}

void registerProfileListenerCtor(llvm::JITEventListener* (*c)()) {
    assert(num_ctors < sizeof(ctors) / sizeof(ctors[0]));
    ctors[num_ctors] = c;
    num_ctors++;
}
}
