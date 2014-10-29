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

#ifndef PYSTON_CODEGEN_IRGEN_IRGENERATOR_H
#define PYSTON_CODEGEN_IRGEN_IRGENERATOR_H

#include <map>

#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Instructions.h"

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

typedef std::unordered_map<std::string, CompilerVariable*> SymbolTable;
typedef std::map<std::string, CompilerVariable*> SortedSymbolTable;
typedef std::unordered_map<std::string, ConcreteCompilerVariable*> ConcreteSymbolTable;

extern const std::string CREATED_CLOSURE_NAME;
extern const std::string PASSED_CLOSURE_NAME;
extern const std::string PASSED_GENERATOR_NAME;


// Class that holds state of the current IR generation, that might not be local
// to the specific phase or pass we're in.
// TODO this probably shouldn't be here
class IRGenState {
private:
    CompiledFunction* cf;
    SourceInfo* source_info;
    GCBuilder* gc;
    llvm::MDNode* func_dbg_info;

    llvm::AllocaInst* scratch_space;
    int scratch_size;

public:
    IRGenState(CompiledFunction* cf, SourceInfo* source_info, GCBuilder* gc, llvm::MDNode* func_dbg_info)
        : cf(cf), source_info(source_info), gc(gc), func_dbg_info(func_dbg_info), scratch_space(NULL), scratch_size(0) {
        assert(cf->func);
        assert(!cf->clfunc); // in this case don't need to pass in sourceinfo
    }

    CompiledFunction* getCurFunction() { return cf; }

    llvm::Function* getLLVMFunction() { return cf->func; }

    EffortLevel::EffortLevel getEffortLevel() { return cf->effort; }

    GCBuilder* getGC() { return gc; }

    llvm::Value* getScratchSpace(int min_bytes);

    ConcreteCompilerType* getReturnType() {
        assert(cf->spec);
        return cf->spec->rtn_type;
    }

    SourceInfo* getSourceInfo() { return source_info; }

    ScopeInfo* getScopeInfo();
    ScopeInfo* getScopeInfoForNode(AST* node);

    llvm::MDNode* getFuncDbgInfo() { return func_dbg_info; }
};

class GuardList {
public:
    struct ExprTypeGuard {
        CFGBlock* cfg_block;
        llvm::BranchInst* branch;
        AST_expr* ast_node;
        CompilerVariable* val;
        SymbolTable st;

        ExprTypeGuard(CFGBlock* cfg_block, llvm::BranchInst* branch, AST_expr* ast_node, CompilerVariable* val,
                      const SymbolTable& st);
    };

    struct BlockEntryGuard {
        CFGBlock* cfg_block;
        llvm::BranchInst* branch;
        SymbolTable symbol_table;

        BlockEntryGuard(CFGBlock* cfg_block, llvm::BranchInst* branch, const SymbolTable& symbol_table);
    };

private:
    std::unordered_map<AST_expr*, ExprTypeGuard*> expr_type_guards;
    std::unordered_map<CFGBlock*, std::vector<BlockEntryGuard*> > block_begin_guards;
    // typedef std::unordered_map<AST_expr*, ExprTypeGuard*>::iterator expr_type_guard_iterator;
    // typedef std::unordered_map<AST_expr*, ExprTypeGuard*>::const_iterator expr_type_guard_const_iterator;
    typedef decltype(expr_type_guards)::iterator expr_type_guard_iterator;
    typedef decltype(expr_type_guards)::const_iterator expr_type_guard_const_iterator;

public:
    llvm::iterator_range<expr_type_guard_iterator> exprGuards() {
        return llvm::iterator_range<expr_type_guard_iterator>(expr_type_guards.begin(), expr_type_guards.end());
    }

    void getBlocksWithGuards(std::unordered_set<CFGBlock*>& add_to) {
        for (const auto& p : block_begin_guards) {
            add_to.insert(p.first);
        }
    }

    void assertGotPatched() {
#ifndef NDEBUG
        for (const auto& p : block_begin_guards) {
            for (const auto g : p.second) {
                assert(g->branch->getSuccessor(0) != g->branch->getSuccessor(1));
            }
        }

        for (const auto& p : expr_type_guards) {
            assert(p.second->branch->getSuccessor(0) != p.second->branch->getSuccessor(1));
        }
#endif
    }

    ExprTypeGuard* getNodeTypeGuard(AST_expr* node) const {
        expr_type_guard_const_iterator it = expr_type_guards.find(node);
        if (it == expr_type_guards.end())
            return NULL;
        return it->second;
    }

    bool isEmpty() const { return expr_type_guards.size() == 0 && block_begin_guards.size() == 0; }

    void addExprTypeGuard(CFGBlock* cfg_block, llvm::BranchInst* branch, AST_expr* ast_node, CompilerVariable* val,
                          const SymbolTable& st) {
        ExprTypeGuard*& g = expr_type_guards[ast_node];
        assert(g == NULL);
        g = new ExprTypeGuard(cfg_block, branch, ast_node, val, st);
    }

    void registerGuardForBlockEntry(CFGBlock* cfg_block, llvm::BranchInst* branch, const SymbolTable& st) {
        // printf("Adding guard for block %p, in %p\n", cfg_block, this);
        std::vector<BlockEntryGuard*>& v = block_begin_guards[cfg_block];
        v.push_back(new BlockEntryGuard(cfg_block, branch, st));
    }

    const std::vector<BlockEntryGuard*>& getGuardsForBlock(CFGBlock* block) const {
        std::unordered_map<CFGBlock*, std::vector<BlockEntryGuard*> >::const_iterator it
            = block_begin_guards.find(block);
        if (it != block_begin_guards.end())
            return it->second;

        static std::vector<BlockEntryGuard*> empty_list;
        return empty_list;
    }
};

class IRGenerator {
private:
public:
    struct EndingState {
        SymbolTable* symbol_table;
        ConcreteSymbolTable* phi_symbol_table;
        llvm::BasicBlock* ending_block;
        EndingState(SymbolTable* symbol_table, ConcreteSymbolTable* phi_symbol_table, llvm::BasicBlock* ending_block)
            : symbol_table(symbol_table), phi_symbol_table(phi_symbol_table), ending_block(ending_block) {}
    };

    virtual ~IRGenerator() {}

    virtual void doFunctionEntry(const SourceInfo::ArgNames& arg_names,
                                 const std::vector<ConcreteCompilerType*>& arg_types) = 0;

    virtual void giveLocalSymbol(const std::string& name, CompilerVariable* var) = 0;
    virtual void copySymbolsFrom(SymbolTable* st) = 0;
    virtual void run(const CFGBlock* block) = 0;
    virtual EndingState getEndingSymbolTable() = 0;
    virtual void doSafePoint() = 0;
    virtual void addFrameStackmapArgs(PatchpointInfo* pp, std::vector<llvm::Value*>& stackmap_args) = 0;
};

class IREmitter;
IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator = NULL);
IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types, GuardList& out_guards,
                               const GuardList& in_guards, bool is_partial);

CLFunction* wrapFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body, SourceInfo* source);
}

#endif
