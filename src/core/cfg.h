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

#ifndef PYSTON_CORE_CFG_H
#define PYSTON_CORE_CFG_H

/*
 * This CFG is a relatively high-level CFG, closely corresponding to the input Python source.  We break down
 * control-flow constructs,
 * but it doesn't do things like decompose IfExpressions or short-circuit conditional expressions.
 * Those will have to get broken into low-level control flow, so this CFG doesn't exactly correspond to the llvm-level
 * one we will eventually
 * generate; this one is (at least for now) meant to be a slightly-lowered version of the input AST, for doing
 * relatively high-level things such
 * as type analysis, liveness, etc, and then using as the source representation for the next lowering pass (emitting
 * llvm SSA)
 */

#include <vector>

#include "core/ast.h"
#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

class AST_stmt;
class Box;

class CFG;
class CFGBlock {
private:
    CFG* cfg;

public:
    // Baseline JIT helper fields:
    // contains address to the start of the code of this basic block
    void* code;
    // contains the address of the entry function
    std::pair<CFGBlock*, Box*>(*entry_code)(void* interpeter, CFGBlock* block);

    std::vector<AST_stmt*> body;
    std::vector<CFGBlock*> predecessors, successors;
    int idx; // index in the CFG
    const char* info;

    typedef std::vector<AST_stmt*>::iterator iterator;

    CFGBlock(CFG* cfg, int idx) : cfg(cfg), code(NULL), entry_code(NULL), idx(idx), info(NULL) {}

    void connectTo(CFGBlock* successor, bool allow_backedge = false);
    void unconnectFrom(CFGBlock* successor);

    void push_back(AST_stmt* node) { body.push_back(node); }
    void print();
};

// Control Flow Graph
class CFG {
private:
    int next_idx;

public:
    std::vector<CFGBlock*> blocks;

    CFG() : next_idx(0) {}

    CFGBlock* getStartingBlock() { return blocks[0]; }

    CFGBlock* addBlock() {
        int idx = next_idx;
        next_idx++;
        CFGBlock* block = new CFGBlock(this, idx);
        blocks.push_back(block);

        return block;
    }

    // Creates a block which must be placed later, using placeBlock().
    // Must be placed on same CFG it was created on.
    // You can also safely delete it without placing it.
    CFGBlock* addDeferredBlock() {
        CFGBlock* block = new CFGBlock(this, -1);
        return block;
    }

    void placeBlock(CFGBlock* block) {
        assert(block->idx == -1);
        block->idx = next_idx;
        next_idx++;
        blocks.push_back(block);
    }

    void print();
};

class SourceInfo;
CFG* computeCFG(SourceInfo* source, std::vector<AST_stmt*> body);
}

#endif
