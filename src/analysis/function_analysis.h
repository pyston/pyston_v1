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

#ifndef PYSTON_ANALYSIS_FUNCTIONANALYSIS_H
#define PYSTON_ANALYSIS_FUNCTIONANALYSIS_H

#include <memory>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include "core/stringpool.h"
#include "core/types.h"

namespace pyston {

class AST_arguments;
class AST_Jump;
class AST_Name;
class CFG;
class CFGBlock;
class ScopeInfo;
class LivenessBBVisitor;

class LivenessAnalysis {
private:
    CFG* cfg;

    friend class LivenessBBVisitor;
    typedef llvm::DenseMap<CFGBlock*, std::unique_ptr<LivenessBBVisitor>> LivenessCacheMap;
    LivenessCacheMap liveness_cache;

    llvm::DenseMap<InternedString, llvm::DenseMap<CFGBlock*, bool>> result_cache;

public:
    LivenessAnalysis(CFG* cfg);
    ~LivenessAnalysis();

    // we don't keep track of node->parent_block relationships, so you have to pass both:
    bool isKill(AST_Name* node, CFGBlock* parent_block);

    bool isLiveAtEnd(InternedString name, CFGBlock* block);
};

class PhiAnalysis;

class DefinednessAnalysis {
public:
    enum DefinitionLevel {
        Unknown,
        Undefined,
        PotentiallyDefined,
        Defined,
    };
    typedef llvm::DenseSet<InternedString> RequiredSet;

private:
    llvm::DenseMap<CFGBlock*, llvm::DenseMap<InternedString, DefinitionLevel>> defined_at_beginning, defined_at_end;
    llvm::DenseMap<CFGBlock*, RequiredSet> defined_at_end_sets;

public:
    DefinednessAnalysis() {}

    void run(llvm::DenseMap<InternedString, DefinitionLevel> initial_map, CFGBlock* initial_block,
             ScopeInfo* scope_info);

    DefinitionLevel isDefinedAtEnd(InternedString name, CFGBlock* block);
    const RequiredSet& getDefinedNamesAtEnd(CFGBlock* block);

    friend class PhiAnalysis;
};

class PhiAnalysis {
public:
    typedef llvm::DenseSet<InternedString> RequiredSet;

    DefinednessAnalysis definedness;

private:
    LivenessAnalysis* liveness;
    llvm::DenseMap<CFGBlock*, RequiredSet> required_phis;

public:
    // Initials_need_phis specifies that initial_map should count as an additional entry point
    // that may require phis.
    PhiAnalysis(llvm::DenseMap<InternedString, DefinednessAnalysis::DefinitionLevel> initial_map,
                CFGBlock* initial_block, bool initials_need_phis, LivenessAnalysis* liveness, ScopeInfo* scope_info);

    bool isRequired(InternedString name, CFGBlock* block);
    bool isRequiredAfter(InternedString name, CFGBlock* block);
    const RequiredSet& getAllRequiredAfter(CFGBlock* block);
    const RequiredSet& getAllRequiredFor(CFGBlock* block);
    // If "name" may be undefined at the beginning of any immediate successor block of "block":
    bool isPotentiallyUndefinedAfter(InternedString name, CFGBlock* block);
    // If "name" may be undefined at the beginning of "block"
    bool isPotentiallyUndefinedAt(InternedString name, CFGBlock* block);
};

std::unique_ptr<LivenessAnalysis> computeLivenessInfo(CFG*);
std::unique_ptr<PhiAnalysis> computeRequiredPhis(const ParamNames&, CFG*, LivenessAnalysis*, ScopeInfo* scope_info);
std::unique_ptr<PhiAnalysis> computeRequiredPhis(const OSREntryDescriptor*, LivenessAnalysis*, ScopeInfo* scope_info);
}

#endif
