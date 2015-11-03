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

#ifndef PYSTON_CODEGEN_OSRENTRY_H
#define PYSTON_CODEGEN_OSRENTRY_H

#include <map>
#include <vector>

#include "core/stringpool.h"

namespace llvm {
class Function;
}


namespace pyston {

struct StackMap;

class OSREntryDescriptor {
private:
    OSREntryDescriptor(FunctionMetadata* md, AST_Jump* backedge, ExceptionStyle exception_style)
        : md(md), backedge(backedge), exception_style(exception_style) {
        assert(md);
    }

public:
    FunctionMetadata* md;
    AST_Jump* const backedge;
    ExceptionStyle exception_style;
    typedef std::map<InternedString, ConcreteCompilerType*> ArgMap;
    ArgMap args;

    static OSREntryDescriptor* create(FunctionMetadata* md, AST_Jump* backedge, ExceptionStyle exception_style) {
        return new OSREntryDescriptor(md, backedge, exception_style);
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
