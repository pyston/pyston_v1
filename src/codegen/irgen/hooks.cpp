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
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
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
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// TODO terrible place for these!
const std::string SourceInfo::getName() {
    assert(ast);
    switch (ast->type) {
        case AST_TYPE::FunctionDef:
            return ast_cast<AST_FunctionDef>(ast)->name;
        case AST_TYPE::Module:
            return this->parent_module->name();
        default:
            RELEASE_ASSERT(0, "%d", ast->type);
    }
}

AST_arguments* SourceInfo::getArgsAST() {
    assert(ast);
    switch (ast->type) {
        case AST_TYPE::FunctionDef:
            return ast_cast<AST_FunctionDef>(ast)->args;
        case AST_TYPE::Module:
            return NULL;
        default:
            RELEASE_ASSERT(0, "%d", ast->type);
    }
}

const std::vector<AST_expr*>& SourceInfo::getArgNames() {
    static std::vector<AST_expr*> empty;
    assert(this);

    AST_arguments* args = getArgsAST();
    if (args == NULL)
        return empty;
    return args->args;
}

const std::vector<AST_stmt*>& SourceInfo::getBody() {
    assert(ast);
    switch (ast->type) {
        case AST_TYPE::FunctionDef:
            return ast_cast<AST_FunctionDef>(ast)->body;
        case AST_TYPE::Module:
            return ast_cast<AST_Module>(ast)->body;
        default:
            RELEASE_ASSERT(0, "%d", ast->type);
    }
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
    if (effort > EffortLevel::INTERPRETED) {
        Timer _t("to jit the IR");
        g.engine->addModule(cf->func->getParent());
        compiled = (void*)g.engine->getFunctionAddress(cf->func->getName());
        assert(compiled);
        cf->llvm_code = embedConstantPtr(compiled, cf->func->getType());

        long us = _t.end();
        static StatCounter us_jitting("us_compiling_jitting");
        us_jitting.log(us);
        static StatCounter num_jits("num_jits");
        num_jits.log();
    } else {
        // HAX just get it for now; this is just to make sure everything works
        //(void*)g.func_registry.getFunctionAddress(cf->func->getName());
    }

    cf->code = compiled;
    if (VERBOSITY("irgen") >= 1) {
        printf("Compiled function to %p\n", compiled);
    }

    StackMap* stackmap = parseStackMap();
    patchpoints::processStackmap(stackmap);
}

// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
CompiledFunction* compileFunction(CLFunction* f, FunctionSignature* sig, EffortLevel::EffortLevel effort,
                                  const OSREntryDescriptor* entry) {
    Timer _t("for compileFunction()");
    assert(sig);

    ASSERT(f->versions.size() < 20, "%ld", f->versions.size());
    SourceInfo* source = f->source;
    assert(source);

    std::string name = source->getName();
    const std::vector<AST_expr*>& arg_names = source->getArgNames();
    AST_arguments* args = source->getArgsAST();

    if (args) {
        // args object can be NULL if this is a module scope
        assert(!args->vararg.size());
        assert(!args->kwarg.size());
        assert(!args->defaults.size());
    }

    if (VERBOSITY("irgen") >= 1) {
        std::string s;
        llvm::raw_string_ostream ss(s);

        ss << "\033[34;1mJIT'ing " << name << " with signature (";
        for (int i = 0; i < sig->arg_types.size(); i++) {
            if (i > 0)
                ss << ", ";
            ss << sig->arg_types[i]->debugName();
            // sig->arg_types[i]->llvmType()->print(ss);
        }
        ss << ") -> ";
        ss << sig->rtn_type->debugName();
        // sig->rtn_type->llvmType()->print(ss);
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
        assert(source->ast);
        source->cfg = computeCFG(source->ast->type, source->getBody());
        source->liveness = computeLivenessInfo(source->cfg);
        source->phis = computeRequiredPhis(args, source->cfg, source->liveness,
                                           source->scoping->getScopeInfoForNode(source->ast));
    }

    CompiledFunction* cf = doCompile(source, entry, effort, sig, arg_names, name);

    compileIR(cf, effort);
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
    Timer _t("for compileModule()");

    ScopingAnalysis* scoping = runScopingAnalysis(m);

    SourceInfo* si = new SourceInfo(bm, scoping);
    si->cfg = computeCFG(AST_TYPE::Module, m->body);
    si->ast = m;
    si->liveness = computeLivenessInfo(si->cfg);
    si->phis = computeRequiredPhis(NULL, si->cfg, si->liveness, si->scoping->getScopeInfoForNode(si->ast));

    CLFunction* cl_f = new CLFunction(si);

    EffortLevel::EffortLevel effort = initialEffort();

    CompiledFunction* cf
        = compileFunction(cl_f, new FunctionSignature(VOID, &si->getArgNames(), 0, false, false), effort, NULL);
    assert(cf->clfunc->versions.size());

    _t.end();

    if (cf->is_interpreted)
        interpretFunction(cf->func, 0, NULL, NULL, NULL, NULL);
    else
        ((void (*)())cf->code)();
}

/// Reoptimizes the given function version at the new effort level.
/// The cf must be an active version in its parents CLFunction; the given
/// version will be replaced by the new version, which will be returned.
static CompiledFunction* _doReopt(CompiledFunction* cf, EffortLevel::EffortLevel new_effort) {
    assert(cf->clfunc->versions.size());

    assert(cf);
    assert(cf->entry_descriptor == NULL && "We can't reopt an osr-entry compile!");
    assert(cf->sig);

    CLFunction* clfunc = cf->clfunc;
    assert(clfunc);

    assert(new_effort > cf->effort);

    FunctionList& versions = clfunc->versions;
    for (int i = 0; i < versions.size(); i++) {
        if (versions[i] == cf) {
            versions.erase(versions.begin() + i);

            CompiledFunction* new_cf
                = compileFunction(clfunc, cf->sig, new_effort,
                                  NULL); // this pushes the new CompiledVersion to the back of the version list

            cf->dependent_callsites.invalidateAll();

            return new_cf;
        }
    }
    assert(0 && "Couldn't find a version to reopt! Probably reopt'd already?");
    abort();
}

static StatCounter stat_osrexits("OSR exits");
void* compilePartialFunc(OSRExit* exit) {
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
            = compileFunction(exit->parent_cf->clfunc, exit->parent_cf->sig, new_effort, exit->entry);
        assert(compiled = new_cf);
    }

    return new_cf->code;
}

static StatCounter stat_reopt("reopts");
extern "C" char* reoptCompiledFunc(CompiledFunction* cf) {
    if (VERBOSITY("irgen") >= 1)
        printf("In reoptCompiledFunc, %p, %ld\n", cf, cf->times_called);
    stat_reopt.log();

    assert(cf->effort < EffortLevel::MAXIMAL);
    assert(cf->clfunc->versions.size());
    CompiledFunction* new_cf = _doReopt(cf, (EffortLevel::EffortLevel(cf->effort + 1)));
    assert(!new_cf->is_interpreted);
    return (char*)new_cf->code;
}

CLFunction* createRTFunction() {
    return new CLFunction(NULL);
}

extern "C" CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs, bool takes_varargs,
                                     bool takes_kwargs) {
    CLFunction* cl_f = createRTFunction();

    addRTFunction(cl_f, f, rtn_type, nargs, takes_varargs, takes_kwargs);
    return cl_f;
}

void addRTFunction(CLFunction* cl_f, void* f, ConcreteCompilerType* rtn_type, int nargs, bool takes_varargs,
                   bool takes_kwargs) {
    std::vector<ConcreteCompilerType*> arg_types(nargs, NULL);
    return addRTFunction(cl_f, f, rtn_type, arg_types, takes_varargs, takes_kwargs);
}

static ConcreteCompilerType* processType(ConcreteCompilerType* type) {
    if (type == NULL)
        return UNKNOWN;
    return type;
}

void addRTFunction(CLFunction* cl_f, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types, bool takes_varargs, bool takes_kwargs) {
    FunctionSignature* sig = new FunctionSignature(processType(rtn_type), NULL, 0, takes_varargs, takes_kwargs);

    for (int i = 0; i < arg_types.size(); i++) {
        sig->arg_types.push_back(processType(arg_types[i]));
    }

    std::vector<llvm::Type*> llvm_arg_types;
    int npassed_args = arg_types.size();
    if (takes_varargs)
        npassed_args++;
    if (takes_kwargs)
        npassed_args++;
    for (int i = 0; i < npassed_args; i++) {
        if (i == 3) {
            llvm_arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
            break;
        }
        llvm_arg_types.push_back(g.llvm_value_type_ptr);
    }

    llvm::FunctionType* ft = llvm::FunctionType::get(g.llvm_value_type_ptr, llvm_arg_types, false);

    cl_f->addVersion(
        new CompiledFunction(NULL, sig, false, f, embedConstantPtr(f, ft->getPointerTo()), EffortLevel::MAXIMAL, NULL));
}
}
