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
    CompiledFunction* cf;
    SourceInfo* source_info;
    PhiAnalysis* phis;
    ParamNames* param_names;
    GCBuilder* gc;
    llvm::MDNode* func_dbg_info;

    llvm::AllocaInst* scratch_space;
    llvm::Value* frame_info;
    llvm::Value* boxed_locals;
    llvm::Value* frame_info_arg;
    int scratch_size;


public:
    IRGenState(CompiledFunction* cf, SourceInfo* source_info, PhiAnalysis* phis, ParamNames* param_names, GCBuilder* gc,
               llvm::MDNode* func_dbg_info)
        : cf(cf), source_info(source_info), phis(phis), param_names(param_names), gc(gc), func_dbg_info(func_dbg_info),
          scratch_space(NULL), frame_info(NULL), frame_info_arg(NULL), scratch_size(0) {
        assert(cf->func);
        assert(!cf->clfunc); // in this case don't need to pass in sourceinfo
    }

    CompiledFunction* getCurFunction() { return cf; }

    llvm::Function* getLLVMFunction() { return cf->func; }

    EffortLevel getEffortLevel() { return cf->effort; }

    GCBuilder* getGC() { return gc; }

    llvm::Value* getScratchSpace(int min_bytes);
    llvm::Value* getFrameInfoVar();
    llvm::Value* getBoxedLocalsVar();

    ConcreteCompilerType* getReturnType() { return cf->getReturnType(); }

    SourceInfo* getSourceInfo() { return source_info; }

    PhiAnalysis* getPhis() { return phis; }

    ScopeInfo* getScopeInfo();
    ScopeInfo* getScopeInfoForNode(AST* node);

    llvm::MDNode* getFuncDbgInfo() { return func_dbg_info; }

    ParamNames* getParamNames() { return param_names; }

    void setFrameInfoArgument(llvm::Value* v) { frame_info_arg = v; }
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
    virtual void doSafePoint() = 0;
    virtual void addFrameStackmapArgs(PatchpointInfo* pp, AST_stmt* current_stmt,
                                      std::vector<llvm::Value*>& stackmap_args) = 0;
};

class IREmitter;
IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator = NULL);
IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types);

CLFunction* wrapFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body, SourceInfo* source);
}

#endif
