#include <memory>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#include "gtest/gtest.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/irgen/future.h"
#include "codegen/osrentry.h"
#include "codegen/parser.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "unittests.h"

using namespace pyston;

class AnalysisTest : public ::testing::Test {
protected:
    static void SetUpTestCase() {
        initCodegen();
    }
};

TEST_F(AnalysisTest, augassign) {
    const std::string fn("test/unittests/analysis_listcomp.py");
    AST_Module* module = caching_parse_file(fn.c_str());
    assert(module);

    ScopingAnalysis *scoping = new ScopingAnalysis(module, true);

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);
    ASSERT_FALSE(scope_info->getScopeTypeOfName(module->interned_strings->get("a")) == ScopeInfo::VarScopeType::GLOBAL);
    ASSERT_FALSE(scope_info->getScopeTypeOfName(module->interned_strings->get("b")) == ScopeInfo::VarScopeType::GLOBAL);

    FutureFlags future_flags = getFutureFlags(module->body, fn.c_str());

    SourceInfo* si = new SourceInfo(createModule("augassign", fn.c_str()), scoping, future_flags, func, func->body, fn);

    CFG* cfg = computeCFG(si, func->body);
    std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(cfg);

    //cfg->print();

    for (CFGBlock* block : cfg->blocks) {
        //printf("%d\n", block->idx);
        if (block->body.back()->type != AST_TYPE::Return)
            ASSERT_TRUE(liveness->isLiveAtEnd(module->interned_strings->get("a"), block));
    }

    std::unique_ptr<PhiAnalysis> phis = computeRequiredPhis(ParamNames(func, si->getInternedStrings()), cfg, liveness.get(), scope_info);
}

void doOsrTest(bool is_osr, bool i_maybe_undefined) {
    const std::string fn("test/unittests/analysis_osr.py");
    AST_Module* module = caching_parse_file(fn.c_str());
    assert(module);

    ScopingAnalysis *scoping = new ScopingAnalysis(module, true);

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    FutureFlags future_flags = getFutureFlags(module->body, fn.c_str());

    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);
    std::unique_ptr<SourceInfo> si(new SourceInfo(createModule("osr" + std::to_string((is_osr << 1) + i_maybe_undefined),
                    fn.c_str()), scoping, future_flags, func, func->body, fn));
    CLFunction* clfunc = new CLFunction(0, 0, false, false, std::move(si));

    CFG* cfg = computeCFG(clfunc->source.get(), func->body);
    std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(cfg);

    // cfg->print();

    InternedString i_str = module->interned_strings->get("i");
    InternedString idi_str = module->interned_strings->get("!is_defined_i");
    InternedString iter_str = module->interned_strings->get("#iter_3");

    CFGBlock* loop_backedge = cfg->blocks[5];
    ASSERT_EQ(6, loop_backedge->idx);
    ASSERT_EQ(1, loop_backedge->body.size());

    ASSERT_EQ(AST_TYPE::Jump, loop_backedge->body[0]->type);
    AST_Jump* backedge = ast_cast<AST_Jump>(loop_backedge->body[0]);
    ASSERT_LE(backedge->target->idx, loop_backedge->idx);

    std::unique_ptr<PhiAnalysis> phis;

    if (is_osr) {
        OSREntryDescriptor* entry_descriptor = OSREntryDescriptor::create(clfunc, backedge);
        entry_descriptor->args[i_str] = NULL;
        if (i_maybe_undefined)
            entry_descriptor->args[idi_str] = NULL;
        entry_descriptor->args[iter_str] = NULL;
        phis = computeRequiredPhis(entry_descriptor, liveness.get(), scope_info);
    } else {
        phis = computeRequiredPhis(ParamNames(func, clfunc->source->getInternedStrings()), cfg, liveness.get(), scope_info);
    }

    // First, verify that we require phi nodes for the block we enter into.
    // This is somewhat tricky since the osr entry represents an extra entry
    // into the BB which the analysis might not otherwise track.

    auto required_phis = phis->getAllRequiredFor(backedge->target);
    EXPECT_EQ(1, required_phis.count(i_str));
    EXPECT_EQ(0, required_phis.count(idi_str));
    EXPECT_EQ(1, required_phis.count(iter_str));
    EXPECT_EQ(2, required_phis.size());

    EXPECT_EQ(!is_osr || i_maybe_undefined, phis->isPotentiallyUndefinedAt(i_str, backedge->target));
    EXPECT_FALSE(phis->isPotentiallyUndefinedAt(iter_str, backedge->target));
    EXPECT_EQ(!is_osr || i_maybe_undefined, phis->isPotentiallyUndefinedAfter(i_str, loop_backedge));
    EXPECT_FALSE(phis->isPotentiallyUndefinedAfter(iter_str, loop_backedge));

    // Now, let's verify that we don't need a phi after the loop

    CFGBlock* if_join = cfg->blocks[7];
    ASSERT_EQ(8, if_join->idx);
    ASSERT_EQ(2, if_join->predecessors.size());

    if (is_osr)
        EXPECT_EQ(0, phis->getAllRequiredFor(if_join).size());
    else
        EXPECT_EQ(1, phis->getAllRequiredFor(if_join).size());
}

TEST_F(AnalysisTest, osr_initial) {
    doOsrTest(false, false);
}
TEST_F(AnalysisTest, osr1) {
    doOsrTest(true, false);
}
TEST_F(AnalysisTest, osr2) {
    doOsrTest(true, true);
}
