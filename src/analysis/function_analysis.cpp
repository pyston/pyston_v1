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

#include "analysis/function_analysis.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_set>

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringSet.h"

#include "analysis/fpc.h"
#include "analysis/scoping_analysis.h"
#include "codegen/osrentry.h"
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

    llvm::DenseMap<InternedString, Status> statuses;
    LivenessAnalysis* analysis;

    void _doLoad(InternedString name, AST_Name* node) {
        Status& status = statuses[name];
        status.addUsage(Status::USED);
    }

    void _doStore(InternedString name) {
        Status& status = statuses[name];
        status.addUsage(Status::DEFINED);
    }

    Status::Usage getStatusFirst(InternedString name) const {
        auto it = statuses.find(name);
        if (it == statuses.end())
            return Status::NONE;
        return it->second.first;
    }

public:
    LivenessBBVisitor(LivenessAnalysis* analysis) : analysis(analysis) {}

    bool firstIsUse(InternedString name) const { return getStatusFirst(name) == Status::USED; }

    bool firstIsDef(InternedString name) const { return getStatusFirst(name) == Status::DEFINED; }

    bool isKilledAt(AST_Name* node, bool is_live_at_end) { return node->is_kill; }


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
        else if (node->ctx_type == AST_TYPE::Del) {
            // Hack: we don't have a bytecode for temporary-kills:
            if (node->id.s()[0] == '#')
                return true;
            _doLoad(node->id, node);
            _doStore(node->id);
        } else if (node->ctx_type == AST_TYPE::Store || node->ctx_type == AST_TYPE::Param)
            _doStore(node->id);
        else {
            ASSERT(0, "%d", node->ctx_type);
            abort();
        }
        return true;
    }

    bool visit_alias(AST_alias* node) {
        InternedString name = node->name;
        if (node->asname.s().size())
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

LivenessAnalysis::~LivenessAnalysis() {
}

bool LivenessAnalysis::isKill(AST_Name* node, CFGBlock* parent_block) {
    if (node->id.s()[0] != '#')
        return false;

    return liveness_cache[parent_block]->isKilledAt(node, isLiveAtEnd(node->id, parent_block));
}

bool LivenessAnalysis::isLiveAtEnd(InternedString name, CFGBlock* block) {
    if (name.s()[0] != '#')
        return true;

    if (block->successors.size() == 0)
        return false;

    if (!result_cache.count(name)) {
        Timer _t("LivenessAnalysis()", 10);

        llvm::DenseMap<CFGBlock*, bool>& map = result_cache[name];

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

        // Note: this one gets counted as part of us_compiling_irgen as well:
        static StatCounter us_liveness("us_compiling_analysis_liveness");
        us_liveness.log(_t.end());
    }

    return result_cache[name][block];
}

class DefinednessBBAnalyzer : public BBAnalyzer<DefinednessAnalysis::DefinitionLevel> {
private:
    typedef DefinednessAnalysis::DefinitionLevel DefinitionLevel;

    ScopeInfo* scope_info;

public:
    DefinednessBBAnalyzer(ScopeInfo* scope_info) : scope_info(scope_info) {}

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
        if (node->asname.s().size())
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
        if (node->kwarg.s().size())
            _doSet(node->kwarg);
        if (node->vararg.s().size())
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

void DefinednessAnalysis::run(llvm::DenseMap<InternedString, DefinednessAnalysis::DefinitionLevel> initial_map,
                              CFGBlock* initial_block, ScopeInfo* scope_info) {
    Timer _t("DefinednessAnalysis()", 10);

    // Don't run this twice:
    assert(!defined_at_end.size());

    computeFixedPoint(std::move(initial_map), initial_block, DefinednessBBAnalyzer(scope_info), false,
                      defined_at_beginning, defined_at_end);

    for (const auto& p : defined_at_end) {
        RequiredSet& required = defined_at_end_sets[p.first];
        for (const auto& p2 : p.second) {
            ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(p2.first);
            if (vst == ScopeInfo::VarScopeType::GLOBAL || vst == ScopeInfo::VarScopeType::NAME)
                continue;

            required.insert(p2.first);
        }
    }

    static StatCounter us_definedness("us_compiling_analysis_definedness");
    us_definedness.log(_t.end());
}

DefinednessAnalysis::DefinitionLevel DefinednessAnalysis::isDefinedAtEnd(InternedString name, CFGBlock* block) {
    assert(defined_at_end.count(block));
    auto& map = defined_at_end[block];
    if (map.count(name) == 0)
        return Undefined;
    return map[name];
}

const DefinednessAnalysis::RequiredSet& DefinednessAnalysis::getDefinedNamesAtEnd(CFGBlock* block) {
    assert(defined_at_end_sets.count(block));
    return defined_at_end_sets[block];
}

PhiAnalysis::PhiAnalysis(llvm::DenseMap<InternedString, DefinednessAnalysis::DefinitionLevel> initial_map,
                         CFGBlock* initial_block, bool initials_need_phis, LivenessAnalysis* liveness,
                         ScopeInfo* scope_info)
    : definedness(), liveness(liveness) {
    // I think this should always be the case -- if we're going to generate phis for the initial block,
    // then we should include the initial arguments as an extra entry point.
    assert(initials_need_phis == (initial_block->predecessors.size() > 0));

    definedness.run(std::move(initial_map), initial_block, scope_info);

    Timer _t("PhiAnalysis()", 10);

    for (const auto& p : definedness.defined_at_end) {
        CFGBlock* block = p.first;
        RequiredSet& required = required_phis[block];

        int npred = 0;
        for (CFGBlock* pred : block->predecessors) {
            if (definedness.defined_at_end.count(pred))
                npred++;
        }

        if (npred > 1 || (initials_need_phis && block == initial_block)) {
            for (CFGBlock* pred : block->predecessors) {
                if (!definedness.defined_at_end.count(pred))
                    continue;

                const RequiredSet& defined = definedness.getDefinedNamesAtEnd(pred);
                for (const auto& s : defined) {
                    if (required.count(s) == 0 && liveness->isLiveAtEnd(s, pred)) {
                        // printf("%d-%d %s\n", pred->idx, block->idx, s.c_str());

                        required.insert(s);
                    }
                }
            }
        }
    }

    static StatCounter us_phis("us_compiling_analysis_phis");
    us_phis.log(_t.end());
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllRequiredAfter(CFGBlock* block) {
    static RequiredSet empty;
    if (block->successors.size() == 0)
        return empty;
    assert(required_phis.count(block->successors[0]));
    return required_phis[block->successors[0]];
}

const PhiAnalysis::RequiredSet& PhiAnalysis::getAllRequiredFor(CFGBlock* block) {
    assert(required_phis.count(block));
    return required_phis[block];
}

bool PhiAnalysis::isRequired(InternedString name, CFGBlock* block) {
    assert(!startswith(name.s(), "!"));
    assert(required_phis.count(block));
    return required_phis[block].count(name) != 0;
}

bool PhiAnalysis::isRequiredAfter(InternedString name, CFGBlock* block) {
    assert(!startswith(name.s(), "!"));
    // If there are multiple successors, then none of them are allowed
    // to require any phi nodes
    if (block->successors.size() != 1)
        return false;

    // Fall back to the other method:
    return isRequired(name, block->successors[0]);
}

bool PhiAnalysis::isPotentiallyUndefinedAfter(InternedString name, CFGBlock* block) {
    assert(!startswith(name.s(), "!"));

    for (auto b : block->successors) {
        if (isPotentiallyUndefinedAt(name, b))
            return true;
    }
    return false;
}

bool PhiAnalysis::isPotentiallyUndefinedAt(InternedString name, CFGBlock* block) {
    assert(!startswith(name.s(), "!"));

    assert(definedness.defined_at_beginning.count(block));
    return definedness.defined_at_beginning[block][name] != DefinednessAnalysis::Defined;
}

std::unique_ptr<LivenessAnalysis> computeLivenessInfo(CFG* cfg) {
    static StatCounter counter("num_liveness_analysis");
    counter.log();

    return std::unique_ptr<LivenessAnalysis>(new LivenessAnalysis(cfg));
}

std::unique_ptr<PhiAnalysis> computeRequiredPhis(const ParamNames& args, CFG* cfg, LivenessAnalysis* liveness,
                                                 ScopeInfo* scope_info) {
    static StatCounter counter("num_phi_analysis");
    counter.log();

    llvm::DenseMap<InternedString, DefinednessAnalysis::DefinitionLevel> initial_map;

    for (auto e : args.args)
        initial_map[scope_info->internString(e)] = DefinednessAnalysis::Defined;
    if (args.vararg.size())
        initial_map[scope_info->internString(args.vararg)] = DefinednessAnalysis::Defined;
    if (args.kwarg.size())
        initial_map[scope_info->internString(args.kwarg)] = DefinednessAnalysis::Defined;

    return std::unique_ptr<PhiAnalysis>(
        new PhiAnalysis(std::move(initial_map), cfg->getStartingBlock(), false, liveness, scope_info));
}

std::unique_ptr<PhiAnalysis> computeRequiredPhis(const OSREntryDescriptor* entry_descriptor, LivenessAnalysis* liveness,
                                                 ScopeInfo* scope_info) {
    static StatCounter counter("num_phi_analysis");
    counter.log();

    llvm::DenseMap<InternedString, DefinednessAnalysis::DefinitionLevel> initial_map;

    llvm::StringSet<> potentially_undefined;
    for (const auto& p : entry_descriptor->args) {
        if (!startswith(p.first.s(), "!is_defined_"))
            continue;
        potentially_undefined.insert(p.first.s().substr(12));
    }

    for (const auto& p : entry_descriptor->args) {
        if (p.first.s()[0] == '!')
            continue;
        if (potentially_undefined.count(p.first.s()))
            initial_map[p.first] = DefinednessAnalysis::PotentiallyDefined;
        else
            initial_map[p.first] = DefinednessAnalysis::Defined;
    }

    return std::unique_ptr<PhiAnalysis>(
        new PhiAnalysis(std::move(initial_map), entry_descriptor->backedge->target, true, liveness, scope_info));
}
}
