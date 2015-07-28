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

#ifndef PYSTON_CODEGEN_IRGEN_IRGENERATOR_H
#define PYSTON_CODEGEN_IRGEN_IRGENERATOR_H

#include <map>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Instructions.h"

#include "core/stringpool.h"
#include "core/types.h"

namespace llvm {
class AllocaInst;
class BasicBlock;
class BranchInst;
// class Function;
class MDNode;
}

namespace pyston {

class AST_Invoke;
class CFGBlock;
class GCBuilder;
struct PatchpointInfo;
class ScopeInfo;
class TypeAnalysis;

typedef std::unordered_map<InternedString, CompilerVariable*> SymbolTable;
typedef std::map<InternedString, CompilerVariable*> SortedSymbolTable;
typedef std::unordered_map<InternedString, ConcreteCompilerVariable*> ConcreteSymbolTable;

extern const std::string CREATED_CLOSURE_NAME;
extern const std::string PASSED_CLOSURE_NAME;
extern const std::string PASSED_GENERATOR_NAME;
extern const std::string FRAME_INFO_PTR_NAME;


// Class that holds state of the current IR generation, that might not be local
// to the specific phase or pass we're in.
// TODO this probably shouldn't be here
class IRGenState {
private:
    // Note: due to some not-yet-fixed behavior, cf->clfunc is NULL will only get set to point
    // to clfunc at the end of irgen.
    CLFunction* clfunc;
    CompiledFunction* cf;
    SourceInfo* source_info;
    std::unique_ptr<PhiAnalysis> phis;
    ParamNames* param_names;
    GCBuilder* gc;
    llvm::MDNode* func_dbg_info;

    llvm::AllocaInst* scratch_space;
    llvm::Value* frame_info;
    llvm::Value* boxed_locals;
    llvm::Value* frame_info_arg;
    int scratch_size;

    llvm::DenseMap<CFGBlock*, ExceptionStyle> landingpad_styles;

public:
    IRGenState(CLFunction* clfunc, CompiledFunction* cf, SourceInfo* source_info, std::unique_ptr<PhiAnalysis> phis,
               ParamNames* param_names, GCBuilder* gc, llvm::MDNode* func_dbg_info);
    ~IRGenState();

    CompiledFunction* getCurFunction() { return cf; }
    CLFunction* getCL() { return clfunc; }

    llvm::Function* getLLVMFunction() { return cf->func; }

    EffortLevel getEffortLevel() { return cf->effort; }

    GCBuilder* getGC() { return gc; }

    llvm::Value* getScratchSpace(int min_bytes);
    llvm::Value* getFrameInfoVar();
    llvm::Value* getBoxedLocalsVar();

    ConcreteCompilerType* getReturnType() { return cf->getReturnType(); }

    SourceInfo* getSourceInfo() { return source_info; }

    LivenessAnalysis* getLiveness() { return source_info->getLiveness(); }
    PhiAnalysis* getPhis() { return phis.get(); }

    ScopeInfo* getScopeInfo();
    ScopeInfo* getScopeInfoForNode(AST* node);

    llvm::MDNode* getFuncDbgInfo() { return func_dbg_info; }

    ParamNames* getParamNames() { return param_names; }

    void setFrameInfoArgument(llvm::Value* v) { frame_info_arg = v; }

    ExceptionStyle getLandingpadStyle(AST_Invoke* invoke);
    ExceptionStyle getLandingpadStyle(CFGBlock* block);
};

// turns CFGBlocks into LLVM IR
class IRGenerator {
private:
public:
    struct EndingState {
        // symbol_table records which Python variables are bound to what CompilerVariables at the end of this block.
        // phi_symbol_table records the ones that will need to be `phi'd.
        // both only record non-globals.
        SymbolTable* symbol_table;
        ConcreteSymbolTable* phi_symbol_table;
        llvm::BasicBlock* ending_block;
        EndingState(SymbolTable* symbol_table, ConcreteSymbolTable* phi_symbol_table, llvm::BasicBlock* ending_block)
            : symbol_table(symbol_table), phi_symbol_table(phi_symbol_table), ending_block(ending_block) {}
    };

    virtual ~IRGenerator() {}

    virtual void doFunctionEntry(const ParamNames& param_names, const std::vector<ConcreteCompilerType*>& arg_types)
        = 0;

    virtual void giveLocalSymbol(InternedString name, CompilerVariable* var) = 0;
    virtual void copySymbolsFrom(SymbolTable* st) = 0;
    virtual void run(const CFGBlock* block) = 0; // primary entry point
    virtual EndingState getEndingSymbolTable() = 0;
    virtual void doSafePoint(AST_stmt* next_statement) = 0;
    virtual void addFrameStackmapArgs(PatchpointInfo* pp, AST_stmt* current_stmt,
                                      std::vector<llvm::Value*>& stackmap_args) = 0;
};

class IREmitter;
class AST_Call;
IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator = NULL);
IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types);

CLFunction* wrapFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body, SourceInfo* source);
std::vector<BoxedString*>* getKeywordNameStorage(AST_Call* node);
}

#endif
