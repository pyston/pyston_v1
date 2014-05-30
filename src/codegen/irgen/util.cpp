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

#include "codegen/irgen/util.h"

#include <sstream>
#include <unordered_map>

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "codegen/codegen.h"
#include "core/common.h"
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

// Returns a llvm::Constant char* to a global string constant
llvm::Constant* getStringConstantPtr(const std::string& str) {
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
    return embedConstantPtr(c, g.i8->getPointerTo());
}

// Returns a llvm::Constant char* to a global string constant
llvm::Constant* getStringConstantPtr(const char* str) {
    return getStringConstantPtr(std::string(str, strlen(str) + 1));
}

// Sometimes we want to embed pointers into the emitted code, usually to link the emitted code
// to some associated compiler-level data structure.
// It's slightly easier to emit them as integers (there are primitive integer constants but not pointer constants),
// but doing it this way makes it clearer what's going on.
llvm::Constant* embedConstantPtr(const void* addr, llvm::Type* type) {
    assert(type);
    llvm::Constant* int_val = llvm::ConstantInt::get(g.i64, reinterpret_cast<uintptr_t>(addr), false);
    llvm::Constant* ptr_val = llvm::ConstantExpr::getIntToPtr(int_val, type);
    return ptr_val;
}

llvm::Constant* getConstantInt(int n, llvm::Type* t) {
    return llvm::ConstantInt::get(t, n);
}

llvm::Constant* getConstantInt(int n) {
    return getConstantInt(n, g.i64);
}

class PrettifyingMaterializer : public llvm::ValueMaterializer {
private:
    llvm::Module* module;

public:
    PrettifyingMaterializer(llvm::Module* module) : module(module) {}

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

void dumpPrettyIR(llvm::Function* f) {
    std::unique_ptr<llvm::Module> tmp_module(llvm::CloneModule(f->getParent()));
    // std::unique_ptr<llvm::Module> tmp_module(new llvm::Module("tmp", g.context));

    llvm::Function* new_f = tmp_module->begin();

    llvm::ValueToValueMapTy VMap;
    PrettifyingMaterializer materializer(tmp_module.get());
    for (llvm::Function::iterator I = new_f->begin(), E = new_f->end(); I != E; ++I) {
        VMap[I] = I;
    }
    for (llvm::inst_iterator it = inst_begin(new_f), end = inst_end(new_f); it != end; ++it) {
        llvm::RemapInstruction(&*it, VMap, llvm::RF_None, NULL, &materializer);
    }
    tmp_module->begin()->dump();
    // tmp_module->dump();
}
}
