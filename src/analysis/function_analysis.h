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

#ifndef PYSTON_ANALYSIS_FUNCTIONANALYSIS_H
#define PYSTON_ANALYSIS_FUNCTIONANALYSIS_H

#include <memory>
#include <unordered_map>
#include <unordered_set>

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
    typedef std::unordered_map<CFGBlock*, std::unique_ptr<LivenessBBVisitor>> LivenessCacheMap;
    LivenessCacheMap liveness_cache;

    std::unordered_map<InternedString, std::unordered_map<CFGBlock*, bool>> result_cache;

public:
    LivenessAnalysis(CFG* cfg);

    // we don't keep track of node->parent_block relationships, so you have to pass both:
    bool isKill(AST_Name* node, CFGBlock* parent_block);

    bool isLiveAtEnd(InternedString name, CFGBlock* block);
};

class DefinednessAnalysis {
public:
    enum DefinitionLevel {
        Undefined,
        PotentiallyDefined,
        Defined,
    };
    typedef std::unordered_set<InternedString> RequiredSet;

private:
    std::unordered_map<CFGBlock*, std::unordered_map<InternedString, DefinitionLevel>> results;
    std::unordered_map<CFGBlock*, const RequiredSet> defined_at_end;
    ScopeInfo* scope_info;

public:
    DefinednessAnalysis(const ParamNames& param_names, CFG* cfg, ScopeInfo* scope_info);

    DefinitionLevel isDefinedAtEnd(InternedString name, CFGBlock* block);
    const RequiredSet& getDefinedNamesAtEnd(CFGBlock* block);
};
class PhiAnalysis {
public:
    typedef std::unordered_set<InternedString> RequiredSet;

    DefinednessAnalysis definedness;

private:
    LivenessAnalysis* liveness;
    std::unordered_map<CFGBlock*, const RequiredSet> required_phis;

public:
    PhiAnalysis(const ParamNames&, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo* scope_info);

    bool isRequired(InternedString name, CFGBlock* block);
    bool isRequiredAfter(InternedString name, CFGBlock* block);
    const RequiredSet& getAllRequiredAfter(CFGBlock* block);
    const RequiredSet& getAllRequiredFor(CFGBlock* block);
    bool isPotentiallyUndefinedAfter(InternedString name, CFGBlock* block);
    bool isPotentiallyUndefinedAt(InternedString name, CFGBlock* block);
};

LivenessAnalysis* computeLivenessInfo(CFG*);
PhiAnalysis* computeRequiredPhis(const ParamNames&, CFG*, LivenessAnalysis*, ScopeInfo* scope_Info);
}

#endif
