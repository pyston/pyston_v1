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

template <typename T> class VRegMap {
private:
    // TODO: switch just to a T*
    std::vector<T> v;

public:
    VRegMap(int num_vregs) : v(num_vregs) {}

    T& operator[](int vreg) {
        assert(vreg >= 0 && vreg < v.size());
        return v[vreg];
    }

    const T& operator[](int vreg) const {
        assert(vreg >= 0 && vreg < v.size());
        return v[vreg];
    }

    class iterator {
    public:
        const VRegMap<T>& map;
        int i;
        iterator(const VRegMap<T>& map, int i) : map(map), i(i) {}

        iterator& operator++() {
            i++;
            return *this;
        }

        bool operator==(const iterator& rhs) const { return i == rhs.i; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

        std::pair<int, const T&> operator*() { return std::pair<int, const T&>(i, map[i]); }
    };

    int numVregs() const { return v.size(); }

    iterator begin() const { return iterator(*this, 0); }

    iterator end() const { return iterator(*this, this->v.size()); }
};

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
    bool isKill(AST_Name* node, CFGBlock* parent_block);

    bool isLiveAtEnd(int vreg, CFGBlock* block);
};

class PhiAnalysis;

class VRegSet {
private:
    // TODO: switch just to a bool*
    std::vector<bool> v;

public:
    VRegSet(int num_vregs) : v(num_vregs, false) {}

    // TODO: what is the referenc type here?
    bool operator[](int vreg) {
        assert(vreg >= 0 && vreg < v.size());
        return v[vreg];
    }
    void set(int vreg) {
        assert(vreg >= 0 && vreg < v.size());
        v[vreg] = true;
    }

    class iterator {
    public:
        const VRegSet& set;
        int i;
        iterator(const VRegSet& set, int i) : set(set), i(i) {}

        iterator& operator++() {
            do {
                i++;
            } while (i < set.v.size() && !set.v[i]);
            return *this;
        }

        bool operator==(const iterator& rhs) const { return i == rhs.i; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

        int operator*() { return i; }
    };

    iterator begin() const {
        for (int i = 0; i < v.size(); i++) {
            if (v[i])
                return iterator(*this, i);
        }
        return iterator(*this, this->v.size());
    }

    iterator end() const { return iterator(*this, this->v.size()); }
};

class DefinednessAnalysis {
public:
    enum DefinitionLevel {
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

    void run(VRegMap<DefinitionLevel> initial_map, CFGBlock* initial_block, ScopeInfo* scope_info);

    DefinitionLevel isDefinedAtEnd(InternedString name, CFGBlock* block);
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
                bool initials_need_phis, LivenessAnalysis* liveness, ScopeInfo* scope_info);

    // TODO: convert these to taking vregs
    bool isRequired(InternedString name, CFGBlock* block);
    bool isRequiredAfter(InternedString name, CFGBlock* block);
    const VRegSet& getAllRequiredAfter(CFGBlock* block);
    const VRegSet& getAllRequiredFor(CFGBlock* block);
    // TODO: convert these to taking vregs
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
