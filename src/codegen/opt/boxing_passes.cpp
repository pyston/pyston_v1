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

#include <unordered_map>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"

#include "codegen/opt/util.h"
#include "core/stats.h"
#include "runtime/inline/boxing.h"

using namespace llvm;

namespace pyston {

// This pass removes boxInt, boxFloat and boxBools calls where the argument is coming from a corresponding unbox call.
// E.g. if 5784704 is boxFloat:
// %5 = call i64 @unboxInt(%"class.pyston::Box"* %0)
// %7 = call %"class.pyston::Box"* @boxInt(i64 %5)
// --> %7 will be replaced with %0
class RemoveUnnecessaryBoxingPass : public FunctionPass {
private:
public:
    static char ID;
    RemoveUnnecessaryBoxingPass() : FunctionPass(ID) {}

    virtual void getAnalysisUsage(AnalysisUsage& info) const { info.setPreservesCFG(); }

    virtual bool runOnFunction(Function& F) {
        int num_changed = 0;
        StatCounter sc_num_unnescessary_boxes("opt_unnescessary_boxes");

        std::unordered_map<CallInst*, CallInst*> dead_boxing_call;
        for (inst_iterator inst_it = inst_begin(F), _inst_end = inst_end(F); inst_it != _inst_end; ++inst_it) {
            CallInst* CI = dyn_cast<CallInst>(&*inst_it);
            if (!CI)
                continue;

            // We are only interested in boxInt, boxFloat and boxBool calls
            auto* func = CI->getCalledFunction();
            if (!func)
                continue;

            llvm::StringRef func_name = func->getName();
            if (func_name != "boxInt" && func_name != "boxFloat" && func_name != "boxBool")
                continue;

            CallInst* CI2 = dyn_cast<CallInst>(CI->getArgOperand(0));
            if (!CI2)
                continue;

            // Check if the value passed as argument to the boxing call is coming from the corresponding unbox call
            auto* func2 = CI2->getCalledFunction();
            if (!func2)
                continue;

            llvm::StringRef func2_name = func2->getName();
            if ((func_name == "boxInt" && func2_name == "unboxInt") || (func_name == "boxFloat" && func2_name == "unboxFloat")
                || (func_name == "boxBool" && func2_name == "unboxBool"))
                dead_boxing_call[CI] = CI2;
        }

        for (auto&& I : dead_boxing_call) {
            I.first->replaceAllUsesWith(new BitCastInst(I.second->getArgOperand(0), I.first->getType(), "", I.first));
            I.first->eraseFromParent();
            ++num_changed;
            sc_num_unnescessary_boxes.log();
        }

        return num_changed > 0;
    }
};
char RemoveUnnecessaryBoxingPass::ID = 0;

FunctionPass* createRemoveUnnecessaryBoxingPass() {
    return new RemoveUnnecessaryBoxingPass();
}

// This pass removes duplicate boxing calls inside the same BB.
// E.g. if 5784704 is boxFloat:
// %282 = call %"class.pyston::Box"* inttoptr (i64 5784704 to %"class.pyston::Box"* (double)*)(double 0.000000e+00)
// %290 = call %"class.pyston::Box"* inttoptr (i64 5784704 to %"class.pyston::Box"* (double)*)(double 0.000000e+00)
// --> %290 will be replaced by %282.
class RemoveDuplicateBoxingPass : public BasicBlockPass {
private:
public:
    static char ID;
    RemoveDuplicateBoxingPass() : BasicBlockPass(ID) {}

    virtual void getAnalysisUsage(AnalysisUsage& info) const { info.setPreservesCFG(); }

    virtual bool runOnBasicBlock(BasicBlock& BB) {
        std::unordered_map<std::pair<void*, Value*>, llvm::SmallVector<CallInst*, 4>> boxing_calls;

        // Find all boxInt, boxFloat and boxBool calls inside this BB and add them into a map,
        // the maps key is pair of the called function and the passed argument
        for (Instruction& I : BB) {
            if (CallInst* CI = dyn_cast<CallInst>(&I)) {
                auto* called_func = CI->getCalledFunction();
                if (!called_func)
                    continue;

                llvm::StringRef func_name = called_func->getName();
                if (func_name == "boxInt" || func_name == "boxFloat" || func_name == "boxBool")
                    boxing_calls[std::make_pair(called_func, CI->getArgOperand(0))].push_back(CI);
            }
        }

        StatCounter sc_num_duplicate_boxes("opt_duplicate_boxes");
        int num_changed = 0;

        // Iterate over over all boxing calls found
        for (auto&& I : boxing_calls) {

            // Check for duplicate calls
            if (I.second.size() < 2)
                continue;

            // Replace the duplicate boxing calls with the first one
            CallInst* FirstCI = 0;
            for (CallInst* CI : I.second) {
                if (FirstCI) {
                    CI->replaceAllUsesWith(FirstCI);
                    CI->eraseFromParent();
                    ++num_changed;
                    sc_num_duplicate_boxes.log();
                } else {
                    FirstCI = CI;
                }
            }
        }

        return num_changed > 0;
    }
};
char RemoveDuplicateBoxingPass::ID = 0;

BasicBlockPass* createRemoveDuplicateBoxingPass() {
    return new RemoveDuplicateBoxingPass();
}
}
