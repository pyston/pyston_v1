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
class CFG;
class CFGBlock;
class ScopeInfo;
class LivenessBBVisitor;

class LivenessAnalysis {
public:
    bool isLiveAtEnd(const std::string& name, CFGBlock* block);

private:
    typedef std::unordered_map<CFGBlock*, std::unique_ptr<LivenessBBVisitor> > LivenessCacheMap;
    LivenessCacheMap livenessCache;
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
