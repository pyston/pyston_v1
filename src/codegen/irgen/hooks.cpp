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

#include "codegen/irgen/hooks.h"

#include "codegen/cpython_ast.h"
// These #defines in Python-ast.h conflict with llvm:
#undef Pass
#undef Module
#undef alias
#undef Option
#undef Name
#undef Attribute
#undef Set

#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "asm_writing/icinfo.h"
#include "codegen/ast_interpreter.h"
#include "codegen/baseline_jit.h"
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
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// TODO terrible place for these!
ParamNames::ParamNames(AST* ast, InternedStringPool& pool)
    : takes_param_names(true), vararg_name(NULL), kwarg_name(NULL) {
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
                AST_Name* name = ast_cast<AST_Name>(arg);
                arg_names.push_back(name);
                args.push_back(name->id.s());
            } else {
                InternedString dot_arg_name = pool.get("." + std::to_string(i));
                arg_names.push_back(new AST_Name(dot_arg_name, AST_TYPE::Param, arg->lineno, arg->col_offset));
                args.push_back(dot_arg_name.s());
            }
        }

        vararg = arguments->vararg.s();
        if (vararg.size())
            vararg_name = new AST_Name(pool.get(vararg), AST_TYPE::Param, arguments->lineno, arguments->col_offset);

        kwarg = arguments->kwarg.s();
        if (kwarg.size())
            kwarg_name = new AST_Name(pool.get(kwarg), AST_TYPE::Param, arguments->lineno, arguments->col_offset);
    } else {
        RELEASE_ASSERT(0, "%d", ast->type);
    }
}

ParamNames::ParamNames(const std::vector<llvm::StringRef>& args, llvm::StringRef vararg, llvm::StringRef kwarg)
    : takes_param_names(true), vararg_name(NULL), kwarg_name(NULL) {
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

BoxedString* SourceInfo::getName() {
    assert(ast);

    static BoxedString* lambda_name = internStringImmortal("<lambda>");
    static BoxedString* module_name = internStringImmortal("<module>");

    switch (ast->type) {
        case AST_TYPE::ClassDef:
            return ast_cast<AST_ClassDef>(ast)->name;
        case AST_TYPE::FunctionDef:
            return ast_cast<AST_FunctionDef>(ast)->name;
        case AST_TYPE::Lambda:
            return lambda_name;
        case AST_TYPE::Module:
        case AST_TYPE::Expression:
        case AST_TYPE::Suite:
            return module_name;
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
    if (!scope_info)
        scope_info = scoping->getScopeInfoForNode(ast);
    return scope_info;
}

LivenessAnalysis* SourceInfo::getLiveness() {
    if (!liveness_info)
        liveness_info = computeLivenessInfo(cfg);
    return liveness_info.get();
}

static void compileIR(CompiledFunction* cf, EffortLevel effort) {
    assert(cf);
    assert(cf->func);

    void* compiled = NULL;
    cf->code = NULL;

    {
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
            printf("Took %.1fs to compile %s\n", us * 0.000001, cf->func->getName().data());
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
CompiledFunction* compileFunction(FunctionMetadata* f, FunctionSpecialization* spec, EffortLevel effort,
                                  const OSREntryDescriptor* entry_descriptor, bool force_exception_style,
                                  ExceptionStyle forced_exception_style) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_compileFunction");
    Timer _t("for compileFunction()", 1000);

    assert((entry_descriptor != NULL) + (spec != NULL) == 1);

    SourceInfo* source = f->source.get();
    assert(source);

    BoxedString* name = source->getName();

    ASSERT(f->versions.size() < 20, "%s %ld", name->c_str(), f->versions.size());

    ExceptionStyle exception_style;
    if (force_exception_style)
        exception_style = forced_exception_style;
    else if (FORCE_LLVM_CAPI_THROWS)
        exception_style = CAPI;
    else if (name->s() == "next")
        exception_style = CAPI;
    else if (f->propagated_cxx_exceptions >= 100)
        exception_style = CAPI;
    else
        exception_style = CXX;

    if (VERBOSITY("irgen") >= 1) {
        std::string s;
        llvm::raw_string_ostream ss(s);

        const char* colors[] = {
            "30",    // grey/black
            "34",    // blue
            "31",    // red
            "31;40", // red-on-black/grey
        };
        RELEASE_ASSERT((int)effort < sizeof(colors) / sizeof(colors[0]), "");

        if (spec) {
            ss << "\033[" << colors[(int)effort] << ";1mJIT'ing " << source->getFn()->s() << ":" << name->s()
               << " with signature (";
            for (int i = 0; i < spec->arg_types.size(); i++) {
                if (i > 0)
                    ss << ", ";
                ss << spec->arg_types[i]->debugName();
                // spec->arg_types[i]->llvmType()->print(ss);
            }
            ss << ") -> ";
            ss << spec->rtn_type->debugName();
        } else {
            ss << "\033[" << colors[(int)effort] << ";1mDoing OSR-entry partial compile of " << source->getFn()->s()
               << ":" << name->s() << ", starting with backedge to block " << entry_descriptor->backedge->target->idx;
        }
        ss << " at effort level " << (int)effort << " with exception style "
           << (exception_style == CXX ? "C++" : "CAPI") << '\n';

        if (entry_descriptor && VERBOSITY("irgen") >= 2) {
            for (const auto& p : entry_descriptor->args) {
                ss << p.first.s() << ": " << p.second->debugName() << '\n';
            }
        }

        ss << "\033[0m";
        printf("%s", ss.str().c_str());
    }

    // Do the analysis now if we had deferred it earlier:
    if (source->cfg == NULL) {
        source->cfg = computeCFG(source, source->body);
    }


    CompiledFunction* cf
        = doCompile(f, source, &f->param_names, entry_descriptor, effort, exception_style, spec, name->s());
    compileIR(cf, effort);

    f->addVersion(cf);

    long us = _t.end();
    static StatCounter us_compiling("us_compiling");
    us_compiling.log(us);
    if (VERBOSITY() >= 1 && us > 100000) {
        printf("Took %ldms to compile %s::%s (effort %d)!\n", us / 1000, source->getFn()->c_str(), name->c_str(),
               (int)effort);
    }

    static StatCounter num_compiles("num_compiles");
    num_compiles.log();

    switch (effort) {
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
    FunctionMetadata* md;

    { // scope for limiting the locked region:
        LOCK_REGION(codegen_rwlock.asWrite());

        Timer _t("for compileModule()");

        const char* fn = PyModule_GetFilename(bm);
        RELEASE_ASSERT(fn, "");

        FutureFlags future_flags = getFutureFlags(m->body, fn);
        ScopingAnalysis* scoping = new ScopingAnalysis(m, true);

        std::unique_ptr<SourceInfo> si(new SourceInfo(bm, scoping, future_flags, m, m->body, boxString(fn)));

        static BoxedString* doc_str = internStringImmortal("__doc__");
        bm->setattr(doc_str, si->getDocString(), NULL);

        static BoxedString* builtins_str = internStringImmortal("__builtins__");
        if (!bm->hasattr(builtins_str))
            bm->giveAttr(builtins_str, PyModule_GetDict(builtins_module));

        md = new FunctionMetadata(0, false, false, std::move(si));
    }

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_interpreted_module_toplevel");
    Box* r = astInterpretFunction(md, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    assert(r == None);
}

Box* evalOrExec(FunctionMetadata* md, Box* globals, Box* boxedLocals) {
    RELEASE_ASSERT(!md->source->scoping->areGlobalsFromModule(), "");

    assert(globals && (globals->cls == module_cls || globals->cls == dict_cls));

    Box* doc_string = md->source->getDocString();
    if (doc_string != None) {
        static BoxedString* doc_box = internStringImmortal("__doc__");
        setGlobal(boxedLocals, doc_box, doc_string);
    }

    return astInterpretFunctionEval(md, globals, boxedLocals);
}

static FunctionMetadata* compileForEvalOrExec(AST* source, std::vector<AST_stmt*> body, BoxedString* fn,
                                              PyCompilerFlags* flags) {
    LOCK_REGION(codegen_rwlock.asWrite());

    Timer _t("for evalOrExec()");

    ScopingAnalysis* scoping = new ScopingAnalysis(source, false);

    // `my_future_flags` are the future flags enabled in the exec's code.
    // `caller_future_flags` are the future flags of the source that the exec statement is in.
    // We need to enable features that are enabled in either.
    FutureFlags caller_future_flags = flags ? flags->cf_flags : 0;
    FutureFlags my_future_flags = getFutureFlags(body, fn->c_str());
    FutureFlags future_flags = caller_future_flags | my_future_flags;

    if (flags) {
        flags->cf_flags = future_flags;
    }

    std::unique_ptr<SourceInfo> si(
        new SourceInfo(getCurrentModule(), scoping, future_flags, source, std::move(body), fn));

    FunctionMetadata* md = new FunctionMetadata(0, false, false, std::move(si));
    return md;
}

static FunctionMetadata* compileExec(AST_Module* parsedModule, BoxedString* fn, PyCompilerFlags* flags) {
    return compileForEvalOrExec(parsedModule, parsedModule->body, fn, flags);
}

static FunctionMetadata* compileEval(AST_Expression* parsedExpr, BoxedString* fn, PyCompilerFlags* flags) {
    // We need body (list of statements) to compile.
    // Obtain this by simply making a single statement which contains the expression.
    AST_Return* stmt = new AST_Return();
    stmt->value = parsedExpr->body;
    std::vector<AST_stmt*> body = { stmt };

    return compileForEvalOrExec(parsedExpr, std::move(body), fn, flags);
}

extern "C" PyCodeObject* PyAST_Compile(struct _mod* _mod, const char* filename, PyCompilerFlags* flags,
                                       PyArena* arena) noexcept {
    try {
        mod_ty mod = _mod;
        AST* parsed = cpythonToPystonAST(mod, filename);
        FunctionMetadata* md = NULL;
        switch (mod->kind) {
            case Module_kind:
            case Interactive_kind:
                if (parsed->type != AST_TYPE::Module) {
                    raiseExcHelper(TypeError, "expected Module node, got %s", AST_TYPE::stringify(parsed->type));
                }
                md = compileExec(static_cast<AST_Module*>(parsed), boxString(filename), flags);
                break;
            case Expression_kind:
                if (parsed->type != AST_TYPE::Expression) {
                    raiseExcHelper(TypeError, "expected Expression node, got %s", AST_TYPE::stringify(parsed->type));
                }
                md = compileEval(static_cast<AST_Expression*>(parsed), boxString(filename), flags);
                break;
            case Suite_kind:
                PyErr_SetString(PyExc_SystemError, "suite should not be possible");
                return NULL;
            default:
                PyErr_Format(PyExc_SystemError, "module kind %d should not be possible", mod->kind);
                return NULL;
        }

        return (PyCodeObject*)md->getCode();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyEval_MergeCompilerFlags(PyCompilerFlags* cf) noexcept {
    int result = cf->cf_flags != 0;

    /* Pyston change:
    PyFrameObject *current_frame = PyEval_GetFrame();
    int result = cf->cf_flags != 0;

    if (current_frame != NULL) {
        const int codeflags = current_frame->f_code->co_flags;
    */

    FunctionMetadata* caller_cl = getTopPythonFunction();
    if (caller_cl != NULL) {
        assert(caller_cl->source != NULL);
        FutureFlags caller_future_flags = caller_cl->source->future_flags;

        const int codeflags = caller_future_flags;
        const int compilerflags = codeflags & PyCF_MASK;
        if (compilerflags) {
            result = 1;
            cf->cf_flags |= compilerflags;
        }
#if 0 /* future keyword */
        if (codeflags & CO_GENERATOR_ALLOWED) {
            result = 1;
            cf->cf_flags |= CO_GENERATOR_ALLOWED;
        }
#endif
    }
    return result;
}

static void pickGlobalsAndLocals(Box*& globals, Box*& locals) {
    if (globals == None)
        globals = NULL;

    if (locals == None)
        locals = NULL;

    if (locals == NULL) {
        locals = globals;
    }

    if (locals == NULL) {
        locals = fastLocalsToBoxedLocals();
    }

    if (globals == NULL)
        globals = getGlobals();

    BoxedModule* module = getCurrentModule();
    if (globals && globals->cls == attrwrapper_cls && unwrapAttrWrapper(globals) == module)
        globals = module;

    if (globals->cls == attrwrapper_cls)
        globals = unwrapAttrWrapper(globals);

    assert(globals && (globals->cls == module_cls || globals->cls == dict_cls));

    if (globals) {
        // From CPython (they set it to be f->f_builtins):
        Box* globals_dict = globals;
        if (globals->cls == module_cls)
            globals_dict = globals->getAttrWrapper();

        auto requested_builtins = PyDict_GetItemString(globals_dict, "__builtins__");
        if (requested_builtins == NULL)
            PyDict_SetItemString(globals_dict, "__builtins__", builtins_module);
        else
            RELEASE_ASSERT(requested_builtins == builtins_module
                               || requested_builtins == builtins_module->getAttrWrapper(),
                           "we don't support overriding __builtins__");
    }
}

extern "C" PyObject* PyEval_EvalCode(PyCodeObject* co, PyObject* globals, PyObject* locals) noexcept {
    try {
        pickGlobalsAndLocals(globals, locals);
        return evalOrExec(metadataFromCode((Box*)co), globals, locals);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* exec(Box* boxedCode, Box* globals, Box* locals, FutureFlags caller_future_flags) {
    if (!globals)
        globals = None;
    if (!locals)
        locals = None;

    // this is based on cpythons exec_statement() but (heavily) adopted
    Box* v = NULL;
    int plain = 0;
    int n;
    PyObject* prog = boxedCode;
    if (PyTuple_Check(prog) && globals == Py_None && locals == Py_None && ((n = PyTuple_Size(prog)) == 2 || n == 3)) {
        /* Backward compatibility hack */
        globals = PyTuple_GetItem(prog, 1);
        if (n == 3)
            locals = PyTuple_GetItem(prog, 2);
        prog = PyTuple_GetItem(prog, 0);
    }
    if (globals == Py_None) {
        globals = PyEval_GetGlobals();
        if (locals == Py_None) {
            locals = PyEval_GetLocals();
            plain = 1;
        }
        if (!globals || !locals) {
            raiseExcHelper(SystemError, "globals and locals cannot be NULL");
        }
    } else if (locals == Py_None)
        locals = globals;
    if (!PyString_Check(prog) &&
#ifdef Py_USING_UNICODE
        !PyUnicode_Check(prog) &&
#endif
        !PyCode_Check(prog) && !PyFile_Check(prog)) {
        raiseExcHelper(TypeError, "exec: arg 1 must be a string, file, or code object");
    }

    if (!PyDict_Check(globals) && globals->cls != attrwrapper_cls) {
        raiseExcHelper(TypeError, "exec: arg 2 must be a dictionary or None");
    }
    if (!PyMapping_Check(locals))
        raiseExcHelper(TypeError, "exec: arg 3 must be a mapping or None");

    if (PyDict_GetItemString(globals, "__builtins__") == NULL)
        // Pyston change:
        // PyDict_SetItemString(globals, "__builtins__", f->f_builtins);
        PyDict_SetItemString(globals, "__builtins__", builtins_module);

    if (PyCode_Check(prog)) {
        /* Pyston change:
        if (PyCode_GetNumFree((PyCodeObject *)prog) > 0) {
            PyErr_SetString(PyExc_TypeError,
        "code object passed to exec may not contain free variables");
            return -1;
        }
        */
        v = PyEval_EvalCode((PyCodeObject*)prog, globals, locals);
    } else if (PyFile_Check(prog)) {
        FILE* fp = PyFile_AsFile(prog);
        char* name = PyString_AsString(PyFile_Name(prog));
        PyCompilerFlags cf;
        if (name == NULL)
            throwCAPIException();
        cf.cf_flags = caller_future_flags & PyCF_MASK;
        if (cf.cf_flags)
            v = PyRun_FileFlags(fp, name, Py_file_input, globals, locals, &cf);
        else
            v = PyRun_File(fp, name, Py_file_input, globals, locals);
    } else {
        PyObject* tmp = NULL;
        char* str;
        PyCompilerFlags cf;
        cf.cf_flags = 0;
#ifdef Py_USING_UNICODE
        if (PyUnicode_Check(prog)) {
            tmp = PyUnicode_AsUTF8String(prog);
            if (tmp == NULL)
                throwCAPIException();
            prog = tmp;
            cf.cf_flags |= PyCF_SOURCE_IS_UTF8;
        }
#endif
        if (PyString_AsStringAndSize(prog, &str, NULL))
            return 0;
        cf.cf_flags |= caller_future_flags & PyCF_MASK;
        if (cf.cf_flags)
            v = PyRun_StringFlags(str, Py_file_input, globals, locals, &cf);
        else
            v = PyRun_String(str, Py_file_input, globals, locals);
        Py_XDECREF(tmp);
    }

    if (!v)
        throwCAPIException();
    return v;
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

    if (this->times_speculation_failed == 4) {
        // printf("Killing %p because it failed too many speculations\n", this);

        FunctionMetadata* md = this->md;
        assert(md);
        assert(this != md->always_use_version);

        bool found = false;
        for (int i = 0; i < md->versions.size(); i++) {
            if (md->versions[i] == this) {
                md->versions.erase(md->versions.begin() + i);
                this->dependent_callsites.invalidateAll();
                found = true;
                break;
            }
        }

        if (!found) {
            for (auto it = md->osr_versions.begin(); it != md->osr_versions.end(); ++it) {
                if (it->second == this) {
                    md->osr_versions.erase(it);
                    this->dependent_callsites.invalidateAll();
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            for (int i = 0; i < md->versions.size(); i++) {
                printf("%p\n", md->versions[i]);
            }
        }
        RELEASE_ASSERT(found, "");
    }
}

std::unordered_set<CompiledFunction*> all_compiled_functions;
CompiledFunction::CompiledFunction(llvm::Function* func, FunctionSpecialization* spec, void* code, EffortLevel effort,
                                   ExceptionStyle exception_style, const OSREntryDescriptor* entry_descriptor)
    : md(NULL),
      func(func),
      effort(effort),
      exception_style(exception_style),
      spec(spec),
      entry_descriptor(entry_descriptor),
      code(code),
      times_called(0),
      times_speculation_failed(0),
      location_map(nullptr) {
    assert((spec != NULL) + (entry_descriptor != NULL) == 1);

#if MOVING_GC
    assert(all_compiled_functions.count(this) == 0);
    all_compiled_functions.insert(this);
#endif
}

#if MOVING_GC
CompiledFunction::~CompiledFunction() {
    assert(all_compiled_functions.count(this) == 1);
    all_compiled_functions.erase(this);
}
#endif

void CompiledFunction::visitAllCompiledFunctions(GCVisitor* visitor) {
    for (CompiledFunction* cf : all_compiled_functions) {
        for (const void* ptr : cf->pointers_in_code) {
            visitor->visitNonRelocatable(const_cast<void*>(ptr));
        }
    }
}

ConcreteCompilerType* CompiledFunction::getReturnType() {
    assert(((bool)spec) ^ ((bool)entry_descriptor));
    if (spec)
        return spec->rtn_type;
    else
        return UNKNOWN;
}

/// Reoptimizes the given function version at the new effort level.
/// The cf must be an active version in its parents FunctionMetadata; the given
/// version will be replaced by the new version, which will be returned.
static CompiledFunction* _doReopt(CompiledFunction* cf, EffortLevel new_effort) {
    LOCK_REGION(codegen_rwlock.asWrite());

    assert(cf->md->versions.size());

    assert(cf);
    assert(cf->entry_descriptor == NULL && "We can't reopt an osr-entry compile!");
    assert(cf->spec);

    FunctionMetadata* md = cf->md;
    assert(md);

    assert(new_effort > cf->effort);

    FunctionList& versions = md->versions;
    for (int i = 0; i < versions.size(); i++) {
        if (versions[i] == cf) {
            versions.erase(versions.begin() + i);

            // this pushes the new CompiledVersion to the back of the version list
            CompiledFunction* new_cf = compileFunction(md, cf->spec, new_effort, NULL, true, cf->exception_style);

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
    stat_osrexits.log();

    // if (VERBOSITY("irgen") >= 1) printf("In compilePartialFunc, handling %p\n", exit);

    FunctionMetadata* md = exit->entry->md;
    assert(md);
    CompiledFunction*& new_cf = md->osr_versions[exit->entry];
    if (new_cf == NULL) {
        EffortLevel new_effort = EffortLevel::MAXIMAL;
        CompiledFunction* compiled
            = compileFunction(md, NULL, new_effort, exit->entry, true, exit->entry->exception_style);
        assert(compiled == new_cf);

        stat_osr_compiles.log();
    }

    return new_cf;
}

void* compilePartialFunc(OSRExit* exit) {
    CompiledFunction* new_cf = compilePartialFuncInternal(exit);
    assert(new_cf->exception_style == exit->entry->exception_style);
    return new_cf->code;
}


static StatCounter stat_reopt("reopts");
extern "C" CompiledFunction* reoptCompiledFuncInternal(CompiledFunction* cf) {
    if (VERBOSITY("irgen") >= 2)
        printf("In reoptCompiledFunc, %p, %ld\n", cf, cf->times_called);
    stat_reopt.log();

    assert(cf->effort < EffortLevel::MAXIMAL);
    assert(cf->md->versions.size());

    EffortLevel new_effort = EffortLevel::MAXIMAL;

    CompiledFunction* new_cf = _doReopt(cf, new_effort);
    return new_cf;
}


extern "C" char* reoptCompiledFunc(CompiledFunction* cf) {
    CompiledFunction* new_cf = reoptCompiledFuncInternal(cf);
    assert(new_cf->exception_style == cf->exception_style);
    return (char*)new_cf->code;
}

void FunctionMetadata::addVersion(void* f, ConcreteCompilerType* rtn_type, ExceptionStyle exception_style) {
    std::vector<ConcreteCompilerType*> arg_types(numReceivedArgs(), UNKNOWN);
    return FunctionMetadata::addVersion(f, rtn_type, arg_types, exception_style);
}

static ConcreteCompilerType* processType(ConcreteCompilerType* type) {
    assert(type);
    return type;
}

void FunctionMetadata::addVersion(void* f, ConcreteCompilerType* rtn_type,
                                  const std::vector<ConcreteCompilerType*>& arg_types, ExceptionStyle exception_style) {
    assert(arg_types.size() == numReceivedArgs());
#ifndef NDEBUG
    for (ConcreteCompilerType* t : arg_types)
        assert(t);
#endif

    FunctionSpecialization* spec = new FunctionSpecialization(processType(rtn_type), arg_types);
    addVersion(new CompiledFunction(NULL, spec, f, EffortLevel::MAXIMAL, exception_style, NULL));
}
}
