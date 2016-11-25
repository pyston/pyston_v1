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

#include "codegen/irgen/util.h"

#include <sstream>
#include <unordered_map>

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "codegen/codegen.h"
#include "codegen/patchpoints.h"
#include "core/common.h"
#include "runtime/types.h"

namespace pyston {

// Sometimes we want to embed pointers into the emitted code, usually to link the emitted code
// to some associated compiler-level data structure.
// It's slightly easier to emit them as integers (there are primitive integer constants but not pointer constants),
// but doing it this way makes it clearer what's going on.

static llvm::StringMap<const void*> relocatable_syms;
static llvm::DenseMap<const void*, llvm::Constant*> addr_gv_map;

void clearRelocatableSymsMap() {
    relocatable_syms.clear();
    addr_gv_map.clear();
}

const void* getValueOfRelocatableSym(llvm::StringRef str) {
    auto it = relocatable_syms.find(str);
    if (it != relocatable_syms.end())
        return it->second;
    return NULL;
}

llvm::Constant* embedRelocatablePtr(const void* addr, llvm::Type* type, llvm::StringRef shared_name) {
    assert(addr);

    if (!ENABLE_JIT_OBJECT_CACHE)
        return embedConstantPtr(addr, type);

    llvm::Constant*& gv = addr_gv_map[addr];
    if (!gv) {
        std::string name;
        if (!shared_name.empty()) {
            assert(!relocatable_syms.count(shared_name));
            name = shared_name;
        } else {
            int nsyms = relocatable_syms.size();
            name = (llvm::Twine("c") + llvm::Twine(nsyms)).str();
        }

        relocatable_syms[name] = addr;

        llvm::Type* var_type = type->getPointerElementType();
        gv = new llvm::GlobalVariable(*g.cur_module, var_type, /* isConstant */ false,
                                      llvm::GlobalVariable::ExternalLinkage, 0, name);
    }

    if (gv->getType() != type)
        return llvm::ConstantExpr::getPointerCast(gv, type);

    return gv;
}

llvm::Constant* embedConstantPtr(const void* addr, llvm::Type* type) {
    assert(type);
    llvm::Constant* int_val = llvm::ConstantInt::get(g.i64, reinterpret_cast<uintptr_t>(addr), false);
    llvm::Constant* ptr_val = llvm::ConstantExpr::getIntToPtr(int_val, type);

    return ptr_val;
}

llvm::Constant* getNullPtr(llvm::Type* t) {
    assert(llvm::isa<llvm::PointerType>(t));
    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(t));
}

llvm::Constant* getConstantInt(int64_t n, llvm::Type* t) {
    return llvm::ConstantInt::get(t, n);
}

llvm::Constant* getConstantInt(int64_t n) {
    return getConstantInt(n, g.i64);
}

llvm::Constant* getConstantDouble(double val) {
    return llvm::ConstantFP::get(g.double_, val);
}

class PrettifyingMaterializer : public llvm::ValueMaterializer {
private:
    llvm::Module* module;
    llvm::ValueToValueMapTy& VMap;
    llvm::RemapFlags flags;
    llvm::ValueMapTypeRemapper* type_remapper;

public:
    PrettifyingMaterializer(llvm::Module* module, llvm::ValueToValueMapTy& VMap, llvm::RemapFlags flags,
                            llvm::ValueMapTypeRemapper* type_remapper)
        : module(module), VMap(VMap), flags(flags), type_remapper(type_remapper) {}

    virtual llvm::Value* materializeValueFor(llvm::Value* v) {
        if (llvm::ConstantExpr* ce = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
            llvm::PointerType* pt = llvm::dyn_cast<llvm::PointerType>(ce->getType());
            if (ce->isCast() && ce->getOpcode() == llvm::Instruction::IntToPtr && pt) {
                llvm::ConstantInt* addr_const = llvm::cast<llvm::ConstantInt>(ce->getOperand(0));
                void* addr = (void*)addr_const->getSExtValue();

                bool lookup_success = true;
                std::string name;
                if (addr == (void*)None) {
                    name = "None";
                } else {
                    name = g.func_addr_registry.getFuncNameAtAddress(addr, true, &lookup_success);
                }

                if (!lookup_success)
                    return v;

                return module->getOrInsertGlobal(name, pt->getElementType());
            }
        }
        return v;
    }
};

template <typename I> void remapPatchpoint(I* ii) {
    int pp_id = -1;
    for (int i = 0; i < ii->getNumArgOperands(); i++) {
        llvm::Value* op = ii->getArgOperand(i);
        if (i == 0) {
            llvm::ConstantInt* l_pp_id = llvm::cast<llvm::ConstantInt>(op);
            pp_id = l_pp_id->getSExtValue();
        } else if (i == 2) {
            assert(pp_id != -1);

            if (pp_id == DECREF_PP_ID || pp_id == XDECREF_PP_ID)
                continue;

            void* addr = PatchpointInfo::getSlowpathAddr(pp_id);

            bool lookup_success = true;
            std::string name;
            if (addr == (void*)None) {
                name = "None";
            } else {
                name = g.func_addr_registry.getFuncNameAtAddress(addr, true, &lookup_success);
            }

            if (!lookup_success) {
                llvm::Constant* int_val = llvm::ConstantInt::get(g.i64, reinterpret_cast<uintptr_t>(addr), false);
                llvm::Constant* ptr_val = llvm::ConstantExpr::getIntToPtr(int_val, g.i8_ptr);
                ii->setArgOperand(i, ptr_val);
                continue;
            } else {
                ii->setArgOperand(i, ii->getParent()->getParent()->getParent()->getOrInsertGlobal(name, g.i8));
            }
        }
    }
}

void dumpPrettyIR(llvm::Function* f) {
    // f->getParent()->dump();
    // return;

    std::unique_ptr<llvm::Module> tmp_module(llvm::CloneModule(f->getParent()));
    // std::unique_ptr<llvm::Module> tmp_module(new llvm::Module("tmp", g.context));

    llvm::Function* new_f = tmp_module->begin();

    llvm::ValueToValueMapTy VMap;
    llvm::RemapFlags flags = llvm::RF_None;
    llvm::ValueMapTypeRemapper* type_remapper = NULL;
    PrettifyingMaterializer materializer(tmp_module.get(), VMap, flags, type_remapper);
    for (llvm::Function::iterator I = new_f->begin(), E = new_f->end(); I != E; ++I) {
        VMap[I] = I;
    }
    for (llvm::inst_iterator it = inst_begin(new_f), end = inst_end(new_f); it != end; ++it) {
        llvm::RemapInstruction(&*it, VMap, flags, type_remapper, &materializer);

        if (llvm::IntrinsicInst* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&*it)) {
            if (ii->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_i64
                || ii->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_void
                || ii->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_double) {
                remapPatchpoint(ii);
            }
        } else if (llvm::InvokeInst* ii = llvm::dyn_cast<llvm::InvokeInst>(&*it)) {
            if (ii->getCalledFunction() && ii->getCalledFunction()->isIntrinsic()) {
                remapPatchpoint(ii);
            }
        }
    }
    tmp_module->begin()->dump();
    // tmp_module->dump();
}
}
