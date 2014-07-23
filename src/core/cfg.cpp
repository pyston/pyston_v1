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

#include "core/cfg.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "analysis/scoping_analysis.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

void CFGBlock::connectTo(CFGBlock* successor, bool allow_backedge) {
    assert(successors.size() <= 1);

    if (!allow_backedge) {
        assert(this->idx >= 0);
        ASSERT(successor->idx == -1 || successor->idx > this->idx, "edge from %d to %d", this->idx, successor->idx);
    }
    // assert(successors.count(successor) == 0);
    // assert(successor->predecessors.count(this) == 0);

    successors.push_back(successor);
    successor->predecessors.push_back(this);
}

void CFGBlock::unconnectFrom(CFGBlock* successor) {
    // assert(successors.count(successor));
    // assert(successor->predecessors.count(this));
    successors.erase(std::remove(successors.begin(), successors.end(), successor), successors.end());
    successor->predecessors.erase(std::remove(successor->predecessors.begin(), successor->predecessors.end(), this),
                                  successor->predecessors.end());
}

static AST_Name* makeName(const std::string& id, AST_TYPE::AST_TYPE ctx_type, int lineno = 0, int col_offset = 0) {
    AST_Name* name = new AST_Name();
    name->id = id;
    name->col_offset = col_offset;
    name->lineno = lineno;
    name->ctx_type = ctx_type;
    return name;
}

class CFGVisitor : public ASTVisitor {
private:
    AST_TYPE::AST_TYPE root_type;
    CFG* cfg;
    CFGBlock* curblock;

    struct LoopInfo {
        CFGBlock* continue_dest, *break_dest;
    };
    std::vector<LoopInfo> loops;
    std::vector<CFGBlock*> returns;

    struct ExcBlockInfo {
        CFGBlock* exc_dest;
        std::string exc_obj_name;
    };
    std::vector<ExcBlockInfo> exc_handlers;

    void pushLoop(CFGBlock* continue_dest, CFGBlock* break_dest) {
        LoopInfo loop;
        loop.continue_dest = continue_dest;
        loop.break_dest = break_dest;
        loops.push_back(loop);
    }

    void popLoop() { loops.pop_back(); }

    void pushReturn(CFGBlock* return_dest) { returns.push_back(return_dest); }

    void popReturn() { returns.pop_back(); }

    void doReturn(AST_expr* value) {
        assert(value);
        CFGBlock* rtn_dest = getReturn();
        if (rtn_dest != NULL) {
            push_back(makeAssign("#rtnval", value));

            AST_Jump* j = makeJump();
            j->target = rtn_dest;
            curblock->connectTo(rtn_dest);
            push_back(j);
        } else {
            AST_Return* node = new AST_Return();
            node->value = value;
            node->col_offset = value->col_offset;
            node->lineno = value->lineno;
            push_back(node);
        }
        curblock = NULL;
    }

    CFGBlock* getContinue() {
        assert(loops.size());
        return loops.back().continue_dest;
    }

    CFGBlock* getBreak() {
        assert(loops.size());
        return loops.back().break_dest;
    }

    CFGBlock* getReturn() {
        if (returns.size())
            return returns.back();
        return NULL;
    }

    AST_expr* applyComprehensionCall(AST_DictComp* node, AST_Name* name) {
        AST_expr* key = remapExpr(node->key);
        AST_expr* value = remapExpr(node->value);
        return makeCall(makeLoadAttribute(name, "__setitem__", true), key, value);
    }

    AST_expr* applyComprehensionCall(AST_ListComp* node, AST_Name* name) {
        AST_expr* elt = remapExpr(node->elt);
        return makeCall(makeLoadAttribute(name, "append", true), elt);
    }

    template <typename ResultASTType, typename CompType> AST_expr* remapComprehension(CompType* node) {
        std::string rtn_name = nodeName(node);
        push_back(makeAssign(rtn_name, new ResultASTType()));
        std::vector<CFGBlock*> exit_blocks;

        // Where the current level should jump to after finishing its iteration.
        // For the outermost comprehension, this is NULL, and it doesn't jump anywhere;
        // for the inner comprehensions, they should jump to the next-outer comprehension
        // when they are done iterating.
        CFGBlock* finished_block = NULL;

        for (int i = 0, n = node->generators.size(); i < n; i++) {
            AST_comprehension* c = node->generators[i];
            bool is_innermost = (i == n - 1);

            AST_expr* remapped_iter = remapExpr(c->iter);
            AST_expr* iter_attr = makeLoadAttribute(remapped_iter, "__iter__", true);
            AST_expr* iter_call = makeCall(iter_attr);
            std::string iter_name = nodeName(node, "lc_iter", i);
            AST_stmt* iter_assign = makeAssign(iter_name, iter_call);
            push_back(iter_assign);

            // TODO bad to save these like this?
            AST_expr* hasnext_attr = makeLoadAttribute(makeName(iter_name, AST_TYPE::Load), "__hasnext__", true);
            AST_expr* next_attr = makeLoadAttribute(makeName(iter_name, AST_TYPE::Load), "next", true);

            AST_Jump* j;

            CFGBlock* test_block = cfg->addBlock();
            test_block->info = "comprehension_test";
            // printf("Test block for comp %d is %d\n", i, test_block->idx);

            j = new AST_Jump();
            j->target = test_block;
            curblock->connectTo(test_block);
            push_back(j);

            curblock = test_block;
            AST_expr* test_call = remapExpr(makeCall(hasnext_attr));

            CFGBlock* body_block = cfg->addBlock();
            body_block->info = "comprehension_body";
            CFGBlock* exit_block = cfg->addDeferredBlock();
            exit_block->info = "comprehension_exit";
            exit_blocks.push_back(exit_block);
            // printf("Body block for comp %d is %d\n", i, body_block->idx);

            AST_Branch* br = new AST_Branch();
            br->col_offset = node->col_offset;
            br->lineno = node->lineno;
            br->test = test_call;
            br->iftrue = body_block;
            br->iffalse = exit_block;
            curblock->connectTo(body_block);
            curblock->connectTo(exit_block);
            push_back(br);

            curblock = body_block;
            push_back(makeAssign(c->target, makeCall(next_attr)));

            for (AST_expr* if_condition : c->ifs) {
                AST_expr* remapped = remapExpr(if_condition);
                AST_Branch* br = new AST_Branch();
                br->test = remapped;
                push_back(br);

                // Put this below the entire body?
                CFGBlock* body_tramp = cfg->addBlock();
                body_tramp->info = "comprehension_if_trampoline";
                // printf("body_tramp for %d is %d\n", i, body_tramp->idx);
                CFGBlock* body_continue = cfg->addBlock();
                body_continue->info = "comprehension_if_continue";
                // printf("body_continue for %d is %d\n", i, body_continue->idx);

                br->iffalse = body_tramp;
                curblock->connectTo(body_tramp);
                br->iftrue = body_continue;
                curblock->connectTo(body_continue);

                curblock = body_tramp;
                j = new AST_Jump();
                j->target = test_block;
                push_back(j);
                curblock->connectTo(test_block, true);

                curblock = body_continue;
            }

            CFGBlock* body_end = curblock;

            assert((finished_block != NULL) == (i != 0));
            if (finished_block) {
                curblock = exit_block;
                j = new AST_Jump();
                j->target = finished_block;
                curblock->connectTo(finished_block, true);
                push_back(j);
            }
            finished_block = test_block;

            curblock = body_end;
            if (is_innermost) {
                push_back(makeExpr(applyComprehensionCall(node, makeName(rtn_name, AST_TYPE::Load))));

                j = new AST_Jump();
                j->target = test_block;
                curblock->connectTo(test_block, true);
                push_back(j);

                assert(exit_blocks.size());
                curblock = exit_blocks[0];
            } else {
                // continue onto the next comprehension and add to this body
            }
        }

        // Wait until the end to place the end blocks, so that
        // we get a nice nesting structure, that looks similar to what
        // you'd get with a nested for loop:
        for (int i = exit_blocks.size() - 1; i >= 0; i--) {
            cfg->placeBlock(exit_blocks[i]);
            // printf("Exit block for comp %d is %d\n", i, exit_blocks[i]->idx);
        }

        return makeName(rtn_name, AST_TYPE::Load);
    }


    AST_expr* makeNum(int n) {
        AST_Num* node = new AST_Num();
        node->num_type = AST_Num::INT;
        node->n_int = n;
        return node;
    }

    AST_Jump* makeJump() {
        AST_Jump* rtn = new AST_Jump();
        return rtn;
    }

    AST_Branch* makeBranch(AST_expr* test) {
        AST_Branch* rtn = new AST_Branch();
        rtn->test = test;
        rtn->col_offset = test->col_offset;
        rtn->lineno = test->lineno;
        return rtn;
    }

    AST_expr* makeLoadAttribute(AST_expr* base, const std::string& name, bool clsonly) {
        AST_expr* rtn;
        if (clsonly) {
            AST_ClsAttribute* attr = new AST_ClsAttribute();
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        } else {
            AST_Attribute* attr = new AST_Attribute();
            attr->ctx_type = AST_TYPE::Load;
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        }
        rtn->col_offset = base->col_offset;
        rtn->lineno = base->lineno;
        return rtn;
    }

    AST_Call* makeCall(AST_expr* func) {
        AST_Call* call = new AST_Call();
        call->starargs = NULL;
        call->kwargs = NULL;
        call->func = func;
        call->col_offset = func->col_offset;
        call->lineno = func->lineno;
        return call;
    }

    AST_Call* makeCall(AST_expr* func, AST_expr* arg0) {
        AST_Call* call = new AST_Call();
        call->args.push_back(arg0);
        call->starargs = NULL;
        call->kwargs = NULL;
        call->func = func;
        call->col_offset = func->col_offset;
        call->lineno = func->lineno;
        return call;
    }

    AST_Call* makeCall(AST_expr* func, AST_expr* arg0, AST_expr* arg1) {
        AST_Call* call = new AST_Call();
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        call->starargs = NULL;
        call->kwargs = NULL;
        call->func = func;
        call->col_offset = func->col_offset;
        call->lineno = func->lineno;
        return call;
    }

    AST_stmt* makeAssign(AST_expr* target, AST_expr* val) {
        AST_Assign* assign = new AST_Assign();
        assign->targets.push_back(target);
        assign->value = val;
        assign->col_offset = val->col_offset;
        assign->lineno = val->lineno;
        return assign;
    }

    AST_stmt* makeAssign(const std::string& id, AST_expr* val) {
        assert(val);
        AST_expr* name = makeName(id, AST_TYPE::Store, val->lineno, 0);
        return makeAssign(name, val);
    }

    AST_stmt* makeExpr(AST_expr* expr) {
        AST_Expr* stmt = new AST_Expr();
        stmt->value = expr;
        stmt->lineno = expr->lineno;
        stmt->col_offset = expr->col_offset;
        return stmt;
    }



    std::string nodeName(AST* node) {
        char buf[40];
        snprintf(buf, 40, "#%p", node);
        return std::string(buf);
    }

    std::string nodeName(AST_expr* node, const std::string& suffix, int idx) {
        char buf[50];
        snprintf(buf, 50, "#%p_%s_%d", node, suffix.c_str(), idx);
        return std::string(buf);
    }

    AST_expr* remapAttribute(AST_Attribute* node) {
        AST_Attribute* rtn = new AST_Attribute();

        rtn->col_offset = node->col_offset;
        rtn->lineno = node->lineno;
        rtn->ctx_type = node->ctx_type;
        rtn->attr = node->attr;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    AST_expr* remapBinOp(AST_BinOp* node) {
        AST_BinOp* rtn = new AST_BinOp();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->op_type = node->op_type;
        rtn->left = remapExpr(node->left);
        rtn->right = remapExpr(node->right);
        return rtn;
    }

    AST_expr* remapBoolOp(AST_BoolOp* node) {
        std::string name = nodeName(node);

        CFGBlock* starting_block = curblock;
        CFGBlock* exit_block = cfg->addDeferredBlock();

        for (int i = 0; i < node->values.size() - 1; i++) {
            AST_expr* val = remapExpr(node->values[i]);
            push_back(makeAssign(name, val));

            AST_Branch* br = new AST_Branch();
            br->test = val;
            push_back(br);

            CFGBlock* was_block = curblock;
            CFGBlock* next_block = cfg->addBlock();
            CFGBlock* crit_break_block = cfg->addBlock();
            was_block->connectTo(next_block);
            was_block->connectTo(crit_break_block);

            if (node->op_type == AST_TYPE::Or) {
                br->iftrue = crit_break_block;
                br->iffalse = next_block;
            } else {
                br->iffalse = crit_break_block;
                br->iftrue = next_block;
            }

            curblock = crit_break_block;
            AST_Jump* j = new AST_Jump();
            j->target = exit_block;
            push_back(j);
            crit_break_block->connectTo(exit_block);

            curblock = next_block;
        }

        AST_expr* final_val = remapExpr(node->values[node->values.size() - 1]);
        push_back(makeAssign(name, final_val));

        AST_Jump* j = new AST_Jump();
        push_back(j);
        j->target = exit_block;
        curblock->connectTo(exit_block);

        cfg->placeBlock(exit_block);
        curblock = exit_block;

        return makeName(name, AST_TYPE::Load);
    }

    AST_expr* remapCall(AST_Call* node) {
        AST_Call* rtn = new AST_Call();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;

        if (node->func->type == AST_TYPE::Attribute) {
            // TODO this is a cludge to make sure that "callattrs" stick together.
            // Probably better to create an AST_Callattr type, and solidify the
            // idea that a callattr is a single expression.
            rtn->func = remapAttribute(ast_cast<AST_Attribute>(node->func));
        } else if (node->func->type == AST_TYPE::ClsAttribute) {
            // TODO this is a cludge to make sure that "callattrs" stick together.
            // Probably better to create an AST_Callattr type, and solidify the
            // idea that a callattr is a single expression.
            rtn->func = remapClsAttribute(ast_cast<AST_ClsAttribute>(node->func));
        } else {
            rtn->func = remapExpr(node->func);
        }

        for (auto e : node->args) {
            rtn->args.push_back(remapExpr(e));
        }
        for (auto e : node->keywords) {
            AST_keyword* kw = new AST_keyword();
            kw->value = remapExpr(e->value);
            kw->arg = e->arg;
            rtn->keywords.push_back(kw);
        }
        rtn->starargs = remapExpr(node->starargs);
        rtn->kwargs = remapExpr(node->kwargs);

        return rtn;
    }

    AST_expr* remapClsAttribute(AST_ClsAttribute* node) {
        AST_ClsAttribute* rtn = new AST_ClsAttribute();

        rtn->col_offset = node->col_offset;
        rtn->lineno = node->lineno;
        rtn->attr = node->attr;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    AST_expr* remapCompare(AST_Compare* node) {
        // special case unchained comparisons to avoid generating a unnecessary complex cfg.
        if (node->ops.size() == 1) {
            AST_Compare* rtn = new AST_Compare();
            rtn->lineno = node->lineno;
            rtn->col_offset = node->col_offset;

            rtn->ops = node->ops;

            rtn->left = remapExpr(node->left);
            for (auto elt : node->comparators) {
                rtn->comparators.push_back(remapExpr(elt));
            }
            return rtn;
        } else {
            std::string name = nodeName(node);

            CFGBlock* exit_block = cfg->addDeferredBlock();
            AST_expr* left = remapExpr(node->left);

            for (int i = 0; i < node->ops.size(); i++) {
                AST_expr* right = remapExpr(node->comparators[i]);

                AST_Compare* val = new AST_Compare;
                val->col_offset = node->col_offset;
                val->lineno = node->lineno;
                val->left = left;
                val->comparators.push_back(right);
                val->ops.push_back(node->ops[i]);

                push_back(makeAssign(name, val));

                AST_Branch* br = new AST_Branch();
                br->test = makeName(name, AST_TYPE::Load);
                push_back(br);

                CFGBlock* was_block = curblock;
                CFGBlock* next_block = cfg->addBlock();
                CFGBlock* crit_break_block = cfg->addBlock();
                was_block->connectTo(next_block);
                was_block->connectTo(crit_break_block);

                br->iffalse = crit_break_block;
                br->iftrue = next_block;

                curblock = crit_break_block;
                AST_Jump* j = new AST_Jump();
                j->target = exit_block;
                push_back(j);
                crit_break_block->connectTo(exit_block);

                curblock = next_block;

                left = right;
            }

            AST_Jump* j = new AST_Jump();
            push_back(j);
            j->target = exit_block;
            curblock->connectTo(exit_block);

            cfg->placeBlock(exit_block);
            curblock = exit_block;

            return makeName(name, AST_TYPE::Load);
        }
    }

    AST_expr* remapDict(AST_Dict* node) {
        AST_Dict* rtn = new AST_Dict();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;

        for (auto k : node->keys) {
            rtn->keys.push_back(remapExpr(k));
        }
        for (auto v : node->values) {
            rtn->values.push_back(remapExpr(v));
        }

        return rtn;
    };

    AST_expr* remapIfExp(AST_IfExp* node) {
        std::string rtn_name = nodeName(node);

        AST_expr* test = remapExpr(node->test);

        CFGBlock* starting_block = curblock;
        AST_Branch* br = new AST_Branch();
        br->col_offset = node->col_offset;
        br->lineno = node->lineno;
        br->test = node->test;
        push_back(br);

        CFGBlock* iftrue = cfg->addBlock();
        iftrue->info = "iftrue";
        br->iftrue = iftrue;
        starting_block->connectTo(iftrue);
        curblock = iftrue;
        push_back(makeAssign(rtn_name, remapExpr(node->body)));
        AST_Jump* jtrue = new AST_Jump();
        push_back(jtrue);
        CFGBlock* endtrue = curblock;

        CFGBlock* iffalse = cfg->addBlock();
        iffalse->info = "iffalse";
        br->iffalse = iffalse;
        starting_block->connectTo(iffalse);
        curblock = iffalse;
        push_back(makeAssign(rtn_name, remapExpr(node->orelse)));
        AST_Jump* jfalse = new AST_Jump();
        push_back(jfalse);
        CFGBlock* endfalse = curblock;

        CFGBlock* exit_block = cfg->addBlock();
        jtrue->target = exit_block;
        endtrue->connectTo(exit_block);
        jfalse->target = exit_block;
        endfalse->connectTo(exit_block);
        curblock = exit_block;

        return makeName(rtn_name, AST_TYPE::Load);
    }

    AST_expr* remapIndex(AST_Index* node) {
        AST_Index* rtn = new AST_Index();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    AST_expr* remapLambda(AST_Lambda* node) {
        // Remap in place: see note in visit_functiondef for why.

        for (int i = 0; i < node->args->defaults.size(); i++) {
            node->args->defaults[i] = remapExpr(node->args->defaults[i]);
        }

        return node;
    }

    AST_expr* remapLangPrimitive(AST_LangPrimitive* node) {
        AST_LangPrimitive* rtn = new AST_LangPrimitive(node->opcode);
        rtn->col_offset = node->col_offset;
        rtn->lineno = node->lineno;

        for (AST_expr* arg : node->args) {
            rtn->args.push_back(remapExpr(arg));
        }
        return rtn;
    }

    AST_expr* remapList(AST_List* node) {
        assert(node->ctx_type == AST_TYPE::Load);

        AST_List* rtn = new AST_List();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->ctx_type == node->ctx_type;

        for (auto elt : node->elts) {
            rtn->elts.push_back(remapExpr(elt));
        }
        return rtn;
    }

    AST_expr* remapRepr(AST_Repr* node) {
        AST_Repr* rtn = new AST_Repr();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    AST_expr* remapSlice(AST_Slice* node) {
        AST_Slice* rtn = new AST_Slice();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;

        rtn->lower = remapExpr(node->lower);
        rtn->upper = remapExpr(node->upper);
        rtn->step = remapExpr(node->step);

        return rtn;
    }

    AST_expr* remapTuple(AST_Tuple* node) {
        assert(node->ctx_type == AST_TYPE::Load);

        AST_Tuple* rtn = new AST_Tuple();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->ctx_type == node->ctx_type;

        for (auto elt : node->elts) {
            rtn->elts.push_back(remapExpr(elt));
        }
        return rtn;
    }

    AST_expr* remapSubscript(AST_Subscript* node) {
        AST_Subscript* rtn = new AST_Subscript();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->ctx_type = node->ctx_type;
        rtn->value = remapExpr(node->value);
        rtn->slice = remapExpr(node->slice);
        return rtn;
    }

    AST_expr* remapUnaryOp(AST_UnaryOp* node) {
        AST_UnaryOp* rtn = new AST_UnaryOp();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->op_type = node->op_type;
        rtn->operand = remapExpr(node->operand);
        return rtn;
    }

    AST_expr* remapExpr(AST_expr* node, bool wrap_with_assign = true) {
        if (node == NULL)
            return NULL;

        AST_expr* rtn;
        switch (node->type) {
            case AST_TYPE::Attribute:
                rtn = remapAttribute(ast_cast<AST_Attribute>(node));
                break;
            case AST_TYPE::BinOp:
                rtn = remapBinOp(ast_cast<AST_BinOp>(node));
                break;
            case AST_TYPE::BoolOp:
                rtn = remapBoolOp(ast_cast<AST_BoolOp>(node));
                break;
            case AST_TYPE::Call:
                rtn = remapCall(ast_cast<AST_Call>(node));
                break;
            case AST_TYPE::ClsAttribute:
                rtn = remapClsAttribute(ast_cast<AST_ClsAttribute>(node));
                break;
            case AST_TYPE::Compare:
                rtn = remapCompare(ast_cast<AST_Compare>(node));
                break;
            case AST_TYPE::Dict:
                rtn = remapDict(ast_cast<AST_Dict>(node));
                break;
            case AST_TYPE::DictComp:
                rtn = remapComprehension<AST_Dict>(ast_cast<AST_DictComp>(node));
                break;
            case AST_TYPE::IfExp:
                rtn = remapIfExp(ast_cast<AST_IfExp>(node));
                break;
            case AST_TYPE::Index:
                rtn = remapIndex(ast_cast<AST_Index>(node));
                break;
            case AST_TYPE::Lambda:
                rtn = remapLambda(ast_cast<AST_Lambda>(node));
                break;
            case AST_TYPE::LangPrimitive:
                rtn = remapLangPrimitive(ast_cast<AST_LangPrimitive>(node));
                break;
            case AST_TYPE::List:
                rtn = remapList(ast_cast<AST_List>(node));
                break;
            case AST_TYPE::ListComp:
                rtn = remapComprehension<AST_List>(ast_cast<AST_ListComp>(node));
                break;
            case AST_TYPE::Name:
                rtn = node;
                break;
            case AST_TYPE::Num:
                return node;
            case AST_TYPE::Repr:
                rtn = remapRepr(ast_cast<AST_Repr>(node));
                break;
            case AST_TYPE::Slice:
                rtn = remapSlice(ast_cast<AST_Slice>(node));
                break;
            case AST_TYPE::Str:
                return node;
            case AST_TYPE::Subscript:
                rtn = remapSubscript(ast_cast<AST_Subscript>(node));
                break;
            case AST_TYPE::Tuple:
                rtn = remapTuple(ast_cast<AST_Tuple>(node));
                break;
            case AST_TYPE::UnaryOp:
                rtn = remapUnaryOp(ast_cast<AST_UnaryOp>(node));
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->type);
        }

        if (wrap_with_assign && (rtn->type != AST_TYPE::Name || ast_cast<AST_Name>(rtn)->id[0] != '#')) {
            std::string name = nodeName(node);
            push_back(makeAssign(name, rtn));
            return makeName(name, AST_TYPE::Load);
        } else {
            return rtn;
        }
    }

public:
    CFGVisitor(AST_TYPE::AST_TYPE root_type, CFG* cfg) : root_type(root_type), cfg(cfg) {
        curblock = cfg->addBlock();
        curblock->info = "entry";
    }

    ~CFGVisitor() {
        assert(loops.size() == 0);
        assert(returns.size() == 0);
        assert(exc_handlers.size() == 0);
    }

    void push_back(AST_stmt* node) {
        assert(node->type != AST_TYPE::Invoke);

        if (!curblock)
            return;

        if (exc_handlers.size() == 0) {
            curblock->push_back(node);
            return;
        }

        AST_TYPE::AST_TYPE type = node->type;
        if (type == AST_TYPE::Jump) {
            curblock->push_back(node);
            return;
        }

        if (type == AST_TYPE::Branch) {
            AST_TYPE::AST_TYPE test_type = ast_cast<AST_Branch>(node)->test->type;
            ASSERT(test_type == AST_TYPE::Name || test_type == AST_TYPE::Num, "%d", test_type);
            curblock->push_back(node);
            return;
        }

        if (type == AST_TYPE::Return) {
            curblock->push_back(node);
            return;
        }

        CFGBlock* normal_dest = cfg->addBlock();
        // Add an extra exc_dest trampoline to prevent critical edges:
        CFGBlock* exc_dest = cfg->addBlock();

        AST_Invoke* invoke = new AST_Invoke(node);
        invoke->normal_dest = normal_dest;
        invoke->exc_dest = exc_dest;
        invoke->col_offset = node->col_offset;
        invoke->lineno = node->lineno;

        curblock->push_back(invoke);
        curblock->connectTo(normal_dest);
        curblock->connectTo(exc_dest);

        ExcBlockInfo& exc_info = exc_handlers.back();

        curblock = exc_dest;
        curblock->push_back(makeAssign(exc_info.exc_obj_name, new AST_LangPrimitive(AST_LangPrimitive::LANDINGPAD)));

        AST_Jump* j = new AST_Jump();
        j->target = exc_info.exc_dest;
        curblock->push_back(j);
        curblock->connectTo(exc_info.exc_dest);

        curblock = normal_dest;
    }

    virtual bool visit_classdef(AST_ClassDef* node) {
        // Remap in place: see note in visit_functiondef for why.

        // Decorators are evaluated before the defaults:
        for (int i = 0; i < node->decorator_list.size(); i++) {
            node->decorator_list[i] = remapExpr(node->decorator_list[i]);
        }

        for (int i = 0; i < node->bases.size(); i++) {
            node->bases[i] = remapExpr(node->bases[i]);
        }

        push_back(node);
        return true;
    }

    virtual bool visit_functiondef(AST_FunctionDef* node) {
        // As much as I don't like it, for now we're remapping these in place.
        // This is because we do certain analyses pre-remapping, and associate the
        // results with the node.  We can either do some refactoring and have a way
        // of associating the new node with the same results, or just do the remapping
        // in-place.
        // Doing it in-place seems ugly, but I can't think of anything it should break,
        // so just do that for now.
        // TODO If we remap these (functiondefs, lambdas, classdefs) in place, we should probably
        // remap everything in place?

        // Decorators are evaluated before the defaults:
        for (int i = 0; i < node->decorator_list.size(); i++) {
            node->decorator_list[i] = remapExpr(node->decorator_list[i]);
        }

        for (int i = 0; i < node->args->defaults.size(); i++) {
            node->args->defaults[i] = remapExpr(node->args->defaults[i]);
        }

        push_back(node);
        return true;
    }

    virtual bool visit_global(AST_Global* node) {
        push_back(node);
        return true;
    }

    virtual bool visit_import(AST_Import* node) {
        push_back(node);
        return true;
    }

    virtual bool visit_importfrom(AST_ImportFrom* node) {
        push_back(node);
        return true;
    }

    virtual bool visit_pass(AST_Pass* node) { return true; }

    bool visit_assert(AST_Assert* node) override {
        AST_Branch* br = new AST_Branch();
        br->test = remapExpr(node->test);
        push_back(br);

        CFGBlock* iffalse = cfg->addBlock();
        iffalse->info = "assert_fail";
        curblock->connectTo(iffalse);
        CFGBlock* iftrue = cfg->addBlock();
        iftrue->info = "assert_pass";
        curblock->connectTo(iftrue);
        br->iftrue = iftrue;
        br->iffalse = iffalse;

        curblock = iffalse;

        // The rest of this is pretty hacky:
        // Emit a "assert(0, msg()); while (1) {}" section that basically captures
        // what the assert will do but in a very hacky way.
        AST_Assert* remapped = new AST_Assert();
        if (node->msg)
            remapped->msg = remapExpr(node->msg);
        else
            remapped->msg = NULL;
        AST_Num* fake_test = new AST_Num();
        fake_test->num_type = AST_Num::INT;
        fake_test->n_int = 0;
        remapped->test = fake_test;
        remapped->lineno = node->lineno;
        remapped->col_offset = node->col_offset;
        push_back(remapped);

        CFGBlock* unreachable = cfg->addBlock();
        unreachable->info = "unreachable";
        curblock->connectTo(unreachable);

        AST_Jump* j = new AST_Jump();
        j->target = unreachable;
        push_back(j);

        curblock = unreachable;
        push_back(j);
        curblock->connectTo(unreachable, true);

        curblock = iftrue;

        return true;
    }

    virtual bool visit_assign(AST_Assign* node) {
        AST_expr* remapped_value = remapExpr(node->value);

        for (AST_expr* target : node->targets) {
            AST_Assign* remapped = new AST_Assign();
            remapped->lineno = node->lineno;
            remapped->col_offset = node->col_offset;
            remapped->value = remapped_value;
            remapped->targets.push_back(target);
            push_back(remapped);
        }
        return true;
    }

    virtual bool visit_augassign(AST_AugAssign* node) {
        // augassign is pretty tricky; "x" += "y" mostly textually maps to
        // "x" = "x" =+ "y" (using "=+" to represent an augbinop)
        // except that "x" only gets evaluated once.  So it's something like
        // "target", val = eval("x")
        // "target" = val =+ "y"
        // where "target" is handled specially, because it can't just be a name;
        // it has to be a name-only version of the target type (ex subscript, attribute).
        // So for "f().x += g()", it has to translate to
        // "c = f(); y = c.x; z = g(); c.x = y =+ z"
        //
        // Even if the target is a simple name, it can be complicated, because the
        // value can change the name.  For "x += f()", have to translate to
        // "y = x; z = f(); x = y =+ z"

        AST_expr* remapped_target;
        AST_expr* remapped_lhs;

        // TODO bad that it's reusing the AST nodes?
        switch (node->target->type) {
            case AST_TYPE::Name: {
                AST_Name* n = ast_cast<AST_Name>(node->target);
                assert(n->ctx_type == AST_TYPE::Store);
                push_back(makeAssign(nodeName(node), makeName(n->id, AST_TYPE::Load)));
                remapped_target = n;
                remapped_lhs = makeName(nodeName(node), AST_TYPE::Load);
                break;
            }
            case AST_TYPE::Subscript: {
                AST_Subscript* s = ast_cast<AST_Subscript>(node->target);
                assert(s->ctx_type == AST_TYPE::Store);

                AST_Subscript* s_target = new AST_Subscript();
                s_target->value = remapExpr(s->value);
                s_target->slice = remapExpr(s->slice);
                s_target->ctx_type = AST_TYPE::Store;
                s_target->col_offset = s->col_offset;
                s_target->lineno = s->lineno;
                remapped_target = s_target;

                AST_Subscript* s_lhs = new AST_Subscript();
                s_lhs->value = s_target->value;
                s_lhs->slice = s_target->slice;
                s_lhs->col_offset = s->col_offset;
                s_lhs->lineno = s->lineno;
                s_lhs->ctx_type = AST_TYPE::Load;
                remapped_lhs = remapExpr(s_lhs);

                break;
            }
            case AST_TYPE::Attribute: {
                AST_Attribute* a = ast_cast<AST_Attribute>(node->target);
                assert(a->ctx_type == AST_TYPE::Store);

                AST_Attribute* a_target = new AST_Attribute();
                a_target->value = remapExpr(a->value);
                a_target->attr = a->attr;
                a_target->ctx_type = AST_TYPE::Store;
                a_target->col_offset = a->col_offset;
                a_target->lineno = a->lineno;
                remapped_target = a_target;

                AST_Attribute* a_lhs = new AST_Attribute();
                a_lhs->value = a_target->value;
                a_lhs->attr = a->attr;
                a_lhs->ctx_type = AST_TYPE::Load;
                a_lhs->col_offset = a->col_offset;
                a_lhs->lineno = a->lineno;
                remapped_lhs = remapExpr(a_lhs);

                break;
            }
            default:
                RELEASE_ASSERT(0, "%d", node->target->type);
        }

        AST_AugBinOp* binop = new AST_AugBinOp();
        binop->op_type = node->op_type;
        binop->left = remapped_lhs;
        binop->right = remapExpr(node->value);
        binop->col_offset = node->col_offset;
        binop->lineno = node->lineno;
        AST_stmt* assign = makeAssign(remapped_target, binop);
        push_back(assign);
        return true;
    }

    virtual bool visit_delete(AST_Delete* node) {
        for (auto t : node->targets) {
            AST_Delete* astdel = new AST_Delete();
            astdel->lineno = node->lineno;
            astdel->col_offset = node->col_offset;
            AST_expr* target = NULL;
            switch (t->type) {
                case AST_TYPE::Subscript: {
                    AST_Subscript* s = static_cast<AST_Subscript*>(t);
                    AST_Subscript* astsubs = new AST_Subscript();
                    astsubs->value = remapExpr(s->value);
                    astsubs->slice = remapExpr(s->slice);
                    astsubs->ctx_type = AST_TYPE::Del;
                    target = astsubs;
                    break;
                }
                default:
                    RELEASE_ASSERT(0, "UnSupported del target: %d", t->type);
            }
            astdel->targets.push_back(target);
            push_back(astdel);
        }

        return true;
    }

    virtual bool visit_expr(AST_Expr* node) {
        AST_Expr* remapped = new AST_Expr();
        remapped->lineno = node->lineno;
        remapped->col_offset = node->col_offset;
        remapped->value = remapExpr(node->value, false);
        push_back(remapped);
        return true;
    }

    virtual bool visit_print(AST_Print* node) {
        AST_expr* dest = remapExpr(node->dest);

        int i = 0;
        for (auto v : node->values) {
            AST_Print* remapped = new AST_Print();
            remapped->col_offset = node->col_offset;
            remapped->lineno = node->lineno;
            // TODO not good to reuse 'dest' like this
            remapped->dest = dest;

            if (i < node->values.size() - 1)
                remapped->nl = false;
            else
                remapped->nl = node->nl;

            remapped->values.push_back(remapExpr(v));
            push_back(remapped);

            i++;
        }

        if (node->values.size() == 0) {
            assert(node->nl);

            AST_Print* final = new AST_Print();
            final->col_offset = node->col_offset;
            final->lineno = node->lineno;
            // TODO not good to reuse 'dest' like this
            final->dest = dest;
            final->nl = node->nl;
            push_back(final);
        }

        return true;
    }

    virtual bool visit_return(AST_Return* node) {
        if (root_type != AST_TYPE::FunctionDef && root_type != AST_TYPE::Lambda) {
            fprintf(stderr, "SyntaxError: 'return' outside function\n");
            exit(1);
        }

        AST_expr* value = remapExpr(node->value);
        if (value == NULL)
            value = makeName("None", AST_TYPE::Load);
        doReturn(value);
        return true;
    }

    virtual bool visit_if(AST_If* node) {
        if (!curblock)
            return true;

        AST_Branch* br = new AST_Branch();
        br->col_offset = node->col_offset;
        br->lineno = node->lineno;
        br->test = remapExpr(node->test);
        push_back(br);

        CFGBlock* starting_block = curblock;
        CFGBlock* exit = cfg->addDeferredBlock();
        exit->info = "ifexit";

        CFGBlock* iftrue = cfg->addBlock();
        iftrue->info = "iftrue";
        br->iftrue = iftrue;
        starting_block->connectTo(iftrue);
        curblock = iftrue;
        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
        }
        if (curblock) {
            AST_Jump* jtrue = new AST_Jump();
            push_back(jtrue);
            jtrue->target = exit;
            curblock->connectTo(exit);
        }

        CFGBlock* iffalse = cfg->addBlock();
        br->iffalse = iffalse;
        starting_block->connectTo(iffalse);

        iffalse->info = "iffalse";
        curblock = iffalse;
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
        }
        if (curblock) {
            AST_Jump* jfalse = new AST_Jump();
            push_back(jfalse);
            jfalse->target = exit;
            curblock->connectTo(exit);
        }

        if (exit->predecessors.size() == 0) {
            curblock = NULL;
        } else {
            cfg->placeBlock(exit);
            curblock = exit;
        }

        return true;
    }

    virtual bool visit_break(AST_Break* node) {
        if (!curblock)
            return true;

        if (loops.size() == 0) {
            fprintf(stderr, "SyntaxError: 'break' outside loop\n");
            exit(1);
        }

        AST_Jump* j = makeJump();
        push_back(j);
        assert(loops.size());
        j->target = getBreak();
        curblock->connectTo(j->target, true);

        curblock = NULL;
        return true;
    }

    virtual bool visit_continue(AST_Continue* node) {
        if (!curblock)
            return true;

        if (loops.size() == 0) {
            // Note: error message is different than the 'break' case
            fprintf(stderr, "SyntaxError: 'continue' not properly in loop\n");
            exit(1);
        }

        AST_Jump* j = makeJump();
        push_back(j);
        assert(loops.size());
        j->target = getContinue();
        curblock->connectTo(j->target, true);

        curblock = NULL;
        return true;
    }

    virtual bool visit_while(AST_While* node) {
        if (!curblock)
            return true;

        CFGBlock* test_block = cfg->addBlock();
        test_block->info = "while_test";

        AST_Jump* j = makeJump();
        push_back(j);
        j->target = test_block;
        curblock->connectTo(test_block);

        curblock = test_block;
        AST_Branch* br = makeBranch(remapExpr(node->test));
        CFGBlock* test_block_end = curblock;
        push_back(br);

        // We need a reference to this block early on so we can break to it,
        // but we don't want it to be placed until after the orelse.
        CFGBlock* end = cfg->addDeferredBlock();
        end->info = "while_exit";
        pushLoop(test_block, end);

        CFGBlock* body = cfg->addBlock();
        body->info = "while_body_start";
        br->iftrue = body;

        test_block_end->connectTo(body);
        curblock = body;
        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
        }
        if (curblock) {
            AST_Jump* jbody = makeJump();
            push_back(jbody);
            jbody->target = test_block;
            curblock->connectTo(test_block, true);
        }
        popLoop();

        CFGBlock* orelse = cfg->addBlock();
        orelse->info = "while_orelse_start";
        br->iffalse = orelse;
        test_block_end->connectTo(orelse);
        curblock = orelse;
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
        }
        if (curblock) {
            AST_Jump* jend = makeJump();
            push_back(jend);
            jend->target = end;
            curblock->connectTo(end);
        }
        curblock = end;

        cfg->placeBlock(end);

        return true;
    }

    virtual bool visit_for(AST_For* node) {
        if (!curblock)
            return true;

        // TODO this is so complicated because I tried doing loop inversion;
        // is it really worth it?  It got so bad because all the edges became
        // critical edges and needed to be broken, otherwise it's not too different.

        AST_expr* remapped_iter = remapExpr(node->iter);
        AST_expr* iter_attr = makeLoadAttribute(remapped_iter, "__iter__", true);
        AST_expr* iter_call = makeCall(iter_attr);

        char itername_buf[80];
        snprintf(itername_buf, 80, "#iter_%p", node);
        AST_stmt* iter_assign = makeAssign(itername_buf, iter_call);
        push_back(iter_assign);

        AST_expr* hasnext_attr = makeLoadAttribute(makeName(itername_buf, AST_TYPE::Load), "__hasnext__", true);
        AST_expr* next_attr = makeLoadAttribute(makeName(itername_buf, AST_TYPE::Load), "next", true);

        CFGBlock* test_block = cfg->addBlock();
        AST_Jump* jump_to_test = makeJump();
        jump_to_test->target = test_block;
        push_back(jump_to_test);
        curblock->connectTo(test_block);
        curblock = test_block;

        AST_expr* test_call = makeCall(hasnext_attr);
        AST_Branch* test_br = makeBranch(remapExpr(test_call));

        push_back(test_br);
        CFGBlock* test_true = cfg->addBlock();
        CFGBlock* test_false = cfg->addBlock();
        test_br->iftrue = test_true;
        test_br->iffalse = test_false;
        curblock->connectTo(test_true);
        curblock->connectTo(test_false);

        CFGBlock* loop_block = cfg->addBlock();
        CFGBlock* end_block = cfg->addDeferredBlock();
        CFGBlock* else_block = cfg->addDeferredBlock();

        curblock = test_true;

        // TODO simplify the breaking of these crit edges?
        AST_Jump* test_true_jump = makeJump();
        test_true_jump->target = loop_block;
        push_back(test_true_jump);
        test_true->connectTo(loop_block);

        curblock = test_false;
        AST_Jump* test_false_jump = makeJump();
        test_false_jump->target = else_block;
        push_back(test_false_jump);
        test_false->connectTo(else_block);

        pushLoop(test_block, end_block);

        curblock = loop_block;
        push_back(makeAssign(node->target, makeCall(next_attr)));

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
        }
        popLoop();

        if (curblock) {
            AST_expr* end_call = makeCall(hasnext_attr);
            AST_Branch* end_br = makeBranch(remapExpr(end_call));
            push_back(end_br);

            CFGBlock* end_true = cfg->addBlock();
            CFGBlock* end_false = cfg->addBlock();
            end_br->iftrue = end_true;
            end_br->iffalse = end_false;
            curblock->connectTo(end_true);
            curblock->connectTo(end_false);

            curblock = end_true;
            AST_Jump* end_true_jump = makeJump();
            end_true_jump->target = loop_block;
            push_back(end_true_jump);
            end_true->connectTo(loop_block, true);

            curblock = end_false;
            AST_Jump* end_false_jump = makeJump();
            end_false_jump->target = else_block;
            push_back(end_false_jump);
            end_false->connectTo(else_block);
        }

        cfg->placeBlock(else_block);
        curblock = else_block;

        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
        }
        if (curblock) {
            AST_Jump* else_jump = makeJump();
            push_back(else_jump);
            else_jump->target = end_block;
            curblock->connectTo(end_block);
        }

        cfg->placeBlock(end_block);
        curblock = end_block;

        return true;
    }

    bool visit_raise(AST_Raise* node) override {
        AST_Raise* remapped = new AST_Raise();
        remapped->col_offset = node->col_offset;
        remapped->lineno = node->lineno;

        if (node->arg0)
            remapped->arg0 = remapExpr(node->arg0);
        if (node->arg1)
            remapped->arg1 = remapExpr(node->arg1);
        if (node->arg2)
            remapped->arg2 = remapExpr(node->arg2);
        push_back(remapped);

        if (!curblock)
            return true;

        curblock->push_back(new AST_Unreachable());
        curblock = NULL;

        return true;
    }

    bool visit_tryexcept(AST_TryExcept* node) override {
        assert(node->handlers.size() > 0);

        CFGBlock* exc_handler_block = cfg->addDeferredBlock();
        std::string exc_obj_name = nodeName(node);
        exc_handlers.push_back({ exc_handler_block, exc_obj_name });

        for (AST_stmt* subnode : node->body) {
            subnode->accept(this);
        }

        exc_handlers.pop_back();

        for (AST_stmt* subnode : node->orelse) {
            subnode->accept(this);
        }

        CFGBlock* join_block = cfg->addDeferredBlock();
        if (curblock) {
            AST_Jump* j = new AST_Jump();
            j->target = join_block;
            push_back(j);
            curblock->connectTo(join_block);
        }

        if (exc_handler_block->predecessors.size() == 0) {
            delete exc_handler_block;
        } else {
            cfg->placeBlock(exc_handler_block);
            curblock = exc_handler_block;

            AST_expr* exc_obj = makeName(exc_obj_name, AST_TYPE::Load);

            bool caught_all = false;
            for (AST_ExceptHandler* exc_handler : node->handlers) {
                assert(!caught_all && "bare except clause not the last one in the list?");

                CFGBlock* exc_next = nullptr;
                if (exc_handler->type) {
                    AST_expr* handled_type = remapExpr(exc_handler->type);

                    AST_LangPrimitive* is_caught_here = new AST_LangPrimitive(AST_LangPrimitive::ISINSTANCE);
                    is_caught_here->args.push_back(exc_obj);
                    is_caught_here->args.push_back(handled_type);
                    is_caught_here->args.push_back(makeNum(1)); // flag: false_on_noncls

                    AST_Branch* br = new AST_Branch();
                    br->test = remapExpr(is_caught_here);

                    CFGBlock* exc_handle = cfg->addBlock();
                    exc_next = cfg->addDeferredBlock();

                    br->iftrue = exc_handle;
                    br->iffalse = exc_next;
                    curblock->connectTo(exc_handle);
                    curblock->connectTo(exc_next);
                    push_back(br);
                    curblock = exc_handle;
                } else {
                    caught_all = true;
                }

                if (exc_handler->name) {
                    push_back(makeAssign(exc_handler->name, exc_obj));
                }

                for (AST_stmt* subnode : exc_handler->body) {
                    subnode->accept(this);
                }

                if (curblock) {
                    AST_Jump* j = new AST_Jump();
                    j->target = join_block;
                    push_back(j);
                    curblock->connectTo(join_block);
                }

                if (exc_next) {
                    cfg->placeBlock(exc_next);
                } else {
                    assert(caught_all);
                }
                curblock = exc_next;
            }

            if (!caught_all) {
                AST_Raise* raise = new AST_Raise();
                push_back(raise);
                curblock->push_back(new AST_Unreachable());
                curblock = NULL;
            }
        }

        if (join_block->predecessors.size() == 0) {
            delete join_block;
            curblock = NULL;
        } else {
            cfg->placeBlock(join_block);
            curblock = join_block;
        }

        return true;
    }

    virtual bool visit_with(AST_With* node) {
        char ctxmgrname_buf[80];
        snprintf(ctxmgrname_buf, 80, "#ctxmgr_%p", node);
        char exitname_buf[80];
        snprintf(exitname_buf, 80, "#exit_%p", node);

        push_back(makeAssign(ctxmgrname_buf, remapExpr(node->context_expr)));

        AST_expr* enter = makeLoadAttribute(makeName(ctxmgrname_buf, AST_TYPE::Load), "__enter__", true);
        AST_expr* exit = makeLoadAttribute(makeName(ctxmgrname_buf, AST_TYPE::Load), "__exit__", true);
        push_back(makeAssign(exitname_buf, exit));
        enter = makeCall(enter);

        if (node->optional_vars) {
            push_back(makeAssign(node->optional_vars, enter));
        } else {
            push_back(makeExpr(enter));
        }

        CFGBlock* continue_dest = NULL, *break_dest = NULL;
        CFGBlock* orig_continue_dest = NULL, *orig_break_dest = NULL;
        if (loops.size()) {
            continue_dest = cfg->addDeferredBlock();
            continue_dest->info = "with_continue";
            break_dest = cfg->addDeferredBlock();
            break_dest->info = "with_break";

            orig_continue_dest = getContinue();
            orig_break_dest = getBreak();

            pushLoop(continue_dest, break_dest);
        }

        CFGBlock* return_dest = cfg->addDeferredBlock();
        return_dest->info = "with_return";
        pushReturn(return_dest);

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
        }

        AST_Call* exit_call = makeCall(makeName(exitname_buf, AST_TYPE::Load));
        exit_call->args.push_back(makeName("None", AST_TYPE::Load));
        exit_call->args.push_back(makeName("None", AST_TYPE::Load));
        exit_call->args.push_back(makeName("None", AST_TYPE::Load));
        push_back(makeExpr(exit_call));

        CFGBlock* orig_ending_block = curblock;

        if (continue_dest) {
            if (continue_dest->predecessors.size() == 0) {
                delete continue_dest;
            } else {
                curblock = continue_dest;

                AST_Call* exit_call = makeCall(makeName(exitname_buf, AST_TYPE::Load));
                exit_call->args.push_back(makeName("None", AST_TYPE::Load));
                exit_call->args.push_back(makeName("None", AST_TYPE::Load));
                exit_call->args.push_back(makeName("None", AST_TYPE::Load));
                push_back(makeExpr(exit_call));

                cfg->placeBlock(continue_dest);
                AST_Jump* jcontinue = makeJump();
                jcontinue->target = orig_continue_dest;
                push_back(jcontinue);
                continue_dest->connectTo(orig_continue_dest, true);
            }

            if (break_dest->predecessors.size() == 0) {
                delete break_dest;
            } else {
                curblock = break_dest;

                AST_Call* exit_call = makeCall(makeName(exitname_buf, AST_TYPE::Load));
                exit_call->args.push_back(makeName("None", AST_TYPE::Load));
                exit_call->args.push_back(makeName("None", AST_TYPE::Load));
                exit_call->args.push_back(makeName("None", AST_TYPE::Load));
                push_back(makeExpr(exit_call));

                cfg->placeBlock(break_dest);
                AST_Jump* jbreak = makeJump();
                jbreak->target = orig_break_dest;
                push_back(jbreak);
                break_dest->connectTo(orig_break_dest, true);
            }
            popLoop();
            curblock = orig_ending_block;
        }

        popReturn();
        if (return_dest->predecessors.size() == 0) {
            delete return_dest;
        } else {
            cfg->placeBlock(return_dest);
            curblock = return_dest;

            AST_Call* exit_call = makeCall(makeName(exitname_buf, AST_TYPE::Load));
            exit_call->args.push_back(makeName("None", AST_TYPE::Load));
            exit_call->args.push_back(makeName("None", AST_TYPE::Load));
            exit_call->args.push_back(makeName("None", AST_TYPE::Load));
            push_back(makeExpr(exit_call));

            doReturn(makeName("#rtnval", AST_TYPE::Load));
            curblock = orig_ending_block;
        }

        return true;
    }
};

void CFG::print() {
    printf("CFG:\n");
    printf("%ld blocks\n", blocks.size());
    PrintVisitor* pv = new PrintVisitor(4);
    for (int i = 0; i < blocks.size(); i++) {
        CFGBlock* b = blocks[i];
        printf("Block %d", b->idx);
        if (b->info)
            printf(" '%s'", b->info);

        printf("; Predecessors:");
        for (int j = 0; j < b->predecessors.size(); j++) {
            printf(" %d", b->predecessors[j]->idx);
        }
        printf(" Successors:");
        for (int j = 0; j < b->successors.size(); j++) {
            printf(" %d", b->successors[j]->idx);
        }
        printf("\n");

        for (int j = 0; j < b->body.size(); j++) {
            printf("    ");
            b->body[j]->accept(pv);
            printf("\n");
        }
    }
    delete pv;
}

CFG* computeCFG(SourceInfo* source, std::vector<AST_stmt*> body) {
    CFG* rtn = new CFG();
    CFGVisitor visitor(source->ast->type, rtn);

    if (source->ast->type == AST_TYPE::ClassDef) {
        // A classdef always starts with "__module__ = __name__"
        Box* module_name = source->parent_module->getattr("__name__", NULL, NULL);
        assert(module_name->cls == str_cls);
        AST_Assign* module_assign = new AST_Assign();
        module_assign->targets.push_back(makeName("__module__", AST_TYPE::Store));
        module_assign->value = new AST_Str(static_cast<BoxedString*>(module_name)->s);
        visitor.push_back(module_assign);

        // If the first statement is just a single string, transform it to an assignment to __doc__
        if (body.size() && body[0]->type == AST_TYPE::Expr) {
            AST_Expr* first_expr = ast_cast<AST_Expr>(body[0]);
            if (first_expr->value->type == AST_TYPE::Str) {
                AST_Assign* doc_assign = new AST_Assign();
                doc_assign->targets.push_back(makeName("__doc__", AST_TYPE::Store));
                doc_assign->value = first_expr->value;
                visitor.push_back(doc_assign);
            }
        }
    }

    for (int i = 0; i < body.size(); i++) {
        body[i]->accept(&visitor);
    }

    // The functions we create for classdefs are supposed to return a dictionary of their locals.
    // This is the place that we add all of that:
    if (source->ast->type == AST_TYPE::ClassDef) {
        ScopeInfo* scope_info = source->scoping->getScopeInfoForNode(source->ast);

        AST_LangPrimitive* locals = new AST_LangPrimitive(AST_LangPrimitive::LOCALS);

        AST_Return* rtn = new AST_Return();
        rtn->value = locals;
        visitor.push_back(rtn);
    } else {
        // Put a fake "return" statement at the end of every function just to make sure they all have one;
        // we already have to support multiple return statements in a function, but this way we can avoid
        // having to support not having a return statement:
        AST_Return* return_stmt = new AST_Return();
        return_stmt->lineno = return_stmt->col_offset = 0;
        return_stmt->value = NULL;
        visitor.push_back(return_stmt);
    }

    if (VERBOSITY("cfg") >= 2) {
        printf("Before cfg checking and transformations:\n");
        rtn->print();
    }

#ifndef NDEBUG
    ////
    // Check some properties expected by later stages:

    assert(rtn->getStartingBlock()->predecessors.size() == 0);

    for (CFGBlock* b : rtn->blocks) {
        ASSERT(b->idx != -1, "Forgot to place a block!");
        for (CFGBlock* b2 : b->predecessors) {
            ASSERT(b2->idx != -1, "Forgot to place a block!");
        }
        for (CFGBlock* b2 : b->successors) {
            ASSERT(b2->idx != -1, "Forgot to place a block!");
        }

        ASSERT(b->body.size(), "%d", b->idx);
        ASSERT(b->successors.size() <= 2, "%d has too many successors!", b->idx);
        if (b->successors.size() == 0) {
            AST_stmt* terminator = b->body.back();
            assert(terminator->type == AST_TYPE::Return || terminator->type == AST_TYPE::Raise
                   || terminator->type == AST_TYPE::Unreachable);
        }

        if (b->predecessors.size() == 0)
            assert(b == rtn->getStartingBlock());
    }

    // We need to generate the CFG in a way that doesn't have any critical edges,
    // since the ir generation requires that.
    // We could do this with a separate critical-edge-breaking pass, but for now
    // the cfg-computing code directly avoids making critical edges.
    // Either way, double check to make sure that we don't have any:
    for (int i = 0; i < rtn->blocks.size(); i++) {
        if (rtn->blocks[i]->successors.size() >= 2) {
            for (int j = 0; j < rtn->blocks[i]->successors.size(); j++) {
                // It's ok to have zero predecessors if you are the entry block
                ASSERT(rtn->blocks[i]->successors[j]->predecessors.size() < 2, "Critical edge from %d to %d!", i,
                       rtn->blocks[i]->successors[j]->idx);
            }
        }
    }

    // The cfg blocks should be generated in roughly program order.
    // Specifically, this means every block should have one predecessor block that
    // has a lower index (except for block 0).
    // We use this during IR generation to ensure that at least one predecessor has always
    // been evaluated before the current block; this property also ensures that there are no
    // dead blocks.
    for (int i = 1; i < rtn->blocks.size(); i++) {
        bool good = false;
        for (int j = 0; j < rtn->blocks[i]->predecessors.size(); j++) {
            if (rtn->blocks[i]->predecessors[j]->idx < i)
                good = true;
        }
        if (!good) {
            printf("internal error: block %d doesn't have a previous predecessor\n", i);
            abort();
        }

        // Later phases also rely on the fact that the first predecessor has a lower index;
        // this can be worked around but it's easiest just to ensure this here.
        assert(rtn->blocks[i]->predecessors[0]->idx < i);
    }

    assert(rtn->getStartingBlock()->idx == 0);

// TODO make sure the result of Invoke nodes are not used on the exceptional path
#endif

    // Prune unnecessary blocks from the CFG.
    // Not strictly necessary, but makes the output easier to look at,
    // and can make the analyses more efficient.
    // The extra blocks would get merged by LLVM passes, so I'm not sure
    // how much overall improvement there is.

    // Must evaluate end() on every iteration because erase() will invalidate the end.
    for (auto it = rtn->blocks.begin(); it != rtn->blocks.end(); ++it) {
        CFGBlock* b = *it;
        while (b->successors.size() == 1) {
            CFGBlock* b2 = b->successors[0];
            if (b2->predecessors.size() != 1)
                break;

            if (VERBOSITY()) {
                // rtn->print();
                printf("Joining blocks %d and %d\n", b->idx, b2->idx);
            }

            assert(b->body[b->body.size() - 1]->type == AST_TYPE::Jump);

            b->body.pop_back();
            b->body.insert(b->body.end(), b2->body.begin(), b2->body.end());
            b->unconnectFrom(b2);

            for (CFGBlock* b3 : b2->successors) {
                b->connectTo(b3, true);
                b2->unconnectFrom(b3);
            }

            rtn->blocks.erase(std::remove(rtn->blocks.begin(), rtn->blocks.end(), b2), rtn->blocks.end());
            delete b2;
        }
    }

    if (VERBOSITY("cfg") >= 2) {
        printf("Final cfg:\n");
        rtn->print();
    }


    return rtn;
}
}
