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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"

#include "core/bst.h"
#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

class BST_stmt;
class Box;

class CFG;
class ParamNames;
class ScopeInfo;

// Simple class to override the default value of an int.
template <int D = -1> class DefaultedInt {
private:
    int x;

public:
    DefaultedInt() : x(D) {}
    DefaultedInt(int x) : x(x) {}
    DefaultedInt(const DefaultedInt& rhs) : x(rhs.x) {}
    DefaultedInt(DefaultedInt&& rhs) : x(rhs.x) {}
    void operator=(const DefaultedInt& rhs) { x = rhs.x; }
    void operator=(DefaultedInt&& rhs) { x = rhs.x; }
    template <typename T> bool operator<(T rhs) const { return x < rhs; }
    template <typename T> bool operator>(T rhs) const { return x > rhs; }
    template <typename T> bool operator<=(T rhs) const { return x <= rhs; }
    template <typename T> bool operator>=(T rhs) const { return x >= rhs; }

    operator int() const { return x; }
};

class CFGBlock {
public:
    CFG* const cfg;

    // Baseline JIT helper fields:
    // contains address to the start of the code of this basic block
    void* code;
    // contains the address of the entry function
    std::pair<CFGBlock*, Box*>(*entry_code)(void* interpeter, CFGBlock* block, Box** vregs);

    llvm::TinyPtrVector<CFGBlock*> predecessors;
    const char* info;
    int idx;                  // index in the CFG
    int offset_of_first_stmt; // offset of this block into the bytecode array in bytes

#ifndef NDEBUG
    // only one block at a time is allowed to add instructions to the CFG
    bool allowed_to_add_stuff = false;
#endif

    // returns the successors by looking at the terminator (which requires iterating over all instructions in the block)
    llvm::SmallVector<CFGBlock*, 2> successors() const;

    CFGBlock(CFG* cfg, int idx, const char* info = NULL)
        : cfg(cfg), code(NULL), entry_code(NULL), info(info), idx(idx), offset_of_first_stmt(-1) {}

    BST_stmt* body() {
        auto it = begin();
        return it != end() ? *it : NULL;
    }
    int sizeInBytes() const {
        int size = 0;
        for (BST_stmt* stmt : *this) {
            size += stmt->size_in_bytes();
        }
        return size;
    }
    BST_stmt* getTerminator() const {
        // TODO: this is inefficient
        for (BST_stmt* stmt : *this) {
            if (stmt->is_terminator())
                return stmt;
        }
        return NULL;
    }
    bool isPlaced() const { return offset_of_first_stmt != -1; }

    void connectTo(CFGBlock* successor, bool allow_backedge = false);
    void unconnectFrom(CFGBlock* successor);

    void print(const CodeConstants& code_constants, llvm::raw_ostream& stream = llvm::outs());

    class iterator {
    private:
        BST_stmt* stmt;

    public:
        iterator(BST_stmt* stmt) : stmt(stmt) {}

        bool operator!=(const iterator& rhs) const { return stmt != rhs.stmt; }
        bool operator==(const iterator& rhs) const { return stmt == rhs.stmt; }
        iterator& operator++() __attribute__((always_inline)) {
            if (likely(stmt)) {
                if (unlikely(stmt->is_terminator()))
                    *this = CFGBlock::end();
                else
                    stmt = (BST_stmt*)&((unsigned char*)stmt)[stmt->size_in_bytes()];
            }
            return *this;
        }
        BST_stmt* operator*() const { return stmt; }
    };

    inline iterator begin() const;
    static iterator end() { return iterator(NULL); }
};

// the vregs are split into three parts.
// user visible: used for all non compiler generated names, name could be used in a single block or multiple
//               all frames contain atleast this vregs in order to do frame introspection
// cross block : used for compiler generated names which get used in several blocks or which have closure scope
// single block: used by compiler created names which are only used in a single block.
//               get reused for different names
//
// we assign the lowest numbers to the user visible ones, followed by the cross block ones and finally the single block
// ones. we do this because not all tiers use all of the vregs and it still makes it fast to switch between tiers.
//
// usage by our different tiers:
// interpreter : [user visible] [cross block] [single block]
// baseline jit: [user visible] [cross block]
// llvm jit    : [user visible]
class VRegInfo {
private:
#ifndef NDEBUG
    // this maps use too much memory, we only use them in the debug build for asserts
    llvm::DenseMap<InternedString, DefaultedInt<VREG_UNDEFINED>> sym_vreg_map_user_visible;
    llvm::DenseMap<InternedString, DefaultedInt<VREG_UNDEFINED>> sym_vreg_map;
#endif

    // Reverse map, from vreg->symbol name.
    // Entries won't exist for all vregs (=no entries for reused vregs)
    std::vector<InternedString> vreg_sym_map;

    int num_vregs_cross_block = -1;
    int num_vregs_user_visible = -1;
    int num_vregs = -1;

public:
#ifndef NDEBUG
    // map of all assigned names. if the name is block local the vreg number is not unique because this vregs get reused
    // between blocks.
    const llvm::DenseMap<InternedString, DefaultedInt<VREG_UNDEFINED>>& getSymVRegMap() const { return sym_vreg_map; }
    const llvm::DenseMap<InternedString, DefaultedInt<VREG_UNDEFINED>>& getUserVisibleSymVRegMap() const {
        return sym_vreg_map_user_visible;
    }

    int getVReg(InternedString name) const {
        assert(hasVRegsAssigned());
        ASSERT(sym_vreg_map.count(name), "%s", name.c_str());
        auto it = sym_vreg_map.find(name);
        assert(it != sym_vreg_map.end());
        assert(it->second != -1);
        return it->second;
    }
#endif

    llvm::ArrayRef<InternedString> getVRegSymUserVisibleMap() const {
        return llvm::makeArrayRef(vreg_sym_map).slice(0, num_vregs_user_visible);
    }

    // Not all vregs correspond to a name; many are our compiler-generated variables.
    bool vregHasName(int vreg) const { return vreg < num_vregs_cross_block; }

// Testing flag to turn off the "vreg reuse" optimization.
#define REUSE_VREGS 1

    InternedString getName(int vreg) const {
        assert(hasVRegsAssigned());
        assert(vreg >= 0 && vreg < num_vregs);
#if REUSE_VREGS
        assert(vregHasName(vreg));
#endif
        return vreg_sym_map[vreg];
    }

    bool isUserVisibleVReg(int vreg) const { return vreg < num_vregs_user_visible; }
    bool isCrossBlockVReg(int vreg) const { return !isUserVisibleVReg(vreg) && vreg < num_vregs_cross_block; }
    bool isBlockLocalVReg(int vreg) const { return vreg >= num_vregs_cross_block; }

    int getTotalNumOfVRegs() const { return num_vregs; }
    int getNumOfUserVisibleVRegs() const { return num_vregs_user_visible; }
    int getNumOfCrossBlockVRegs() const { return num_vregs_cross_block; }

    bool hasVRegsAssigned() const { return num_vregs != -1; }
    void assignVRegs(const CodeConstants& code_constants, CFG* cfg, const ParamNames& param_names,
                     llvm::DenseMap<class TrackingVRegPtr, InternedString>& id_vreg);
};

// Control Flow Graph
class CFG {
private:
    int next_idx;
    VRegInfo vreg_info;

public:
    std::vector<CFGBlock*> blocks;
    BSTAllocator bytecode;

public:
    CFG() : next_idx(0) {}
    ~CFG() {
        for (auto&& block : blocks) {
            delete block;
        }
    }

    CFGBlock* getStartingBlock() { return blocks[0]; }
    VRegInfo& getVRegInfo() { return vreg_info; }

    // Creates a block which must be placed later, using placeBlock().
    // Must be placed on same CFG it was created on.
    // You can also safely delete it without placing it.
    CFGBlock* addDeferredBlock() {
        CFGBlock* block = new CFGBlock(this, -1);
        return block;
    }

    void placeBlock(CFGBlock* block) {
        assert(!block->isPlaced());

#ifndef NDEBUG
        // check that there is no block with the same offset of first stmt
        assert(!block->allowed_to_add_stuff);
        std::unordered_map<int /* offset */, int> check_no_dup_blocks;
        for (auto&& b : blocks) {
            b->allowed_to_add_stuff = false;
            ++check_no_dup_blocks[b->offset_of_first_stmt];
        }
        ++check_no_dup_blocks[bytecode.getSize()];
        assert(check_no_dup_blocks[bytecode.getSize()] == 1);
        for (auto&& e : check_no_dup_blocks) {
            assert(e.second == 1);
        }
#endif

        assert(block->idx == -1);
        block->idx = next_idx;
        next_idx++;
        blocks.push_back(block);
        block->offset_of_first_stmt = bytecode.getSize();

#ifndef NDEBUG
        block->allowed_to_add_stuff = true;
#endif
    }

    void print(const CodeConstants& code_constants, llvm::raw_ostream& stream = llvm::outs());
};

CFGBlock::iterator CFGBlock::begin() const {
    if (offset_of_first_stmt >= cfg->bytecode.getSize())
        return end();
    return iterator((BST_stmt*)&cfg->bytecode.getData()[offset_of_first_stmt]);
}

class VRegSet {
private:
    llvm::BitVector v;

public:
    VRegSet(int num_vregs) : v(num_vregs, false) {}

    // TODO: what is the referenc type here?
    bool operator[](int vreg) const {
        assert(vreg >= 0 && vreg < v.size());
        return v[vreg];
    }
    void set(int vreg) {
        assert(vreg >= 0 && vreg < v.size());
        v[vreg] = true;
    }

    int numSet() const { return v.count(); }

    class iterator {
    public:
        const VRegSet& set;
        int i;
        iterator(const VRegSet& set, int i) : set(set), i(i) {}

        iterator& operator++() {
            i = set.v.find_next(i);
            return *this;
        }

        bool operator==(const iterator& rhs) const { return i == rhs.i; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

        int operator*() { return i; }
    };

    iterator begin() const { return iterator(*this, v.find_first()); }
    iterator end() const { return iterator(*this, -1); }
};

// VRegMap: A compact way of representing a value per vreg.
//
// One thing to note is that every vreg will get a value by default
// (the default value of T()), and fetching an unset vreg will return
// that value.
//
// Iterating will skip over these values though.  If you want to see them,
// you can iterate from 0 to numVregs().
template <typename T> class VRegMap {
private:
    // TODO: switch just to a T*?
    std::vector<T> v;

public:
    VRegMap(int num_vregs) : v(num_vregs) {}

    T& operator[](int vreg) {
        assert(vreg >= 0 && vreg < v.size());
        return v[vreg];
    }

    const T& operator[](int vreg) const {
        assert(vreg >= 0 && vreg < v.size());
        return v[vreg];
    }

    void clear() {
        int n = v.size();
        for (int i = 0; i < n; i++) {
            v[i] = T();
        }
    }

    int numSet() {
        int n = v.size();
        int r = 0;
        for (int i = 0; i < n; i++) {
            if (v[i] != T())
                r++;
        }
        return r;
    }

    class iterator {
    public:
        const VRegMap<T>& map;
        int i;
        iterator(const VRegMap<T>& map, int i) : map(map), i(i) {}

        // TODO: make this skip unset values?
        iterator& operator++() {
            do {
                i++;
            } while (i < map.numVregs() && map[i] == T());
            return *this;
        }

        bool operator==(const iterator& rhs) const { return i == rhs.i; }
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

        std::pair<int, const T&> operator*() { return std::pair<int, const T&>(i, map[i]); }
        int first() const { return i; }
        const T& second() const { return map[i]; }
    };

    int numVregs() const { return v.size(); }

    iterator begin() const {
        iterator it = iterator(*this, -1);
        ++it;
        return it;
    }

    iterator end() const { return iterator(*this, this->v.size()); }
};

BoxedCode* computeAllCFGs(AST* ast, bool globals_from_module, FutureFlags future_flags, BoxedString* fn,
                          BoxedModule* bm);
void printCFG(CFG* cfg, const CodeConstants& code_constants);
}

#endif
