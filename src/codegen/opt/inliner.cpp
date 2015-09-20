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

#include "codegen/opt/inliner.h"

#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#if LLVMREV < 229094
#include "llvm/PassManager.h"
#else
#include "llvm/IR/LegacyPassManager.h"
#endif
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "codegen/codegen.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/util.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

class MyMaterializer : public llvm::ValueMaterializer {
private:
    llvm::Module* new_module;

public:
    MyMaterializer(llvm::Module* new_module) : new_module(new_module) {}
    virtual llvm::Value* materializeValueFor(llvm::Value* v) {
        // llvm::errs() << "materializing\n";
        // v->dump();

        llvm::Value* r = NULL;
        if (llvm::Function* f = llvm::dyn_cast<llvm::Function>(v)) {
            // llvm::errs() << "is function\n";
            r = new_module->getOrInsertFunction(f->getName(), f->getFunctionType());
        } else if (llvm::GlobalVariable* gv = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
            // llvm::errs() << " is gv\n";
            llvm::GlobalVariable* new_gv = new llvm::GlobalVariable(
                  *new_module, gv->getType()->getElementType(), gv->isConstant(),
                  gv->getLinkage(), nullptr, gv->getName(), nullptr,
                  gv->getThreadLocalMode(), gv->getType()->getAddressSpace());
            new_gv->copyAttributesFrom(gv);
            RELEASE_ASSERT(!gv->isThreadLocal(), "I don't think MCJIT supports thread-local variables yet");
            assert(!gv->hasInitializer());
            r = new_gv;
        } else if (llvm::GlobalAlias* alias = llvm::dyn_cast<llvm::GlobalAlias>(v)) {
#if LLVMREV < 209040
            llvm::Value* addressee = llvm::cast<llvm::Constant>(materializeValueFor(alias->getAliasedGlobal()));
#else
            llvm::Value* addressee = llvm::cast<llvm::Constant>(materializeValueFor(alias->getAliasee()));
#endif
            assert(addressee);
            assert(alias->getType() == addressee->getType());
            r = addressee;
            // r = new llvm::GlobalAlias(alias->getType(), alias->getLinkage(), alias->getName(), addressee,
            // new_module);
        } else if (llvm::isa<llvm::Constant>(v)) {
            // llvm::errs() << " is a constant\n";
            r = NULL;
        } else {
            r = v;
        }

        // if (r)
        // r->dump();
        // llvm::errs() << "---\n";
        return r;
    }
};

class MyInliningPass : public llvm::FunctionPass {
public:
    static char ID;
    static bool initialized;
    static llvm::Module* fake_module;

    int threshold;
    MyInliningPass(int threshold = 275) : FunctionPass(ID), threshold(threshold) {}

    static void initialize() {
        if (!initialized) {
            llvm::initializeInlineCostAnalysisPass(*llvm::PassRegistry::getPassRegistry());
            llvm::initializeSimpleInlinerPass(*llvm::PassRegistry::getPassRegistry());
#if LLVMREV < 227669
            llvm::initializeTargetTransformInfoAnalysisGroup(*llvm::PassRegistry::getPassRegistry());
#else
            llvm::initializeTargetTransformInfoWrapperPassPass(*llvm::PassRegistry::getPassRegistry());
#endif
            fake_module = new llvm::Module("fake", g.context);

            initialized = true;
        }
    }

    virtual const char* getPassName() const { return "Pyston inlining pass"; }

    bool _runOnFunction(llvm::Function& f) {
        Timer _t2("(sum)");
        Timer _t("initializing");
        initialize();
        _t.split("overhead");

        // f.dump();

        llvm::Module* cur_module = f.getParent();

#if LLVMREV < 217548
        llvm::PassManager fake_pm;
#else
        llvm::legacy::PassManager fake_pm;
#endif
        llvm::InlineCostAnalysis* cost_analysis = new llvm::InlineCostAnalysis();
        fake_pm.add(cost_analysis);
        // llvm::errs() << "doing fake run\n";
        fake_pm.run(*fake_module);
        // llvm::errs() << "done with fake run\n";

        bool did_any_inlining = false;

        // TODO I haven't gotten the callgraph-updating part of the inliner to work,
        // so it's not easy to tell what callsites have been inlined into (ie added to)
        // the function.
        // One simple-but-not-great way to handle it is to just iterate over the entire function
        // multiple times and re-inline things until we don't want to inline any more;
        // NPASSES controls the maximum number of times to attempt that.
        // Right now we actually don't need that, since we only inline fully-optimized
        // functions (from the stdlib), and those will already have had inlining
        // applied recursively.
        const int NPASSES = 1;
        for (int passnum = 0; passnum < NPASSES; passnum++) {
            _t.split("collecting calls");

            std::vector<llvm::CallSite> calls;
            for (llvm::inst_iterator I = llvm::inst_begin(f), E = llvm::inst_end(f); I != E; ++I) {
                llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(&(*I));
                // From Inliner.cpp:
                if (!call || llvm::isa<llvm::IntrinsicInst>(call))
                    continue;
                // I->dump();
                llvm::CallSite CS(call);

                llvm::Value* v = CS.getCalledValue();
                llvm::ConstantExpr* ce = llvm::dyn_cast<llvm::ConstantExpr>(v);
                if (!ce)
                    continue;

                assert(ce->isCast());
                llvm::ConstantInt* l_addr = llvm::cast<llvm::ConstantInt>(ce->getOperand(0));
                int64_t addr = l_addr->getSExtValue();

                if (addr == (int64_t)printf)
                    continue;
                llvm::Function* f = g.func_addr_registry.getLLVMFuncAtAddress((void*)addr);
                if (f == NULL) {
                    if (VERBOSITY() >= 3) {
                        printf("Giving up on inlining %s:\n",
                               g.func_addr_registry.getFuncNameAtAddress((void*)addr, true).c_str());
                        call->dump();
                    }
                    continue;
                }

                // We load the bitcode lazily, so check if we haven't yet fully loaded the function:
                if (f->isMaterializable()) {
#if LLVMREV < 220600
                    f->Materialize();
#else
                    f->materialize();
#endif
                }

                // It could still be a declaration, though I think the code won't generate this case any more:
                if (f->isDeclaration())
                    continue;

                // Keep this section as a release_assert since the code-to-be-inlined, as well as the inlining
                // decisions, can be different in release mode:
                int op_idx = -1;
                for (llvm::Argument& arg : f->args()) {
                    ++op_idx;
                    llvm::Type* op_type = call->getOperand(op_idx)->getType();
                    if (arg.getType() != op_type) {
                        llvm::errs() << f->getName() << " has arg " << op_idx << " mismatched!\n";
                        llvm::errs() << "Given ";
                        op_type->dump();
                        llvm::errs() << " but underlying function expected ";
                        arg.getType()->dump();
                        llvm::errs() << '\n';
                    }
                    RELEASE_ASSERT(arg.getType() == call->getOperand(op_idx)->getType(), "");
                }

                assert(!f->isDeclaration());
                CS.setCalledFunction(f);
                calls.push_back(CS);
            }

            // assert(0 && "TODO");
            // printf("%ld\n", calls.size());

            bool did_inline = false;
            _t.split("doing inlining");
            while (calls.size()) {
                llvm::CallSite cs = calls.back();
                calls.pop_back();

                // if (VERBOSITY("irgen.inlining") >= 1) {
                // llvm::errs() << "Evaluating callsite ";
                // cs->dump();
                //}
                llvm::InlineCost IC = cost_analysis->getInlineCost(cs, threshold);
                bool do_inline = false;
                if (IC.isAlways()) {
                    // if (VERBOSITY("irgen.inlining") >= 2)
                    // llvm::errs() << "always inline\n";
                    do_inline = true;
                } else if (IC.isNever()) {
                    // if (VERBOSITY("irgen.inlining") >= 2)
                    // llvm::errs() << "never inline\n";
                    do_inline = false;
                } else {
                    // if (VERBOSITY("irgen.inlining") >= 2)
                    // llvm::errs() << "Inline cost: " << IC.getCost() << '\n';
                    do_inline = (bool)IC;
                }

                if (VERBOSITY("irgen.inlining") >= 1) {
                    if (!do_inline)
                        llvm::outs() << "not ";
                    llvm::outs() << "inlining ";
                    cs->dump();
                }

                if (do_inline) {
                    static StatCounter num_inlines("num_inlines");
                    num_inlines.log();

                    // llvm::CallGraph cg(*f.getParent());
                    ////cg.addToCallGraph(cs->getCalledFunction());
                    // llvm::InlineFunctionInfo InlineInfo(&cg);

                    llvm::InlineFunctionInfo InlineInfo;
                    bool inlined = llvm::InlineFunction(cs, InlineInfo, false);
                    did_inline = did_inline || inlined;
                    did_any_inlining = did_any_inlining || inlined;

                    // if (inlined)
                    // f.dump();
                }
            }

            if (!did_inline) {
                if (passnum >= NPASSES - 1 && VERBOSITY("irgen.inlining"))
                    printf("quitting after %d passes\n", passnum + 1);
                break;
            }
        }

        // TODO would be nice to break out here and not have to rematerialize the function;
        // I think I have to do that even if no inlining happened from the "setCalledFunction" call above.
        // I thought that'd just change the CS object, but maybe it changes the underlying instruction as well?
        // if (!did_any_inlining)
        // return false;

        _t.split("remapping");

        llvm::ValueToValueMapTy VMap;
        for (llvm::Function::iterator I = f.begin(), E = f.end(); I != E; ++I) {
            VMap[I] = I;
        }
        MyMaterializer materializer(cur_module);
        for (llvm::inst_iterator I = llvm::inst_begin(f), E = llvm::inst_end(f); I != E; ++I) {
            RemapInstruction(&(*I), VMap, llvm::RF_None, NULL, &materializer);
        }

        _t.split("cleaning up");

        std::vector<llvm::GlobalValue*> to_remove;
        for (llvm::Module::global_iterator I = cur_module->global_begin(), E = cur_module->global_end(); I != E; ++I) {
            if (I->use_empty()) {
                to_remove.push_back(I);
                continue;
            }
        }

        for (int i = 0; i < to_remove.size(); i++) {
            to_remove[i]->eraseFromParent();
        }

        for (llvm::Module::iterator I = cur_module->begin(), E = cur_module->end(); I != E;) {
            if (!I->isDeclaration()) {
                ++I;
                continue;
            }

            if (I->use_empty()) {
                I = cur_module->getFunctionList().erase(I);
            } else {
                ++I;
            }
        }

        return did_any_inlining;
    }

    virtual bool runOnFunction(llvm::Function& f) {
        Timer _t("inlining");

        bool rtn = _runOnFunction(f);

        static StatCounter us_inlining("us_compiling_optimizing_inlining");
        long us = _t.end();
        us_inlining.log(us);

        return rtn;
    }
};
char MyInliningPass::ID = 0;
bool MyInliningPass::initialized = false;
llvm::Module* MyInliningPass::fake_module = 0;
static llvm::RegisterPass<MyInliningPass> X("myinliner", "Function-level inliner", false, false);

llvm::FunctionPass* makeFPInliner(int threshold) {
    return new MyInliningPass(threshold);
}
}
