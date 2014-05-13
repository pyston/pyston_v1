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

#ifndef PYSTON_CODEGEN_DIS_H
#define PYSTON_CODEGEN_DIS_H

#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"

namespace llvm {
class MCDisassembler;
class MCInstrAnalysis;
class MCInstPrinter;
}

namespace pyston {

class PystonJITEventListener : public llvm::JITEventListener {
private:
    llvm::AsmPrinter* asm_printer;
    llvm::MCDisassembler* DisAsm;
    llvm::MCInstrAnalysis* MIA;
    llvm::MCInstPrinter* IP;

public:
    PystonJITEventListener();
    virtual void NotifyFunctionEmitted(const llvm::Function& f, void* ptr, size_t size,
                                       const llvm::JITEvent_EmittedFunctionDetails& details);

    virtual void NotifyObjectEmitted(const llvm::ObjectImage& Obj);
};
}

#endif
