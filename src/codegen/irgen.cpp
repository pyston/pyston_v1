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
#include <stdint.h>
#include <iostream>
#include <sstream>

#include "llvm/DIBuilder.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Scalar.h"

#include "core/options.h"
#include "core/stats.h"

#include "core/ast.h"
#include "core/cfg.h"
#include "core/util.h"

#include "analysis/scoping_analysis.h"
#include "analysis/function_analysis.h"
#include "analysis/type_analysis.h"

#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/patchpoints.h"
#include "codegen/osrentry.h"
#include "codegen/stackmaps.h"
#include "codegen/irgen/util.h"
#include "codegen/opt/escape_analysis.h"
#include "codegen/opt/inliner.h"
#include "codegen/opt/passes.h"

#include "runtime/types.h"
#include "runtime/objmodel.h"

namespace pyston {

typedef std::unordered_set<CFGBlock*> BlockSet;

// This is where you can add a hook for any instruction added through the IRBuilder.
// It's currently not doing any hooking; hopefully there's not too much overhead from this.
void MyInserter::InsertHelper(llvm::Instruction *I, const llvm::Twine &Name,
        llvm::BasicBlock *BB, llvm::BasicBlock::iterator InsertPt) const {
    llvm::IRBuilderDefaultInserter<true>::InsertHelper(I, Name, BB, InsertPt);
}

// Class that holds state of the current IR generation, that might not be local
// to the specific phase or pass we're in.
class IRGenState {
    private:
        CompiledFunction *cf;
        SourceInfo* source_info;
        GCBuilder *gc;
        llvm::MDNode* func_dbg_info;

        llvm::AllocaInst *scratch_space;
        int scratch_size;

    public:
        IRGenState(CompiledFunction *cf, SourceInfo* source_info, GCBuilder *gc, llvm::MDNode* func_dbg_info) : cf(cf), source_info(source_info), gc(gc), func_dbg_info(func_dbg_info), scratch_space(NULL), scratch_size(0) {
            assert(cf->func);
            assert(!cf->clfunc); // in this case don't need to pass in sourceinfo
        }

        CompiledFunction* getCurFunction() {
            return cf;
        }

        llvm::Function* getLLVMFunction() {
            return cf->func;
        }

        EffortLevel::EffortLevel getEffortLevel() {
            return cf->effort;
        }

        GCBuilder* getGC() {
            return gc;
        }

        llvm::Value* getScratchSpace(int min_bytes) {
            llvm::BasicBlock &entry_block = getLLVMFunction()->getEntryBlock();

            if (scratch_space) {
                assert(scratch_space->getParent() == &entry_block);
                assert(scratch_space->isStaticAlloca());
                if (scratch_size >= min_bytes)
                    return scratch_space;
            }

            // Not sure why, but LLVM wants to canonicalize an alloca into an array alloca (assuming
            // the alloca is static); just to keep things straightforward, let's do that here:
            llvm::Type* array_type = llvm::ArrayType::get(g.i8, min_bytes);

            llvm::AllocaInst* new_scratch_space;
            // If the entry block is currently empty, we have to be more careful:
            if (entry_block.begin() == entry_block.end()) {
                new_scratch_space = new llvm::AllocaInst(array_type, getConstantInt(1, g.i64), "scratch", &entry_block);
            } else {
                new_scratch_space = new llvm::AllocaInst(array_type, getConstantInt(1, g.i64), "scratch", entry_block.getFirstInsertionPt());
            }
            assert(new_scratch_space->isStaticAlloca());

            if (scratch_space)
                scratch_space->replaceAllUsesWith(new_scratch_space);

            scratch_size = min_bytes;
            scratch_space = new_scratch_space;

            return scratch_space;
        }

        ConcreteCompilerType* getReturnType() {
            assert(cf->sig);
            return cf->sig->rtn_type;
        }

        SourceInfo* getSourceInfo() {
            return source_info;
        }

        ScopeInfo* getScopeInfo() {
            SourceInfo *source = getSourceInfo();
            return source->scoping->getScopeInfoForNode(source->ast);
        }

        llvm::MDNode* getFuncDbgInfo() {
            return func_dbg_info;
        }
};

class IREmitterImpl : public IREmitter {
    private:
        IRGenState *irstate;
        IRBuilder *builder;

    public:
        explicit IREmitterImpl(IRGenState *irstate) : irstate(irstate), builder(new IRBuilder(g.context)) {
            builder->setEmitter(this);
        }

        Target getTarget() override {
            if (irstate->getEffortLevel() == EffortLevel::INTERPRETED)
                return INTERPRETER;
            return COMPILATION;
        }

        IRBuilder* getBuilder() override {
            return builder;
        }

        GCBuilder* getGC() override {
            return irstate->getGC();
        }

        llvm::Function* getIntrinsic(llvm::Intrinsic::ID intrinsic_id) override {
            return llvm::Intrinsic::getDeclaration(g.cur_module, intrinsic_id);
        }

        CompiledFunction* currentFunction() override {
            return irstate->getCurFunction();
        }

        llvm::Value* createPatchpoint(const PatchpointSetupInfo* pp, void* func_addr, const std::vector<llvm::Value*> &args) override {
            std::vector<llvm::Value*> pp_args;
            pp_args.push_back(getConstantInt(pp->getPatchpointId(), g.i64));
            pp_args.push_back(getConstantInt(pp->totalSize(), g.i32));
            pp_args.push_back(embedConstantPtr(func_addr, g.i8->getPointerTo()));
            pp_args.push_back(getConstantInt(args.size(), g.i32));

            pp_args.insert(pp_args.end(), args.begin(), args.end());

            int num_scratch_bytes = pp->numScratchBytes();
            if (num_scratch_bytes) {
                llvm::Value* scratch_space = irstate->getScratchSpace(num_scratch_bytes);
                pp_args.push_back(scratch_space);
            }

            llvm::Intrinsic::ID intrinsic_id = pp->hasReturnValue() ? llvm::Intrinsic::experimental_patchpoint_i64 : llvm::Intrinsic::experimental_patchpoint_void;
            llvm::Function *patchpoint = this->getIntrinsic(intrinsic_id);
            llvm::CallInst* rtn = this->getBuilder()->CreateCall(patchpoint, pp_args);

            /*
            static int n = 0;
            n++;
            if (n >= 3)
                rtn->setCallingConv(llvm::CallingConv::PreserveAll);
            */
            rtn->setCallingConv(pp->getCallingConvention());

            // Not sure why this doesn't work:
            //rtn->setCallingConv(llvm::CallingConv::AnyReg);

            return rtn;
        }
};

static void addIRDebugSymbols(llvm::Function *f) {
    llvm::legacy::PassManager mpm;

    llvm::error_code code = llvm::sys::fs::create_directory(".debug_ir", true);
    assert(!code);

    mpm.add(llvm::createDebugIRPass(false, false, ".debug_ir", f->getName()));

    mpm.run(*g.cur_module);
}

static void optimizeIR(llvm::Function *f, EffortLevel::EffortLevel effort) {
    // TODO maybe should do some simple passes (ex: gvn?) if effort level isn't maximal?
    // In general, this function needs a lot of tuning.
    if (effort < EffortLevel::MAXIMAL)
        return;

    Timer _t("optimizing");

    llvm::FunctionPassManager fpm(g.cur_module);

    fpm.add(new llvm::DataLayout(*g.tm->getDataLayout()));

    if (ENABLE_INLINING && effort >= EffortLevel::MAXIMAL) fpm.add(makeFPInliner(275));
    fpm.add(llvm::createCFGSimplificationPass());

    fpm.add(llvm::createBasicAliasAnalysisPass());
    fpm.add(llvm::createTypeBasedAliasAnalysisPass());
    if (ENABLE_PYSTON_PASSES) {
        fpm.add(new EscapeAnalysis());
        fpm.add(createPystonAAPass());
    }

    if (ENABLE_PYSTON_PASSES) fpm.add(createMallocsNonNullPass());

    // TODO Find the right place for this pass (and ideally not duplicate it)
    if (ENABLE_PYSTON_PASSES) {
        fpm.add(llvm::createGVNPass());
        fpm.add(createConstClassesPass());
    }

    // TODO: find the right set of passes
    if (0) {
        // My original set of passes, that seem to get about 90% of the benefit:
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createReassociatePass());
        fpm.add(llvm::createGVNPass());
        fpm.add(llvm::createCFGSimplificationPass());
    } else {
        // copied + slightly modified from llvm/lib/Transforms/IPO/PassManagerBuilder.cpp::populateModulePassManager
        fpm.add(llvm::createEarlyCSEPass());              // Catch trivial redundancies
        fpm.add(llvm::createJumpThreadingPass());         // Thread jumps.
        fpm.add(llvm::createCorrelatedValuePropagationPass()); // Propagate conditionals
        fpm.add(llvm::createCFGSimplificationPass());     // Merge & remove BBs
        fpm.add(llvm::createInstructionCombiningPass());  // Combine silly seq's

        fpm.add(llvm::createTailCallEliminationPass());   // Eliminate tail calls
        fpm.add(llvm::createCFGSimplificationPass());     // Merge & remove BBs
        fpm.add(llvm::createReassociatePass());           // Reassociate expressions
        fpm.add(llvm::createLoopRotatePass());            // Rotate Loop
        fpm.add(llvm::createLICMPass());                  // Hoist loop invariants
        fpm.add(llvm::createLoopUnswitchPass(true /*optimize_for_size*/));
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createIndVarSimplifyPass());        // Canonicalize indvars
        fpm.add(llvm::createLoopIdiomPass());             // Recognize idioms like memset.
        fpm.add(llvm::createLoopDeletionPass());          // Delete dead loops

        fpm.add(llvm::createLoopUnrollPass());          // Unroll small loops

        fpm.add(llvm::createGVNPass());                 // Remove redundancies
        fpm.add(llvm::createMemCpyOptPass());             // Remove memcpy / form memset
        fpm.add(llvm::createSCCPPass());                  // Constant prop with SCCP

        // Run instcombine after redundancy elimination to exploit opportunities
        // opened up by them.
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createJumpThreadingPass());         // Thread jumps
        fpm.add(llvm::createCorrelatedValuePropagationPass());
        fpm.add(llvm::createDeadStoreEliminationPass());  // Delete dead stores

        fpm.add(llvm::createLoopRerollPass());
        //fpm.add(llvm::createSLPVectorizerPass());   // Vectorize parallel scalar chains.


        fpm.add(llvm::createAggressiveDCEPass());         // Delete dead instructions
        fpm.add(llvm::createCFGSimplificationPass()); // Merge & remove BBs
        fpm.add(llvm::createInstructionCombiningPass());  // Clean up after everything.

        //fpm.add(llvm::createBarrierNoopPass());
        //fpm.add(llvm::createLoopVectorizePass(DisableUnrollLoops, LoopVectorize));
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createCFGSimplificationPass());
    }

    // TODO Find the right place for this pass (and ideally not duplicate it)
    if (ENABLE_PYSTON_PASSES) {
        fpm.add(createConstClassesPass());
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createCFGSimplificationPass());
        fpm.add(createConstClassesPass());
        fpm.add(createDeadAllocsPass());
        //fpm.add(llvm::createSCCPPass());                  // Constant prop with SCCP
        //fpm.add(llvm::createEarlyCSEPass());              // Catch trivial redundancies
        //fpm.add(llvm::createInstructionCombiningPass());
        //fpm.add(llvm::createCFGSimplificationPass());
    }

    fpm.doInitialization();

    for (int i = 0; i < MAX_OPT_ITERATIONS; i++) {
        bool changed = fpm.run(*f);

        if (!changed) {
            if (VERBOSITY("irgen")) printf("done after %d optimization iterations\n", i-1);
            break;
        }

        if (VERBOSITY("irgen") >= 1) {
            fprintf(stderr, "after optimization %d:\n", i);
            printf("\033[36m");
            fflush(stdout);
            dumpPrettyIR(f);
            //f->dump();
            //g.cur_module->dump();
            printf("\033[0m");
            fflush(stdout);
        }
    }

    long us = _t.end();
    static StatCounter us_optimizing("us_compiling_optimizing");
    us_optimizing.log(us);
}

class GuardList {
    public:
        struct ExprTypeGuard {
            CFGBlock *cfg_block;
            llvm::BranchInst* branch;
            AST_expr* ast_node;
            CompilerVariable* val;
            SymbolTable st;

            ExprTypeGuard(CFGBlock *cfg_block, llvm::BranchInst* branch, AST_expr* ast_node, CompilerVariable* val, const SymbolTable &st) :
                    cfg_block(cfg_block), branch(branch), ast_node(ast_node) {
                DupCache cache;
                this->val = val->dup(cache);
                for (SymbolTable::const_iterator it = st.begin(), end = st.end(); it != end; ++it) {
                    this->st[it->first] = it->second->dup(cache);
                }
            }
        };

        struct BlockEntryGuard {
            CFGBlock *cfg_block;
            llvm::BranchInst* branch;
            SymbolTable symbol_table;

            BlockEntryGuard(CFGBlock *cfg_block, llvm::BranchInst* branch, const SymbolTable &symbol_table) :
                    cfg_block(cfg_block), branch(branch) {
                DupCache cache;
                for (SymbolTable::const_iterator it = symbol_table.begin(), end = symbol_table.end(); it != end; ++it) {
                    this->symbol_table[it->first] = it->second->dup(cache);
                }
            }
        };

    private:
        std::unordered_map<AST_expr*, ExprTypeGuard*> expr_type_guards;
        std::unordered_map<CFGBlock*, std::vector<BlockEntryGuard*> > block_begin_guards;

    public:
        typedef std::unordered_map<AST_expr*, ExprTypeGuard*>::iterator expr_type_guard_iterator;
        typedef std::unordered_map<AST_expr*, ExprTypeGuard*>::const_iterator expr_type_guard_const_iterator;
        expr_type_guard_iterator after_begin() {
            return expr_type_guards.begin();
        }
        expr_type_guard_iterator after_end() {
            return expr_type_guards.end();
        }
        ExprTypeGuard* getNodeTypeGuard(AST_expr* node) const {
            expr_type_guard_const_iterator it = expr_type_guards.find(node);
            if (it == expr_type_guards.end())
                return NULL;
            return it->second;
        }

        bool isEmpty() const {
            return expr_type_guards.size() == 0;
        }

        void addExprTypeGuard(CFGBlock *cfg_block, llvm::BranchInst* branch, AST_expr* ast_node, CompilerVariable* val, const SymbolTable &st) {
            ExprTypeGuard* &g = expr_type_guards[ast_node];
            assert(g == NULL);
            g = new ExprTypeGuard(cfg_block, branch, ast_node, val, st);
        }

        void registerGuardForBlockEntry(CFGBlock *cfg_block, llvm::BranchInst* branch, const SymbolTable &st) {
            std::vector<BlockEntryGuard*> &v = block_begin_guards[cfg_block];
            v.push_back(new BlockEntryGuard(cfg_block, branch, st));
        }

        const std::vector<BlockEntryGuard*>& getGuardsForBlock(CFGBlock *block) const {
            std::unordered_map<CFGBlock*, std::vector<BlockEntryGuard*> >::const_iterator it = block_begin_guards.find(block);
            if (it != block_begin_guards.end())
                return it->second;

            static std::vector<BlockEntryGuard*> empty_list;
            return empty_list;
        }
};

class IRGenerator {
    private:
        IRGenState *irstate;

        IREmitterImpl emitter;
        SymbolTable symbol_table;
        std::vector<llvm::BasicBlock*> &entry_blocks;
        llvm::BasicBlock *curblock;
        CFGBlock *myblock;
        TypeAnalysis *types;
        GuardList &out_guards;
        const GuardList &in_guards;

        enum State {
            PARTIAL, // running through a partial block, waiting to hit the first in_guard
            RUNNING, // normal
            DEAD, // passed a Return statement; still syntatically valid but the code should not be compiled
            FINISHED, // passed a pseudo-node such as Branch or Jump; internal error if there are any more statements
        } state;

    public:
        IRGenerator(IRGenState *irstate, std::vector<llvm::BasicBlock*> &entry_blocks, CFGBlock *myblock, TypeAnalysis *types, GuardList &out_guards, const GuardList &in_guards, bool is_partial) : irstate(irstate), emitter(irstate), entry_blocks(entry_blocks), myblock(myblock), types(types), out_guards(out_guards), in_guards(in_guards), state(is_partial ? PARTIAL : RUNNING) {
            llvm::BasicBlock* entry_block = entry_blocks[myblock->idx];
            emitter.getBuilder()->SetInsertPoint(entry_block);
            curblock = entry_block;
        }

        ~IRGenerator() {
            delete emitter.getBuilder();
        }

    private:
        void createExprTypeGuard(llvm::Value *check_val, AST_expr* node, CompilerVariable* node_value) {
            assert(check_val->getType() == g.i1);

            llvm::Value* md_vals[] = {llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1000), getConstantInt(1)};
            llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));

            // For some reason there doesn't seem to be the ability to place the new BB
            // right after the current bb (can only place it *before* something else),
            // but we can put it somewhere arbitrary and then move it.
            llvm::BasicBlock* success_bb = llvm::BasicBlock::Create(g.context, "check_succeeded", irstate->getLLVMFunction());
            success_bb->moveAfter(curblock);

            llvm::BranchInst* guard = emitter.getBuilder()->CreateCondBr(check_val, success_bb, success_bb, branch_weights);

            curblock = success_bb;
            emitter.getBuilder()->SetInsertPoint(curblock);

            out_guards.addExprTypeGuard(myblock, guard, node, node_value, symbol_table);
        }

        CompilerVariable* evalAttribute(AST_Attribute *node) {
            assert(node->ctx_type == AST_TYPE::Load);
            CompilerVariable *value = evalExpr(node->value);
            if (state == PARTIAL)
                return NULL;

            CompilerVariable *rtn = value->getattr(emitter, node->attr);
            value->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalClsAttribute(AST_ClsAttribute *node) {
            CompilerVariable *value = evalExpr(node->value);
            if (state == PARTIAL)
                return NULL;

            //ASSERT((node->attr == "__iter__" || node->attr == "__hasnext__" || node->attr == "next" || node->attr == "__enter__" || node->attr == "__exit__") && (value->getType() == UNDEF || value->getType() == value->getBoxType()) && "inefficient for anything else, should change", "%s", node->attr.c_str());

            ConcreteCompilerVariable *converted = value->makeConverted(emitter, value->getBoxType());
            value->decvref(emitter);

            bool do_patchpoint = ENABLE_ICGETATTRS && emitter.getTarget() != IREmitter::INTERPRETER;
            llvm::Value *rtn;
            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createGetattrPatchpoint(emitter.currentFunction());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(converted->getValue());
                llvm_args.push_back(getStringConstantPtr(node->attr + '\0'));

                llvm::Value* uncasted = emitter.createPatchpoint(pp, (void*)pyston::getclsattr, llvm_args);
                rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            } else {
                rtn = emitter.getBuilder()->CreateCall2(g.funcs.getclsattr,
                        converted->getValue(), getStringConstantPtr(node->attr + '\0'));
            }
            converted->decvref(emitter);
            return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
        }

        enum BinExpType {
            BinOp,
            Compare,
        };
        CompilerVariable* _evalBinExp(CompilerVariable *left, CompilerVariable *right, AST_TYPE::AST_TYPE type, BinExpType exp_type) {
            assert(left);
            assert(right);

            if (left->getType() == INT && right->getType() == INT) {
                ConcreteCompilerVariable *converted_left = left->makeConverted(emitter, INT);
                ConcreteCompilerVariable *converted_right = right->makeConverted(emitter, INT);
                llvm::Value *v;
                if (type == AST_TYPE::Mod) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.mod_i64_i64, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Div || type == AST_TYPE::FloorDiv) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.div_i64_i64, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Pow) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.pow_i64_i64, converted_left->getValue(), converted_right->getValue());
                } else if (exp_type == BinOp) {
                    llvm::Instruction::BinaryOps binopcode;
                    switch (type) {
                        case AST_TYPE::Add:
                            binopcode = llvm::Instruction::Add;
                            break;
                        case AST_TYPE::BitAnd:
                            binopcode = llvm::Instruction::And;
                            break;
                        case AST_TYPE::BitOr:
                            binopcode = llvm::Instruction::Or;
                            break;
                        case AST_TYPE::BitXor:
                            binopcode = llvm::Instruction::Xor;
                            break;
                        case AST_TYPE::LShift:
                            binopcode = llvm::Instruction::Shl;
                            break;
                        case AST_TYPE::RShift:
                            binopcode = llvm::Instruction::AShr;
                            break;
                        case AST_TYPE::Mult:
                            binopcode = llvm::Instruction::Mul;
                            break;
                        case AST_TYPE::Sub:
                            binopcode = llvm::Instruction::Sub;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }
                    v = emitter.getBuilder()->CreateBinOp(binopcode, converted_left->getValue(), converted_right->getValue());
                } else {
                    assert(exp_type == Compare);
                    llvm::CmpInst::Predicate cmp_pred;
                    switch(type) {
                        case AST_TYPE::Eq:
                        case AST_TYPE::Is:
                            cmp_pred = llvm::CmpInst::ICMP_EQ;
                            break;
                        case AST_TYPE::Lt:
                            cmp_pred = llvm::CmpInst::ICMP_SLT;
                            break;
                        case AST_TYPE::LtE:
                            cmp_pred = llvm::CmpInst::ICMP_SLE;
                            break;
                        case AST_TYPE::Gt:
                            cmp_pred = llvm::CmpInst::ICMP_SGT;
                            break;
                        case AST_TYPE::GtE:
                            cmp_pred = llvm::CmpInst::ICMP_SGE;
                            break;
                        case AST_TYPE::NotEq:
                        case AST_TYPE::IsNot:
                            cmp_pred = llvm::CmpInst::ICMP_NE;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }
                    v = emitter.getBuilder()->CreateICmp(cmp_pred, converted_left->getValue(), converted_right->getValue());
                }
                converted_left->decvref(emitter);
                converted_right->decvref(emitter);
                assert(v->getType() == g.i64 || v->getType() == g.i1);
                return new ConcreteCompilerVariable(v->getType() == g.i64 ? INT : BOOL, v, true);
            }

            if (left->getType() == FLOAT && (right->getType() == FLOAT || right->getType() == INT)) {
                ConcreteCompilerVariable *converted_left = left->makeConverted(emitter, FLOAT);

                ConcreteCompilerVariable *converted_right;
                if (right->getType() == FLOAT) {
                    converted_right = right->makeConverted(emitter, FLOAT);
                } else {
                    converted_right = right->makeConverted(emitter, INT);
                    llvm::Value* conv = emitter.getBuilder()->CreateSIToFP(converted_right->getValue(), g.double_);
                    converted_right->decvref(emitter);
                    converted_right = new ConcreteCompilerVariable(FLOAT, conv, true);
                }
                llvm::Value *v;
                bool succeeded = true;
                if (type == AST_TYPE::Mod) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.mod_float_float, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Div || type == AST_TYPE::FloorDiv) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.div_float_float, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Pow) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.pow_float_float, converted_left->getValue(), converted_right->getValue());
                } else if (exp_type == BinOp) {
                    llvm::Instruction::BinaryOps binopcode;
                    switch (type) {
                        case AST_TYPE::Add:
                            binopcode = llvm::Instruction::FAdd;
                            break;
                        case AST_TYPE::Mult:
                            binopcode = llvm::Instruction::FMul;
                            break;
                        case AST_TYPE::Sub:
                            binopcode = llvm::Instruction::FSub;
                            break;
                        case AST_TYPE::BitAnd:
                        case AST_TYPE::BitOr:
                        case AST_TYPE::BitXor:
                        case AST_TYPE::LShift:
                        case AST_TYPE::RShift:
                            succeeded = false;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }

                    if (succeeded) {
                        v = emitter.getBuilder()->CreateBinOp(binopcode, converted_left->getValue(), converted_right->getValue());
                    }
                } else {
                    assert(exp_type == Compare);
                    llvm::CmpInst::Predicate cmp_pred;
                    switch(type) {
                        case AST_TYPE::Eq:
                        case AST_TYPE::Is:
                            cmp_pred = llvm::CmpInst::FCMP_OEQ;
                            break;
                        case AST_TYPE::Lt:
                            cmp_pred = llvm::CmpInst::FCMP_OLT;
                            break;
                        case AST_TYPE::LtE:
                            cmp_pred = llvm::CmpInst::FCMP_OLE;
                            break;
                        case AST_TYPE::Gt:
                            cmp_pred = llvm::CmpInst::FCMP_OGT;
                            break;
                        case AST_TYPE::GtE:
                            cmp_pred = llvm::CmpInst::FCMP_OGE;
                            break;
                        case AST_TYPE::NotEq:
                        case AST_TYPE::IsNot:
                            cmp_pred = llvm::CmpInst::FCMP_UNE;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }
                    v = emitter.getBuilder()->CreateFCmp(cmp_pred, converted_left->getValue(), converted_right->getValue());
                }
                converted_left->decvref(emitter);
                converted_right->decvref(emitter);

                if (succeeded) {
                    assert(v->getType() == g.double_ || v->getType() == g.i1);
                    return new ConcreteCompilerVariable(v->getType() == g.double_ ? FLOAT : BOOL, v, true);
                }
            }
            //ASSERT(left->getType() == left->getBoxType() || right->getType() == right->getBoxType(), "%s %s",
                    //left->getType()->debugName().c_str(), right->getType()->debugName().c_str());

            ConcreteCompilerVariable *boxed_left = left->makeConverted(emitter, left->getBoxType());
            ConcreteCompilerVariable *boxed_right = right->makeConverted(emitter, right->getBoxType());

            llvm::Value* rtn;
            bool do_patchpoint = ENABLE_ICBINEXPS && emitter.getTarget() != IREmitter::INTERPRETER;

            llvm::Value *rt_func;
            void* rt_func_addr;
            if (exp_type == BinOp) {
                rt_func = g.funcs.binop;
                rt_func_addr = (void*)binop;
            } else {
                rt_func = g.funcs.compare;
                rt_func_addr = (void*)compare;
            }

            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createBinexpPatchpoint(emitter.currentFunction());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(boxed_left->getValue());
                llvm_args.push_back(boxed_right->getValue());
                llvm_args.push_back(getConstantInt(type, g.i32));

                llvm::Value* uncasted = emitter.createPatchpoint(pp, rt_func_addr, llvm_args);
                rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            } else {

                rtn = emitter.getBuilder()->CreateCall3(rt_func, boxed_left->getValue(), boxed_right->getValue(), getConstantInt(type, g.i32));
            }

            boxed_left->decvref(emitter);
            boxed_right->decvref(emitter);

            return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
        }

        CompilerVariable* evalBinOp(AST_BinOp *node) {
            CompilerVariable *left = evalExpr(node->left);
            _setFake(_nodeFakeName(0, node), left); // 'fakes' are for handling deopt entries
            CompilerVariable *right = evalExpr(node->right);
            _setFake(_nodeFakeName(1, node), right);

            if (state == PARTIAL) {
                _clearFake(_nodeFakeName(0, node));
                _clearFake(_nodeFakeName(1, node));
                return NULL;
            }
            left = _getFake(_nodeFakeName(0, node));
            right = _getFake(_nodeFakeName(1, node));

            assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

            CompilerVariable *rtn = this->_evalBinExp(left, right, node->op_type, BinOp);
            left->decvref(emitter);
            right->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalBoolOp(AST_BoolOp *node) {
            assert(state != PARTIAL);

            assert(node->op_type == AST_TYPE::And || node->op_type == AST_TYPE::Or);
            bool is_and = node->op_type == AST_TYPE::And;
            int nvals = node->values.size();
            assert(nvals >= 2);

            std::vector<llvm::BasicBlock*> starting_blocks;
            for (int i = 0; i < nvals - 1; i++) {
                starting_blocks.push_back(llvm::BasicBlock::Create(g.context, "", irstate->getLLVMFunction()));
            }
            llvm::BasicBlock *exit_block = llvm::BasicBlock::Create(g.context, "", irstate->getLLVMFunction());
            std::vector<llvm::BasicBlock*> ending_blocks;

            std::vector<llvm::Value*> converted_vals;
            ConcreteCompilerVariable *prev = NULL;

            for (int i = 0; i < nvals; i++) {
                if (i > 0) {
                    assert(prev);
                    prev->decvref(emitter);
                }

                CompilerVariable *v = evalExpr(node->values[i]);
                ConcreteCompilerVariable *converted = prev = v->makeConverted(emitter, v->getBoxType());
                v->decvref(emitter);
                converted_vals.push_back(converted->getValue());

                ending_blocks.push_back(curblock);

                if (i == nvals - 1) {
                    emitter.getBuilder()->CreateBr(exit_block);
                    emitter.getBuilder()->SetInsertPoint(exit_block);
                    curblock = exit_block;
                } else {
                    ConcreteCompilerVariable *nz = converted->nonzero(emitter);
                    //ConcreteCompilerVariable *nz = v->nonzero(emitter);
                    assert(nz->getType() == BOOL);
                    llvm::Value* nz_v = nz->getValue();

                    if (is_and)
                        emitter.getBuilder()->CreateCondBr(nz_v, starting_blocks[i], exit_block);
                    else
                        emitter.getBuilder()->CreateCondBr(nz_v, exit_block, starting_blocks[i]);

                    // Shouldn't generate any code, so should be safe to put after branch instruction;
                    // if that assumption fails, will fail loudly.
                    nz->decvref(emitter);
                    emitter.getBuilder()->SetInsertPoint(starting_blocks[i]);
                    curblock = starting_blocks[i];
                }
            }

            // TODO prev (from the last block) doesn't get freed!

            llvm::PHINode* phi = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, nvals);
            for (int i = 0; i < nvals; i++) {
                phi->addIncoming(converted_vals[i], ending_blocks[i]);
            }

            //cf->func->dump();
            return new ConcreteCompilerVariable(UNKNOWN, phi, true);
        }

        CompilerVariable* evalCompare(AST_Compare *node) {
            RELEASE_ASSERT(node->ops.size() == 1, "");

            CompilerVariable *left = evalExpr(node->left);
            _setFake(_nodeFakeName(0, node), left); // 'fakes' are for handling deopt entries
            CompilerVariable *right = evalExpr(node->comparators[0]);
            _setFake(_nodeFakeName(1, node), right);

            if (state == PARTIAL) {
                _clearFake(_nodeFakeName(0, node));
                _clearFake(_nodeFakeName(1, node));
                return NULL;
            }
            left = _getFake(_nodeFakeName(0, node));
            right = _getFake(_nodeFakeName(1, node));

            assert(left);
            assert(right);

            CompilerVariable *rtn = _evalBinExp(left, right, node->ops[0], Compare);
            left->decvref(emitter);
            right->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalCall(AST_Call *node) {
            bool is_callattr;
            bool callattr_clsonly = false;
            std::string *attr = NULL;
            CompilerVariable *func;
            if (node->func->type == AST_TYPE::Attribute) {
                is_callattr = true;
                callattr_clsonly = false;
                AST_Attribute* attr_ast = static_cast<AST_Attribute*>(node->func);
                func = evalExpr(attr_ast->value);
                attr = &attr_ast->attr;
            } else if (node->func->type == AST_TYPE::ClsAttribute) {
                is_callattr = true;
                callattr_clsonly = true;
                AST_ClsAttribute* attr_ast = static_cast<AST_ClsAttribute*>(node->func);
                func = evalExpr(attr_ast->value);
                attr = &attr_ast->attr;
            } else {
                is_callattr = false;
                func = evalExpr(node->func);
            }

            _setFake(_nodeFakeName(-1, node), func);

            std::vector<CompilerVariable*> args;
            for (int i = 0; i < node->args.size(); i++) {
                CompilerVariable *a = evalExpr(node->args[i]);
                _setFake(_nodeFakeName(i, node), a);
            }
            if (state == PARTIAL) {
                _clearFake(_nodeFakeName(-1, node));
                for (int i = 0; i < node->args.size(); i++) {
                    _clearFake(_nodeFakeName(i, node));
                }
                return NULL;
            }

            func = _getFake(_nodeFakeName(-1, node));
            for (int i = 0; i < node->args.size(); i++) {
                args.push_back(_getFake(_nodeFakeName(i, node)));
            }

            //if (VERBOSITY("irgen") >= 1)
                //_addAnnotation("before_call");

            CompilerVariable *rtn;
            if (is_callattr) {
                rtn = func->callattr(emitter, *attr, callattr_clsonly, args);
            } else {
                rtn = func->call(emitter, args);
            }

            func->decvref(emitter);
            for (int i = 0; i < args.size(); i++) {
                args[i]->decvref(emitter);
            }

            //if (VERBOSITY("irgen") >= 1)
                //_addAnnotation("end_of_call");

            return rtn;
        }

        CompilerVariable* evalDict(AST_Dict *node) {
            assert(state != PARTIAL);

            llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
            ConcreteCompilerVariable *rtn = new ConcreteCompilerVariable(DICT, v, true);
            if (node->keys.size()) {
                CompilerVariable *setitem = rtn->getattr(emitter, "__setitem__");
                for (int i = 0; i < node->keys.size(); i++) {
                    CompilerVariable *key = evalExpr(node->keys[i]);
                    CompilerVariable *value = evalExpr(node->values[i]);

                    std::vector<CompilerVariable*> args;
                    args.push_back(key);
                    args.push_back(value);
                    // TODO could use the internal _listAppend function to avoid incref/decref'ing None
                    CompilerVariable *rtn = setitem->call(emitter, args);
                    rtn->decvref(emitter);

                    key->decvref(emitter);
                    value->decvref(emitter);
                }
                setitem->decvref(emitter);
            }
            return rtn;
        }

        void _addAnnotation(const char* message) {
            llvm::Instruction *inst = emitter.getBuilder()->CreateCall(llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::donothing));
            llvm::Value* md_vals[] = {getConstantInt(0)};
            llvm::MDNode* mdnode = llvm::MDNode::get(g.context, md_vals);
            inst->setMetadata(message, mdnode);
        }

        CompilerVariable* evalIndex(AST_Index *node) {
            return evalExpr(node->value);
        }

        CompilerVariable* evalList(AST_List *node) {
            for (int i = 0; i < node->elts.size(); i++) {
                CompilerVariable *value = evalExpr(node->elts[i]);
                _setFake(_nodeFakeName(i, node), value);
            }

            if (state == PARTIAL) {
                for (int i = 0; i < node->elts.size(); i++) {
                    _clearFake(_nodeFakeName(i, node));
                }
                return NULL;
            }

            std::vector<CompilerVariable*> elts;
            for (int i = 0; i < node->elts.size(); i++) {
                elts.push_back(_getFake(_nodeFakeName(i, node)));
            }

            llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createList);
            ConcreteCompilerVariable *rtn = new ConcreteCompilerVariable(LIST, v, true);

            llvm::Value *f = g.funcs.listAppendInternal;
            llvm::Value *bitcast = emitter.getBuilder()->CreateBitCast(v, *llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(f->getType())->getElementType())->param_begin());

            for (int i = 0; i < node->elts.size(); i++) {
                CompilerVariable *elt = elts[i];
                ConcreteCompilerVariable *converted = elt->makeConverted(emitter, elt->getBoxType());
                elt->decvref(emitter);

                emitter.getBuilder()->CreateCall2(f, bitcast, converted->getValue());
                converted->decvref(emitter);
            }
            return rtn;
        }

        CompilerVariable* getNone() {
            ConcreteCompilerVariable *v = new ConcreteCompilerVariable(typeFromClass(none_cls), embedConstantPtr(None, g.llvm_value_type_ptr), false);
            return v;
        }

        CompilerVariable* evalName(AST_Name *node) {
            if (state == PARTIAL)
                return NULL;

            if (irstate->getScopeInfo()->refersToGlobal(node->id)) {
                if (1) {
                    // Method 1: calls into the runtime getGlobal(), which handles things like falling back to builtins
                    // or raising the correct error message.
                    bool do_patchpoint = ENABLE_ICGETGLOBALS && emitter.getTarget() != IREmitter::INTERPRETER;
                    bool from_global = irstate->getSourceInfo()->ast->type == AST_TYPE::Module;
                    if (do_patchpoint) {
                        PatchpointSetupInfo *pp = patchpoints::createGetGlobalPatchpoint(emitter.currentFunction());

                        std::vector<llvm::Value*> llvm_args;
                        llvm_args.push_back(embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
                        llvm_args.push_back(embedConstantPtr(&node->id, g.llvm_str_type_ptr));
                        llvm_args.push_back(getConstantInt(from_global, g.i1));

                        llvm::Value* uncasted = emitter.createPatchpoint(pp, (void*)pyston::getGlobal, llvm_args);
                        llvm::Value* r = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
                        return new ConcreteCompilerVariable(UNKNOWN, r, true);
                    } else {
                        llvm::Value *r = emitter.getBuilder()->CreateCall3(g.funcs.getGlobal, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr), embedConstantPtr(&node->id, g.llvm_str_type_ptr), getConstantInt(from_global, g.i1));
                        return new ConcreteCompilerVariable(UNKNOWN, r, true);
                    }
                } else {
                    // Method 2 [testing-only]: (ab)uses existing getattr patchpoints and just calls module.getattr()
                    // This option exists for performance testing because method 1 does not currently use patchpoints.
                    ConcreteCompilerVariable *mod = new ConcreteCompilerVariable(MODULE, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_value_type_ptr), false);
                    CompilerVariable *attr = mod->getattr(emitter, node->id);
                    mod->decvref(emitter);
                    return attr;
                }
            } else {
                if (symbol_table.find(node->id) == symbol_table.end()) {
                    // TODO should mark as DEAD here, though we won't end up setting all the names appropriately
                    //state = DEAD;
                    llvm::CallInst *call = emitter.getBuilder()->CreateCall2(g.funcs.assertNameDefined, getConstantInt(0, g.i1), getStringConstantPtr(node->id + '\0'));
                    call->setDoesNotReturn();
                    return undefVariable();
                }

                std::string defined_name = _getFakeName("is_defined", node->id.c_str());
                ConcreteCompilerVariable *is_defined = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));
                if (is_defined) {
                    emitter.getBuilder()->CreateCall2(g.funcs.assertNameDefined, is_defined->getValue(), getStringConstantPtr(node->id + '\0'));
                }

                CompilerVariable *rtn = symbol_table[node->id];
                rtn->incvref();
                return rtn;
            }
        }

        CompilerVariable* evalNum(AST_Num *node) {
            if (state == PARTIAL)
                return NULL;
            if (node->num_type == AST_Num::INT)
                return makeInt(node->n_int);
            else if (node->num_type == AST_Num::FLOAT)
                return makeFloat(node->n_float);
            else
                RELEASE_ASSERT(0, "");
        }

        CompilerVariable* evalSlice(AST_Slice *node) {
            if (state == PARTIAL)
                return NULL;

            CompilerVariable *start, *stop, *step;
            start = node->lower ? evalExpr(node->lower) : getNone();
            stop = node->upper ? evalExpr(node->upper) : getNone();
            step = node->step ? evalExpr(node->step) : getNone();

            ConcreteCompilerVariable *cstart, *cstop, *cstep;
            cstart = start->makeConverted(emitter, start->getBoxType());
            cstop = stop->makeConverted(emitter, stop->getBoxType());
            cstep = step->makeConverted(emitter, step->getBoxType());
            start->decvref(emitter);
            stop->decvref(emitter);
            step->decvref(emitter);

            std::vector<llvm::Value*> args;
            args.push_back(cstart->getValue());
            args.push_back(cstop->getValue());
            args.push_back(cstep->getValue());
            llvm::Value* rtn = emitter.getBuilder()->CreateCall(g.funcs.createSlice, args);

            cstart->decvref(emitter);
            cstop->decvref(emitter);
            cstep->decvref(emitter);
            return new ConcreteCompilerVariable(SLICE, rtn, true);
        }

        CompilerVariable* evalStr(AST_Str *node) {
            if (state == PARTIAL)
                return NULL;
            return makeStr(&node->s);
        }

        CompilerVariable* evalSubscript(AST_Subscript *node) {
            CompilerVariable *value = evalExpr(node->value);
            _setFake(_nodeFakeName(0, node), value); // 'fakes' are for handling deopt entries
            CompilerVariable *slice = evalExpr(node->slice);
            _setFake(_nodeFakeName(1, node), slice);

            if (state == PARTIAL) {
                _clearFake(_nodeFakeName(0, node));
                _clearFake(_nodeFakeName(1, node));
                return NULL;
            }
            value = _getFake(_nodeFakeName(0, node));
            slice = _getFake(_nodeFakeName(1, node));

            CompilerVariable *rtn = value->getitem(emitter, slice);
            value->decvref(emitter);
            slice->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalTuple(AST_Tuple *node) {
            for (int i = 0; i < node->elts.size(); i++) {
                CompilerVariable *value = evalExpr(node->elts[i]);
                _setFake(_nodeFakeName(i, node), value);
            }

            if (state == PARTIAL) {
                for (int i = 0; i < node->elts.size(); i++) {
                    _clearFake(_nodeFakeName(i, node));
                }
                return NULL;
            }

            std::vector<CompilerVariable*> elts;
            for (int i = 0; i < node->elts.size(); i++) {
                elts.push_back(_getFake(_nodeFakeName(i, node)));
            }

            // TODO makeTuple should probably just transfer the vref, but I want to keep things consistent
            CompilerVariable *rtn = makeTuple(elts);
            for (int i = 0; i < node->elts.size(); i++) {
                elts[i]->decvref(emitter);
            }
            return rtn;
        }

        CompilerVariable* evalUnaryOp(AST_UnaryOp *node) {
            assert(state != PARTIAL);

            CompilerVariable* operand = evalExpr(node->operand);

            if (node->op_type == AST_TYPE::Not) {
                ConcreteCompilerVariable *rtn = operand->nonzero(emitter);
                operand->decvref(emitter);

                llvm::Value* v = rtn->getValue();
                assert(v->getType() == g.i1);
                llvm::Value* negated = emitter.getBuilder()->CreateNot(v);
                rtn->decvref(emitter);
                return new ConcreteCompilerVariable(BOOL, negated, true);
            } else {
                // TODO These are pretty inefficient, but luckily I don't think they're used that often:
                ConcreteCompilerVariable *converted = operand->makeConverted(emitter, operand->getBoxType());
                operand->decvref(emitter);

                llvm::Value* rtn = emitter.getBuilder()->CreateCall2(g.funcs.unaryop, converted->getValue(), getConstantInt(node->op_type, g.i32));
                converted->decvref(emitter);

                return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
            }
        }

        ConcreteCompilerVariable* unboxVar(ConcreteCompilerType *t, llvm::Value *v, bool grabbed) {
            if (t == BOXED_INT) {
                llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxInt, v);
                ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(INT, unboxed, true);
                return rtn;
            }
            if (t == BOXED_FLOAT) {
                llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxFloat, v);
                ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(FLOAT, unboxed, true);
                return rtn;
            }
            return new ConcreteCompilerVariable(t, v, grabbed);
        }

        CompilerVariable* evalExpr(AST_expr *node) {
            emitter.getBuilder()->SetCurrentDebugLocation(llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));

            CompilerVariable *rtn;
            switch (node->type) {
                case AST_TYPE::Attribute:
                    rtn = evalAttribute(static_cast<AST_Attribute*>(node));
                    break;
                case AST_TYPE::BinOp:
                    rtn = evalBinOp(static_cast<AST_BinOp*>(node));
                    break;
                case AST_TYPE::BoolOp:
                    rtn = evalBoolOp(static_cast<AST_BoolOp*>(node));
                    break;
                case AST_TYPE::Call:
                    rtn = evalCall(static_cast<AST_Call*>(node));
                    break;
                case AST_TYPE::Compare:
                    rtn = evalCompare(static_cast<AST_Compare*>(node));
                    break;
                case AST_TYPE::Dict:
                    rtn = evalDict(static_cast<AST_Dict*>(node));
                    break;
                case AST_TYPE::Index:
                    rtn = evalIndex(static_cast<AST_Index*>(node));
                    break;
                case AST_TYPE::List:
                    rtn = evalList(static_cast<AST_List*>(node));
                    break;
                case AST_TYPE::Name:
                    rtn = evalName(static_cast<AST_Name*>(node));
                    break;
                case AST_TYPE::Num:
                    rtn = evalNum(static_cast<AST_Num*>(node));
                    break;
                case AST_TYPE::Slice:
                    rtn = evalSlice(static_cast<AST_Slice*>(node));
                    break;
                case AST_TYPE::Str:
                    rtn = evalStr(static_cast<AST_Str*>(node));
                    break;
                case AST_TYPE::Subscript:
                    rtn = evalSubscript(static_cast<AST_Subscript*>(node));
                    break;
                case AST_TYPE::Tuple:
                    rtn = evalTuple(static_cast<AST_Tuple*>(node));
                    break;
                case AST_TYPE::UnaryOp:
                    rtn = evalUnaryOp(static_cast<AST_UnaryOp*>(node));
                    break;
                case AST_TYPE::ClsAttribute:
                    rtn = evalClsAttribute(static_cast<AST_ClsAttribute*>(node));
                    break;
                default:
                    printf("Unhandled expr type: %d (irgen.cpp:" STRINGIFY(__LINE__) ")\n", node->type);
                    exit(1);
            }

            if (rtn == NULL) {
                assert(state == PARTIAL);
            }

            // Out-guarding:
            BoxedClass *speculated_class = types->speculatedExprClass(node);
            if (speculated_class != NULL && state != PARTIAL) {
                assert(rtn);

                ConcreteCompilerType *speculated_type = typeFromClass(speculated_class);
                if (VERBOSITY("irgen") >= 1) {
                    printf("Speculating that %s is actually %s, at ", rtn->getConcreteType()->debugName().c_str(), speculated_type->debugName().c_str());
                    PrintVisitor printer;
                    node->accept(&printer);
                    printf("\n");
                }

                // That's not really a speculation.... could potentially handle this here, but
                // I think it's better to just not generate bad speculations:
                assert(!rtn->canConvertTo(speculated_type));

                ConcreteCompilerVariable *old_rtn = rtn->makeConverted(emitter, UNKNOWN);
                rtn->decvref(emitter);

                llvm::Value* guard_check = old_rtn->makeClassCheck(emitter, speculated_class);
                assert(guard_check->getType() == g.i1);
                createExprTypeGuard(guard_check, node, old_rtn);

                rtn = unboxVar(speculated_type, old_rtn->getValue(), true);
            }

            // In-guarding:
            GuardList::ExprTypeGuard* guard = in_guards.getNodeTypeGuard(node);
            if (guard != NULL) {
                if (VERBOSITY("irgen") >= 1) {
                    printf("merging guard after ");
                    PrintVisitor printer;
                    node->accept(&printer);
                    printf("; is_partial=%d\n", state == PARTIAL);
                }
                if (state == PARTIAL) {
                    guard->branch->setSuccessor(1, curblock);
                    symbol_table = SymbolTable(guard->st);
                    assert(guard->val);
                    state = RUNNING;

                    return guard->val;
                } else {
                    assert(state == RUNNING);
                    compareKeyset(&symbol_table, &guard->st);

                    assert(symbol_table.size() == guard->st.size());
                    llvm::BasicBlock *ramp_block = llvm::BasicBlock::Create(g.context, "deopt_ramp", irstate->getLLVMFunction());
                    llvm::BasicBlock *join_block = llvm::BasicBlock::Create(g.context, "deopt_join", irstate->getLLVMFunction());
                    SymbolTable joined_st;
                    for (SymbolTable::iterator it = guard->st.begin(), end = guard->st.end(); it != end; ++it) {
                        //if (VERBOSITY("irgen") >= 1) printf("merging %s\n", it->first.c_str());
                        CompilerVariable *curval = symbol_table[it->first];
                        // I'm not sure this is necessary or even correct:
                        //ASSERT(curval->getVrefs() == it->second->getVrefs(), "%s %d %d", it->first.c_str(), curval->getVrefs(), it->second->getVrefs());

                        ConcreteCompilerType *merged_type = curval->getConcreteType();

                        emitter.getBuilder()->SetInsertPoint(ramp_block);
                        ConcreteCompilerVariable* converted1 = it->second->makeConverted(emitter, merged_type);
                        it->second->decvref(emitter); // for makeconverted
                        //guard->st[it->first] = converted;
                        //it->second->decvref(emitter); // for the replaced version

                        emitter.getBuilder()->SetInsertPoint(curblock);
                        ConcreteCompilerVariable* converted2 = curval->makeConverted(emitter, merged_type);
                        curval->decvref(emitter); // for makeconverted
                        //symbol_table[it->first] = converted;
                        //curval->decvref(emitter); // for the replaced version

                        if (converted1->getValue() == converted2->getValue()) {
                            joined_st[it->first] = new ConcreteCompilerVariable(merged_type, converted1->getValue(), true);
                        } else {
                            emitter.getBuilder()->SetInsertPoint(join_block);
                            llvm::PHINode* phi = emitter.getBuilder()->CreatePHI(merged_type->llvmType(), 2, it->first);
                            phi->addIncoming(converted1->getValue(), ramp_block);
                            phi->addIncoming(converted2->getValue(), curblock);
                            joined_st[it->first] = new ConcreteCompilerVariable(merged_type, phi, true);
                        }

                        // TODO free dead Variable objects!
                    }
                    symbol_table = joined_st;

                    emitter.getBuilder()->SetInsertPoint(curblock);
                    emitter.getBuilder()->CreateBr(join_block);

                    emitter.getBuilder()->SetInsertPoint(ramp_block);
                    emitter.getBuilder()->CreateBr(join_block);

                    guard->branch->setSuccessor(1, ramp_block);

                    {
                        ConcreteCompilerType *this_merged_type = rtn->getConcreteType();

                        emitter.getBuilder()->SetInsertPoint(ramp_block);
                        ConcreteCompilerVariable *converted_guard_rtn = guard->val->makeConverted(emitter, this_merged_type);
                        guard->val->decvref(emitter);

                        emitter.getBuilder()->SetInsertPoint(curblock);
                        ConcreteCompilerVariable *converted_rtn = rtn->makeConverted(emitter, this_merged_type);
                        rtn->decvref(emitter);

                        emitter.getBuilder()->SetInsertPoint(join_block);
                        llvm::PHINode* this_phi = emitter.getBuilder()->CreatePHI(this_merged_type->llvmType(), 2);
                        this_phi->addIncoming(converted_rtn->getValue(), curblock);
                        this_phi->addIncoming(converted_guard_rtn->getValue(), ramp_block);
                        rtn = new ConcreteCompilerVariable(this_merged_type, this_phi, true);

                        // TODO free dead Variable objects!
                    }

                    curblock = join_block;
                    emitter.getBuilder()->SetInsertPoint(curblock);
                }
            }

            return rtn;
        }

        static std::string _getFakeName(const char* prefix, const char* token) {
            char buf[40];
            snprintf(buf, 40, "!%s_%s", prefix, token);
            return std::string(buf);
        }
        static std::string _nodeFakeName(int idx, AST* node) {
            char buf[40];
            snprintf(buf, 40, "%p(%d)_%d", (void*)node, node->type, idx);
            return _getFakeName("node", buf);
        }
        void _setFake(std::string name, CompilerVariable* val) {
            assert(name[0] == '!');
            CompilerVariable* &cur = symbol_table[name];
            assert(cur == NULL);
            cur = val;
        }
        CompilerVariable* _clearFake(std::string name) {
            assert(name[0] == '!');
            CompilerVariable* rtn = symbol_table[name];
            assert(rtn == NULL);
            symbol_table.erase(name);
            return rtn;
        }
        CompilerVariable* _getFake(std::string name, bool allow_missing=false) {
            assert(name[0] == '!');
            CompilerVariable* rtn = symbol_table[name];
            if (!allow_missing)
                assert(rtn != NULL);
            symbol_table.erase(name);
            return rtn;
        }

        void _doSet(const std::string &name, CompilerVariable* val) {
            assert(name != "None");
            if (irstate->getScopeInfo()->refersToGlobal(name)) {
                // TODO do something special here so that it knows to only emit a monomorphic inline cache?
                ConcreteCompilerVariable* module = new ConcreteCompilerVariable(MODULE, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_value_type_ptr), false);
                module->setattr(emitter, name, val);
                module->decvref(emitter);
            } else {
                CompilerVariable* &prev = symbol_table[name];
                if (prev != NULL) {
                    prev->decvref(emitter);
                }
                prev = val;
                val->incvref();
            }
        }

        void _doSetattr(AST_Attribute* target, CompilerVariable* val) {
            assert(state != PARTIAL);
            CompilerVariable *t = evalExpr(target->value);
            t->setattr(emitter, target->attr, val);
            t->decvref(emitter);
        }

        void _doSetitem(AST_Subscript* target, CompilerVariable* val) {
            assert(state != PARTIAL);
            CompilerVariable *tget = evalExpr(target->value);
            CompilerVariable *slice = evalExpr(target->slice);

            ConcreteCompilerVariable *converted_target = tget->makeConverted(emitter, tget->getBoxType());
            ConcreteCompilerVariable *converted_slice = slice->makeConverted(emitter, slice->getBoxType());
            tget->decvref(emitter);
            slice->decvref(emitter);

            ConcreteCompilerVariable *converted_val = val->makeConverted(emitter, val->getBoxType());

            bool do_patchpoint = ENABLE_ICSETITEMS && emitter.getTarget() != IREmitter::INTERPRETER;
            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createSetitemPatchpoint(emitter.currentFunction());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(converted_target->getValue());
                llvm_args.push_back(converted_slice->getValue());
                llvm_args.push_back(converted_val->getValue());

                emitter.createPatchpoint(pp, (void*)pyston::setitem, llvm_args);
            } else {
                emitter.getBuilder()->CreateCall3(g.funcs.setitem,
                        converted_target->getValue(), converted_slice->getValue(), converted_val->getValue());
            }

            converted_target->decvref(emitter);
            converted_slice->decvref(emitter);
            converted_val->decvref(emitter);
        }

        void _doUnpackTuple(AST_Tuple* target, CompilerVariable* val) {
            int ntargets = target->elts.size();
            ConcreteCompilerVariable *len = val->len(emitter);
            emitter.getBuilder()->CreateCall2(g.funcs.checkUnpackingLength,
                    getConstantInt(ntargets, g.i64), len->getValue());

            for (int i = 0; i < ntargets; i++) {
                CompilerVariable *unpacked = val->getitem(emitter, makeInt(i));
                _doSet(target->elts[i], unpacked);
                unpacked->decvref(emitter);
            }
        }

        void _doSet(AST* target, CompilerVariable* val) {
            switch (target->type) {
                case AST_TYPE::Attribute:
                    _doSetattr(static_cast<AST_Attribute*>(target), val);
                    break;
                case AST_TYPE::Name:
                    _doSet(static_cast<AST_Name*>(target)->id, val);
                    break;
                case AST_TYPE::Subscript:
                    _doSetitem(static_cast<AST_Subscript*>(target), val);
                    break;
                case AST_TYPE::Tuple:
                    _doUnpackTuple(static_cast<AST_Tuple*>(target), val);
                    break;
                default:
                    ASSERT(0, "Unknown type for IRGenerator: %d", target->type);
                    abort();
            }
        }

        void doAssign(AST_Assign *node) {
            CompilerVariable *val = evalExpr(node->value);
            if (state == PARTIAL)
                return;
            for (int i = 0; i < node->targets.size(); i++) {
                _doSet(node->targets[i], val);
            }
            val->decvref(emitter);
        }

        void doClassDef(AST_ClassDef *node) {
            if (state == PARTIAL)
                return;

            ScopeInfo *scope_info = irstate->getSourceInfo()->scoping->getScopeInfoForNode(node);

            llvm::Value *classobj = emitter.getBuilder()->CreateCall2(g.funcs.createClass, embedConstantPtr(&node->name, g.llvm_str_type_ptr), embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
            ConcreteCompilerVariable* cls = new ConcreteCompilerVariable(typeFromClass(type_cls), classobj, true);

            RELEASE_ASSERT(node->bases.size() == 1, "");
            RELEASE_ASSERT(node->bases[0]->type == AST_TYPE::Name, "");
            RELEASE_ASSERT(static_cast<AST_Name*>(node->bases[0])->id == "object", "");

            //CompilerVariable* name = makeStr(&node->name);
            //cls->setattr(emitter, "__name__", name);
            //name->decvref(emitter);

            for (int i = 0, n = node->body.size(); i < n; i++) {
                AST_TYPE::AST_TYPE type = node->body[i]->type;
                if (type == AST_TYPE::Pass) {
                    continue;
                } else if (type == AST_TYPE::FunctionDef) {
                    AST_FunctionDef *fdef = static_cast<AST_FunctionDef*>(node->body[i]);
                    CLFunction *cl = this->_wrapFunction(fdef);
                    CompilerVariable *func = makeFunction(emitter, cl);
                    cls->setattr(emitter, fdef->name, func);
                    func->decvref(emitter);
                } else {
                    RELEASE_ASSERT(node->body[i]->type == AST_TYPE::Pass, "%d", type);
                }
            }

            _doSet(node->name, cls);
            cls->decvref(emitter);
        }

        CLFunction* _wrapFunction(AST_FunctionDef *node) {
            // Different compilations of the parent scope of a functiondef should lead
            // to the same CLFunction* being used:
            static std::unordered_map<AST_FunctionDef*, CLFunction*> made;

            CLFunction* &cl = made[node];
            if (cl == NULL) {
                SourceInfo *si = new SourceInfo(irstate->getSourceInfo()->parent_module, irstate->getSourceInfo()->scoping);
                si->ast = node;
                cl = new CLFunction(si);
            }
            return cl;
        }

        void doFunction(AST_FunctionDef *node) {
            if (state == PARTIAL)
                return;

            CLFunction *cl = this->_wrapFunction(node);
            CompilerVariable *func = makeFunction(emitter, cl);

            //llvm::Type* boxCLFuncArgType = g.funcs.boxCLFunction->arg_begin()->getType();
            //llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxCLFunction, embedConstantPtr(cl, boxCLFuncArgType));
            //CompilerVariable *func = new ConcreteCompilerVariable(typeFromClass(function_cls), boxed, true);

            _doSet(node->name, func);
            func->decvref(emitter);
        }

        void doIf(AST_If *node) {
            assert(0);
        }

        void doImport(AST_Import *node) {
            for (int i = 0; i < node->names.size(); i++) {
                AST_alias *alias = node->names[i];

                std::string &modname = alias->name;
                std::string &asname = alias->asname.size() ? alias->asname : alias->name;

                llvm::Value* imported = emitter.getBuilder()->CreateCall(g.funcs.import, embedConstantPtr(&modname, g.llvm_str_type_ptr));
                ConcreteCompilerVariable *v = new ConcreteCompilerVariable(UNKNOWN, imported, true);
                _doSet(asname, v);
                v->decvref(emitter);
            }
        }

        void doPrint(AST_Print *node) {
            assert(node->dest == NULL);
            for (int i = 0; i < node->values.size(); i++) {
                if (i > 0) {
                    emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr(" "));
                }
                CompilerVariable* var = evalExpr(node->values[i]);
                if (state != PARTIAL) {
                    var->print(emitter);
                    var->decvref(emitter);
                }
            }
            if (state != PARTIAL) {
                if (node->nl)
                    emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr("\n"));
                else
                    emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr(" "));
            }
        }

        void doReturn(AST_Return *node) {
            CompilerVariable *val;
            if (node->value == NULL) {
                if (irstate->getReturnType() == VOID) {
                    endBlock(DEAD);
                    emitter.getBuilder()->CreateRetVoid();
                    return;
                }

                val = new ConcreteCompilerVariable(NONE, embedConstantPtr(None, g.llvm_value_type_ptr), false);
            } else {
                val = evalExpr(node->value);
                assert(state != PARTIAL);
            }
            assert(val);

            // If we ask the return variable to become UNKNOWN (the typical return type),
            // it will be forced to split a copy of itself and incref.
            // But often the return variable will already be in the right shape, so in
            // that case asking it to convert to itself ends up just being an incvref
            // and doesn't end up emitting an incref+decref pair.
            // This could also be handled by casting from the CompilerVariable to
            // ConcreteCOmpilerVariable, but this way feels a little more robust to me.
            ConcreteCompilerType *opt_rtn_type = irstate->getReturnType();
            if (irstate->getReturnType()->llvmType() == val->getConcreteType()->llvmType())
                opt_rtn_type = val->getConcreteType();

            ConcreteCompilerVariable* rtn = val->makeConverted(emitter, opt_rtn_type);
            rtn->ensureGrabbed(emitter);
            val->decvref(emitter);

            endBlock(DEAD);

            assert(rtn->getVrefs() == 1);
            emitter.getBuilder()->CreateRet(rtn->getValue());
        }

        void doBranch(AST_Branch *node) {
            assert(node->iftrue->idx > myblock->idx);
            assert(node->iffalse->idx > myblock->idx);

            CompilerVariable *val = evalExpr(node->test);
            assert(state != PARTIAL);

            ConcreteCompilerVariable* nonzero = val->nonzero(emitter);
            assert(nonzero->getType() == BOOL);
            val->decvref(emitter);

            llvm::Value *llvm_nonzero = nonzero->getValue();
            llvm::BasicBlock *iftrue = entry_blocks[node->iftrue->idx];
            llvm::BasicBlock *iffalse = entry_blocks[node->iffalse->idx];

            nonzero->decvref(emitter);

            endBlock(FINISHED);

            emitter.getBuilder()->CreateCondBr(llvm_nonzero, iftrue, iffalse);
        }

        void doExpr(AST_Expr *node) {
            CompilerVariable *var = evalExpr(node->value);
            if (state != PARTIAL)
                var->decvref(emitter);
        }

        void doOSRExit(llvm::BasicBlock *normal_target, AST_Jump* osr_key) {
            llvm::BasicBlock *starting_block = curblock;
            llvm::BasicBlock *onramp = llvm::BasicBlock::Create(g.context, "onramp", irstate->getLLVMFunction());

            // Code to check if we want to do the OSR:
            llvm::GlobalVariable* edgecount_ptr = new llvm::GlobalVariable(*g.cur_module, g.i64, false, llvm::GlobalValue::InternalLinkage, getConstantInt(0, g.i64), "edgecount");
            llvm::Value* curcount = emitter.getBuilder()->CreateLoad(edgecount_ptr);
            llvm::Value* newcount = emitter.getBuilder()->CreateAdd(curcount, getConstantInt(1, g.i64));
            emitter.getBuilder()->CreateStore(newcount, edgecount_ptr);

            int OSR_THRESHOLD = 10000;
            if (irstate->getEffortLevel() == EffortLevel::INTERPRETED)
                OSR_THRESHOLD = 100;
            llvm::Value* osr_test = emitter.getBuilder()->CreateICmpSGT(newcount, getConstantInt(OSR_THRESHOLD));

            llvm::Value* md_vals[] = {llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1), getConstantInt(1000)};
            llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));
            emitter.getBuilder()->CreateCondBr(osr_test, onramp, normal_target, branch_weights);

            // Emitting the actual OSR:
            emitter.getBuilder()->SetInsertPoint(onramp);
            OSRExit* exit = new OSRExit(irstate->getCurFunction(), OSREntryDescriptor::create(irstate->getCurFunction(), osr_key));
            llvm::Value* partial_func = emitter.getBuilder()->CreateCall(g.funcs.compilePartialFunc, embedConstantPtr(exit, g.i8->getPointerTo()));

            std::vector<llvm::Value*> llvm_args;
            std::vector<llvm::Type*> llvm_arg_types;
            std::vector<ConcreteCompilerVariable*> converted_args;

            SortedSymbolTable sorted_symbol_table(symbol_table.begin(), symbol_table.end());
            /*
            for (SortedSymbolTable::iterator it = sorted_symbol_table.begin(), end = sorted_symbol_table.end(); it != end; ) {
                if (!source->liveness->isLiveAtEnd(it->first, myblock)) {
                    // I think this line can never get hit: nothing can die on a backedge, since control flow can eventually
                    // reach this block again, where the symbol was live (as shown by it being in the symbol table)
                    printf("Not sending %s to osr since it will die\n", it->first.c_str());
                    it = sorted_symbol_table.erase(it);
                } else {
                    ++it;
                }
            }*/

            // For OSR calls, we use the same calling convention as in some other places; namely,
            // arg1, arg2, arg3, argarray [nargs is ommitted]
            // It would be nice to directly pass all variables as arguments, instead of packing them into
            // an array, for a couple reasons (eliminate copies, and allow for a tail call).
            // But this doesn't work if the IR is being interpreted, because the interpreter can't
            // do arbitrary-arity function calls (yet?).  One possibility is to pass them as an
            // array for the interpreter and as all arguments for compilation, but I'd rather avoid
            // having two different calling conventions for the same thing.  Plus, this would
            // prevent us from having two OSR exits point to the same OSR entry; not something that
            // we're doing right now but something that would be nice in the future.

            llvm::Value *arg_array = NULL, *malloc_save = NULL;
            if (sorted_symbol_table.size() > 3) {
                // Leave in the ability to use malloc but I guess don't use it.
                // Maybe if there are a ton of live variables it'd be nice to have them be
                // heap-allocated, or if we don't immediately return the result of the OSR?
                bool use_malloc = false;
                if (false) {
                    llvm::Value *n_bytes = getConstantInt((sorted_symbol_table.size() - 3) * sizeof(Box*), g.i64);
                    llvm::Value *l_malloc = embedConstantPtr((void*)malloc, llvm::FunctionType::get(g.i8->getPointerTo(), g.i64, false)->getPointerTo());
                    malloc_save = emitter.getBuilder()->CreateCall(l_malloc, n_bytes);
                    arg_array = emitter.getBuilder()->CreateBitCast(malloc_save, g.llvm_value_type_ptr->getPointerTo());
                } else {
                    llvm::Value *n_varargs = llvm::ConstantInt::get(g.i64, sorted_symbol_table.size() - 3, false);
                    arg_array = emitter.getBuilder()->CreateAlloca(g.llvm_value_type_ptr, n_varargs);
                }
            }

            int i = 0;
            for (SortedSymbolTable::iterator it = sorted_symbol_table.begin(), end = sorted_symbol_table.end(); it != end; ++it, ++i) {
                // I don't think this can fail, but if it can we should filter out dead symbols before
                // passing them on:
                assert(irstate->getSourceInfo()->liveness->isLiveAtEnd(it->first, myblock));

                // This line can never get hit right now since we unnecessarily force every variable to be concrete
                // for a loop, since we generate all potential phis:
                ASSERT(it->second->getType() == it->second->getConcreteType(), "trying to pass through %s\n", it->second->getType()->debugName().c_str());

                ConcreteCompilerVariable* var = it->second->makeConverted(emitter, it->second->getConcreteType());
                converted_args.push_back(var);

                assert(var->getType() != BOXED_INT && "should probably unbox it, but why is it boxed in the first place?");
                assert(var->getType() != BOXED_FLOAT && "should probably unbox it, but why is it boxed in the first place?");

                // This line can never get hit right now for the same reason that the variables must already be concrete,
                // because we're over-generating phis.
                ASSERT(var->isGrabbed(), "%s", it->first.c_str());
                //var->ensureGrabbed(emitter);

                llvm::Value* val = var->getValue();

                if (i < 3) {
                    llvm_args.push_back(val);
                    llvm_arg_types.push_back(val->getType());
                } else {
                    llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, i-3);

                    if (var->getType() == INT) {
                        val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                    } else if (var->getType() == FLOAT) {
                        //val = emitter.getBuilder()->CreateBitCast(val, g.llvm_value_type_ptr);
                        ptr = emitter.getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                    } else {
                        assert(val->getType() == g.llvm_value_type_ptr);
                    }

                    emitter.getBuilder()->CreateStore(val, ptr);
                }

                ConcreteCompilerType* &t = exit->entry->args[it->first];
                if (t == NULL)
                    t = var->getType();
                else
                    ASSERT(t == var->getType(), "%s %s\n", t->debugName().c_str(), var->getType()->debugName().c_str());
            }

            if (sorted_symbol_table.size() > 3) {
                llvm_args.push_back(arg_array);
                llvm_arg_types.push_back(arg_array->getType());
            }

            llvm::FunctionType* ft = llvm::FunctionType::get(irstate->getReturnType()->llvmType(), llvm_arg_types, false /*vararg*/);
            partial_func = emitter.getBuilder()->CreateBitCast(partial_func, ft->getPointerTo());

            llvm::CallInst *rtn = emitter.getBuilder()->CreateCall(partial_func, llvm_args);

            // If we alloca'd the arg array, we can't make this into a tail call:
            if (arg_array == NULL && malloc_save != NULL) {
                rtn->setTailCall(true);
            }

            if (malloc_save != NULL) {
                llvm::Value *l_free = embedConstantPtr((void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
                emitter.getBuilder()->CreateCall(l_free, malloc_save);
            }

            for (int i = 0; i < converted_args.size(); i++) {
                converted_args[i]->decvref(emitter);
            }

            if (irstate->getReturnType() == VOID)
                emitter.getBuilder()->CreateRetVoid();
            else
                emitter.getBuilder()->CreateRet(rtn);

            emitter.getBuilder()->SetInsertPoint(starting_block);
        }

        void doJump(AST_Jump *node) {
            endBlock(FINISHED);

            llvm::BasicBlock *target = entry_blocks[node->target->idx];

            if (ENABLE_OSR && node->target->idx < myblock->idx && irstate->getEffortLevel() < EffortLevel::MAXIMAL) {
                assert(node->target->predecessors.size() > 1);
                doOSRExit(target, node);
            } else {
                emitter.getBuilder()->CreateBr(target);
            }
        }

        void doStmt(AST *node) {
            switch (node->type) {
                case AST_TYPE::Assign:
                    doAssign(static_cast<AST_Assign*>(node));
                    break;
                case AST_TYPE::ClassDef:
                    doClassDef(static_cast<AST_ClassDef*>(node));
                    break;
                case AST_TYPE::Expr:
                    doExpr(static_cast<AST_Expr*>(node));
                    break;
                case AST_TYPE::FunctionDef:
                    doFunction(static_cast<AST_FunctionDef*>(node));
                    break;
                case AST_TYPE::If:
                    doIf(static_cast<AST_If*>(node));
                    break;
                case AST_TYPE::Import:
                    doImport(static_cast<AST_Import*>(node));
                    break;
                case AST_TYPE::Global:
                    // Should have been handled already
                    break;
                case AST_TYPE::Pass:
                    break;
                case AST_TYPE::Print:
                    doPrint(static_cast<AST_Print*>(node));
                    break;
                case AST_TYPE::Return:
                    doReturn(static_cast<AST_Return*>(node));
                    break;
                case AST_TYPE::Branch:
                    doBranch(static_cast<AST_Branch*>(node));
                    break;
                case AST_TYPE::Jump:
                    doJump(static_cast<AST_Jump*>(node));
                    break;
                default:
                    printf("Unhandled stmt type at " __FILE__ ":" STRINGIFY(__LINE__) ": %d\n", node->type);
                    exit(1);
            }
        }

        template <typename T>
        void loadArgument(const T &name, ConcreteCompilerType* t, llvm::Value* v) {
            ConcreteCompilerVariable *var = unboxVar(t, v, false);
            _doSet(name, var);
            var->decvref(emitter);
        }

        void endBlock(State new_state) {
            assert(state == RUNNING);

            //cf->func->dump();

            SourceInfo* source = irstate->getSourceInfo();
            ScopeInfo *scope_info = irstate->getScopeInfo();

            for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end();) {
                ASSERT(it->first[0] != '!' || startswith(it->first, "!is_defined"), "left a fake variable in the real symbol table? '%s'", it->first.c_str());

                if (!source->liveness->isLiveAtEnd(it->first, myblock)) {
                    //printf("%s dead at end of %d; grabbed = %d, %d vrefs\n", it->first.c_str(), myblock->idx, it->second->isGrabbed(), it->second->getVrefs());
                    it->second->decvref(emitter);
                    it = symbol_table.erase(it);
                } else if (source->phis->isRequiredAfter(it->first, myblock)) {
                    assert(!scope_info->refersToGlobal(it->first));
                    ConcreteCompilerType *phi_type = types->getTypeAtBlockEnd(it->first, myblock);
                    //printf("Converting %s from %s to %s\n", it->first.c_str(), it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                    //printf("have to convert %s from %s to %s\n", it->first.c_str(), it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                    ConcreteCompilerVariable *v = it->second->makeConverted(emitter, phi_type);
                    it->second->decvref(emitter);
                    symbol_table[it->first] = v->split(emitter);
                    it++;
                } else {
#ifndef NDEBUG
                    // TODO getTypeAtBlockEnd will automatically convert up to the concrete type, which we don't want here,
                    // but this is just for debugging so I guess let it happen for now:
                    ConcreteCompilerType *ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                    ASSERT(it->second->canConvertTo(ending_type), "%s is supposed to be %s, but somehow is %s", it->first.c_str(), ending_type->debugName().c_str(), it->second->getType()->debugName().c_str());
#endif

                    it++;
                }
            }

            const PhiAnalysis::RequiredSet &all_phis = source->phis->getAllRequiredAfter(myblock);
            for (PhiAnalysis::RequiredSet::const_iterator it = all_phis.begin(), end = all_phis.end(); it != end; ++it) {
                //printf("phi will be required for %s\n", it->c_str());
                assert(!scope_info->refersToGlobal(*it));
                CompilerVariable* &cur = symbol_table[*it];

                if (cur != NULL) {
                    if (source->phis->isPotentiallyUndefinedAfter(*it, myblock)) {
                        //printf("is potentially undefined\n");
                        _setFake(_getFakeName("is_defined", it->c_str()), new ConcreteCompilerVariable(BOOL, getConstantInt(1, g.i1), true));
                    } else {
                        //printf("is definitely defined\n");
                    }
                } else {
                    //printf("no st entry, setting undefined\n");
                    ConcreteCompilerType *phi_type = types->getTypeAtBlockEnd(*it, myblock);
                    cur = new ConcreteCompilerVariable(phi_type, llvm::UndefValue::get(phi_type->llvmType()), true);
                    _setFake(_getFakeName("is_defined", it->c_str()), new ConcreteCompilerVariable(BOOL, getConstantInt(0, g.i1), true));
                }
            }

            state = new_state;
        }

    public:
        struct EndingState {
            SymbolTable* symbol_table;
            ConcreteSymbolTable* phi_symbol_table;
            llvm::BasicBlock* ending_block;
            EndingState(SymbolTable* symbol_table, ConcreteSymbolTable* phi_symbol_table, llvm::BasicBlock* ending_block) :
                    symbol_table(symbol_table), phi_symbol_table(phi_symbol_table), ending_block(ending_block) {}
        };

        EndingState getEndingSymbolTable() {
            assert(state == FINISHED || state == DEAD);

            //for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end(); ++it) {
                //printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
            //}

            SourceInfo* source = irstate->getSourceInfo();

            SymbolTable *st = new SymbolTable(symbol_table);
            ConcreteSymbolTable *phi_st = new ConcreteSymbolTable();
            for (SymbolTable::iterator it = st->begin(); it != st->end(); it++) {
                if (it->first[0] == '!') {
                    ASSERT(startswith(it->first, _getFakeName("is_defined", "")), "left a fake variable in the real symbol table? '%s'", it->first.c_str());
                } else {
                    ASSERT(source->liveness->isLiveAtEnd(it->first, myblock), "%s", it->first.c_str());
                }
            }

            if (myblock->successors.size() == 0) {
                assert(st->size() == 0); // shouldn't have anything live if there are no successors!
                return EndingState(st, phi_st, curblock);
            } else if (myblock->successors.size() > 1) {
                // Since there are no critical edges, all successors come directly from this node,
                // so there won't be any required phis.
                return EndingState(st, phi_st, curblock);
            }

            assert(myblock->successors.size() == 1); // other cases should have been handled
            for (SymbolTable::iterator it = st->begin(); it != st->end();) {
                if (startswith(it->first, "!is_defined") || source->phis->isRequiredAfter(it->first, myblock)) {
                    assert(it->second->isGrabbed());
                    assert(it->second->getVrefs() == 1);
                    // this conversion should have already happened... should refactor this.
                    ConcreteCompilerType *ending_type;
                    if (startswith(it->first, "!is_defined")) {
                        assert(it->second->getType() == BOOL);
                        ending_type = BOOL;
                    } else {
                        ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                    }
                    //(*phi_st)[it->first] = it->second->makeConverted(emitter, it->second->getConcreteType());
                    //printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
                    (*phi_st)[it->first] = it->second->split(emitter)->makeConverted(emitter, ending_type);
                    it = st->erase(it);
                } else {
                    ++it;
                }
            }
            return EndingState(st, phi_st, curblock);
        }

        void giveLocalSymbol(const std::string &name, CompilerVariable *var) {
            assert(name != "None");
            ASSERT(!irstate->getScopeInfo()->refersToGlobal(name), "%s", name.c_str());
            assert(var->getType() != BOXED_INT);
            assert(var->getType() != BOXED_FLOAT);
            CompilerVariable* &cur = symbol_table[name];
            assert(cur == NULL);
            cur = var;
        }

        void copySymbolsFrom(SymbolTable* st) {
            assert(st);
            DupCache cache;
            for (SymbolTable::iterator it = st->begin(); it != st->end(); it++) {
                //printf("Copying in %s: %p, %d\n", it->first.c_str(), it->second, it->second->getVrefs());
                symbol_table[it->first] = it->second->dup(cache);
                //printf("got: %p, %d\n", symbol_table[it->first], symbol_table[it->first]->getVrefs());
            }
        }

        void unpackArguments(const std::vector<AST_expr*> &arg_names, const std::vector<ConcreteCompilerType*> &arg_types) {
            int i = 0;
            llvm::Value* argarray = NULL;
            for (llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin(); AI != irstate->getLLVMFunction()->arg_end(); AI++, i++) {
                if (i == 3) {
                    argarray = AI;
                    break;
                }
                loadArgument(arg_names[i], arg_types[i], AI);
            }

            for (int i = 3; i < arg_types.size(); i++) {
                llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(argarray, i-3);
                llvm::Value* loaded = emitter.getBuilder()->CreateLoad(ptr);

                if (arg_types[i]->llvmType() == g.i64)
                    loaded = emitter.getBuilder()->CreatePtrToInt(loaded, arg_types[i]->llvmType());
                else
                    assert(arg_types[i]->llvmType() == g.llvm_value_type_ptr);

                loadArgument(arg_names[i], arg_types[i], loaded);
            }
        }

        void run(const CFGBlock* block) {
            for (int i = 0; i < block->body.size(); i++) {
                if (state == DEAD)
                    break;
                assert(state != FINISHED);
                doStmt(block->body[i]);
            }
        }

};

static bool compareBlockPairs (const std::pair<CFGBlock*, CFGBlock*>& p1, const std::pair<CFGBlock*, CFGBlock*>& p2) {
    return p1.first->idx < p2.first->idx;
}

static std::vector<std::pair<CFGBlock*, CFGBlock*> > computeBlockTraversalOrder(const BlockSet &full_blocks, const BlockSet &partial_blocks, CFGBlock *start) {

    std::vector<std::pair<CFGBlock*, CFGBlock*> > rtn;
    std::unordered_set<CFGBlock*> in_queue;

    if (start) {
        assert(full_blocks.count(start));
        in_queue.insert(start);
        rtn.push_back(std::make_pair(start, (CFGBlock*)NULL));
    }

    for (BlockSet::const_iterator it = partial_blocks.begin(), end = partial_blocks.end(); it != end; ++it) {
        in_queue.insert(*it);
        rtn.push_back(std::make_pair(*it, (CFGBlock*)NULL));
    }

    // It's important for debugging purposes that the order is deterministic, but the iteration
    // over the BlockSet is not:
    std::sort(rtn.begin(), rtn.end(), compareBlockPairs);

    int idx = 0;
    while (rtn.size() < full_blocks.size() + partial_blocks.size()) {
        // TODO: come up with an alternative algorithm that outputs
        // the blocks in "as close to in-order as possible".
        // Do this by iterating over all blocks and picking the smallest one
        // that has a predecessor in the list already.
        while (idx < rtn.size()) {
            CFGBlock *cur = rtn[idx].first;

            for (int i = 0; i < cur->successors.size(); i++) {
                CFGBlock *b = cur->successors[i];
                assert(full_blocks.count(b) || partial_blocks.count(b));
                if (in_queue.count(b))
                    continue;

                rtn.push_back(std::make_pair(b, cur));
                in_queue.insert(b);
            }

            idx++;
        }

        if (rtn.size() == full_blocks.size() + partial_blocks.size())
            break;

        CFGBlock *best = NULL;
        for (BlockSet::const_iterator it = full_blocks.begin(), end = full_blocks.end(); it != end; ++it) {
            CFGBlock *b = *it;
            if (in_queue.count(b))
                continue;

            // Avoid picking any blocks where we can't add an epilogue to the predecessors
            if (b->predecessors.size() == 1 && b->predecessors[0]->successors.size() > 1)
                continue;

            if (best == NULL || b->idx < best->idx)
                best = b;
        }
        assert(best != NULL);

        if (VERBOSITY("irgen") >= 1) printf("Giving up and adding block %d to the order\n", best->idx);
        in_queue.insert(best);
        rtn.push_back(std::make_pair(best, (CFGBlock*)NULL));
    }

    ASSERT(rtn.size() == full_blocks.size() + partial_blocks.size(), "%ld\n", rtn.size());
    return rtn;
}

static void emitBBs(IRGenState* irstate, const char* bb_type, GuardList &out_guards, const GuardList &in_guards, TypeAnalysis *types, const std::vector<AST_expr*> &arg_names, const OSREntryDescriptor *entry_descriptor, const BlockSet &full_blocks, const BlockSet &partial_blocks) {
    SourceInfo *source = irstate->getSourceInfo();
    EffortLevel::EffortLevel effort = irstate->getEffortLevel();
    CompiledFunction *cf = irstate->getCurFunction();
    ConcreteCompilerType *rtn_type = irstate->getReturnType();
    llvm::MDNode* func_info = irstate->getFuncDbgInfo();

    if (entry_descriptor != NULL)
        assert(full_blocks.count(source->cfg->blocks[0]) == 0);

    // We need the entry blocks pre-allocated so that we can jump forward to them.
    std::vector<llvm::BasicBlock*> llvm_entry_blocks;
    for (int i = 0; i < source->cfg->blocks.size(); i++) {
        CFGBlock *block = source->cfg->blocks[i];
        if (partial_blocks.count(block) == 0 && full_blocks.count(block) == 0) {
            llvm_entry_blocks.push_back(NULL);
            continue;
        }

        char buf[40];
        snprintf(buf, 40, "%s_block%d", bb_type, i);
        llvm_entry_blocks.push_back(llvm::BasicBlock::Create(g.context, buf, irstate->getLLVMFunction()));
    }

    llvm::BasicBlock *osr_entry_block = NULL; // the function entry block, where we add the type guards
    llvm::BasicBlock *osr_unbox_block = NULL; // the block after type guards where we up/down-convert things
    ConcreteSymbolTable *osr_syms = NULL; // syms after conversion
    if (entry_descriptor != NULL) {
        osr_unbox_block = llvm::BasicBlock::Create(g.context, "osr_unbox", irstate->getLLVMFunction(), &irstate->getLLVMFunction()->getEntryBlock());
        osr_entry_block = llvm::BasicBlock::Create(g.context, "osr_entry", irstate->getLLVMFunction(), &irstate->getLLVMFunction()->getEntryBlock());
        assert(&irstate->getLLVMFunction()->getEntryBlock() == osr_entry_block);

        osr_syms = new ConcreteSymbolTable();
        SymbolTable *initial_syms = new SymbolTable();
        //llvm::BranchInst::Create(llvm_entry_blocks[entry_descriptor->backedge->target->idx], entry_block);

        IREmitterImpl entry_emitter(irstate);
        entry_emitter.getBuilder()->SetInsertPoint(osr_entry_block);
        IREmitterImpl unbox_emitter(irstate);
        unbox_emitter.getBuilder()->SetInsertPoint(osr_unbox_block);

        CFGBlock *target_block = entry_descriptor->backedge->target;

        // Currently we AND all the type guards together and then do just a single jump;
        // guard_val is the current AND'd value, or NULL if there weren't any guards
        llvm::Value *guard_val = NULL;

        std::vector<llvm::Value*> func_args;
        for (llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin(); AI != irstate->getLLVMFunction()->arg_end(); AI++) {
            func_args.push_back(AI);
        }

        // Handle loading symbols from the passed osr arguments:
        int i = 0;
        for (OSREntryDescriptor::ArgMap::const_iterator it = entry_descriptor->args.begin(), end = entry_descriptor->args.end(); it != end; ++it, ++i) {
            llvm::Value* from_arg;
            if (i < 3) {
                from_arg = func_args[i];
            } else {
                ASSERT(func_args.size() == 4, "%ld", func_args.size());
                llvm::Value* ptr = entry_emitter.getBuilder()->CreateConstGEP1_32(func_args[3], i-3);
                if (it->second == INT) {
                    ptr = entry_emitter.getBuilder()->CreateBitCast(ptr, g.i64->getPointerTo());
                } else if (it->second == FLOAT) {
                    ptr = entry_emitter.getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                }
                from_arg = entry_emitter.getBuilder()->CreateLoad(ptr);
                assert(from_arg->getType() == it->second->llvmType());
            }

            ConcreteCompilerType *phi_type = types->getTypeAtBlockStart(it->first, target_block);
            //ConcreteCompilerType *analyzed_type = types->getTypeAtBlockStart(it->first, block);
            //ConcreteCompilerType *phi_type = (*phis)[it->first].first;

            ConcreteCompilerVariable *var = new ConcreteCompilerVariable(it->second, from_arg, true);
            (*initial_syms)[it->first] = var;

            // It's possible to OSR into a version of the function with a higher speculation level;
            // this means that the types of the OSR variables are potentially higher (more unspecialized)
            // than what the optimized code expects.
            // So, we have to re-check the speculations and potentially deopt.
            llvm::Value *v = NULL;
            if (it->second == phi_type) {
                // good to go
                v = from_arg;
            } else if (it->second->canConvertTo(phi_type)) {
                // not sure if/when this happens, but if there's a type mismatch but one we know
                // can be handled (such as casting from a subclass to a superclass), handle it:
                ConcreteCompilerVariable *converted = var->makeConverted(unbox_emitter, phi_type);
                v = converted->getValue();
                delete converted;
            } else {
                ASSERT(it->second == UNKNOWN, "%s", it->second->debugName().c_str());
                BoxedClass *speculated_class = NULL;
                if (phi_type == INT) {
                    speculated_class = int_cls;
                } else if (phi_type == FLOAT) {
                    speculated_class = float_cls;
                } else {
                    speculated_class = phi_type->guaranteedClass();
                }
                ASSERT(speculated_class, "%s", phi_type->debugName().c_str());

                llvm::Value* type_check = ConcreteCompilerVariable(it->second, from_arg, true).makeClassCheck(entry_emitter, speculated_class);
                if (guard_val) {
                    guard_val = entry_emitter.getBuilder()->CreateAnd(guard_val, type_check);
                } else {
                    guard_val = type_check;
                }
                //entry_emitter.getBuilder()->CreateCall(g.funcs.my_assert, type_check);

                if (speculated_class == int_cls) {
                    v = unbox_emitter.getBuilder()->CreateCall(g.funcs.unboxInt, from_arg);
                    (new ConcreteCompilerVariable(BOXED_INT, from_arg, true))->decvref(unbox_emitter);
                } else if (speculated_class == float_cls) {
                    v = unbox_emitter.getBuilder()->CreateCall(g.funcs.unboxFloat, from_arg);
                    (new ConcreteCompilerVariable(BOXED_FLOAT, from_arg, true))->decvref(unbox_emitter);
                } else {
                    assert(phi_type == typeFromClass(speculated_class));
                    v = from_arg;
                }
            }

            if (VERBOSITY("irgen")) v->setName("prev_" + it->first);

            (*osr_syms)[it->first] = new ConcreteCompilerVariable(phi_type, v, true);
        }

        if (guard_val) {
            llvm::BranchInst *br = entry_emitter.getBuilder()->CreateCondBr(guard_val, osr_unbox_block, osr_unbox_block);
            out_guards.registerGuardForBlockEntry(target_block, br, *initial_syms);
        } else {
            entry_emitter.getBuilder()->CreateBr(osr_unbox_block);
        }
        unbox_emitter.getBuilder()->CreateBr(llvm_entry_blocks[entry_descriptor->backedge->target->idx]);

        for (SymbolTable::iterator it = initial_syms->begin(), end = initial_syms->end(); it != end; ++it) {
            delete it->second;
        }
        delete initial_syms;
    }

    // In a similar vein, we need to keep track of the exit blocks for each cfg block,
    // so that we can construct phi nodes later.
    // Originally I preallocated these blocks as well, but we can construct the phi's
    // after the fact, so we can just record the exit blocks as we go along.
    std::unordered_map<int, llvm::BasicBlock*> llvm_exit_blocks;

    ////
    // Main ir generation: go through each basic block in the CFG and emit the code

    std::unordered_map<int, SymbolTable*> ending_symbol_tables;
    std::unordered_map<int, ConcreteSymbolTable*> phi_ending_symbol_tables;
    typedef std::unordered_map<std::string, std::pair<ConcreteCompilerType*, llvm::PHINode*> > PHITable;
    std::unordered_map<int, PHITable*> created_phis;

    CFGBlock* initial_block = NULL;
    if (entry_descriptor) {
        initial_block = entry_descriptor->backedge->target;
    } else if (full_blocks.count(source->cfg->blocks[0])) {
        initial_block = source->cfg->blocks[0];
    }

    // The rest of this code assumes that for each non-entry block that gets evaluated,
    // at least one of its predecessors has been evaluated already (from which it will
    // get type information).
    // The cfg generation code will generate a cfg such that each block has a predecessor
    // with a lower index value, so if the entry block is 0 then we can iterate in index
    // order.
    // The entry block doesn't have to be zero, so we have to calculate an allowable order here:
    std::vector<std::pair<CFGBlock*, CFGBlock*> > traversal_order = computeBlockTraversalOrder(full_blocks, partial_blocks, initial_block);

    std::unordered_set<CFGBlock*> into_hax;
    for (int _i = 0; _i < traversal_order.size(); _i++) {
        CFGBlock *block = traversal_order[_i].first;
        CFGBlock *pred = traversal_order[_i].second;
    //for (int _i = 0; _i < source->cfg->blocks.size(); _i++) {
        //CFGBlock *block = source->cfg->blocks[_i];
        //CFGBlock *pred = NULL;
        //if (block->predecessors.size())
            //CFGBlock *pred = block->predecessors[0];

        if (VERBOSITY("irgen") >= 1) printf("processing %s block %d\n", bb_type, block->idx);

        bool is_partial = false;
        if (partial_blocks.count(block)) {
            if (VERBOSITY("irgen") >= 1) printf("is partial block\n");
            is_partial = true;
        } else if (!full_blocks.count(block)) {
            if (VERBOSITY("irgen") >= 1) printf("Skipping this block\n");
            //created_phis[block->idx] = NULL;
            //ending_symbol_tables[block->idx] = NULL;
            //phi_ending_symbol_tables[block->idx] = NULL;
            //llvm_exit_blocks[block->idx] = NULL;
            continue;
        }

        IRGenerator generator(irstate, llvm_entry_blocks, block, types, out_guards, in_guards, is_partial);
        IREmitterImpl emitter(irstate);
        emitter.getBuilder()->SetInsertPoint(llvm_entry_blocks[block->idx]);

        PHITable* phis = NULL;
        if (!is_partial) {
            phis = new PHITable();
            created_phis[block->idx] = phis;
        }

        // Set initial symbol table:
        if (is_partial) {
            // pass
        } else if (block->idx == 0) {
            assert(entry_descriptor == NULL);
            // number of times a function needs to be called to be reoptimized:
            static const int REOPT_THRESHOLDS[] = {
                10,      // INTERPRETED->MINIMAL
                250,     // MINIMAL->MODERATE
                10000,   // MODERATE->MAXIMAL
            };

            assert(strcmp("opt", bb_type) == 0);

            if (ENABLE_REOPT && effort < EffortLevel::MAXIMAL && source->ast != NULL && source->ast->type != AST_TYPE::Module) {
                llvm::BasicBlock* preentry_bb = llvm::BasicBlock::Create(g.context, "pre_entry", irstate->getLLVMFunction(), llvm_entry_blocks[0]);
                llvm::BasicBlock* reopt_bb = llvm::BasicBlock::Create(g.context, "reopt", irstate->getLLVMFunction());
                emitter.getBuilder()->SetInsertPoint(preentry_bb);

                llvm::Value *call_count_ptr = embedConstantPtr(&cf->times_called, g.i64->getPointerTo());
                llvm::Value *cur_call_count = emitter.getBuilder()->CreateLoad(call_count_ptr);
                llvm::Value *new_call_count = emitter.getBuilder()->CreateAdd(cur_call_count, getConstantInt(1, g.i64));
                emitter.getBuilder()->CreateStore(new_call_count, call_count_ptr);
                llvm::Value *reopt_test = emitter.getBuilder()->CreateICmpSGT(new_call_count, getConstantInt(REOPT_THRESHOLDS[effort], g.i64));

                llvm::Value* md_vals[] = {llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1), getConstantInt(1000)};
                llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));

                llvm::BranchInst* guard = emitter.getBuilder()->CreateCondBr(reopt_test, reopt_bb, llvm_entry_blocks[0], branch_weights);

                emitter.getBuilder()->SetInsertPoint(reopt_bb);
                //emitter.getBuilder()->CreateCall(g.funcs.my_assert, getConstantInt(0, g.i1));
                llvm::Value* r = emitter.getBuilder()->CreateCall(g.funcs.reoptCompiledFunc, embedConstantPtr(cf, g.i8->getPointerTo()));
                assert(r);
                assert(r->getType() == g.i8->getPointerTo());

                llvm::Value *bitcast_r = emitter.getBuilder()->CreateBitCast(r, irstate->getLLVMFunction()->getType());

                std::vector<llvm::Value*> args;
                //bitcast_r->dump();
                for (llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin(); AI != irstate->getLLVMFunction()->arg_end(); AI++) {
                    //AI->dump();
                    args.push_back(&(*AI));
                }
                //printf("%ld\n", args.size());
                llvm::CallInst *postcall = emitter.getBuilder()->CreateCall(bitcast_r, args);
                postcall->setTailCall(true);
                if (rtn_type == VOID) {
                    emitter.getBuilder()->CreateRetVoid();
                } else {
                    emitter.getBuilder()->CreateRet(postcall);
                }

                emitter.getBuilder()->SetInsertPoint(llvm_entry_blocks[0]);
            }
            generator.unpackArguments(arg_names, cf->sig->arg_types);
        } else if (entry_descriptor && block == entry_descriptor->backedge->target) {
            assert(block->predecessors.size() > 1);
            assert(osr_entry_block);
            assert(phis);

            for (OSREntryDescriptor::ArgMap::const_iterator it = entry_descriptor->args.begin(), end = entry_descriptor->args.end(); it != end; ++it) {
                ConcreteCompilerType *analyzed_type = types->getTypeAtBlockStart(it->first, block);
                //printf("For %s, given %s, analyzed for %s\n", it->first.c_str(), it->second->debugName().c_str(), analyzed_type->debugName().c_str());

                llvm::PHINode *phi = emitter.getBuilder()->CreatePHI(analyzed_type->llvmType(), block->predecessors.size()+1, it->first);
                ConcreteCompilerVariable *var = new ConcreteCompilerVariable(analyzed_type, phi, true);
                generator.giveLocalSymbol(it->first, var);
                (*phis)[it->first] = std::make_pair(analyzed_type, phi);
            }
        } else if (pred == NULL) {
            assert(traversal_order.size() < source->cfg->blocks.size());
            assert(phis);
            assert(block->predecessors.size());
            for (int i = 0; i < block->predecessors.size(); i++) {
                CFGBlock *b2 = block->predecessors[i];
                assert(ending_symbol_tables.count(b2->idx) == 0);
                into_hax.insert(b2);
            }

            const PhiAnalysis::RequiredSet &names = source->phis->getAllDefinedAt(block);
            for (PhiAnalysis::RequiredSet::const_iterator it = names.begin(), end = names.end(); it != end; ++it) {
                // TODO the list from getAllDefinedAt should come filtered:
                if (!source->liveness->isLiveAtEnd(*it, block->predecessors[0]))
                    continue;

                //printf("adding guessed phi for %s\n", it->c_str());
                ConcreteCompilerType *type = types->getTypeAtBlockStart(*it, block);
                llvm::PHINode *phi = emitter.getBuilder()->CreatePHI(type->llvmType(), block->predecessors.size(), *it);
                ConcreteCompilerVariable *var = new ConcreteCompilerVariable(type, phi, true);
                generator.giveLocalSymbol(*it, var);

                (*phis)[*it] = std::make_pair(type, phi);
            }
        } else {
            assert(pred);
            assert(full_blocks.count(pred) || partial_blocks.count(pred));

            if (block->predecessors.size() == 1)  {
                // If this block has only one predecessor, it by definition doesn't need any phi nodes.
                // Assert that the phi_st is empty, and just create the symbol table from the non-phi st:
                assert(phi_ending_symbol_tables[pred->idx]->size() == 0);
                assert(ending_symbol_tables.count(pred->idx));
                generator.copySymbolsFrom(ending_symbol_tables[pred->idx]);
            } else {
                // With multiple predecessors, the symbol tables at the end of each predecessor should be *exactly* the same.
                // (this should be satisfied by the post-run() code in this function)

                // With multiple predecessors, we have to combine the non-phi and phi symbol tables.
                // Start off with the non-phi ones:
                generator.copySymbolsFrom(ending_symbol_tables[pred->idx]);

                // And go through and add phi nodes:
                ConcreteSymbolTable *pred_st = phi_ending_symbol_tables[pred->idx];
                for (ConcreteSymbolTable::iterator it = pred_st->begin(); it != pred_st->end(); it++) {
                    //printf("adding phi for %s\n", it->first.c_str());
                    llvm::PHINode *phi = emitter.getBuilder()->CreatePHI(it->second->getType()->llvmType(), block->predecessors.size(), it->first);
                    //emitter.getBuilder()->CreateCall(g.funcs.dump, phi);
                    ConcreteCompilerVariable *var = new ConcreteCompilerVariable(it->second->getType(), phi, true);
                    generator.giveLocalSymbol(it->first, var);

                    (*phis)[it->first] = std::make_pair(it->second->getType(), phi);
                }
            }
        }

        generator.run(block);

        const IRGenerator::EndingState &ending_st = generator.getEndingSymbolTable();
        ending_symbol_tables[block->idx] = ending_st.symbol_table;
        phi_ending_symbol_tables[block->idx] = ending_st.phi_symbol_table;
        llvm_exit_blocks[block->idx] = ending_st.ending_block;

        if (into_hax.count(block))
            ASSERT(ending_st.symbol_table->size() == 0, "%d", block->idx);
    }

    ////
    // Phi generation.
    // We don't know the exact ssa values to back-propagate to the phi nodes until we've generated
    // the relevant IR, so after we have done all of it, go back through and populate the phi nodes.
    // Also, do some checking to make sure that the phi analysis stuff worked out, and that all blocks
    // agreed on what symbols + types they should be propagiting for the phis.
    for (int i = 0; i < source->cfg->blocks.size(); i++) {
        PHITable *phis = created_phis[i];
        if (phis == NULL)
            continue;

        bool this_is_osr_entry = (entry_descriptor && i == entry_descriptor->backedge->target->idx);

        CFGBlock *b = source->cfg->blocks[i];

        const std::vector<GuardList::BlockEntryGuard*> &block_guards = in_guards.getGuardsForBlock(b);

        for (int j = 0; j < b->predecessors.size(); j++) {
            CFGBlock *b2 = b->predecessors[j];
            if (full_blocks.count(b2) == 0 && partial_blocks.count(b2) == 0)
                continue;

            //printf("%d %d %ld %ld\n", i, b2->idx, phi_ending_symbol_tables[b2->idx]->size(), phis->size());
            compareKeyset(phi_ending_symbol_tables[b2->idx], phis);
            assert(phi_ending_symbol_tables[b2->idx]->size() == phis->size());
        }

        if (this_is_osr_entry) {
            compareKeyset(osr_syms, phis);
        }

        std::vector<IREmitterImpl*> emitters;
        std::vector<llvm::BasicBlock*> offramps;
        for (int i = 0; i < block_guards.size(); i++) {
            compareKeyset(&block_guards[i]->symbol_table, phis);

            llvm::BasicBlock* off_ramp = llvm::BasicBlock::Create(g.context, "deopt_ramp", irstate->getLLVMFunction());
            offramps.push_back(off_ramp);
            IREmitterImpl *emitter = new IREmitterImpl(irstate);
            emitter->getBuilder()->SetInsertPoint(off_ramp);
            emitters.push_back(emitter);

            block_guards[i]->branch->setSuccessor(1, off_ramp);
        }

        for (PHITable::iterator it = phis->begin(); it != phis->end(); it++) {
            llvm::PHINode* llvm_phi = it->second.second;
            for (int j = 0; j < b->predecessors.size(); j++) {
                CFGBlock *b2 = b->predecessors[j];
                if (full_blocks.count(b2) == 0 && partial_blocks.count(b2) == 0)
                    continue;

                ConcreteCompilerVariable *v = (*phi_ending_symbol_tables[b2->idx])[it->first];
                assert(v);
                assert(v->isGrabbed());

                // Make sure they all prepared for the same type:
                ASSERT(it->second.first == v->getType(), "%d %d: %s %s %s", b->idx, b2->idx, it->first.c_str(), it->second.first->debugName().c_str(), v->getType()->debugName().c_str());

                llvm_phi->addIncoming(v->getValue(), llvm_exit_blocks[b->predecessors[j]->idx]);
            }

            if (this_is_osr_entry) {
                ConcreteCompilerVariable *v = (*osr_syms)[it->first];
                assert(v);
                assert(v->isGrabbed());

                ASSERT(it->second.first == v->getType(), "");
                llvm_phi->addIncoming(v->getValue(), osr_unbox_block);
            }

            for (int i = 0; i < block_guards.size(); i++) {
                GuardList::BlockEntryGuard *g = block_guards[i];
                IREmitterImpl *emitter = emitters[i];

                CompilerVariable *unconverted = g->symbol_table[it->first];
                ConcreteCompilerVariable *v = unconverted->makeConverted(*emitter, it->second.first);
                assert(v);
                assert(v->isGrabbed());


                ASSERT(it->second.first == v->getType(), "");
                llvm_phi->addIncoming(v->getValue(), offramps[i]);

                // TODO not sure if this is right:
                unconverted->decvref(*emitter);
                delete v;
            }
        }

        for (int i = 0; i < block_guards.size(); i++) {
            emitters[i]->getBuilder()->CreateBr(llvm_entry_blocks[b->idx]);
            delete emitters[i];
        }
    }

    for (int i = 0; i < source->cfg->blocks.size(); i++) {
        if (ending_symbol_tables[i] == NULL)
            continue;

        for (SymbolTable::iterator it = ending_symbol_tables[i]->begin(); it != ending_symbol_tables[i]->end(); it++) {
            it->second->decvrefNodrop();
        }
        for (ConcreteSymbolTable::iterator it = phi_ending_symbol_tables[i]->begin(); it != phi_ending_symbol_tables[i]->end(); it++) {
            it->second->decvrefNodrop();
        }
        delete phi_ending_symbol_tables[i];
        delete ending_symbol_tables[i];
        delete created_phis[i];
    }

    if (entry_descriptor) {
        for (ConcreteSymbolTable::iterator it = osr_syms->begin(), end = osr_syms->end(); it != end; ++it) {
            delete it->second;
        }
        delete osr_syms;
    }
}

static void computeBlockSetClosure(BlockSet &full_blocks, BlockSet &partial_blocks) {
    if (VERBOSITY("irgen") >= 1) {
        printf("Initial full:");
        for (BlockSet::iterator it = full_blocks.begin(), end = full_blocks.end(); it != end; ++it) {
            printf(" %d", (*it)->idx);
        }
        printf("\n");
        printf("Initial partial:");
        for (BlockSet::iterator it = partial_blocks.begin(), end = partial_blocks.end(); it != end; ++it) {
            printf(" %d", (*it)->idx);
        }
        printf("\n");
    }
    std::vector<CFGBlock*> q;
    BlockSet expanded;
    q.insert(q.end(), full_blocks.begin(), full_blocks.end());
    q.insert(q.end(), partial_blocks.begin(), partial_blocks.end());

    while (q.size()) {
        CFGBlock *b = q.back();
        q.pop_back();

        if (expanded.count(b))
            continue;
        expanded.insert(b);

        for (int i = 0; i < b->successors.size(); i++) {
            CFGBlock *b2 = b->successors[i];
            partial_blocks.erase(b2);
            full_blocks.insert(b2);
            q.push_back(b2);
        }
    }

    if (VERBOSITY("irgen") >= 1) {
        printf("Ending full:");
        for (BlockSet::iterator it = full_blocks.begin(), end = full_blocks.end(); it != end; ++it) {
            printf(" %d", (*it)->idx);
        }
        printf("\n");
        printf("Ending partial:");
        for (BlockSet::iterator it = partial_blocks.begin(), end = partial_blocks.end(); it != end; ++it) {
            printf(" %d", (*it)->idx);
        }
        printf("\n");
    }
}
// returns a pointer to the function-info mdnode
static llvm::MDNode* setupDebugInfo(SourceInfo *source, llvm::Function *f, std::string origname) {
    int lineno = 0;
    if (source->ast)
        lineno = source->ast->lineno;

    llvm::DIBuilder builder(*g.cur_module);

    std::string fn = source->parent_module->fn;
    std::string dir = "TODO fill this in";
    std::string producer = "pyston; git rev " STRINGIFY(GITREV);

    llvm::DIFile file = builder.createFile(fn, dir);
    llvm::DIArray param_types = builder.getOrCreateArray(llvm::None);
    llvm::DICompositeType func_type = builder.createSubroutineType(file, param_types);
    llvm::DISubprogram func_info = builder.createFunction(file, f->getName(), f->getName(), file, lineno, func_type, false, true, lineno + 1, 0, true, f);
    llvm::DICompileUnit compile_unit = builder.createCompileUnit(llvm::dwarf::DW_LANG_Python, fn, dir, producer, true, "", 0);

    llvm::DIArray subprograms = builder.getOrCreateArray(&*func_info);
    compile_unit.getSubprograms()->replaceAllUsesWith(subprograms);

    compile_unit.getEnumTypes()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    compile_unit.getRetainedTypes()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    compile_unit.getGlobalVariables()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    compile_unit.getImportedEntities()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    return func_info;
}

static std::string getUniqueFunctionName(std::string nameprefix, EffortLevel::EffortLevel effort, const OSREntryDescriptor *entry) {
    static int num_functions = 0;

    std::ostringstream os;
    os << nameprefix;
    os << "_e" << effort;
    if (entry) {
        os << "_osr" << entry->backedge->target->idx << "_from_" << entry->cf->func->getName().data();
    }
    os << '_' << num_functions;
    num_functions++;
    return os.str();
}

CompiledFunction* compileFunction(SourceInfo *source, const OSREntryDescriptor *entry_descriptor, EffortLevel::EffortLevel effort, FunctionSignature *sig, const std::vector<AST_expr*> &arg_names, std::string nameprefix) {
    Timer _t("in compileFunction");

    if (VERBOSITY("irgen") >= 1) source->cfg->print();

    assert(g.cur_module == NULL);
    std::string name = getUniqueFunctionName(nameprefix, effort, entry_descriptor);
    g.cur_module = new llvm::Module(name, g.context);
    g.cur_module->setDataLayout(g.tm->getDataLayout()->getStringRepresentation());
    //g.engine->addModule(g.cur_module);

    ////
    // Initializing the llvm-level structures:

    int nargs = arg_names.size();
    ASSERT(nargs == sig->arg_types.size(), "%d %ld", nargs, sig->arg_types.size());

    std::vector<llvm::Type*> llvm_arg_types;
    if (entry_descriptor == NULL) {
        for (int i = 0; i < nargs; i++) {
            if (i == 3) {
                llvm_arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                break;
            }
            llvm_arg_types.push_back(sig->arg_types[i]->llvmType());
        }
    } else {
        int i = 0;
        for (OSREntryDescriptor::ArgMap::const_iterator it = entry_descriptor->args.begin(), end = entry_descriptor->args.end(); it != end; ++it, ++i) {
            //printf("Loading %s: %s\n", it->first.c_str(), it->second->debugName().c_str());
            if (i < 3)
                llvm_arg_types.push_back(it->second->llvmType());
            else {
                llvm_arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                break;
            }
        }
    }

    llvm::FunctionType *ft = llvm::FunctionType::get(sig->rtn_type->llvmType(), llvm_arg_types, false /*vararg*/);

    llvm::Function *f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, g.cur_module);
    //g.func_registry.registerFunction(f, g.cur_module);

    CompiledFunction *cf = new CompiledFunction(f, sig, (effort == EffortLevel::INTERPRETED), NULL, NULL, effort, entry_descriptor);

    llvm::MDNode* dbg_funcinfo = setupDebugInfo(source, f, nameprefix);

    TypeAnalysis::SpeculationLevel speculation_level = TypeAnalysis::NONE;
    if (ENABLE_SPECULATION && effort >= EffortLevel::MODERATE)
        speculation_level = TypeAnalysis::SOME;
    TypeAnalysis *types = doTypeAnalysis(source->cfg, arg_names, sig->arg_types, speculation_level, source->scoping->getScopeInfoForNode(source->ast));

    GuardList guards;

    BlockSet full_blocks, partial_blocks;
    if (entry_descriptor == NULL) {
        for (int i = 0; i < source->cfg->blocks.size(); i++) {
            full_blocks.insert(source->cfg->blocks[i]);
        }
    } else {
        full_blocks.insert(entry_descriptor->backedge->target);
        computeBlockSetClosure(full_blocks, partial_blocks);
    }

    IRGenState irstate(cf, source, getGCBuilder(), dbg_funcinfo);

    emitBBs(&irstate, "opt", guards, GuardList(), types, arg_names, entry_descriptor, full_blocks, partial_blocks);

    // De-opt handling:

    if (!guards.isEmpty()) {
        BlockSet deopt_full_blocks, deopt_partial_blocks;
        GuardList deopt_guards;
        //typedef std::unordered_map<CFGBlock*, std::unordered_map<AST_expr*, GuardList::ExprTypeGuard*> > Worklist;
        //Worklist guard_worklist;
        for (GuardList::expr_type_guard_iterator it = guards.after_begin(), end = guards.after_end(); it != end; ++it) {
            deopt_partial_blocks.insert(it->second->cfg_block);
        }

        computeBlockSetClosure(deopt_full_blocks, deopt_partial_blocks);

        TypeAnalysis *deopt_types = doTypeAnalysis(source->cfg, arg_names, sig->arg_types, TypeAnalysis::NONE, source->scoping->getScopeInfoForNode(source->ast));
        emitBBs(&irstate, "deopt", deopt_guards, guards, deopt_types, arg_names, NULL, deopt_full_blocks, deopt_partial_blocks);
        assert(deopt_guards.isEmpty());

        delete deopt_types;
    }

    for (GuardList::expr_type_guard_iterator it = guards.after_begin(), end = guards.after_end(); it != end; ++it) {
        delete it->second;
    }

    delete types;

    if (VERBOSITY("irgen") >= 1) {
        printf("generated IR:\n");
        printf("\033[33m");
        fflush(stdout);
        dumpPrettyIR(f);
        //f->dump();
        //g.cur_module->dump();
        //g.cur_module->print(llvm::outs(), NULL);
        printf("\033[0m");
        fflush(stdout);
    } else {
        // Somehow, running this code makes it faster...?????
        //printf("\033[0m");
        //fflush(stdout);
    }

#ifndef NDEBUG
    if (!BENCH) {
        // Calling verifyFunction() confuses the profiler, which will end up attributing
        // a large amount of runtime to it since the call stack looks very similar to
        // the (expensive) case of compiling the function.
        llvm::verifyFunction(*f);
    }
#endif

    long us = _t.end();
    static StatCounter us_irgen("us_compiling_irgen");
    us_irgen.log(us);

    if (ENABLE_LLVMOPTS)
        optimizeIR(f, effort);

    bool ENABLE_IR_DEBUG = false;
    if (ENABLE_IR_DEBUG) {
        addIRDebugSymbols(f);
        //dumpPrettyIR(f);
    }

    g.cur_module = NULL;

    return cf;
}



} // namespace pyston
