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
#include "core/bst.h"
#include "core/cfg.h"
#include "runtime/types.h"
#include "unittests.h"

using namespace pyston;

class AnalysisTest : public ::testing::Test {
protected:
    static void SetUpTestCase() {
        Py_Initialize();
    }
};

static BoxedCode* getCodeObjectOfFirstMakeFunction(BoxedCode* module_code) {
    BoxedCode* code = NULL;
    for (BST_stmt* stmt : *module_code->source->cfg->getStartingBlock()) {
        if (stmt->type() !=  BST_TYPE::MakeFunction)
            continue;
        code = module_code->code_constants.getFuncOrClass(bst_cast<BST_MakeFunction>(stmt)->index_func_def).second;
        break;
    }
    assert(code);
    return code;
}

// this test use functions (VRegInfo::getVReg) which are only available in a debug build
#ifndef NDEBUG
TEST_F(AnalysisTest, augassign) {
    const std::string fn("test/unittests/analysis_listcomp.py");
    std::unique_ptr<ASTAllocator> ast_allocator;
    AST_Module* module;
    std::tie(module, ast_allocator) = caching_parse_file(fn.c_str(), 0);
    assert(module);

    FutureFlags future_flags = getFutureFlags(module->body, fn.c_str());

    auto scoping = std::make_shared<ScopingAnalysis>(module, true);
    BoxedModule* main_module = createModule(autoDecref(boxString("__main__")), "<string>");
    auto module_code = computeAllCFGs(module, true, future_flags, boxString(fn), main_module);

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);

    ASSERT_NE(scope_info->getScopeTypeOfName(module->interned_strings->get("a")), ScopeInfo::VarScopeType::GLOBAL);
    ASSERT_FALSE(scope_info->getScopeTypeOfName(module->interned_strings->get("b")) == ScopeInfo::VarScopeType::GLOBAL);

    AST_arguments* args = new (*ast_allocator) AST_arguments();
    ParamNames param_names(args, *module->interned_strings.get());

    // Hack to get at the cfg:
    BoxedCode* code = getCodeObjectOfFirstMakeFunction(module_code);
    CFG* cfg = code->source->cfg;

    std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(cfg, code->code_constants);
    auto&& vregs = cfg->getVRegInfo();

    //cfg->print();

    for (CFGBlock* block : cfg->blocks) {
        //printf("%d\n", block->idx);
        if (block->getLastStmt()->type() != BST_TYPE::Return)
            ASSERT_TRUE(liveness->isLiveAtEnd(vregs.getVReg(module->interned_strings->get("a")), block));
    }

    std::unique_ptr<PhiAnalysis> phis = computeRequiredPhis(ParamNames(args, *module->interned_strings.get()), cfg, liveness.get());
}

void doOsrTest(bool is_osr, bool i_maybe_undefined) {
    const std::string fn("test/unittests/analysis_osr.py");
    std::unique_ptr<ASTAllocator> ast_allocator;
    AST_Module* module;
    std::tie(module, ast_allocator) = caching_parse_file(fn.c_str(), 0);
    assert(module);

    ParamNames param_names(NULL, *module->interned_strings.get());

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    auto scoping = std::make_shared<ScopingAnalysis>(module, true);
    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);

    FutureFlags future_flags = getFutureFlags(module->body, fn.c_str());

    BoxedModule* main_module = createModule(autoDecref(boxString("__main__")), "<string>");
    auto module_code = computeAllCFGs(module, true, future_flags, boxString(fn), main_module);

    // Hack to get at the cfg:
    BoxedCode* code = getCodeObjectOfFirstMakeFunction(module_code);
    CFG* cfg = code->source->cfg;
    std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(cfg, module_code->code_constants);

    // cfg->print();

    auto&& vregs = cfg->getVRegInfo();

    InternedString i_str = module->interned_strings->get("i");
    InternedString idi_str = module->interned_strings->get("!is_defined_i");
    InternedString iter_str = module->interned_strings->get("#iter_4");

    CFGBlock* loop_backedge = cfg->blocks[5];
    ASSERT_EQ(6, loop_backedge->idx);
    ASSERT_TRUE(loop_backedge->body()->is_terminator());

    ASSERT_EQ(BST_TYPE::Jump, loop_backedge->body()->type());
    BST_Jump* backedge = bst_cast<BST_Jump>(loop_backedge->body());
    ASSERT_LE(backedge->target->idx, loop_backedge->idx);

    std::unique_ptr<PhiAnalysis> phis;

    if (is_osr) {
        int vreg = vregs.getVReg(i_str);
        OSREntryDescriptor* entry_descriptor = OSREntryDescriptor::create(code, backedge, CXX);
        // need to set it to non-null
        ConcreteCompilerType* fake_type = (ConcreteCompilerType*)1;
        entry_descriptor->args[vreg] = fake_type;
        if (i_maybe_undefined)
            entry_descriptor->potentially_undefined.set(vreg);
        entry_descriptor->args[vregs.getVReg(iter_str)] = fake_type;
        phis = computeRequiredPhis(entry_descriptor, liveness.get());
    } else {
        phis = computeRequiredPhis(ParamNames(func->args, *module->interned_strings), cfg, liveness.get());
    }

    // First, verify that we require phi nodes for the block we enter into.
    // This is somewhat tricky since the osr entry represents an extra entry
    // into the BB which the analysis might not otherwise track.

    auto required_phis = phis->getAllRequiredFor(backedge->target);
    EXPECT_EQ(1, required_phis[vregs.getVReg(i_str)]);
    EXPECT_EQ(1, required_phis[vregs.getVReg(iter_str)]);
    EXPECT_EQ(2, required_phis.numSet());

    EXPECT_EQ(!is_osr || i_maybe_undefined, phis->isPotentiallyUndefinedAt(vregs.getVReg(i_str), backedge->target));
    EXPECT_FALSE(phis->isPotentiallyUndefinedAt(vregs.getVReg(iter_str), backedge->target));
    EXPECT_EQ(!is_osr || i_maybe_undefined, phis->isPotentiallyUndefinedAfter(vregs.getVReg(i_str), loop_backedge));
    EXPECT_FALSE(phis->isPotentiallyUndefinedAfter(vregs.getVReg(iter_str), loop_backedge));

    // Now, let's verify that we don't need a phi after the loop

    CFGBlock* if_join = cfg->blocks[7];
    ASSERT_EQ(8, if_join->idx);
    ASSERT_EQ(2, if_join->predecessors.size());

    if (is_osr)
        EXPECT_EQ(0, phis->getAllRequiredFor(if_join).numSet());
    else
        EXPECT_EQ(1, phis->getAllRequiredFor(if_join).numSet());
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
#endif
