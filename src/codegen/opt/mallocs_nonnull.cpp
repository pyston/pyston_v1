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

#include <map>
#include <queue>
#include <set>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "core/common.h"
#include "core/options.h"

using namespace llvm;

namespace pyston {

// TODO Copy-paste
static bool isMallocCall(const CallInst* CI) {
    if (!CI)
        return false;

    Function* Callee = CI->getCalledFunction();
    if (Callee == 0 || !Callee->isDeclaration())
        return false;
    if (Callee->getName() != "malloc" /*&&
            Callee->getName() != "my_malloc" &&
            Callee->getName() != "_Znwj" && // operator new(unsigned int)
            Callee->getName() != "_Znwm" && // operator new(unsigned long)
            Callee->getName() != "_Znaj" && // operator new[](unsigned int)
            Callee->getName() != "_Znam"*/) // operator new[](unsigned long)
        return false;

    // Check malloc prototype.
    // FIXME: workaround for PR5130, this will be obsolete when a nobuiltin
    // attribute will exist.
    FunctionType* FTy = Callee->getFunctionType();
    return FTy->getReturnType() == Type::getInt8PtrTy(FTy->getContext()) && FTy->getNumParams() == 1
           && (FTy->getParamType(0)->isIntegerTy(32) || FTy->getParamType(0)->isIntegerTy(64));
}

/// isFreeCall - Returns non-null if the value is a call to the builtin free()
static const CallInst* isFreeCall(const Value* I) {
    const CallInst* CI = dyn_cast<CallInst>(I);
    if (!CI)
        return 0;
    Function* Callee = CI->getCalledFunction();
    if (Callee == 0 || !Callee->isDeclaration())
        return 0;

    if (Callee->getName() != "free" /*&&
            Callee->getName() != "my_free" &&
            Callee->getName() != "_ZdlPv" && // operator delete(void*)
            Callee->getName() != "_ZdaPv"*/) // operator delete[](void*)
        return 0;

    // Check free prototype.
    // FIXME: workaround for PR5130, this will be obsolete when a nobuiltin
    // attribute will exist.
    FunctionType* FTy = Callee->getFunctionType();
    if (!FTy->getReturnType()->isVoidTy())
        return 0;
    if (FTy->getNumParams() != 1)
        return 0;
    if (FTy->getParamType(0) != Type::getInt8PtrTy(Callee->getContext()))
        return 0;

    return CI;
}

class ComparisonFinder : public InstVisitor<ComparisonFinder, bool> {
private:
    bool any_changes;
    std::deque<Instruction*> to_process;
    Instruction* processing;

public:
    ComparisonFinder(CallInst* malloc) : any_changes(false) { to_process.push_back(malloc); }

    bool elide_comparisons() {
        while (to_process.size()) {
            processing = to_process.front();
            to_process.pop_front();

            // errs() << "processing: " << *processing << '\n';
            bool changed = false;
            do {
                changed = false;
                for (User* user : processing->users()) {
                    // errs() << "looking at: " << *user << '\n';
                    changed = visit(cast<Instruction>(user));
                    if (changed)
                        break;
                }
            } while (changed);
        }
        return any_changes;
    }

    bool visitBitCastInst(BitCastInst& inst) {
        to_process.push_back(&inst);
        return false;
    }

    bool visitICmpInst(ICmpInst& inst) {
        // errs() << "got icmp instruction!  " << inst << '\n';

        bool changed = false;
        if (inst.getPredicate() == CmpInst::ICMP_EQ) {
            assert(inst.getNumOperands() == 2);

            if (inst.getOperand(1) == processing) {
                inst.swapOperands();
                changed = true;
                any_changes = true;
            }
            assert(dyn_cast<Instruction>(inst.getOperand(0)) == processing);
            Value* other = inst.getOperand(1);
            if (isa<ConstantPointerNull>(other)) {
                if (VERBOSITY("opt") >= 2) {
                    errs() << inst << '\n';
                    errs() << "replacing with false!\n";
                }

                Value* new_value = ConstantInt::getFalse(other->getContext());
                inst.replaceAllUsesWith(new_value);
                inst.eraseFromParent();
                changed = true;
                any_changes = true;
            }
        }
        return changed;
    }

    bool visitInstruction(Instruction& inst) {
        // errs() << "got misc instruction: " << inst << '\n';
        return false;
    }
};

class MallocsNonNullPass : public FunctionPass {
public:
    static char ID;
    MallocsNonNullPass() : FunctionPass(ID) {}

    virtual void getAnalysisUsage(AnalysisUsage& info) const { info.setPreservesCFG(); }

    virtual bool runOnFunction(Function& F) {
        int num_changed = 0;
        for (inst_iterator inst_it = inst_begin(F), _inst_end = inst_end(F); inst_it != _inst_end; ++inst_it) {
            if (!isMallocCall(dyn_cast<CallInst>(&*inst_it)))
                continue;

            if (VERBOSITY("opt") >= 2) {
                errs() << "\nFound malloc call:\n" << *inst_it << '\n';
            }
            num_changed += ComparisonFinder(cast<CallInst>(&*inst_it)).elide_comparisons();
        }

        return num_changed > 0;
    }
};
char MallocsNonNullPass::ID = 0;

FunctionPass* createMallocsNonNullPass() {
    return new MallocsNonNullPass();
}
}

static RegisterPass<pyston::MallocsNonNullPass> X("mallocs_nonnull", "Use the fact that malloc() doesnt return NULL",
                                                  true, false);
