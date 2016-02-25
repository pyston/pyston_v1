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

#include "codegen/opt/util.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

namespace pyston {

bool isAllocCall(const llvm::StringRef name) {
    if (name == "malloc")
        return true;

    if (name == "_ZN6pyston2gc4Heap10allocSmallEmi")
        return true;

    return false;
}

bool isAllocCall(const llvm::CallInst* CI) {
    if (!CI)
        return false;

    llvm::Function* Callee = CI->getCalledFunction();
    if (Callee == 0 || !Callee->isDeclaration())
        return false;

    return isAllocCall(Callee->getName());
}

void* getCalledFuncAddr(const llvm::CallInst* CI) {
    if (const llvm::ConstantExpr* CE = llvm::dyn_cast<const llvm::ConstantExpr>(CI->getCalledValue())) {
        llvm::PointerType* PT = llvm::dyn_cast<llvm::PointerType>(CE->getType());
        if (CE->isCast() && CE->getOpcode() == llvm::Instruction::IntToPtr && PT)
            return (void*)llvm::cast<llvm::ConstantInt>(CE->getOperand(0))->getSExtValue();
    }
    return 0;
}
}
