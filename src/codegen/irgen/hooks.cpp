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

#include "codegen/irgen/hooks.h"

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "asm_writing/icinfo.h"
#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/future.h"
#include "codegen/irgen/util.h"
#include "codegen/llvm_interpreter.h"
#include "codegen/osrentry.h"
#include "codegen/patchpoints.h"
#include "codegen/stackmaps.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// TODO terrible place for these!
SourceInfo::ArgNames::ArgNames(AST* ast) {
    if (ast->type == AST_TYPE::Module || ast->type == AST_TYPE::ClassDef) {
        args = NULL;
        kwarg = vararg = NULL;
    } else if (ast->type == AST_TYPE::FunctionDef) {
        AST_FunctionDef* f = ast_cast<AST_FunctionDef>(ast);
        args = &f->args->args;
        vararg = &f->args->vararg;
        kwarg = &f->args->kwarg;
    } else if (ast->type == AST_TYPE::Lambda) {
        AST_Lambda* l = ast_cast<AST_Lambda>(ast);
        args = &l->args->args;
        vararg = &l->args->vararg;
        kwarg = &l->args->kwarg;
    } else {
        RELEASE_ASSERT(0, "%d", ast->type);
    }
}

const std::string SourceInfo::getName() {
    assert(ast);
    switch (ast->type) {
        case AST_TYPE::ClassDef:
            return ast_cast<AST_ClassDef>(ast)->name;
        case AST_TYPE::FunctionDef:
            return ast_cast<AST_FunctionDef>(ast)->name;
        case AST_TYPE::Lambda:
            return "<lambda>";
        case AST_TYPE::Module:
            return this->parent_module->name();
        default:
            RELEASE_ASSERT(0, "%d", ast->type);
    }
}

ScopeInfo* SourceInfo::getScopeInfo() {
    return scoping->getScopeInfoForNode(ast);
}

EffortLevel::EffortLevel initialEffort() {
    if (FORCE_OPTIMIZE)
        return EffortLevel::MAXIMAL;
    if (ENABLE_INTERPRETER)
        return EffortLevel::INTERPRETED;
    return EffortLevel::MINIMAL;
}

static void compileIR(CompiledFunction* cf, EffortLevel::EffortLevel effort) {
    assert(cf);
    assert(cf->func);

    // g.engine->finalizeOBject();
    if (VERBOSITY("irgen") >= 1) {
        printf("Compiling...\n");
        // g.cur_module->dump();
    }

    void* compiled = NULL;
    cf->code = NULL;
    if (effort > EffortLevel::INTERPRETED) {
        Timer _t("to jit the IR");
#if LLVMREV < 215967
        g.engine->addModule(cf->func->getParent());
#else
        g.engine->addModule(std::unique_ptr<llvm::Module>(cf->func->getParent()));
#endif

        g.cur_cf = cf;
        void* compiled = (void*)g.engine->getFunctionAddress(cf->func->getName());
        g.cur_cf = NULL;
        assert(compiled);
        ASSERT(compiled == cf->code, "cf->code should have gotten filled in");

        cf->llvm_code = embedConstantPtr(compiled, cf->func->getType());

        long us = _t.end();
        static StatCounter us_jitting("us_compiling_jitting");
        us_jitting.log(us);
        static StatCounter num_jits("num_jits");
        num_jits.log();
    }

    if (VERBOSITY("irgen") >= 1) {
        printf("Compiled function to %p\n", cf->code);
    }

    StackMap* stackmap = parseStackMap();
    processStackmap(cf, stackmap);
    delete stackmap;
}

static std::unordered_map<std::string, CompiledFunction*> machine_name_to_cf;
CompiledFunction* cfForMachineFunctionName(const std::string& machine_name) {
    assert(machine_name.size());
    auto r = machine_name_to_cf[machine_name];
    return r;
}

void registerMachineName(const std::string& machine_name, CompiledFunction* cf) {
    assert(machine_name_to_cf.count(machine_name) == 0);
    machine_name_to_cf[machine_name] = cf;
}

// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
// The codegen_lock needs to be held in W mode before calling this function:
CompiledFunction* compileFunction(CLFunction* f, FunctionSpecialization* spec, EffortLevel::EffortLevel effort,
                                  const OSREntryDescriptor* entry) {
    Timer _t("for compileFunction()");
    assert(spec);

    ASSERT(f->versions.size() < 20, "%ld", f->versions.size());
    SourceInfo* source = f->source;
    assert(source);

    std::string name = source->getName();

    if (VERBOSITY("irgen") >= 1) {
        std::string s;
        llvm::raw_string_ostream ss(s);

        ss << "\033[34;1mJIT'ing " << name << " with signature (";
        for (int i = 0; i < spec->arg_types.size(); i++) {
            if (i > 0)
                ss << ", ";
            ss << spec->arg_types[i]->debugName();
            // spec->arg_types[i]->llvmType()->print(ss);
        }
        ss << ") -> ";
        ss << spec->rtn_type->debugName();
        // spec->rtn_type->llvmType()->print(ss);
        ss << " at effort level " << effort;
        if (entry != NULL) {
            ss << "\nDoing OSR-entry partial compile, starting with backedge to block " << entry->backedge->target->idx
               << '\n';
        }
        ss << "\033[0m";
        printf("%s\n", ss.str().c_str());
    }

    // Do the analysis now if we had deferred it earlier:
    if (source->cfg == NULL) {
        source->cfg = computeCFG(source, source->body);
    }

    if (effort != EffortLevel::INTERPRETED) {
        if (source->liveness == NULL)
            source->liveness = computeLivenessInfo(source->cfg);

        if (source->phis == NULL)
            source->phis
                = computeRequiredPhis(source->arg_names, source->cfg, source->liveness, source->getScopeInfo());
    }



    CompiledFunction* cf = 0;
    if (effort == EffortLevel::INTERPRETED) {
        cf = new CompiledFunction(0, spec, true, NULL, NULL, effort, 0);
    } else {
        cf = doCompile(source, entry, effort, spec, name);
        registerMachineName(cf->func->getName(), cf);
        compileIR(cf, effort);
    }

    f->addVersion(cf);
    assert(f->versions.size());

    long us = _t.end();
    static StatCounter us_compiling("us_compiling");
    us_compiling.log(us);
    static StatCounter num_compiles("num_compiles");
    num_compiles.log();

    switch (effort) {
        case EffortLevel::INTERPRETED: {
            static StatCounter us_compiling("us_compiling_0_interpreted");
            us_compiling.log(us);
            static StatCounter num_compiles("num_compiles_0_interpreted");
            num_compiles.log();
            break;
        }
        case EffortLevel::MINIMAL: {
            static StatCounter us_compiling("us_compiling_1_minimal");
            us_compiling.log(us);
            static StatCounter num_compiles("num_compiles_1_minimal");
            num_compiles.log();
            break;
        }
        case EffortLevel::MODERATE: {
            static StatCounter us_compiling("us_compiling_2_moderate");
            us_compiling.log(us);
            static StatCounter num_compiles("num_compiles_2_moderate");
            num_compiles.log();
            break;
        }
        case EffortLevel::MAXIMAL: {
            static StatCounter us_compiling("us_compiling_3_maximal");
            us_compiling.log(us);
            static StatCounter num_compiles("num_compiles_3_maximal");
            num_compiles.log();
            break;
        }
    }

    return cf;
}

void compileAndRunModule(AST_Module* m, BoxedModule* bm) {
    CompiledFunction* cf;

    { // scope for limiting the locked region:
        LOCK_REGION(codegen_rwlock.asWrite());

        Timer _t("for compileModule()");

        bm->future_flags = getFutureFlags(m, bm->fn.c_str());

        ScopingAnalysis* scoping = runScopingAnalysis(m);

        SourceInfo* si = new SourceInfo(bm, scoping, m, m->body);
        CLFunction* cl_f = new CLFunction(0, 0, false, false, si);

        EffortLevel::EffortLevel effort = initialEffort();

        cf = compileFunction(cl_f, new FunctionSpecialization(VOID), effort, NULL);
        assert(cf->clfunc->versions.size());
    }

    if (cf->is_interpreted)
        astInterpretFunction(cf, 0, NULL, NULL, NULL, NULL, NULL, NULL);
    else
        ((void (*)())cf->code)();
}

/// Reoptimizes the given function version at the new effort level.
/// The cf must be an active version in its parents CLFunction; the given
/// version will be replaced by the new version, which will be returned.
static CompiledFunction* _doReopt(CompiledFunction* cf, EffortLevel::EffortLevel new_effort) {
    LOCK_REGION(codegen_rwlock.asWrite());

    assert(cf->clfunc->versions.size());

    assert(cf);
    assert(cf->entry_descriptor == NULL && "We can't reopt an osr-entry compile!");
    assert(cf->spec);

    CLFunction* clfunc = cf->clfunc;
    assert(clfunc);

    assert(new_effort > cf->effort);

    FunctionList& versions = clfunc->versions;
    for (int i = 0; i < versions.size(); i++) {
        if (versions[i] == cf) {
            versions.erase(versions.begin() + i);

            CompiledFunction* new_cf
                = compileFunction(clfunc, cf->spec, new_effort,
                                  NULL); // this pushes the new CompiledVersion to the back of the version list

            cf->dependent_callsites.invalidateAll();

            return new_cf;
        }
    }

    printf("Couldn't find a version; %ld exist:\n", versions.size());
    for (auto cf : versions) {
        printf("%p\n", cf);
    }
    assert(0 && "Couldn't find a version to reopt! Probably reopt'd already?");
    abort();
}

static StatCounter stat_osrexits("OSR exits");
CompiledFunction* compilePartialFuncInternal(OSRExit* exit) {
    LOCK_REGION(codegen_rwlock.asWrite());

    assert(exit);
    assert(exit->parent_cf);
    assert(exit->parent_cf->effort < EffortLevel::MAXIMAL);
    stat_osrexits.log();

    // if (VERBOSITY("irgen") >= 1) printf("In compilePartialFunc, handling %p\n", exit);

    assert(exit->parent_cf->clfunc);
    CompiledFunction*& new_cf = exit->parent_cf->clfunc->osr_versions[exit->entry];
    if (new_cf == NULL) {
        EffortLevel::EffortLevel new_effort = EffortLevel::MAXIMAL;
        if (exit->parent_cf->effort == EffortLevel::INTERPRETED)
            new_effort = EffortLevel::MINIMAL;
        // EffortLevel::EffortLevel new_effort = (EffortLevel::EffortLevel)(exit->parent_cf->effort + 1);
        // new_effort = EffortLevel::MAXIMAL;
        CompiledFunction* compiled
            = compileFunction(exit->parent_cf->clfunc, exit->parent_cf->spec, new_effort, exit->entry);
        assert(compiled = new_cf);
    }

    return new_cf;
}

void* compilePartialFunc(OSRExit* exit) {
    return compilePartialFuncInternal(exit)->code;
}


static StatCounter stat_reopt("reopts");
extern "C" CompiledFunction* reoptCompiledFuncInternal(CompiledFunction* cf) {
    if (VERBOSITY("irgen") >= 1)
        printf("In reoptCompiledFunc, %p, %ld\n", cf, cf->times_called);
    stat_reopt.log();

    assert(cf->effort < EffortLevel::MAXIMAL);
    assert(cf->clfunc->versions.size());
    CompiledFunction* new_cf = _doReopt(cf, (EffortLevel::EffortLevel(cf->effort + 1)));
    assert(!new_cf->is_interpreted);
    return new_cf;
}


extern "C" char* reoptCompiledFunc(CompiledFunction* cf) {
    return (char*)reoptCompiledFuncInternal(cf)->code;
}

CLFunction* createRTFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs) {
    return new CLFunction(num_args, num_defaults, takes_varargs, takes_kwargs, NULL);
}

CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int num_args) {
    return boxRTFunction(f, rtn_type, num_args, 0, false, false);
}

CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int num_args, int num_defaults, bool takes_varargs,
                          bool takes_kwargs) {
    CLFunction* cl_f = createRTFunction(num_args, num_defaults, takes_varargs, takes_kwargs);

    addRTFunction(cl_f, f, rtn_type);
    return cl_f;
}

void addRTFunction(CLFunction* cl_f, void* f, ConcreteCompilerType* rtn_type) {
    std::vector<ConcreteCompilerType*> arg_types(cl_f->numReceivedArgs(), UNKNOWN);
    return addRTFunction(cl_f, f, rtn_type, arg_types);
}

static ConcreteCompilerType* processType(ConcreteCompilerType* type) {
    assert(type);
    return type;
}

void addRTFunction(CLFunction* cl_f, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types) {
    assert(arg_types.size() == cl_f->numReceivedArgs());
#ifndef NDEBUG
    for (ConcreteCompilerType* t : arg_types)
        assert(t);
#endif

    FunctionSpecialization* spec = new FunctionSpecialization(processType(rtn_type), arg_types);

    std::vector<llvm::Type*> llvm_arg_types;
    int npassed_args = arg_types.size();
    assert(npassed_args == cl_f->numReceivedArgs());
    for (int i = 0; i < npassed_args; i++) {
        if (i == 3) {
            llvm_arg_types.push_back(g.i8_ptr->getPointerTo());
            break;
        }
        llvm_arg_types.push_back(arg_types[i]->llvmType());
    }

    llvm::FunctionType* ft = llvm::FunctionType::get(g.llvm_value_type_ptr, llvm_arg_types, false);

    cl_f->addVersion(new CompiledFunction(NULL, spec, false, f, embedConstantPtr(f, ft->getPointerTo()),
                                          EffortLevel::MAXIMAL, NULL));
}
}
