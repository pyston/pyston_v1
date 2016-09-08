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

#include "core/cfg.h"
#include "core/stringpool.h"
#include "core/types.h"

namespace pyston {

class BST_arguments;
class BST_Jump;
class BST_Name;
class CFG;
class CFGBlock;
class LivenessBBVisitor;

class LivenessAnalysis {
private:
    CFG* cfg;

    friend class LivenessBBVisitor;
    typedef llvm::DenseMap<CFGBlock*, std::unique_ptr<LivenessBBVisitor>> LivenessCacheMap;
    LivenessCacheMap liveness_cache;

    VRegMap<llvm::DenseMap<CFGBlock*, bool>> result_cache;

public:
    LivenessAnalysis(CFG* cfg);
    ~LivenessAnalysis();

    // we don't keep track of node->parent_block relationships, so you have to pass both:
    bool isKill(BST_Name* node, CFGBlock* parent_block);

    bool isLiveAtEnd(int vreg, CFGBlock* block);
};

class PhiAnalysis;

class DefinednessAnalysis {
public:
    enum DefinitionLevel : char {
        Unknown,
        Undefined,
        PotentiallyDefined,
        Defined,
    };

private:
    llvm::DenseMap<CFGBlock*, VRegMap<DefinitionLevel>> defined_at_beginning, defined_at_end;
    llvm::DenseMap<CFGBlock*, VRegSet> defined_at_end_sets;

public:
    DefinednessAnalysis() {}

    void run(VRegMap<DefinitionLevel> initial_map, CFGBlock* initial_block);

    DefinitionLevel isDefinedAtEnd(int vreg, CFGBlock* block);
    const VRegSet& getDefinedVregsAtEnd(CFGBlock* block);

    friend class PhiAnalysis;
};

class PhiAnalysis {
public:
    DefinednessAnalysis definedness;

    VRegSet empty_set;

private:
    LivenessAnalysis* liveness;
    llvm::DenseMap<CFGBlock*, VRegSet> required_phis;

public:
    // Initials_need_phis specifies that initial_map should count as an additional entry point
    // that may require phis.
    PhiAnalysis(VRegMap<DefinednessAnalysis::DefinitionLevel> initial_map, CFGBlock* initial_block,
                bool initials_need_phis, LivenessAnalysis* liveness);

    bool isRequired(int vreg, CFGBlock* block);
    bool isRequiredAfter(int vreg, CFGBlock* block);
    const VRegSet& getAllRequiredAfter(CFGBlock* block);
    const VRegSet& getAllRequiredFor(CFGBlock* block);
    // If "name" may be undefined at the beginning of any immediate successor block of "block":
    bool isPotentiallyUndefinedAfter(int vreg, CFGBlock* block);
    // If "name" may be undefined at the beginning of "block"
    bool isPotentiallyUndefinedAt(int vreg, CFGBlock* block);
};

std::unique_ptr<LivenessAnalysis> computeLivenessInfo(CFG*);
std::unique_ptr<PhiAnalysis> computeRequiredPhis(const ParamNames&, CFG*, LivenessAnalysis*);
std::unique_ptr<PhiAnalysis> computeRequiredPhis(const OSREntryDescriptor*, LivenessAnalysis*);
}

#endif
