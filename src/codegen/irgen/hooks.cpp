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
#include "codegen/osrentry.h"
#include "codegen/parser.h"
#include "codegen/patchpoints.h"
#include "codegen/stackmaps.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/capi.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// TODO terrible place for these!
ParamNames::ParamNames(AST* ast) : takes_param_names(true) {
    if (ast->type == AST_TYPE::Module || ast->type == AST_TYPE::ClassDef || ast->type == AST_TYPE::Expression
        || ast->type == AST_TYPE::Suite) {
        kwarg = "";
        vararg = "";
    } else if (ast->type == AST_TYPE::FunctionDef || ast->type == AST_TYPE::Lambda) {
        AST_arguments* arguments = ast->type == AST_TYPE::FunctionDef ? ast_cast<AST_FunctionDef>(ast)->args
                                                                      : ast_cast<AST_Lambda>(ast)->args;
        for (int i = 0; i < arguments->args.size(); i++) {
            AST_expr* arg = arguments->args[i];
            if (arg->type == AST_TYPE::Name) {
                args.push_back(ast_cast<AST_Name>(arg)->id.str());
            } else {
                args.push_back("." + std::to_string(i + 1));
            }
        }

        vararg = arguments->vararg.str();
        kwarg = arguments->kwarg.str();
    } else {
        RELEASE_ASSERT(0, "%d", ast->type);
    }
}

ParamNames::ParamNames(const std::vector<llvm::StringRef>& args, llvm::StringRef vararg, llvm::StringRef kwarg)
    : takes_param_names(true) {
    this->args = args;
    this->vararg = vararg;
    this->kwarg = kwarg;
}

InternedString SourceInfo::mangleName(InternedString id) {
    assert(ast);
    if (ast->type == AST_TYPE::Module)
        return id;
    return getScopeInfo()->mangleName(id);
}

InternedStringPool& SourceInfo::getInternedStrings() {
    return scoping->getInternedStrings();
}

const std::string SourceInfo::getName() {
    assert(ast);
    switch (ast->type) {
        case AST_TYPE::ClassDef:
            return ast_cast<AST_ClassDef>(ast)->name.str();
        case AST_TYPE::FunctionDef:
            return ast_cast<AST_FunctionDef>(ast)->name.str();
        case AST_TYPE::Lambda:
            return "<lambda>";
        case AST_TYPE::Module:
        case AST_TYPE::Expression:
        case AST_TYPE::Suite:
            return "<module>";
        default:
            RELEASE_ASSERT(0, "%d", ast->type);
    }
}

Box* SourceInfo::getDocString() {
    AST_Str* first_str = NULL;

    if (body.size() > 0 && body[0]->type == AST_TYPE::Expr
        && static_cast<AST_Expr*>(body[0])->value->type == AST_TYPE::Str) {
        return boxString(static_cast<AST_Str*>(static_cast<AST_Expr*>(body[0])->value)->str_data);
    }

    return None;
}

ScopeInfo* SourceInfo::getScopeInfo() {
    return scoping->getScopeInfoForNode(ast);
}

EffortLevel initialEffort() {
    if (FORCE_INTERPRETER)
        return EffortLevel::INTERPRETED;
    if (FORCE_OPTIMIZE)
        return EffortLevel::MAXIMAL;
    if (ENABLE_INTERPRETER)
        return EffortLevel::INTERPRETED;
    return EffortLevel::MINIMAL;
}

static void compileIR(CompiledFunction* cf, EffortLevel effort) {
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

        long us = _t.end();
        static StatCounter us_jitting("us_compiling_jitting");
        us_jitting.log(us);
        static StatCounter num_jits("num_jits");
        num_jits.log();

        if (VERBOSITY() >= 1 && us > 100000) {
            printf("Took %.1fs to compile %s\n", us * 0.000001, cf->func->getName().str().c_str());
            printf("Has %ld basic blocks\n", cf->func->getBasicBlockList().size());
        }
    }

    if (VERBOSITY("irgen") >= 2) {
        printf("Compiled function to %p\n", cf->code);
    }

    StackMap* stackmap = parseStackMap();
    processStackmap(cf, stackmap);
    delete stackmap;
}

// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
// The codegen_lock needs to be held in W mode before calling this function:
CompiledFunction* compileFunction(CLFunction* f, FunctionSpecialization* spec, EffortLevel effort,
                                  const OSREntryDescriptor* entry_descriptor) {
    Timer _t("for compileFunction()", 1000);

    assert((entry_descriptor != NULL) + (spec != NULL) == 1);

    SourceInfo* source = f->source;
    assert(source);

    std::string name = source->getName();

    ASSERT(f->versions.size() < 20, "%s %ld", name.c_str(), f->versions.size());

    if (VERBOSITY("irgen") >= 1) {
        std::string s;
        llvm::raw_string_ostream ss(s);

        if (spec) {
            ss << "\033[34;1mJIT'ing " << source->parent_module->fn << ":" << name << " with signature (";
            for (int i = 0; i < spec->arg_types.size(); i++) {
                if (i > 0)
                    ss << ", ";
                ss << spec->arg_types[i]->debugName();
                // spec->arg_types[i]->llvmType()->print(ss);
            }
            ss << ") -> ";
            ss << spec->rtn_type->debugName();
        } else {
            ss << "\033[34;1mDoing OSR-entry partial compile of " << source->parent_module->fn << ":" << name
               << ", starting with backedge to block " << entry_descriptor->backedge->target->idx;
        }
        ss << " at effort level " << (int)effort;
        ss << "\033[0m";
        printf("%s\n", ss.str().c_str());
    }

#ifndef NDEBUG
    if (effort == EffortLevel::INTERPRETED) {
        for (auto arg_type : spec->arg_types)
            assert(arg_type == UNKNOWN);
    }
#endif

    // Do the analysis now if we had deferred it earlier:
    if (source->cfg == NULL) {
        source->cfg = computeCFG(source, source->body);
    }

    if (effort != EffortLevel::INTERPRETED) {
        if (source->liveness == NULL)
            source->liveness = computeLivenessInfo(source->cfg);

        PhiAnalysis*& phis = source->phis[entry_descriptor];
        if (!phis) {
            if (entry_descriptor)
                phis = computeRequiredPhis(entry_descriptor, source->liveness, source->getScopeInfo());
            else
                phis = computeRequiredPhis(f->param_names, source->cfg, source->liveness, source->getScopeInfo());
        }
    }



    CompiledFunction* cf = 0;
    if (effort == EffortLevel::INTERPRETED) {
        assert(!entry_descriptor);
        cf = new CompiledFunction(0, spec, true, NULL, effort, 0);
    } else {
        cf = doCompile(source, &f->param_names, entry_descriptor, effort, spec, name);
        compileIR(cf, effort);
    }

    f->addVersion(cf);
    assert(f->versions.size());

    long us = _t.end();
    static StatCounter us_compiling("us_compiling");
    us_compiling.log(us);
    if (VERBOSITY() >= 1 && us > 100000) {
        printf("Took %ldms to compile %s::%s!\n", us / 1000, source->parent_module->fn.c_str(), name.c_str());
    }

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
        default:
            RELEASE_ASSERT(0, "%d", effort);
    }

    return cf;
}

void compileAndRunModule(AST_Module* m, BoxedModule* bm) {
    CompiledFunction* cf;

    { // scope for limiting the locked region:
        LOCK_REGION(codegen_rwlock.asWrite());

        Timer _t("for compileModule()");

        bm->future_flags = getFutureFlags(m, bm->fn.c_str());

        ScopingAnalysis* scoping = new ScopingAnalysis(m);

        SourceInfo* si = new SourceInfo(bm, scoping, m, m->body);
        CLFunction* cl_f = new CLFunction(0, 0, false, false, si);

        bm->setattr("__doc__", si->getDocString(), NULL);

        EffortLevel effort = initialEffort();

        assert(scoping->areGlobalsFromModule());

        cf = compileFunction(cl_f, new FunctionSpecialization(VOID), effort, NULL);
        assert(cf->clfunc->versions.size());
    }

    if (cf->is_interpreted)
        astInterpretFunction(cf, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    else
        ((void (*)())cf->code)();
}

template <typename AST_Type>
Box* evalOrExec(AST_Type* source, std::vector<AST_stmt*>& body, BoxedModule* bm, BoxedDict* globals, Box* boxedLocals) {
    CompiledFunction* cf;

    assert(!globals || globals->cls == dict_cls);

    { // scope for limiting the locked region:
        LOCK_REGION(codegen_rwlock.asWrite());

        Timer _t("for evalOrExec()");

        ScopingAnalysis* scoping = new ScopingAnalysis(source, globals == NULL);

        SourceInfo* si = new SourceInfo(bm, scoping, source, body);
        CLFunction* cl_f = new CLFunction(0, 0, false, false, si);

        // TODO Right now we only support going into an exec or eval through the
        // intepretter, since the interpretter has a special function which lets
        // us set the locals object. We should probably support it for optimized
        // code as well, so we could use initialEffort() here instead of hard-coding
        // INTERPRETED. This could actually be useful if we actually cache the parse
        // results (since sometimes eval or exec might be called on constant strings).
        EffortLevel effort = EffortLevel::INTERPRETED;

        cf = compileFunction(cl_f, new FunctionSpecialization(VOID), effort, NULL);
        assert(cf->clfunc->versions.size());
    }

    return astInterpretFunctionEval(cf, globals, boxedLocals);
}

// Main entrypoints for eval and exec.
Box* eval(Box* boxedCode) {
    Box* boxedLocals = fastLocalsToBoxedLocals();
    BoxedModule* module = getCurrentModule();
    Box* globals = getGlobals();

    if (globals == module)
        globals = NULL;

    if (globals && globals->cls == attrwrapper_cls && unwrapAttrWrapper(globals) == module)
        globals = NULL;

    if (boxedCode->cls == unicode_cls) {
        boxedCode = PyUnicode_AsUTF8String(boxedCode);
        if (!boxedCode)
            throwCAPIException();
        // cf.cf_flags |= PyCF_SOURCE_IS_UTF8
    }

    // TODO error message if parse fails or if it isn't an expr
    // TODO should have a cleaner interface that can parse the Expression directly
    // TODO this memory leaks
    RELEASE_ASSERT(boxedCode->cls == str_cls, "%s", boxedCode->cls->tp_name);
    const char* code = static_cast<BoxedString*>(boxedCode)->s.data();

    // Hack: we need to support things like `eval(" 2")`.
    // This is over-accepting since it will accept things like `eval("\n 2")`
    while (*code == ' ' || *code == '\t' || *code == '\n' || *code == '\r')
        code++;

    AST_Module* parsedModule = parse_string(code);
    if (parsedModule->body.size() == 0)
        raiseSyntaxError("unexpected EOF while parsing", 0, 0, "<string>", "");

    RELEASE_ASSERT(parsedModule->body.size() == 1, "");
    RELEASE_ASSERT(parsedModule->body[0]->type == AST_TYPE::Expr, "");
    AST_Expression* parsedExpr = new AST_Expression(std::move(parsedModule->interned_strings));
    parsedExpr->body = static_cast<AST_Expr*>(parsedModule->body[0])->value;

    // We need body (list of statements) to compile.
    // Obtain this by simply making a single statement which contains the expression.
    AST_Return* stmt = new AST_Return();
    stmt->value = parsedExpr->body;
    std::vector<AST_stmt*> body = { stmt };

    assert(!globals || globals->cls == dict_cls);

    return evalOrExec<AST_Expression>(parsedExpr, body, module, static_cast<BoxedDict*>(globals), boxedLocals);
}

Box* exec(Box* boxedCode, Box* globals, Box* locals) {
    if (isSubclass(boxedCode->cls, tuple_cls)) {
        RELEASE_ASSERT(!globals, "");
        RELEASE_ASSERT(!locals, "");

        BoxedTuple* t = static_cast<BoxedTuple*>(boxedCode);
        RELEASE_ASSERT(t->size() >= 2 && t->size() <= 3, "%ld", t->size());
        boxedCode = t->elts[0];
        globals = t->elts[1];
        if (t->size() >= 3)
            locals = t->elts[2];
    }

    if (globals == None)
        globals = NULL;

    if (locals == None)
        locals = NULL;

    // TODO boxedCode is allowed to be a tuple
    // TODO need to handle passing in globals
    if (locals == NULL) {
        locals = globals;
    }

    if (locals == NULL) {
        locals = fastLocalsToBoxedLocals();
    }

    if (globals == NULL)
        globals = getGlobals();

    BoxedModule* module = getCurrentModule();

    if (globals == module)
        globals = NULL;

    if (globals && globals->cls == attrwrapper_cls && unwrapAttrWrapper(globals) == module)
        globals = NULL;

    ASSERT(!globals || globals->cls == dict_cls, "%s", globals->cls->tp_name);

    if (globals) {
        // From CPython (they set it to be f->f_builtins):
        if (PyDict_GetItemString(globals, "__builtins__") == NULL)
            PyDict_SetItemString(globals, "__builtins__", builtins_module);
    }

    if (boxedCode->cls == unicode_cls) {
        boxedCode = PyUnicode_AsUTF8String(boxedCode);
        if (!boxedCode)
            throwCAPIException();
        // cf.cf_flags |= PyCF_SOURCE_IS_UTF8
    }

    // TODO same issues as in `eval`
    RELEASE_ASSERT(boxedCode->cls == str_cls, "%s", boxedCode->cls->tp_name);
    const char* code = static_cast<BoxedString*>(boxedCode)->s.data();
    AST_Module* parsedModule = parse_string(code);
    AST_Suite* parsedSuite = new AST_Suite(std::move(parsedModule->interned_strings));
    parsedSuite->body = parsedModule->body;

    return evalOrExec<AST_Suite>(parsedSuite, parsedSuite->body, module, static_cast<BoxedDict*>(globals), locals);
}

// If a function version keeps failing its speculations, kill it (remove it
// from the list of valid function versions).  The next time we go to call
// the function, we will have to pick a different version, potentially recompiling.
//
// TODO we should have logic like this at the CLFunc level that detects that we keep
// on creating functions with failing speculations, and then stop speculating.
void CompiledFunction::speculationFailed() {
    LOCK_REGION(codegen_rwlock.asWrite());

    this->times_speculation_failed++;

    if (this->times_speculation_failed >= 4) {
        // printf("Killing %p because it failed too many speculations\n", this);

        CLFunction* cl = this->clfunc;
        assert(cl);

        bool found = false;
        for (int i = 0; i < clfunc->versions.size(); i++) {
            if (clfunc->versions[i] == this) {
                clfunc->versions.erase(clfunc->versions.begin() + i);
                this->dependent_callsites.invalidateAll();
                found = true;
                break;
            }
        }

        if (!found) {
            for (int i = 0; i < clfunc->versions.size(); i++) {
                printf("%p\n", clfunc->versions[i]);
            }
        }
        assert(found);
    }
}

ConcreteCompilerType* CompiledFunction::getReturnType() {
    if (spec)
        return spec->rtn_type;
    return entry_descriptor->cf->getReturnType();
}

/// Reoptimizes the given function version at the new effort level.
/// The cf must be an active version in its parents CLFunction; the given
/// version will be replaced by the new version, which will be returned.
static CompiledFunction* _doReopt(CompiledFunction* cf, EffortLevel new_effort) {
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

static StatCounter stat_osrexits("num_osr_exits");
static StatCounter stat_osr_compiles("num_osr_compiles");
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
        EffortLevel new_effort = exit->parent_cf->effort == EffortLevel::INTERPRETED ? EffortLevel::MINIMAL
                                                                                     : EffortLevel::MAXIMAL;
        CompiledFunction* compiled = compileFunction(exit->parent_cf->clfunc, NULL, new_effort, exit->entry);
        assert(compiled == new_cf);

        stat_osr_compiles.log();
    }

    return new_cf;
}

void* compilePartialFunc(OSRExit* exit) {
    return compilePartialFuncInternal(exit)->code;
}


static StatCounter stat_reopt("reopts");
extern "C" CompiledFunction* reoptCompiledFuncInternal(CompiledFunction* cf) {
    if (VERBOSITY("irgen") >= 2)
        printf("In reoptCompiledFunc, %p, %ld\n", cf, cf->times_called);
    stat_reopt.log();

    assert(cf->effort < EffortLevel::MAXIMAL);
    assert(cf->clfunc->versions.size());

    EffortLevel new_effort;
    if (cf->effort == EffortLevel::INTERPRETED)
        new_effort = EffortLevel::MINIMAL;
    else if (cf->effort == EffortLevel::MINIMAL)
        new_effort = EffortLevel::MODERATE;
    else if (cf->effort == EffortLevel::MODERATE)
        new_effort = EffortLevel::MAXIMAL;
    else
        RELEASE_ASSERT(0, "unknown effort: %d", cf->effort);

    CompiledFunction* new_cf = _doReopt(cf, new_effort);
    assert(!new_cf->is_interpreted);
    return new_cf;
}


extern "C" char* reoptCompiledFunc(CompiledFunction* cf) {
    return (char*)reoptCompiledFuncInternal(cf)->code;
}

CLFunction* createRTFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs,
                             const ParamNames& param_names) {
    return new CLFunction(num_args, num_defaults, takes_varargs, takes_kwargs, param_names);
}

CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int num_args, const ParamNames& param_names) {
    assert(!param_names.takes_param_names || num_args == param_names.args.size());
    assert(param_names.vararg.str() == "");
    assert(param_names.kwarg.str() == "");

    return boxRTFunction(f, rtn_type, num_args, 0, false, false, param_names);
}

CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int num_args, int num_defaults, bool takes_varargs,
                          bool takes_kwargs, const ParamNames& param_names) {
    assert(!param_names.takes_param_names || num_args == param_names.args.size());
    assert(takes_varargs || param_names.vararg.str() == "");
    assert(takes_kwargs || param_names.kwarg.str() == "");

    CLFunction* cl_f = createRTFunction(num_args, num_defaults, takes_varargs, takes_kwargs, param_names);

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
    cl_f->addVersion(new CompiledFunction(NULL, spec, false, f, EffortLevel::MAXIMAL, NULL));
}
}
