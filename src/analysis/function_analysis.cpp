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
        enum Usage : char {
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

    VRegMap<Status> statuses;
    LivenessAnalysis* analysis;

    void _doLoad(int vreg, AST_Name* node) {
        Status& status = statuses[vreg];
        status.addUsage(Status::USED);
    }

    void _doStore(int vreg) {
        assert(vreg >= 0);
        Status& status = statuses[vreg];
        status.addUsage(Status::DEFINED);
    }

    Status::Usage getStatusFirst(int vreg) const { return statuses[vreg].first; }

public:
    LivenessBBVisitor(LivenessAnalysis* analysis)
        : statuses(analysis->cfg->getVRegInfo().getTotalNumOfVRegs()), analysis(analysis) {}

    bool firstIsUse(int vreg) const { return getStatusFirst(vreg) == Status::USED; }

    bool firstIsDef(int vreg) const { return getStatusFirst(vreg) == Status::DEFINED; }

    bool isKilledAt(AST_Name* node, bool is_live_at_end) { return node->is_kill; }

    bool visit_import(AST_Import* node) { RELEASE_ASSERT(0, "these should all get removed by the cfg"); }

    bool visit_classdef(AST_ClassDef* node) {
        for (auto e : node->bases)
            e->accept(this);
        for (auto e : node->decorator_list)
            e->accept(this);

        return true;
    }

    bool visit_functiondef(AST_FunctionDef* node) {
        for (auto* d : node->decorator_list)
            d->accept(this);
        for (auto* d : node->args->defaults)
            d->accept(this);

        return true;
    }

    bool visit_lambda(AST_Lambda* node) {
        for (auto* d : node->args->defaults)
            d->accept(this);
        return true;
    }

    bool visit_name(AST_Name* node) {
        if (node->vreg == -1)
            return true;

        if (node->ctx_type == AST_TYPE::Load)
            _doLoad(node->vreg, node);
        else if (node->ctx_type == AST_TYPE::Del) {
            // Hack: we don't have a bytecode for temporary-kills:
            if (node->vreg >= analysis->cfg->getVRegInfo().getNumOfUserVisibleVRegs())
                return true;
            _doLoad(node->vreg, node);
            _doStore(node->vreg);
        } else if (node->ctx_type == AST_TYPE::Store || node->ctx_type == AST_TYPE::Param)
            _doStore(node->vreg);
        else {
            ASSERT(0, "%d", node->ctx_type);
            abort();
        }
        return true;
    }

    bool visit_alias(AST_alias* node) { RELEASE_ASSERT(0, "these should be removed by the cfg"); }
};

LivenessAnalysis::LivenessAnalysis(CFG* cfg) : cfg(cfg), result_cache(cfg->getVRegInfo().getTotalNumOfVRegs()) {
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

    return liveness_cache[parent_block]->isKilledAt(node, isLiveAtEnd(node->vreg, parent_block));
}

bool LivenessAnalysis::isLiveAtEnd(int vreg, CFGBlock* block) {
    // Is a user-visible name, always live:
    if (vreg < block->cfg->getVRegInfo().getNumOfUserVisibleVRegs())
        return true;

    // For block-local vregs, this query doesn't really make sense,
    // since the vreg will be live but that's probably not what we care about.
    // It's probably safe to return false, but let's just error for now.
    if (block->cfg->getVRegInfo().isBlockLocalVReg(vreg))
        return false;

    if (block->successors.size() == 0)
        return false;

    if (!result_cache[vreg].size()) {
        Timer _t("LivenessAnalysis()", 10);

        llvm::DenseMap<CFGBlock*, bool>& map = result_cache[vreg];

        // Approach:
        // - Find all uses (blocks where the status is USED)
        // - Trace backwards, marking all blocks as live-at-end
        // - If we hit a block that is DEFINED, stop
        for (CFGBlock* b : cfg->blocks) {
            if (!liveness_cache[b]->firstIsUse(vreg))
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
                if (!liveness_cache[thisblock]->firstIsDef(vreg)) {
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

    return result_cache[vreg][block];
}

class DefinednessBBAnalyzer : public BBAnalyzer<DefinednessAnalysis::DefinitionLevel> {
private:
    typedef DefinednessAnalysis::DefinitionLevel DefinitionLevel;

    ScopeInfo* scope_info;

public:
    DefinednessBBAnalyzer(ScopeInfo* scope_info) : scope_info(scope_info) {}

    virtual DefinitionLevel merge(DefinitionLevel from, DefinitionLevel into) const {
        assert(from != DefinitionLevel::Unknown);
        if (into == DefinitionLevel::Unknown)
            return from;

        if (into == DefinednessAnalysis::Undefined && from == DefinednessAnalysis::Undefined)
            return DefinednessAnalysis::Undefined;

        if (into == DefinednessAnalysis::Defined && from == DefinednessAnalysis::Defined)
            return DefinednessAnalysis::Defined;
        return DefinednessAnalysis::PotentiallyDefined;
    }
    virtual void processBB(Map& starting, CFGBlock* block) const;
};

class DefinednessVisitor : public ASTVisitor {
private:
    typedef DefinednessBBAnalyzer::Map Map;
    Map& state;

    void _doSet(int vreg) {
        assert(vreg >= 0 && vreg < state.numVregs());
        state[vreg] = DefinednessAnalysis::Defined;
    }

    void _doSet(AST* t) {
        switch (t->type) {
            case AST_TYPE::Attribute:
                // doesn't affect definedness (yet?)
                break;
            case AST_TYPE::Name: {
                auto name = ast_cast<AST_Name>(t);
                if (name->lookup_type == ScopeInfo::VarScopeType::FAST
                    || name->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
                    assert(name->vreg != -1);
                    _doSet(name->vreg);
                } else if (name->lookup_type == ScopeInfo::VarScopeType::GLOBAL
                           || name->lookup_type == ScopeInfo::VarScopeType::NAME) {
                    assert(name->vreg == -1);
                    // skip
                } else {
                    RELEASE_ASSERT(0, "%d", name->lookup_type);
                }
                break;
            }
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
                if (name->lookup_type != ScopeInfo::VarScopeType::GLOBAL
                    && name->lookup_type != ScopeInfo::VarScopeType::NAME) {
                    assert(name->vreg != -1);
                    state[name->vreg] = DefinednessAnalysis::Undefined;
                } else
                    assert(name->vreg == -1);
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
        assert(0 && "I think this isn't needed");
        //_doSet(node->name);
        return true;
    }

    virtual bool visit_functiondef(AST_FunctionDef* node) {
        assert(0 && "I think this isn't needed");
        //_doSet(node->name);
        return true;
    }

    virtual bool visit_alias(AST_alias* node) {
        int vreg = node->name_vreg;
        if (node->asname.s().size())
            vreg = node->asname_vreg;

        _doSet(vreg);
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
        if (node->vararg)
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
            if (p.second != DefinednessAnalysis::Undefined)
                printf("%s: %d\n", block->cfg->getVRegInfo().getName(p.first).c_str(), p.second);
        }
    }
}

void DefinednessAnalysis::run(VRegMap<DefinednessAnalysis::DefinitionLevel> initial_map, CFGBlock* initial_block,
                              ScopeInfo* scope_info) {
    Timer _t("DefinednessAnalysis()", 10);

    // Don't run this twice:
    assert(!defined_at_end.size());

    auto cfg = initial_block->cfg;
    int nvregs = cfg->getVRegInfo().getTotalNumOfVRegs();
    assert(initial_map.numVregs() == nvregs);

    auto&& vreg_info = cfg->getVRegInfo();
    computeFixedPoint(std::move(initial_map), initial_block, DefinednessBBAnalyzer(scope_info), false,
                      defined_at_beginning, defined_at_end);

    for (const auto& p : defined_at_end) {
        assert(p.second.numVregs() == nvregs);

        assert(!defined_at_end_sets.count(p.first));
        VRegSet& required = defined_at_end_sets.insert(std::make_pair(p.first, VRegSet(nvregs))).first->second;

        // required.resize(nvregs, /* value= */ false);

        for (int vreg = 0; vreg < nvregs; vreg++) {
#ifndef NDEBUG
            if (vreg_info.vregHasName(vreg)) {
                ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(vreg_info.getName(vreg));
                ASSERT(vst != ScopeInfo::VarScopeType::GLOBAL && vst != ScopeInfo::VarScopeType::NAME, "%s",
                       vreg_info.getName(vreg).c_str());
            }
#endif

            auto status = p.second[vreg];
            // assert(p.second.count(name));
            // auto status = p.second.find(name)->second;
            assert(status != DefinednessAnalysis::Unknown);
            if (status != DefinednessAnalysis::Undefined)
                required.set(vreg);
        }
    }

    static StatCounter us_definedness("us_compiling_analysis_definedness");
    us_definedness.log(_t.end());
}

DefinednessAnalysis::DefinitionLevel DefinednessAnalysis::isDefinedAtEnd(int vreg, CFGBlock* block) {
    assert(defined_at_end.count(block));
    auto&& map = defined_at_end.find(block)->second;
    return map[vreg];
}

const VRegSet& DefinednessAnalysis::getDefinedVregsAtEnd(CFGBlock* block) {
    assert(defined_at_end_sets.count(block));
    return defined_at_end_sets.find(block)->second;
}

PhiAnalysis::PhiAnalysis(VRegMap<DefinednessAnalysis::DefinitionLevel> initial_map, CFGBlock* initial_block,
                         bool initials_need_phis, LivenessAnalysis* liveness, ScopeInfo* scope_info)
    : definedness(), empty_set(initial_map.numVregs()), liveness(liveness) {
    auto cfg = initial_block->cfg;
    auto&& vreg_info = cfg->getVRegInfo();

    // I think this should always be the case -- if we're going to generate phis for the initial block,
    // then we should include the initial arguments as an extra entry point.
    assert(initials_need_phis == (initial_block->predecessors.size() > 0));

    int num_vregs = initial_map.numVregs();
    assert(num_vregs == vreg_info.getTotalNumOfVRegs());

    definedness.run(std::move(initial_map), initial_block, scope_info);

    Timer _t("PhiAnalysis()", 10);

    for (const auto& p : definedness.defined_at_end) {
        CFGBlock* block = p.first;
        assert(!required_phis.count(block));
        VRegSet& required = required_phis.insert(std::make_pair(block, VRegSet(num_vregs))).first->second;

        int npred = 0;
        for (CFGBlock* pred : block->predecessors) {
            if (definedness.defined_at_end.count(pred))
                npred++;
        }

        if (npred > 1 || (initials_need_phis && block == initial_block)) {
            for (CFGBlock* pred : block->predecessors) {
                if (!definedness.defined_at_end.count(pred))
                    continue;

                const VRegSet& defined = definedness.getDefinedVregsAtEnd(pred);
                for (int vreg : defined) {
                    if (!required[vreg] && liveness->isLiveAtEnd(vreg, pred)) {
                        // printf("%d-%d %s\n", pred->idx, block->idx, vreg_info.getName(vreg).c_str());

                        required.set(vreg);
                    }
                }
            }
        }

        if (VERBOSITY() >= 3) {
            printf("Phis required at end of %d:", block->idx);
            for (auto vreg : required) {
                printf(" %s", vreg_info.getName(vreg).c_str());
            }
            printf("\n");
        }
    }

    static StatCounter us_phis("us_compiling_analysis_phis");
    us_phis.log(_t.end());
}

const VRegSet& PhiAnalysis::getAllRequiredAfter(CFGBlock* block) {
    if (block->successors.size() == 0)
        return empty_set;
    assert(required_phis.count(block->successors[0]));
    return required_phis.find(block->successors[0])->second;
}

const VRegSet& PhiAnalysis::getAllRequiredFor(CFGBlock* block) {
    assert(required_phis.count(block));
    return required_phis.find(block)->second;
}

bool PhiAnalysis::isRequired(int vreg, CFGBlock* block) {
    assert(required_phis.count(block));
    return required_phis.find(block)->second[vreg];
}

bool PhiAnalysis::isRequiredAfter(int vreg, CFGBlock* block) {
    // If there are multiple successors, then none of them are allowed
    // to require any phi nodes
    if (block->successors.size() != 1)
        return false;

    // Fall back to the other method:
    return isRequired(vreg, block->successors[0]);
}

bool PhiAnalysis::isPotentiallyUndefinedAfter(int vreg, CFGBlock* block) {
    for (auto b : block->successors) {
        if (isPotentiallyUndefinedAt(vreg, b))
            return true;
    }
    return false;
}

bool PhiAnalysis::isPotentiallyUndefinedAt(int vreg, CFGBlock* block) {
    assert(definedness.defined_at_beginning.count(block));
    return definedness.defined_at_beginning.find(block)->second[vreg] != DefinednessAnalysis::Defined;
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

    auto&& vreg_info = cfg->getVRegInfo();
    int num_vregs = vreg_info.getTotalNumOfVRegs();

    VRegMap<DefinednessAnalysis::DefinitionLevel> initial_map(num_vregs);

    assert(vreg_info.hasVRegsAssigned());
    for (int vreg = 0; vreg < num_vregs; vreg++) {
        initial_map[vreg] = DefinednessAnalysis::Undefined;
    }

    auto maybe_add = [&](AST_Name* n) {
        ScopeInfo::VarScopeType vst = n->lookup_type;
        assert(vst != ScopeInfo::VarScopeType::UNKNOWN);
        assert(vst != ScopeInfo::VarScopeType::GLOBAL); // global-and-local error
        if (vst == ScopeInfo::VarScopeType::NAME)
            return;
        assert(n->vreg >= 0);
        initial_map[n->vreg] = DefinednessAnalysis::Defined;
    };

    for (auto e : args.arg_names)
        maybe_add(e);
    if (args.vararg_name)
        maybe_add(args.vararg_name);
    if (args.kwarg_name)
        maybe_add(args.kwarg_name);

    assert(initial_map.numVregs() == vreg_info.getTotalNumOfVRegs());

    return std::unique_ptr<PhiAnalysis>(
        new PhiAnalysis(std::move(initial_map), cfg->getStartingBlock(), false, liveness, scope_info));
}

std::unique_ptr<PhiAnalysis> computeRequiredPhis(const OSREntryDescriptor* entry_descriptor, LivenessAnalysis* liveness,
                                                 ScopeInfo* scope_info) {
    static StatCounter counter("num_phi_analysis");
    counter.log();

    auto cfg = entry_descriptor->md->source->cfg;
    int num_vregs = cfg->getVRegInfo().getTotalNumOfVRegs();
    VRegMap<DefinednessAnalysis::DefinitionLevel> initial_map(num_vregs);

    for (int vreg = 0; vreg < num_vregs; vreg++) {
        initial_map[vreg] = DefinednessAnalysis::Undefined;
    }

    for (const auto& p : entry_descriptor->args) {
        int vreg = p.first;
        ASSERT(initial_map[vreg] == DefinednessAnalysis::Undefined, "%d %d", vreg, initial_map[vreg]);
        if (entry_descriptor->potentially_undefined[vreg])
            initial_map[vreg] = DefinednessAnalysis::PotentiallyDefined;
        else
            initial_map[vreg] = DefinednessAnalysis::Defined;
    }

    return std::unique_ptr<PhiAnalysis>(
        new PhiAnalysis(std::move(initial_map), entry_descriptor->backedge->target, true, liveness, scope_info));
}
}
