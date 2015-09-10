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

#include "codegen/irgen/util.h"

#include <sstream>
#include <unordered_map>

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "codegen/codegen.h"
#include "codegen/patchpoints.h"
#include "core/common.h"
#include "gc/gc.h"
#include "runtime/types.h"

namespace pyston {

/*
static std::string getStringName(std::string strvalue) {
    std::ostringstream name_os;
    name_os << "str";
    name_os << g.global_string_table.size();
    name_os << '_';
    for (int i = 0; i < strvalue.size(); i++) {
        if (isalnum(strvalue[i]))
            name_os << strvalue[i];
    }
    return name_os.str();
}

// Gets a reference (creating if necessary) to a global string constant with the given value.
// The return type will be a char array.
static llvm::Constant* getStringConstant(const std::string &str) {
    if (g.global_string_table.find(str) != g.global_string_table.end()) {
        llvm::GlobalVariable *gv = g.global_string_table[str];
        llvm::Constant *rtn = g.cur_module->getOrInsertGlobal(gv->getName(), gv->getType()->getElementType());
        return rtn;
    }

    int len = str.size();
    std::vector<llvm::Constant*> chars;
    for (int i = 0; i < len; i++) {
        chars.push_back(llvm::ConstantInt::get(g.i8, str[i]));
    }
    llvm::ArrayType *at = llvm::ArrayType::get(g.i8, len);
    llvm::Constant *val = llvm::ConstantArray::get(at, llvm::ArrayRef<llvm::Constant*>(chars));
    llvm::GlobalVariable *gv = new llvm::GlobalVariable(*g.cur_module, at, true, llvm::GlobalValue::ExternalLinkage,
val, getStringName(str));
    g.global_string_table[str] = gv;
    return gv;
}
*/

std::unordered_map<std::string, const char*> strings;

/*
// Returns a llvm::Constant char* to a global string constant
llvm::Constant* getStringConstantPtr(llvm::StringRef str) {
    const char* c;
    if (strings.count(str)) {
        c = strings[str];
    } else {
        char* buf = (char*)malloc(str.size() + 1);
        memcpy(buf, str.c_str(), str.size());
        buf[str.size()] = '\0';

        strings[str] = buf;
        c = buf;
    }
    return embedRelocatablePtr(c, g.i8->getPointerTo());
}

// Returns a llvm::Constant char* to a global string constant
llvm::Constant* getStringConstantPtr(llvm::StringRef str) {
    return getStringConstantPtr(std::string(str, strlen(str) + 1));
}
*/

// Sometimes we want to embed pointers into the emitted code, usually to link the emitted code
// to some associated compiler-level data structure.
// It's slightly easier to emit them as integers (there are primitive integer constants but not pointer constants),
// but doing it this way makes it clearer what's going on.

static llvm::StringMap<const void*> relocatable_syms;

// Pointer to a vector where we want to keep track of all the pointers written directly into
// the compiled code, which the GC needs to be aware of.
std::vector<const void*>* pointers_in_code;

void clearRelocatableSymsMap() {
    relocatable_syms.clear();
}

void setPointersInCodeStorage(std::vector<const void*>* v) {
    pointers_in_code = v;
}

const void* getValueOfRelocatableSym(const std::string& str) {
    auto it = relocatable_syms.find(str);
    if (it != relocatable_syms.end())
        return it->second;
    return NULL;
}

llvm::Constant* embedRelocatablePtr(const void* addr, llvm::Type* type, llvm::StringRef shared_name) {
    assert(addr);

    if (!ENABLE_JIT_OBJECT_CACHE)
        return embedConstantPtr(addr, type);

    std::string name;
    if (!shared_name.empty()) {
        llvm::GlobalVariable* gv = g.cur_module->getGlobalVariable(shared_name, true);
        if (gv)
            return gv;
        assert(!relocatable_syms.count(name));
        name = shared_name;
    } else {
        name = (llvm::Twine("c") + llvm::Twine(relocatable_syms.size())).str();
    }

    relocatable_syms[name] = addr;

#if MOVING_GC
    gc::GCAllocation* al = gc::global_heap.getAllocationFromInteriorPointer(const_cast<void*>(addr));
    if (al) {
        pointers_in_code->push_back(al->user_data);
    }
#endif

    llvm::Type* var_type = type->getPointerElementType();
    return new llvm::GlobalVariable(*g.cur_module, var_type, true, llvm::GlobalVariable::ExternalLinkage, 0, name);
}

llvm::Constant* embedConstantPtr(const void* addr, llvm::Type* type) {
    assert(type);
    llvm::Constant* int_val = llvm::ConstantInt::get(g.i64, reinterpret_cast<uintptr_t>(addr), false);
    llvm::Constant* ptr_val = llvm::ConstantExpr::getIntToPtr(int_val, type);

#if MOVING_GC
    gc::GCAllocation* al = gc::global_heap.getAllocationFromInteriorPointer(const_cast<void*>(addr));
    if (al) {
        pointers_in_code->push_back(al->user_data);
    }
#endif

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
        if (llvm::IntrinsicInst* ii = llvm::dyn_cast<llvm::IntrinsicInst>(v)) {
            if (ii->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_i64
                || ii->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_void
                || ii->getIntrinsicID() == llvm::Intrinsic::experimental_patchpoint_double) {
                int pp_id = -1;
                for (int i = 0; i < ii->getNumArgOperands(); i++) {
                    llvm::Value* op = ii->getArgOperand(i);
                    if (i != 2) {
                        if (i == 0) {
                            llvm::ConstantInt* l_pp_id = llvm::cast<llvm::ConstantInt>(op);
                            pp_id = l_pp_id->getSExtValue();
                        }
                        ii->setArgOperand(i, llvm::MapValue(op, VMap, flags, type_remapper, this));
                        continue;
                    } else {
#if LLVMREV < 235483
                        assert(pp_id != -1);
                        void* addr = PatchpointInfo::getSlowpathAddr(pp_id);

                        bool lookup_success = true;
                        std::string name;
                        if (addr == (void*)None) {
                            name = "None";
                        } else {
                            name = g.func_addr_registry.getFuncNameAtAddress(addr, true, &lookup_success);
                        }

                        if (!lookup_success) {
                            llvm::Constant* int_val
                                = llvm::ConstantInt::get(g.i64, reinterpret_cast<uintptr_t>(addr), false);
                            llvm::Constant* ptr_val = llvm::ConstantExpr::getIntToPtr(int_val, g.i8);
                            ii->setArgOperand(i, ptr_val);
                            continue;
                        } else {
                            ii->setArgOperand(i, module->getOrInsertGlobal(name, g.i8));
                        }
#else
                        assert(0);
#endif
                    }
                }
                return ii;
            }
        }
        return v;
    }
};

void dumpPrettyIR(llvm::Function* f) {
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
    }
    tmp_module->begin()->dump();
    // tmp_module->dump();
}
}
