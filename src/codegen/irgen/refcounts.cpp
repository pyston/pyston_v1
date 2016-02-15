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

#include "codegen/compvars.h"

#include <cstdio>
#include <sstream>

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "codegen/codegen.h"
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

void RefcountTracker::setType(llvm::Value* v, RefType reftype) {
    auto& var = this->vars[v];

    assert(var.reftype == reftype || var.reftype == RefType::UNKNOWN);
    var.reftype = reftype;
}

void RefcountTracker::refConsumed(llvm::Value* v, llvm::Instruction* inst) {
    auto& var = this->vars[v];

    assert(var.reftype != RefType::UNKNOWN);
    var.ref_consumers.push_back(inst);
}

void RefcountTracker::addRefcounts(IRGenState* irstate) {
    llvm::Function* f = irstate->getLLVMFunction();
    RefcountTracker* rt = irstate->getRefcounts();

    fprintf(stderr, "Before refcounts:\n");
    fprintf(stderr, "\033[35m");
    dumpPrettyIR(f);
    fprintf(stderr, "\033[0m");

#ifndef NDEBUG
    int num_untracked = 0;
    auto check_val_missed = [&](llvm::Value* v) {
        if (rt->vars.count(v))
            return;

        auto t = v->getType();
        auto p = llvm::dyn_cast<llvm::PointerType>(t);
        if (!p) {
            //t->dump();
            //printf("Not a pointer\n");
            return;
        }
        auto s = llvm::dyn_cast<llvm::StructType>(p->getElementType());
        if (!s) {
            //t->dump();
            //printf("Not a pointer\n");
            return;
        }

        // Take care of inheritance.  It's represented as an instance of the base type at the beginning of the
        // derived type, not as the types concatenated.
        while (s->elements().size() > 0 && llvm::isa<llvm::StructType>(s->elements()[0]))
            s = llvm::cast<llvm::StructType>(s->elements()[0]);

        bool ok_type = false;
        if (s->elements().size() >= 2 && s->elements()[0] == g.i64 && s->elements()[1] == g.llvm_class_type_ptr) {
            //printf("This looks likes a class\n");
            ok_type = true;
        }

        if (!ok_type) {
#ifndef NDEBUG
            if (s->getName().startswith("struct.pyston::Box") || (s->getName().startswith("Py") || s->getName().endswith("Object")) || s->getName().startswith("class.pyston::Box")) {
                v->dump();
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
            //abort();
        }
    };

    for (auto&& g : f->getParent()->getGlobalList()) {
        //g.dump();
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
        }
    }
    ASSERT(num_untracked == 0, "");
#endif

    assert(0 && "implement me");
}

} // namespace pyston
