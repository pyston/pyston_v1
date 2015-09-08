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

#include <dlfcn.h>
#include <map>
#include <queue>
#include <set>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"

#include "codegen/codegen.h"
#include "codegen/irgen/util.h"
#include "core/common.h"
#include "core/options.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace llvm;

namespace pyston {

llvm::Constant* embedConstantPtr(const void* addr, llvm::Type* type) {
    RELEASE_ASSERT(0, "unimplemented");
}

class ConstClassesPass : public FunctionPass {
private:
    void* getGVAddr(GlobalVariable* gv) {
        // TODO cache this?
        void* handle = dlopen(NULL, RTLD_LAZY);
        void* addr = dlsym(handle, gv->getName().data());
        assert(addr);
        dlclose(handle);
        return addr;
    }
    BoxedClass* getClassFromGV(GlobalVariable* gv) { return *(BoxedClass**)getGVAddr(gv); }

    void replaceUsesWithConstant(llvm::Value* v, uintptr_t val) {
        if (isa<PointerType>(v->getType()))
            v->replaceAllUsesWith(embedConstantPtr((void*)val, v->getType()));
        else
            v->replaceAllUsesWith(getConstantInt(val, v->getType()));
    }

    bool handleBool(LoadInst* li, GlobalVariable* gv) {
        if (VERBOSITY()) {
            llvm::errs() << "Constant-folding this load: " << *li << '\n';
        }
        if (gv->getName() == "True")
            li->replaceAllUsesWith(embedConstantPtr(True, g.llvm_bool_type_ptr));
        else
            li->replaceAllUsesWith(embedConstantPtr(False, g.llvm_bool_type_ptr));
        return true;
    }

    bool handleCls(LoadInst* li, GlobalVariable* gv) {
        bool changed = true;

        if (VERBOSITY("opt") >= 1) {
            errs() << "\nFound load of class-typed global variable:\n" << *li << '\n';
        }

        BoxedClass* cls = getClassFromGV(gv);
        if (!cls->is_constant) {
            assert(0 && "what globally-resolved classes are not constant??");
            if (VERBOSITY("opt") >= 1) {
                errs() << gv->getName() << " is not constant; moving on\n";
            }
            return false;
        }

        std::vector<Instruction*> to_remove;
        for (User* user : li->users()) {
            if (CallInst* call = dyn_cast<CallInst>(user)) {
                continue;
            }

            GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(user);
            if (!gep) {
                // errs() << "Not a gep: " << *user << '\n';
                continue;
            }

            APInt ap_offset(64, 0, true);
#if LLVMREV < 214781
            bool success = gep->accumulateConstantOffset(*g.tm->getDataLayout(), ap_offset);
#elif LLVMREV < 227113
            bool success = gep->accumulateConstantOffset(*g.tm->getSubtargetImpl()->getDataLayout(), ap_offset);
#else
            bool success = gep->accumulateConstantOffset(*g.tm->getDataLayout(), ap_offset);
#endif
            assert(success);
            int64_t offset = ap_offset.getSExtValue();

            if (VERBOSITY("opt") >= 1)
                errs() << "Found a gep at offset " << offset << ": " << *gep << '\n';

            for (User* gep_user : gep->users()) {
                LoadInst* gep_load = dyn_cast<LoadInst>(gep_user);
                if (!gep_load) {
                    // errs() << "Not a load: " << *gep_user << '\n';
                    continue;
                }


                if (VERBOSITY("opt") >= 1)
                    errs() << "Found a load: " << *gep_load << '\n';

                if (offset == offsetof(BoxedClass, attrs_offset)) {
                    if (VERBOSITY("opt") >= 1)
                        errs() << "attrs_offset; replacing with " << cls->attrs_offset << "\n";
                    replaceUsesWithConstant(gep_load, cls->attrs_offset);
                    changed = true;
                } else if (offset == offsetof(BoxedClass, tp_basicsize)) {
                    if (VERBOSITY("opt") >= 1)
                        errs() << "tp_basicsize; replacing with " << cls->tp_basicsize << "\n";
                    replaceUsesWithConstant(gep_load, cls->tp_basicsize);
                    changed = true;
                }
            }
        }

        for (int i = 0; i < to_remove.size(); i++) {
            to_remove[i]->eraseFromParent();
            changed = true;
        }

        if (VERBOSITY()) {
            llvm::errs() << "Constant-folding this load: " << *li << '\n';
        }
        li->replaceAllUsesWith(embedConstantPtr(cls, g.llvm_class_type_ptr));

        changed = true;
        return changed;
    }

public:
    static char ID;
    ConstClassesPass() : FunctionPass(ID) {}

    virtual void getAnalysisUsage(AnalysisUsage& info) const { info.setPreservesCFG(); }

    virtual bool runOnFunction(Function& F) {
        // F.dump();
        bool changed = false;
        for (inst_iterator inst_it = inst_begin(F), _inst_end = inst_end(F); inst_it != _inst_end; ++inst_it) {
            LoadInst* li = dyn_cast<LoadInst>(&*inst_it);
            if (!li)
                continue;

            ConstantExpr* ce = dyn_cast<ConstantExpr>(li->getOperand(0));
            // Not 100% sure what the isGEPWithNoNotionalOverIndexing() means, but
            // at least it checks if it's a gep:

            GlobalVariable* gv = dyn_cast<GlobalVariable>(li->getOperand(0));
            if (!gv)
                continue;

            llvm::Type* gv_t = gv->getType();

            if (gv_t == g.llvm_bool_type_ptr->getPointerTo()) {
                changed = handleBool(li, gv) || changed;
                continue;
            }

            if (gv_t == g.llvm_class_type_ptr->getPointerTo()) {
                changed = handleCls(li, gv) || changed;
                continue;
            }
        }

        return changed;
    }
};
char ConstClassesPass::ID = 0;

FunctionPass* createConstClassesPass() {
    return new ConstClassesPass();
}
}

static RegisterPass<pyston::ConstClassesPass>
    X("const_classes", "Use the fact that builtin classes are constant and their attributes can be constant-folded",
      true, false);
