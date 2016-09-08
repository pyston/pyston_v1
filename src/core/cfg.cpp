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

#include "core/cfg.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "Python.h"

#include "analysis/scoping_analysis.h"
#include "codegen/unwinding.h"
#include "core/bst.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

void fillScopingInfo(BST_Name* node, ScopeInfo* scope_info) {
    node->lookup_type = scope_info->getScopeTypeOfName(node->id);

    if (node->lookup_type == ScopeInfo::VarScopeType::CLOSURE)
        node->closure_offset = scope_info->getClosureOffset(node->id);
    else if (node->lookup_type == ScopeInfo::VarScopeType::DEREF)
        node->deref_info = scope_info->getDerefInfo(node->id);

    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
}

ParamNames::ParamNames(AST_arguments* arguments, InternedStringPool& pool)
    : all_args_contains_names(1), takes_param_names(1), has_vararg_name(0), has_kwarg_name(0) {
    if (!arguments)
        return;

    for (int i = 0; i < arguments->args.size(); i++) {
        AST_expr* arg = arguments->args[i];
        if (arg->type == AST_TYPE::Name) {
            AST_Name* name = ast_cast<AST_Name>(arg);
            BST_Name* new_name = new BST_Name(name->id, name->ctx_type, name->lineno);
            all_args.emplace_back(new_name);
        } else {
            InternedString dot_arg_name = pool.get("." + std::to_string(i));
            auto new_name = new BST_Name(dot_arg_name, AST_TYPE::Param, arg->lineno);
            new_name->lookup_type = ScopeInfo::VarScopeType::FAST;
            all_args.emplace_back(new_name);
        }
    }

    auto vararg_name = arguments->vararg;
    if (vararg_name) {
        has_vararg_name = 1;
        BST_Name* new_name = new BST_Name(vararg_name->id, vararg_name->ctx_type, vararg_name->lineno);
        all_args.emplace_back(new_name);
    }

    auto kwarg_name = arguments->kwarg;
    if (kwarg_name) {
        has_kwarg_name = 1;
        BST_Name* new_name = new BST_Name(kwarg_name->id, kwarg_name->ctx_type, kwarg_name->lineno);
        all_args.emplace_back(new_name);
    }
}

ParamNames::ParamNames(const std::vector<const char*>& args, const char* vararg, const char* kwarg)
    : all_args_contains_names(0),
      takes_param_names(1),
      has_vararg_name(vararg && *vararg),
      has_kwarg_name(kwarg && *kwarg) {
    all_args.reserve(args.size() + has_vararg_name + has_kwarg_name);
    for (auto&& arg : args) {
        all_args.emplace_back(arg);
    }
    if (has_vararg_name)
        all_args.emplace_back(vararg);
    if (has_kwarg_name)
        all_args.emplace_back(kwarg);
}

std::vector<const char*> ParamNames::allArgsAsStr() const {
    std::vector<const char*> ret;
    ret.reserve(all_args.size());
    if (all_args_contains_names) {
        for (auto&& arg : all_args) {
            ret.push_back(arg.name->id.c_str());
        }
    } else {
        for (auto&& arg : all_args) {
            ret.push_back(arg.str);
        }
    }
    return ret;
}

// getLastLineno and getLastLinenoSub: gets the last line of a block.
// getLastLineno takes the block itself, and getLastLinenoSub takes an entry
// inside the block.  This is important because if there is a functiondef as the last
// statement in a block, we should not look inside it.
static int getLastLinenoSub(AST* ast) {
    if (ast->type == AST_TYPE::TryExcept) {
        auto te = ast_cast<AST_TryExcept>(ast);
        if (!te->orelse.empty())
            return getLastLinenoSub(te->orelse.back());
        return getLastLinenoSub(te->handlers.back()->body.back());
    }
    if (ast->type == AST_TYPE::For) {
        return getLastLinenoSub(ast_cast<AST_For>(ast)->body.back());
    }
    if (ast->type == AST_TYPE::While) {
        return getLastLinenoSub(ast_cast<AST_While>(ast)->body.back());
    }
    if (ast->type == AST_TYPE::TryFinally) {
        return getLastLinenoSub(ast_cast<AST_TryFinally>(ast)->finalbody.back());
    }
    if (ast->type == AST_TYPE::With) {
        return getLastLinenoSub(ast_cast<AST_With>(ast)->body.back());
    }
    if (ast->type == AST_TYPE::If) {
        auto if_ = ast_cast<AST_If>(ast);
        if (!if_->orelse.empty())
            return getLastLinenoSub(if_->orelse.back());
        return getLastLinenoSub(if_->body.back());
    }

    // TODO: this is not quite right if the last statement is multiline.  See exited_lineno_multiline.py
    return ast->lineno;
}

static int getLastLineno(llvm::ArrayRef<AST_stmt*> body, int default_lineno) {
    if (body.size() == 0)
        return default_lineno;
    return getLastLinenoSub(body.back());
}

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

// A class that manages the computation of all CFGs in a module
class ModuleCFGProcessor {
public:
    ScopingAnalysis scoping;
    InternedStringPool& stringpool;
    FutureFlags future_flags;
    BoxedString* fn;
    BoxedModule* bm;

    ModuleCFGProcessor(AST* ast, bool globals_from_module, FutureFlags future_flags, BoxedString* fn, BoxedModule* bm)
        : scoping(ast, globals_from_module),
          stringpool(ast->getStringpool()),
          future_flags(future_flags),
          fn(fn),
          bm(bm) {}

    // orig_node is the node from the original ast, but 'ast' can be a desugared version.
    // For example if we convert a generator expression into a function, the new function
    // should get passed as 'ast', but the original generator expression should get
    // passed as 'orig_node' so that the scoping analysis can know what we're talking about.
    BoxedCode* runRecursively(llvm::ArrayRef<AST_stmt*> body, BoxedString* name, int lineno, AST_arguments* args,
                              AST* orig_node);
};

static CFG* computeCFG(llvm::ArrayRef<AST_stmt*> body, AST_TYPE::AST_TYPE ast_type, int lineno, AST_arguments* args,
                       BoxedString* filename, SourceInfo* source, const ParamNames& param_names, ScopeInfo* scoping,
                       ModuleCFGProcessor* cfgizer);

// A class that crawls the AST of a single function and computes the CFG
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

        // Similar to did_why: says whether the block might have been jumped-to
        bool maybe_taken;
    };

    // ---------- Member fields ----------
private:
    BoxedString* filename;
    SourceInfo* source;
    InternedStringPool& stringpool;
    ScopeInfo* scoping;
    // `root_type' is the type of the root of the AST tree that we are turning
    // into a CFG. Used when we find a "return" to check that we're inside a
    // function (otherwise we SyntaxError).
    AST_TYPE::AST_TYPE root_type;
    FutureFlags future_flags;
    CFG* cfg;
    ModuleCFGProcessor* cfgizer;

    CFGBlock* curblock;
    std::vector<ContInfo> continuations;
    std::vector<ExcBlockInfo> exc_handlers;

    unsigned int next_var_index = 0;

    friend CFG* computeCFG(llvm::ArrayRef<AST_stmt*> body, AST_TYPE::AST_TYPE ast_type, int lineno, AST_arguments* args,
                           BoxedString* filename, SourceInfo* source, const ParamNames& param_names, ScopeInfo* scoping,
                           ModuleCFGProcessor* cfgizer);

public:
    CFGVisitor(BoxedString* filename, SourceInfo* source, InternedStringPool& stringpool, ScopeInfo* scoping,
               AST_TYPE::AST_TYPE root_type, FutureFlags future_flags, CFG* cfg, ModuleCFGProcessor* cfgizer)
        : filename(filename),
          source(source),
          stringpool(stringpool),
          scoping(scoping),
          root_type(root_type),
          future_flags(future_flags),
          cfg(cfg),
          cfgizer(cfgizer) {
        curblock = cfg->addBlock();
        curblock->info = "entry";
    }

    ~CFGVisitor() {
        // if we're being destroyed due to an exception, our internal invariants may be violated, but that's okay; the
        // CFG isn't going to get used anyway. (Maybe we should check that it won't be used somehow?)
        assert(continuations.size() == 0 || isUnwinding());
        assert(exc_handlers.size() == 0 || isUnwinding());
    }

    // ---------- private methods ----------
private:
    template <typename T> InternedString internString(T&& s) { return stringpool.get(std::forward<T>(s)); }

    InternedString createUniqueName(llvm::Twine prefix) {
        std::string name = (prefix + llvm::Twine(next_var_index++)).str();
        return stringpool.get(std::move(name));
    }

    AST_Name* makeASTName(ASTAllocator& allocator, InternedString id, AST_TYPE::AST_TYPE ctx_type, int lineno,
                          int col_offset = 0) {
        AST_Name* name = new (allocator) AST_Name(id, ctx_type, lineno, col_offset);
        return name;
    }

    BST_Name* makeName(InternedString id, AST_TYPE::AST_TYPE ctx_type, int lineno, bool is_kill = false) {
        BST_Name* name = new BST_Name(id, ctx_type, lineno);
        fillScopingInfo(name, scoping);
        name->is_kill = is_kill;
        return name;
    }

    BST_Name* makeLoad(InternedString id, AST* node, bool is_kill = false) {
        return makeName(id, AST_TYPE::Load, node->lineno, is_kill);
    }

    BST_Name* makeLoad(InternedString id, BST* node, bool is_kill = false) {
        return makeName(id, AST_TYPE::Load, node->lineno, is_kill);
    }

    BST_Name* makeLoad(InternedString id, int lineno, bool is_kill = false) {
        return makeName(id, AST_TYPE::Load, lineno, is_kill);
    }

    void pushLoopContinuation(CFGBlock* continue_dest, CFGBlock* break_dest) {
        assert(continue_dest
               != break_dest); // I guess this doesn't have to be true, but validates passing say_why=false
        continuations.emplace_back(continue_dest, break_dest, nullptr, false, internString(""));
    }

    void pushFinallyContinuation(CFGBlock* finally_block, InternedString why_name) {
        continuations.emplace_back(finally_block, finally_block, finally_block, true, why_name);
    }

    void popContinuation() { continuations.pop_back(); }

    void doReturn(BST_expr* value) {
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

        BST_Return* node = new BST_Return();
        node->value = value;
        node->lineno = value->lineno;
        push_back(node);
        curblock = NULL;
    }

    void doContinue(AST* value) {
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

        raiseSyntaxError("'continue' not properly in loop", value->lineno, value->col_offset, filename->s(), "", true);
    }

    void doBreak(AST* value) {
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

        raiseSyntaxError("'break' outside loop", value->lineno, value->col_offset, filename->s(), "", true);
    }

    BST_expr* callNonzero(BST_expr* e) {
        BST_LangPrimitive* call = new BST_LangPrimitive(BST_LangPrimitive::NONZERO);
        call->args.push_back(e);
        call->lineno = e->lineno;

        // Simple optimization: allow the generation of nested nodes if there isn't a
        // current exc handler.
        if (exc_handlers.size() == 0)
            return call;

        auto name = nodeName();
        pushAssign(name, call);
        return makeLoad(name, e, true);
    }

    BST_Name* remapName(AST_Name* name) {
        if (!name)
            return NULL;
        BST_Name* rtn = new BST_Name(name->id, name->ctx_type, name->lineno);
        fillScopingInfo(rtn, scoping);
        return rtn;
    }

    BST_Num* remapNum(AST_Num* num) {
        auto r = new BST_Num();
        r->lineno = num->lineno;
        r->num_type = num->num_type;
        switch (r->num_type) {
            case AST_Num::INT:
                r->n_int = num->n_int;
                break;
            case AST_Num::FLOAT:
            case AST_Num::COMPLEX:
                r->n_float = num->n_float;
                break;
            case AST_Num::LONG:
                r->n_long = num->n_long;
                break;
        }
        return r;
    }

    BST_Str* remapStr(AST_Str* str) {
        auto r = new BST_Str();
        r->str_data = str->str_data;
        r->str_type = str->str_type;
        r->lineno = str->lineno;
        return r;
    }

    BST_expr* applyComprehensionCall(AST_ListComp* node, BST_Name* name) {
        BST_expr* elt = remapExpr(node->elt);
        return makeCall(makeLoadAttribute(name, internString("append"), true), elt);
    }

    template <typename ResultASTType, typename CompType> BST_expr* remapComprehension(CompType* node) {
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

            BST_expr* remapped_iter = remapExpr(c->iter);
            BST_LangPrimitive* iter_call = new BST_LangPrimitive(BST_LangPrimitive::GET_ITER);
            iter_call->args.push_back(remapped_iter);
            iter_call->lineno = c->target->lineno; // Not sure if this should be c->target or c->iter
            InternedString iter_name = nodeName("lc_iter", i);
            pushAssign(iter_name, iter_call);

            BST_expr* next_attr = makeLoadAttribute(makeLoad(iter_name, node), internString("next"), true);

            CFGBlock* test_block = cfg->addBlock();
            test_block->info = "comprehension_test";
            // printf("Test block for comp %d is %d\n", i, test_block->idx);
            pushJump(test_block);

            curblock = test_block;
            BST_LangPrimitive* test_call = new BST_LangPrimitive(BST_LangPrimitive::HASNEXT);
            test_call->args.push_back(makeName(iter_name, AST_TYPE::Load, node->lineno));
            test_call->lineno = c->target->lineno;
            BST_expr* test = wrap(test_call);

            CFGBlock* body_block = cfg->addBlock();
            body_block->info = "comprehension_body";
            CFGBlock* exit_block = cfg->addDeferredBlock();
            exit_block->info = "comprehension_exit";
            exit_blocks.push_back(exit_block);
            // printf("Body block for comp %d is %d\n", i, body_block->idx);

            BST_Branch* br = new BST_Branch();
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
            pushAssign(c->target, makeLoad(next_name, node, true));

            for (AST_expr* if_condition : c->ifs) {
                BST_expr* remapped = callNonzero(remapExpr(if_condition));
                BST_Branch* br = new BST_Branch();
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
                push_back(makeKill(iter_name));
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

        return makeLoad(rtn_name, node, /* is_kill */ true);
    }

    BST_expr* makeNum(int n) {
        BST_Num* node = new BST_Num();
        node->num_type = AST_Num::INT;
        node->n_int = n;
        return node;
    }

    void pushJump(CFGBlock* target, bool allow_backedge = false, int lineno = 0) {
        BST_Jump* rtn = new BST_Jump();
        rtn->target = target;
        rtn->lineno = lineno;

        push_back(rtn);
        curblock->connectTo(target, allow_backedge);
        curblock = nullptr;
    }

    // NB. can generate blocks, because callNonzero can
    BST_Branch* makeBranch(BST_expr* test) {
        BST_Branch* rtn = new BST_Branch();
        rtn->test = callNonzero(test);
        rtn->lineno = test->lineno;
        return rtn;
    }

    // NB. this can (but usually doesn't) generate new blocks, which is why we require `iftrue' and `iffalse' to be
    // deferred, to avoid heisenbugs. of course, this doesn't allow these branches to be backedges, but that hasn't yet
    // been necessary.
    void pushBranch(BST_expr* test, CFGBlock* iftrue, CFGBlock* iffalse) {
        assert(iftrue->idx == -1 && iffalse->idx == -1);
        BST_Branch* branch = makeBranch(test);
        branch->iftrue = iftrue;
        branch->iffalse = iffalse;
        curblock->connectTo(iftrue);
        curblock->connectTo(iffalse);
        push_back(branch);
        curblock = nullptr;
    }

    void pushReraise(int lineno, InternedString exc_type_name, InternedString exc_value_name,
                     InternedString exc_traceback_name) {
        auto raise = new BST_Raise();
        raise->lineno = lineno;
        raise->arg0 = makeLoad(exc_type_name, lineno, true);
        raise->arg1 = makeLoad(exc_value_name, lineno, true);
        raise->arg2 = makeLoad(exc_traceback_name, lineno, true);
        push_back(raise);
        curblock = nullptr;
    }

    AST_expr* makeASTLoadAttribute(ASTAllocator& allocator, AST_expr* base, InternedString name, bool clsonly) {
        AST_expr* rtn;
        if (clsonly) {
            AST_ClsAttribute* attr = new (allocator) AST_ClsAttribute();
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        } else {
            AST_Attribute* attr = new (allocator) AST_Attribute();
            attr->ctx_type = AST_TYPE::Load;
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        }
        rtn->col_offset = base->col_offset;
        rtn->lineno = base->lineno;
        return rtn;
    }

    BST_expr* makeLoadAttribute(BST_expr* base, InternedString name, bool clsonly) {
        BST_expr* rtn;
        if (clsonly) {
            BST_ClsAttribute* attr = new BST_ClsAttribute();
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        } else {
            BST_Attribute* attr = new BST_Attribute();
            attr->ctx_type = AST_TYPE::Load;
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        }
        rtn->lineno = base->lineno;
        return rtn;
    }

    AST_Call* makeASTCall(ASTAllocator& allocator, AST_expr* func) {
        AST_Call* call = new (allocator) AST_Call();
        call->starargs = NULL;
        call->kwargs = NULL;
        call->func = func;
        call->col_offset = func->col_offset;
        call->lineno = func->lineno;
        return call;
    }

    AST_Call* makeASTCall(ASTAllocator& allocator, AST_expr* func, AST_expr* arg0) {
        auto call = makeASTCall(allocator, func);
        call->args.push_back(arg0);
        return call;
    }

    AST_Call* makeASTCall(ASTAllocator& allocator, AST_expr* func, AST_expr* arg0, AST_expr* arg1) {
        auto call = makeASTCall(allocator, func);
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        return call;
    }

    BST_Call* makeCall(BST_expr* func) {
        BST_Call* call = new BST_Call();
        call->starargs = NULL;
        call->kwargs = NULL;
        call->func = func;
        call->lineno = func->lineno;
        return call;
    }

    BST_Call* makeCall(BST_expr* func, BST_expr* arg0) {
        auto call = makeCall(func);
        call->args.push_back(arg0);
        return call;
    }

    BST_Call* makeCall(BST_expr* func, BST_expr* arg0, BST_expr* arg1) {
        auto call = makeCall(func);
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        return call;
    }

    BST_Call* makeCall(BST_expr* func, BST_expr* arg0, BST_expr* arg1, BST_expr* arg2) {
        auto call = makeCall(func);
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        call->args.push_back(arg2);
        return call;
    }

    BST_Compare* makeCompare(AST_TYPE::AST_TYPE oper, BST_expr* left, BST_expr* right) {
        auto compare = new BST_Compare();
        compare->op = oper;
        compare->left = left;
        compare->comparator = right;
        return compare;
    }

    BST_Index* makeIndex(BST_expr* value) {
        auto index = new BST_Index();
        index->value = value;
        index->lineno = value->lineno;
        return index;
    }

    BST_Subscript* makeSubscript(AST_TYPE::AST_TYPE ctx_type, BST_expr* value, BST_slice* slice) {
        auto subscript = new BST_Subscript();
        subscript->ctx_type = ctx_type;
        subscript->value = value;
        subscript->slice = slice;
        subscript->lineno = slice->lineno;
        return subscript;
    }

    // We need this both on asts (ex assigning to the element name in a for loop), and
    // for bsts (desugaring an augassign)
    // TODO: we should get rid of this bst one, or at least not call it the same thing?
    void pushAssign(BST_expr* target, BST_expr* val) {
        BST_Assign* assign = new BST_Assign();
        assign->value = val;
        assign->lineno = val->lineno;
        assign->target = target;
        push_back(assign);
    }

    void pushAssign(AST_expr* target, BST_expr* val) {
        BST_Assign* assign = new BST_Assign();
        assign->value = val;
        assign->lineno = val->lineno;

        if (target->type == AST_TYPE::Name) {
            assign->target = makeName(ast_cast<AST_Name>(target)->id, AST_TYPE::Store, val->lineno, 0);
            push_back(assign);
        } else if (target->type == AST_TYPE::Subscript) {
            AST_Subscript* s = ast_cast<AST_Subscript>(target);
            assert(s->ctx_type == AST_TYPE::Store);

            BST_Subscript* s_target = new BST_Subscript();
            s_target->value = remapExpr(s->value);
            s_target->slice = remapSlice(s->slice);
            s_target->ctx_type = AST_TYPE::Store;
            s_target->lineno = s->lineno;

            assign->target = s_target;
            push_back(assign);
        } else if (target->type == AST_TYPE::Attribute) {
            AST_Attribute* a = ast_cast<AST_Attribute>(target);
            assert(a->ctx_type == AST_TYPE::Store);

            BST_Attribute* a_target = new BST_Attribute();
            a_target->value = remapExpr(a->value);
            a_target->attr = scoping->mangleName(a->attr);
            a_target->ctx_type = AST_TYPE::Store;
            a_target->lineno = a->lineno;

            assign->target = a_target;
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

            BST_Tuple* new_target = new BST_Tuple();
            new_target->ctx_type = AST_TYPE::Store;
            new_target->lineno = target->lineno;

            // A little hackery: push the assign, even though we're not done constructing it yet,
            // so that we can iteratively push more stuff after it
            assign->target = new_target;
            push_back(assign);

            for (int i = 0; i < elts->size(); i++) {
                InternedString tmp_name = nodeName("", i);
                new_target->elts.push_back(makeName(tmp_name, AST_TYPE::Store, target->lineno));

                pushAssign((*elts)[i], makeLoad(tmp_name, target, /* is_kill */ true));
            }
        } else {
            RELEASE_ASSERT(0, "%d", target->type);
        }
    }

    void pushAssign(InternedString id, BST_expr* val) {
        BST_Assign* assign = new BST_Assign();
        assign->value = val;
        assign->lineno = val->lineno;
        assign->target = makeName(id, AST_TYPE::Store, val->lineno, 0);
        push_back(assign);
    }

    AST_stmt* makeASTExpr(ASTAllocator& allocator, AST_expr* expr) {
        AST_Expr* stmt = new (allocator) AST_Expr();
        stmt->value = expr;
        stmt->lineno = expr->lineno;
        stmt->col_offset = expr->col_offset;
        return stmt;
    }

    BST_stmt* makeExpr(BST_expr* expr) {
        BST_Expr* stmt = new BST_Expr();
        stmt->value = expr;
        stmt->lineno = expr->lineno;
        return stmt;
    }

    InternedString nodeName() { return createUniqueName("#"); }

    InternedString nodeName(llvm::StringRef suffix) { return createUniqueName(llvm::Twine("#") + suffix + "_"); }

    InternedString nodeName(llvm::StringRef suffix, int idx) {
        return createUniqueName(llvm::Twine("#") + suffix + "_" + llvm::Twine(idx) + "_");
    }

    BST_expr* remapAttribute(AST_Attribute* node) {
        BST_Attribute* rtn = new BST_Attribute();

        rtn->lineno = node->lineno;
        rtn->ctx_type = node->ctx_type;
        rtn->attr = scoping->mangleName(node->attr);
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    BST_expr* remapBinOp(AST_BinOp* node) {
        BST_BinOp* rtn = new BST_BinOp();
        rtn->lineno = node->lineno;
        rtn->op_type = remapBinOpType(node->op_type);
        rtn->left = remapExpr(node->left);
        rtn->right = remapExpr(node->right);
        return rtn;
    }

    BST_slice* _dup(BST_slice* val) {
        if (val == nullptr) {
            return nullptr;
        } else if (val->type == BST_TYPE::Ellipsis) {
            BST_Ellipsis* orig = bst_cast<BST_Ellipsis>(val);
            BST_Ellipsis* made = new BST_Ellipsis();
            made->lineno = orig->lineno;
            return made;
        } else if (val->type == BST_TYPE::ExtSlice) {
            BST_ExtSlice* orig = bst_cast<BST_ExtSlice>(val);
            BST_ExtSlice* made = new BST_ExtSlice();
            made->lineno = orig->lineno;
            made->dims.reserve(orig->dims.size());
            for (BST_slice* item : orig->dims) {
                made->dims.push_back(_dup(item));
            }
            return made;
        } else if (val->type == BST_TYPE::Index) {
            BST_Index* orig = bst_cast<BST_Index>(val);
            BST_Index* made = new BST_Index();
            made->value = _dup(orig->value);
            made->lineno = orig->lineno;
            return made;
        } else if (val->type == BST_TYPE::Slice) {
            BST_Slice* orig = bst_cast<BST_Slice>(val);
            BST_Slice* made = new BST_Slice();
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
    BST_expr* _dup(BST_expr* val) {
        if (val == nullptr)
            return val;

        if (val->type == BST_TYPE::Name) {
            BST_Name* orig = bst_cast<BST_Name>(val);
            BST_Name* made = makeName(orig->id, orig->ctx_type, orig->lineno);
            return made;
        } else if (val->type == BST_TYPE::Num) {
            BST_Num* orig = bst_cast<BST_Num>(val);
            BST_Num* made = new BST_Num();
            made->num_type = orig->num_type;
            made->n_int = orig->n_int;
            made->n_long = orig->n_long;
            made->lineno = orig->lineno;
            return made;
        } else if (val->type == BST_TYPE::Str) {
            BST_Str* orig = bst_cast<BST_Str>(val);
            BST_Str* made = new BST_Str();
            made->str_type = orig->str_type;
            made->str_data = orig->str_data;
            made->lineno = orig->lineno;
            return made;
        } else {
            RELEASE_ASSERT(0, "%d", val->type);
        }
    }

    BST_expr* remapBoolOp(AST_BoolOp* node) {
        assert(curblock);

        InternedString name = nodeName();

        CFGBlock* starting_block = curblock;
        CFGBlock* exit_block = cfg->addDeferredBlock();

        for (int i = 0; i < node->values.size() - 1; i++) {
            BST_expr* val = remapExpr(node->values[i]);
            pushAssign(name, _dup(val));

            BST_Branch* br = new BST_Branch();
            br->test = callNonzero(val);
            br->lineno = val->lineno;
            push_back(br);

            CFGBlock* was_block = curblock;
            CFGBlock* next_block = cfg->addBlock();
            CFGBlock* crit_break_block = cfg->addBlock();
            was_block->connectTo(next_block);
            was_block->connectTo(crit_break_block);

            if (node->op_type == AST_TYPE::Or) {
                br->iftrue = crit_break_block;
                br->iffalse = next_block;
            } else if (node->op_type == AST_TYPE::And) {
                br->iffalse = crit_break_block;
                br->iftrue = next_block;
            } else {
                RELEASE_ASSERT(0, "");
            }

            curblock = crit_break_block;
            pushJump(exit_block);

            curblock = next_block;
        }

        BST_expr* final_val = remapExpr(node->values[node->values.size() - 1]);
        pushAssign(name, final_val);
        pushJump(exit_block);

        cfg->placeBlock(exit_block);
        curblock = exit_block;

        return makeLoad(name, node, true);
    }

    BST_expr* remapCall(AST_Call* node) {
        BST_Call* rtn = new BST_Call();
        rtn->lineno = node->lineno;

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
            BST_keyword* kw = new BST_keyword();
            kw->value = remapExpr(e->value);
            kw->arg = e->arg;
            rtn->keywords.push_back(kw);
        }
        rtn->starargs = remapExpr(node->starargs);
        rtn->kwargs = remapExpr(node->kwargs);

        return rtn;
    }

    BST_expr* remapClsAttribute(AST_ClsAttribute* node) {
        BST_ClsAttribute* rtn = new BST_ClsAttribute();

        rtn->lineno = node->lineno;
        rtn->attr = node->attr;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    BST_expr* remapCompare(AST_Compare* node) {
        assert(curblock);

        // special case unchained comparisons to avoid generating a unnecessary complex cfg.
        if (node->ops.size() == 1) {
            BST_Compare* rtn = new BST_Compare();
            rtn->lineno = node->lineno;

            rtn->op = node->ops[0];

            rtn->left = remapExpr(node->left);
            assert(node->comparators.size() == 1);
            rtn->comparator = remapExpr(node->comparators[0]);
            return rtn;
        } else {
            InternedString name = nodeName();

            CFGBlock* exit_block = cfg->addDeferredBlock();
            BST_expr* left = remapExpr(node->left);

            for (int i = 0; i < node->ops.size(); i++) {
                if (i > 0)
                    push_back(makeKill(name));

                BST_expr* right = remapExpr(node->comparators[i]);

                BST_Compare* val = new BST_Compare;
                val->lineno = node->lineno;
                val->left = left;
                if (i < node->ops.size() - 1)
                    val->comparator = _dup(right);
                else
                    val->comparator = right;
                val->op = node->ops[i];

                pushAssign(name, val);

                if (i == node->ops.size() - 1) {
                    continue;
                }

                BST_Branch* br = new BST_Branch();
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

                left = right;
            }

            pushJump(exit_block);
            cfg->placeBlock(exit_block);
            curblock = exit_block;

            return makeLoad(name, node, true);
        }
    }

    BST_expr* remapDict(AST_Dict* node) {
        BST_Dict* rtn = new BST_Dict();
        rtn->lineno = node->lineno;

        InternedString dict_name = nodeName();

        pushAssign(dict_name, rtn);

        for (int i = 0; i < node->keys.size(); i++) {
            BST_expr* value = remapExpr(node->values[i]);
            BST_Index* index = makeIndex(remapExpr(node->keys[i]));
            BST_Subscript* subscript = makeSubscript(AST_TYPE::Store, makeLoad(dict_name, node, false), index);
            pushAssign(subscript, value);
        }

        return makeLoad(dict_name, node, true);
    }

    BST_slice* remapEllipsis(AST_Ellipsis* node) {
        auto r = new BST_Ellipsis();
        r->lineno = node->lineno;
        return r;
    }

    BST_slice* remapExtSlice(AST_ExtSlice* node) {
        BST_ExtSlice* rtn = new BST_ExtSlice();
        rtn->lineno = node->lineno;

        for (auto* e : node->dims)
            rtn->dims.push_back(remapSlice(e));
        return rtn;
    }

    // This is a helper function used for generator expressions and comprehensions.
    // TODO(rntz): use this to handle unscoped (i.e. list) comprehensions as well?
    void emitComprehensionLoops(ASTAllocator& allocator, int lineno, std::vector<AST_stmt*>* insert_point,
                                const std::vector<AST_comprehension*>& comprehensions, AST_expr* first_generator,
                                std::function<void(std::vector<AST_stmt*>*)> do_yield) {
        for (int i = 0; i < comprehensions.size(); i++) {
            AST_comprehension* c = comprehensions[i];

            AST_For* loop = new (allocator) AST_For();
            loop->target = c->target;
            loop->iter = (i == 0) ? first_generator : c->iter;
            loop->lineno = lineno;

            insert_point->push_back(loop);
            insert_point = &loop->body;

            for (AST_expr* if_condition : c->ifs) {
                AST_If* if_block = new (allocator) AST_If();
                if_block->lineno = if_condition->lineno;
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

    BST_expr* remapGeneratorExp(AST_GeneratorExp* node) {
        assert(node->generators.size());

        // We need to evaluate the first for-expression immediately, as the PEP dictates; so we pass it in as an
        // argument to the function we create. See
        // https://www.python.org/dev/peps/pep-0289/#early-binding-versus-late-binding
        BST_expr* first = remapExpr(node->generators[0]->iter);
        InternedString arg_name = internString("#arg");

        ASTAllocator allocator;
        AST_arguments* genexp_args = new (allocator) AST_arguments();
        genexp_args->args.push_back(makeASTName(allocator, arg_name, AST_TYPE::Param, node->lineno));
        std::vector<AST_stmt*> new_body;
        emitComprehensionLoops(allocator, node->lineno, &new_body, node->generators,
                               makeASTName(allocator, arg_name, AST_TYPE::Load, node->lineno, /* col_offset */ 0),
                               [this, node, &allocator](std::vector<AST_stmt*>* insert_point) {
                                   auto y = new (allocator) AST_Yield();
                                   y->value = node->elt;
                                   y->lineno = node->lineno;
                                   insert_point->push_back(makeASTExpr(allocator, y));
                               });

        // I'm not sure this actually gets used
        static BoxedString* gen_name = getStaticString("<generator>");

        BoxedCode* code = cfgizer->runRecursively(new_body, gen_name, node->lineno, genexp_args, node);
        // XXX bad!  this should be tracked ex through co_consts
        // cur_code->co_consts.push_back(def->code)
        constants.push_back(code);

        BST_FunctionDef* func = new BST_FunctionDef();
        func->args = new BST_arguments(); // hacky, but we don't have to fill this in except for the defaults
        func->code = code;
        BST_MakeFunction* mkfunc = new BST_MakeFunction(func);
        InternedString func_var_name = nodeName();
        pushAssign(func_var_name, mkfunc);

        return makeCall(makeLoad(func_var_name, node, true), first);
    }

    void emitComprehensionYield(ASTAllocator& allocator, AST_DictComp* node, InternedString dict_name,
                                std::vector<AST_stmt*>* insert_point) {
        // add entry to the dictionary
        AST_expr* setitem
            = makeASTLoadAttribute(allocator, makeASTName(allocator, dict_name, AST_TYPE::Load, node->lineno),
                                   internString("__setitem__"), true);
        insert_point->push_back(makeASTExpr(allocator, makeASTCall(allocator, setitem, node->key, node->value)));
    }

    void emitComprehensionYield(ASTAllocator& allocator, AST_SetComp* node, InternedString set_name,
                                std::vector<AST_stmt*>* insert_point) {
        // add entry to the dictionary
        AST_expr* add = makeASTLoadAttribute(allocator, makeASTName(allocator, set_name, AST_TYPE::Load, node->lineno),
                                             internString("add"), true);
        insert_point->push_back(makeASTExpr(allocator, makeASTCall(allocator, add, node->elt)));
    }

    template <typename ResultType, typename CompType> BST_expr* remapScopedComprehension(CompType* node) {
        // See comment in remapGeneratorExp re early vs. late binding.
        BST_expr* first = remapExpr(node->generators[0]->iter);
        InternedString arg_name = internString("#arg");

        ASTAllocator allocator;
        AST_arguments* args = new (allocator) AST_arguments();
        args->args.push_back(makeASTName(allocator, arg_name, AST_TYPE::Param, node->lineno));

        std::vector<AST_stmt*> new_body;

        InternedString rtn_name = internString("#comp_rtn");
        auto asgn = new (allocator) AST_Assign();
        asgn->targets.push_back(makeASTName(allocator, rtn_name, AST_TYPE::Store, node->lineno));
        asgn->value = new (allocator) ResultType();
        asgn->lineno = node->lineno;
        new_body.push_back(asgn);

        auto lambda = [&](std::vector<AST_stmt*>* insert_point) {
            emitComprehensionYield(allocator, node, rtn_name, insert_point);
        };
        AST_Name* first_name
            = makeASTName(allocator, internString("#arg"), AST_TYPE::Load, node->lineno, /* col_offset */ 0);
        emitComprehensionLoops(allocator, node->lineno, &new_body, node->generators, first_name, lambda);

        auto rtn = new (allocator) AST_Return();
        rtn->value = makeASTName(allocator, rtn_name, AST_TYPE::Load, node->lineno, /* col_offset */ 0);
        rtn->lineno = node->lineno;
        new_body.push_back(rtn);

        // I'm not sure this actually gets used
        static BoxedString* comp_name = getStaticString("<comperehension>");

        BoxedCode* code = cfgizer->runRecursively(new_body, comp_name, node->lineno, args, node);
        // XXX bad!  this should be tracked ex through co_consts
        // cur_code->co_consts.push_back(def->code)
        constants.push_back(code);

        BST_FunctionDef* func = new BST_FunctionDef();
        func->args = new BST_arguments(); // hacky, but we don't have to fill this in except for the defaults
        func->code = code;
        BST_MakeFunction* mkfunc = new BST_MakeFunction(func);
        InternedString func_var_name = nodeName();
        pushAssign(func_var_name, mkfunc);

        return makeCall(makeLoad(func_var_name, node, true), first);
    }

    BST_expr* remapIfExp(AST_IfExp* node) {
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

        return makeLoad(rtn_name, node, true);
    }

    BST_slice* remapIndex(AST_Index* node) {
        BST_Index* rtn = new BST_Index();
        rtn->lineno = node->lineno;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    BST_arguments* remapArguments(AST_arguments* args) {
        auto rtn = new BST_arguments();
        for (auto expr : args->defaults)
            rtn->defaults.push_back(remapExpr(expr));
        return rtn;
    }

    BST_expr* remapLambda(AST_Lambda* node) {
        ASTAllocator allocator;
        auto stmt = new (allocator) AST_Return;
        stmt->lineno = node->lineno;

        stmt->value = node->body; // don't remap now; will be CFG'ed later

        auto bdef = new BST_FunctionDef();
        bdef->lineno = node->lineno;

        bdef->args = remapArguments(node->args);

        auto name = getStaticString("<lambda>");
        bdef->code = cfgizer->runRecursively({ stmt }, name, node->lineno, node->args, node);
        // XXX bad!  this should be tracked ex through co_consts
        // cur_code->co_consts.push_back(def->code)
        constants.push_back(bdef->code);

        return new BST_MakeFunction(bdef);
    }

    BST_expr* remapLangPrimitive(AST_LangPrimitive* node) {
        // AST_LangPrimitive can be PRINT_EXPR
        assert(node->opcode == AST_LangPrimitive::PRINT_EXPR);
        BST_LangPrimitive* rtn = new BST_LangPrimitive(BST_LangPrimitive::PRINT_EXPR);
        rtn->lineno = node->lineno;

        for (AST_expr* arg : node->args) {
            rtn->args.push_back(remapExpr(arg));
        }
        return rtn;
    }

    BST_expr* remapList(AST_List* node) {
        assert(node->ctx_type == AST_TYPE::Load);

        BST_List* rtn = new BST_List();
        rtn->lineno = node->lineno;
        rtn->ctx_type = node->ctx_type;

        for (auto elt : node->elts) {
            rtn->elts.push_back(remapExpr(elt));
        }
        return rtn;
    }

    BST_expr* remapRepr(AST_Repr* node) {
        BST_Repr* rtn = new BST_Repr();
        rtn->lineno = node->lineno;
        rtn->value = remapExpr(node->value);
        return rtn;
    }

    BST_expr* remapSet(AST_Set* node) {
        BST_Set* rtn = new BST_Set();
        rtn->lineno = node->lineno;

        for (auto e : node->elts) {
            rtn->elts.push_back(remapExpr(e));
        }

        return rtn;
    }

    BST_slice* remapSlice(AST_Slice* node) {
        BST_Slice* rtn = new BST_Slice();
        rtn->lineno = node->lineno;

        rtn->lower = remapExpr(node->lower);
        rtn->upper = remapExpr(node->upper);
        rtn->step = remapExpr(node->step);

        return rtn;
    }

    BST_expr* remapTuple(AST_Tuple* node) {
        assert(node->ctx_type == AST_TYPE::Load);

        BST_Tuple* rtn = new BST_Tuple();
        rtn->lineno = node->lineno;
        rtn->ctx_type = node->ctx_type;

        for (auto elt : node->elts) {
            rtn->elts.push_back(remapExpr(elt));
        }
        return rtn;
    }

    BST_expr* remapSubscript(AST_Subscript* node) {
        BST_Subscript* rtn = new BST_Subscript();
        rtn->lineno = node->lineno;
        rtn->ctx_type = node->ctx_type;
        rtn->value = remapExpr(node->value);
        rtn->slice = remapSlice(node->slice);
        return rtn;
    }

    BST_expr* remapUnaryOp(AST_UnaryOp* node) {
        BST_UnaryOp* rtn = new BST_UnaryOp();
        rtn->lineno = node->lineno;
        rtn->op_type = node->op_type;
        rtn->operand = remapExpr(node->operand);
        return rtn;
    }

    BST_expr* remapYield(AST_Yield* node) {
        BST_Yield* rtn = new BST_Yield();
        rtn->lineno = node->lineno;
        rtn->value = remapExpr(node->value);

        InternedString node_name(nodeName());
        pushAssign(node_name, rtn);

        push_back(makeExpr(new BST_LangPrimitive(BST_LangPrimitive::UNCACHE_EXC_INFO)));

        if (root_type != AST_TYPE::FunctionDef && root_type != AST_TYPE::Lambda)
            raiseExcHelper(SyntaxError, "'yield' outside function");

        return makeLoad(node_name, node, /* is_kill */ true);
    }

    BST_slice* remapSlice(AST_slice* node) {
        if (node == nullptr)
            return nullptr;

        BST_slice* rtn = nullptr;
        switch (node->type) {
            case AST_TYPE::Ellipsis:
                rtn = remapEllipsis(ast_cast<AST_Ellipsis>(node));
                break;
            case AST_TYPE::ExtSlice:
                rtn = remapExtSlice(ast_cast<AST_ExtSlice>(node));
                break;
            case AST_TYPE::Index:
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

    BST_expr* wrap(BST_expr* node) {
        // this is the part that actually generates temporaries & assigns to them.
        // TODO: could skip num, str, etc.
        if (node->type != BST_TYPE::Name || bst_cast<BST_Name>(node)->id.s()[0] != '#') {
            InternedString name = nodeName();
            pushAssign(name, node);
            return makeLoad(name, node, true);
        }
        return node;
    }

    // Flattens a nested expression into a flat one, emitting instructions &
    // generating temporary variables as needed.
    //
    // If `wrap_with_assign` is true, it will always return a temporary
    // variable.
    BST_expr* remapExpr(AST_expr* node, bool wrap_with_assign = true) {
        if (node == NULL)
            return NULL;

        BST_expr* rtn;
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
                rtn = remapComprehension<BST_List>(ast_cast<AST_ListComp>(node));
                break;
            case AST_TYPE::Name:
                rtn = remapName(ast_cast<AST_Name>(node));
                break;
            case AST_TYPE::Num:
                rtn = remapNum(ast_cast<AST_Num>(node));
                // TODO: might want to start wrapping literals with assigns too
                return rtn;
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
                rtn = remapStr(ast_cast<AST_Str>(node));
                // TODO: might want to start wrapping literals with assigns too
                return rtn;
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

        if (wrap_with_assign)
            return wrap(rtn);
        else
            return rtn;
    }

    // helper for visit_{tryfinally,with}
    CFGBlock* makeFinallyCont(Why reason, BST_expr* whyexpr, CFGBlock* then_block) {
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
                doReturn(makeLoad(internString(RETURN_NAME), node, true));
                break;
            case Why::BREAK:
                doBreak(node);
                break;
            case Why::CONTINUE:
                doContinue(node);
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
    void exitFinallyIf(AST* node, Why why, InternedString whyname, bool is_kill = false) {
        CFGBlock* do_exit = cfg->addDeferredBlock();
        do_exit->info = "with_exit_if";
        CFGBlock* otherwise = makeFinallyCont(why, makeLoad(whyname, node, is_kill), do_exit);

        cfg->placeBlock(do_exit);
        curblock = do_exit;
        exitFinally(node, why);

        curblock = otherwise;
    }

    // ---------- public methods ----------
public:
    void push_back(BST_stmt* node) {
        assert(node->type != BST_TYPE::Invoke);

        if (!curblock)
            return;

        if (exc_handlers.size() == 0) {
            curblock->push_back(node);
            return;
        }

        BST_TYPE::BST_TYPE type = node->type;
        if (type == BST_TYPE::Jump) {
            curblock->push_back(node);
            return;
        }

        if (type == BST_TYPE::Branch) {
            BST_TYPE::BST_TYPE test_type = bst_cast<BST_Branch>(node)->test->type;
            ASSERT(test_type == BST_TYPE::Name || test_type == BST_TYPE::Num, "%d", test_type);
            curblock->push_back(node);
            return;
        }

        if (type == BST_TYPE::Return) {
            curblock->push_back(node);
            return;
        }

        if (type == BST_TYPE::Expr) {
            auto expr = bst_cast<BST_Expr>(node)->value;
            if (expr->type == BST_TYPE::LangPrimitive) {
                auto lp = bst_cast<BST_LangPrimitive>(expr);
                if (lp->opcode == BST_LangPrimitive::UNCACHE_EXC_INFO
                    || lp->opcode == BST_LangPrimitive::SET_EXC_INFO) {
                    curblock->push_back(node);
                    return;
                }
                // TODO: there are others that are also nothrow
            }
        }

        if (node->type == BST_TYPE::Assign) {
            BST_Assign* asgn = bst_cast<BST_Assign>(node);
            if (asgn->target->type == BST_TYPE::Name) {
                BST_Name* target = bst_cast<BST_Name>(asgn->target);
                if (target->id.s()[0] != '#') {
// assigning to a non-temporary
#ifndef NDEBUG
                    if (!(asgn->value->type == BST_TYPE::Name && bst_cast<BST_Name>(asgn->value)->id.s()[0] == '#')
                        && asgn->value->type != BST_TYPE::Str && asgn->value->type != BST_TYPE::Num) {
                        fprintf(stdout, "\nError: doing a non-trivial assignment in an invoke is not allowed:\n");
                        print_bst(node);
                        printf("\n");
                        abort();
                    }
#endif
                    curblock->push_back(node);
                    return;
                } else if (asgn->value->type == BST_TYPE::Name && bst_cast<BST_Name>(asgn->value)->id.s()[0] == '#') {
                    // Assigning from one temporary name to another:
                    curblock->push_back(node);
                    return;
                } else if (asgn->value->type == BST_TYPE::Num || asgn->value->type == BST_TYPE::Str
                           || (asgn->value->type == BST_TYPE::Name
                               && bst_cast<BST_Name>(asgn->value)->id.s() == "None")) {
                    // Assigning to a temporary name from an expression that can't throw:
                    // NB. `None' can't throw in Python, because it's hardcoded
                    // (seriously, try reassigning "None" in CPython).
                    curblock->push_back(node);
                    return;
                }
            }
        }

        // Deleting temporary names is safe, since we only use it to represent kills.
        if (node->type == BST_TYPE::Delete) {
            BST_Delete* del = bst_cast<BST_Delete>(node);
            if (del->target->type == BST_TYPE::Name) {
                BST_Name* target = bst_cast<BST_Name>(del->target);
                if (target->id.s()[0] == '#') {
                    curblock->push_back(node);
                    return;
                }
            }
        }

        // We remapped asserts to just be assertion failures at this point.
        bool is_raise = (node->type == BST_TYPE::Raise || node->type == BST_TYPE::Assert);

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

        BST_Invoke* invoke = new BST_Invoke(node);
        invoke->normal_dest = normal_dest;
        invoke->exc_dest = exc_dest;
        invoke->lineno = node->lineno;

        curblock->push_back(invoke);
        curblock->connectTo(normal_dest);
        if (!is_raise)
            curblock->connectTo(exc_dest);

        ExcBlockInfo& exc_info = exc_handlers.back();
        exc_info.maybe_taken = true;

        curblock = exc_dest;
        // TODO: need to clear some temporaries here
        BST_Assign* exc_asgn = new BST_Assign();
        BST_Tuple* target = new BST_Tuple();
        target->elts.push_back(makeName(exc_info.exc_type_name, AST_TYPE::Store, node->lineno));
        target->elts.push_back(makeName(exc_info.exc_value_name, AST_TYPE::Store, node->lineno));
        target->elts.push_back(makeName(exc_info.exc_traceback_name, AST_TYPE::Store, node->lineno));
        exc_asgn->target = target;

        exc_asgn->value = new BST_LangPrimitive(BST_LangPrimitive::LANDINGPAD);
        curblock->push_back(exc_asgn);

        pushJump(exc_info.exc_dest);

        if (is_raise)
            curblock = NULL;
        else
            curblock = normal_dest;
    }

    bool visit_classdef(AST_ClassDef* node) override {
        auto def = new BST_ClassDef();
        def->lineno = node->lineno;
        def->name = node->name;

        // Decorators are evaluated before bases:
        for (auto expr : node->decorator_list)
            def->decorator_list.push_back(remapExpr(expr));
        for (auto expr : node->bases)
            def->bases.push_back(remapExpr(expr));

        def->code = cfgizer->runRecursively(node->body, node->name.getBox(), node->lineno, NULL, node);
        // XXX bad!  this should be tracked ex through co_consts
        // cur_code->co_consts.push_back(def->code)
        constants.push_back(def->code);

        auto tmp = nodeName();
        pushAssign(tmp, new BST_MakeClass(def));
        pushAssign(scoping->mangleName(def->name), makeName(tmp, AST_TYPE::Load, node->lineno, true));

        return true;
    }

    bool visit_functiondef(AST_FunctionDef* node) override {
        auto def = new BST_FunctionDef();
        def->lineno = node->lineno;
        def->name = node->name;

        // Decorators are evaluated before the defaults, so this *must* go before remapArguments().
        // TODO(rntz): do we have a test for this
        for (auto expr : node->decorator_list)
            def->decorator_list.push_back(remapExpr(expr));
        def->args = remapArguments(node->args);

        def->code = cfgizer->runRecursively(node->body, node->name.getBox(), node->lineno, node->args, node);
        // XXX bad!  this should be tracked ex through co_consts
        // cur_code->co_consts.push_back(def->code)
        constants.push_back(def->code);

        auto tmp = nodeName();
        pushAssign(tmp, new BST_MakeFunction(def));
        pushAssign(scoping->mangleName(def->name), makeName(tmp, AST_TYPE::Load, node->lineno, true));

        return true;
    }

    bool visit_global(AST_Global*) override {
        // nothing todo only the scoping analysis cares about this node
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
            BST_LangPrimitive* import = new BST_LangPrimitive(BST_LangPrimitive::IMPORT_NAME);
            import->lineno = node->lineno;

            import->args.push_back(new BST_Num());
            static_cast<BST_Num*>(import->args[0])->num_type = AST_Num::INT;

            // level == 0 means only check sys path for imports, nothing package-relative,
            // level == -1 means check both sys path and relative for imports.
            // so if `from __future__ import absolute_import` was used in the file, set level to 0
            int level;
            if (!(future_flags & CO_FUTURE_ABSOLUTE_IMPORT))
                level = -1;
            else
                level = 0;
            static_cast<BST_Num*>(import->args[0])->n_int = level;
            import->args.push_back(new BST_LangPrimitive(BST_LangPrimitive::NONE));
            import->args.push_back(new BST_Str(a->name.s()));

            InternedString tmpname = nodeName();
            pushAssign(tmpname, import);

            if (a->asname.s().size() == 0) {
                // No asname, so load the top-level module into the name
                // (e.g., for `import os.path`, loads the os module into `os`)
                pushAssign(internString(getTopModule(a->name.s())), makeLoad(tmpname, node, /* is_kill */ true));
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
                    auto attr = new BST_Attribute(makeLoad(tmpname, node, true), AST_TYPE::Load,
                                                  internString(a->name.s().substr(l, r - l)));
                    attr->lineno = import->lineno;
                    pushAssign(tmpname, attr);
                    l = r + 1;
                } while (l < a->name.s().size());
                pushAssign(a->asname, makeLoad(tmpname, node, true));
            }
        }

        return true;
    }

    bool visit_importfrom(AST_ImportFrom* node) override {
        BST_LangPrimitive* import = new BST_LangPrimitive(BST_LangPrimitive::IMPORT_NAME);
        import->lineno = node->lineno;

        import->args.push_back(new BST_Num());
        static_cast<BST_Num*>(import->args[0])->num_type = AST_Num::INT;

        // level == 0 means only check sys path for imports, nothing package-relative,
        // level == -1 means check both sys path and relative for imports.
        // so if `from __future__ import absolute_import` was used in the file, set level to 0
        int level;
        if (node->level == 0 && !(future_flags & CO_FUTURE_ABSOLUTE_IMPORT))
            level = -1;
        else
            level = node->level;
        static_cast<BST_Num*>(import->args[0])->n_int = level;

        import->args.push_back(new BST_Tuple());
        static_cast<BST_Tuple*>(import->args[1])->ctx_type = AST_TYPE::Load;
        for (int i = 0; i < node->names.size(); i++) {
            static_cast<BST_Tuple*>(import->args[1])->elts.push_back(new BST_Str(node->names[i]->name.s()));
        }
        import->args.push_back(new BST_Str(node->module.s()));

        InternedString tmp_module_name = nodeName();
        pushAssign(tmp_module_name, import);

        int i = 0;
        for (AST_alias* a : node->names) {
            i++;
            bool is_kill = (i == node->names.size());
            if (a->name.s() == "*") {

                BST_LangPrimitive* import_star = new BST_LangPrimitive(BST_LangPrimitive::IMPORT_STAR);
                import_star->lineno = node->lineno;
                import_star->args.push_back(makeLoad(tmp_module_name, node, is_kill));

                BST_Expr* import_star_expr = new BST_Expr();
                import_star_expr->value = import_star;
                import_star_expr->lineno = node->lineno;

                push_back(import_star_expr);
            } else {
                BST_LangPrimitive* import_from = new BST_LangPrimitive(BST_LangPrimitive::IMPORT_FROM);
                import_from->lineno = node->lineno;
                import_from->args.push_back(makeLoad(tmp_module_name, node, is_kill));
                import_from->args.push_back(new BST_Str(a->name.s()));

                InternedString tmp_import_name = nodeName();
                pushAssign(tmp_import_name, import_from);
                pushAssign(a->asname.s().size() ? a->asname : a->name, makeLoad(tmp_import_name, node, true));
            }
        }

        return true;
    }

    bool visit_pass(AST_Pass* node) override { return true; }

    bool visit_assert(AST_Assert* node) override {
        assert(curblock);

        BST_Branch* br = new BST_Branch();
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

        BST_Assert* remapped = new BST_Assert();
        if (node->msg)
            remapped->msg = remapExpr(node->msg);
        else
            remapped->msg = NULL;
        BST_Num* fake_test = new BST_Num();
        fake_test->num_type = AST_Num::INT;
        fake_test->n_int = 0;
        remapped->test = fake_test;
        remapped->lineno = node->lineno;
        push_back(remapped);

        curblock = iftrue;

        return true;
    }

    bool visit_assign(AST_Assign* node) override {
        BST_expr* remapped_value = remapExpr(node->value);

        for (int i = 0; i < node->targets.size(); i++) {
            BST_expr* val;
            if (i == node->targets.size() - 1)
                val = remapped_value;
            else
                val = _dup(remapped_value);
            pushAssign(node->targets[i], val);
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

        BST_expr* remapped_target;
        BST_expr* remapped_lhs;

        // TODO bad that it's reusing the AST nodes?
        switch (node->target->type) {
            case AST_TYPE::Name: {
                AST_Name* n = ast_cast<AST_Name>(node->target);
                assert(n->ctx_type == AST_TYPE::Store);
                InternedString n_name(nodeName());
                pushAssign(n_name, makeLoad(n->id, node));
                remapped_target = remapName(n);
                remapped_lhs = makeLoad(n_name, node, /* is_kill */ true);
                break;
            }
            case AST_TYPE::Subscript: {
                AST_Subscript* s = ast_cast<AST_Subscript>(node->target);
                assert(s->ctx_type == AST_TYPE::Store);

                BST_Subscript* s_target = new BST_Subscript();
                s_target->value = remapExpr(s->value);
                s_target->slice = remapSlice(s->slice);
                s_target->ctx_type = AST_TYPE::Store;
                s_target->lineno = s->lineno;
                remapped_target = s_target;

                BST_Subscript* s_lhs = new BST_Subscript();
                s_lhs->value = _dup(s_target->value);
                s_lhs->slice = _dup(s_target->slice);
                s_lhs->lineno = s->lineno;
                s_lhs->ctx_type = AST_TYPE::Load;
                remapped_lhs = wrap(s_lhs);

                break;
            }
            case AST_TYPE::Attribute: {
                AST_Attribute* a = ast_cast<AST_Attribute>(node->target);
                assert(a->ctx_type == AST_TYPE::Store);

                BST_Attribute* a_target = new BST_Attribute();
                a_target->value = remapExpr(a->value);
                a_target->attr = scoping->mangleName(a->attr);
                a_target->ctx_type = AST_TYPE::Store;
                a_target->lineno = a->lineno;
                remapped_target = a_target;

                BST_Attribute* a_lhs = new BST_Attribute();
                a_lhs->value = _dup(a_target->value);
                a_lhs->attr = a_target->attr;
                a_lhs->ctx_type = AST_TYPE::Load;
                a_lhs->lineno = a->lineno;
                remapped_lhs = wrap(a_lhs);

                break;
            }
            default:
                RELEASE_ASSERT(0, "%d", node->target->type);
        }

        BST_AugBinOp* binop = new BST_AugBinOp();
        binop->op_type = remapBinOpType(node->op_type);
        binop->left = remapped_lhs;
        binop->right = remapExpr(node->value);
        binop->lineno = node->lineno;

        InternedString node_name(nodeName());
        pushAssign(node_name, binop);
        pushAssign(remapped_target, makeLoad(node_name, node, true));
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
            BST_expr* target = NULL;
            switch (t->type) {
                case AST_TYPE::Subscript: {
                    AST_Subscript* s = static_cast<AST_Subscript*>(t);
                    BST_Subscript* astsubs = new BST_Subscript();
                    astsubs->value = remapExpr(s->value);
                    astsubs->slice = remapSlice(s->slice);
                    astsubs->ctx_type = AST_TYPE::Del;
                    target = astsubs;
                    break;
                }
                case AST_TYPE::Attribute: {
                    BST_Attribute* astattr = static_cast<BST_Attribute*>(remapExpr(t, false));
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
                    ASTAllocator allocator;
                    AST_Delete* temp_ast_del = new (allocator) AST_Delete();
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
                    ASTAllocator allocator;
                    AST_Delete* temp_ast_del = new (allocator) AST_Delete();
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

            if (target != NULL) {
                BST_Delete* astdel = new BST_Delete();
                astdel->lineno = node->lineno;
                astdel->target = target;
                push_back(astdel);
            }
        }

        return true;
    }

    bool visit_expr(AST_Expr* node) override {
        BST_Expr* remapped = new BST_Expr();
        remapped->lineno = node->lineno;
        remapped->value = remapExpr(node->value, false);
        push_back(remapped);
        return true;
    }

    bool visit_print(AST_Print* node) override {
        BST_expr* dest = remapExpr(node->dest);

        int i = 0;
        for (auto v : node->values) {
            BST_Print* remapped = new BST_Print();
            remapped->lineno = node->lineno;

            if (i < node->values.size() - 1) {
                remapped->dest = _dup(dest);
                remapped->nl = false;
            } else {
                remapped->dest = dest;
                remapped->nl = node->nl;
            }

            remapped->value = remapExpr(v);
            push_back(remapped);

            i++;
        }

        if (node->values.size() == 0) {
            assert(node->nl);

            BST_Print* final = new BST_Print();
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

        BST_Branch* br = new BST_Branch();
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

        doBreak(node);
        assert(!curblock);
        return true;
    }

    bool visit_continue(AST_Continue* node) override {
        assert(curblock);

        doContinue(node);
        assert(!curblock);
        return true;
    }

    bool visit_exec(AST_Exec* node) override {
        BST_Exec* astexec = new BST_Exec();
        astexec->lineno = node->lineno;
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
        BST_Branch* br = makeBranch(remapExpr(node->test));
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

    BST_stmt* makeKill(InternedString name) {
        // There might be a better way to represent this, maybe with a dedicated AST_Kill bytecode?
        auto del = new BST_Delete();
        del->target = makeName(name, AST_TYPE::Del, 0, false);
        return del;
    }

    bool visit_for(AST_For* node) override {
        assert(curblock);

        // TODO this is so complicated because I tried doing loop inversion;
        // is it really worth it?  It got so bad because all the edges became
        // critical edges and needed to be broken, otherwise it's not too different.

        BST_expr* remapped_iter = remapExpr(node->iter);
        BST_LangPrimitive* iter_call = new BST_LangPrimitive(BST_LangPrimitive::GET_ITER);
        iter_call->args.push_back(remapped_iter);
        iter_call->lineno = node->lineno;

        InternedString itername = createUniqueName("#iter_");
        pushAssign(itername, iter_call);

        BST_expr* next_attr = makeLoadAttribute(makeLoad(itername, node), internString("next"), true);

        CFGBlock* test_block = cfg->addBlock();
        pushJump(test_block);
        curblock = test_block;

        BST_LangPrimitive* test_call = new BST_LangPrimitive(BST_LangPrimitive::HASNEXT);
        test_call->lineno = node->lineno;
        test_call->args.push_back(makeName(itername, AST_TYPE::Load, node->lineno));
        BST_Branch* test_br = makeBranch(test_call);

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

        // TODO: need to del the iter_name when break'ing out of the loop
        pushLoopContinuation(test_block, end_block);

        curblock = loop_block;
        InternedString next_name(nodeName());
        pushAssign(next_name, makeCall(next_attr));
        pushAssign(node->target, makeLoad(next_name, node, true));

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }
        popContinuation();

        if (curblock) {
            BST_LangPrimitive* end_call = new BST_LangPrimitive(BST_LangPrimitive::HASNEXT);
            end_call->args.push_back(makeName(itername, AST_TYPE::Load, node->lineno));
            end_call->lineno = node->lineno;
            BST_Branch* end_br = makeBranch(end_call);
            push_back(end_br);

            CFGBlock* end_true = cfg->addBlock();
            CFGBlock* end_false = cfg->addBlock();
            end_br->iftrue = end_true;
            end_br->iffalse = end_false;
            curblock->connectTo(end_true);
            curblock->connectTo(end_false);

            curblock = end_true;
            pushJump(loop_block, true, getLastLinenoSub(node->body.back()));

            curblock = end_false;
            pushJump(else_block);
        }

        cfg->placeBlock(else_block);
        curblock = else_block;

        push_back(makeKill(itername));
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

        BST_Raise* remapped = new BST_Raise();
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
        assert(node->handlers.size() > 0);

        CFGBlock* exc_handler_block = cfg->addDeferredBlock();
        InternedString exc_type_name = nodeName("type");
        InternedString exc_value_name = nodeName("value");
        InternedString exc_traceback_name = nodeName("traceback");
        exc_handlers.push_back({ exc_handler_block, exc_type_name, exc_value_name, exc_traceback_name, false });

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

            bool caught_all = false;
            for (AST_ExceptHandler* exc_handler : node->handlers) {
                assert(!caught_all && "bare except clause not the last one in the list?");

                CFGBlock* exc_next = nullptr;
                if (exc_handler->type) {
                    BST_expr* handled_type = remapExpr(exc_handler->type);

                    BST_LangPrimitive* is_caught_here = new BST_LangPrimitive(BST_LangPrimitive::CHECK_EXC_MATCH);
                    // TODO This is supposed to be exc_type_name (value doesn't matter for checking matches)
                    is_caught_here->args.push_back(makeLoad(exc_value_name, exc_handler));
                    is_caught_here->args.push_back(handled_type);
                    is_caught_here->lineno = exc_handler->lineno;

                    BST_Branch* br = new BST_Branch();
                    br->test = callNonzero(is_caught_here);
                    br->lineno = exc_handler->lineno;

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
                    pushAssign(exc_handler->name, makeLoad(exc_value_name, exc_handler));
                }

                BST_LangPrimitive* set_exc_info = new BST_LangPrimitive(BST_LangPrimitive::SET_EXC_INFO);
                set_exc_info->args.push_back(makeLoad(exc_type_name, node, true));
                set_exc_info->args.push_back(makeLoad(exc_value_name, node, true));
                set_exc_info->args.push_back(makeLoad(exc_traceback_name, node, true));
                push_back(makeExpr(set_exc_info));

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
                BST_Raise* raise = new BST_Raise();
                raise->arg0 = makeLoad(exc_type_name, node, true);
                raise->arg1 = makeLoad(exc_value_name, node, true);
                raise->arg2 = makeLoad(exc_traceback_name, node, true);

                // This is weird but I think it is right.
                // Even though the line number of the trackback will correctly point to the line that
                // raised, this matches CPython's behavior that the frame's line number points to
                // the last statement of the last except block.
                raise->lineno = getLastLinenoSub(node->handlers.back()->body.back());

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
        exc_handlers.push_back({ exc_handler_block, exc_type_name, exc_value_name, exc_traceback_name, false });

        CFGBlock* finally_block = cfg->addDeferredBlock();
        pushFinallyContinuation(finally_block, exc_why_name);

        for (AST_stmt* subnode : node->body) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        bool maybe_exception = exc_handlers.back().maybe_taken;
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
            if (maybe_exception) {
                CFGBlock* reraise = cfg->addDeferredBlock();
                CFGBlock* noexc = makeFinallyCont(Why::EXCEPTION, makeLoad(exc_why_name, node, true), reraise);

                cfg->placeBlock(reraise);
                curblock = reraise;
                pushReraise(getLastLinenoSub(node->finalbody.back()), exc_type_name, exc_value_name,
                            exc_traceback_name);

                curblock = noexc;
            }
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
        BST_expr* exit = makeLoadAttribute(makeLoad(ctxmgrname, node), internString("__exit__"), true);
        pushAssign(exitname, exit);

        // Oddly, this acces to __enter__ doesn't suffer from the same bug. Perhaps it has something to do with
        // __enter__ being called immediately?
        BST_expr* enter = makeLoadAttribute(makeLoad(ctxmgrname, node, true), internString("__enter__"), true);
        enter = wrap(makeCall(enter));
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
        exc_handlers.push_back({ exc_block, exc_type_name, exc_value_name, exc_traceback_name, false });

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
            pushAssign(suppressname, makeCall(makeLoad(exitname, node, true), makeLoad(exc_type_name, node),
                                              makeLoad(exc_value_name, node), makeLoad(exc_traceback_name, node)));

            // if it returns true, suppress the error and go to our exit block
            CFGBlock* reraise_block = cfg->addDeferredBlock();
            reraise_block->info = "with_reraise";
            // break potential critical edge
            CFGBlock* exiter = cfg->addDeferredBlock();
            exiter->info = "with_exiter";
            pushBranch(makeLoad(suppressname, node, true), exiter, reraise_block);

            cfg->placeBlock(exiter);
            curblock = exiter;
            pushJump(exit_block);

            // otherwise, reraise the exception
            cfg->placeBlock(reraise_block);
            curblock = reraise_block;
            pushReraise(getLastLinenoSub(node->body.back()), exc_type_name, exc_value_name, exc_traceback_name);
        }

        // The finally block
        if (finally_block->predecessors.size() == 0) {
            // TODO(rntz): test for this case, "with foo: raise bar"
            delete finally_block;
        } else {
            cfg->placeBlock(finally_block);
            curblock = finally_block;
            // call the context-manager's exit method, ignoring result
            push_back(makeExpr(makeCall(makeLoad(exitname, node, true), makeLoad(nonename, node),
                                        makeLoad(nonename, node), makeLoad(nonename, node))));

            if (finally_did_why & (1 << Why::CONTINUE))
                exitFinallyIf(node, Why::CONTINUE, whyname, /* is_kill */ finally_did_why == (1 << Why::CONTINUE));
            if (finally_did_why & (1 << Why::BREAK))
                exitFinallyIf(node, Why::BREAK, whyname, /* is_kill */ !(finally_did_why & (1 << Why::RETURN)));
            if (finally_did_why & (1 << Why::RETURN))
                exitFinallyIf(node, Why::RETURN, whyname, /* is_kill */ true);
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

class AssignVRegsVisitor : public NoopBSTVisitor {
public:
    CFGBlock* current_block;
    int next_vreg;
    llvm::DenseMap<InternedString, DefaultedInt<-1>> sym_vreg_map;
    llvm::DenseMap<InternedString, std::unordered_set<CFGBlock*>> sym_blocks_map;
    std::vector<InternedString> vreg_sym_map;

    enum Step { TrackBlockUsage = 0, UserVisible, CrossBlock, SingleBlockUse } step;

    AssignVRegsVisitor() : current_block(0), next_vreg(0) {}

    bool visit_arguments(BST_arguments* node) override {
        for (BST_expr* d : node->defaults)
            d->accept(this);
        return true;
    }

    bool visit_classdef(BST_ClassDef* node) override {
        for (auto e : node->bases)
            e->accept(this);
        for (auto e : node->decorator_list)
            e->accept(this);
        return true;
    }

    bool visit_functiondef(BST_FunctionDef* node) override {
        for (auto* d : node->decorator_list)
            d->accept(this);
        node->args->accept(this);
        return true;
    }

    bool isNameUsedInSingleBlock(InternedString id) {
        assert(step != TrackBlockUsage);
        assert(sym_blocks_map.count(id));
        return sym_blocks_map[id].size() == 1;
    }

    bool visit_name(BST_Name* node) override {
        if (node->vreg != -1)
            return true;

        ASSERT(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN, "%s", node->id.c_str());

        if (node->lookup_type != ScopeInfo::VarScopeType::FAST && node->lookup_type != ScopeInfo::VarScopeType::CLOSURE)
            return true;

        if (step == TrackBlockUsage) {
            sym_blocks_map[node->id].insert(current_block);
            return true;
        } else if (step == UserVisible) {
            if (node->id.isCompilerCreatedName())
                return true;
        } else {
            bool is_block_local = node->lookup_type == ScopeInfo::VarScopeType::FAST
                                  && isNameUsedInSingleBlock(node->id);
            if (step == CrossBlock && is_block_local)
                return true;
            if (step == SingleBlockUse && !is_block_local)
                return true;
        }
        node->vreg = assignVReg(node->id);
        return true;
    }

    int assignVReg(InternedString id) {
        assert(id.s().size());

        auto it = sym_vreg_map.find(id);
        if (sym_vreg_map.end() == it) {
            sym_vreg_map[id] = next_vreg;

            if (!REUSE_VREGS || step == UserVisible || step == CrossBlock) {
                assert(next_vreg == vreg_sym_map.size());
                vreg_sym_map.push_back(id);
            }
            return next_vreg++;
        }
        return it->second;
    }
};

void VRegInfo::assignVRegs(CFG* cfg, const ParamNames& param_names) {
    assert(!hasVRegsAssigned());

    // warning: don't rearrange the steps, they need to be run in this exact order!
    AssignVRegsVisitor visitor;
    for (auto step : { AssignVRegsVisitor::TrackBlockUsage, AssignVRegsVisitor::UserVisible,
                       AssignVRegsVisitor::CrossBlock, AssignVRegsVisitor::SingleBlockUse }) {
        visitor.step = step;
        for (CFGBlock* b : cfg->blocks) {
            visitor.current_block = b;

#if REUSE_VREGS
            if (step == AssignVRegsVisitor::SingleBlockUse)
                visitor.next_vreg = num_vregs_cross_block;
#endif

            if (b == cfg->getStartingBlock()) {
                for (auto* name : param_names.allArgsAsName()) {
                    name->accept(&visitor);
                }
            }

            for (BST_stmt* stmt : b->body) {
                stmt->accept(&visitor);
            }

            if (step == AssignVRegsVisitor::SingleBlockUse)
                num_vregs = std::max(num_vregs, visitor.next_vreg);
        }

        if (step == AssignVRegsVisitor::UserVisible) {
            num_vregs_user_visible = visitor.sym_vreg_map.size();
#ifndef NDEBUG
            sym_vreg_map_user_visible = visitor.sym_vreg_map;
#endif
        } else if (step == AssignVRegsVisitor::CrossBlock)
            num_vregs = num_vregs_cross_block = visitor.next_vreg;
    }
#ifndef NDEBUG
    sym_vreg_map = std::move(visitor.sym_vreg_map);
#endif
    vreg_sym_map = std::move(visitor.vreg_sym_map);
    assert(hasVRegsAssigned());
#if REUSE_VREGS
    assert(vreg_sym_map.size() == num_vregs_cross_block);
#else
    assert(vreg_sym_map.size() == num_vregs);
#endif
}


static CFG* computeCFG(llvm::ArrayRef<AST_stmt*> body, AST_TYPE::AST_TYPE ast_type, int lineno, AST_arguments* args,
                       BoxedString* filename, SourceInfo* source, const ParamNames& param_names, ScopeInfo* scoping,
                       ModuleCFGProcessor* cfgizer) {
    STAT_TIMER(t0, "us_timer_computecfg", 0);

    CFG* rtn = new CFG();

    assert((bool)args == (ast_type == AST_TYPE::FunctionDef || ast_type == AST_TYPE::Lambda));

    auto&& stringpool = cfgizer->stringpool;
    CFGVisitor visitor(filename, source, stringpool, scoping, ast_type, source->future_flags, rtn, cfgizer);

    bool skip_first = false;

    if (ast_type == AST_TYPE::ClassDef) {
        // A classdef always starts with "__module__ = __name__"
        BST_Assign* module_assign = new BST_Assign();
        auto module_name_target = new BST_Name(stringpool.get("__module__"), AST_TYPE::Store, lineno);
        fillScopingInfo(module_name_target, scoping);

        auto module_name_value = new BST_Name(stringpool.get("__name__"), AST_TYPE::Load, lineno);
        fillScopingInfo(module_name_value, scoping);

        module_assign->target = module_name_target;
        module_assign->value = module_name_value;

        module_assign->lineno = lineno;
        visitor.push_back(module_assign);

        // If the first statement is just a single string, transform it to an assignment to __doc__
        if (body.size() && body[0]->type == AST_TYPE::Expr) {
            AST_Expr* first_expr = ast_cast<AST_Expr>(body[0]);
            if (first_expr->value->type == AST_TYPE::Str) {
                BST_Assign* doc_assign = new BST_Assign();
                auto doc_target_name = new BST_Name(stringpool.get("__doc__"), AST_TYPE::Store, lineno);
                fillScopingInfo(doc_target_name, scoping);
                doc_assign->target = doc_target_name;
                auto doc_val = new BST_Str();
                doc_val->str_data = ast_cast<AST_Str>(first_expr->value)->str_data;
                doc_val->str_type = ast_cast<AST_Str>(first_expr->value)->str_type;
                doc_assign->value = doc_val;
                doc_assign->lineno = lineno;
                visitor.push_back(doc_assign);
                skip_first = true;
            }
        }
    }

    if (ast_type == AST_TYPE::FunctionDef || ast_type == AST_TYPE::Lambda) {
        // Unpack tuple arguments
        // Tuple arguments get assigned names ".0", ".1" etc. So this
        // def f(a, (b,c), (d,e)):
        // would expand to:
        // def f(a, .1, .2):
        //     (b, c) = .1
        //     (d, e) = .2
        int counter = 0;
        for (AST_expr* arg_expr : args->args) {
            if (arg_expr->type == AST_TYPE::Tuple) {
                InternedString arg_name = stringpool.get("." + std::to_string(counter));
                BST_Name* arg_name_expr = new BST_Name(arg_name, AST_TYPE::Load, arg_expr->lineno);
                assert(scoping->getScopeTypeOfName(arg_name) == ScopeInfo::VarScopeType::FAST);
                arg_name_expr->lookup_type = ScopeInfo::VarScopeType::FAST;
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
        ASSERT(body[i]->lineno > 0, "%d", body[i]->type);
        body[i]->accept(&visitor);
    }

    // The functions we create for classdefs are supposed to return a dictionary of their locals.
    // This is the place that we add all of that:
    if (ast_type == AST_TYPE::ClassDef) {
        BST_LangPrimitive* locals = new BST_LangPrimitive(BST_LangPrimitive::LOCALS);

        BST_Return* rtn = new BST_Return();
        rtn->lineno = getLastLineno(body, lineno);
        rtn->value = locals;
        visitor.push_back(rtn);
    } else {
        // Put a fake "return" statement at the end of every function just to make sure they all have one;
        // we already have to support multiple return statements in a function, but this way we can avoid
        // having to support not having a return statement:
        BST_Return* return_stmt = new BST_Return();
        return_stmt->lineno = getLastLineno(body, lineno);
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
            BST_stmt* terminator = b->body.back();
            assert(terminator->type == BST_TYPE::Return || terminator->type == BST_TYPE::Raise
                   || terminator->type == BST_TYPE::Raise || terminator->type == BST_TYPE::Assert);
        }

        if (b->predecessors.size() == 0) {
            if (b != rtn->getStartingBlock()) {
                rtn->print();
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

    std::vector<BST*> flattened;
    for (auto b : rtn->blocks)
        flatten(b->body, flattened, true);

    std::unordered_map<BST*, int> deduped;
    bool no_dups = true;
    for (auto e : flattened) {
        deduped[e]++;
        if (deduped[e] == 2) {
            printf("Duplicated: ");
            print_bst(e);
            printf("\n");
            no_dups = false;
        }
    }
    if (!no_dups)
        rtn->print();
    assert(no_dups);

// Uncomment this for some heavy checking to make sure that we don't forget
// to set lineno.  It will catch a lot of things that don't necessarily
// need to be fixed.
#if 0
    for (auto b : rtn->blocks) {
        for (auto ast : b->body) {
            if (ast->type == AST_TYPE::Jump)
                continue;
            if (ast->type == AST_TYPE::Assign) {
                auto asgn = ast_cast<AST_Assign>(ast);
                if (asgn->value->type == AST_TYPE::LangPrimitive) {
                    auto lp = ast_cast<AST_LangPrimitive>(asgn->value);
                    if (lp->opcode == AST_LangPrimitive::LANDINGPAD)
                        continue;
                }
            }
            if (ast->type == AST_TYPE::Expr) {
                auto expr = ast_cast<AST_Expr>(ast);
                if (expr->value->type == AST_TYPE::LangPrimitive) {
                    auto lp = ast_cast<AST_LangPrimitive>(expr->value);
                    if (lp->opcode == AST_LangPrimitive::UNCACHE_EXC_INFO || lp->opcode == AST_LangPrimitive::SET_EXC_INFO)
                        continue;
                }
            }

            if (ast->type == AST_TYPE::Delete) {
                AST_Delete* del = ast_cast<AST_Delete>(ast);
                assert(del->targets.size() == 1);
                if (del->targets[0]->type == AST_TYPE::Name) {
                    AST_Name* target = ast_cast<AST_Name>(del->targets[0]);
                    if (target->id.s()[0] == '#') {
                        continue;
                    }
                }
            }


            //if (ast->type != AST_TYPE::Return)
                //continue;
            if (ast->lineno == 0) {
                rtn->print();
                printf("\n");
                print_ast(ast);
                printf("\n");
            }
            assert(ast->lineno > 0);
        }
    }
#endif

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

            BST_TYPE::BST_TYPE end_ast_type = b->body[b->body.size() - 1]->type;
            assert(end_ast_type == BST_TYPE::Jump || end_ast_type == BST_TYPE::Invoke);
            if (end_ast_type == BST_TYPE::Invoke) {
                // TODO probably shouldn't be generating these anyway:
                auto invoke = bst_cast<BST_Invoke>(b->body.back());
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

    rtn->getVRegInfo().assignVRegs(rtn, param_names);

    return rtn;
}


BoxedCode* ModuleCFGProcessor::runRecursively(llvm::ArrayRef<AST_stmt*> body, BoxedString* name, int lineno,
                                              AST_arguments* args, AST* orig_node) {
    ScopeInfo* scope_info = scoping.getScopeInfoForNode(orig_node);

    AST_TYPE::AST_TYPE ast_type = orig_node->type;
    bool is_generator;
    switch (ast_type) {
        case AST_TYPE::ClassDef:
        case AST_TYPE::Module:
        case AST_TYPE::Expression:
        case AST_TYPE::Suite:
            is_generator = false;
            break;
        case AST_TYPE::GeneratorExp:
        case AST_TYPE::DictComp:
        case AST_TYPE::SetComp:
            is_generator = ast_type == AST_TYPE::GeneratorExp;
            assert(containsYield(body) == is_generator);

            // Hack: our old system represented this as ast_type == FuntionDef, so
            // keep doing that for now
            ast_type = AST_TYPE::FunctionDef;

            break;
        case AST_TYPE::FunctionDef:
        case AST_TYPE::Lambda:
            is_generator = containsYield(orig_node);
            break;
        default:
            RELEASE_ASSERT(0, "Unknown type: %d", ast_type);
            break;
    }

    std::unique_ptr<SourceInfo> si(new SourceInfo(bm, ScopingResults(scope_info, scoping.areGlobalsFromModule()),
                                                  future_flags, ast_type, is_generator));

    assert((bool)args == (ast_type == AST_TYPE::FunctionDef || ast_type == AST_TYPE::Lambda));

    ParamNames param_names(args, stringpool);

    for (auto e : param_names.allArgsAsName())
        fillScopingInfo(e, scope_info);

    si->cfg = computeCFG(body, ast_type, lineno, args, fn, si.get(), param_names, scope_info, this);

    BoxedCode* code;
    if (args)
        code = new BoxedCode(args->args.size(), args->vararg, args->kwarg, lineno, std::move(si),
                             std::move(param_names), fn, name, autoDecref(getDocString(body)));
    else
        code = new BoxedCode(0, false, false, lineno, std::move(si), std::move(param_names), fn, name,
                             autoDecref(getDocString(body)));

    return code;
}

BoxedCode* computeAllCFGs(AST* ast, bool globals_from_module, FutureFlags future_flags, BoxedString* fn,
                          BoxedModule* bm) {
    return ModuleCFGProcessor(ast, globals_from_module, future_flags, fn, bm)
        .runRecursively(ast->getBody(), ast->getName(), ast->lineno, nullptr, ast);
}

void printCFG(CFG* cfg) {
    cfg->print();
}
}
