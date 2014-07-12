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

#ifndef PYSTON_CODEGEN_IRGEN_H
#define PYSTON_CODEGEN_IRGEN_H

#include "llvm/IR/CallSite.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"

#include "codegen/compvars.h"
#include "core/types.h"

namespace pyston {

class AST_expr;
class GCBuilder;
class IREmitter;

struct ExcInfo {
public:
    llvm::BasicBlock* exc_dest;

    bool needsInvoke() { return exc_dest != NULL; }

    ExcInfo(llvm::BasicBlock* exc_dest) : exc_dest(exc_dest) {}

    static ExcInfo none() { return ExcInfo(NULL); }
};

// TODO get rid of this
class MyInserter : public llvm::IRBuilderDefaultInserter<true> {
private:
    IREmitter* emitter;

protected:
    void InsertHelper(llvm::Instruction* I, const llvm::Twine& Name, llvm::BasicBlock* BB,
                      llvm::BasicBlock::iterator InsertPt) const;

public:
    void setEmitter(IREmitter* emitter) { this->emitter = emitter; }
};

class PatchpointSetupInfo;

class IREmitter {
public:
    typedef llvm::IRBuilder<true, llvm::ConstantFolder, MyInserter> IRBuilder;

    virtual ~IREmitter() {}

    virtual IRBuilder* getBuilder() = 0;
    virtual GCBuilder* getGC() = 0;
    virtual CompiledFunction* currentFunction() = 0;

    virtual llvm::Value* getScratch(int num_bytes) = 0;
    virtual void releaseScratch(llvm::Value*) = 0;

    virtual llvm::Function* getIntrinsic(llvm::Intrinsic::ID) = 0;

    virtual llvm::CallSite createCall(ExcInfo exc_info, llvm::Value* callee, const std::vector<llvm::Value*>& args) = 0;
    virtual llvm::CallSite createCall(ExcInfo exc_info, llvm::Value* callee, llvm::Value* arg1) = 0;
    virtual llvm::CallSite createCall2(ExcInfo exc_info, llvm::Value* callee, llvm::Value* arg1, llvm::Value* arg2) = 0;
    virtual llvm::CallSite createCall3(ExcInfo exc_info, llvm::Value* callee, llvm::Value* arg1, llvm::Value* arg2,
                                       llvm::Value* arg3) = 0;
    virtual llvm::CallSite createPatchpoint(const PatchpointSetupInfo* pp, void* func_addr,
                                            const std::vector<llvm::Value*>& args, ExcInfo exc_info) = 0;
};

CompiledFunction* doCompile(SourceInfo* source, const OSREntryDescriptor* entry_descriptor,
                            EffortLevel::EffortLevel effort, FunctionSpecialization* spec, std::string nameprefix);

class TypeRecorder;
class OpInfo {
private:
    const EffortLevel::EffortLevel effort;
    TypeRecorder* const type_recorder;

public:
    const ExcInfo exc_info;

    OpInfo(EffortLevel::EffortLevel effort, TypeRecorder* type_recorder, ExcInfo exc_info)
        : effort(effort), type_recorder(type_recorder), exc_info(exc_info) {}

    bool isInterpreted() const { return effort == EffortLevel::INTERPRETED; }
    TypeRecorder* getTypeRecorder() const { return type_recorder; }
};
}

#endif
