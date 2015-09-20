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

#include "codegen/codegen.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/baseline_jit.h"
#include "codegen/compvars.h"
#include "core/ast.h"
#include "core/util.h"
#include "runtime/code.h"
#include "runtime/types.h"

namespace pyston {

DS_DEFINE_RWLOCK(codegen_rwlock);

CLFunction::CLFunction(int num_args, bool takes_varargs, bool takes_kwargs, std::unique_ptr<SourceInfo> source)
    : code_obj(NULL),
      num_args(num_args),
      takes_varargs(takes_varargs),
      takes_kwargs(takes_kwargs),
      source(std::move(source)),
      param_names(this->source->ast, this->source->getInternedStrings()),
      always_use_version(NULL),
      times_interpreted(0),
      internal_callable(NULL, NULL) {
}

CLFunction::CLFunction(int num_args, bool takes_varargs, bool takes_kwargs, const ParamNames& param_names)
    : code_obj(NULL),
      num_args(num_args),
      takes_varargs(takes_varargs),
      takes_kwargs(takes_kwargs),
      source(nullptr),
      param_names(param_names),
      always_use_version(NULL),
      times_interpreted(0),
      internal_callable(NULL, NULL) {
}

BoxedCode* CLFunction::getCode() {
    if (!code_obj) {
        code_obj = new BoxedCode(this);
        // CLFunctions don't currently participate in GC.  They actually never get freed currently.
        gc::registerPermanentRoot(code_obj);
    }
    return code_obj;
}

void CLFunction::addVersion(CompiledFunction* compiled) {
    assert(compiled);
    assert((compiled->spec != NULL) + (compiled->entry_descriptor != NULL) == 1);
    assert(compiled->clfunc == NULL);
    assert(compiled->code);
    compiled->clfunc = this;

    if (compiled->entry_descriptor == NULL) {
        bool could_have_speculations = (source.get() != NULL);
        if (!could_have_speculations && versions.size() == 0 && compiled->effort == EffortLevel::MAXIMAL
            && compiled->spec->accepts_all_inputs && compiled->spec->boxed_return_value)
            always_use_version = compiled;

        assert(compiled->spec->arg_types.size() == numReceivedArgs());
        versions.push_back(compiled);
    } else {
        osr_versions[compiled->entry_descriptor] = compiled;
    }
}

SourceInfo::SourceInfo(BoxedModule* m, ScopingAnalysis* scoping, FutureFlags future_flags, AST* ast,
                       std::vector<AST_stmt*> body, BoxedString* fn)
    : parent_module(m),
      scoping(scoping),
      scope_info(NULL),
      future_flags(future_flags),
      ast(ast),
      cfg(NULL),
      body(std::move(body)) {
    assert(fn->size());
    // TODO: we should track this reference correctly rather than making it a root
    gc::registerPermanentRoot(fn, true);
    this->fn = fn;

    switch (ast->type) {
        case AST_TYPE::ClassDef:
        case AST_TYPE::Module:
        case AST_TYPE::Expression:
        case AST_TYPE::Suite:
            is_generator = false;
            break;
        case AST_TYPE::FunctionDef:
        case AST_TYPE::Lambda:
            is_generator = containsYield(ast);
            break;
        default:
            RELEASE_ASSERT(0, "Unknown type: %d", ast->type);
            break;
    }
}

SourceInfo::~SourceInfo() {
    // TODO: release memory..
}

void FunctionAddressRegistry::registerFunction(const std::string& name, void* addr, int length,
                                               llvm::Function* llvm_func) {
    assert(addr);
    assert(functions.count(addr) == 0);
    functions.insert(std::make_pair(addr, FuncInfo(name, length, llvm_func)));
}

void FunctionAddressRegistry::dumpPerfMap() {
    std::string out_path = "perf_map";
    removeDirectoryIfExists(out_path);

    llvm_error_code code;
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
    virtual void NotifyObjectEmitted(const llvm::object::ObjectFile& Obj,
                                     const llvm::RuntimeDyld::LoadedObjectInfo& L) {
        static StatCounter code_bytes("code_bytes");
        code_bytes.log(Obj.getData().size());

        llvm_error_code code;
        for (const auto& sym : Obj.symbols()) {
            llvm::object::section_iterator section(Obj.section_end());
            code = sym.getSection(section);
            assert(!code);
            bool is_text;
#if LLVMREV < 219314
            code = section->isText(is_text);
            assert(!code);
#else
            is_text = section->isText();
#endif
            if (!is_text)
                continue;

            llvm::StringRef name;
            code = sym.getName(name);
            assert(!code);
            uint64_t size;
            code = sym.getSize(size);
            assert(!code);

            if (name == ".text" || name.empty() || !size)
                continue;


            uint64_t sym_addr = g.engine->getGlobalValueAddress(name);
            assert(sym_addr);

            g.func_addr_registry.registerFunction(name.data(), (void*)sym_addr, size, NULL);
        }
    }
};

GlobalState::GlobalState() : context(llvm::getGlobalContext()), cur_module(NULL), cur_cf(NULL){};

llvm::JITEventListener* makeRegistryListener() {
    return new RegistryEventListener();
}


FunctionSpecialization::FunctionSpecialization(ConcreteCompilerType* rtn_type) : rtn_type(rtn_type) {
    accepts_all_inputs = true;
    boxed_return_value = (rtn_type->llvmType() == UNKNOWN->llvmType());
}

FunctionSpecialization::FunctionSpecialization(ConcreteCompilerType* rtn_type,
                                               const std::vector<ConcreteCompilerType*>& arg_types)
    : rtn_type(rtn_type), arg_types(arg_types) {
    accepts_all_inputs = true;
    boxed_return_value = (rtn_type->llvmType() == UNKNOWN->llvmType());
    for (auto t : arg_types) {
        accepts_all_inputs = accepts_all_inputs && (t == UNKNOWN);
    }
}
}
