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

#include <queue>
#include <set>
#include <unordered_set>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "codegen/irgen/util.h"
#include "codegen/opt/util.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"

using namespace llvm;

//#undef VERBOSITY
//#define VERBOSITY(...) 2

namespace pyston {

class DeadAllocsPass : public FunctionPass {
private:
    struct ChainInfo {
        // there can be cyclic dependencies; use this to track those:
        std::unordered_set<Instruction*> seen;

        // Instructions that are free to be deleted if the chain is dead:
        std::vector<Instruction*> deletions;
        // Loads that have to be remapped if the chain is dead:
        std::vector<LoadInst*> loads;
    };

    bool canBeRead(llvm::Instruction* v, ChainInfo& chain) {
        if (chain.seen.count(v))
            return false;
        chain.seen.insert(v);

        for (User* user : v->users()) {
            if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(user)) {
                if (canBeRead(gep, chain))
                    return true;
                continue;
            }

            if (BitCastInst* bc = dyn_cast<BitCastInst>(user)) {
                if (canBeRead(bc, chain))
                    return true;
                continue;
            }

            if (PtrToIntInst* pti = dyn_cast<PtrToIntInst>(user)) {
                if (canBeRead(pti, chain))
                    return true;
                continue;
            }

            if (PHINode* phi = dyn_cast<PHINode>(user)) {
                if (canBeRead(phi, chain))
                    return true;
                continue;
            }


            // Can't call canBeRead after this point:
            chain.seen.insert(cast<Instruction>(user));

            if (StoreInst* si = dyn_cast<StoreInst>(user)) {
                if (si->getPointerOperand() == v) {
                    chain.deletions.push_back(si);
                    continue;
                } else {
                    if (VERBOSITY() >= 2)
                        errs() << "Not dead; used here: " << *si << '\n';
                    return true;
                }
            }

            if (MemSetInst* msi = dyn_cast<MemSetInst>(user)) {
                assert(v == msi->getArgOperand(0));
                chain.deletions.push_back(msi);
                continue;
            }

            if (llvm::isa<CallInst>(user) || llvm::isa<InvokeInst>(user)) {
                if (VERBOSITY() >= 2)
                    errs() << "Not dead; used here: " << *user << '\n';
                return true;
            }

            if (ReturnInst* ret = dyn_cast<ReturnInst>(user)) {
                if (VERBOSITY() >= 2)
                    errs() << "Not dead; used here: " << *ret << '\n';
                return true;
            }

            if (LoadInst* li = dyn_cast<LoadInst>(user)) {
                assert(li->getPointerOperand() == v);
                chain.loads.push_back(li);
                continue;
            }

            errs() << *user << '\n';
            RELEASE_ASSERT(0, "");
        }

        chain.deletions.push_back(v);
        return false;
    }

    bool isDerivedFrom(Value* derived, Instruction* ancestor) {
        if (derived == ancestor)
            return true;

        if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(derived)) {
            return isDerivedFrom(gep->getPointerOperand(), ancestor);
        }

        if (BitCastInst* bc = dyn_cast<BitCastInst>(derived)) {
            return isDerivedFrom(bc->getOperand(0), ancestor);
        }

        derived->dump();
        assert(0);
        return false;
    }

    Value* deriveSimilarly(Value* derived, Value* ancestor, Value* new_ancestor, Instruction* insert_before,
                           std::vector<Instruction*>& added) {
        if (derived == ancestor)
            return new_ancestor;

        if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(derived)) {
            std::vector<Value*> indices;
            // No range version of this for now:
            for (GetElementPtrInst::op_iterator it = gep->idx_begin(), end = gep->idx_end(); it != end; ++it) {
                indices.push_back(it->get());
            }

            Value* new_parent = deriveSimilarly(gep->getPointerOperand(), ancestor, new_ancestor, insert_before, added);

            Instruction* rtn
                = GetElementPtrInst::Create(nullptr, new_parent, indices, "t", insert_before);
            if (VERBOSITY() >= 2)
                errs() << "Added: " << *rtn << '\n';
            added.push_back(rtn);
            return rtn;
        }

        if (BitCastInst* bc = dyn_cast<BitCastInst>(derived)) {
            Value* new_parent = deriveSimilarly(bc->getOperand(0), ancestor, new_ancestor, insert_before, added);
            Instruction* rtn = new BitCastInst(new_parent, bc->getType(), "t", insert_before);
            added.push_back(rtn);
            if (VERBOSITY() >= 2)
                errs() << "Added: " << *rtn << '\n';
            return rtn;
        }


        derived->dump();
        RELEASE_ASSERT(0, "");
    }

    // Given a pointer that we're interested in, and an instruction that could potentially change the value of
    // that pointer, return a Value that represents what was stored to the pointer.
    // If the instruction has no effect on the pointer, returns NULL.
    //
    // TODO maybe this should take an AliasAnalysis::Location instead of the bare ptr?
    Value* extractLoadValue(Value* ptr, Instruction* inst, ChainInfo& chain) {
        // We've already determined all of the instructions that are related to this memory access,
        // so we can just check to see if this instruction is potentially-related to our pointer:
        if (chain.seen.count(inst) == 0)
            return NULL;

        AliasAnalysis* aa = &getAnalysis<AliasAnalysis>();
        assert(aa);
        const DataLayout* dl = &inst->getParent()->getModule()->getDataLayout();
        assert(dl);

        Type* elt_type = cast<PointerType>(ptr->getType())->getElementType();
        AliasAnalysis::Location ptr_loc(ptr, dl->getTypeStoreSize(elt_type));

        if (StoreInst* si = dyn_cast<StoreInst>(inst)) {
            AliasAnalysis::AliasResult ar = aa->alias(ptr_loc, aa->getLocation(si));
            if (ar == AliasAnalysis::NoAlias)
                return NULL;

            if (ar == AliasAnalysis::MustAlias) {
                if (ptr->getType() == si->getPointerOperand()->getType()) {
                    return si->getValueOperand();
                }

                if (dl->getTypeStoreSize(elt_type) == dl->getTypeStoreSize(si->getValueOperand()->getType())) {
                    Instruction::CastOps cast_opcode
                        = CastInst::getCastOpcode(si->getValueOperand(), true, elt_type, true);
                    CastInst* ci = CastInst::Create(cast_opcode, si->getValueOperand(), elt_type, "t", si);
                    return ci;
                }

                si->dump();
                RELEASE_ASSERT(0, "");
            }
            errs() << ar << ' ' << *inst << '\n';
            assert(ar != AliasAnalysis::MayAlias);
            assert(ar != AliasAnalysis::PartialAlias);
            RELEASE_ASSERT(0, "");
        }

        if (LoadInst* li = dyn_cast<LoadInst>(inst)) {
            // TODO: could optimize this by just returning this load, which would (hopefully)
            // just end up doing the scanning once):
            // if (li->getPointerOperand() == ptr) {
            // return li;
            //}
            return NULL;
        }

        if (isa<DbgInfoIntrinsic>(inst)) {
            return NULL;
        }

        if (isa<CastInst>(inst)) {
            return NULL;
        }

        if (PHINode* phi = dyn_cast<PHINode>(inst)) {
            if (phi == ptr) {
                assert(0 && "unimplemented");
            }

            // Right now we only handle the case that `ptr` is directly derived from phi.
            // In this case, push the derivation up into the previous blocks, and then
            // get a phi of the resolved loads.
            // Note that it's possible that some of the incoming branches don't come from the
            // allocation that we're trying to get rid of; in this case, just leave
            // those branches as unresolved loads.
            assert(isDerivedFrom(ptr, phi));

            PHINode* load_phi = PHINode::Create(elt_type, phi->getNumIncomingValues(), "t", phi);

            if (VERBOSITY() >= 2)
                errs() << "Derived from phi: " << *phi << "; pushing back and adding " << *load_phi << "\n";
            for (int i = 0, e = phi->getNumIncomingValues(); i < e; i++) {
                BasicBlock* prev_bb = phi->getIncomingBlock(i);
                Value* prev_ptr = phi->getIncomingValue(i);

                std::vector<Instruction*> added;
                Value* prev_derived = deriveSimilarly(ptr, phi, prev_ptr, prev_bb->getTerminator(), added);
                if (VERBOSITY() >= 2)
                    errs() << "Phi-recursing on " << *prev_derived << '\n';
                std::unordered_map<BasicBlock*, Value*> seen;

                llvm::Value* prev_resolved = getLoadValFrom(prev_derived, prev_bb, seen, chain);
                if (prev_resolved == NULL) {
                    if (VERBOSITY() >= 2)
                        errs() << "Wasn't able to resolve " << *prev_derived << "; just emitting a load\n";
                    prev_resolved = new LoadInst(prev_derived, "t", prev_bb->getTerminator());
                } else {
                    if (VERBOSITY() >= 2)
                        errs() << "Resolved " << *prev_derived << " to " << *prev_resolved
                               << "; deleting the temporary instructions\n";
                    for (int i = added.size() - 1; i >= 0; i--) {
                        if (VERBOSITY() >= 2)
                            errs() << "Deleting temporary " << *added[i] << '\n';
                        added[i]->eraseFromParent();
                    }
                }
                // errs() << "Got " << *prev_resolved << " for " << *prev_derived << '\n';
                load_phi->addIncoming(prev_resolved, prev_bb);
            }
            return load_phi;
        }

        if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(inst)) {
            return NULL;
        }

        if (CallInst* ci = dyn_cast<CallInst>(inst)) {
            if (isAllocCall(ci))
                return NULL;

            inst->dump();
            RELEASE_ASSERT(0, "");
        }

        inst->dump();
        RELEASE_ASSERT(0, "");
    }

    // Extract a Value corresponding to the value of this pointer, potentially traversing the CFG.
    // Starts looking a the end of this BB and working backwards.
    Value* getLoadValFrom(Value* ptr, BasicBlock* bb, std::unordered_map<BasicBlock*, Value*>& seen, ChainInfo& chain) {
        // No range version of this for now:
        for (auto it = bb->rbegin(), end = bb->rend(); it != end; ++it) {
            Value* v = extractLoadValue(ptr, &*it, chain);
            if (v == NULL)
                continue;
            return v;
        }
        // errs() << "Not in bb " << bb << ", so trying to do phi-reconstruction\n";
        return getLoadValFromPrevious(ptr, bb, seen, chain);
    }

    // Extract a Value corresponding to the value of this pointer, potentially traversing the CFG.
    // Starts looking a the beginning of this BB, ie at its predecessors
    Value* getLoadValFromPrevious(Value* ptr, BasicBlock* bb, std::unordered_map<BasicBlock*, Value*>& seen,
                                  ChainInfo& chain) {
        Value*& r = seen[bb];
        if (r != NULL)
            return r;

        auto prev_bb = bb->getUniquePredecessor();
        if (prev_bb) {
            return r = getLoadValFrom(ptr, prev_bb, seen, chain);
        }

        PHINode* phi = PHINode::Create(cast<PointerType>(ptr->getType())->getElementType(), bb->getNumUses(), "t",
                                       bb->getFirstNonPHI());
        r = phi;
        if (VERBOSITY() >= 2)
            errs() << "Added phi " << *phi << " in " << bb->getName() << '\n';

        int num_predecessors = 0;
        // No range version of this for now:
        for (auto prev_bb = pred_begin(bb), end = pred_end(bb); prev_bb != end; ++prev_bb) {
            num_predecessors++;

            if (VERBOSITY() >= 2)
                errs() << "Recursing into " << (*prev_bb)->getName() << '\n';
            Value* v = getLoadValFrom(ptr, *prev_bb, seen, chain);
            if (VERBOSITY() >= 2)
                errs() << "Done recursing into " << (*prev_bb)->getName() << '\n';

            assert(v);
            phi->addIncoming(v, *prev_bb);
        }

        // TODO should just have known this from the start
        if (num_predecessors == 0) {
            phi->eraseFromParent();
            r = NULL;
            return NULL;
        }

        if (VERBOSITY("opt") >= 1)
            errs() << "Finished adding phi in " << bb->getName() << ": " << *phi << '\n';
        return phi;
    }

    // Remap a load that we have determined points to non-escaped memory.
    //
    // Maybe this could be implemented in terms of AA + GVN?
    // This is pretty similar, but with slightly different assumptions about the
    // memory model so I'm not sure it's a natural fit (not saying it can't be done)>
    void remapLoad(LoadInst* li, ChainInfo& chain) {
        if (VERBOSITY("opt")) {
            // li->getParent()->getParent()->dump();
            errs() << "\nRemapping " << *li << '\n';
        }

        BasicBlock::reverse_iterator it = li->getParent()->rbegin();
        while (&*it != li) {
            ++it;
        }
        ++it;

        Value* ptr = li->getPointerOperand();
        while (it != li->getParent()->rend()) {
            Value* new_v = extractLoadValue(ptr, &*it, chain);
            if (new_v == NULL) {
                ++it;
                continue;
            }

            assert(new_v);
            if (VERBOSITY("opt") >= 1)
                errs() << "Remapped to: " << *new_v << '\n';
            li->replaceAllUsesWith(new_v);
            li->eraseFromParent();
            return;
        }

        std::unordered_map<BasicBlock*, Value*> seen;
        Value* new_v = getLoadValFromPrevious(li->getPointerOperand(), li->getParent(), seen, chain);
        if (!new_v) {
            new_v = llvm::UndefValue::get(li->getType());
        }
        if (VERBOSITY("opt") >= 1)
            errs() << "Remapped to: " << *new_v << '\n';
        llvm::replaceAndRecursivelySimplify(li, new_v);
    }

public:
    static char ID;
    DeadAllocsPass() : FunctionPass(ID) {}

    virtual void getAnalysisUsage(AnalysisUsage& info) const {
        info.setPreservesCFG();
        info.addRequiredTransitive<AliasAnalysis>();
#if LLVMREV < 231270
        info.addRequiredTransitive<DataLayoutPass>();
#endif
    }

    virtual bool runOnFunction(Function& F) {
        int num_changed = 0;

        StatCounter sc_numchains("opt_dead_chains");
        StatCounter sc_numdeleted("opt_dead_insts");
        StatCounter sc_numremapped("opt_dead_remappedloads");

        std::vector<ChainInfo> dead_chains;
        for (inst_iterator inst_it = inst_begin(F), _inst_end = inst_end(F); inst_it != _inst_end; ++inst_it) {
            if (!isAllocCall(dyn_cast<CallInst>(&*inst_it)))
                continue;

            if (VERBOSITY("opt") >= 2) {
                errs() << "\nFound alloc:\n" << *inst_it << '\n';
            }

            ChainInfo chain;
            bool escapes = canBeRead(&*inst_it, chain);
            if (escapes)
                continue;

            if (VERBOSITY("opt") >= 1) {
                errs() << "\nFound dead alloc:" << *inst_it << '\n';
                errs() << "Taking along with it:\n";
                for (const auto I : chain.deletions) {
                    errs() << *I << '\n';
                }
                errs() << "\nLoads that need to be remapped:\n";
                for (const auto I : chain.loads) {
                    errs() << *I << '\n';
                }
            }

            // TODO: bad, lots of copying
            dead_chains.push_back(chain);
            sc_numchains.log();
        }

        // dumpPrettyIR(&F);
        // int i = 0;
        for (auto chain : dead_chains) {
            sc_numremapped.log(chain.loads.size());
            for (LoadInst* L : chain.loads) {
                remapLoad(L, chain);
            }

            sc_numdeleted.log(chain.deletions.size());
            for (const auto I : chain.deletions) {
                I->eraseFromParent();
            }
        }

        return num_changed > 0;
    }
};
char DeadAllocsPass::ID = 0;

FunctionPass* createDeadAllocsPass() {
    return new DeadAllocsPass();
}
}

static RegisterPass<pyston::DeadAllocsPass> X("dead_allocs", "Kill allocations that don't escape", true, false);
