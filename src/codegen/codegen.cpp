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

#include "codegen/codegen.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "core/util.h"

namespace pyston {

DS_DEFINE_RWLOCK(codegen_rwlock);

void FunctionAddressRegistry::registerFunction(const std::string& name, void* addr, int length,
                                               llvm::Function* llvm_func) {
    assert(addr);
    assert(functions.count(addr) == 0);
    functions.insert(std::make_pair(addr, FuncInfo(name, length, llvm_func)));
}

void FunctionAddressRegistry::dumpPerfMap() {
    std::string out_path = "perf_map";
    removeDirectoryIfExists(out_path);

    llvm::error_code code;
    code = llvm::sys::fs::create_directory(out_path, false);
    assert(!code);

    FILE* index_f = fopen((out_path + "/index.txt").c_str(), "w");

    char buf[80];
    snprintf(buf, 80, "/tmp/perf-%d.map", getpid());
    FILE* f = fopen(buf, "w");
    for (const auto& p : functions) {
        const FuncInfo& info = p.second;
        fprintf(f, "%lx %x %s\n", (uintptr_t)p.first, info.length, info.name.c_str());

        if (info.length > 0) {
            fprintf(index_f, "%lx %s\n", (uintptr_t)p.first, info.name.c_str());

            FILE* data_f = fopen((out_path + "/" + info.name).c_str(), "wb");

            int written = fwrite((void*)p.first, 1, info.length, data_f);
            assert(written == info.length);
            fclose(data_f);
        }
    }
    fclose(f);
}

llvm::Function* FunctionAddressRegistry::getLLVMFuncAtAddress(void* addr) {
    FuncMap::iterator it = functions.find(addr);
    if (it == functions.end()) {
        if (lookup_neg_cache.count(addr))
            return NULL;

        bool success;
        std::string name = getFuncNameAtAddress(addr, false, &success);
        if (!success) {
            lookup_neg_cache.insert(addr);
            return NULL;
        }

        llvm::Function* r = g.stdlib_module->getFunction(name);

        if (!r) {
            lookup_neg_cache.insert(addr);
            return NULL;
        }

        registerFunction(name, addr, 0, r);
        return r;
    }
    return it->second.llvm_func;
}

static std::string tryDemangle(const char* s) {
    int status;
    char* demangled = abi::__cxa_demangle(s, NULL, NULL, &status);
    if (!demangled) {
        return s;
    }
    std::string rtn = demangled;
    free(demangled);
    return rtn;
}

std::string FunctionAddressRegistry::getFuncNameAtAddress(void* addr, bool demangle, bool* out_success) {
    FuncMap::iterator it = functions.find(addr);
    if (it == functions.end()) {
        Dl_info info;
        int success = dladdr(addr, &info);

        if (success && info.dli_sname == NULL)
            success = false;

        if (out_success)
            *out_success = success;
        // if (success && info.dli_saddr == addr) {
        if (success) {
            if (demangle)
                return tryDemangle(info.dli_sname);
            return info.dli_sname;
        }

        return "<unknown>";
    }

    if (out_success)
        *out_success = true;
    if (!demangle)
        return it->second.name;

    return tryDemangle(it->second.name.c_str());
}

class RegistryEventListener : public llvm::JITEventListener {
public:
    void NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
        static StatCounter code_bytes("code_bytes");
        code_bytes.log(Obj.getData().size());

        llvm::error_code code;
        for (llvm::object::symbol_iterator I = Obj.begin_symbols(), E = Obj.end_symbols(); I != E;
#if LLVMREV < 200442
             I = I.increment(code)
#else
             ++I
#endif
             ) {
            llvm::object::section_iterator section(Obj.end_sections());
            code = I->getSection(section);
            assert(!code);
            bool is_text;
            code = section->isText(is_text);
            assert(!code);
            if (!is_text)
                continue;

            llvm::StringRef name;
            uint64_t addr, size;
            code = I->getName(name);
            assert(!code);
            code = I->getAddress(addr);
            assert(!code);
            code = I->getSize(size);
            assert(!code);

            if (name == ".text")
                continue;

            g.func_addr_registry.registerFunction(name.data(), (void*)addr, size, NULL);
        }
    }
};

GlobalState::GlobalState() : context(llvm::getGlobalContext()) {};

llvm::JITEventListener* makeRegistryListener() {
    return new RegistryEventListener();
}
}
