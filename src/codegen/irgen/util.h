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

#ifndef PYSTON_CODEGEN_IRGEN_UTIL_H
#define PYSTON_CODEGEN_IRGEN_UTIL_H

#include <string>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"

namespace llvm {
class Constant;
class Function;
class Type;
}

namespace gc {
class GCVisitor;
}

namespace pyston {

llvm::Constant* embedRelocatablePtr(const void* addr, llvm::Type*, llvm::StringRef shared_name = llvm::StringRef());
llvm::Constant* embedConstantPtr(const void* addr, llvm::Type*);
llvm::Constant* getConstantInt(int64_t val);
llvm::Constant* getConstantDouble(double val);
llvm::Constant* getConstantInt(int64_t val, llvm::Type*);
llvm::Constant* getNullPtr(llvm::Type* t);

void clearRelocatableSymsMap();
void setPointersInCodeStorage(std::vector<const void*>* v);
const void* getValueOfRelocatableSym(const std::string& str);

void visitRelocatableSymsMap(gc::GCVisitor* visitor);

void dumpPrettyIR(llvm::Function* f);

// Insert an instruction at the first valid point *after* the given instruction.
// The non-triviality of this is that if the given instruction is an invoke, we have
// to be careful about where we place the new instruction -- this puts it on the
// normal-case destination.
//
// Note: I wish the `create_after` argument could be placed after the `Args... args` one.
// And I think that that should be valid, but clang doesn't seem to be accepting it.
template <typename T, typename... Args> T* createAfter(llvm::Instruction* create_after, Args... args) {
    if (llvm::InvokeInst* ii = llvm::dyn_cast<llvm::InvokeInst>(create_after)) {
        auto* block = ii->getNormalDest();
        if (block->empty())
            return new T(args..., block);
        else
            return new T(args..., block->getFirstInsertionPt());
    } else {
        auto* new_inst = new T(args...);
        new_inst->insertAfter(create_after);
        return new_inst;
    }
}
}

#endif
