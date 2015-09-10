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

#include <algorithm>

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "codegen/opt/escape_analysis.h"
#include "codegen/opt/util.h"

#ifndef STANDALONE
#include "core/common.h"
#include "core/options.h"

#include "codegen/codegen.h"

#else
#define VERBOSITY(...) 1
#endif

using namespace llvm;

namespace llvm {
void initializePystonAAPass(PassRegistry&);
}

namespace pyston {

class PystonAA : public ImmutablePass, public AliasAnalysis {
private:
    int depth;
    const DataLayout* DL = nullptr;

    void indent() {
        for (int i = 0; i < depth - 1; i++) {
            errs() << "  ";
        }
    }

public:
    static char ID; // Class identification, replacement for typeinfo
    PystonAA() : ImmutablePass(ID), depth(0) { initializePystonAAPass(*PassRegistry::getPassRegistry()); }

#if LLVMREV < 231270
    void initializePass() override { AliasAnalysis::InitializeAliasAnalysis(this); }
#else
    bool doInitialization(Module& M) override {
        DL = &M.getDataLayout();
        InitializeAliasAnalysis(this, DL);
        return true;
    }
#endif

    void getAnalysisUsage(AnalysisUsage& AU) const override {
        AliasAnalysis::getAnalysisUsage(AU);
        AU.addRequired<AliasAnalysis>();
        AU.addRequired<EscapeAnalysis>();
        AU.setPreservesAll();
    }

    AliasResult _alias(const Location& LocA, const Location& LocB) {
        AliasResult base = AliasAnalysis::alias(LocA, LocB);

        if (VERBOSITY("opt.aa") >= 4) {
            indent();
            errs() << "_alias():\n";
            // cast<Instruction>(LocA.Ptr)->getParent()->dump();
            indent();
            errs() << LocA.Size << "  ";
            LocA.Ptr->dump();
            indent();
            errs() << LocB.Size << "  ";
            LocB.Ptr->dump();
            indent();
            errs() << "base: " << base << '\n';
        }

        if (base != MayAlias)
            return base;

        if (LocA.Ptr == LocB.Ptr) {
            assert(0 && "this should be handled by BasicAA???");
            return MustAlias;
        }

        const Location locs[] = { LocA, LocB };

        for (int i = 0; i < 2; i++) {
            const BitCastInst* BI = dyn_cast<BitCastInst>(locs[i].Ptr);
            if (!BI)
                continue;

            const Value* bc_base = *BI->op_begin();
            if (VERBOSITY("opt.aa") >= 4) {
                indent();
                errs() << "loc " << i << " is bitcast, recursing\n";
            }
            AliasResult bc_base_aliases = alias(locs[i ^ 1], Location(bc_base, locs[i].Size));
            if (VERBOSITY("opt.aa") >= 4) {
                indent();
                bc_base->dump();
                indent();
                errs() << "bc base aliases: " << bc_base_aliases << '\n';
            }
            return bc_base_aliases;
        }

        {
            const GetElementPtrInst* GIa, *GIb;
            GIa = dyn_cast<GetElementPtrInst>(LocA.Ptr);
            GIb = dyn_cast<GetElementPtrInst>(LocB.Ptr);
            if (GIa && GIb) {
                const Value* baseA, *baseB;
                baseA = GIa->getPointerOperand();
                baseB = GIb->getPointerOperand();
                assert(baseA);
                assert(baseB);

                if (VERBOSITY("opt.aa") >= 4) {
                    indent();
                    errs() << "2 geps, recursing\n";
                }
                AliasResult bases_alias = alias(Location(baseA), Location(baseB));
                if (VERBOSITY("opt.aa") >= 4) {
                    indent();
                    errs() << "2gep base aliases: " << bases_alias << '\n';
                    indent();
                    LocA.Ptr->dump();
                    indent();
                    LocB.Ptr->dump();
                }

                if (bases_alias == NoAlias)
                    return NoAlias;

                if (bases_alias == MustAlias) {
                    APInt offsetA(64, 0, true), offsetB(64, 0, true);
                    assert(DL);
                    bool accumA = GIa->accumulateConstantOffset(*DL, offsetA);
                    bool accumB = GIb->accumulateConstantOffset(*DL, offsetB);
                    if (accumA && accumB) {
                        if (VERBOSITY("opt.aa") >= 4) {
                            indent();
                            errs() << offsetA << ' ' << LocA.Size << ' ' << offsetB << ' ' << LocB.Size << '\n';
                        }
                        int sizeA = LocA.Size;
                        int sizeB = LocB.Size;
                        if (offsetA == offsetB) {
                            if (sizeA == sizeB)
                                return MustAlias;
                            return PartialAlias;
                        } else if (offsetA.slt(offsetB)) {
                            if (APInt(64, sizeA, true).sle(offsetB - offsetA))
                                return NoAlias;
                            return PartialAlias;
                        } else {
                            if (APInt(64, sizeB, true).sle(offsetA - offsetB))
                                return NoAlias;
                            return PartialAlias;
                        }
                    }
                }

                return MayAlias;
                // RELEASE_ASSERT(0, "");
            }
        }

        for (int i = 0; i < 2; i++) {
            const GetElementPtrInst* GI = dyn_cast<GetElementPtrInst>(locs[i].Ptr);
            if (!GI)
                continue;

            if (!GI->isInBounds())
                continue;
            // ASSERT(GI->getNumIndices() > 1, "%d %u", i, GI->getNumIndices());

            const Value* gep_base = GI->getPointerOperand();
            assert(gep_base);
            if (VERBOSITY("opt.aa") >= 4) {
                indent();
                errs() << "loc " << i << " is gep, recursing\n";
            }
            AliasResult gep_base_aliases = alias(locs[i ^ 1], Location(gep_base));
            if (VERBOSITY("opt.aa") >= 4) {
                indent();
                gep_base->dump();
                indent();
                errs() << "gep base aliases: " << gep_base_aliases << '\n';
            }
            if (gep_base_aliases == NoAlias)
                return NoAlias;
            return MayAlias;
        }

        for (int i = 0; i < 2; i++) {
            const CallInst* I = dyn_cast<CallInst>(locs[i].Ptr);
            if (!I)
                continue;

            Function* F = I->getCalledFunction();
            if (!F)
                continue;

            if (isAllocCall(F->getName()))
                return NoAlias;

            if (F->getName() == "_ZN6pyston2gc13runCollectionEv") {
                assert(0);
                return NoAlias;
            }
        }

        return MayAlias;
    }

    AliasResult alias(const Location& LocA, const Location& LocB) override {
        if (VERBOSITY("opt.aa") >= 4 && depth == 0 && isa<Instruction>(LocA.Ptr)) {
            cast<Instruction>(LocA.Ptr)->getParent()->dump();
        }

        depth++;
        AliasResult rtn = _alias(LocA, LocB);
        if (VERBOSITY("opt.aa") >= 4) {
            indent();
            errs() << "alias():\n";
            indent();
            LocA.Ptr->dump();
            indent();
            LocB.Ptr->dump();
            indent();
            errs() << "result: " << rtn << '\n';
        }
        depth--;
        return rtn;
    }

    // There are multiple (overloaded) "getModRefInfo" functions in AliasAnalysis, and apparently
    // this means you need to add this line:
    using AliasAnalysis::getModRefInfo;
    ModRefResult getModRefInfo(ImmutableCallSite CS, const Location& Loc) override {
        ModRefResult base = AliasAnalysis::getModRefInfo(CS, Loc);
        if (!CS.getCalledFunction())
            return base;

        if (VERBOSITY("opt.aa") >= 4) {
            errs() << "getModRefInfo():\n";
            CS->dump();
            Loc.Ptr->dump();
            outs() << "base: " << base << '\n';
        }

        ModRefResult mask = ModRef;

        StringRef name = CS.getCalledFunction()->getName();
        if (isAllocCall(name)) {
            return NoModRef;
        }

        EscapeAnalysis& escape = getAnalysis<EscapeAnalysis>();
        EscapeAnalysis::EscapeResult escapes = escape.escapes(Loc.Ptr, CS.getInstruction());
        if (escapes != EscapeAnalysis::Escaped) {
            StatCounter num_improved("opt_modref_noescape");
            num_improved.log();
            if (VERBOSITY("opt.aa") >= 4) {
                errs() << "Was able to show that " << *CS.getInstruction() << " can't modify " << *Loc.Ptr << '\n';
            }
            return NoModRef;
        }

        /*if (name == "printf" || name == "my_realloc" || name == "print_space_if_necessary" || name == "write") {
            mask = Ref;
            bool found_alias = false;
            for (User::const_op_iterator op_it = CS.arg_begin(), op_end = CS.arg_end(); op_it != op_end; ++op_it) {
                if (alias(Loc, Location(op_it->get())) != NoAlias) {
                    found_alias = true;
                    break;
                }
            }
            if (!found_alias)
                mask = NoModRef;
        } else if (name == "snprintf" || name == "str_decref" || name == "read" || name == "file_write") {
            mask = ModRef;
            bool found_alias = false;
            for (User::const_op_iterator op_it = CS.arg_begin(), op_end = CS.arg_end(); op_it != op_end; ++op_it) {
                if (alias(Loc, Location(op_it->get())) != NoAlias) {
                    //errs() << '\n';
                    //errs() << *CS.getInstruction() << '\n';
                    //errs() << **op_it << '\n';
                    found_alias = true;
                    break;
                }
            }
            if (!found_alias) {
                mask = NoModRef;
            }
        } else if (name == "my_free" || name == "my_malloc" || name == "close" || name == "int_repr") {
            mask = NoModRef;
        }*/

        return ModRefResult(mask & base);
    }

    void* getAdjustedAnalysisPointer(const void* ID) override {
        if (ID == &AliasAnalysis::ID)
            return (AliasAnalysis*)this;
        return this;
    }
};
char PystonAA::ID = 0;

llvm::ImmutablePass* createPystonAAPass() {
    return new PystonAA();
}
}

using namespace pyston;
INITIALIZE_AG_PASS(PystonAA, AliasAnalysis, "pystonaa", "Pyston AA", false, true, false)

namespace {
struct Foo {
    Foo() { initializePystonAAPass(*PassRegistry::getPassRegistry()); }
} _f;
}
