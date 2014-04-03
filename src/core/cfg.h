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

#ifndef PYSTON_CORE_CFG_H
#define PYSTON_CORE_CFG_H

/*
 * This CFG is a relatively high-level CFG, closely corresponding to the input Python source.  We break down control-flow constructs,
 * but it doesn't do things like decompose IfExpressions or short-circuit conditional expressions.
 * Those will have to get broken into low-level control flow, so this CFG doesn't exactly correspond to the llvm-level one we will eventually
 * generate; this one is (at least for now) meant to be a slightly-lowered version of the input AST, for doing relatively high-level things such
 * as type analysis, liveness, etc, and then using as the source representation for the next lowering pass (emitting llvm SSA)
 */

#include <vector>

#include "core/common.h"

namespace pyston {

class AST_stmt;

namespace AST_TYPE {
enum AST_TYPE;
}

class CFG;
class CFGBlock {
    private:
        CFG* cfg;
    public:
        std::vector<AST_stmt*> body;
        std::vector<CFGBlock*> predecessors, successors;
        int idx; // index in the CFG
        const char* info;

        typedef std::vector<AST_stmt*>::iterator iterator;

        CFGBlock(CFG *cfg, int idx) : cfg(cfg), idx(idx), info(NULL) {
        }

        void connectTo(CFGBlock *successor, bool allow_backedge=false);

        void push_back(AST_stmt* node) {
            body.push_back(node);
        }
};

// Control Flow Graph
class CFG {
    private:
    public:
        std::vector<CFGBlock*> blocks;

        CFGBlock* addBlock() {
            int idx = blocks.size();
            CFGBlock* block = new CFGBlock(this, idx);
            blocks.push_back(block);

            return block;
        }

        CFGBlock* addDeferredBlock() {
            CFGBlock* block = new CFGBlock(this, -1);
            return block;
        }

        void placeBlock(CFGBlock *block) {
            assert(block->idx == -1);
            block->idx = blocks.size();
            blocks.push_back(block);
        }

        void print();
};

CFG* computeCFG(AST_TYPE::AST_TYPE root_type, std::vector<AST_stmt*> body);


}

#endif
