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

#ifndef PYSTON_CODEGEN_OPT_ESCAPEANALYSIS_H
#define PYSTON_CODEGEN_OPT_ESCAPEANALYSIS_H

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "llvm/Pass.h"

namespace llvm {
class Instruction;
class Value;
}

namespace pyston {

class EscapeAnalysis : public llvm::FunctionPass {
private:
    enum BBEscape {
        BBNoEscape,      // at all points in this bb, the pointer hasn't escaped
        BBPartialEscape, // at some points it hasn't escaped, but at some it has
        BBFullEscape,    // at all points in this bb, the pointer has escaped
    };

    struct ChainInfo {
        llvm::Value* allocation;

        std::unordered_set<llvm::Value*> derived;

        std::unordered_set<const llvm::Instruction*> escape_points;

        //// Instructions that are free to be deleted if the chain is dead:
        // std::vector<Instruction*> deletions;
        //// Loads that have to be remapped if the chain is dead:
        // std::vector<LoadInst*> loads;

        std::unordered_map<const llvm::BasicBlock*, BBEscape> bb_escapes;

        ChainInfo(llvm::Value* alloc) : allocation(alloc) {}

        void dump();
    };
    std::vector<ChainInfo*> chains;
    std::unordered_map<const llvm::Value*, ChainInfo*> chain_by_pointer;

public:
    static char ID;
    EscapeAnalysis() : llvm::FunctionPass(ID) {}
    ~EscapeAnalysis();

    void getAnalysisUsage(llvm::AnalysisUsage& info) const override;
    bool runOnFunction(llvm::Function& F) override;

    enum EscapeResult {
        // This pointer has already escaped, so arbitrary code could be modifying it
        Escaped,
        // This point has not escaped, but will escape later; non-local code can't modify it,
        // but might read it later
        WillEscape,
        // This pointer never escapes.
        NoEscape,
    };

    EscapeResult escapes(const llvm::Value* ptr, const llvm::Instruction* at_instruction);
};
}


#endif
