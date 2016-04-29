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

#include <cstdio>
#include <deque>
#include <queue>
#include <sstream>

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/gcbuilder.h"
#include "codegen/irgen.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/patchpoints.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

static int numSuccessors(llvm::BasicBlock* b) {
    return std::distance(llvm::succ_begin(b), llvm::succ_end(b));
}

static int numPredecessors(llvm::BasicBlock* b) {
    return std::distance(llvm::pred_begin(b), llvm::pred_end(b));
}

llvm::Value* RefcountTracker::setType(llvm::Value* v, RefType reftype) {
    assert(!llvm::isa<llvm::UndefValue>(v));

    auto& var = this->vars[v];

    assert(var.reftype == reftype || var.reftype == RefType::UNKNOWN);
    var.reftype = reftype;
    return v;
}

llvm::Value* RefcountTracker::setNullable(llvm::Value* v, bool nullable) {
    assert(!llvm::isa<llvm::UndefValue>(v));

    auto& var = this->vars[v];

    assert(var.nullable == nullable || var.nullable == false);
    var.nullable = nullable;
    return v;
}

bool RefcountTracker::isNullable(llvm::Value* v) {
    assert(vars.count(v));
    return vars.lookup(v).nullable;
}

void RefcountTracker::refConsumed(llvm::Value* v, llvm::Instruction* inst) {
    assert(this->vars[v].reftype != RefType::UNKNOWN);

    this->refs_consumed[inst].push_back(v);
}

void RefcountTracker::refUsed(llvm::Value* v, llvm::Instruction* inst) {
    assert(this->vars[v].reftype != RefType::UNKNOWN);

    this->refs_used[inst].push_back(v);
}

void RefcountTracker::setMayThrow(llvm::Instruction* inst) {
    assert(!may_throw.count(inst));
    this->may_throw.insert(inst);
}


void remapPhis(llvm::BasicBlock* in_block, llvm::BasicBlock* from_block, llvm::BasicBlock* new_from_block) {
    for (llvm::Instruction& i : *in_block) {
        llvm::Instruction* I = &i;
        llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(I);
        if (!phi)
            break;

        int idx = phi->getBasicBlockIndex(from_block);
        if (idx == -1)
            continue;
        phi->setIncomingBlock(idx, new_from_block);
    }
}

typedef llvm::DenseMap<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>, llvm::Instruction*> InsertionCache;
llvm::Instruction* findInsertionPoint(llvm::BasicBlock* BB, llvm::BasicBlock* from_bb, InsertionCache& cache) {
    assert(BB);
    assert(BB != from_bb);

    auto key = std::make_pair(BB, from_bb);

    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    // Break critical edges if we need to:
    if (numPredecessors(BB) > 1) {
        ASSERT(from_bb, "Don't know how to break the critical edge to(%s)", BB->getName().data());

        llvm::BasicBlock* breaker_block = llvm::BasicBlock::Create(g.context, "breaker", from_bb->getParent(), BB);
        llvm::BranchInst::Create(BB, breaker_block);
        // llvm::outs() << "Breaking edge from " << from_bb->getName() << " to " << BB->getName() << "; name is "
        //<< breaker_block->getName() << '\n';

        auto terminator = from_bb->getTerminator();

        if (llvm::BranchInst* br = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
            if (br->getSuccessor(0) == BB)
                br->setSuccessor(0, breaker_block);
            if (br->isConditional() && br->getSuccessor(1) == BB)
                br->setSuccessor(1, breaker_block);
        } else if (llvm::InvokeInst* ii = llvm::dyn_cast<llvm::InvokeInst>(terminator)) {
            if (ii->getNormalDest() == BB)
                ii->setNormalDest(breaker_block);
            ASSERT(ii->getUnwindDest() != BB, "don't know how break critical unwind edges");
        } else {
            llvm::outs() << *terminator << '\n';
            RELEASE_ASSERT(0, "unhandled terminator type");
        }

        remapPhis(BB, from_bb, breaker_block);

        cache[key] = breaker_block->getFirstInsertionPt();
        return cache[key];
    }

    if (llvm::isa<llvm::LandingPadInst>(*BB->begin())) {
        // Don't split up the landingpad+extract+cxa_begin_catch
        auto it = BB->begin();
        ++it;
        ++it;
        ++it;
        cache[key] = it;
        return &*it;
    } else {
        for (llvm::Instruction& I : *BB) {
            if (!llvm::isa<llvm::PHINode>(I) && !llvm::isa<llvm::AllocaInst>(I)) {
                cache[key] = &I;
                return &I;
            }
        }
        abort();
    }
}

#ifdef Py_TRACE_REFS
#define REFCOUNT_IDX 2
#else
#define REFCOUNT_IDX 0
#endif

void addIncrefs(llvm::Value* v, bool nullable, int num_refs, llvm::Instruction* incref_pt) {
    if (num_refs > 1) {
        // Not bad but I don't think this should happen:
        // printf("Whoa more than one incref??\n");
        // raise(SIGTRAP);
    }

    if (isa<ConstantPointerNull>(v)) {
        assert(nullable);
        return;
    }

    assert(num_refs > 0);

    llvm::BasicBlock* cur_block;
    llvm::BasicBlock* continue_block = NULL;
    llvm::BasicBlock* incref_block;

    llvm::IRBuilder<true> builder(incref_pt);

    // Deal with subtypes of Box:
    while (v->getType() != g.llvm_value_type_ptr) {
        v = builder.CreateConstInBoundsGEP2_32(v, 0, 0);
    }

    if (nullable) {
        cur_block = incref_pt->getParent();
        continue_block = cur_block->splitBasicBlock(incref_pt);
        incref_block
            = llvm::BasicBlock::Create(g.context, "incref", incref_pt->getParent()->getParent(), continue_block);

        assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
        cur_block->getTerminator()->eraseFromParent();

        builder.SetInsertPoint(cur_block);
        auto isnull = builder.CreateICmpEQ(v, getNullPtr(g.llvm_value_type_ptr));
        builder.CreateCondBr(isnull, continue_block, incref_block);

        builder.SetInsertPoint(incref_block);
    }

#ifdef Py_REF_DEBUG
    auto reftotal_gv = g.cur_module->getOrInsertGlobal("_Py_RefTotal", g.i64);
    auto reftotal = builder.CreateLoad(reftotal_gv);
    auto new_reftotal = builder.CreateAdd(reftotal, getConstantInt(num_refs, g.i64));
    builder.CreateStore(new_reftotal, reftotal_gv);
#endif

    auto refcount_ptr = builder.CreateConstInBoundsGEP2_32(v, 0, REFCOUNT_IDX);
    auto refcount = builder.CreateLoad(refcount_ptr);
    auto new_refcount = builder.CreateAdd(refcount, getConstantInt(num_refs, g.i64));
    builder.CreateStore(new_refcount, refcount_ptr);

    if (nullable)
        builder.CreateBr(continue_block);
}

void addDecrefs(llvm::Value* v, bool nullable, int num_refs, llvm::Instruction* decref_pt) {
    if (num_refs > 1) {
        // Not bad but I don't think this should happen:
        printf("Whoa more than one decref??\n");
        raise(SIGTRAP);
    }

    // TODO -- assert that v isn't a constant None?  Implies wasted extra increfs/decrefs

    if (isa<ConstantPointerNull>(v)) {
        assert(nullable);
        return;
    }

    assert(num_refs > 0);
    llvm::IRBuilder<true> builder(decref_pt);

    if (nullable) {
        llvm::BasicBlock* cur_block = decref_pt->getParent();
        llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(decref_pt);
        llvm::BasicBlock* decref_block
            = llvm::BasicBlock::Create(g.context, "decref", decref_pt->getParent()->getParent(), continue_block);

        assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
        cur_block->getTerminator()->eraseFromParent();

        builder.SetInsertPoint(cur_block);
        auto isnull = builder.CreateICmpEQ(v, getNullPtr(v->getType()));
        builder.CreateCondBr(isnull, continue_block, decref_block);

        builder.SetInsertPoint(decref_block);
        auto jmp = builder.CreateBr(continue_block);
        addDecrefs(v, false, num_refs, jmp);
        return;
    }

    RELEASE_ASSERT(num_refs == 1, "decref patchpoints don't support >1 refs");

    llvm::Function* patchpoint
        = llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::experimental_patchpoint_void);
    int pp_id = nullable ? XDECREF_PP_ID : DECREF_PP_ID;
    int pp_size = nullable ? XDECREF_PP_SIZE : DECREF_PP_SIZE;
    builder.CreateCall(patchpoint, { getConstantInt(pp_id, g.i64), getConstantInt(pp_size, g.i32), getNullPtr(g.i8_ptr),
                                     getConstantInt(1, g.i32), v });

#if 0
    // Deal with subtypes of Box:
    while (v->getType() != g.llvm_value_type_ptr) {
        v = builder.CreateConstInBoundsGEP2_32(v, 0, 0);
    }

#ifdef Py_REF_DEBUG
    auto reftotal_gv = g.cur_module->getOrInsertGlobal("_Py_RefTotal", g.i64);
    auto reftotal = new llvm::LoadInst(reftotal_gv, "", decref_pt);
    auto new_reftotal = llvm::BinaryOperator::Create(llvm::BinaryOperator::BinaryOps::Sub, reftotal,
                                                     getConstantInt(num_refs, g.i64), "", decref_pt);
    new llvm::StoreInst(new_reftotal, reftotal_gv, decref_pt);
#endif

    llvm::ArrayRef<llvm::Value*> idxs({ getConstantInt(0, g.i32), getConstantInt(REFCOUNT_IDX, g.i32) });
    auto refcount_ptr = llvm::GetElementPtrInst::CreateInBounds(v, idxs, "", decref_pt);
    auto refcount = new llvm::LoadInst(refcount_ptr, "", decref_pt);
    auto new_refcount = llvm::BinaryOperator::Create(llvm::BinaryOperator::BinaryOps::Sub, refcount,
                                                     getConstantInt(num_refs, g.i64), "", decref_pt);
    new llvm::StoreInst(new_refcount, refcount_ptr, decref_pt);

#ifdef Py_REF_DEBUG
    llvm::CallInst::Create(g.funcs.checkRefs, { v }, "", decref_pt);
#endif

    llvm::BasicBlock* cur_block = decref_pt->getParent();
    llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(decref_pt);
    llvm::BasicBlock* dealloc_block
        = llvm::BasicBlock::Create(g.context, "dealloc", decref_pt->getParent()->getParent(), continue_block);

    assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
    cur_block->getTerminator()->eraseFromParent();

    builder.SetInsertPoint(cur_block);
    auto iszero = builder.CreateICmpEQ(new_refcount, getConstantInt(0, g.i64));
    builder.CreateCondBr(iszero, dealloc_block, continue_block);

    builder.SetInsertPoint(dealloc_block);

#ifdef COUNT_ALLOCS
#error "Don't support COUNT_ALLOCS here yet"
#endif

#ifdef Py_TRACE_REFS
    builder.CreateCall(g.funcs._Py_Dealloc, v);
#else
    auto cls_ptr = builder.CreateConstInBoundsGEP2_32(v, 0, 1 + REFCOUNT_IDX);
    auto cls = builder.CreateLoad(cls_ptr);
    auto dtor_ptr = builder.CreateConstInBoundsGEP2_32(cls, 0, 4);

#ifndef NDEBUG
    llvm::APInt offset(64, 0, true);
    assert(llvm::cast<llvm::GetElementPtrInst>(dtor_ptr)->accumulateConstantOffset(*g.tm->getDataLayout(), offset));
    assert(offset.getZExtValue() == offsetof(BoxedClass, tp_dealloc));
#endif
    auto dtor = builder.CreateLoad(dtor_ptr);
    builder.CreateCall(dtor, v);
#endif

    builder.CreateBr(continue_block);

    builder.SetInsertPoint(continue_block);
#endif
}

void addCXXFixup(llvm::Instruction* inst, const llvm::SmallVector<llvm::TrackingVH<llvm::Value>, 4>& to_decref,
                 RefcountTracker* rt) {
    // inst->getParent()->getParent()->dump();
    // inst->dump();

    ASSERT(!llvm::isa<llvm::InvokeInst>(inst),
           "don't need a fixup here!"); // could either not ask for the fixup or maybe just skip it here
    assert(llvm::isa<llvm::CallInst>(inst));

    llvm::CallInst* call = llvm::cast<llvm::CallInst>(inst);

    llvm::BasicBlock* cur_block = inst->getParent();
    llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(inst);
    llvm::BasicBlock* fixup_block
        = llvm::BasicBlock::Create(g.context, "cxx_fixup", inst->getParent()->getParent(), continue_block);

    assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
    cur_block->getTerminator()->eraseFromParent();

    // llvm::SmallVector<llvm::Value*, 4> args(call->arg_begin(), call->arg_end());
    llvm::SmallVector<llvm::Value*, 4> args(call->arg_operands().begin(), call->arg_operands().end());
    llvm::InvokeInst* new_invoke
        = InvokeInst::Create(call->getCalledValue(), continue_block, fixup_block, args, call->getName(), cur_block);
    new_invoke->setAttributes(call->getAttributes());
    new_invoke->setDebugLoc(call->getDebugLoc());
    assert(!call->hasMetadataOtherThanDebugLoc());
    call->replaceAllUsesWith(new_invoke);
    call->eraseFromParent();

    llvm::Value* exc_type, *exc_value, *exc_traceback;
    std::tie(exc_type, exc_value, exc_traceback) = createLandingpad(fixup_block);

    llvm::IRBuilder<true> builder(fixup_block);

    llvm::SmallVector<llvm::Value*, 4> decref_args;
    decref_args.push_back(getConstantInt(to_decref.size(), g.i32));
    decref_args.append(to_decref.begin(), to_decref.end());
    builder.CreateCall(g.funcs.xdecrefAll, decref_args);

    auto rethrow = builder.CreateCall3(g.funcs.rawReraise, exc_type, exc_value, exc_traceback);
    builder.CreateUnreachable();

    // new_invoke->getParent()->getParent()->dump();
}

// TODO: this should be cleaned up and moved to src/core/
template <typename K, typename V> class OrderedMap {
private:
    llvm::DenseMap<K, V> map;
    std::vector<K> order;

public:
    V& operator[](const K& k) {
        if (map.count(k) == 0)
            order.push_back(k);

        return map[k];
    }

    const V& get(const K& k) const { return map.find(k)->second; }

    size_t size() const {
        assert(order.size() == map.size());
        return order.size();
    }

    void clear() {
        map.clear();
        order.clear();
    }

    // TODO: this is slow
    void erase(const K& k) {
        assert(map.count(k));
        map.erase(k);
        order.erase(std::remove(order.begin(), order.end(), k), order.end());
    }

    class const_iterator {
    private:
        const OrderedMap* themap;
        int idx;

        const_iterator(const OrderedMap* themap, int idx) : themap(themap), idx(idx) {}

    public:
        std::pair<K, V> operator*() { return *themap->map.find(themap->order[idx]); }
        std::pair<K, V> operator->() { return *this; }

        bool operator==(const const_iterator& rhs) const {
            assert(themap == rhs.themap);
            return idx == rhs.idx;
        }
        bool operator!=(const const_iterator& rhs) const { return !(*this == rhs); }

        const_iterator& operator++() {
            idx++;
            return *this;
        }

        friend class OrderedMap;
    };

    size_t count(K& k) const { return map.count(k); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size()); }
};

static std::vector<llvm::BasicBlock*> computeTraversalOrder(llvm::Function* f) {
    std::vector<llvm::BasicBlock*> ordering;
    llvm::DenseSet<llvm::BasicBlock*> added;
    llvm::DenseMap<llvm::BasicBlock*, int> num_successors_added;

    for (auto&& BB : *f) {
        if (llvm::succ_begin(&BB) == llvm::succ_end(&BB)) {
            // llvm::outs() << "Adding " << BB.getName() << " since it is an exit node.\n";
            ordering.push_back(&BB);
            added.insert(&BB);
        }
    }

    int check_predecessors_idx = 0;
    int num_bb = f->size();
    while (ordering.size() < num_bb) {
        if (check_predecessors_idx < ordering.size()) {
            // Case 1: look for any blocks whose successors have already been traversed.

            llvm::BasicBlock* bb = ordering[check_predecessors_idx];
            check_predecessors_idx++;

            for (auto&& PBB : llvm::iterator_range<llvm::pred_iterator>(llvm::pred_begin(bb), llvm::pred_end(bb))) {
                if (added.count(PBB))
                    continue;

                num_successors_added[PBB]++;
                int num_successors = std::distance(llvm::succ_begin(PBB), llvm::succ_end(PBB));
                if (num_successors_added[PBB] == num_successors) {
                    ordering.push_back(PBB);
                    added.insert(PBB);
                    // llvm::outs() << "Adding " << PBB->getName() << " since it has all of its successors added.\n";
                }
            }
        } else {
            // Case 2: we hit a cycle.  Try to pick a good node to add.
            // The heuristic here is just to make sure to pick one in 0-successor component of the SCC

            std::vector<std::pair<llvm::BasicBlock*, int>> num_successors;
            // TODO this could be sped up:
            for (auto&& BB : *f) {
                if (!num_successors_added.count(&BB))
                    continue;
                if (added.count(&BB))
                    continue;
                num_successors.push_back(std::make_pair(&BB, num_successors_added[&BB]));
            }

            std::sort(num_successors.begin(), num_successors.end(),
                      [](const std::pair<llvm::BasicBlock*, int>& p1, const std::pair<llvm::BasicBlock*, int>& p2) {
                          return p1.second > p2.second;
                      });

            std::deque<llvm::BasicBlock*> visit_queue;
            llvm::DenseSet<llvm::BasicBlock*> visited;
            llvm::BasicBlock* best = NULL;

            for (auto&& p : num_successors) {
                if (visited.count(p.first))
                    continue;

                best = p.first;
                visit_queue.push_back(p.first);
                visited.insert(p.first);

                while (visit_queue.size()) {
                    llvm::BasicBlock* bb = visit_queue.front();
                    visit_queue.pop_front();

                    for (auto&& SBB : llvm::successors(bb)) {
                        if (!visited.count(SBB)) {
                            visited.insert(SBB);
                            visit_queue.push_back(SBB);
                        }
                    }
                }
            }

#ifndef NDEBUG
            // This can currently get tripped if we generate an LLVM IR that has an infinite loop in it.
            // This could definitely be supported, but I don't think we should be generating those cases anyway.
            if (!best) {
                for (auto& BB : ordering) {
                    llvm::outs() << "added to " << BB->getName() << '\n';
                }
                for (auto& BB : *f) {
                    if (added.count(&BB) == 0)
                        llvm::outs() << "never got to " << BB.getName() << '\n';
                }
            }
#endif
            assert(best);
            ordering.push_back(best);
            added.insert(best);
            // llvm::outs() << "Adding " << best->getName() << " since it is the best provisional node.\n";
        }
    }

    assert(ordering.size() == num_bb);
    assert(added.size() == num_bb);
    return ordering;
}

class BlockOrderer {
private:
    llvm::DenseMap<llvm::BasicBlock*, int> priority; // lower goes first

    struct BlockComparer {
        bool operator()(std::pair<llvm::BasicBlock*, int> lhs, std::pair<llvm::BasicBlock*, int> rhs) {
            assert(lhs.second != rhs.second);
            return lhs.second > rhs.second;
        }
    };

    llvm::DenseSet<llvm::BasicBlock*> in_queue;
    std::priority_queue<std::pair<llvm::BasicBlock*, int>, std::vector<std::pair<llvm::BasicBlock*, int>>,
                        BlockComparer> queue;

public:
    BlockOrderer(std::vector<llvm::BasicBlock*> order) {
        for (int i = 0; i < order.size(); i++) {
            // errs() << "DETERMINISM: " << order[i]->getName() << " has priority " << i << '\n';
            priority[order[i]] = i;
        }
    }

    void add(llvm::BasicBlock* b) {
        // errs() << "DETERMINISM: adding " << b->getName() << '\n';
        assert(in_queue.size() == queue.size());
        if (in_queue.count(b))
            return;
        assert(priority.count(b));
        in_queue.insert(b);
        queue.push(std::make_pair(b, priority[b]));
    }

    llvm::BasicBlock* pop() {
        if (!queue.size()) {
            assert(!in_queue.size());
            return NULL;
        }

        llvm::BasicBlock* b = queue.top().first;
        // errs() << "DETERMINISM: popping " << b->getName() << " (priority " << priority[b] << " \n";
        queue.pop();
        assert(in_queue.count(b));
        in_queue.erase(b);
        return b;
    }
};

typedef OrderedMap<llvm::Value*, int> BlockMap;
bool endingRefsDifferent(const BlockMap& lhs, const BlockMap& rhs) {
    if (lhs.size() != rhs.size())
        return true;
    for (auto&& p : lhs) {
        if (rhs.count(p.first) == 0)
            return true;
        if (p.second != rhs.get(p.first))
            return true;
    }
    return false;
}

void RefcountTracker::addRefcounts(IRGenState* irstate) {
    Timer _t("refcounting");

    llvm::Function* f = irstate->getLLVMFunction();
    RefcountTracker* rt = irstate->getRefcounts();

    if (VERBOSITY() >= 2) {
        fprintf(stderr, "Before refcounts:\n");
        fprintf(stderr, "\033[35m");
        dumpPrettyIR(f);
        fprintf(stderr, "\033[0m");
    }

#ifndef NDEBUG
    int num_untracked = 0;
    auto check_val_missed = [&](llvm::Value* v) {
        if (rt->vars.count(v))
            return;

        auto t = v->getType();
        auto p = llvm::dyn_cast<llvm::PointerType>(t);
        if (!p) {
            // t->dump();
            // printf("Not a pointer\n");
            return;
        }
        auto s = llvm::dyn_cast<llvm::StructType>(p->getElementType());
        if (!s) {
            // t->dump();
            // printf("Not a pointer\n");
            return;
        }

        // Take care of inheritance.  It's represented as an instance of the base type at the beginning of the
        // derived type, not as the types concatenated.
        while (s->elements().size() > 0 && llvm::isa<llvm::StructType>(s->elements()[0]))
            s = llvm::cast<llvm::StructType>(s->elements()[0]);

        if (isa<ConstantPointerNull>(v))
            return;

        bool ok_type = false;
        if (s->elements().size() >= 2 + REFCOUNT_IDX && s->elements()[REFCOUNT_IDX] == g.i64
            && s->elements()[REFCOUNT_IDX + 1] == g.llvm_class_type_ptr) {
            // printf("This looks likes a class\n");
            ok_type = true;
        }

        if (!ok_type) {
#ifndef NDEBUG
            if (s->getName().startswith("struct.pyston::Box")
                || (s->getName().startswith("Py") && s->getName().endswith("Object"))
                || s->getName().startswith("class.pyston::Box")) {
                v->dump();
                s->dump();
                if (s && s->elements().size() >= 2) {
                    s->elements()[0]->dump();
                    s->elements()[1]->dump();
                }
                fprintf(stderr, "This is named like a refcounted object though it doesn't look like one");
                assert(0);
            }
#endif
            return;
        }

        if (rt->vars.count(v) == 0) {
            num_untracked++;
            printf("missed a refcounted object: ");
            fflush(stdout);
            v->dump();
            abort();
        }
    };

    for (auto&& g : f->getParent()->getGlobalList()) {
        // g.dump();
        check_val_missed(&g);
    }

    for (auto&& a : f->args()) {
        check_val_missed(&a);
    }

    for (auto&& BB : *f) {
        for (auto&& inst : BB) {
            check_val_missed(&inst);
            for (auto&& u : inst.uses()) {
                check_val_missed(u.get());
            }
            for (auto&& op : inst.operands()) {
                check_val_missed(op);
            }
        }
    }
    ASSERT(num_untracked == 0, "");
#endif

    struct RefOp {
        llvm::TrackingVH<llvm::Value> operand;
        bool nullable;
        int num_refs;

        // Exactly one of these should be NULL:
        llvm::Instruction* insertion_inst;
        llvm::BasicBlock* insertion_bb;
        llvm::BasicBlock* insertion_from_bb;
    };

    struct RefState {
        // We do a backwards scan and starting/ending here refers to the scan, not the instruction sequence.
        // So "starting_refs" are the refs that are inherited, ie the refstate at the end of the basic block.
        // "ending_refs" are the refs we calculated, which corresponds to the refstate at the beginning of the block.
        BlockMap starting_refs;
        BlockMap ending_refs;

        llvm::SmallVector<RefOp, 4> increfs;
        llvm::SmallVector<RefOp, 4> decrefs;

        struct CXXFixup {
            llvm::Instruction* inst;
            llvm::SmallVector<llvm::TrackingVH<llvm::Value>, 4> to_decref;
        };
        llvm::SmallVector<CXXFixup, 4> cxx_fixups;
    };
    llvm::DenseMap<llvm::BasicBlock*, RefState> states;

    BlockOrderer orderer(computeTraversalOrder(f));
    for (auto&& BB : *f) {
        orderer.add(&BB);
    }

    std::vector<llvm::InvokeInst*> invokes;
    for (auto&& II : llvm::inst_range(f)) {
        llvm::Instruction* inst = &II;
        if (!rt->vars.count(inst))
            continue;
        if (auto ii = dyn_cast<InvokeInst>(inst))
            invokes.push_back(ii);
    }

    while (llvm::BasicBlock* bb = orderer.pop()) {
        // errs() << "DETERMINISM: Processing " << bb->getName() << '\n';
        llvm::BasicBlock& BB = *bb;

#if 0
        llvm::Instruction* term_inst = BB.getTerminator();
        llvm::Instruction* insert_before = term_inst;
        if (llvm::isa<llvm::UnreachableInst>(insert_before)) {
            insert_before = &*(++BB.rbegin());
            assert(llvm::isa<llvm::CallInst>(insert_before) || llvm::isa<llvm::IntrinsicInst>(insert_before));
        }
#endif

        if (VERBOSITY() >= 2) {
            llvm::outs() << '\n';
            llvm::outs() << "Processing " << BB.getName() << '\n';
        }

        bool firsttime = (states.count(&BB) == 0);
        RefState& state = states[&BB];

        BlockMap orig_ending_refs = std::move(state.ending_refs);

        state.starting_refs.clear();
        state.ending_refs.clear();
        state.increfs.clear();
        state.decrefs.clear();
        state.cxx_fixups.clear();

        // Compute the incoming refstate based on the refstate of any successor nodes
        llvm::SmallVector<llvm::BasicBlock*, 4> successors;
        for (auto SBB : llvm::successors(&BB)) {
            if (states.count(SBB))
                successors.push_back(SBB);
        }
        if (successors.size()) {
            std::vector<llvm::Value*> tracked_values;
            llvm::DenseSet<llvm::Value*> in_tracked_values;
            for (auto SBB : successors) {
                // errs() << "DETERMINISM: successor " << SBB->getName() << '\n';
                assert(states.count(SBB));
                for (auto&& p : states[SBB].ending_refs) {
                    // errs() << "DETERMINISM: looking at ref " << p.first->getName() << '\n';
                    assert(p.second > 0);
                    if (!in_tracked_values.count(p.first)) {
                        in_tracked_values.insert(p.first);
                        tracked_values.push_back(p.first);
                    }
                }
            }

            llvm::SmallVector<std::pair<BlockMap*, llvm::BasicBlock*>, 4> successor_info;
            for (auto SBB : successors) {
                successor_info.emplace_back(&states[SBB].ending_refs, SBB);
            }

            // size_t hash = 0;
            for (auto v : tracked_values) {
                // hash = hash * 31 + std::hash<llvm::StringRef>()(v->getName());
                assert(rt->vars.count(v));
                const auto refstate = rt->vars.lookup(v);

                int min_refs = 1000000000;
                for (auto&& s_info : successor_info) {
                    if (s_info.first->count(v)) {
                        // llvm::outs() << "Going from " << BB.getName() << " to " << SBB->getName() << ", have "
                        //<< it->second << " refs on " << *v << '\n';
                        min_refs = std::min((*s_info.first)[v], min_refs);
                    } else {
                        // llvm::outs() << "Going from " << BB.getName() << " to " << SBB->getName()
                        //<< ", have 0 (missing) refs on " << *v << '\n';
                        min_refs = 0;
                    }
                }

                if (refstate.reftype == RefType::OWNED)
                    min_refs = std::max(1, min_refs);

                for (auto&& s_info : successor_info) {
                    int this_refs = 0;
                    if (s_info.first->count(v))
                        this_refs = (*s_info.first)[v];

                    if (this_refs > min_refs) {
                        // llvm::outs() << "Going from " << BB.getName() << " to " << SBB->getName() << ", need to add "
                        //<< (this_refs - min_refs) << " refs to " << *v << '\n';
                        state.increfs.push_back(
                            RefOp({ v, refstate.nullable, this_refs - min_refs, NULL, s_info.second, bb }));
                    } else if (this_refs < min_refs) {
                        assert(refstate.reftype == RefType::OWNED);
                        state.decrefs.push_back(
                            RefOp({ v, refstate.nullable, min_refs - this_refs, NULL, s_info.second, bb }));
                    }
                }

                if (min_refs)
                    state.starting_refs[v] = min_refs;
                else
                    assert(state.starting_refs.count(v) == 0);
            }
            // errs() << "DETERMINISM: tracked value name hash: " << hash << '\n';
        }

        state.ending_refs = state.starting_refs;

        // Then, iterate backwards through the instructions in this BB, updating the ref states
        for (auto& I : llvm::iterator_range<llvm::BasicBlock::reverse_iterator>(BB.rbegin(), BB.rend())) {
            // Phis get special handling:
            // - we only use one of the operands to the phi node (based on the block we came from)
            // - the phi-node-generator is supposed to handle that by putting a refConsumed on the terminator of the
            // previous block
            // - that refConsumed will caus a use as well.
            auto inst = &I;

            if (!isa<InvokeInst>(inst) && rt->vars.count(&I)) {
                const auto&& rstate = rt->vars.lookup(inst);
                int starting_refs = (rstate.reftype == RefType::OWNED ? 1 : 0);
                if (state.ending_refs[inst] != starting_refs) {
                    llvm::Instruction* insertion_pt = NULL;
                    llvm::BasicBlock* insertion_block = NULL, * insertion_from_block = NULL;
                    insertion_pt = inst->getNextNode();
                    while (llvm::isa<llvm::PHINode>(insertion_pt)) {
                        insertion_pt = insertion_pt->getNextNode();
                    }

                    if (state.ending_refs[inst] < starting_refs) {
                        assert(rstate.reftype == RefType::OWNED);
                        state.decrefs.push_back(RefOp({ inst, rstate.nullable, starting_refs - state.ending_refs[inst],
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    } else {
                        state.increfs.push_back(RefOp({ inst, rstate.nullable, state.ending_refs[inst] - starting_refs,
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    }
                }
                state.ending_refs.erase(inst);
            }

            if (llvm::isa<llvm::PHINode>(&I))
                continue;


            // If we are about to insert a CXX fixup, do the increfs after the call, rather than trying to push
            // them before the call and having to insert decrefs on the fixup path.
            if (rt->may_throw.count(&I)) {
                llvm::SmallVector<llvm::Value*, 4> to_erase;
                for (auto&& p : state.ending_refs) {
                    int needed_refs = (rt->vars.lookup(p.first).reftype == RefType::OWNED ? 1 : 0);
                    if (p.second > needed_refs) {
                        state.increfs.push_back(RefOp({ p.first, rt->vars.lookup(p.first).nullable,
                                                        p.second - needed_refs, I.getNextNode(), NULL, NULL }));
                    }

                    state.ending_refs[p.first] = needed_refs;
                    if (needed_refs == 0)
                        to_erase.push_back(p.first);
                }

                for (auto v : to_erase)
                    state.ending_refs.erase(v);
            }

            llvm::DenseMap<llvm::Value*, int> num_consumed_by_inst;
            OrderedMap<llvm::Value*, int> num_times_as_op;

            for (auto v : rt->refs_consumed[&I]) {
                num_consumed_by_inst[v]++;
                assert(rt->vars.count(v) && rt->vars.lookup(v).reftype != RefType::UNKNOWN);
                num_times_as_op[v]; // just make sure it appears in there
            }

            for (auto v : rt->refs_used[&I]) {
                assert(rt->vars.lookup(v).reftype != RefType::UNKNOWN);
                num_times_as_op[v]++;
            }

            for (llvm::Value* op : I.operands()) {
                auto it = rt->vars.find(op);
                if (it == rt->vars.end())
                    continue;

                num_times_as_op[op]++;
            }

            // First, calculate anything we need to keep alive through the end of the function call:
            for (auto&& p : num_times_as_op) {
                auto& op = p.first;

                auto&& it = num_consumed_by_inst.find(op);
                int num_consumed = 0;
                if (it != num_consumed_by_inst.end())
                    num_consumed = it->second;

                if (num_times_as_op[op] > num_consumed) {
                    if (rt->vars.lookup(op).reftype == RefType::OWNED) {
                        if (state.ending_refs[op] == 0) {
                            // llvm::outs() << "Last use of " << *op << " is at " << I << "; adding a decref after\n";

                            if (llvm::InvokeInst* invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
                                state.decrefs.push_back(
                                    RefOp({ op, rt->vars.lookup(op).nullable, 1, NULL, invoke->getNormalDest(), bb }));
                                state.decrefs.push_back(
                                    RefOp({ op, rt->vars.lookup(op).nullable, 1, NULL, invoke->getUnwindDest(), bb }));
                            } else {
                                assert(&I != I.getParent()->getTerminator());
                                auto next = I.getNextNode();
                                // while (llvm::isa<llvm::PHINode>(next))
                                // next = next->getNextNode();
                                if (llvm::isa<llvm::UnreachableInst>(next)) {
                                    // ASSERT(!llvm::isa<llvm::UnreachableInst>(next),
                                    //"Can't add decrefs after this function...");
                                    assert(rt->may_throw.count(&I));
                                } else {
                                    state.decrefs.push_back(
                                        RefOp({ op, rt->vars.lookup(op).nullable, 1, next, NULL, NULL }));
                                }
                            }
                            state.ending_refs[op] = 1;
                        }
                    }
                }
            }

            if (rt->may_throw.count(&I)) {
                // TODO: pump out any increfs rather than pushing them before this.

                state.cxx_fixups.emplace_back();
                auto&& fixup = state.cxx_fixups.back();
                fixup.inst = &I;
                for (auto&& p : state.ending_refs) {
                    for (int i = 0; i < p.second; i++) {
                        assert(rt->vars.count(p.first));
                        fixup.to_decref.push_back(p.first);
                    }
                }

                if (fixup.to_decref.empty())
                    state.cxx_fixups.pop_back();
            }

            // Lastly, take care of any stolen refs.  This happens regardless of whether an exception gets thrown,
            // so it goes after that handling (since we are processing in reverse).
            for (auto&& p : num_times_as_op) {
                auto& op = p.first;

                auto&& it = num_consumed_by_inst.find(op);
                int num_consumed = 0;
                if (it != num_consumed_by_inst.end())
                    num_consumed = it->second;

                if (num_consumed)
                    state.ending_refs[op] += num_consumed;
            }
        }

        if (VERBOSITY() >= 2) {
            llvm::outs() << "End of " << BB.getName() << '\n';
            if (VERBOSITY() >= 3) {
                for (auto&& p : state.ending_refs) {
                    llvm::outs() << *p.first << ": " << p.second << '\n';
                }
            }
        }


        // Invokes are special.  Handle them here by treating them as if they happened in their normal-dest block.
        for (InvokeInst* ii : invokes) {
            const auto&& rstate = rt->vars.lookup(ii);

            if (ii->getNormalDest() == &BB) {
                // TODO: duplicated with the non-invoke code
                int starting_refs = (rstate.reftype == RefType::OWNED ? 1 : 0);
                if (state.ending_refs[ii] != starting_refs) {
                    llvm::Instruction* insertion_pt = NULL;
                    llvm::BasicBlock* insertion_block = NULL, * insertion_from_block = NULL;

                    insertion_block = bb;
                    insertion_from_block = ii->getParent();

                    if (state.ending_refs[ii] < starting_refs) {
                        assert(rstate.reftype == RefType::OWNED);
                        state.decrefs.push_back(RefOp({ ii, rstate.nullable, starting_refs - state.ending_refs[ii],
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    } else {
                        state.increfs.push_back(RefOp({ ii, rstate.nullable, state.ending_refs[ii] - starting_refs,
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    }
                }
                state.ending_refs.erase(ii);
            }
        }

        // If this is the entry block, finish dealing with the ref state rather than handing off to a predecessor
        if (&BB == &BB.getParent()->front()) {
            for (auto&& p : state.ending_refs) {
                assert(p.second);

// Anything left should either be an argument, constant or global variable
#ifndef NDEBUG
                if (!llvm::isa<llvm::GlobalVariable>(p.first) && !llvm::isa<llvm::Constant>(p.first)) {
                    bool found = false;
                    for (auto&& arg : f->args()) {
                        if (&arg == p.first) {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        llvm::outs() << "Couldn't find " << *p.first << '\n';
                    assert(found);
                }
#endif
                assert(rt->vars.lookup(p.first).reftype == RefType::BORROWED);

                state.increfs.push_back(
                    RefOp({ p.first, rt->vars.lookup(p.first).nullable, p.second, NULL, &BB, NULL }));
            }
            state.ending_refs.clear();
        }

        // It is possible that we ended with zero live variables, which due to our skipping of un-run blocks,
        // is not the same thing as an un-run block.  Hence the check of 'firsttime'
        if (firsttime || endingRefsDifferent(orig_ending_refs, state.ending_refs)) {
            for (auto&& SBB : llvm::predecessors(&BB)) {
                // llvm::outs() << "reconsidering: " << SBB->getName() << '\n';
                orderer.add(SBB);
            }
        }

        // for (auto&& p : state.ending_refs) {
        // errs() << "DETERMINISM: ending ref: " << p.first->getName() << '\n';
        //}
    }

    ASSERT(states.size() == f->size(), "We didn't process all nodes...");

    // Note: we iterate over the basicblocks in the order they appear in the llvm function;
    // iterating over states directly is nondeterministic.
    std::vector<llvm::BasicBlock*> basic_blocks;
    for (auto&& BB : *f)
        basic_blocks.push_back(&BB);

    // First, find all insertion points.  This may change the CFG by breaking critical edges.
    InsertionCache insertion_pts;
    for (auto bb : basic_blocks) {
        auto&& state = states[bb];
        for (auto& op : state.increfs) {
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
        }
        for (auto& op : state.decrefs) {
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
        }
    }

    // Then use the insertion points (it's the same code but this time it will hit the cache).
    // This may change the CFG by adding decref's
    for (auto bb : basic_blocks) {
        auto&& state = states[bb];
        for (auto& op : state.increfs) {
            assert(rt->vars.count(op.operand));
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
            addIncrefs(op.operand, op.nullable, op.num_refs, insertion_pt);
        }
        for (auto& op : state.decrefs) {
            assert(rt->vars.count(op.operand));
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
            addDecrefs(op.operand, op.nullable, op.num_refs, insertion_pt);
        }

        for (auto&& fixup : state.cxx_fixups) {
            addCXXFixup(fixup.inst, fixup.to_decref, rt);
        }
    }

    long us = _t.end();
    static StatCounter us_refcounting("us_compiling_irgen_refcounting");
    us_refcounting.log(us);
}

} // namespace pyston
