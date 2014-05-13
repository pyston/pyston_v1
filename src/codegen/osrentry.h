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

#ifndef PYSTON_CODEGEN_OSRENTRY_H
#define PYSTON_CODEGEN_OSRENTRY_H

#include <vector>

namespace llvm {
class Function;
}


namespace pyston {

class StackMap;

class OSREntryDescriptor {
private:
    OSREntryDescriptor(CompiledFunction* from_cf, AST_Jump* backedge) : cf(from_cf), backedge(backedge) {}

public:
    CompiledFunction* const cf;
    AST_Jump* const backedge;
    typedef std::map<std::string, ConcreteCompilerType*> ArgMap;
    ArgMap args;

    static OSREntryDescriptor* create(CompiledFunction* from_cf, AST_Jump* backedge) {
        return new OSREntryDescriptor(from_cf, backedge);
    }
};

class OSRExit {
private:
public:
    CompiledFunction* const parent_cf;
    OSREntryDescriptor* entry;

    OSRExit(CompiledFunction* parent_cf, OSREntryDescriptor* entry) : parent_cf(parent_cf), entry(entry) {}
};
}

#endif
