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

#include "codegen/irgen.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdint.h>

#include "llvm/Analysis/Passes.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/PassManager.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Scalar.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "analysis/type_analysis.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/gcbuilder.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/opt/escape_analysis.h"
#include "codegen/opt/inliner.h"
#include "codegen/opt/passes.h"
#include "codegen/osrentry.h"
#include "codegen/patchpoints.h"
#include "codegen/stackmaps.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

typedef std::unordered_set<CFGBlock*> BlockSet;

// This is where you can add a hook for any instruction added through the IRBuilder.
// It's currently not doing any hooking; hopefully there's not too much overhead from this.
void MyInserter::InsertHelper(llvm::Instruction* I, const llvm::Twine& Name, llvm::BasicBlock* BB,
                              llvm::BasicBlock::iterator InsertPt) const {
    llvm::IRBuilderDefaultInserter<true>::InsertHelper(I, Name, BB, InsertPt);
}

static void addIRDebugSymbols(llvm::Function* f) {
    llvm::legacy::PassManager mpm;

    llvm_error_code code = llvm::sys::fs::create_directory(".debug_ir", true);
    assert(!code);

    mpm.add(llvm::createDebugIRPass(false, false, ".debug_ir", f->getName()));

    mpm.run(*g.cur_module);
}

static void optimizeIR(llvm::Function* f, EffortLevel::EffortLevel effort) {
    // TODO maybe should do some simple passes (ex: gvn?) if effort level isn't maximal?
    // In general, this function needs a lot of tuning.
    if (effort < EffortLevel::MAXIMAL)
        return;

    Timer _t("optimizing");

    llvm::FunctionPassManager fpm(g.cur_module);

#if LLVMREV < 217548
    fpm.add(new llvm::DataLayoutPass(*g.tm->getDataLayout()));
#else
    fpm.add(new llvm::DataLayoutPass());
#endif

    if (ENABLE_INLINING && effort >= EffortLevel::MAXIMAL)
        fpm.add(makeFPInliner(275));
    fpm.add(llvm::createCFGSimplificationPass());

    fpm.add(llvm::createBasicAliasAnalysisPass());
    fpm.add(llvm::createTypeBasedAliasAnalysisPass());
    if (ENABLE_PYSTON_PASSES) {
        fpm.add(new EscapeAnalysis());
        fpm.add(createPystonAAPass());
    }

    if (ENABLE_PYSTON_PASSES)
        fpm.add(createMallocsNonNullPass());

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
        fpm.add(llvm::createEarlyCSEPass());                   // Catch trivial redundancies
        fpm.add(llvm::createJumpThreadingPass());              // Thread jumps.
        fpm.add(llvm::createCorrelatedValuePropagationPass()); // Propagate conditionals
        fpm.add(llvm::createCFGSimplificationPass());          // Merge & remove BBs
        fpm.add(llvm::createInstructionCombiningPass());       // Combine silly seq's

        fpm.add(llvm::createTailCallEliminationPass()); // Eliminate tail calls
        fpm.add(llvm::createCFGSimplificationPass());   // Merge & remove BBs
        fpm.add(llvm::createReassociatePass());         // Reassociate expressions
        fpm.add(llvm::createLoopRotatePass());          // Rotate Loop
        fpm.add(llvm::createLICMPass());                // Hoist loop invariants
        fpm.add(llvm::createLoopUnswitchPass(true /*optimize_for_size*/));
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createIndVarSimplifyPass()); // Canonicalize indvars
        fpm.add(llvm::createLoopIdiomPass());      // Recognize idioms like memset.
        fpm.add(llvm::createLoopDeletionPass());   // Delete dead loops

        fpm.add(llvm::createLoopUnrollPass()); // Unroll small loops

        fpm.add(llvm::createGVNPass());       // Remove redundancies
        fpm.add(llvm::createMemCpyOptPass()); // Remove memcpy / form memset
        fpm.add(llvm::createSCCPPass());      // Constant prop with SCCP

        // Run instcombine after redundancy elimination to exploit opportunities
        // opened up by them.
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createJumpThreadingPass()); // Thread jumps
        fpm.add(llvm::createCorrelatedValuePropagationPass());
        fpm.add(llvm::createDeadStoreEliminationPass()); // Delete dead stores

        fpm.add(llvm::createLoopRerollPass());
        // fpm.add(llvm::createSLPVectorizerPass());   // Vectorize parallel scalar chains.


        fpm.add(llvm::createAggressiveDCEPass());        // Delete dead instructions
        fpm.add(llvm::createCFGSimplificationPass());    // Merge & remove BBs
        fpm.add(llvm::createInstructionCombiningPass()); // Clean up after everything.

        // fpm.add(llvm::createBarrierNoopPass());
        // fpm.add(llvm::createLoopVectorizePass(DisableUnrollLoops, LoopVectorize));
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
        // fpm.add(llvm::createSCCPPass());                  // Constant prop with SCCP
        // fpm.add(llvm::createEarlyCSEPass());              // Catch trivial redundancies
        // fpm.add(llvm::createInstructionCombiningPass());
        // fpm.add(llvm::createCFGSimplificationPass());
    }

    fpm.doInitialization();

    for (int i = 0; i < MAX_OPT_ITERATIONS; i++) {
        bool changed = fpm.run(*f);

        if (!changed) {
            if (VERBOSITY("irgen"))
                printf("done after %d optimization iterations\n", i - 1);
            break;
        }

        if (VERBOSITY("irgen") >= 1) {
            fprintf(stderr, "after optimization %d:\n", i);
            printf("\033[36m");
            fflush(stdout);
            dumpPrettyIR(f);
            // f->dump();
            // g.cur_module->dump();
            printf("\033[0m");
            fflush(stdout);
        }
    }

    long us = _t.end();
    static StatCounter us_optimizing("us_compiling_optimizing");
    us_optimizing.log(us);
}

static bool compareBlockPairs(const std::pair<CFGBlock*, CFGBlock*>& p1, const std::pair<CFGBlock*, CFGBlock*>& p2) {
    return p1.first->idx < p2.first->idx;
}

static std::vector<std::pair<CFGBlock*, CFGBlock*> >
computeBlockTraversalOrder(const BlockSet& full_blocks, const BlockSet& partial_blocks, CFGBlock* start) {

    std::vector<std::pair<CFGBlock*, CFGBlock*> > rtn;
    std::unordered_set<CFGBlock*> in_queue;

    if (start) {
        assert(full_blocks.count(start));
        in_queue.insert(start);
        rtn.push_back(std::make_pair(start, (CFGBlock*)NULL));
    }

    for (CFGBlock* b : partial_blocks) {
        in_queue.insert(b);
        rtn.push_back(std::make_pair(b, (CFGBlock*)NULL));
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
            CFGBlock* cur = rtn[idx].first;

            for (int i = 0; i < cur->successors.size(); i++) {
                CFGBlock* b = cur->successors[i];
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

        CFGBlock* best = NULL;
        for (CFGBlock* b : full_blocks) {
            if (in_queue.count(b))
                continue;

            // Avoid picking any blocks where we can't add an epilogue to the predecessors
            if (b->predecessors.size() == 1 && b->predecessors[0]->successors.size() > 1)
                continue;

            if (best == NULL || b->idx < best->idx)
                best = b;
        }
        assert(best != NULL);

        if (VERBOSITY("irgen") >= 1)
            printf("Giving up and adding block %d to the order\n", best->idx);
        in_queue.insert(best);
        rtn.push_back(std::make_pair(best, (CFGBlock*)NULL));
    }

    ASSERT(rtn.size() == full_blocks.size() + partial_blocks.size(), "%ld\n", rtn.size());
    return rtn;
}

static ConcreteCompilerType* getTypeAtBlockStart(TypeAnalysis* types, const std::string& name, CFGBlock* block) {
    if (startswith(name, "!is_defined"))
        return BOOL;
    else if (name == PASSED_GENERATOR_NAME)
        return GENERATOR;
    else if (name == PASSED_CLOSURE_NAME)
        return CLOSURE;
    else
        return types->getTypeAtBlockStart(name, block);
}

static void emitBBs(IRGenState* irstate, const char* bb_type, GuardList& out_guards, const GuardList& in_guards,
                    TypeAnalysis* types, const OSREntryDescriptor* entry_descriptor, const BlockSet& full_blocks,
                    const BlockSet& partial_blocks) {
    SourceInfo* source = irstate->getSourceInfo();
    EffortLevel::EffortLevel effort = irstate->getEffortLevel();
    CompiledFunction* cf = irstate->getCurFunction();
    ConcreteCompilerType* rtn_type = irstate->getReturnType();
    // llvm::MDNode* func_info = irstate->getFuncDbgInfo();

    if (entry_descriptor != NULL)
        assert(full_blocks.count(source->cfg->getStartingBlock()) == 0);

    // We need the entry blocks pre-allocated so that we can jump forward to them.
    std::unordered_map<CFGBlock*, llvm::BasicBlock*> llvm_entry_blocks;
    for (CFGBlock* block : source->cfg->blocks) {
        if (partial_blocks.count(block) == 0 && full_blocks.count(block) == 0) {
            llvm_entry_blocks[block] = NULL;
            continue;
        }

        char buf[40];
        snprintf(buf, 40, "%s_block%d", bb_type, block->idx);
        llvm_entry_blocks[block] = llvm::BasicBlock::Create(g.context, buf, irstate->getLLVMFunction());
    }

    llvm::BasicBlock* osr_entry_block = NULL; // the function entry block, where we add the type guards
    llvm::BasicBlock* osr_unbox_block = NULL; // the block after type guards where we up/down-convert things
    ConcreteSymbolTable* osr_syms = NULL;     // syms after conversion
    if (entry_descriptor != NULL) {
        osr_unbox_block = llvm::BasicBlock::Create(g.context, "osr_unbox", irstate->getLLVMFunction(),
                                                   &irstate->getLLVMFunction()->getEntryBlock());
        osr_entry_block = llvm::BasicBlock::Create(g.context, "osr_entry", irstate->getLLVMFunction(),
                                                   &irstate->getLLVMFunction()->getEntryBlock());
        assert(&irstate->getLLVMFunction()->getEntryBlock() == osr_entry_block);

        osr_syms = new ConcreteSymbolTable();
        SymbolTable* initial_syms = new SymbolTable();
        // llvm::BranchInst::Create(llvm_entry_blocks[entry_descriptor->backedge->target->idx], entry_block);

        llvm::BasicBlock* osr_entry_block_end = osr_entry_block;
        llvm::BasicBlock* osr_unbox_block_end = osr_unbox_block;
        std::unique_ptr<IREmitter> entry_emitter(createIREmitter(irstate, osr_entry_block_end));
        std::unique_ptr<IREmitter> unbox_emitter(createIREmitter(irstate, osr_unbox_block_end));

        CFGBlock* target_block = entry_descriptor->backedge->target;

        // Currently we AND all the type guards together and then do just a single jump;
        // guard_val is the current AND'd value, or NULL if there weren't any guards
        llvm::Value* guard_val = NULL;

        std::vector<llvm::Value*> func_args;
        for (llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin();
             AI != irstate->getLLVMFunction()->arg_end(); AI++) {
            func_args.push_back(AI);
        }

        // Handle loading symbols from the passed osr arguments:
        int arg_num = -1;
        for (const auto& p : entry_descriptor->args) {
            llvm::Value* from_arg;
            arg_num++;
            if (arg_num < 3) {
                from_arg = func_args[arg_num];
#ifndef NDEBUG
                if (from_arg->getType() != p.second->llvmType()) {
                    from_arg->getType()->dump();
                    printf("\n");
                    p.second->llvmType()->dump();
                    printf("\n");
                }
#endif
                assert(from_arg->getType() == p.second->llvmType());
            } else {
                ASSERT(func_args.size() == 4, "%ld", func_args.size());
                llvm::Value* ptr = entry_emitter->getBuilder()->CreateConstGEP1_32(func_args[3], arg_num - 3);
                if (p.second == INT) {
                    ptr = entry_emitter->getBuilder()->CreateBitCast(ptr, g.i64->getPointerTo());
                } else if (p.second == BOOL) {
                    ptr = entry_emitter->getBuilder()->CreateBitCast(ptr, BOOL->llvmType()->getPointerTo());
                } else if (p.second == FLOAT) {
                    ptr = entry_emitter->getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                } else {
                    assert(p.second->llvmType() == g.llvm_value_type_ptr);
                }
                from_arg = entry_emitter->getBuilder()->CreateLoad(ptr);
                assert(from_arg->getType() == p.second->llvmType());
            }

            ConcreteCompilerType* phi_type;
            phi_type = getTypeAtBlockStart(types, p.first, target_block);

            ConcreteCompilerVariable* var = new ConcreteCompilerVariable(p.second, from_arg, true);
            (*initial_syms)[p.first] = var;

            // It's possible to OSR into a version of the function with a higher speculation level;
            // this means that the types of the OSR variables are potentially higher (more unspecialized)
            // than what the optimized code expects.
            // So, we have to re-check the speculations and potentially deopt.
            llvm::Value* v = NULL;
            if (p.second == phi_type) {
                // good to go
                v = from_arg;
            } else if (p.second->canConvertTo(phi_type)) {
                // not sure if/when this happens, but if there's a type mismatch but one we know
                // can be handled (such as casting from a subclass to a superclass), handle it:
                ConcreteCompilerVariable* converted = var->makeConverted(*unbox_emitter, phi_type);
                v = converted->getValue();
                delete converted;
            } else {
                ASSERT(p.second == UNKNOWN, "%s", p.second->debugName().c_str());
                BoxedClass* speculated_class = NULL;
                if (phi_type == INT) {
                    speculated_class = int_cls;
                } else if (phi_type == FLOAT) {
                    speculated_class = float_cls;
                } else {
                    speculated_class = phi_type->guaranteedClass();
                }
                ASSERT(speculated_class, "%s", phi_type->debugName().c_str());

                ASSERT(entry_descriptor->args.count("!is_defined_" + p.first) == 0,
                       "This class-check-creating behavior will segfault if the argument wasn't actually defined!");

                llvm::Value* type_check = ConcreteCompilerVariable(p.second, from_arg, true)
                                              .makeClassCheck(*entry_emitter, speculated_class);
                if (guard_val) {
                    guard_val = entry_emitter->getBuilder()->CreateAnd(guard_val, type_check);
                } else {
                    guard_val = type_check;
                }
                // entry_emitter->getBuilder()->CreateCall(g.funcs.my_assert, type_check);

                if (speculated_class == int_cls) {
                    v = unbox_emitter->getBuilder()->CreateCall(g.funcs.unboxInt, from_arg);
                    (new ConcreteCompilerVariable(BOXED_INT, from_arg, true))->decvref(*unbox_emitter);
                } else if (speculated_class == float_cls) {
                    v = unbox_emitter->getBuilder()->CreateCall(g.funcs.unboxFloat, from_arg);
                    (new ConcreteCompilerVariable(BOXED_FLOAT, from_arg, true))->decvref(*unbox_emitter);
                } else {
                    assert(phi_type == typeFromClass(speculated_class));
                    v = from_arg;
                }
            }

            if (VERBOSITY("irgen"))
                v->setName("prev_" + p.first);

            (*osr_syms)[p.first] = new ConcreteCompilerVariable(phi_type, v, true);
        }

        if (guard_val) {
            // Create the guard with both branches leading to the success_bb,
            // and let the deopt path change the failure case to point to the
            // as-yet-unknown deopt block.
            // TODO Not the best approach since if we fail to do that patching,
            // the guard will just silently be ignored.
            llvm::BranchInst* br
                = entry_emitter->getBuilder()->CreateCondBr(guard_val, osr_unbox_block, osr_unbox_block);
            out_guards.registerGuardForBlockEntry(target_block, br, *initial_syms);
        } else {
            entry_emitter->getBuilder()->CreateBr(osr_unbox_block);
        }
        unbox_emitter->getBuilder()->CreateBr(llvm_entry_blocks[entry_descriptor->backedge->target]);

        for (const auto& p : *initial_syms) {
            delete p.second;
        }
        delete initial_syms;
    }

    // In a similar vein, we need to keep track of the exit blocks for each cfg block,
    // so that we can construct phi nodes later.
    // Originally I preallocated these blocks as well, but we can construct the phi's
    // after the fact, so we can just record the exit blocks as we go along.
    std::unordered_map<CFGBlock*, llvm::BasicBlock*> llvm_exit_blocks;

    ////
    // Main ir generation: go through each basic block in the CFG and emit the code

    std::unordered_map<CFGBlock*, SymbolTable*> ending_symbol_tables;
    std::unordered_map<CFGBlock*, ConcreteSymbolTable*> phi_ending_symbol_tables;
    typedef std::unordered_map<std::string, std::pair<ConcreteCompilerType*, llvm::PHINode*> > PHITable;
    std::unordered_map<CFGBlock*, PHITable*> created_phis;

    CFGBlock* initial_block = NULL;
    if (entry_descriptor) {
        initial_block = entry_descriptor->backedge->target;
    } else if (full_blocks.count(source->cfg->getStartingBlock())) {
        initial_block = source->cfg->getStartingBlock();
    }

    // The rest of this code assumes that for each non-entry block that gets evaluated,
    // at least one of its predecessors has been evaluated already (from which it will
    // get type information).
    // The cfg generation code will generate a cfg such that each block has a predecessor
    // with a lower index value, so if the entry block is 0 then we can iterate in index
    // order.
    // The entry block doesn't have to be zero, so we have to calculate an allowable order here:
    std::vector<std::pair<CFGBlock*, CFGBlock*> > traversal_order
        = computeBlockTraversalOrder(full_blocks, partial_blocks, initial_block);

    std::unordered_set<CFGBlock*> into_hax;
    for (int _i = 0; _i < traversal_order.size(); _i++) {
        CFGBlock* block = traversal_order[_i].first;
        CFGBlock* pred = traversal_order[_i].second;

        if (VERBOSITY("irgen") >= 1)
            printf("processing %s block %d\n", bb_type, block->idx);

        bool is_partial = false;
        if (partial_blocks.count(block)) {
            if (VERBOSITY("irgen") >= 1)
                printf("is partial block\n");
            is_partial = true;
        } else if (!full_blocks.count(block)) {
            if (VERBOSITY("irgen") >= 1)
                printf("Skipping this block\n");
            // created_phis[block] = NULL;
            // ending_symbol_tables[block] = NULL;
            // phi_ending_symbol_tables[block] = NULL;
            // llvm_exit_blocks[block] = NULL;
            continue;
        }

        std::unique_ptr<IRGenerator> generator(
            createIRGenerator(irstate, llvm_entry_blocks, block, types, out_guards, in_guards, is_partial));
        llvm::BasicBlock* entry_block_end = llvm_entry_blocks[block];
        std::unique_ptr<IREmitter> emitter(createIREmitter(irstate, entry_block_end));

        PHITable* phis = NULL;
        if (!is_partial) {
            phis = new PHITable();
            created_phis[block] = phis;
        }

        // Set initial symbol table:
        if (is_partial) {
            // pass
        } else if (block == source->cfg->getStartingBlock()) {
            assert(entry_descriptor == NULL);
            // number of times a function needs to be called to be reoptimized:
            static const int REOPT_THRESHOLDS[] = {
                10,    // INTERPRETED->MINIMAL
                250,   // MINIMAL->MODERATE
                10000, // MODERATE->MAXIMAL
            };

            assert(strcmp("opt", bb_type) == 0);

            if (ENABLE_REOPT && effort < EffortLevel::MAXIMAL && source->ast != NULL
                && source->ast->type != AST_TYPE::Module) {
                llvm::BasicBlock* preentry_bb
                    = llvm::BasicBlock::Create(g.context, "pre_entry", irstate->getLLVMFunction(),
                                               llvm_entry_blocks[source->cfg->getStartingBlock()]);
                llvm::BasicBlock* reopt_bb = llvm::BasicBlock::Create(g.context, "reopt", irstate->getLLVMFunction());
                emitter->getBuilder()->SetInsertPoint(preentry_bb);

                llvm::Value* call_count_ptr = embedConstantPtr(&cf->times_called, g.i64->getPointerTo());
                llvm::Value* cur_call_count = emitter->getBuilder()->CreateLoad(call_count_ptr);
                llvm::Value* new_call_count
                    = emitter->getBuilder()->CreateAdd(cur_call_count, getConstantInt(1, g.i64));
                emitter->getBuilder()->CreateStore(new_call_count, call_count_ptr);
                llvm::Value* reopt_test = emitter->getBuilder()->CreateICmpSGT(
                    new_call_count, getConstantInt(REOPT_THRESHOLDS[effort], g.i64));

                llvm::Value* md_vals[]
                    = { llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1), getConstantInt(1000) };
                llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));

                llvm::BranchInst* guard = emitter->getBuilder()->CreateCondBr(
                    reopt_test, reopt_bb, llvm_entry_blocks[source->cfg->getStartingBlock()], branch_weights);

                emitter->getBuilder()->SetInsertPoint(reopt_bb);
                // emitter->getBuilder()->CreateCall(g.funcs.my_assert, getConstantInt(0, g.i1));
                llvm::Value* r = emitter->getBuilder()->CreateCall(g.funcs.reoptCompiledFunc,
                                                                   embedConstantPtr(cf, g.i8->getPointerTo()));
                assert(r);
                assert(r->getType() == g.i8->getPointerTo());

                llvm::Value* bitcast_r = emitter->getBuilder()->CreateBitCast(r, irstate->getLLVMFunction()->getType());

                std::vector<llvm::Value*> args;
                // bitcast_r->dump();
                for (llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin();
                     AI != irstate->getLLVMFunction()->arg_end(); AI++) {
                    // AI->dump();
                    args.push_back(&(*AI));
                }
                // printf("%ld\n", args.size());
                llvm::CallInst* postcall = emitter->getBuilder()->CreateCall(bitcast_r, args);
                postcall->setTailCall(true);
                if (rtn_type == VOID) {
                    emitter->getBuilder()->CreateRetVoid();
                } else {
                    emitter->getBuilder()->CreateRet(postcall);
                }

                emitter->getBuilder()->SetInsertPoint(llvm_entry_blocks[source->cfg->getStartingBlock()]);
            }

            generator->doFunctionEntry(source->arg_names, cf->spec->arg_types);

            // Function-entry safepoint:
            // TODO might be more efficient to do post-call safepoints?
            generator->doSafePoint();
        } else if (entry_descriptor && block == entry_descriptor->backedge->target) {
            assert(block->predecessors.size() > 1);
            assert(osr_entry_block);
            assert(phis);

            for (const auto& p : entry_descriptor->args) {
                ConcreteCompilerType* analyzed_type = getTypeAtBlockStart(types, p.first, block);

                // printf("For %s, given %s, analyzed for %s\n", p.first.c_str(), p.second->debugName().c_str(),
                // analyzed_type->debugName().c_str());

                llvm::PHINode* phi = emitter->getBuilder()->CreatePHI(analyzed_type->llvmType(),
                                                                      block->predecessors.size() + 1, p.first);
                ConcreteCompilerVariable* var = new ConcreteCompilerVariable(analyzed_type, phi, true);
                generator->giveLocalSymbol(p.first, var);
                (*phis)[p.first] = std::make_pair(analyzed_type, phi);
            }
        } else if (pred == NULL) {
            assert(traversal_order.size() < source->cfg->blocks.size());
            assert(phis);
            assert(block->predecessors.size());
            for (int i = 0; i < block->predecessors.size(); i++) {
                CFGBlock* b2 = block->predecessors[i];
                assert(ending_symbol_tables.count(b2) == 0);
                into_hax.insert(b2);
            }

            const PhiAnalysis::RequiredSet& names = source->phis->getAllRequiredFor(block);
            for (const auto& s : names) {
                // printf("adding guessed phi for %s\n", s.c_str());
                ConcreteCompilerType* type = types->getTypeAtBlockStart(s, block);
                llvm::PHINode* phi = emitter->getBuilder()->CreatePHI(type->llvmType(), block->predecessors.size(), s);
                ConcreteCompilerVariable* var = new ConcreteCompilerVariable(type, phi, true);
                generator->giveLocalSymbol(s, var);

                (*phis)[s] = std::make_pair(type, phi);

                if (source->phis->isPotentiallyUndefinedAfter(s, block->predecessors[0])) {
                    std::string is_defined_name = "!is_defined_" + s;
                    llvm::PHINode* phi = emitter->getBuilder()->CreatePHI(BOOL->llvmType(), block->predecessors.size(),
                                                                          is_defined_name);
                    ConcreteCompilerVariable* var = new ConcreteCompilerVariable(BOOL, phi, true);
                    generator->giveLocalSymbol(is_defined_name, var);

                    (*phis)[is_defined_name] = std::make_pair(BOOL, phi);
                }
            }
        } else {
            assert(pred);
            assert(full_blocks.count(pred) || partial_blocks.count(pred));

            if (block->predecessors.size() == 1) {
                // If this block has only one predecessor, it by definition doesn't need any phi nodes.
                // Assert that the phi_st is empty, and just create the symbol table from the non-phi st:
                ASSERT(phi_ending_symbol_tables[pred]->size() == 0, "%d %d", block->idx, pred->idx);
                assert(ending_symbol_tables.count(pred));

                // Filter out any names set by an invoke statement at the end
                // of the previous block, if we're in the unwind path.
                // This definitely doesn't seem like the most elegant way to do this,
                // but the rest of the analysis frameworks can't (yet) support the idea of
                // a block flowing differently to its different predecessors.
                auto pred = block->predecessors[0];
                auto last_inst = pred->body.back();

                SymbolTable* sym_table = ending_symbol_tables[pred];
                bool created_new_sym_table = false;
                if (last_inst->type == AST_TYPE::Invoke) {
                    auto invoke = ast_cast<AST_Invoke>(last_inst);
                    if (invoke->exc_dest == block && invoke->stmt->type == AST_TYPE::Assign) {
                        auto asgn = ast_cast<AST_Assign>(invoke->stmt);
                        assert(asgn->targets.size() == 1);
                        if (asgn->targets[0]->type == AST_TYPE::Name) {
                            auto name = ast_cast<AST_Name>(asgn->targets[0]);

                            // TODO: inneficient
                            sym_table = new SymbolTable(*sym_table);
                            assert(sym_table->count(name->id));
                            sym_table->erase(name->id);
                            created_new_sym_table = true;
                        }
                    }
                }

                generator->copySymbolsFrom(sym_table);
                if (created_new_sym_table)
                    delete sym_table;
            } else {
                // With multiple predecessors, the symbol tables at the end of each predecessor should be *exactly* the
                // same.
                // (this should be satisfied by the post-run() code in this function)

                // With multiple predecessors, we have to combine the non-phi and phi symbol tables.
                // Start off with the non-phi ones:
                generator->copySymbolsFrom(ending_symbol_tables[pred]);

                // And go through and add phi nodes:
                ConcreteSymbolTable* pred_st = phi_ending_symbol_tables[pred];
                for (ConcreteSymbolTable::iterator it = pred_st->begin(); it != pred_st->end(); it++) {
                    // printf("adding phi for %s\n", it->first.c_str());
                    llvm::PHINode* phi = emitter->getBuilder()->CreatePHI(it->second->getType()->llvmType(),
                                                                          block->predecessors.size(), it->first);
                    // emitter->getBuilder()->CreateCall(g.funcs.dump, phi);
                    ConcreteCompilerVariable* var = new ConcreteCompilerVariable(it->second->getType(), phi, true);
                    generator->giveLocalSymbol(it->first, var);

                    (*phis)[it->first] = std::make_pair(it->second->getType(), phi);
                }
            }
        }

        for (CFGBlock* predecessor : block->predecessors) {
            if (predecessor->idx > block->idx) {
                // Loop safepoint:
                // TODO does it matter which side of the backedge these are on?
                generator->doSafePoint();
                break;
            }
        }

        generator->run(block);

        const IRGenerator::EndingState& ending_st = generator->getEndingSymbolTable();
        ending_symbol_tables[block] = ending_st.symbol_table;
        phi_ending_symbol_tables[block] = ending_st.phi_symbol_table;
        llvm_exit_blocks[block] = ending_st.ending_block;

        if (into_hax.count(block))
            ASSERT(ending_st.symbol_table->size() == 0, "%d", block->idx);
    }

    ////
    // Phi generation.
    // We don't know the exact ssa values to back-propagate to the phi nodes until we've generated
    // the relevant IR, so after we have done all of it, go back through and populate the phi nodes.
    // Also, do some checking to make sure that the phi analysis stuff worked out, and that all blocks
    // agreed on what symbols + types they should be propagating for the phis.
    for (CFGBlock* b : source->cfg->blocks) {
        PHITable* phis = created_phis[b];
        if (phis == NULL)
            continue;

        bool this_is_osr_entry = (entry_descriptor && b == entry_descriptor->backedge->target);

        const std::vector<GuardList::BlockEntryGuard*>& block_guards = in_guards.getGuardsForBlock(b);
        // printf("Found %ld guards for block %p, for %p\n", block_guards.size(), b, &in_guards);

        for (int j = 0; j < b->predecessors.size(); j++) {
            CFGBlock* b2 = b->predecessors[j];
            if (full_blocks.count(b2) == 0 && partial_blocks.count(b2) == 0)
                continue;

            // printf("%d %d %ld %ld\n", i, b2->idx, phi_ending_symbol_tables[b2]->size(), phis->size());
            compareKeyset(phi_ending_symbol_tables[b2], phis);
            assert(phi_ending_symbol_tables[b2]->size() == phis->size());
        }

        if (this_is_osr_entry) {
            compareKeyset(osr_syms, phis);
        }

        std::vector<IREmitter*> emitters;
        std::vector<llvm::BasicBlock*> offramps;
        for (int i = 0; i < block_guards.size(); i++) {
            compareKeyset(&block_guards[i]->symbol_table, phis);

            llvm::BasicBlock* off_ramp = llvm::BasicBlock::Create(g.context, "deopt_ramp", irstate->getLLVMFunction());
            offramps.push_back(off_ramp);
            llvm::BasicBlock* off_ramp_end = off_ramp;
            IREmitter* emitter = createIREmitter(irstate, off_ramp_end);
            emitters.push_back(emitter);

            block_guards[i]->branch->setSuccessor(1, off_ramp);
        }

        for (PHITable::iterator it = phis->begin(); it != phis->end(); it++) {
            llvm::PHINode* llvm_phi = it->second.second;
            for (int j = 0; j < b->predecessors.size(); j++) {
                CFGBlock* b2 = b->predecessors[j];
                if (full_blocks.count(b2) == 0 && partial_blocks.count(b2) == 0)
                    continue;

                ConcreteCompilerVariable* v = (*phi_ending_symbol_tables[b2])[it->first];
                assert(v);
                assert(v->isGrabbed());

                // Make sure they all prepared for the same type:
                ASSERT(it->second.first == v->getType(), "%d %d: %s %s %s", b->idx, b2->idx, it->first.c_str(),
                       it->second.first->debugName().c_str(), v->getType()->debugName().c_str());

                llvm_phi->addIncoming(v->getValue(), llvm_exit_blocks[b->predecessors[j]]);
            }

            if (this_is_osr_entry) {
                ConcreteCompilerVariable* v = (*osr_syms)[it->first];
                assert(v);
                assert(v->isGrabbed());

                ASSERT(it->second.first == v->getType(), "");
                llvm_phi->addIncoming(v->getValue(), osr_unbox_block);
            }

            for (int i = 0; i < block_guards.size(); i++) {
                GuardList::BlockEntryGuard* guard = block_guards[i];
                IREmitter* emitter = emitters[i];

                ASSERT(phis->count("!is_defined_" + it->first) == 0,
                       "This class-check-creating behavior will segfault if the argument wasn't actually defined!");

                CompilerVariable* unconverted = guard->symbol_table[it->first];
                ConcreteCompilerVariable* v;
                if (unconverted->canConvertTo(it->second.first)) {
                    v = unconverted->makeConverted(*emitter, it->second.first);
                    assert(v);
                    assert(v->isGrabbed());
                } else {
                    // This path is for handling the case that we did no type analysis in the previous tier,
                    // but in this tier we know that even in the deopt branch with no speculations, that
                    // the type is more refined than what we got from the previous tier.
                    //
                    // We're going to blindly assume that we're right about what the type should be.
                    assert(unconverted->getType() == UNKNOWN);
                    assert(strcmp(bb_type, "deopt") == 0);

                    ConcreteCompilerVariable* converted = unconverted->makeConverted(*emitter, UNKNOWN);

                    if (it->second.first->llvmType() == g.llvm_value_type_ptr) {
                        v = new ConcreteCompilerVariable(it->second.first, converted->getValue(), true);
                    } else if (it->second.first == FLOAT) {
                        llvm::Value* unboxed
                            = emitter->getBuilder()->CreateCall(g.funcs.unboxFloat, converted->getValue());
                        v = new ConcreteCompilerVariable(FLOAT, unboxed, true);
                    } else if (it->second.first == INT) {
                        llvm::Value* unboxed
                            = emitter->getBuilder()->CreateCall(g.funcs.unboxInt, converted->getValue());
                        v = new ConcreteCompilerVariable(INT, unboxed, true);
                    } else {
                        printf("%s\n", it->second.first->debugName().c_str());
                        abort();
                    }

                    converted->decvref(*emitter);

                    /*
                    if (speculated_class == int_cls) {
                        v = unbox_emitter->getBuilder()->CreateCall(g.funcs.unboxInt, from_arg);
                        (new ConcreteCompilerVariable(BOXED_INT, from_arg, true))->decvref(*unbox_emitter);
                    } else if (speculated_class == float_cls) {
                        v = unbox_emitter->getBuilder()->CreateCall(g.funcs.unboxFloat, from_arg);
                        (new ConcreteCompilerVariable(BOXED_FLOAT, from_arg, true))->decvref(*unbox_emitter);
                    } else {
                        assert(phi_type == typeFromClass(speculated_class));
                        v = from_arg;
                    }
                    */
                }


                ASSERT(it->second.first == v->getType(), "");
                assert(it->second.first->llvmType() == v->getValue()->getType());
                llvm_phi->addIncoming(v->getValue(), offramps[i]);

                // TODO not sure if this is right:
                unconverted->decvref(*emitter);
                delete v;
            }
        }

        for (int i = 0; i < block_guards.size(); i++) {
            emitters[i]->getBuilder()->CreateBr(llvm_entry_blocks[b]);
            delete emitters[i];
        }
    }

    for (CFGBlock* b : source->cfg->blocks) {
        if (ending_symbol_tables[b] == NULL)
            continue;

        for (SymbolTable::iterator it = ending_symbol_tables[b]->begin(); it != ending_symbol_tables[b]->end(); it++) {
            it->second->decvrefNodrop();
        }
        for (ConcreteSymbolTable::iterator it = phi_ending_symbol_tables[b]->begin();
             it != phi_ending_symbol_tables[b]->end(); it++) {
            it->second->decvrefNodrop();
        }
        delete phi_ending_symbol_tables[b];
        delete ending_symbol_tables[b];
        delete created_phis[b];
    }

    if (entry_descriptor) {
        for (const auto& p : *osr_syms) {
            delete p.second;
        }
        delete osr_syms;
    }
}

static void computeBlockSetClosure(BlockSet& full_blocks, BlockSet& partial_blocks) {
    if (VERBOSITY("irgen") >= 1) {
        printf("Initial full:");
        for (CFGBlock* b : full_blocks) {
            printf(" %d", b->idx);
        }
        printf("\n");
        printf("Initial partial:");
        for (CFGBlock* b : partial_blocks) {
            printf(" %d", b->idx);
        }
        printf("\n");
    }
    std::vector<CFGBlock*> q;
    BlockSet expanded;
    q.insert(q.end(), full_blocks.begin(), full_blocks.end());
    q.insert(q.end(), partial_blocks.begin(), partial_blocks.end());

    while (q.size()) {
        CFGBlock* b = q.back();
        q.pop_back();

        if (expanded.count(b))
            continue;
        expanded.insert(b);

        for (int i = 0; i < b->successors.size(); i++) {
            CFGBlock* b2 = b->successors[i];
            partial_blocks.erase(b2);
            full_blocks.insert(b2);
            q.push_back(b2);
        }
    }

    if (VERBOSITY("irgen") >= 1) {
        printf("Ending full:");
        for (CFGBlock* b : full_blocks) {
            printf(" %d", b->idx);
        }
        printf("\n");
        printf("Ending partial:");
        for (CFGBlock* b : partial_blocks) {
            printf(" %d", b->idx);
        }
        printf("\n");
    }
}
// returns a pointer to the function-info mdnode
static llvm::MDNode* setupDebugInfo(SourceInfo* source, llvm::Function* f, std::string origname) {
    int lineno = 0;
    if (source->ast)
        lineno = source->ast->lineno;

    llvm::DIBuilder builder(*g.cur_module);

    std::string fn = source->parent_module->fn;
    std::string dir = "";
    std::string producer = "pyston; git rev " STRINGIFY(GITREV);

    llvm::DIFile file = builder.createFile(fn, dir);
#if LLVMREV < 214132
    llvm::DIArray param_types = builder.getOrCreateArray(llvm::None);
#else
    llvm::DITypeArray param_types = builder.getOrCreateTypeArray(llvm::None);
#endif
    llvm::DICompositeType func_type = builder.createSubroutineType(file, param_types);
    llvm::DISubprogram func_info = builder.createFunction(file, f->getName(), f->getName(), file, lineno, func_type,
                                                          false, true, lineno + 1, 0, true, f);

    // The 'variables' field gets initialized with a tag-prefixed array, but
    // a later verifier asserts that there is no tag.  Replace it with an empty array:
    func_info.getVariables()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));

    llvm::DICompileUnit compile_unit
        = builder.createCompileUnit(llvm::dwarf::DW_LANG_Python, fn, dir, producer, true, "", 0);

    llvm::DIArray subprograms = builder.getOrCreateArray(&*func_info);
    compile_unit.getSubprograms()->replaceAllUsesWith(subprograms);

    compile_unit.getEnumTypes()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    compile_unit.getRetainedTypes()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    compile_unit.getGlobalVariables()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    compile_unit.getImportedEntities()->replaceAllUsesWith(builder.getOrCreateArray(llvm::ArrayRef<llvm::Value*>()));
    return func_info;
}

static std::string getUniqueFunctionName(std::string nameprefix, EffortLevel::EffortLevel effort,
                                         const OSREntryDescriptor* entry) {
    static int num_functions = 0;

    std::ostringstream os;
    os << nameprefix;
    os << "_e" << effort;
    if (entry) {
        os << "_osr" << entry->backedge->target->idx;
        if (entry->cf->func)
            os << "_from_" << entry->cf->func->getName().data();
    }
    os << '_' << num_functions;
    num_functions++;
    return os.str();
}

CompiledFunction* doCompile(SourceInfo* source, const OSREntryDescriptor* entry_descriptor,
                            EffortLevel::EffortLevel effort, FunctionSpecialization* spec, std::string nameprefix) {
    Timer _t("in doCompile");
    Timer _t2;
    long irgen_us = 0;

    if (VERBOSITY("irgen") >= 1)
        source->cfg->print();

    assert(g.cur_module == NULL);
    std::string name = getUniqueFunctionName(nameprefix, effort, entry_descriptor);
    g.cur_module = new llvm::Module(name, g.context);
#if LLVMREV < 217070 // not sure if this is the right rev
    g.cur_module->setDataLayout(g.tm->getDataLayout()->getStringRepresentation());
#else
    g.cur_module->setDataLayout(g.tm->getSubtargetImpl()->getDataLayout());
#endif
    // g.engine->addModule(g.cur_module);

    ////
    // Initializing the llvm-level structures:

    int nargs = source->arg_names.totalParameters();
    ASSERT(nargs == spec->arg_types.size(), "%d %ld", nargs, spec->arg_types.size());


    std::vector<llvm::Type*> llvm_arg_types;
    if (entry_descriptor == NULL) {
        if (source->getScopeInfo()->takesClosure())
            llvm_arg_types.push_back(g.llvm_closure_type_ptr);

        if (source->getScopeInfo()->takesGenerator())
            llvm_arg_types.push_back(g.llvm_generator_type_ptr);

        for (int i = 0; i < nargs; i++) {
            if (i == 3) {
                llvm_arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                break;
            }
            llvm_arg_types.push_back(spec->arg_types[i]->llvmType());
        }
    } else {
        int arg_num = -1;
        for (const auto& p : entry_descriptor->args) {
            arg_num++;
            // printf("Loading %s: %s\n", p.first.c_str(), p.second->debugName().c_str());
            if (arg_num < 3)
                llvm_arg_types.push_back(p.second->llvmType());
            else {
                llvm_arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                break;
            }
        }
    }

    llvm::FunctionType* ft = llvm::FunctionType::get(spec->rtn_type->llvmType(), llvm_arg_types, false /*vararg*/);

    llvm::Function* f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, g.cur_module);
    // g.func_registry.registerFunction(f, g.cur_module);

    CompiledFunction* cf
        = new CompiledFunction(f, spec, (effort == EffortLevel::INTERPRETED), NULL, NULL, effort, entry_descriptor);

    llvm::MDNode* dbg_funcinfo = setupDebugInfo(source, f, nameprefix);

    irgen_us += _t2.split();

    TypeAnalysis::SpeculationLevel speculation_level = TypeAnalysis::NONE;
    if (ENABLE_SPECULATION && effort >= EffortLevel::MODERATE)
        speculation_level = TypeAnalysis::SOME;
    TypeAnalysis* types = doTypeAnalysis(source->cfg, source->arg_names, spec->arg_types, effort, speculation_level,
                                         source->getScopeInfo());

    _t2.split();

    GuardList guards;

    BlockSet full_blocks, partial_blocks;
    if (entry_descriptor == NULL) {
        for (CFGBlock* b : source->cfg->blocks) {
            full_blocks.insert(b);
        }
    } else {
        full_blocks.insert(entry_descriptor->backedge->target);
        computeBlockSetClosure(full_blocks, partial_blocks);
    }

    IRGenState irstate(cf, source, getGCBuilder(), dbg_funcinfo);

    emitBBs(&irstate, "opt", guards, GuardList(), types, entry_descriptor, full_blocks, partial_blocks);

    // De-opt handling:

    if (!guards.isEmpty()) {
        BlockSet deopt_full_blocks, deopt_partial_blocks;
        GuardList deopt_guards;
        // typedef std::unordered_map<CFGBlock*, std::unordered_map<AST_expr*, GuardList::ExprTypeGuard*> > Worklist;
        // Worklist guard_worklist;

        guards.getBlocksWithGuards(deopt_full_blocks);
        for (const auto& p : guards.exprGuards()) {
            deopt_partial_blocks.insert(p.second->cfg_block);
        }

        computeBlockSetClosure(deopt_full_blocks, deopt_partial_blocks);

        assert(deopt_full_blocks.size() || deopt_partial_blocks.size());

        irgen_us += _t2.split();
        TypeAnalysis* deopt_types = doTypeAnalysis(source->cfg, source->arg_names, spec->arg_types, effort,
                                                   TypeAnalysis::NONE, source->getScopeInfo());
        _t2.split();

        emitBBs(&irstate, "deopt", deopt_guards, guards, deopt_types, NULL, deopt_full_blocks, deopt_partial_blocks);
        assert(deopt_guards.isEmpty());
        deopt_guards.assertGotPatched();

        delete deopt_types;
    }
    guards.assertGotPatched();

    for (const auto& p : guards.exprGuards()) {
        delete p.second;
    }

    delete types;

    if (VERBOSITY("irgen") >= 1) {
        printf("generated IR:\n");
        printf("\033[33m");
        fflush(stdout);
        dumpPrettyIR(f);
        // f->dump();
        // g.cur_module->dump();
        // g.cur_module->print(llvm::outs(), NULL);
        printf("\033[0m");
        fflush(stdout);
    } else {
        // Somehow, running this code makes it faster...?????
        // printf("\033[0m");
        // fflush(stdout);
    }

#ifndef NDEBUG
    if (!BENCH) {
        // Calling verifyFunction() confuses the profiler, which will end up attributing
        // a large amount of runtime to it since the call stack looks very similar to
        // the (expensive) case of compiling the function.
        llvm::verifyFunction(*f);
    }
#endif

    irgen_us += _t2.split();
    static StatCounter us_irgen("us_compiling_irgen");
    us_irgen.log(irgen_us);

    if (ENABLE_LLVMOPTS)
        optimizeIR(f, effort);

    bool ENABLE_IR_DEBUG = false;
    if (ENABLE_IR_DEBUG) {
        addIRDebugSymbols(f);
        // dumpPrettyIR(f);
    }

    g.cur_module = NULL;

    return cf;
}



} // namespace pyston
