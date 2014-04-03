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

#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstdlib>

#include "core/options.h"

#include "core/ast.h"
#include "core/cfg.h"

namespace pyston {

void CFGBlock::connectTo(CFGBlock *successor, bool allow_backedge) {
    if (!allow_backedge) {
        assert(this->idx >= 0);
        ASSERT(successor->idx == -1 || successor->idx > this->idx, "edge from %d to %d", this->idx, successor->idx);
    }
    successors.push_back(successor);
    successor->predecessors.push_back(this);
}

class CFGVisitor : public ASTVisitor {
    private:
        AST_TYPE::AST_TYPE root_type;
        CFG *cfg;
        CFGBlock *curblock;

        struct LoopInfo {
            CFGBlock *continue_dest, *break_dest;
        };
        std::vector<LoopInfo> loops;
        std::vector<CFGBlock*> returns;

        void pushLoop(CFGBlock *continue_dest, CFGBlock *break_dest) {
            LoopInfo loop;
            loop.continue_dest = continue_dest;
            loop.break_dest = break_dest;
            loops.push_back(loop);
        }

        void popLoop() {
            loops.pop_back();
        }

        void pushReturn(CFGBlock *return_dest) {
            returns.push_back(return_dest);
        }

        void popReturn() {
            returns.pop_back();
        }

        void doReturn(AST_expr* value) {
            assert(value);
            CFGBlock *rtn_dest = getReturn();
            if (rtn_dest != NULL) {
                push_back(makeAssign("#rtnval", value));

                AST_Jump *j = makeJump();
                j->target = rtn_dest;
                curblock->connectTo(rtn_dest);
                push_back(j);
            } else {
                AST_Return *node = new AST_Return();
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



        AST_expr* makeNum(int n) {
            AST_Num* node = new AST_Num();
            node->col_offset = -1;
            node->lineno = -1;
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

        AST_expr* makeLoadAttribute(AST_expr* base, const std::string &name, bool clsonly) {
            AST_expr* rtn;
            if (clsonly) {
                AST_ClsAttribute *attr = new AST_ClsAttribute();
                attr->value = base;
                attr->attr = name;
                rtn = attr;
            } else {
                AST_Attribute *attr = new AST_Attribute();
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
            AST_Call *call = new AST_Call();
            call->starargs = NULL;
            call->kwargs = NULL;
            call->func = func;
            call->col_offset = func->col_offset;
            call->lineno = func->lineno;
            return call;
        }

        AST_expr* makeName(const std::string &id, AST_TYPE::AST_TYPE ctx_type, int lineno=-1, int col_offset=-1) {
            AST_Name *name = new AST_Name();
            name->id = id;
            name->col_offset = col_offset;
            name->lineno = lineno;
            name->ctx_type = ctx_type;
            return name;
        }

        AST_stmt* makeAssign(AST_expr *target, AST_expr *val) {
            AST_Assign *assign = new AST_Assign();
            assign->targets.push_back(target);
            assign->value = val;
            assign->col_offset = val->col_offset;
            assign->lineno = val->lineno;
            return assign;
        }

        AST_stmt* makeAssign(const std::string &id, AST_expr *val) {
            assert(val);
            AST_expr *name = makeName(id, AST_TYPE::Store, val->lineno, 0);
            return makeAssign(name, val);
        }

        AST_stmt* makeExpr(AST_expr* expr) {
            AST_Expr *stmt = new AST_Expr();
            stmt->value = expr;
            stmt->lineno = expr->lineno;
            stmt->col_offset = expr->col_offset;
            return stmt;
        }

    public:
        CFGVisitor(AST_TYPE::AST_TYPE root_type, CFG* cfg) : root_type(root_type), cfg(cfg) {
            curblock = cfg->addBlock();
            curblock->info = "entry";
        }

        ~CFGVisitor() {
            assert(loops.size() == 0);
            assert(returns.size() == 0);
        }

        void push_back(AST_stmt* node) {
            if (curblock)
                curblock->push_back(node);
        }

        virtual bool visit_assign(AST_Assign* node) { push_back(node); return true; }
        virtual bool visit_augassign(AST_AugAssign* node) { push_back(node); return true; }
        virtual bool visit_classdef(AST_ClassDef* node) { push_back(node); return true; }
        virtual bool visit_expr(AST_Expr* node) { push_back(node); return true; }
        virtual bool visit_functiondef(AST_FunctionDef* node) { push_back(node); return true; }
        virtual bool visit_global(AST_Global* node) { push_back(node); return true; }
        virtual bool visit_import(AST_Import* node) { push_back(node); return true; }
        virtual bool visit_pass(AST_Pass* node) { push_back(node); return true; }
        virtual bool visit_print(AST_Print* node) { push_back(node); return true; }

        virtual bool visit_return(AST_Return* node) {
            if (root_type != AST_TYPE::FunctionDef) {
                fprintf(stderr, "SyntaxError: 'return' outside function\n");
                exit(1);
            }

            AST_expr *value = node->value;
            if (value == NULL)
                value = makeName("None", AST_TYPE::Load);
            doReturn(value);
            return true;
        }

        virtual bool visit_if(AST_If* node) {
            if (!curblock) return true;

            AST_Branch *br = new AST_Branch();
            br->col_offset = node->col_offset;
            br->lineno = node->lineno;
            br->test = node->test;
            push_back(br);

            CFGBlock *starting_block = curblock;
            CFGBlock *exit = cfg->addDeferredBlock();
            exit->info = "ifexit";

            CFGBlock *iftrue = cfg->addBlock();
            iftrue->info = "iftrue";
            br->iftrue = iftrue;
            starting_block->connectTo(iftrue);
            curblock = iftrue;
            for (int i = 0; i < node->body.size(); i++) {
                node->body[i]->accept(this);
            }
            if (curblock) {
                AST_Jump *jtrue = new AST_Jump();
                push_back(jtrue);
                jtrue->target = exit;
                curblock->connectTo(exit);
            }

            CFGBlock *iffalse = cfg->addBlock();
            br->iffalse = iffalse;
            starting_block->connectTo(iffalse);

            iffalse->info = "iffalse";
            curblock = iffalse;
            for (int i = 0; i < node->orelse.size(); i++) {
                node->orelse[i]->accept(this);
            }
            if (curblock) {
                AST_Jump *jfalse = new AST_Jump();
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
            if (!curblock) return true;

            if (loops.size() == 0) {
                fprintf(stderr, "SyntaxError: 'break' outside loop\n");
                exit(1);
            }

            AST_Jump *j = makeJump();
            push_back(j);
            assert(loops.size());
            j->target = loops[loops.size()-1].break_dest;
            curblock->connectTo(j->target, true);

            curblock = NULL;
            return true;
        }

        virtual bool visit_continue(AST_Continue* node) {
            if (!curblock) return true;

            if (loops.size() == 0) {
                // Note: error message is different than the 'break' case
                fprintf(stderr, "SyntaxError: 'continue' not properly in loop\n");
                exit(1);
            }

            AST_Jump *j = makeJump();
            push_back(j);
            assert(loops.size());
            j->target = loops[loops.size()-1].continue_dest;
            curblock->connectTo(j->target, true);

            // See visit_break for explanation:
            curblock = NULL;
            return true;
        }

        virtual bool visit_while(AST_While* node) {
            if (!curblock) return true;

            CFGBlock *test_block = cfg->addBlock();
            test_block->info = "while_test";

            AST_Jump *j = makeJump();
            push_back(j);
            j->target = test_block;
            curblock->connectTo(test_block);

            curblock = test_block;
            AST_Branch *br = makeBranch(node->test);
            push_back(br);

            // We need a reference to this block early on so we can break to it,
            // but we don't want it to be placed until after the orelse.
            CFGBlock *end = cfg->addDeferredBlock();
            end->info = "while_exit";
            pushLoop(test_block, end);

            CFGBlock *body = cfg->addBlock();
            body->info = "while_body_start";
            br->iftrue = body;
            test_block->connectTo(body);
            curblock = body;
            for (int i = 0; i < node->body.size(); i++) {
                node->body[i]->accept(this);
            }
            if (curblock) {
                AST_Jump *jbody = makeJump();
                push_back(jbody);
                jbody->target = test_block;
                curblock->connectTo(test_block, true);
            }
            popLoop();

            CFGBlock *orelse = cfg->addBlock();
            orelse->info = "while_orelse_start";
            br->iffalse = orelse;
            test_block->connectTo(orelse);
            curblock = orelse;
            for (int i = 0; i < node->orelse.size(); i++) {
                node->orelse[i]->accept(this);
            }
            if (curblock) {
                AST_Jump *jend = makeJump();
                push_back(jend);
                jend->target = end;
                curblock->connectTo(end);
            }
            curblock = end;

            cfg->placeBlock(end);

            return true;
        }

        virtual bool visit_for(AST_For* node) {
            if (!curblock) return true;

            // TODO this is so complicated because I tried doing loop inversion;
            // is it really worth it?  It got so bad because all the edges became
            // critical edges and needed to be broken, otherwise it's not too different.

            AST_expr *iter_attr = makeLoadAttribute(node->iter, "__iter__", true);
            AST_expr *iter_call = makeCall(iter_attr);

            char itername_buf[80];
            snprintf(itername_buf, 80, "#iter_%p", node);
            AST_stmt *iter_assign = makeAssign(itername_buf, iter_call);
            push_back(iter_assign);

            AST_expr *hasnext_attr = makeLoadAttribute(makeName(itername_buf, AST_TYPE::Load), "__hasnext__", true);
            AST_expr *next_attr = makeLoadAttribute(makeName(itername_buf, AST_TYPE::Load), "next", true);
//#define SAVE_ATTRS
#ifdef SAVE_ATTRS
            char hasnextname_buf[80];
            snprintf(hasnextname_buf, 80, "#hasnext_%p", node);
            AST_stmt *hasnext_assign = makeAssign(hasnextname_buf, hasnext_attr);
            push_back(hasnext_assign);
            char nextname_buf[80];
            snprintf(nextname_buf, 80, "#next_%p", node);
            push_back(makeAssign(nextname_buf, next_attr));
#endif

            CFGBlock *test_block = cfg->addBlock();
            AST_Jump* jump_to_test = makeJump();
            jump_to_test->target = test_block;
            push_back(jump_to_test);
            curblock->connectTo(test_block);
            curblock = test_block;

#ifdef SAVE_ATTRS
            AST_expr *test_call = makeCall(makeName(hasnextname_buf, AST_TYPE::Load));
#else
            AST_expr *test_call = makeCall(hasnext_attr);
#endif
            AST_Branch *test_br = makeBranch(test_call);
            push_back(test_br);

            CFGBlock *test_true = cfg->addBlock();
            CFGBlock *test_false = cfg->addBlock();
            test_br->iftrue = test_true;
            test_br->iffalse = test_false;
            test_block->connectTo(test_true);
            test_block->connectTo(test_false);

            CFGBlock *loop_block = cfg->addBlock();
            CFGBlock *end_block = cfg->addDeferredBlock();
            CFGBlock *else_block = cfg->addDeferredBlock();

            curblock = test_true;

            // TODO simplify the breaking of these crit edges?
            AST_Jump *test_true_jump = makeJump();
            test_true_jump->target = loop_block;
            push_back(test_true_jump);
            test_true->connectTo(loop_block);

            curblock = test_false;
            AST_Jump *test_false_jump = makeJump();
            test_false_jump->target = else_block;
            push_back(test_false_jump);
            test_false->connectTo(else_block);

            pushLoop(test_block, end_block);

            curblock = loop_block;
#ifdef SAVE_ATTRS
            push_back(makeAssign(node->target, makeCall(makeName(nextname_buf, AST_TYPE::Load))));
#else
            push_back(makeAssign(node->target, makeCall(next_attr)));
#endif

            for (int i = 0; i < node->body.size(); i++) {
                node->body[i]->accept(this);
            }
            popLoop();

            if (curblock) {
#ifdef SAVE_ATTRS
                AST_expr *end_call = makeCall(makeName(hasnextname_buf, AST_TYPE::Load));
#else
                AST_expr *end_call = makeCall(hasnext_attr);
#endif
                AST_Branch *end_br = makeBranch(end_call);
                push_back(end_br);

                CFGBlock *end_true = cfg->addBlock();
                CFGBlock *end_false = cfg->addBlock();
                end_br->iftrue = end_true;
                end_br->iffalse = end_false;
                curblock->connectTo(end_true);
                curblock->connectTo(end_false);

                curblock = end_true;
                AST_Jump *end_true_jump = makeJump();
                end_true_jump->target = loop_block;
                push_back(end_true_jump);
                end_true->connectTo(loop_block, true);

                curblock = end_false;
                AST_Jump *end_false_jump = makeJump();
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
                AST_Jump *else_jump = makeJump();
                push_back(else_jump);
                else_jump->target = end_block;
                curblock->connectTo(end_block);
            }

            cfg->placeBlock(end_block);
            curblock = end_block;

            return true;
        }

        virtual bool visit_with(AST_With* node) {
            char ctxmgrname_buf[80];
            snprintf(ctxmgrname_buf, 80, "#ctxmgr_%p", node);
            char exitname_buf[80];
            snprintf(exitname_buf, 80, "#exit_%p", node);

            push_back(makeAssign(ctxmgrname_buf, node->context_expr));

            AST_expr *enter = makeLoadAttribute(makeName(ctxmgrname_buf, AST_TYPE::Load), "__enter__", true);
            AST_expr *exit = makeLoadAttribute(makeName(ctxmgrname_buf, AST_TYPE::Load), "__exit__", true);
            push_back(makeAssign(exitname_buf, exit));
            enter = makeCall(enter);

            if (node->optional_vars) {
                push_back(makeAssign(node->optional_vars, enter));
            } else {
                push_back(makeExpr(enter));
            }

            CFGBlock *continue_dest = NULL, *break_dest = NULL;
            CFGBlock *orig_continue_dest = NULL, *orig_break_dest = NULL;
            if (loops.size()) {
                continue_dest = cfg->addDeferredBlock();
                continue_dest->info = "with_continue";
                break_dest = cfg->addDeferredBlock();
                break_dest->info = "with_break";

                orig_continue_dest = loops[loops.size() - 1].continue_dest;
                orig_break_dest = loops[loops.size() - 1].break_dest;

                pushLoop(continue_dest, break_dest);
            }

            CFGBlock *orig_return_dest = getReturn();
            CFGBlock *return_dest = cfg->addDeferredBlock();
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

            CFGBlock *orig_ending_block = curblock;

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
    PrintVisitor *pv = new PrintVisitor(4);
    for (int i = 0; i < blocks.size(); i++) {
        printf("Block %d", i);
        CFGBlock *b = blocks[i];
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

CFG* computeCFG(AST_TYPE::AST_TYPE root_type, std::vector<AST_stmt*> body) {
    CFG *rtn = new CFG();
    CFGVisitor visitor(root_type, rtn);
    for (int i = 0; i < body.size(); i++) {
        body[i]->accept(&visitor);
    }

    // Put a fake "return" statement at the end of every function just to make sure they all have one;
    // we already have to support multiple return statements in a function, but this way we can avoid
    // having to support not having a return statement:
    AST_Return *return_stmt = new AST_Return();
    return_stmt->value = NULL;
    visitor.push_back(return_stmt);

    ////
    // Check some properties expected by later stages:

    // Block 0 is hard-coded to be the entry block, and shouldn't have any
    // predecessors:
    assert(rtn->blocks[0]->predecessors.size() == 0);

    // We need to generate the CFG in a way that doesn't have any critical edges,
    // since the ir generation requires that.
    // We could do this with a separate critical-edge-breaking pass, but for now
    // the cfg-computing code directly avoids making critical edges.
    // Either way, double check to make sure that we don't have any:
    for (int i = 0; i < rtn->blocks.size(); i++) {
        if (rtn->blocks[i]->successors.size() >= 2) {
            for (int j = 0; j < rtn->blocks[i]->successors.size(); j++) {
                // It's ok to have zero predecessors if you are the entry block
                ASSERT(rtn->blocks[i]->successors[j]->predecessors.size() < 2, "Critical edge from %d to %d!", i, rtn->blocks[i]->successors[j]->idx);
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

    return rtn;
}

}
