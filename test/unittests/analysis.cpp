#include <memory>
#include <unordered_map>
#include <vector>
#include <unordered_set>

#include "gtest/gtest.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/parser.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "unittests.h"

using namespace pyston;

class AnalysisTest : public ::testing::Test {
protected:
    virtual void SetUp() {
        initCodegen();
    }
};

TEST_F(AnalysisTest, augassign) {
    const std::string fn("../test/unittests/analysis_listcomp.py");
    AST_Module* module = caching_parse(fn.c_str());
    assert(module);

    ScopingAnalysis *scoping = runScopingAnalysis(module);

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);
    ASSERT_FALSE(scope_info->refersToGlobal("a"));
    ASSERT_FALSE(scope_info->refersToGlobal("b"));

    SourceInfo* si = new SourceInfo(createModule("__main__", fn), scoping, func, func->body);

    CFG* cfg = computeCFG(si, func->body);
    LivenessAnalysis* liveness = computeLivenessInfo(cfg);

    //cfg->print();

    for (CFGBlock* block : cfg->blocks) {
        //printf("%d\n", block->idx);
        if (block->body.back()->type != AST_TYPE::Return)
            ASSERT_TRUE(liveness->isLiveAtEnd("a", block));
    }

    PhiAnalysis* phis = computeRequiredPhis(SourceInfo::ArgNames(func), cfg, liveness, scope_info);
}

