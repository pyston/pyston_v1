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

#include "analysis/function_analysis.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_set>

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"

#include "analysis/fpc.h"
#include "analysis/scoping_analysis.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/util.h"

namespace pyston {

class LivenessBBVisitor : public NoopASTVisitor {
public:
    typedef llvm::SmallSet<std::string, 4> StrSet;

private:
    StrSet _loads;
    StrSet _stores;

    void _doLoad(const std::string& name) {
        if (_stores.count(name))
            return;
        _loads.insert(name);
    }
    void _doStore(const std::string& name) {
        if (_loads.count(name))
            return;
        _stores.insert(name);
    }

public:
    LivenessBBVisitor() {}
    const StrSet& loads() { return _loads; }
    const StrSet& stores() { return _stores; }

    bool visit_classdef(AST_ClassDef* node) {
        _doStore(node->name);
        return true;
    }
    bool visit_functiondef(AST_FunctionDef* node) {
        _doStore(node->name);
        return true;
    }
    bool visit_name(AST_Name* node) {
        if (node->ctx_type == AST_TYPE::Load)
            _doLoad(node->id);
        else if (node->ctx_type == AST_TYPE::Store)
            _doStore(node->id);
        else {
            assert(0);
            abort();
        }
        return true;
    }
    bool visit_alias(AST_alias* node) {
        const std::string* name = &node->name;
        if (node->asname.size())
            name = &node->asname;

        _doStore(*name);
        return true;
    }
};

bool LivenessAnalysis::isLiveAtEnd(const std::string& name, CFGBlock* block) {
    if (block->successors.size() == 0)
        return false;

    // Very inefficient liveness analysis:
    // for each query, trace forward through all possible control flow paths.
    // if we hit a store to the name, stop tracing that path
    // if we hit a load to the name, return true.
    // to improve performance we cache the liveness result of every visited BB.
    llvm::SmallPtrSet<CFGBlock*, 1> visited;
    std::deque<CFGBlock*> q;
    for (CFGBlock* successor : block->successors) {
        q.push_back(successor);
    }

    while (q.size()) {
        CFGBlock* thisblock = q.front();
        q.pop_front();
        if (visited.count(thisblock))
            continue;

        LivenessBBVisitor* visitor = nullptr;
        LivenessCacheMap::iterator it = livenessCache.find(thisblock);
        if (it != livenessCache.end()) {
            visitor = it->second.get();
        } else {
            visitor = new LivenessBBVisitor; // livenessCache unique_ptr will delete it.
            for (AST_stmt* stmt : thisblock->body) {
                stmt->accept(visitor);
            }
            livenessCache.insert(std::make_pair(thisblock, std::unique_ptr<LivenessBBVisitor>(visitor)));
        }
        visited.insert(thisblock);

        if (visitor->loads().count(name)) {
            assert(!visitor->stores().count(name));
            return true;
        }

        if (!visitor->stores().count(name)) {
            assert(!visitor->loads().count(name));
            for (CFGBlock* successor : thisblock->successors) {
                q.push_back(successor);
            }
        }
    }

    return false;
}

class DefinednessBBAnalyzer : public BBAnalyzer<DefinednessAnalysis::DefinitionLevel> {
private:
    typedef DefinednessAnalysis::DefinitionLevel DefinitionLevel;

    CFG* cfg;
    AST_arguments* arguments;

public:
    DefinednessBBAnalyzer(CFG* cfg, AST_arguments* arguments) : cfg(cfg), arguments(arguments) {}

    virtual DefinitionLevel merge(DefinitionLevel from, DefinitionLevel into) const {
        assert(from != DefinednessAnalysis::Undefined);
        assert(into != DefinednessAnalysis::Undefined);
        if (from == DefinednessAnalysis::PotentiallyDefined || into == DefinednessAnalysis::PotentiallyDefined)
            return DefinednessAnalysis::PotentiallyDefined;
        return DefinednessAnalysis::Defined;
    }
    virtual void processBB(Map& starting, CFGBlock* block) const;
    virtual DefinitionLevel mergeBlank(DefinitionLevel into) const {
        assert(into != DefinednessAnalysis::Undefined);
        return DefinednessAnalysis::PotentiallyDefined;
    }
};

class DefinednessVisitor : public ASTVisitor {
private:
    typedef DefinednessBBAnalyzer::Map Map;
    Map& state;

    void _doSet(const std::string& s) { state[s] = DefinednessAnalysis::Defined; }

    void _doSet(AST* t) {
        switch (t->type) {
            case AST_TYPE::Attribute:
                // doesn't affect definedness (yet?)
                break;
            case AST_TYPE::Name:
                _doSet(((AST_Name*)t)->id);
                break;
            case AST_TYPE::Subscript:
                break;
            case AST_TYPE::Tuple: {
                AST_Tuple* tt = ast_cast<AST_Tuple>(t);
                for (int i = 0; i < tt->elts.size(); i++) {
                    _doSet(tt->elts[i]);
                }
                break;
            }
            default:
                ASSERT(0, "Unknown type for DefinednessVisitor: %d", t->type);
        }
    }

public:
    DefinednessVisitor(Map& state) : state(state) {}

    virtual bool visit_assert(AST_Assert* node) { return true; }
    virtual bool visit_branch(AST_Branch* node) { return true; }
    virtual bool visit_delete(AST_Delete* node) { return true; }
    virtual bool visit_expr(AST_Expr* node) { return true; }
    virtual bool visit_global(AST_Global* node) { return true; }
    virtual bool visit_invoke(AST_Invoke* node) { return false; }
    virtual bool visit_jump(AST_Jump* node) { return true; }
    virtual bool visit_pass(AST_Pass* node) { return true; }
    virtual bool visit_print(AST_Print* node) { return true; }
    virtual bool visit_raise(AST_Raise* node) { return true; }
    virtual bool visit_return(AST_Return* node) { return true; }
    virtual bool visit_unreachable(AST_Unreachable* node) { return true; }

    virtual bool visit_classdef(AST_ClassDef* node) {
        _doSet(node->name);
        return true;
    }

    virtual bool visit_functiondef(AST_FunctionDef* node) {
        _doSet(node->name);
        return true;
    }

    virtual bool visit_alias(AST_alias* node) {
        const std::string* name = &node->name;
        if (node->asname.size())
            name = &node->asname;

        _doSet(*name);
        return true;
    }
    virtual bool visit_import(AST_Import* node) { return false; }
    virtual bool visit_importfrom(AST_ImportFrom* node) { return false; }

    virtual bool visit_assign(AST_Assign* node) {
        for (int i = 0; i < node->targets.size(); i++) {
            _doSet(node->targets[i]);
        }
        return true;
    }

    virtual bool visit_arguments(AST_arguments* node) {
        if (node->kwarg)
            _doSet(node->kwarg);
        if (node->vararg.size())
            _doSet(node->vararg);
        for (int i = 0; i < node->args.size(); i++) {
            _doSet(node->args[i]);
        }
        return true;
    }
};

void DefinednessBBAnalyzer::processBB(Map& starting, CFGBlock* block) const {
    DefinednessVisitor visitor(starting);
    for (int i = 0; i < block->body.size(); i++) {
        block->body[i]->accept(&visitor);
    }
    if (block == cfg->getStartingBlock() && arguments) {
        arguments->accept(&visitor);
    }

    if (VERBOSITY("analysis") >= 2) {
        printf("At end of block %d:\n", block->idx);
        for (const auto& p : starting) {
            printf("%s: %d\n", p.first.c_str(), p.second);
        }
    }
}

DefinednessAnalysis::DefinednessAnalysis(AST_arguments* args, CFG* cfg, ScopeInfo* scope_info)
    : scope_info(scope_info) {
    results = computeFixedPoint(cfg, DefinednessBBAnalyzer(cfg, args), false);

    for (const auto& p : results) {
        RequiredSet required;
        for (const auto& p2 : p.second) {
            if (scope_info->refersToGlobal(p2.first))
                continue;

            // printf("%d %s %d\n", p.first->idx, p2.first.c_str(), p2.second);
            required.insert(p2.first);
        }
        defined.insert(make_pair(p.first, required));
    }
}

DefinednessAnalysis::DefinitionLevel DefinednessAnalysis::isDefinedAt(const std::string& name, CFGBlock* block) {
    std::unordered_map<std::string, DefinitionLevel>& map = results[block];
    if (map.count(name) == 0)
        return Undefined;
    return map[name];
}

const DefinednessAnalysis::RequiredSet& DefinednessAnalysis::getDefinedNamesAt(CFGBlock* block) {
    return defined[block];
}

PhiAnalysis::PhiAnalysis(AST_arguments* args, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo* scope_info)
    : definedness(args, cfg, scope_info), liveness(liveness) {
    for (CFGBlock* block : cfg->blocks) {
        RequiredSet required;
        if (block->predecessors.size() < 2)
            continue;

        const RequiredSet& defined = definedness.getDefinedNamesAt(block);
        if (defined.size())
            assert(block->predecessors.size());
        for (const auto& s : defined) {
            if (liveness->isLiveAtEnd(s, block->predecessors[0])) {
                required.insert(s);
            }
        }

        required_phis.insert(make_pair(block, required));
    }
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllRequiredAfter(CFGBlock* block) {
    static RequiredSet empty;
    if (block->successors.size() == 0)
        return empty;
    return required_phis[block->successors[0]];
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllDefinedAt(CFGBlock* block) {
    return definedness.getDefinedNamesAt(block);
}

bool PhiAnalysis::isRequired(const std::string& name, CFGBlock* block) {
    assert(!startswith(name, "!"));
    return required_phis[block].count(name) != 0;
}

bool PhiAnalysis::isRequiredAfter(const std::string& name, CFGBlock* block) {
    assert(!startswith(name, "!"));
    // If there are multiple successors, then none of them are allowed
    // to require any phi nodes
    if (block->successors.size() != 1)
        return false;

    // Fall back to the other method:
    return isRequired(name, block->successors[0]);
}

bool PhiAnalysis::isPotentiallyUndefinedAfter(const std::string& name, CFGBlock* block) {
    assert(!startswith(name, "!"));
    assert(block->successors.size() > 0);
    DefinednessAnalysis::DefinitionLevel dlevel = definedness.isDefinedAt(name, block->successors[0]);
    ASSERT(dlevel != DefinednessAnalysis::Undefined, "%s %d", name.c_str(), block->idx);

    return dlevel == DefinednessAnalysis::PotentiallyDefined;
}

LivenessAnalysis* computeLivenessInfo(CFG*) {
    return new LivenessAnalysis();
}

PhiAnalysis* computeRequiredPhis(AST_arguments* args, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo* scope_info) {
    return new PhiAnalysis(args, cfg, liveness, scope_info);
}
}
