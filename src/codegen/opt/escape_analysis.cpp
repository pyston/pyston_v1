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

#include <unordered_set>
#include <queue>
#include <set>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/raw_ostream.h"

#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"

#include "codegen/opt/escape_analysis.h"
#include "codegen/opt/util.h"

using namespace llvm;

namespace pyston {

EscapeAnalysis::~EscapeAnalysis() {
    for (ChainInfo *chain : chains) {
        delete chain;
    }
}

void EscapeAnalysis::getAnalysisUsage(llvm::AnalysisUsage &info) const {
    info.setPreservesCFG();
    info.addRequiredTransitive<DataLayout>();
}

bool EscapeAnalysis::runOnFunction(Function &F) {
    if (VERBOSITY("opt") >= 1) outs() << "Running escape analysis on " << F.getName() << '\n';

    for (inst_iterator inst_it = inst_begin(F), _inst_end = inst_end(F); inst_it != _inst_end; ++inst_it) {
        CallInst *alloc = dyn_cast<CallInst>(&*inst_it);
        if (!alloc || !isAllocCall(alloc))
            continue;

        ChainInfo *chain = new ChainInfo(alloc);
        chains.push_back(chain);

        // Calculating derived pointers, and finding escape points
        {
            // Instructions in the queue to be visited:
            std::deque<Instruction*> queue;
            // Instructions we've fully visited:
            std::unordered_set<Instruction*> checked;

            queue.push_back(alloc);

            while (queue.size()) {
                Instruction* next = queue.back();
                queue.pop_back();

                if (checked.count(next))
                    continue;

                checked.insert(next);

                for (Value::use_iterator use_it = next->use_begin(), use_end = next->use_end(); use_it != use_end; ++use_it) {
                    if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(*use_it)) {
                        queue.push_back(gep);
                        chain->derived.insert(gep);
                        chain_by_pointer[gep] = chain;
                        continue;
                    }

                    if (CastInst *bc = dyn_cast<CastInst>(*use_it)) {
                        queue.push_back(bc);
                        chain->derived.insert(bc);
                        chain_by_pointer[bc] = chain;
                        continue;
                    }

                    if (PHINode *phi = dyn_cast<PHINode>(*use_it)) {
                        queue.push_back(phi);
                        chain->derived.insert(phi);
                        chain_by_pointer[phi] = chain;
                        continue;
                    }

                    if (isa<LoadInst>(*use_it)) {
                        continue;
                    }

                    if (ReturnInst *ret = dyn_cast<ReturnInst>(*use_it)) {
                        if (VERBOSITY() >= 2) errs() << "Not dead; used here: " << *ret << '\n';
                        chain->escape_points.insert(ret);
                        continue;
                    }






                    if (StoreInst *si = dyn_cast<StoreInst>(*use_it)) {
                        if (si->getPointerOperand() == next) {
                        } else {
                            assert(si->getValueOperand() == next);
                            if (VERBOSITY() >= 2) errs() << "Escapes here: " << *si << '\n';
                            chain->escape_points.insert(si);
                        }
                        continue;
                    }

                    if (CallInst *si = dyn_cast<CallInst>(*use_it)) {
                        if (VERBOSITY() >= 2) errs() << "Escapes here: " << *si << '\n';
                        chain->escape_points.insert(si);
                        continue;
                    }



                    use_it->dump();
                    RELEASE_ASSERT(0, "");
                }
            }
        }

        // Calculating BB-level escape-ness
        {
            std::deque<const BasicBlock*> queue;

            for (auto I : chain->escape_points) {
                chain->bb_escapes[I->getParent()] = BBPartialEscape;
                queue.insert(queue.end(), succ_begin(I->getParent()), succ_end(I->getParent()));
            }

            while (queue.size()) {
                const BasicBlock* bb = queue.back();
                queue.pop_back();

                if (chain->bb_escapes[bb] == BBFullEscape)
                    continue;

                chain->bb_escapes[bb] = BBFullEscape;
                queue.insert(queue.end(), succ_begin(bb), succ_end(bb));
            }

            for (BasicBlock &bb : F) {
                if (chain->bb_escapes.count(&bb) == 0)
                    chain->bb_escapes[&bb] = BBNoEscape;

                //outs() << bb.getName() << ' ' << chain->bb_escapes[&bb] << '\n';
            }
        }
    }


    return false;
}

EscapeAnalysis::EscapeResult EscapeAnalysis::escapes(const Value* ptr, const Instruction *at_instruction) {
    assert(ptr);
    assert(at_instruction);

    if (chain_by_pointer.count(ptr) == 0)
        return Escaped;

    ChainInfo *chain = chain_by_pointer[ptr];
    assert(chain);

    if (chain->escape_points.size() == 0) {
        //ptr->dump();
        return NoEscape;
    }

    BBEscape bb_escape = chain->bb_escapes[at_instruction->getParent()];
    if (bb_escape == BBNoEscape)
        return WillEscape;
    if (bb_escape == BBFullEscape)
        return Escaped;

    // This pointer escapes at some point in this bb.
    // If the at_instruction occurs before any of the escape points, then we're fine.
    assert(bb_escape == BBPartialEscape);
    for (const Instruction &I : *at_instruction->getParent()) {
        if (chain->escape_points.count(&I))
            return Escaped;
        if (&I == at_instruction)
            return WillEscape;
    }

    RELEASE_ASSERT(0, "");
}



char EscapeAnalysis::ID = 0;
static RegisterPass<EscapeAnalysis> X("escape_analysis", "Escape analysis", false, true);

FunctionPass* createEscapeAnalysisPass() {
    return new EscapeAnalysis();
}

}

