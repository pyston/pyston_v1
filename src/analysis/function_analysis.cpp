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
private:
    struct Status {
        enum Usage {
            NONE,
            USED,
            DEFINED,
        };

        Usage first = NONE;
        Usage second = NONE;

        void addUsage(Usage u) {
            if (first == NONE)
                first = u;
            second = u;
        }
    };

    std::unordered_set<AST_Name*> kills;
    std::unordered_map<InternedString, AST_Name*> last_uses;

    std::unordered_map<InternedString, Status> statuses;
    LivenessAnalysis* analysis;

    void _doLoad(InternedString name, AST_Name* node) {
        Status& status = statuses[name];
        status.addUsage(Status::USED);

        last_uses[name] = node;
    }

    void _doStore(InternedString name) {
        Status& status = statuses[name];
        status.addUsage(Status::DEFINED);

        auto it = last_uses.find(name);
        if (it != last_uses.end()) {
            kills.insert(it->second);
            last_uses.erase(it);
        }
    }

public:
    LivenessBBVisitor(LivenessAnalysis* analysis) : analysis(analysis) {}

    bool firstIsUse(InternedString name) { return statuses[name].first == Status::USED; }

    bool firstIsDef(InternedString name) { return statuses[name].first == Status::DEFINED; }

    bool isKilledAt(AST_Name* node, bool is_live_at_end) {
        if (kills.count(node))
            return true;

        // If it's not live at the end, then the last use is a kill
        // even though we weren't able to determine that in a single
        // pass
        if (!is_live_at_end) {
            auto it = last_uses.find(node->id);
            if (it != last_uses.end() && node == it->second)
                return true;
        }

        return false;
    }



    bool visit_classdef(AST_ClassDef* node) {
        _doStore(node->name);

        for (auto e : node->bases)
            e->accept(this);
        for (auto e : node->decorator_list)
            e->accept(this);

        return true;
    }
    bool visit_functiondef(AST_FunctionDef* node) {
        for (auto* d : node->decorator_list)
            d->accept(this);
        node->args->accept(this);

        _doStore(node->name);
        return true;
    }

    bool visit_lambda(AST_Lambda* node) {
        for (auto* d : node->args->defaults)
            d->accept(this);
        return true;
    }

    bool visit_name(AST_Name* node) {
        if (node->ctx_type == AST_TYPE::Load)
            _doLoad(node->id, node);
        else if (node->ctx_type == AST_TYPE::Store || node->ctx_type == AST_TYPE::Del
                 || node->ctx_type == AST_TYPE::Param)
            _doStore(node->id);
        else {
            ASSERT(0, "%d", node->ctx_type);
            abort();
        }
        return true;
    }
    bool visit_alias(AST_alias* node) {
        InternedString name = node->name;
        if (node->asname.str().size())
            name = node->asname;

        _doStore(name);
        return true;
    }
};

LivenessAnalysis::LivenessAnalysis(CFG* cfg) : cfg(cfg) {
    Timer _t("LivenessAnalysis()", 100);

    for (CFGBlock* b : cfg->blocks) {
        auto visitor = new LivenessBBVisitor(this); // livenessCache unique_ptr will delete it.
        for (AST_stmt* stmt : b->body) {
            stmt->accept(visitor);
        }
        liveness_cache.insert(std::make_pair(b, std::unique_ptr<LivenessBBVisitor>(visitor)));
    }

    static StatCounter us_liveness("us_compiling_analysis_liveness");
    us_liveness.log(_t.end());
}

bool LivenessAnalysis::isKill(AST_Name* node, CFGBlock* parent_block) {
    if (node->id.str()[0] != '#')
        return false;

    return liveness_cache[parent_block]->isKilledAt(node, isLiveAtEnd(node->id, parent_block));
}

bool LivenessAnalysis::isLiveAtEnd(InternedString name, CFGBlock* block) {
    Timer _t("LivenessAnalysis()", 10);

    if (name.str()[0] != '#')
        return true;

    if (block->successors.size() == 0)
        return false;

    if (!result_cache.count(name)) {
        std::unordered_map<CFGBlock*, bool>& map = result_cache[name];

        // Approach:
        // - Find all uses (blocks where the status is USED)
        // - Trace backwards, marking all blocks as live-at-end
        // - If we hit a block that is DEFINED, stop
        for (CFGBlock* b : cfg->blocks) {
            if (!liveness_cache[b]->firstIsUse(name))
                continue;

            std::deque<CFGBlock*> q;
            for (CFGBlock* pred : b->predecessors) {
                q.push_back(pred);
            }

            while (q.size()) {
                CFGBlock* thisblock = q.front();
                q.pop_front();

                if (map[thisblock])
                    continue;

                map[thisblock] = true;
                if (!liveness_cache[thisblock]->firstIsDef(name)) {
                    for (CFGBlock* pred : thisblock->predecessors) {
                        q.push_back(pred);
                    }
                }
            }
        }
    }

    // Note: this one gets counted as part of us_compiling_irgen as well:
    static StatCounter us_liveness("us_compiling_analysis_liveness");
    us_liveness.log(_t.end());

    return result_cache[name][block];
}

class DefinednessBBAnalyzer : public BBAnalyzer<DefinednessAnalysis::DefinitionLevel> {
private:
    typedef DefinednessAnalysis::DefinitionLevel DefinitionLevel;

    CFG* cfg;
    ScopeInfo* scope_info;
    const ParamNames& arg_names;

public:
    DefinednessBBAnalyzer(CFG* cfg, ScopeInfo* scope_info, const ParamNames& arg_names)
        : cfg(cfg), scope_info(scope_info), arg_names(arg_names) {}

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

    void _doSet(InternedString s) { state[s] = DefinednessAnalysis::Defined; }

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
    virtual bool visit_expr(AST_Expr* node) { return true; }
    virtual bool visit_global(AST_Global* node) { return true; }
    virtual bool visit_invoke(AST_Invoke* node) { return false; }
    virtual bool visit_jump(AST_Jump* node) { return true; }
    virtual bool visit_pass(AST_Pass* node) { return true; }
    virtual bool visit_print(AST_Print* node) { return true; }
    virtual bool visit_raise(AST_Raise* node) { return true; }
    virtual bool visit_return(AST_Return* node) { return true; }

    virtual bool visit_delete(AST_Delete* node) {
        for (auto t : node->targets) {
            if (t->type == AST_TYPE::Name) {
                AST_Name* name = ast_cast<AST_Name>(t);
                state.erase(name->id);
            } else {
                // The CFG pass should reduce all deletes to the "basic" deletes on names/attributes/subscripts.
                // If not, probably the best way to do this would be to just do a full AST traversal
                // and look for AST_Name's with a ctx of Del
                assert(t->type == AST_TYPE::Attribute || t->type == AST_TYPE::Subscript);
            }
        }
        return true;
    }

    virtual bool visit_classdef(AST_ClassDef* node) {
        _doSet(node->name);
        return true;
    }

    virtual bool visit_functiondef(AST_FunctionDef* node) {
        _doSet(node->name);
        return true;
    }

    virtual bool visit_alias(AST_alias* node) {
        InternedString name = node->name;
        if (node->asname.str().size())
            name = node->asname;

        _doSet(name);
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
        if (node->kwarg.str().size())
            _doSet(node->kwarg);
        if (node->vararg.str().size())
            _doSet(node->vararg);
        for (int i = 0; i < node->args.size(); i++) {
            _doSet(node->args[i]);
        }
        return true;
    }

    virtual bool visit_exec(AST_Exec* node) { return true; }

    friend class DefinednessBBAnalyzer;
};

void DefinednessBBAnalyzer::processBB(Map& starting, CFGBlock* block) const {
    DefinednessVisitor visitor(starting);

    if (block == cfg->getStartingBlock()) {
        for (auto e : arg_names.args)
            visitor._doSet(scope_info->internString(e));
        if (arg_names.vararg.size())
            visitor._doSet(scope_info->internString(arg_names.vararg));
        if (arg_names.kwarg.size())
            visitor._doSet(scope_info->internString(arg_names.kwarg));
    }

    for (int i = 0; i < block->body.size(); i++) {
        block->body[i]->accept(&visitor);
    }

    if (VERBOSITY("analysis") >= 3) {
        printf("At end of block %d:\n", block->idx);
        for (const auto& p : starting) {
            printf("%s: %d\n", p.first.c_str(), p.second);
        }
    }
}

DefinednessAnalysis::DefinednessAnalysis(const ParamNames& arg_names, CFG* cfg, ScopeInfo* scope_info)
    : scope_info(scope_info) {
    Timer _t("DefinednessAnalysis()", 10);

    results = computeFixedPoint(cfg, DefinednessBBAnalyzer(cfg, scope_info, arg_names), false);

    for (const auto& p : results) {
        RequiredSet required;
        for (const auto& p2 : p.second) {
            ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(p2.first);
            if (vst == ScopeInfo::VarScopeType::GLOBAL || vst == ScopeInfo::VarScopeType::NAME)
                continue;

            // printf("%d %s %d\n", p.first->idx, p2.first.c_str(), p2.second);
            required.insert(p2.first);
        }
        defined_at_end.insert(make_pair(p.first, required));
    }

    static StatCounter us_definedness("us_compiling_analysis_definedness");
    us_definedness.log(_t.end());
}

DefinednessAnalysis::DefinitionLevel DefinednessAnalysis::isDefinedAtEnd(InternedString name, CFGBlock* block) {
    auto& map = results[block];
    if (map.count(name) == 0)
        return Undefined;
    return map[name];
}

const DefinednessAnalysis::RequiredSet& DefinednessAnalysis::getDefinedNamesAtEnd(CFGBlock* block) {
    return defined_at_end[block];
}

PhiAnalysis::PhiAnalysis(const ParamNames& arg_names, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo* scope_info)
    : definedness(arg_names, cfg, scope_info), liveness(liveness) {
    Timer _t("PhiAnalysis()", 10);

    for (CFGBlock* block : cfg->blocks) {
        RequiredSet required;

        if (block->predecessors.size() > 1) {
            for (CFGBlock* pred : block->predecessors) {
                const RequiredSet& defined = definedness.getDefinedNamesAtEnd(pred);
                for (const auto& s : defined) {
                    if (required.count(s) == 0 && liveness->isLiveAtEnd(s, pred)) {
                        // printf("%d-%d %s\n", pred->idx, block->idx, s.c_str());

                        required.insert(s);
                    }
                }
            }
        }

        required_phis.insert(make_pair(block, std::move(required)));
    }

    static StatCounter us_phis("us_compiling_analysis_phis");
    us_phis.log(_t.end());
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllRequiredAfter(CFGBlock* block) {
    static RequiredSet empty;
    if (block->successors.size() == 0)
        return empty;
    return required_phis[block->successors[0]];
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllRequiredFor(CFGBlock* block) {
    return required_phis[block];
}

bool PhiAnalysis::isRequired(InternedString name, CFGBlock* block) {
    assert(!startswith(name.str(), "!"));
    return required_phis[block].count(name) != 0;
}

bool PhiAnalysis::isRequiredAfter(InternedString name, CFGBlock* block) {
    assert(!startswith(name.str(), "!"));
    // If there are multiple successors, then none of them are allowed
    // to require any phi nodes
    if (block->successors.size() != 1)
        return false;

    // Fall back to the other method:
    return isRequired(name, block->successors[0]);
}

bool PhiAnalysis::isPotentiallyUndefinedAfter(InternedString name, CFGBlock* block) {
    assert(!startswith(name.str(), "!"));

    if (block->successors.size() != 1)
        return false;

    return isPotentiallyUndefinedAt(name, block->successors[0]);
}

bool PhiAnalysis::isPotentiallyUndefinedAt(InternedString name, CFGBlock* block) {
    assert(!startswith(name.str(), "!"));

    for (CFGBlock* pred : block->predecessors) {
        DefinednessAnalysis::DefinitionLevel dlevel = definedness.isDefinedAtEnd(name, pred);
        if (dlevel != DefinednessAnalysis::Defined)
            return true;
    }
    return false;
}

LivenessAnalysis* computeLivenessInfo(CFG* cfg) {
    return new LivenessAnalysis(cfg);
}

PhiAnalysis* computeRequiredPhis(const ParamNames& args, CFG* cfg, LivenessAnalysis* liveness, ScopeInfo* scope_info) {
    return new PhiAnalysis(args, cfg, liveness, scope_info);
}
}
