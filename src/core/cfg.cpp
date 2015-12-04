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

#include "core/cfg.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "Python.h"

#include "analysis/scoping_analysis.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

void CFGBlock::connectTo(CFGBlock* successor, bool allow_backedge) {
    assert(successors.size() <= 1);

    if (!allow_backedge) {
        assert(this->idx >= 0);
        ASSERT(successor->idx == -1 || successor->idx > this->idx, "edge from %d (%s) to %d (%s)", this->idx,
               this->info, successor->idx, successor->info);
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

void CFGBlock::print(llvm::raw_ostream& stream) {
    stream << "Block " << idx;
    if (info)
        stream << " '" << info << "'";

    stream << "; Predecessors:";
    for (int j = 0; j < predecessors.size(); j++) {
        stream << " " << predecessors[j]->idx;
    }
    stream << " Successors:";
    for (int j = 0; j < successors.size(); j++) {
        stream << " " << successors[j]->idx;
    }
    stream << "\n";

    PrintVisitor pv(4);
    for (int j = 0; j < body.size(); j++) {
        stream << "    ";
        body[j]->accept(&pv);
        stream << "\n";
    }
}

static const std::string RETURN_NAME("#rtnval");

// The various reasons why a `finally' block (or similar, eg. a `with' exit block) might get entered.
// this has to go outside CFGVisitor b/c why_values can't go inside it.
enum Why : int8_t {
    FALLTHROUGH, // i.e. normal control flow
    CONTINUE,
    BREAK,
    RETURN,
    EXCEPTION,
};

static const Why why_values[] = { FALLTHROUGH, CONTINUE, BREAK, RETURN, EXCEPTION };

class CFGVisitor : public ASTVisitor {
    // ---------- Types ----------
private:
    /* Explanation of ContInfo and ExcBlockInfo:
     *
     * While generating the CFG, we need to know what to do if we:
     * 1. hit a `continue'
     * 2. hit a `break'
     * 3. hit a `return'
     * 4. raise an exception
     *
     * We call these "continuations", because they're what we "continue on to" after these conditions occur.
     *
     * Various control flow constructs affect each of these:
     * - `for' and `while' affect (1-2).
     * - `try/except' affects (4).
     * - `try/finally' and `with' affect all four.
     *
     * Each of these take effect only within some chunk of code. So, notionally, we keep a stack for each of (1-4) whose
     * _top_ value says what to do if that condition occurs. The top of the continue-stack points to the block to jump
     * to if we hit a `continue', etc.
     *
     * For example, when we enter a loop, we push a pointer to the head of the loop onto the continue-stack, and a
     * pointer to the code after the loop onto the break-stack. When we visit a `break' in the loop body, we emit a jump
     * to the top of the break-stack, which is the end of the loop. After we finish visiting the loop body, we pop the
     * break- & continue-stacks, restoring our old state (maybe we were inside another loop, for example).
     *
     * It's more complicated in practice, because:
     *
     * 1. When we jump to a `finally' block, we must tell it *why* we jumped to it. After the `finally' block finishes,
     *    it uses this info to resume what we were doing before we entered it (returning, raising an exception, etc).
     *
     * 2. When we jump to a `except' block, we must record three pieces of information about the exception (its type,
     *    value, and traceback).
     *
     * So instead of four stacks of block pointers, instead we have two stacks:
     * - `continuations', a stack of ContInfos, for `continue', `break', and `return'
     * - `exc_handlers', a stack of ExcBlockInfos, for exceptions
     *
     * Read the comments in ContInfo & ExcBlockInfo for more information.
     */
    struct ContInfo {
        // where to jump to if a continue, break, or return happens respectively
        CFGBlock* continue_dest, *break_dest, *return_dest;
        // true if this continuation needs to know the reason why we entered it. `finally' blocks use this info to
        // determine how to resume execution after they finish.
        bool say_why;
        // bit-vector tracking all reasons Why we ever might enter this continuation. is only updated/used if `say_why'
        // is true. when we emit a jump to this continuation for reason w, we set the bit (did_why & (1 << w)). this is
        // used when emitting `finally' blocks to determine which continuation-cases to emit.
        int did_why;
        // name of the variable to store the reason Why we jumped in.
        InternedString why_name;

        ContInfo(CFGBlock* continue_dest, CFGBlock* break_dest, CFGBlock* return_dest, bool say_why,
                 InternedString why_name)
            : continue_dest(continue_dest),
              break_dest(break_dest),
              return_dest(return_dest),
              say_why(say_why),
              did_why(0),
              why_name(why_name) {}
    };

    struct ExcBlockInfo {
        // where to jump in case of an exception
        CFGBlock* exc_dest;
        // variable names to store the exception (type, value, traceback) in
        InternedString exc_type_name, exc_value_name, exc_traceback_name;
    };

    // ---------- Member fields ----------
private:
    SourceInfo* source;
    // `root_type' is the type of the root of the AST tree that we are turning
    // into a CFG. Used when we find a "return" to check that we're inside a
    // function (otherwise we SyntaxError).
    AST_TYPE::AST_TYPE root_type;
    FutureFlags future_flags;
    CFG* cfg;
    CFGBlock* curblock;
    ScopingAnalysis* scoping_analysis;
    std::vector<ContInfo> continuations;
    std::vector<ExcBlockInfo> exc_handlers;

    unsigned int next_var_index = 0;

    friend CFG* computeCFG(SourceInfo* source, std::vector<AST_stmt*> body);

public:
    CFGVisitor(SourceInfo* source, AST_TYPE::AST_TYPE root_type, FutureFlags future_flags,
               ScopingAnalysis* scoping_analysis, CFG* cfg)
        : source(source),
          root_type(root_type),
          future_flags(future_flags),
          cfg(cfg),
          scoping_analysis(scoping_analysis) {
        curblock = cfg->addBlock();
        curblock->info = "entry";
    }

    ~CFGVisitor() {
        // if we're being destroyed due to an exception, our internal invariants may be violated, but that's okay; the
        // CFG isn't going to get used anyway. (Maybe we should check that it won't be used somehow?)
        assert(continuations.size() == 0 || std::uncaught_exception());
        assert(exc_handlers.size() == 0 || std::uncaught_exception());
    }

    // ---------- private methods ----------
private:
    template <typename T> InternedString internString(T&& s) {
        return source->getInternedStrings().get(std::forward<T>(s));
    }

    InternedString createUniqueName(llvm::Twine prefix) {
        std::string name = (prefix + llvm::Twine(next_var_index++)).str();
        return source->getInternedStrings().get(std::move(name));
    }

    AST_Name* makeName(InternedString id, AST_TYPE::AST_TYPE ctx_type, int lineno, int col_offset = 0) {
        AST_Name* name = new AST_Name(id, ctx_type, lineno, col_offset);
        return name;
    }

    AST_Name* makeLoad(InternedString id, AST* node) { return makeName(id, AST_TYPE::Load, node->lineno); }

    void pushLoopContinuation(CFGBlock* continue_dest, CFGBlock* break_dest) {
        assert(continue_dest
               != break_dest); // I guess this doesn't have to be true, but validates passing say_why=false
        continuations.emplace_back(continue_dest, break_dest, nullptr, false, internString(""));
    }

    void pushFinallyContinuation(CFGBlock* finally_block, InternedString why_name) {
        continuations.emplace_back(finally_block, finally_block, finally_block, true, why_name);
    }

    void popContinuation() { continuations.pop_back(); }

    void doReturn(AST_expr* value) {
        assert(value);
        assert(curblock);

        for (auto& cont : llvm::make_range(continuations.rbegin(), continuations.rend())) {
            if (cont.return_dest) {
                if (cont.say_why) {
                    pushAssign(cont.why_name, makeNum(Why::RETURN));
                    cont.did_why |= (1 << Why::RETURN);
                }

                pushAssign(internString(RETURN_NAME), value);
                pushJump(cont.return_dest);
                return;
            }
        }

        AST_Return* node = new AST_Return();
        node->value = value;
        node->col_offset = value->col_offset;
        node->lineno = value->lineno;
        push_back(node);
        curblock = NULL;
    }

    void doContinue() {
        assert(curblock);
        for (auto& cont : llvm::make_range(continuations.rbegin(), continuations.rend())) {
            if (cont.continue_dest) {
                if (cont.say_why) {
                    pushAssign(cont.why_name, makeNum(Why::CONTINUE));
                    cont.did_why |= (1 << Why::CONTINUE);
                }

                pushJump(cont.continue_dest, true);
                return;
            }
        }

        raiseExcHelper(SyntaxError, "'continue' not properly in loop");
    }

    void doBreak() {
        assert(curblock);
        for (auto& cont : llvm::make_range(continuations.rbegin(), continuations.rend())) {
            if (cont.break_dest) {
                if (cont.say_why) {
                    pushAssign(cont.why_name, makeNum(Why::BREAK));
                    cont.did_why |= (1 << Why::BREAK);
                }

                pushJump(cont.break_dest, true);
                return;
            }
        }

        raiseExcHelper(SyntaxError, "'break' outside loop");
    }

    AST_expr* callNonzero(AST_expr* e) {
        AST_LangPrimitive* call = new AST_LangPrimitive(AST_LangPrimitive::NONZERO);
        call->args.push_back(e);
        call->lineno = e->lineno;
        call->col_offset = e->col_offset;

        // Simple optimization: allow the generation of nested nodes if there isn't a
        // current exc handler.
        if (exc_handlers.size() == 0)
            return call;

        auto name = nodeName();
        pushAssign(name, call);
        return makeLoad(name, e);
    }

    AST_Name* remapName(AST_Name* name) { return name; }

    AST_expr* applyComprehensionCall(AST_ListComp* node, AST_Name* name) {
        AST_expr* elt = remapExpr(node->elt);
        return makeCall(makeLoadAttribute(name, internString("append"), true), elt);
    }

    template <typename ResultASTType, typename CompType> AST_expr* remapComprehension(CompType* node) {
        assert(curblock);

        InternedString rtn_name = nodeName();
        pushAssign(rtn_name, new ResultASTType());
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
            AST_LangPrimitive* iter_call = new AST_LangPrimitive(AST_LangPrimitive::GET_ITER);
            iter_call->args.push_back(remapped_iter);
            InternedString iter_name = nodeName("lc_iter", i);
            pushAssign(iter_name, iter_call);

            AST_expr* next_attr = makeLoadAttribute(makeLoad(iter_name, node), internString("next"), true);

            CFGBlock* test_block = cfg->addBlock();
            test_block->info = "comprehension_test";
            // printf("Test block for comp %d is %d\n", i, test_block->idx);
            pushJump(test_block);

            curblock = test_block;
            AST_LangPrimitive* test_call = new AST_LangPrimitive(AST_LangPrimitive::HASNEXT);
            test_call->args.push_back(makeName(iter_name, AST_TYPE::Load, node->lineno));
            AST_expr* test = remapExpr(test_call);

            CFGBlock* body_block = cfg->addBlock();
            body_block->info = "comprehension_body";
            CFGBlock* exit_block = cfg->addDeferredBlock();
            exit_block->info = "comprehension_exit";
            exit_blocks.push_back(exit_block);
            // printf("Body block for comp %d is %d\n", i, body_block->idx);

            AST_Branch* br = new AST_Branch();
            br->col_offset = node->col_offset;
            br->lineno = node->lineno;
            br->test = test;
            br->iftrue = body_block;
            br->iffalse = exit_block;
            curblock->connectTo(body_block);
            curblock->connectTo(exit_block);
            push_back(br);

            curblock = body_block;
            InternedString next_name(nodeName());
            pushAssign(next_name, makeCall(next_attr));
            pushAssign(c->target, makeLoad(next_name, node));

            for (AST_expr* if_condition : c->ifs) {
                AST_expr* remapped = callNonzero(remapExpr(if_condition));
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
                pushJump(test_block, true);

                curblock = body_continue;
            }

            CFGBlock* body_end = curblock;

            assert((finished_block != NULL) == (i != 0));
            if (finished_block) {
                curblock = exit_block;
                pushJump(finished_block, true);
            }
            finished_block = test_block;

            curblock = body_end;
            if (is_innermost) {
                push_back(makeExpr(applyComprehensionCall(node, makeLoad(rtn_name, node))));

                pushJump(test_block, true);

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

        return makeLoad(rtn_name, node);
    }

    AST_expr* makeNum(int n) {
        AST_Num* node = new AST_Num();
        node->num_type = AST_Num::INT;
        node->n_int = n;
        return node;
    }

    void pushJump(CFGBlock* target, bool allow_backedge = false) {
        AST_Jump* rtn = new AST_Jump();
        rtn->target = target;
        push_back(rtn);
        curblock->connectTo(target, allow_backedge);
        curblock = nullptr;
    }

    // NB. can generate blocks, because callNonzero can
    AST_Branch* makeBranch(AST_expr* test) {
        AST_Branch* rtn = new AST_Branch();
        rtn->test = callNonzero(test);
        rtn->col_offset = test->col_offset;
        rtn->lineno = test->lineno;
        return rtn;
    }

    // NB. this can (but usually doesn't) generate new blocks, which is why we require `iftrue' and `iffalse' to be
    // deferred, to avoid heisenbugs. of course, this doesn't allow these branches to be backedges, but that hasn't yet
    // been necessary.
    void pushBranch(AST_expr* test, CFGBlock* iftrue, CFGBlock* iffalse) {
        assert(iftrue->idx == -1 && iffalse->idx == -1);
        AST_Branch* branch = makeBranch(test);
        branch->iftrue = iftrue;
        branch->iffalse = iffalse;
        curblock->connectTo(iftrue);
        curblock->connectTo(iffalse);
        push_back(branch);
        curblock = nullptr;
    }

    void pushReraise(AST* node, InternedString exc_type_name, InternedString exc_value_name,
                     InternedString exc_traceback_name) {
        auto raise = new AST_Raise();
        raise->arg0 = makeLoad(exc_type_name, node);
        raise->arg1 = makeLoad(exc_value_name, node);
        raise->arg2 = makeLoad(exc_traceback_name, node);
        push_back(raise);
        curblock = nullptr;
    }

    AST_expr* makeLoadAttribute(AST_expr* base, InternedString name, bool clsonly) {
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
        auto call = makeCall(func);
        call->args.push_back(arg0);
        return call;
    }

    AST_Call* makeCall(AST_expr* func, AST_expr* arg0, AST_expr* arg1) {
        auto call = makeCall(func);
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        return call;
    }

    AST_Call* makeCall(AST_expr* func, AST_expr* arg0, AST_expr* arg1, AST_expr* arg2) {
        auto call = makeCall(func);
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        call->args.push_back(arg2);
        return call;
    }

    AST_Compare* makeCompare(AST_TYPE::AST_TYPE oper, AST_expr* left, AST_expr* right) {
        auto compare = new AST_Compare();
        compare->ops.push_back(AST_TYPE::Eq);
        compare->left = left;
        compare->comparators.push_back(right);
        return compare;
    }

    void pushAssign(AST_expr* target, AST_expr* val) {
        AST_Assign* assign = new AST_Assign();
        assign->value = val;
        assign->col_offset = val->col_offset;
        assign->lineno = val->lineno;

        if (target->type == AST_TYPE::Name) {
            assign->targets.push_back(remapName(ast_cast<AST_Name>(target)));
            push_back(assign);
        } else if (target->type == AST_TYPE::Subscript) {
            AST_Subscript* s = ast_cast<AST_Subscript>(target);
            assert(s->ctx_type == AST_TYPE::Store);

            AST_Subscript* s_target = new AST_Subscript();
            s_target->value = remapExpr(s->value);
            s_target->slice = remapSlice(s->slice);
            s_target->ctx_type = AST_TYPE::Store;
            s_target->col_offset = s->col_offset;
            s_target->lineno = s->lineno;

            assign->targets.push_back(s_target);
            push_back(assign);
        } else if (target->type == AST_TYPE::Attribute) {
            AST_Attribute* a = ast_cast<AST_Attribute>(target);
            assert(a->ctx_type == AST_TYPE::Store);

            AST_Attribute* a_target = new AST_Attribute();
            a_target->value = remapExpr(a->value);
            a_target->attr = source->mangleName(a->attr);
            a_target->ctx_type = AST_TYPE::Store;
            a_target->col_offset = a->col_offset;
            a_target->lineno = a->lineno;

            assign->targets.push_back(a_target);
            push_back(assign);
        } else if (target->type == AST_TYPE::Tuple || target->type == AST_TYPE::List) {
            std::vector<AST_expr*>* elts;
            if (target->type == AST_TYPE::Tuple) {
                AST_Tuple* _t = ast_cast<AST_Tuple>(target);
                assert(_t->ctx_type == AST_TYPE::Store);
                elts = &_t->elts;
            } else {
                AST_List* _t = ast_cast<AST_List>(target);
                assert(_t->ctx_type == AST_TYPE::Store);
                elts = &_t->elts;
            }

            AST_Tuple* new_target = new AST_Tuple();
            new_target->ctx_type = AST_TYPE::Store;
            new_target->lineno = target->lineno;
            new_target->col_offset = target->col_offset;

            // A little hackery: push the assign, even though we're not done constructing it yet,
            // so that we can iteratively push more stuff after it
            assign->targets.push_back(new_target);
            push_back(assign);

            for (int i = 0; i < elts->size(); i++) {
                InternedString tmp_name = nodeName("", i);
                new_target->elts.push_back(makeName(tmp_name, AST_TYPE::Store, target->lineno));

                pushAssign((*elts)[i], makeLoad(tmp_name, target));
            }
        } else {
            RELEASE_ASSERT(0, "%d", target->type);
        }
    }

    void pushAssign(InternedString id, AST_expr* val) {
        assert(val);
        AST_expr* name = makeName(id, AST_TYPE::Store, val->lineno, 0);
        pushAssign(name, val);
    }

    AST_stmt* makeExpr(AST_expr* expr) {
        AST_Expr* stmt = new AST_Expr();
        stmt->value = expr;
        stmt->lineno = expr->lineno;
        stmt->col_offset = expr->col_offset;
        return stmt;
    }

    InternedString nodeName() { return createUniqueName("#"); }

    InternedString nodeName(llvm::StringRef suffix) { return createUniqueName(llvm::Twine("#") + suffix + "_"); }

    InternedString nodeName(llvm::StringRef suffix, int idx) {
        return createUniqueName(llvm::Twine("#") + suffix + "_" + llvm::Twine(idx) + "_");
    }

    AST_expr* remapAttribute(AST_Attribute* node) {
        AST_Attribute* rtn = new AST_Attribute();

        rtn->col_offset = node->col_offset;
        rtn->lineno = node->lineno;
        rtn->ctx_type = node->ctx_type;
        rtn->attr = source->mangleName(node->attr);
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    AST_expr* remapBinOp(AST_BinOp* node) {
        AST_BinOp* rtn = new AST_BinOp();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->op_type = remapBinOpType(node->op_type);
        rtn->left = remapExpr(node->left);
        rtn->right = remapExpr(node->right);
        return rtn;
    }

    AST_slice* _dup(AST_slice* val) {
        if (val == nullptr) {
            return nullptr;
        } else if (val->type == AST_TYPE::Ellipsis) {
            AST_Ellipsis* orig = ast_cast<AST_Ellipsis>(val);
            AST_Ellipsis* made = new AST_Ellipsis();
            made->col_offset = orig->col_offset;
            made->lineno = orig->lineno;
            return made;
        } else if (val->type == AST_TYPE::ExtSlice) {
            AST_ExtSlice* orig = ast_cast<AST_ExtSlice>(val);
            AST_ExtSlice* made = new AST_ExtSlice();
            made->col_offset = orig->col_offset;
            made->lineno = orig->lineno;
            made->dims.reserve(orig->dims.size());
            for (AST_slice* item : orig->dims) {
                made->dims.push_back(_dup(item));
            }
            return made;
        } else if (val->type == AST_TYPE::Index) {
            AST_Index* orig = ast_cast<AST_Index>(val);
            AST_Index* made = new AST_Index();
            made->value = _dup(orig->value);
            made->col_offset = orig->col_offset;
            made->lineno = orig->lineno;
            return made;
        } else if (val->type == AST_TYPE::Slice) {
            AST_Slice* orig = ast_cast<AST_Slice>(val);
            AST_Slice* made = new AST_Slice();
            made->col_offset = orig->col_offset;
            made->lineno = orig->lineno;
            made->lower = _dup(orig->lower);
            made->upper = _dup(orig->upper);
            made->step = _dup(orig->step);
            return made;
        } else {
            RELEASE_ASSERT(0, "%d", val->type);
        }
        return nullptr;
    }

    // Sometimes we want to refer to the same object twice,
    // but we require that no AST* object gets reused.
    // So instead, just create a copy of it.
    // This is only intended to be used with the primitev types,
    // ie those that can be used as operands (temp names and constants).
    AST_expr* _dup(AST_expr* val) {
        if (val == nullptr)
            return val;

        if (val->type == AST_TYPE::Name) {
            AST_Name* orig = ast_cast<AST_Name>(val);
            AST_Name* made = makeName(orig->id, orig->ctx_type, orig->lineno, orig->col_offset);
            return made;
        } else if (val->type == AST_TYPE::Num) {
            AST_Num* orig = ast_cast<AST_Num>(val);
            AST_Num* made = new AST_Num();
            made->num_type = orig->num_type;
            made->n_int = orig->n_int;
            made->n_long = orig->n_long;
            made->col_offset = orig->col_offset;
            made->lineno = orig->lineno;
            return made;
        } else if (val->type == AST_TYPE::Str) {
            AST_Str* orig = ast_cast<AST_Str>(val);
            AST_Str* made = new AST_Str();
            made->str_type = orig->str_type;
            made->str_data = orig->str_data;
            made->col_offset = orig->col_offset;
            made->lineno = orig->lineno;
            return made;
        } else {
            RELEASE_ASSERT(0, "%d", val->type);
        }
    }

    AST_expr* remapBoolOp(AST_BoolOp* node) {
        assert(curblock);

        InternedString name = nodeName();

        CFGBlock* starting_block = curblock;
        CFGBlock* exit_block = cfg->addDeferredBlock();

        for (int i = 0; i < node->values.size() - 1; i++) {
            AST_expr* val = remapExpr(node->values[i]);
            pushAssign(name, val);

            AST_Branch* br = new AST_Branch();
            br->test = callNonzero(_dup(val));
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
            pushJump(exit_block);

            curblock = next_block;
        }

        AST_expr* final_val = remapExpr(node->values[node->values.size() - 1]);
        pushAssign(name, final_val);
        pushJump(exit_block);

        cfg->placeBlock(exit_block);
        curblock = exit_block;

        return makeLoad(name, node);
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
        assert(curblock);

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
            InternedString name = nodeName();

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

                pushAssign(name, val);

                AST_Branch* br = new AST_Branch();
                br->test = callNonzero(makeLoad(name, node));
                push_back(br);

                CFGBlock* was_block = curblock;
                CFGBlock* next_block = cfg->addBlock();
                CFGBlock* crit_break_block = cfg->addBlock();
                was_block->connectTo(next_block);
                was_block->connectTo(crit_break_block);

                br->iffalse = crit_break_block;
                br->iftrue = next_block;

                curblock = crit_break_block;
                pushJump(exit_block);

                curblock = next_block;

                left = _dup(right);
            }

            pushJump(exit_block);
            cfg->placeBlock(exit_block);
            curblock = exit_block;

            return makeLoad(name, node);
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
    }

    AST_slice* remapEllipsis(AST_Ellipsis* node) { return node; }

    AST_slice* remapExtSlice(AST_ExtSlice* node) {
        AST_ExtSlice* rtn = new AST_ExtSlice();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;

        for (auto* e : node->dims)
            rtn->dims.push_back(remapSlice(e));
        return rtn;
    }

    // This is a helper function used for generators expressions and comprehensions.
    //
    // Generates a FunctionDef which produces scope for `node'. The function produced is empty, so you'd better fill it.
    // `node' had better be a kind of node that scoping_analysis thinks can carry scope (see the switch (node->type)
    // block in ScopingAnalysis::processNameUsages in analysis/scoping_analysis.cpp); e.g. a Lambda or GeneratorExp.
    AST_MakeFunction* makeFunctionForScope(AST* node) {
        AST_FunctionDef* func = new AST_FunctionDef();
        func->lineno = node->lineno;
        func->col_offset = node->col_offset;
        // TODO this should be set off the type of the comprehension (ie <setcomp> or <dictcomp> or <genexpr>)
        InternedString func_name = internString("<comprehension>");
        func->name = func_name;
        func->args = new AST_arguments();
        func->args->vararg = internString("");
        func->args->kwarg = internString("");
        scoping_analysis->registerScopeReplacement(node, func); // critical bit
        return new AST_MakeFunction(func);
    }

    // This is a helper function used for generator expressions and comprehensions.
    // TODO(rntz): use this to handle unscoped (i.e. list) comprehensions as well?
    void emitComprehensionLoops(std::vector<AST_stmt*>* insert_point,
                                const std::vector<AST_comprehension*>& comprehensions, AST_expr* first_generator,
                                std::function<void(std::vector<AST_stmt*>*)> do_yield) {
        for (int i = 0; i < comprehensions.size(); i++) {
            AST_comprehension* c = comprehensions[i];

            AST_For* loop = new AST_For();
            loop->target = c->target;
            loop->iter = (i == 0) ? first_generator : c->iter;

            insert_point->push_back(loop);
            insert_point = &loop->body;

            for (AST_expr* if_condition : c->ifs) {
                AST_If* if_block = new AST_If();
                // Note: don't call callNonzero here, since we are generating
                // AST inside a new functiondef which will go through the CFG
                // process again.
                if_block->test = if_condition;

                insert_point->push_back(if_block);
                insert_point = &if_block->body;
            }
        }

        do_yield(insert_point);
    }

    AST_expr* remapGeneratorExp(AST_GeneratorExp* node) {
        assert(node->generators.size());

        // We need to evaluate the first for-expression immediately, as the PEP dictates; so we pass it in as an
        // argument to the function we create. See
        // https://www.python.org/dev/peps/pep-0289/#early-binding-versus-late-binding
        AST_expr* first = remapExpr(node->generators[0]->iter);
        InternedString arg_name = internString("#arg");

        AST_MakeFunction* func = makeFunctionForScope(node);
        func->function_def->args->args.push_back(makeName(arg_name, AST_TYPE::Param, node->lineno));
        emitComprehensionLoops(&func->function_def->body, node->generators,
                               makeName(arg_name, AST_TYPE::Load, node->lineno),
                               [this, node](std::vector<AST_stmt*>* insert_point) {
                                   auto y = new AST_Yield();
                                   y->value = node->elt;
                                   insert_point->push_back(makeExpr(y));
                               });

        InternedString func_var_name = nodeName();
        pushAssign(func_var_name, func);

        return makeCall(makeLoad(func_var_name, node), first);
    }

    void emitComprehensionYield(AST_DictComp* node, InternedString dict_name, std::vector<AST_stmt*>* insert_point) {
        // add entry to the dictionary
        AST_expr* setitem
            = makeLoadAttribute(makeName(dict_name, AST_TYPE::Load, node->lineno), internString("__setitem__"), true);
        insert_point->push_back(makeExpr(makeCall(setitem, node->key, node->value)));
    }

    void emitComprehensionYield(AST_SetComp* node, InternedString set_name, std::vector<AST_stmt*>* insert_point) {
        // add entry to the dictionary
        AST_expr* add = makeLoadAttribute(makeName(set_name, AST_TYPE::Load, node->lineno), internString("add"), true);
        insert_point->push_back(makeExpr(makeCall(add, node->elt)));
    }

    template <typename ResultType, typename CompType> AST_expr* remapScopedComprehension(CompType* node) {
        // See comment in remapGeneratorExp re early vs. late binding.
        AST_expr* first = remapExpr(node->generators[0]->iter);
        InternedString arg_name = internString("#arg");

        AST_MakeFunction* func = makeFunctionForScope(node);
        func->function_def->args->args.push_back(makeName(arg_name, AST_TYPE::Param, node->lineno));

        InternedString rtn_name = internString("#comp_rtn");
        auto asgn = new AST_Assign();
        asgn->targets.push_back(makeName(rtn_name, AST_TYPE::Store, node->lineno));
        asgn->value = new ResultType();
        func->function_def->body.push_back(asgn);

        auto lambda =
            [&](std::vector<AST_stmt*>* insert_point) { emitComprehensionYield(node, rtn_name, insert_point); };
        AST_Name* first_name = makeName(internString("#arg"), AST_TYPE::Load, node->lineno);
        emitComprehensionLoops(&func->function_def->body, node->generators, first_name, lambda);

        auto rtn = new AST_Return();
        rtn->value = makeName(rtn_name, AST_TYPE::Load, node->lineno);
        func->function_def->body.push_back(rtn);

        InternedString func_var_name = nodeName();
        pushAssign(func_var_name, func);

        return makeCall(makeName(func_var_name, AST_TYPE::Load, node->lineno), first);
    }

    AST_expr* remapIfExp(AST_IfExp* node) {
        assert(curblock);

        InternedString rtn_name = nodeName();
        CFGBlock* iftrue = cfg->addDeferredBlock();
        CFGBlock* iffalse = cfg->addDeferredBlock();
        CFGBlock* exit_block = cfg->addDeferredBlock();

        pushBranch(remapExpr(node->test), iftrue, iffalse);

        // if true block
        cfg->placeBlock(iftrue);
        curblock = iftrue;
        iftrue->info = "iftrue";
        pushAssign(rtn_name, remapExpr(node->body));
        pushJump(exit_block);

        // if false block
        cfg->placeBlock(iffalse);
        curblock = iffalse;
        iffalse->info = "iffalse";
        pushAssign(rtn_name, remapExpr(node->orelse));
        pushJump(exit_block);

        // exit block
        cfg->placeBlock(exit_block);
        curblock = exit_block;

        return makeLoad(rtn_name, node);
    }

    AST_slice* remapIndex(AST_Index* node) {
        AST_Index* rtn = new AST_Index();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    AST_arguments* remapArguments(AST_arguments* args) {
        auto rtn = new AST_arguments();
        rtn = new AST_arguments();
        // don't remap args, they're not evaluated. NB. expensive vector copy.
        rtn->args = args->args;
        rtn->kwarg = args->kwarg;
        rtn->vararg = args->vararg;
        for (auto expr : args->defaults)
            rtn->defaults.push_back(remapExpr(expr));
        return rtn;
    }

    AST_expr* remapLambda(AST_Lambda* node) {
        auto rtn = new AST_Lambda();
        rtn->body = node->body; // don't remap now; will be CFG'ed later
        rtn->args = remapArguments(node->args);
        // lambdas create scope, need to register as replacement
        scoping_analysis->registerScopeReplacement(node, rtn);
        return rtn;
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
        rtn->ctx_type = node->ctx_type;

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

    AST_expr* remapSet(AST_Set* node) {
        AST_Set* rtn = new AST_Set();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;

        for (auto e : node->elts) {
            rtn->elts.push_back(remapExpr(e));
        }

        return rtn;
    }

    AST_slice* remapSlice(AST_Slice* node) {
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
        rtn->ctx_type = node->ctx_type;

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
        rtn->slice = remapSlice(node->slice);
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

    AST_expr* remapYield(AST_Yield* node) {
        AST_Yield* rtn = new AST_Yield();
        rtn->lineno = node->lineno;
        rtn->col_offset = node->col_offset;
        rtn->value = remapExpr(node->value);

        InternedString node_name(nodeName());
        pushAssign(node_name, rtn);

        push_back(makeExpr(new AST_LangPrimitive(AST_LangPrimitive::UNCACHE_EXC_INFO)));

        if (root_type != AST_TYPE::FunctionDef && root_type != AST_TYPE::Lambda)
            raiseExcHelper(SyntaxError, "'yield' outside function");

        return makeLoad(node_name, node);
    }

    AST_slice* remapSlice(AST_slice* node) {
        if (node == nullptr)
            return nullptr;

        AST_slice* rtn = nullptr;
        switch (node->type) {
            case AST_TYPE::Ellipsis:
                rtn = remapEllipsis(ast_cast<AST_Ellipsis>(node));
                break;
            case AST_TYPE::ExtSlice:
                rtn = remapExtSlice(ast_cast<AST_ExtSlice>(node));
                break;
            case AST_TYPE::Index:
                if (ast_cast<AST_Index>(node)->value->type == AST_TYPE::Num)
                    return node;
                rtn = remapIndex(ast_cast<AST_Index>(node));
                break;
            case AST_TYPE::Slice:
                rtn = remapSlice(ast_cast<AST_Slice>(node));
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->type);
        }
        return rtn;
    }

    // Flattens a nested expression into a flat one, emitting instructions &
    // generating temporary variables as needed.
    //
    // If `wrap_with_assign` is true, it will always return a temporary
    // variable.
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
                rtn = remapScopedComprehension<AST_Dict>(ast_cast<AST_DictComp>(node));
                break;
            case AST_TYPE::GeneratorExp:
                rtn = remapGeneratorExp(ast_cast<AST_GeneratorExp>(node));
                break;
            case AST_TYPE::IfExp:
                rtn = remapIfExp(ast_cast<AST_IfExp>(node));
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
                rtn = remapName(ast_cast<AST_Name>(node));
                break;
            case AST_TYPE::Num:
                return node;
            case AST_TYPE::Repr:
                rtn = remapRepr(ast_cast<AST_Repr>(node));
                break;
            case AST_TYPE::Set:
                rtn = remapSet(ast_cast<AST_Set>(node));
                break;
            case AST_TYPE::SetComp:
                rtn = remapScopedComprehension<AST_Set>(ast_cast<AST_SetComp>(node));
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
            case AST_TYPE::Yield:
                rtn = remapYield(ast_cast<AST_Yield>(node));
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->type);
        }

        // this is the part that actually generates temporaries & assigns to them.
        if (wrap_with_assign && (rtn->type != AST_TYPE::Name || ast_cast<AST_Name>(rtn)->id.s()[0] != '#')) {
            InternedString name = nodeName();
            pushAssign(name, rtn);
            return makeLoad(name, node);
        } else {
            return rtn;
        }
    }

    // helper for visit_{tryfinally,with}
    CFGBlock* makeFinallyCont(Why reason, AST_expr* whyexpr, CFGBlock* then_block) {
        CFGBlock* otherwise = cfg->addDeferredBlock();
        otherwise->info = "finally_otherwise";
        pushBranch(makeCompare(AST_TYPE::Eq, whyexpr, makeNum(reason)), then_block, otherwise);
        cfg->placeBlock(otherwise);
        return otherwise;
    }

    // Helper for visit_with. Performs the appropriate exit from a with-block, according to the value of `why'.
    // NB. `exit_block' is only used if `why' is FALLTHROUGH.
    void exitFinally(AST* node, Why why, CFGBlock* exit_block = nullptr) {
        switch (why) {
            case Why::RETURN:
                doReturn(makeLoad(internString(RETURN_NAME), node));
                break;
            case Why::BREAK:
                doBreak();
                break;
            case Why::CONTINUE:
                doContinue();
                break;
            case Why::FALLTHROUGH:
                assert(exit_block);
                pushJump(exit_block);
                break;
            case Why::EXCEPTION:
                assert(why != Why::EXCEPTION); // not handled here
                break;
        }
        assert(curblock == nullptr);
    }

    // helper for visit_{with,tryfinally}. Generates a branch testing the value of `whyexpr' against `why', and
    // performing the appropriate exit from the with-block if they are equal.
    // NB. `exit_block' is only used if `why' is FALLTHROUGH.
    void exitFinallyIf(AST* node, Why why, InternedString whyname, CFGBlock* exit_block = nullptr) {
        CFGBlock* do_exit = cfg->addDeferredBlock();
        do_exit->info = "with_exit_if";
        CFGBlock* otherwise = makeFinallyCont(why, makeLoad(whyname, node), do_exit);

        cfg->placeBlock(do_exit);
        curblock = do_exit;
        exitFinally(node, why, exit_block);

        curblock = otherwise;
    }

    // ---------- public methods ----------
public:
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

        if (node->type == AST_TYPE::Assign) {
            AST_Assign* asgn = ast_cast<AST_Assign>(node);
            assert(asgn->targets.size() == 1);
            if (asgn->targets[0]->type == AST_TYPE::Name) {
                AST_Name* target = ast_cast<AST_Name>(asgn->targets[0]);
                if (target->id.s()[0] != '#') {
// assigning to a non-temporary
#ifndef NDEBUG
                    if (!(asgn->value->type == AST_TYPE::Name && ast_cast<AST_Name>(asgn->value)->id.s()[0] == '#')
                        && asgn->value->type != AST_TYPE::Str && asgn->value->type != AST_TYPE::Num) {
                        fprintf(stdout, "\nError: doing a non-trivial assignment in an invoke is not allowed:\n");
                        print_ast(node);
                        printf("\n");
                        abort();
                    }
#endif
                    curblock->push_back(node);
                    return;
                } else if (asgn->value->type == AST_TYPE::Name && ast_cast<AST_Name>(asgn->value)->id.s()[0] == '#') {
                    // Assigning from one temporary name to another:
                    curblock->push_back(node);
                    return;
                } else if (asgn->value->type == AST_TYPE::Num || asgn->value->type == AST_TYPE::Str
                           || (asgn->value->type == AST_TYPE::Name
                               && ast_cast<AST_Name>(asgn->value)->id.s() == "None")) {
                    // Assigning to a temporary name from an expression that can't throw:
                    // NB. `None' can't throw in Python, because it's hardcoded
                    // (seriously, try reassigning "None" in CPython).
                    curblock->push_back(node);
                    return;
                }
            }
        }

        bool is_raise = (node->type == AST_TYPE::Raise);
        // If we invoke a raise statement, generate an invoke where both destinations
        // are the exception handler, since we know the non-exceptional path won't be taken.
        // TODO: would be much better (both more efficient and require less special casing)
        // if we just didn't generate this control flow as exceptions.

        CFGBlock* normal_dest = cfg->addBlock();
        // Add an extra exc_dest trampoline to prevent critical edges:
        CFGBlock* exc_dest;
        if (is_raise)
            exc_dest = normal_dest;
        else
            exc_dest = cfg->addBlock();

        AST_Invoke* invoke = new AST_Invoke(node);
        invoke->normal_dest = normal_dest;
        invoke->exc_dest = exc_dest;
        invoke->col_offset = node->col_offset;
        invoke->lineno = node->lineno;

        curblock->push_back(invoke);
        curblock->connectTo(normal_dest);
        if (!is_raise)
            curblock->connectTo(exc_dest);

        ExcBlockInfo& exc_info = exc_handlers.back();

        curblock = exc_dest;
        AST_Assign* exc_asgn = new AST_Assign();
        AST_Tuple* target = new AST_Tuple();
        target->elts.push_back(makeName(exc_info.exc_type_name, AST_TYPE::Store, node->lineno));
        target->elts.push_back(makeName(exc_info.exc_value_name, AST_TYPE::Store, node->lineno));
        target->elts.push_back(makeName(exc_info.exc_traceback_name, AST_TYPE::Store, node->lineno));
        exc_asgn->targets.push_back(target);

        exc_asgn->value = new AST_LangPrimitive(AST_LangPrimitive::LANDINGPAD);
        curblock->push_back(exc_asgn);

        pushJump(exc_info.exc_dest);

        if (is_raise)
            curblock = NULL;
        else
            curblock = normal_dest;
    }

    bool visit_classdef(AST_ClassDef* node) override {
        // waitaminute, who deallocates `node'?
        auto def = new AST_ClassDef();
        def->lineno = node->lineno;
        def->col_offset = node->col_offset;
        def->name = node->name;
        def->body = node->body; // expensive vector copy

        // Decorators are evaluated before bases:
        for (auto expr : node->decorator_list)
            def->decorator_list.push_back(remapExpr(expr));
        for (auto expr : node->bases)
            def->bases.push_back(remapExpr(expr));

        scoping_analysis->registerScopeReplacement(node, def);

        auto tmp = nodeName();
        pushAssign(tmp, new AST_MakeClass(def));
        // is this name mangling correct?
        pushAssign(source->mangleName(def->name), makeName(tmp, AST_TYPE::Load, node->lineno));

        return true;
    }

    bool visit_functiondef(AST_FunctionDef* node) override {
        auto def = new AST_FunctionDef();
        def->lineno = node->lineno;
        def->col_offset = node->col_offset;
        def->name = node->name;
        def->body = node->body; // expensive vector copy
        // Decorators are evaluated before the defaults, so this *must* go before remapArguments().
        // TODO(rntz): do we have a test for this
        for (auto expr : node->decorator_list)
            def->decorator_list.push_back(remapExpr(expr));
        def->args = remapArguments(node->args);

        scoping_analysis->registerScopeReplacement(node, def);

        auto tmp = nodeName();
        pushAssign(tmp, new AST_MakeFunction(def));
        // is this name mangling correct?
        pushAssign(source->mangleName(def->name), makeName(tmp, AST_TYPE::Load, node->lineno));

        return true;
    }

    bool visit_global(AST_Global* node) override {
        push_back(node);
        return true;
    }

    static llvm::StringRef getTopModule(llvm::StringRef full_name) {
        size_t period_index = full_name.find('.');
        if (period_index == std::string::npos) {
            return full_name;
        } else {
            return full_name.substr(0, period_index);
        }
    }

    bool visit_import(AST_Import* node) override {
        for (AST_alias* a : node->names) {
            AST_LangPrimitive* import = new AST_LangPrimitive(AST_LangPrimitive::IMPORT_NAME);
            import->lineno = node->lineno;
            import->col_offset = node->col_offset;

            import->args.push_back(new AST_Num());
            static_cast<AST_Num*>(import->args[0])->num_type = AST_Num::INT;

            // level == 0 means only check sys path for imports, nothing package-relative,
            // level == -1 means check both sys path and relative for imports.
            // so if `from __future__ import absolute_import` was used in the file, set level to 0
            int level;
            if (!(future_flags & CO_FUTURE_ABSOLUTE_IMPORT))
                level = -1;
            else
                level = 0;
            static_cast<AST_Num*>(import->args[0])->n_int = level;
            import->args.push_back(new AST_LangPrimitive(AST_LangPrimitive::NONE));
            import->args.push_back(new AST_Str(a->name.s()));

            InternedString tmpname = nodeName();
            pushAssign(tmpname, import);

            if (a->asname.s().size() == 0) {
                // No asname, so load the top-level module into the name
                // (e.g., for `import os.path`, loads the os module into `os`)
                pushAssign(internString(getTopModule(a->name.s())), makeLoad(tmpname, node));
            } else {
                // If there is an asname, get the bottom-level module by
                // getting the attributes and load it into asname.
                int l = 0;
                do {
                    int r = a->name.s().find('.', l);
                    if (r == std::string::npos) {
                        r = a->name.s().size();
                    }
                    if (l == 0) {
                        l = r + 1;
                        continue;
                    }
                    pushAssign(tmpname, new AST_Attribute(makeLoad(tmpname, node), AST_TYPE::Load,
                                                          internString(a->name.s().substr(l, r - l))));
                    l = r + 1;
                } while (l < a->name.s().size());
                pushAssign(a->asname, makeLoad(tmpname, node));
            }
        }

        return true;
    }

    bool visit_importfrom(AST_ImportFrom* node) override {
        AST_LangPrimitive* import = new AST_LangPrimitive(AST_LangPrimitive::IMPORT_NAME);
        import->lineno = node->lineno;
        import->col_offset = node->col_offset;

        import->args.push_back(new AST_Num());
        static_cast<AST_Num*>(import->args[0])->num_type = AST_Num::INT;

        // level == 0 means only check sys path for imports, nothing package-relative,
        // level == -1 means check both sys path and relative for imports.
        // so if `from __future__ import absolute_import` was used in the file, set level to 0
        int level;
        if (node->level == 0 && !(future_flags & CO_FUTURE_ABSOLUTE_IMPORT))
            level = -1;
        else
            level = node->level;
        static_cast<AST_Num*>(import->args[0])->n_int = level;

        import->args.push_back(new AST_Tuple());
        static_cast<AST_Tuple*>(import->args[1])->ctx_type = AST_TYPE::Load;
        for (int i = 0; i < node->names.size(); i++) {
            static_cast<AST_Tuple*>(import->args[1])->elts.push_back(new AST_Str(node->names[i]->name.s()));
        }
        import->args.push_back(new AST_Str(node->module.s()));

        InternedString tmp_module_name = nodeName();
        pushAssign(tmp_module_name, import);

        for (AST_alias* a : node->names) {
            if (a->name.s() == "*") {

                AST_LangPrimitive* import_star = new AST_LangPrimitive(AST_LangPrimitive::IMPORT_STAR);
                import_star->lineno = node->lineno;
                import_star->col_offset = node->col_offset;
                import_star->args.push_back(makeLoad(tmp_module_name, node));

                AST_Expr* import_star_expr = new AST_Expr();
                import_star_expr->value = import_star;
                import_star_expr->lineno = node->lineno;
                import_star_expr->col_offset = node->col_offset;

                push_back(import_star_expr);
            } else {
                AST_LangPrimitive* import_from = new AST_LangPrimitive(AST_LangPrimitive::IMPORT_FROM);
                import_from->lineno = node->lineno;
                import_from->col_offset = node->col_offset;
                import_from->args.push_back(makeLoad(tmp_module_name, node));
                import_from->args.push_back(new AST_Str(a->name.s()));

                InternedString tmp_import_name = nodeName();
                pushAssign(tmp_import_name, import_from);
                pushAssign(a->asname.s().size() ? a->asname : a->name, makeLoad(tmp_import_name, node));
            }
        }

        return true;
    }

    bool visit_pass(AST_Pass* node) override { return true; }

    bool visit_assert(AST_Assert* node) override {
        assert(curblock);

        AST_Branch* br = new AST_Branch();
        br->test = callNonzero(remapExpr(node->test));
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
        pushJump(unreachable);

        curblock = unreachable;
        pushJump(unreachable, true);

        curblock = iftrue;

        return true;
    }

    bool visit_assign(AST_Assign* node) override {
        AST_expr* remapped_value = remapExpr(node->value);

        for (AST_expr* target : node->targets) {
            pushAssign(target, _dup(remapped_value));
        }
        return true;
    }

    bool visit_augassign(AST_AugAssign* node) override {
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
        //
        // Finally, due to possibility of exceptions, we don't want to assign directly
        // to the final target at the same time as evaluating the augbinop

        AST_expr* remapped_target;
        AST_expr* remapped_lhs;

        // TODO bad that it's reusing the AST nodes?
        switch (node->target->type) {
            case AST_TYPE::Name: {
                AST_Name* n = ast_cast<AST_Name>(node->target);
                assert(n->ctx_type == AST_TYPE::Store);
                InternedString n_name(nodeName());
                pushAssign(n_name, makeLoad(n->id, node));
                remapped_target = n;
                remapped_lhs = makeLoad(n_name, node);
                break;
            }
            case AST_TYPE::Subscript: {
                AST_Subscript* s = ast_cast<AST_Subscript>(node->target);
                assert(s->ctx_type == AST_TYPE::Store);

                AST_Subscript* s_target = new AST_Subscript();
                s_target->value = remapExpr(s->value);
                s_target->slice = remapSlice(s->slice);
                s_target->ctx_type = AST_TYPE::Store;
                s_target->col_offset = s->col_offset;
                s_target->lineno = s->lineno;
                remapped_target = s_target;

                AST_Subscript* s_lhs = new AST_Subscript();
                s_lhs->value = _dup(s_target->value);
                s_lhs->slice = _dup(s_target->slice);
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
                a_lhs->value = _dup(a_target->value);
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
        binop->op_type = remapBinOpType(node->op_type);
        binop->left = remapped_lhs;
        binop->right = remapExpr(node->value);
        binop->col_offset = node->col_offset;
        binop->lineno = node->lineno;

        InternedString node_name(nodeName());
        pushAssign(node_name, binop);
        pushAssign(remapped_target, makeLoad(node_name, node));
        return true;
    }

    AST_TYPE::AST_TYPE remapBinOpType(AST_TYPE::AST_TYPE op_type) {
        if (op_type == AST_TYPE::Div && (future_flags & (CO_FUTURE_DIVISION))) {
            return AST_TYPE::TrueDiv;
        } else {
            return op_type;
        }
    }

    bool visit_delete(AST_Delete* node) override {
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
                    astsubs->slice = remapSlice(s->slice);
                    astsubs->ctx_type = AST_TYPE::Del;
                    target = astsubs;
                    break;
                }
                case AST_TYPE::Attribute: {
                    AST_Attribute* astattr = static_cast<AST_Attribute*>(remapExpr(t, false));
                    astattr->ctx_type = AST_TYPE::Del;
                    target = astattr;
                    break;
                }
                case AST_TYPE::Name: {
                    target = remapName(ast_cast<AST_Name>(t));
                    break;
                }
                case AST_TYPE::List: {
                    AST_List* list = static_cast<AST_List*>(t);
                    AST_Delete* temp_ast_del = new AST_Delete();
                    temp_ast_del->lineno = node->lineno;
                    temp_ast_del->col_offset = node->col_offset;

                    for (auto elt : list->elts) {
                        temp_ast_del->targets.push_back(elt);
                    }
                    visit_delete(temp_ast_del);
                    break;
                }
                case AST_TYPE::Tuple: {
                    AST_Tuple* tuple = static_cast<AST_Tuple*>(t);
                    AST_Delete* temp_ast_del = new AST_Delete();
                    temp_ast_del->lineno = node->lineno;
                    temp_ast_del->col_offset = node->col_offset;

                    for (auto elt : tuple->elts) {
                        temp_ast_del->targets.push_back(elt);
                    }
                    visit_delete(temp_ast_del);
                    break;
                }
                default:
                    RELEASE_ASSERT(0, "Unsupported del target: %d", t->type);
            }

            if (target != NULL)
                astdel->targets.push_back(target);

            if (astdel->targets.size() > 0)
                push_back(astdel);
        }

        return true;
    }

    bool visit_expr(AST_Expr* node) override {
        AST_Expr* remapped = new AST_Expr();
        remapped->lineno = node->lineno;
        remapped->col_offset = node->col_offset;
        remapped->value = remapExpr(node->value, false);
        push_back(remapped);
        return true;
    }

    bool visit_print(AST_Print* node) override {
        AST_expr* dest = remapExpr(node->dest);

        int i = 0;
        for (auto v : node->values) {
            AST_Print* remapped = new AST_Print();
            remapped->col_offset = node->col_offset;
            remapped->lineno = node->lineno;
            // TODO not good to reuse 'dest' like this
            remapped->dest = _dup(dest);

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

    bool visit_return(AST_Return* node) override {
        // returns are allowed in functions (of course), and also in eval("...") strings - basically, eval strings get
        // an implicit `return'. root_type is AST_TYPE::Expression when we're compiling an eval string.
        assert(curblock);

        if (root_type != AST_TYPE::FunctionDef && root_type != AST_TYPE::Lambda && root_type != AST_TYPE::Expression) {
            raiseExcHelper(SyntaxError, "'return' outside function");
        }

        if (!curblock)
            return true;

        doReturn(node->value ? remapExpr(node->value) : makeLoad(internString("None"), node));
        return true;
    }

    bool visit_if(AST_If* node) override {
        assert(curblock);

        AST_Branch* br = new AST_Branch();
        br->col_offset = node->col_offset;
        br->lineno = node->lineno;
        br->test = callNonzero(remapExpr(node->test));
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
            if (!curblock)
                break;
        }
        if (curblock) {
            pushJump(exit);
        }

        CFGBlock* iffalse = cfg->addBlock();
        br->iffalse = iffalse;
        starting_block->connectTo(iffalse);

        iffalse->info = "iffalse";
        curblock = iffalse;
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock) {
            pushJump(exit);
        }

        if (exit->predecessors.size() == 0) {
            curblock = NULL;
        } else {
            cfg->placeBlock(exit);
            curblock = exit;
        }

        return true;
    }

    bool visit_break(AST_Break* node) override {
        assert(curblock);

        doBreak();
        assert(!curblock);
        return true;
    }

    bool visit_continue(AST_Continue* node) override {
        assert(curblock);

        doContinue();
        assert(!curblock);
        return true;
    }

    bool visit_exec(AST_Exec* node) override {
        AST_Exec* astexec = new AST_Exec();
        astexec->lineno = node->lineno;
        astexec->col_offset = node->col_offset;
        astexec->body = remapExpr(node->body);
        astexec->globals = remapExpr(node->globals);
        astexec->locals = remapExpr(node->locals);
        push_back(astexec);
        return true;
    }

    bool visit_while(AST_While* node) override {
        assert(curblock);

        CFGBlock* test_block = cfg->addBlock();
        test_block->info = "while_test";
        pushJump(test_block);

        curblock = test_block;
        AST_Branch* br = makeBranch(remapExpr(node->test));
        CFGBlock* test_block_end = curblock;
        push_back(br);

        // We need a reference to this block early on so we can break to it,
        // but we don't want it to be placed until after the orelse.
        CFGBlock* end = cfg->addDeferredBlock();
        end->info = "while_exit";
        pushLoopContinuation(test_block, end);

        CFGBlock* body = cfg->addBlock();
        body->info = "while_body_start";
        br->iftrue = body;

        test_block_end->connectTo(body);
        curblock = body;
        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock)
            pushJump(test_block, true);
        popContinuation();

        CFGBlock* orelse = cfg->addBlock();
        orelse->info = "while_orelse_start";
        br->iffalse = orelse;
        test_block_end->connectTo(orelse);
        curblock = orelse;
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock)
            pushJump(end);

        if (end->predecessors.size() == 0) {
            delete end;
            curblock = NULL;
        } else {
            curblock = end;
            cfg->placeBlock(end);
        }

        return true;
    }

    bool visit_for(AST_For* node) override {
        assert(curblock);

        // TODO this is so complicated because I tried doing loop inversion;
        // is it really worth it?  It got so bad because all the edges became
        // critical edges and needed to be broken, otherwise it's not too different.

        AST_expr* remapped_iter = remapExpr(node->iter);
        AST_LangPrimitive* iter_call = new AST_LangPrimitive(AST_LangPrimitive::GET_ITER);
        iter_call->args.push_back(remapped_iter);

        InternedString itername = createUniqueName("#iter_");
        pushAssign(itername, iter_call);

        AST_expr* next_attr = makeLoadAttribute(makeLoad(itername, node), internString("next"), true);

        CFGBlock* test_block = cfg->addBlock();
        pushJump(test_block);
        curblock = test_block;

        AST_LangPrimitive* test_call = new AST_LangPrimitive(AST_LangPrimitive::HASNEXT);
        test_call->args.push_back(makeName(itername, AST_TYPE::Load, node->lineno));
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
        pushJump(loop_block);

        curblock = test_false;
        pushJump(else_block);

        pushLoopContinuation(test_block, end_block);

        curblock = loop_block;
        InternedString next_name(nodeName());
        pushAssign(next_name, makeCall(next_attr));
        pushAssign(node->target, makeLoad(next_name, node));

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }
        popContinuation();

        if (curblock) {
            AST_LangPrimitive* end_call = new AST_LangPrimitive(AST_LangPrimitive::HASNEXT);
            end_call->args.push_back(makeName(itername, AST_TYPE::Load, node->lineno));
            AST_Branch* end_br = makeBranch(remapExpr(end_call));
            push_back(end_br);

            CFGBlock* end_true = cfg->addBlock();
            CFGBlock* end_false = cfg->addBlock();
            end_br->iftrue = end_true;
            end_br->iffalse = end_false;
            curblock->connectTo(end_true);
            curblock->connectTo(end_false);

            curblock = end_true;
            pushJump(loop_block, true);

            curblock = end_false;
            pushJump(else_block);
        }

        cfg->placeBlock(else_block);
        curblock = else_block;

        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock)
            pushJump(end_block);

        if (end_block->predecessors.size() == 0) {
            delete end_block;
            curblock = NULL;
        } else {
            cfg->placeBlock(end_block);
            curblock = end_block;
        }

        return true;
    }

    bool visit_raise(AST_Raise* node) override {
        assert(curblock);

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

        curblock = NULL;

        return true;
    }

    bool visit_tryexcept(AST_TryExcept* node) override {
        assert(curblock);

        // The pypa parser will generate a tryexcept node inside a try-finally block with
        // no except clauses
        if (node->handlers.size() == 0) {
            assert(ENABLE_PYPA_PARSER);
            assert(node->orelse.size() == 0);

            for (AST_stmt* subnode : node->body) {
                subnode->accept(this);
                if (!curblock)
                    break;
            }
            return true;
        }

        assert(node->handlers.size() > 0);

        CFGBlock* exc_handler_block = cfg->addDeferredBlock();
        InternedString exc_type_name = nodeName("type");
        InternedString exc_value_name = nodeName("value");
        InternedString exc_traceback_name = nodeName("traceback");
        exc_handlers.push_back({ exc_handler_block, exc_type_name, exc_value_name, exc_traceback_name });

        for (AST_stmt* subnode : node->body) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        exc_handlers.pop_back();

        if (curblock) {
            for (AST_stmt* subnode : node->orelse) {
                subnode->accept(this);
                if (!curblock)
                    break;
            }
        }

        CFGBlock* join_block = cfg->addDeferredBlock();
        if (curblock)
            pushJump(join_block);

        if (exc_handler_block->predecessors.size() == 0) {
            delete exc_handler_block;
        } else {
            cfg->placeBlock(exc_handler_block);
            curblock = exc_handler_block;

            // TODO This is supposed to be exc_type_name (value doesn't matter for checking matches)
            AST_expr* exc_obj = makeLoad(exc_value_name, node);

            bool caught_all = false;
            for (AST_ExceptHandler* exc_handler : node->handlers) {
                assert(!caught_all && "bare except clause not the last one in the list?");

                CFGBlock* exc_next = nullptr;
                if (exc_handler->type) {
                    AST_expr* handled_type = remapExpr(exc_handler->type);

                    AST_LangPrimitive* is_caught_here = new AST_LangPrimitive(AST_LangPrimitive::CHECK_EXC_MATCH);
                    is_caught_here->args.push_back(_dup(exc_obj));
                    is_caught_here->args.push_back(handled_type);

                    AST_Branch* br = new AST_Branch();
                    br->test = callNonzero(remapExpr(is_caught_here));

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

                AST_LangPrimitive* set_exc_info = new AST_LangPrimitive(AST_LangPrimitive::SET_EXC_INFO);
                set_exc_info->args.push_back(makeLoad(exc_type_name, node));
                set_exc_info->args.push_back(makeLoad(exc_value_name, node));
                set_exc_info->args.push_back(makeLoad(exc_traceback_name, node));
                push_back(makeExpr(set_exc_info));

                if (exc_handler->name) {
                    pushAssign(exc_handler->name, _dup(exc_obj));
                }

                for (AST_stmt* subnode : exc_handler->body) {
                    subnode->accept(this);
                    if (!curblock)
                        break;
                }

                if (curblock) {
                    pushJump(join_block);
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
                raise->arg0 = makeLoad(exc_type_name, node);
                raise->arg1 = makeLoad(exc_value_name, node);
                raise->arg2 = makeLoad(exc_traceback_name, node);
                push_back(raise);
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

    bool visit_tryfinally(AST_TryFinally* node) override {
        assert(curblock);

        CFGBlock* exc_handler_block = cfg->addDeferredBlock();
        InternedString exc_type_name = nodeName("type");
        InternedString exc_value_name = nodeName("value");
        InternedString exc_traceback_name = nodeName("traceback");
        InternedString exc_why_name = nodeName("why");
        exc_handlers.push_back({ exc_handler_block, exc_type_name, exc_value_name, exc_traceback_name });

        CFGBlock* finally_block = cfg->addDeferredBlock();
        pushFinallyContinuation(finally_block, exc_why_name);

        for (AST_stmt* subnode : node->body) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        exc_handlers.pop_back();

        int did_why = continuations.back().did_why; // bad to just reach in like this
        popContinuation();                          // finally continuation

        if (curblock) {
            // assign the exc_*_name variables to tell irgen that they won't be undefined?
            // have an :UNDEF() langprimitive to not have to do any loading there?
            pushAssign(exc_why_name, makeNum(Why::FALLTHROUGH));
            pushJump(finally_block);
        }

        if (exc_handler_block->predecessors.size() == 0) {
            delete exc_handler_block;
        } else {
            cfg->placeBlock(exc_handler_block);
            curblock = exc_handler_block;
            pushAssign(exc_why_name, makeNum(Why::EXCEPTION));
            pushJump(finally_block);
        }

        cfg->placeBlock(finally_block);
        curblock = finally_block;

        for (AST_stmt* subnode : node->finalbody) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        if (curblock) {
            if (did_why & (1 << Why::RETURN))
                exitFinallyIf(node, Why::RETURN, exc_why_name);
            if (did_why & (1 << Why::BREAK))
                exitFinallyIf(node, Why::BREAK, exc_why_name);
            if (did_why & (1 << Why::CONTINUE))
                exitFinallyIf(node, Why::CONTINUE, exc_why_name);

            CFGBlock* reraise = cfg->addDeferredBlock();
            CFGBlock* noexc = makeFinallyCont(Why::EXCEPTION, makeLoad(exc_why_name, node), reraise);

            cfg->placeBlock(reraise);
            curblock = reraise;
            pushReraise(node, exc_type_name, exc_value_name, exc_traceback_name);

            curblock = noexc;
        }

        return true;
    }

    bool visit_with(AST_With* node) override {
        // see https://www.python.org/dev/peps/pep-0343/
        // section "Specification: the 'with' Statement"
        // which contains pseudocode for what this implements:
        //
        // mgr = (EXPR)
        // exit = type(mgr).__exit__            # not calling it yet
        // value = type(mgr).__enter__(mgr)
        // exc = True
        // try:
        //     VAR = value
        //     BLOCK
        // except:
        //     exc = False
        //     if not exit(mgr, *sys.exc_info()):
        //         raise
        // finally:
        //     if exc:
        //         exit(mgr, None, None, None)
        //
        // Unfortunately, this pseudocode isn't *quite* correct. We don't actually call type(mgr).__exit__ and
        // type(mgr).__enter__; rather, we use Python's "special method lookup rules" to find the appropriate method.
        // See https://docs.python.org/2/reference/datamodel.html#new-style-special-lookup. This is one reason we can't
        // just translate this into AST_Try{Except,Finally} nodes and recursively visit those. (If there are other
        // reasons, I've forgotten them.)
        assert(curblock);
        InternedString ctxmgrname = nodeName("ctxmgr");
        InternedString exitname = nodeName("exit");
        InternedString whyname = nodeName("why");
        InternedString exc_type_name = nodeName("exc_type");
        InternedString exc_value_name = nodeName("exc_value");
        InternedString exc_traceback_name = nodeName("exc_traceback");
        InternedString nonename = internString("None");
        CFGBlock* exit_block = cfg->addDeferredBlock();
        exit_block->info = "with_exit";

        pushAssign(ctxmgrname, remapExpr(node->context_expr));

        // TODO(rntz): for some reason, in the interpreter (but not the JIT), this is looking up __exit__ on the
        // instance rather than the class. See test/tests/with_ctxclass_instance_attrs.py.
        AST_expr* exit = makeLoadAttribute(makeLoad(ctxmgrname, node), internString("__exit__"), true);
        pushAssign(exitname, exit);

        // Oddly, this acces to __enter__ doesn't suffer from the same bug. Perhaps it has something to do with
        // __enter__ being called immediately?
        AST_expr* enter = makeLoadAttribute(makeLoad(ctxmgrname, node), internString("__enter__"), true);
        enter = remapExpr(makeCall(enter));
        if (node->optional_vars)
            pushAssign(node->optional_vars, enter);
        else
            push_back(makeExpr(enter));

        // push continuations
        CFGBlock* finally_block = cfg->addDeferredBlock();
        finally_block->info = "with_finally";
        pushFinallyContinuation(finally_block, whyname);

        CFGBlock* exc_block = cfg->addDeferredBlock();
        exc_block->info = "with_exc";
        exc_handlers.push_back({ exc_block, exc_type_name, exc_value_name, exc_traceback_name });

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }

        exc_handlers.pop_back();
        int finally_did_why = continuations.back().did_why;
        popContinuation();

        if (curblock) {
            // The try-suite finished as normal; jump to the finally block.
            pushAssign(whyname, makeNum(Why::FALLTHROUGH));
            pushJump(finally_block);
        }

        // The exception-handling block
        if (exc_block->predecessors.size() == 0) {
            // TODO(rntz): test for this case
            delete exc_block;
        } else {
            cfg->placeBlock(exc_block);
            curblock = exc_block;

            // call the context-manager's exit method
            InternedString suppressname = nodeName("suppress");
            pushAssign(suppressname, makeCall(makeLoad(exitname, node), makeLoad(exc_type_name, node),
                                              makeLoad(exc_value_name, node), makeLoad(exc_traceback_name, node)));

            // if it returns true, suppress the error and go to our exit block
            CFGBlock* reraise_block = cfg->addDeferredBlock();
            reraise_block->info = "with_reraise";
            // break potential critical edge
            CFGBlock* exiter = cfg->addDeferredBlock();
            exiter->info = "with_exiter";
            pushBranch(makeLoad(suppressname, node), exiter, reraise_block);

            cfg->placeBlock(exiter);
            curblock = exiter;
            pushJump(exit_block);

            // otherwise, reraise the exception
            cfg->placeBlock(reraise_block);
            curblock = reraise_block;
            pushReraise(node, exc_type_name, exc_value_name, exc_traceback_name);
        }

        // The finally block
        if (finally_block->predecessors.size() == 0) {
            // TODO(rntz): test for this case, "with foo: raise bar"
            delete finally_block;
        } else {
            cfg->placeBlock(finally_block);
            curblock = finally_block;
            // call the context-manager's exit method, ignoring result
            push_back(makeExpr(makeCall(makeLoad(exitname, node), makeLoad(nonename, node), makeLoad(nonename, node),
                                        makeLoad(nonename, node))));

            if (finally_did_why & (1 << Why::CONTINUE))
                exitFinallyIf(node, Why::CONTINUE, whyname);
            if (finally_did_why & (1 << Why::BREAK))
                exitFinallyIf(node, Why::BREAK, whyname);
            if (finally_did_why & (1 << Why::RETURN))
                exitFinallyIf(node, Why::RETURN, whyname);
            exitFinally(node, Why::FALLTHROUGH, exit_block);
        }

        if (exit_block->predecessors.size() == 0) {
            // FIXME(rntz): does this ever happen?
            // make a test for it!
            delete exit_block;
        } else {
            cfg->placeBlock(exit_block);
            curblock = exit_block;
        }

        return true;
    }
};

void CFG::print(llvm::raw_ostream& stream) {
    stream << "CFG:\n";
    stream << blocks.size() << " blocks\n";
    for (int i = 0; i < blocks.size(); i++)
        blocks[i]->print(stream);
}

class AssignVRegsVisitor : public NoopASTVisitor {
public:
    int index = 0;
    bool only_user_visible;
    llvm::DenseMap<InternedString, int> sym_vreg_map;
    ScopeInfo* scope_info;

    AssignVRegsVisitor(ScopeInfo* scope_info, bool only_user_visible) : only_user_visible(only_user_visible), scope_info(scope_info) {}

    bool visit_arguments(AST_arguments* node) override {
        for (AST_expr* d : node->defaults)
            d->accept(this);
        return true;
    }

    bool visit_classdef(AST_ClassDef* node) override {
        for (auto e : node->bases)
            e->accept(this);
        for (auto e : node->decorator_list)
            e->accept(this);
        return true;
    }

    bool visit_functiondef(AST_FunctionDef* node) override {
        for (auto* d : node->decorator_list)
            d->accept(this);
        node->args->accept(this);
        return true;
    }

    bool visit_lambda(AST_Lambda* node) override {
        node->args->accept(this);
        return true;
    }

    bool visit_name(AST_Name* node) override {
        if (node->vreg != -1)
            return true;

        if (only_user_visible && node->id.isCompilerCreatedName())
            return true;

        if (node->lookup_type == ScopeInfo::VarScopeType::UNKNOWN)
            node->lookup_type = scope_info->getScopeTypeOfName(node->id);

        if (node->lookup_type == ScopeInfo::VarScopeType::FAST || node->lookup_type == ScopeInfo::VarScopeType::CLOSURE)
            node->vreg = assignVReg(node->id);
        return true;
    }

    int assignVReg(InternedString id) {
        auto it = sym_vreg_map.find(id);
        if (sym_vreg_map.end() == it) {
            sym_vreg_map[id] = index;
            return index++;
        }
        return it->second;
    }
};

void CFG::assignVRegs(const ParamNames& param_names, ScopeInfo* scope_info) {
    if (has_vregs_assigned)
        return;

    AssignVRegsVisitor visitor(scope_info, true);

    // we need todo two passes: first we assign the user visible vars a vreg and than the compiler created get there value.
    for (int i=0; i<2; ++i) {
        for (CFGBlock* b : blocks) {
            for (AST_stmt* stmt : b->body) {
                stmt->accept(&visitor);
            }
        }

        for (auto* name : param_names.arg_names) {
            name->accept(&visitor);
        }

        if (param_names.vararg_name)
            param_names.vararg_name->accept(&visitor);

        if (param_names.kwarg_name)
            param_names.kwarg_name->accept(&visitor);

        if (visitor.only_user_visible) {
            visitor.only_user_visible = false;
            num_vregs_user_visible = visitor.sym_vreg_map.size();
            sym_vreg_map_user = visitor.sym_vreg_map;
        }
    }
    sym_vreg_map = std::move(visitor.sym_vreg_map);
    has_vregs_assigned = true;
}

CFG* computeCFG(SourceInfo* source, std::vector<AST_stmt*> body) {
    STAT_TIMER(t0, "us_timer_computecfg", 0);

    CFG* rtn = new CFG();

    ScopingAnalysis* scoping_analysis = source->scoping;

    CFGVisitor visitor(source, source->ast->type, source->future_flags, scoping_analysis, rtn);

    bool skip_first = false;

    if (source->ast->type == AST_TYPE::ClassDef) {
        // A classdef always starts with "__module__ = __name__"
        AST_Assign* module_assign = new AST_Assign();
        module_assign->targets.push_back(
            new AST_Name(source->getInternedStrings().get("__module__"), AST_TYPE::Store, source->ast->lineno));

        if (source->scoping->areGlobalsFromModule()) {
            static BoxedString* name_str = internStringImmortal("__name__");
            Box* module_name = source->parent_module->getattr(name_str);
            assert(module_name->cls == str_cls);
            module_assign->value = new AST_Str(static_cast<BoxedString*>(module_name)->s());
        } else {
            module_assign->value
                = new AST_Name(source->getInternedStrings().get("__name__"), AST_TYPE::Load, source->ast->lineno);
        }
        module_assign->lineno = 0;
        visitor.push_back(module_assign);

        // If the first statement is just a single string, transform it to an assignment to __doc__
        if (body.size() && body[0]->type == AST_TYPE::Expr) {
            AST_Expr* first_expr = ast_cast<AST_Expr>(body[0]);
            if (first_expr->value->type == AST_TYPE::Str) {
                AST_Assign* doc_assign = new AST_Assign();
                doc_assign->targets.push_back(
                    new AST_Name(source->getInternedStrings().get("__doc__"), AST_TYPE::Store, source->ast->lineno));
                doc_assign->value = first_expr->value;
                doc_assign->lineno = 0;
                visitor.push_back(doc_assign);
                skip_first = true;
            }
        }
    }

    if (source->ast->type == AST_TYPE::FunctionDef || source->ast->type == AST_TYPE::Lambda) {
        // Unpack tuple arguments
        // Tuple arguments get assigned names ".0", ".1" etc. So this
        // def f(a, (b,c), (d,e)):
        // would expand to:
        // def f(a, .1, .2):
        //     (b, c) = .1
        //     (d, e) = .2
        AST_arguments* args;
        if (source->ast->type == AST_TYPE::FunctionDef) {
            args = ast_cast<AST_FunctionDef>(source->ast)->args;
        } else {
            args = ast_cast<AST_Lambda>(source->ast)->args;
        }
        int counter = 0;
        for (AST_expr* arg_expr : args->args) {
            if (arg_expr->type == AST_TYPE::Tuple) {
                InternedString arg_name = source->getInternedStrings().get("." + std::to_string(counter));
                AST_Name* arg_name_expr
                    = new AST_Name(arg_name, AST_TYPE::Load, arg_expr->lineno, arg_expr->col_offset);
                visitor.pushAssign(arg_expr, arg_name_expr);
            } else {
                assert(arg_expr->type == AST_TYPE::Name);
            }
            counter++;
        }
    }

    for (int i = (skip_first ? 1 : 0); i < body.size(); i++) {
        if (!visitor.curblock)
            break;
        body[i]->accept(&visitor);
    }

    // The functions we create for classdefs are supposed to return a dictionary of their locals.
    // This is the place that we add all of that:
    if (source->ast->type == AST_TYPE::ClassDef) {
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

    if (VERBOSITY("cfg") >= 3) {
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
                   || terminator->type == AST_TYPE::Raise);
        }

        if (b->predecessors.size() == 0) {
            if (b != rtn->getStartingBlock()) {
                rtn->print();
                printf("%s\n", source->getName()->c_str());
            }
            ASSERT(b == rtn->getStartingBlock(), "%d", b->idx);
        }
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

    std::vector<AST*> flattened;
    for (auto b : rtn->blocks)
        flatten(b->body, flattened, true);

    std::unordered_map<AST*, int> deduped;
    bool no_dups = true;
    for (auto e : flattened) {
        deduped[e]++;
        if (deduped[e] == 2) {
            printf("Duplicated: ");
            print_ast(e);
            printf("\n");
            no_dups = false;
        }
    }
    if (!no_dups)
        rtn->print();
    assert(no_dups);

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

            AST_TYPE::AST_TYPE end_ast_type = b->body[b->body.size() - 1]->type;
            assert(end_ast_type == AST_TYPE::Jump || end_ast_type == AST_TYPE::Invoke);
            if (end_ast_type == AST_TYPE::Invoke) {
                // TODO probably shouldn't be generating these anyway:
                auto invoke = ast_cast<AST_Invoke>(b->body.back());
                assert(invoke->normal_dest == invoke->exc_dest);
                break;
            }

            if (VERBOSITY("cfg") >= 2) {
                // rtn->print();
                printf("Joining blocks %d and %d\n", b->idx, b2->idx);
            }

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
