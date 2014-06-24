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

#ifndef PYSTON_ANALYSIS_TYPEANALYSIS_H
#define PYSTON_ANALYSIS_TYPEANALYSIS_H

#include <unordered_map>
#include <vector>

#include "codegen/compvars.h"
#include "core/types.h"

namespace pyston {

class ScopeInfo;
class CFGBlock;
class BoxedClass;
class AST_expr;

class TypeAnalysis {
public:
    enum SpeculationLevel {
        NONE,
        SOME,
    };

    virtual ~TypeAnalysis() {}

    virtual ConcreteCompilerType* getTypeAtBlockStart(const std::string& name, CFGBlock* block) = 0;
    virtual ConcreteCompilerType* getTypeAtBlockEnd(const std::string& name, CFGBlock* block) = 0;
    virtual BoxedClass* speculatedExprClass(AST_expr*) = 0;
};

// TypeAnalysis* analyze(CFG *cfg, std::unordered_map<std::string, ConcreteCompilerType*> arg_types);
TypeAnalysis* doTypeAnalysis(CFG* cfg, const SourceInfo::ArgNames& arg_names,
                             const std::vector<ConcreteCompilerType*>& arg_types,
                             TypeAnalysis::SpeculationLevel speculation, ScopeInfo* scope_info);
}

#endif
