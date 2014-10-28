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

#ifndef PYSTON_ANALYSIS_FUNCTIONANALYSIS_H
#define PYSTON_ANALYSIS_FUNCTIONANALYSIS_H

#include <memory>
#include <unordered_map>
#include <unordered_set>

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
    typedef std::unordered_map<CFGBlock*, std::unique_ptr<LivenessBBVisitor> > LivenessCacheMap;
    LivenessCacheMap liveness_cache;

    std::unordered_map<int, std::unordered_map<CFGBlock*, bool> > result_cache;

    // Map strings to unique indices.  For a given CFG, the set of strings should be fairly small
    // (a constant fraction max of the CFG itself), so just store all of them.  The theory is that
    // for any particular name, we will do many lookups on it in different hash tables, and by
    // converting to a string only once, the extra hashtable lookup will be profitable since it
    // can make all the rest faster (int hashes vs string hashes).
    //
    // Haven't validated this, though.
    std::unordered_map<std::string, int> string_index_map;
    int getStringIndex(const std::string& s);

public:
    LivenessAnalysis(CFG* cfg);

    // we don't keep track of node->parent_block relationships, so you have to pass both:
    bool isKill(AST_Name* node, CFGBlock* parent_block);

    bool isLiveAtEnd(const std::string& name, CFGBlock* block);
};

class DefinednessAnalysis {
public:
    enum DefinitionLevel {
        Undefined,
        PotentiallyDefined,
        Defined,
    };
    typedef std::unordered_set<std::string> RequiredSet;

private:
    std::unordered_map<CFGBlock*, std::unordered_map<std::string, DefinitionLevel> > results;
    std::unordered_map<CFGBlock*, const RequiredSet> defined_at_end;
    ScopeInfo* scope_info;

public:
    DefinednessAnalysis(const SourceInfo::ArgNames& args, CFG* cfg, ScopeInfo* scope_info);

    DefinitionLevel isDefinedAtEnd(const std::string& name, CFGBlock* block);
    const RequiredSet& getDefinedNamesAtEnd(CFGBlock* block);
};
class PhiAnalysis {
public:
    typedef std::unordered_set<std::string> RequiredSet;

private:
    DefinednessAnalysis definedness;
    LivenessAnalysis* liveness;
    std::unordered_map<CFGBlock*, const RequiredSet> required_phis;

public:
    PhiAnalysis(const SourceInfo::ArgNames&, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo* scope_info);

    bool isRequired(const std::string& name, CFGBlock* block);
    bool isRequiredAfter(const std::string& name, CFGBlock* block);
    const RequiredSet& getAllRequiredAfter(CFGBlock* block);
    const RequiredSet& getAllRequiredFor(CFGBlock* block);
    bool isPotentiallyUndefinedAfter(const std::string& name, CFGBlock* block);
};

LivenessAnalysis* computeLivenessInfo(CFG*);
PhiAnalysis* computeRequiredPhis(const SourceInfo::ArgNames&, CFG*, LivenessAnalysis*, ScopeInfo* scope_Info);
}

#endif
