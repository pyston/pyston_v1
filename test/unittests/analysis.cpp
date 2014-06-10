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

TEST(func_analysis, augassign) {
    AST_Module* module = caching_parse("../test/unittests/analysis_listcomp.py");
    assert(module);

    ScopingAnalysis *scoping = runScopingAnalysis(module);

    assert(module->body[0]->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* func = static_cast<AST_FunctionDef*>(module->body[0]);

    ScopeInfo* scope_info = scoping->getScopeInfoForNode(func);
    ASSERT_FALSE(scope_info->refersToGlobal("a"));
    ASSERT_FALSE(scope_info->refersToGlobal("b"));

    CFG* cfg = computeCFG(func->type, func->body);
    LivenessAnalysis* liveness = computeLivenessInfo(cfg);

    //cfg->print();

    for (CFGBlock* block : cfg->blocks) {
        //printf("%d\n", block->idx);
        if (block->body.back()->type != AST_TYPE::Return)
            ASSERT_TRUE(liveness->isLiveAtEnd("a", block));
    }

    PhiAnalysis* phis = computeRequiredPhis(func->args, cfg, liveness, scope_info);
}

