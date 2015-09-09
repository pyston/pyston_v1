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
CompiledFunction* compileFunction(CLFunction* f, FunctionSpecialization* spec, EffortLevel effort,
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
    CLFunction* clfunc;

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

        clfunc = new CLFunction(0, false, false, std::move(si));
    }

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_interpreted_module_toplevel");
    Box* r = astInterpretFunction(clfunc, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    assert(r == None);
}

Box* evalOrExec(CLFunction* cl, Box* globals, Box* boxedLocals) {
    RELEASE_ASSERT(!cl->source->scoping->areGlobalsFromModule(), "");

    assert(globals && (globals->cls == module_cls || globals->cls == dict_cls));

    Box* doc_string = cl->source->getDocString();
    if (doc_string != None) {
        static BoxedString* doc_box = internStringImmortal("__doc__");
        setGlobal(boxedLocals, doc_box, doc_string);
    }

    return astInterpretFunctionEval(cl, globals, boxedLocals);
}

static CLFunction* compileForEvalOrExec(AST* source, std::vector<AST_stmt*> body, BoxedString* fn,
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
    CLFunction* cl_f = new CLFunction(0, false, false, std::move(si));

    return cl_f;
}

static AST_Module* parseExec(llvm::StringRef source, bool interactive = false) {
    // TODO error message if parse fails or if it isn't an expr
    // TODO should have a cleaner interface that can parse the Expression directly
    // TODO this memory leaks
    const char* code = source.data();
    AST_Module* parsedModule = parse_string(code);

    if (interactive) {
        for (int i = 0; i < parsedModule->body.size(); ++i) {
            AST_stmt* s = parsedModule->body[i];
            if (s->type != AST_TYPE::Expr)
                continue;

            AST_Expr* expr = (AST_Expr*)s;
            AST_LangPrimitive* print_expr = new AST_LangPrimitive(AST_LangPrimitive::PRINT_EXPR);
            print_expr->args.push_back(expr->value);
            expr->value = print_expr;
        }
    }

    return parsedModule;
}

static CLFunction* compileExec(AST_Module* parsedModule, BoxedString* fn, PyCompilerFlags* flags) {
    return compileForEvalOrExec(parsedModule, parsedModule->body, fn, flags);
}

static AST_Expression* parseEval(llvm::StringRef source) {
    const char* code = source.data();

    // TODO error message if parse fails or if it isn't an expr
    // TODO should have a cleaner interface that can parse the Expression directly
    // TODO this memory leaks

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
    return parsedExpr;
}

static CLFunction* compileEval(AST_Expression* parsedExpr, BoxedString* fn, PyCompilerFlags* flags) {
    // We need body (list of statements) to compile.
    // Obtain this by simply making a single statement which contains the expression.
    AST_Return* stmt = new AST_Return();
    stmt->value = parsedExpr->body;
    std::vector<AST_stmt*> body = { stmt };

    return compileForEvalOrExec(parsedExpr, std::move(body), fn, flags);
}

Box* compile(Box* source, Box* fn, Box* type, Box** _args) {
    Box* flags = _args[0];

    RELEASE_ASSERT(PyInt_Check(_args[1]), "");
    bool dont_inherit = (bool)static_cast<BoxedInt*>(_args[1])->n;

    RELEASE_ASSERT(flags->cls == int_cls, "");
    int64_t iflags = static_cast<BoxedInt*>(flags)->n;

    // source is allowed to be an AST, unicode, or anything that supports the buffer protocol
    if (source->cls == unicode_cls) {
        source = PyUnicode_AsUTF8String(source);
        if (!source)
            throwCAPIException();
        // cf.cf_flags |= PyCF_SOURCE_IS_UTF8
    }

    if (isSubclass(fn->cls, unicode_cls)) {
        fn = _PyUnicode_AsDefaultEncodedString(fn, NULL);
        if (!fn)
            throwCAPIException();
    }
    RELEASE_ASSERT(PyString_Check(fn), "");

    if (isSubclass(type->cls, unicode_cls)) {
        type = _PyUnicode_AsDefaultEncodedString(type, NULL);
        if (!type)
            throwCAPIException();
    }
    RELEASE_ASSERT(PyString_Check(type), "");

    BoxedString* filename_str = static_cast<BoxedString*>(fn);
    BoxedString* type_str = static_cast<BoxedString*>(type);

    if (iflags & ~(PyCF_MASK | PyCF_MASK_OBSOLETE | /* PyCF_DONT_IMPLY_DEDENT | */ PyCF_ONLY_AST)) {
        raiseExcHelper(ValueError, "compile(): unrecognised flags");
    }

    bool only_ast = (bool)(iflags & PyCF_ONLY_AST);

    iflags &= ~PyCF_ONLY_AST;

    FutureFlags arg_future_flags = iflags & PyCF_MASK;
    FutureFlags future_flags;
    if (dont_inherit) {
        future_flags = arg_future_flags;
    } else {
        CLFunction* caller_cl = getTopPythonFunction();
        assert(caller_cl != NULL);
        assert(caller_cl->source != NULL);
        FutureFlags caller_future_flags = caller_cl->source->future_flags;
        future_flags = arg_future_flags | caller_future_flags;
    }

    iflags &= !(PyCF_MASK | PyCF_MASK_OBSOLETE);
    RELEASE_ASSERT(iflags == 0, "");

    AST* parsed;

    if (PyAST_Check(source)) {
        parsed = unboxAst(source);
    } else {
        RELEASE_ASSERT(PyString_Check(source), "");
        llvm::StringRef source_str = static_cast<BoxedString*>(source)->s();

        if (type_str->s() == "exec") {
            parsed = parseExec(source_str);
        } else if (type_str->s() == "eval") {
            parsed = parseEval(source_str);
        } else if (type_str->s() == "single") {
            parsed = parseExec(source_str, true);
        } else {
            raiseExcHelper(ValueError, "compile() arg 3 must be 'exec', 'eval' or 'single'");
        }
    }

    if (only_ast)
        return boxAst(parsed);

    PyCompilerFlags pcf;
    pcf.cf_flags = future_flags;

    CLFunction* cl;
    if (type_str->s() == "exec" || type_str->s() == "single") {
        // TODO: CPython parses execs as Modules
        if (parsed->type != AST_TYPE::Module)
            raiseExcHelper(TypeError, "expected Module node, got %s", boxAst(parsed)->cls->tp_name);
        cl = compileExec(static_cast<AST_Module*>(parsed), filename_str, &pcf);
    } else if (type_str->s() == "eval") {
        if (parsed->type != AST_TYPE::Expression)
            raiseExcHelper(TypeError, "expected Expression node, got %s", boxAst(parsed)->cls->tp_name);
        cl = compileEval(static_cast<AST_Expression*>(parsed), filename_str, &pcf);
    } else {
        raiseExcHelper(ValueError, "compile() arg 3 must be 'exec', 'eval' or 'single'");
    }

    return (Box*)cl->getCode();
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
        if (PyDict_GetItemString(globals_dict, "__builtins__") == NULL)
            PyDict_SetItemString(globals_dict, "__builtins__", builtins_module);
    }
}

static Box* evalMain(Box* boxedCode, Box* globals, Box* locals, PyCompilerFlags* flags) {
    pickGlobalsAndLocals(globals, locals);

    if (boxedCode->cls == unicode_cls) {
        boxedCode = PyUnicode_AsUTF8String(boxedCode);
        if (!boxedCode)
            throwCAPIException();
        // cf.cf_flags |= PyCF_SOURCE_IS_UTF8
    }

    CLFunction* cl;
    if (boxedCode->cls == str_cls) {
        AST_Expression* parsed = parseEval(static_cast<BoxedString*>(boxedCode)->s());
        static BoxedString* string_string = internStringImmortal("<string>");
        cl = compileEval(parsed, string_string, flags);
    } else if (boxedCode->cls == code_cls) {
        cl = clfunctionFromCode(boxedCode);
    } else {
        abort();
    }

    return evalOrExec(cl, globals, locals);
}

Box* eval(Box* boxedCode, Box* globals, Box* locals) {
    CLFunction* caller_cl = getTopPythonFunction();
    assert(caller_cl != NULL);
    assert(caller_cl->source != NULL);
    FutureFlags caller_future_flags = caller_cl->source->future_flags;
    PyCompilerFlags pcf;
    pcf.cf_flags = caller_future_flags;

    return evalMain(boxedCode, globals, locals, &pcf);
}

Box* execfile(Box* _fn, Box* globals, Box* locals) {
    if (!PyString_Check(_fn)) {
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(_fn));
    }

    BoxedString* fn = static_cast<BoxedString*>(_fn);

    pickGlobalsAndLocals(globals, locals);

#if LLVMREV < 217625
    bool exists;
    llvm_error_code code = llvm::sys::fs::exists(fn->s, exists);

#if LLVMREV < 210072
    ASSERT(code == 0, "%s: %s", code.message().c_str(), fn->s.c_str());
#else
    assert(!code);
#endif

#else
    bool exists = llvm::sys::fs::exists(std::string(fn->s()));
#endif

    if (!exists)
        raiseExcHelper(IOError, "No such file or directory: '%s'", fn->s().data());

    AST_Module* parsed = caching_parse_file(fn->s().data());
    assert(parsed);

    CLFunction* caller_cl = getTopPythonFunction();
    assert(caller_cl != NULL);
    assert(caller_cl->source != NULL);
    FutureFlags caller_future_flags = caller_cl->source->future_flags;
    PyCompilerFlags pcf;
    pcf.cf_flags = caller_future_flags;

    CLFunction* cl = compileForEvalOrExec(parsed, parsed->body, fn, &pcf);
    assert(cl);

    return evalOrExec(cl, globals, locals);
}

Box* execMain(Box* boxedCode, Box* globals, Box* locals, PyCompilerFlags* flags) {
    if (PyTuple_Check(boxedCode)) {
        RELEASE_ASSERT(!globals, "");
        RELEASE_ASSERT(!locals, "");

        BoxedTuple* t = static_cast<BoxedTuple*>(boxedCode);
        RELEASE_ASSERT(t->size() >= 2 && t->size() <= 3, "%ld", t->size());
        boxedCode = t->elts[0];
        globals = t->elts[1];
        if (t->size() >= 3)
            locals = t->elts[2];
    }

    pickGlobalsAndLocals(globals, locals);

    if (boxedCode->cls == unicode_cls) {
        boxedCode = PyUnicode_AsUTF8String(boxedCode);
        if (!boxedCode)
            throwCAPIException();
        // cf.cf_flags |= PyCF_SOURCE_IS_UTF8
    }

    CLFunction* cl;
    if (boxedCode->cls == str_cls) {
        auto parsed = parseExec(static_cast<BoxedString*>(boxedCode)->s());
        static BoxedString* string_string = internStringImmortal("<string>");
        cl = compileExec(parsed, string_string, flags);
    } else if (boxedCode->cls == code_cls) {
        cl = clfunctionFromCode(boxedCode);
    } else {
        abort();
    }
    assert(cl);

    return evalOrExec(cl, globals, locals);
}

Box* exec(Box* boxedCode, Box* globals, Box* locals, FutureFlags caller_future_flags) {
    PyCompilerFlags pcf;
    pcf.cf_flags = caller_future_flags;
    return execMain(boxedCode, globals, locals, &pcf);
}

extern "C" PyObject* PyRun_StringFlags(const char* str, int start, PyObject* globals, PyObject* locals,
                                       PyCompilerFlags* flags) noexcept {
    try {
        // TODO pass future_flags (the information is in PyCompilerFlags but we need to
        // unify the format...)
        if (start == Py_file_input)
            return execMain(boxString(str), globals, locals, flags);
        else if (start == Py_eval_input)
            return evalMain(boxString(str), globals, locals, flags);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }

    // Py_single_input is not yet implemented
    RELEASE_ASSERT(0, "Unimplemented %d", start);
    return 0;
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

        CLFunction* cl = this->clfunc;
        assert(cl);
        assert(this != cl->always_use_version);

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
            for (auto it = clfunc->osr_versions.begin(); it != clfunc->osr_versions.end(); ++it) {
                if (it->second == this) {
                    clfunc->osr_versions.erase(it);
                    this->dependent_callsites.invalidateAll();
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            for (int i = 0; i < clfunc->versions.size(); i++) {
                printf("%p\n", clfunc->versions[i]);
            }
        }
        RELEASE_ASSERT(found, "");
    }
}

std::unordered_set<CompiledFunction*> all_compiled_functions;
CompiledFunction::CompiledFunction(llvm::Function* func, FunctionSpecialization* spec, void* code, EffortLevel effort,
                                   ExceptionStyle exception_style, const OSREntryDescriptor* entry_descriptor)
    : clfunc(NULL),
      func(func),
      spec(spec),
      entry_descriptor(entry_descriptor),
      code(code),
      effort(effort),
      exception_style(exception_style),
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

            // this pushes the new CompiledVersion to the back of the version list
            CompiledFunction* new_cf = compileFunction(clfunc, cf->spec, new_effort, NULL, true, cf->exception_style);

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

    CLFunction* clfunc = exit->entry->clfunc;
    assert(clfunc);
    CompiledFunction*& new_cf = clfunc->osr_versions[exit->entry];
    if (new_cf == NULL) {
        EffortLevel new_effort = EffortLevel::MAXIMAL;
        CompiledFunction* compiled
            = compileFunction(clfunc, NULL, new_effort, exit->entry, true, exit->entry->exception_style);
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
    assert(cf->clfunc->versions.size());

    EffortLevel new_effort = EffortLevel::MAXIMAL;

    CompiledFunction* new_cf = _doReopt(cf, new_effort);
    return new_cf;
}


extern "C" char* reoptCompiledFunc(CompiledFunction* cf) {
    CompiledFunction* new_cf = reoptCompiledFuncInternal(cf);
    assert(new_cf->exception_style == cf->exception_style);
    return (char*)new_cf->code;
}

CLFunction* createRTFunction(int num_args, bool takes_varargs, bool takes_kwargs, const ParamNames& param_names) {
    return new CLFunction(num_args, takes_varargs, takes_kwargs, param_names);
}

CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int num_args, const ParamNames& param_names,
                          ExceptionStyle exception_style) {
    assert(!param_names.takes_param_names || num_args == param_names.args.size());
    assert(param_names.vararg.str() == "");
    assert(param_names.kwarg.str() == "");

    return boxRTFunction(f, rtn_type, num_args, false, false, param_names, exception_style);
}

CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int num_args, bool takes_varargs, bool takes_kwargs,
                          const ParamNames& param_names, ExceptionStyle exception_style) {
    assert(!param_names.takes_param_names || num_args == param_names.args.size());
    assert(takes_varargs || param_names.vararg.str() == "");
    assert(takes_kwargs || param_names.kwarg.str() == "");

    CLFunction* cl_f = createRTFunction(num_args, takes_varargs, takes_kwargs, param_names);

    addRTFunction(cl_f, f, rtn_type, exception_style);
    return cl_f;
}

void addRTFunction(CLFunction* cl_f, void* f, ConcreteCompilerType* rtn_type, ExceptionStyle exception_style) {
    std::vector<ConcreteCompilerType*> arg_types(cl_f->numReceivedArgs(), UNKNOWN);
    return addRTFunction(cl_f, f, rtn_type, arg_types, exception_style);
}

static ConcreteCompilerType* processType(ConcreteCompilerType* type) {
    assert(type);
    return type;
}

void addRTFunction(CLFunction* cl_f, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types, ExceptionStyle exception_style) {
    assert(arg_types.size() == cl_f->numReceivedArgs());
#ifndef NDEBUG
    for (ConcreteCompilerType* t : arg_types)
        assert(t);
#endif

    FunctionSpecialization* spec = new FunctionSpecialization(processType(rtn_type), arg_types);
    cl_f->addVersion(new CompiledFunction(NULL, spec, f, EffortLevel::MAXIMAL, exception_style, NULL));
}
}
