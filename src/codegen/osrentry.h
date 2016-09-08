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

#ifndef PYSTON_CODEGEN_OSRENTRY_H
#define PYSTON_CODEGEN_OSRENTRY_H

#include <map>
#include <vector>

#include "core/cfg.h"
#include "core/stringpool.h"
#include "runtime/types.h"

namespace llvm {
class Function;
}


namespace pyston {

struct StackMap;

class OSREntryDescriptor {
private:
    OSREntryDescriptor(BoxedCode* code, BST_Jump* backedge, ExceptionStyle exception_style)
        : code(code),
          backedge(backedge),
          exception_style(exception_style),
          args(code->source->cfg->getVRegInfo().getTotalNumOfVRegs()),
          potentially_undefined(code->source->cfg->getVRegInfo().getTotalNumOfVRegs()) {
        assert(code);
    }

public:
    BoxedCode* code;
    BST_Jump* const backedge;
    ExceptionStyle exception_style;
    typedef VRegMap<ConcreteCompilerType*> ArgMap;
    ArgMap args;
    VRegSet potentially_undefined;

    static OSREntryDescriptor* create(BoxedCode* code, BST_Jump* backedge, ExceptionStyle exception_style) {
        return new OSREntryDescriptor(code, backedge, exception_style);
    }
};

class OSRExit {
private:
public:
    const OSREntryDescriptor* entry;

    OSRExit(const OSREntryDescriptor* entry) : entry(entry) {}
};
}

#endif
