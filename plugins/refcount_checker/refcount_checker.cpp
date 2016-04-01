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

// Includes code from the following examples/tutorials:
// - http://clang.llvm.org/docs/LibASTMatchersTutorial.html
// - http://clang.llvm.org/docs/RAVFrontendAction.html

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include "core/common.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace std;

SourceManager* SM;
CompilerInstance* CI;
ASTContext* Context;

/*
 * Features I think need to be added:
 * - incref() function
 * - autoDecref (destructors)
 * - return/break from loop
 * - storing to memory locations
 *   - esp with the "t = PyTuple_Create(1); PyTuple_SETITEM(t, 0, my_owned_reference);" pattern
 *
 * nice to haves:
 * - assert usable (can't use after last decref)
 * - separate in/out annotations
 * - better diagnostics
 * - nullability?
 */

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static cl::OptionCategory RefcheckingToolCategory("my-tool options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...");

static void dumpSingle(DeclContext* ctx) {
    errs() << ctx->getDeclKindName() << '\n';
    if (ctx->isClosure())
        errs() << "a closure\n";
    if (ctx->isFunctionOrMethod())
        errs() << "a function / method\n";
    // if (ctx->isLookupContext()) errs() << "a lookup context\n";
    if (ctx->isFileContext())
        errs() << "a file context\n";
    if (ctx->isTranslationUnit())
        errs() << "a translation unit\n";
    if (ctx->isRecord())
        errs() << "a record\n";
    if (ctx->isNamespace())
        errs() << "a namespace\n";
    if (ctx->isStdNamespace())
        errs() << "a std namespace\n";
    if (ctx->isInlineNamespace())
        errs() << "an inline namespace\n";
    if (ctx->isDependentContext())
        errs() << "a dependent context\n";
    if (ctx->isTransparentContext())
        errs() << "a transparent context\n";
    if (ctx->isExternCContext())
        errs() << "an extern-C context\n";
    if (ctx->isExternCXXContext())
        errs() << "an extern-C++ context\n";
    // ctx->dumpLookups();
}

static void dump(DeclContext* ctx) {
    auto cur = ctx;
    while (cur) {
        dumpSingle(cur);
        cur = cur->getParent();
        if (cur)
            errs() << "parent is...\n";
    }
}

enum class AnnotationType {
    NONE,
    BORROWED,
    STOLEN,
    SKIP,
};

enum RefKind {
    UNKNOWN,
    BORROWED,
    OWNED,
};

const char* refKindName(RefKind kind) {
    if (kind == BORROWED)
        return "BORROWED";
    if (kind == OWNED)
        return "OWNED";
    abort();
}

enum ExceptionStyle {
    CAPI,
    CXX,
};

struct RefState {
    RefKind kind;
    int num_refs;

    std::vector<std::string> log;
};

class RefStateStore;
struct RefStateHandle {
private:
    RefStateStore* store;
    int index;
    RefStateHandle(RefStateStore* store, int index) : store(store), index(index) {}

public:
    RefState& getState();

    // RefStateHandle copyTo(RefStateStore* new_store) { return RefStateHandle(new_store, index); }

    void check(const RefStateStore& store) { assert(&store == this->store); }

    RefStateHandle copyTo(RefStateStore& new_store) { return RefStateHandle(&new_store, index); }

    void assertUsable() {
        RefState& state = getState();

        assert(state.kind == RefKind::BORROWED || state.num_refs > 0);
    }

    void useRef() {
        RefState& state = getState();

        RELEASE_ASSERT(state.num_refs > 0, "");
        state.num_refs--;
    }

    void addRef() {
        RefState& state = getState();

        assertUsable();
        state.num_refs++;
    }

    friend class RefStateStore;
};
struct RefStateStore {
private:
    std::vector<RefState> refstates;

public:
    RefStateHandle addState() {
        refstates.emplace_back();
        return RefStateHandle(this, refstates.size() - 1);
    }
    RefState& getState(const RefStateHandle& handle) {
        assert(handle.store == this);
        ASSERT(0 <= handle.index && handle.index < refstates.size(), "%d, %ld", handle.index, refstates.size());
        return refstates[handle.index];
    }

    decltype(refstates)::const_iterator begin() const { return refstates.begin(); }

    decltype(refstates)::const_iterator end() const { return refstates.end(); }

    size_t size() const { return refstates.size(); }
    void clear() { refstates.clear(); }
};
RefState& RefStateHandle::getState() {
    return store->getState(*this);
}

class ExprType;
typedef shared_ptr<ExprType> Val;

class ExprType {
public:
    enum class TypeKind {
        // RefPointer,
        RefcountReference,
        Ref,
        Null,
        DeclPointer,
    } kind;

public:
    ExprType(TypeKind kind) : kind(kind) {}

    virtual ~ExprType() {}

    virtual void useAsArg(AnnotationType annotation) { assert(0 && "unimplemented"); }
    virtual void useAsArgOut(AnnotationType annotation) { assert(0 && "unimplemented"); }

    virtual void useAsReturn(AnnotationType annotation, ExceptionStyle exc_style) { assert(0 && "unimplemented"); }

    virtual Val unaryOp(UnaryOperatorKind opcode) { assert(0 && "unimplemented"); }

    virtual Val getMember(StringRef member_name) { assert(0 && "unimplemented"); }


    virtual Val merge(ExprType* rhs, RefStateStore& new_store, bool steal_hint) { assert(0 && "unimplemented"); }

    virtual Val copyTo(RefStateStore& new_store) { assert(0 && "unimplemented"); }

    virtual void checkBelongsTo(const RefStateStore& store) {}
    virtual void dump() { errs() << "Unknown kind\n"; }
};


class RefcountReference : public ExprType {
private:
    RefStateHandle handle;

public:
    RefcountReference(RefStateHandle handle) : ExprType(TypeKind::RefcountReference), handle(handle) {}

    virtual Val unaryOp(UnaryOperatorKind opcode) override {
        if (opcode == UO_PreInc || opcode == UO_PostInc) {
            handle.addRef();
            handle.getState().log.push_back("incref");
            return NULL;
        }

        if (opcode == UO_PreDec || opcode == UO_PostDec) {
            handle.useRef();
            handle.getState().log.push_back("decref");
            // RELEASE_ASSERT(state.num_refs > 0, "decreffing something we don't own any references to");
            return NULL;
        }

        errs() << UnaryOperator::getOpcodeStr(opcode) << '\n';
        assert(0 && "unhandled opcode");
    }

    virtual Val copyTo(RefStateStore& new_store) override {
        return Val(new RefcountReference(handle.copyTo(new_store)));
    }

    virtual void checkBelongsTo(const RefStateStore& store) override { handle.check(store); }
};

class NullType;

class RefType : public ExprType {
private:
    RefStateHandle handle;

public:
    RefType(RefStateHandle handle) : ExprType(TypeKind::Ref), handle(handle) {}
    virtual ~RefType() {}

    virtual Val getMember(StringRef member_name) override {
        if (member_name == "ob_refcnt") {
            return Val(new RefcountReference(handle));
        }

        return NULL;
    }

    virtual void useAsArg(AnnotationType annotation) override { assert(annotation == AnnotationType::NONE); }
    virtual void useAsArgOut(AnnotationType annotation) override {}

    virtual void useAsReturn(AnnotationType annotation, ExceptionStyle exc_style) override {

        if (annotation != AnnotationType::BORROWED) {
            handle.useRef();
            // RELEASE_ASSERT(state.num_refs > 0, "Returning an object with zero refs!");
        }
    }

    virtual Val unaryOp(UnaryOperatorKind opcode) override {
        if (opcode == UO_AddrOf)
            assert(0 && "too late to handle this");

        return NULL;
    }

    virtual Val copyTo(RefStateStore& new_store) override { return Val(new RefType(handle.copyTo(new_store))); }

    virtual Val merge(ExprType* rhs, RefStateStore& new_store, bool steal_hint) override {
        if (RefType* r_rhs = dyn_cast<RefType>(rhs)) {
            auto handle = new_store.addState();
            RefState& new_state = handle.getState();

            auto& s1 = this->handle.getState();
            auto& s2 = r_rhs->handle.getState();

            RELEASE_ASSERT(s1.kind == s2.kind, "Merging two states with different kinds (%d vs %d)", s1.kind, s2.kind);
            new_state.kind = s1.kind;

            int refs_to_steal = 0;
            if (steal_hint)
                refs_to_steal = min(s1.num_refs, s2.num_refs);

            char buf[180];
            snprintf(buf, sizeof(buf), "Inherited %d refs", refs_to_steal);
            new_state.log.push_back(buf);

            new_state.num_refs = refs_to_steal;
            s1.num_refs -= refs_to_steal;
            s2.num_refs -= refs_to_steal;

            return Val(new RefType(handle));
        }

        if (NullType* n_rhs = dyn_cast<NullType>(rhs)) {
            auto handle = new_store.addState();
            RefState& new_state = handle.getState();

            auto& s1 = this->handle.getState();

            RELEASE_ASSERT(s1.kind == BORROWED, "Merging OWNED with NULL?");
            new_state.kind = s1.kind;

            int refs_to_steal = 0;
            // if (steal_hint)
                // refs_to_steal = min(s1.num_refs, s2.num_refs);

            char buf[180];
            snprintf(buf, sizeof(buf), "Inherited %d refs", refs_to_steal);
            new_state.log.push_back(buf);

            new_state.num_refs = refs_to_steal;
            s1.num_refs -= refs_to_steal;

            return Val(new RefType(handle));
        }

        assert(0 && "unimplemented");
    }

    static bool classof(const ExprType* t) { return t->kind == TypeKind::Ref; }

    virtual void checkBelongsTo(const RefStateStore& store) override { handle.check(store); }

    virtual void dump() override {
        auto& state = handle.getState();
        errs() << "Ref to a " << refKindName(state.kind) << " with " << state.num_refs << " refs\n";
    }
};

class NullType : public ExprType {
public:
    NullType() : ExprType(TypeKind::Null) {}

    virtual void useAsArg(AnnotationType annotation) override {}
    virtual void useAsArgOut(AnnotationType annotation) override {}

    virtual void useAsReturn(AnnotationType annotation, ExceptionStyle exc_style) override {
        RELEASE_ASSERT(exc_style == CAPI, "returning NULL from a CXX function!");
    }

    virtual Val copyTo(RefStateStore& new_store) override { return Val(new NullType()); }

    static bool classof(const ExprType* t) { return t->kind == TypeKind::Null; }

    virtual Val merge(ExprType* rhs, RefStateStore& new_store, bool steal_hint) { 
        if (isa<NullType>(rhs))
            return copyTo(new_store);

        return rhs->merge(this, new_store, steal_hint);
    }
};

struct BlockState {
public:
    RefStateStore states;
    DenseMap<ValueDecl*, Val> vars;

public:
    BlockState() {}
    BlockState(const BlockState& other) { *this = other; }
    BlockState(BlockState&& other) { *this = other; }
    void operator=(const BlockState& other) {
        states.clear();
        vars.clear();

        states = other.states;
        for (auto&& p : other.vars) {
            vars[p.first] = p.second->copyTo(states);
        }
    }
    void operator=(BlockState&& other) { *this = other; }

    unique_ptr<BlockState> copy() {
        return make_unique<BlockState>(*this);
    }

    Val createBorrowed(std::string log) {
        RefStateHandle handle = states.addState();
        RefState& state = states.getState(handle);
        state.kind = BORROWED;
        state.num_refs = 0;
        state.log.push_back(std::move(log));
        return Val(new RefType(handle));
    }

    Val createOwned(std::string log) {
        RefStateHandle handle = states.addState();
        RefState& state = states.getState(handle);
        state.kind = OWNED;
        state.num_refs = 1;
        state.log.push_back(std::move(log));
        return Val(new RefType(handle));
    }

    void doAssign(ValueDecl* decl, Val val) {
        if (val) {
            val->checkBelongsTo(states);
            vars[decl] = val;
        } else
            vars.erase(decl);

        // assert(vars[decl]->num_refs == 0);
        // vars[decl]->kind = newstate->kind;
        // std::swap(vars[decl]->num_refs, newstate->num_refs);
    }

    /*
    BlockState() {}
    BlockState(const BlockState& rhs) {
        states = rhs.states;
        for (auto&& p : rhs.vars) {
            auto it = states.begin();
            auto it_rhs = rhs.states.begin();
            while (&*it_rhs != p.second) {
                ++it;
                ++it_rhs;
            }
            assert(!vars.count(p.first));
            vars[p.first] = &*it;
        }
    }
    // TODO:
    BlockState(BlockState&& rhs) = delete;
    BlockState& operator=(const BlockState& rhs) = delete;
    BlockState& operator=(BlockState&& rhs) = delete;
    */

    void checkClean(const std::string& when) {
        for (auto&& s : states) {
            if (s.num_refs) {
                errs() << when << ":\n";
                if (s.num_refs > 1)
                    errs() << "Abandoned " << s.num_refs << "refs:\n";
                else
                    errs() << "Abandoned a ref:\n";

                if (!s.log.size())
                    errs() << "No additional information :/\n";

                for (auto&& l : s.log)
                    errs() << l << '\n';
            }
            assert(s.num_refs == 0);
        }
    }

    void checkSane() {
        for (auto&& p : vars)
            p.second->checkBelongsTo(states);
    }

    void dump() {
        errs() << states.size() << " states:\n";
        for (auto&& s : states) {
            errs() << (s.kind == OWNED ? "OWNED" : "BORROWED") << ", " << s.num_refs << " refs\n";
        }
        errs() << vars.size() << " vars:\n";
        for (auto&& p : vars) {
            p.first->dump();
            p.second->dump();
        }
        errs() << '\n';
    }

    static unique_ptr<BlockState> checkSameAndMerge(BlockState& state1, BlockState& state2, const char* pre, Stmt* stmt,
                                        const char* post = NULL) {
        std::string s;
        raw_string_ostream os(s);
        os << pre;
        stmt->printPretty(os, NULL, PrintingPolicy(Context->getLangOpts()));
        if (post)
            os << post;
        return checkSameAndMerge(state1, state2, os.str());
    }

    static unique_ptr<BlockState> checkSameAndMerge(BlockState& state1, BlockState& state2, const std::string& when) {
        // state1.dump();
        // state2.dump();

        DenseSet<ValueDecl*> decls;
        for (auto&& p : state1.vars) {
            decls.insert(p.first);
            p.second->checkBelongsTo(state1.states);
        }
        for (auto&& p : state2.vars) {
            decls.insert(p.first);
            p.second->checkBelongsTo(state2.states);
        }

        unique_ptr<BlockState> rtn(new BlockState());

        for (auto&& decl : decls) {
            if (!state2.vars.count(decl)) {
                state1.vars.erase(decl);
            } else if (!state1.vars.count(decl)) {
                state2.vars.erase(decl);
            } else {
                auto s1 = state1.vars[decl];
                auto s2 = state2.vars[decl];

                // errs() << "merging: ";
                // decl->dump();

                auto new_state = s1->merge(s2.get(), rtn->states, true);
                assert(new_state);
                rtn->doAssign(decl, new_state);
                /*
                assert(s1->num_refs == s2->num_refs);

                if (s1->kind != s2->kind) {
                    assert(s1->kind != UNKNOWN);
                    assert(s2->kind != UNKNOWN);

                    s1->kind = OWNED;
                    s2->kind = OWNED;
                }
                */
            }
        }

        state1.checkClean("With first part of: " + when);
        state2.checkClean("With second part of: " + when);

        return rtn;
    }
};

class DeclPointerType : public ExprType {
private:
    ValueDecl* decl;
    BlockState& state;

public:
    DeclPointerType(ValueDecl* decl, BlockState& state) : ExprType(TypeKind::DeclPointer), decl(decl), state(state) {}

    virtual void useAsArg(AnnotationType annotation) { assert(annotation != AnnotationType::STOLEN); }

    virtual void useAsArgOut(AnnotationType annotation) {
        std::string s;
        raw_string_ostream os(s);
        os << "Assigned to '" << decl->getName() << "' via "
           << (annotation == AnnotationType::BORROWED ? "borrowed" : "owned") << "-set of out-parameter";

        if (annotation == AnnotationType::BORROWED)
            state.doAssign(decl, state.createBorrowed(os.str()));
        else
            state.doAssign(decl, state.createOwned(os.str()));
    }
};

AnnotationType getAnnotationType(SourceLocation loc) {
    // see clang::DiagnosticRenderer::emitMacroExpansions for more info:
    if (!loc.isMacroID())
        return AnnotationType::NONE;
    StringRef MacroName = Lexer::getImmediateMacroName(loc, *SM, CI->getLangOpts());

    auto inner_loc = SM->getImmediateMacroCallerLoc(loc);
    auto inner_ann = getAnnotationType(SM->getImmediateMacroCallerLoc(loc));

    if (MacroName == "BORROWED") {
        // I'm not really sure why it can sometimes see nested annotations
        assert(inner_ann == AnnotationType::NONE || inner_ann == AnnotationType::BORROWED);
        return AnnotationType::BORROWED;
    }
    if (MacroName == "STOLEN") {
        assert(inner_ann == AnnotationType::NONE);
        return AnnotationType::STOLEN;
    }
    if (MacroName == "NOREFCHECK") {
        assert(inner_ann == AnnotationType::NONE);
        return AnnotationType::SKIP;
    }

    return inner_ann;
}

AnnotationType getReturnAnnotationType(FunctionDecl* fdecl) {
    // fdecl->dump();
    return getAnnotationType(fdecl->getReturnTypeSourceRange().getBegin());
}

std::vector<AnnotationType> getParamAnnotations(FunctionDecl* fdecl) {
    std::vector<AnnotationType> rtn;
    for (auto param : fdecl->params()) {
        // rtn.push_back(getAnnotationType(param.getLocStart()));
        rtn.push_back(getAnnotationType(param->getTypeSpecStartLoc()));
    }
    return rtn;
}

ExceptionStyle determineExcStyle(FunctionDecl* fdecl) {
    // TODO: look at name

    auto ft = cast<FunctionProtoType>(fdecl->getType());
    bool can_throw = ft && !isUnresolvedExceptionSpec(ft->getExceptionSpecType()) && !ft->isNothrow(*Context, false);
    return can_throw ? CXX : CAPI;
}

class FunctionSemantics {
public:
    virtual ~FunctionSemantics() {}

    virtual AnnotationType getReturnAnnotation() = 0;
    virtual AnnotationType getParamAnnotation(int param_idx) = 0;
    virtual bool canReturnNull() = 0;
    virtual bool canThrow() = 0;
};

class DefaultFunctionSemantics : public FunctionSemantics {
private:
    ExceptionStyle exc_style;

public:
    DefaultFunctionSemantics(ExceptionStyle exc_style) : exc_style(exc_style) {}

    AnnotationType getReturnAnnotation() override { return AnnotationType::NONE; }

    AnnotationType getParamAnnotation(int param_idx) override { return AnnotationType::NONE; }

    bool canReturnNull() override { return exc_style != CXX; }

    bool canThrow() override { return exc_style != CAPI; }
};

class PyArgFunctionSemantics : public FunctionSemantics {
private:
    FunctionDecl* decl;

public:
    PyArgFunctionSemantics(FunctionDecl* decl) : decl(decl) {}

    AnnotationType getReturnAnnotation() override { return getReturnAnnotationType(decl); }

    AnnotationType getParamAnnotation(int param_idx) override {
        if (param_idx < decl->getNumParams())
            return getAnnotationType(decl->getParamDecl(param_idx)->getTypeSpecStartLoc());
        RELEASE_ASSERT(decl->isVariadic(), "");
        return AnnotationType::BORROWED;
    }

    bool canThrow() override {
        auto ft = cast<FunctionProtoType>(decl->getType());
        assert(ft && !isUnresolvedExceptionSpec(ft->getExceptionSpecType()) && ft->isNothrow(*Context, false));
        return false;
    }

    bool canReturnNull() override { return false; }
};

class DeclFunctionSemantics : public FunctionSemantics {
private:
    FunctionDecl* decl;

public:
    DeclFunctionSemantics(FunctionDecl* decl) : decl(decl) {}

    AnnotationType getReturnAnnotation() override { return getReturnAnnotationType(decl); }

    AnnotationType getParamAnnotation(int param_idx) override {
        if (param_idx < decl->getNumParams())
            return getAnnotationType(decl->getParamDecl(param_idx)->getTypeSpecStartLoc());
        RELEASE_ASSERT(decl->isVariadic(), "");
        return AnnotationType::NONE;
    }

    bool canThrow() override {
        auto ft = cast<FunctionProtoType>(decl->getType());
        if (ft && !isUnresolvedExceptionSpec(ft->getExceptionSpecType()) && ft->isNothrow(*Context, false))
            return false;
        return true;
    }

    bool canReturnNull() override { assert(0 && "unimplemented"); }
};

const Type* stripSugar(const Type* t) {
    while (1) {
        // errs() << "is refcounted?   ";
        // t->dump();
        if (auto pt = dyn_cast<ParenType>(t)) {
            t = pt->getInnerType().getTypePtr();
            continue;
        }

        if (auto pt = dyn_cast<TypedefType>(t)) {
            t = pt->desugar().getTypePtr();
            continue;
        }

        if (auto pt = dyn_cast<ElaboratedType>(t)) {
            t = pt->desugar().getTypePtr();
            continue;
        }

        break;
    };
    return t;
}

bool isPyObjectBase(const Type* t) {
    while (t->isPointerType())
        t = t->getPointeeType().getTypePtr();

    if (auto tdt = dyn_cast<TypedefType>(t))
        if (tdt->getDecl()->getName() == "PyObject")
            return true;

    return false;
}

unique_ptr<FunctionSemantics> functionSemanticsFromCallee(Expr* callee) {
    while (true) {
        if (auto castexpr = dyn_cast<CastExpr>(callee)) {
            callee = castexpr->getSubExpr();
            continue;
        }

        if (auto unop = dyn_cast<UnaryOperator>(callee)) {
            // TODO: I don't know if this is always correct, but this is to handle explicitly dereferencing function
            // pointers:
            if (unop->getOpcode() == UO_Deref) {
                callee = unop->getSubExpr();
                continue;
            }
        }

        if (auto pe = dyn_cast<ParenExpr>(callee)) {
            callee = pe->getSubExpr();
            continue;
        }

        break;
    }

    bool is_member;
    ValueDecl* callee_decl;

    if (auto me = dyn_cast<MemberExpr>(callee)) {
        callee_decl = me->getMemberDecl();
        is_member = true;
    } else if (auto ref = dyn_cast<DeclRefExpr>(callee)) {
        callee_decl = ref->getDecl();
        is_member = false;
    } else {
        callee->dump();
        RELEASE_ASSERT(0, "");
        // return unique_ptr<FunctionSemantics>(new DefaultFunctionSemantics());
    }

    auto callee_fdecl = dyn_cast<FunctionDecl>(callee_decl);
    if (!callee_fdecl) {
        if (auto fielddecl = dyn_cast<FieldDecl>(callee_decl)) {
            auto name = fielddecl->getName();

            if (name.startswith("tp_"))
                return unique_ptr<FunctionSemantics>(new DefaultFunctionSemantics(CAPI));

            callee_decl->dump();
            RELEASE_ASSERT(0, "couldn't determine exception style of function pointer");
        }

        auto t = stripSugar(callee_decl->getType().getTypePtr());
        if (auto pt = dyn_cast<PointerType>(t)) {
            // TODO: again, not sure if this is ok, but it's here to handle explicitly dereferencing function pointers:
            t = stripSugar(pt->getPointeeType().getTypePtr());
        }

        auto ft = dyn_cast<FunctionProtoType>(t);
        if (ft && ft->getNumParams() > 0) {
            //&& ft->getParamType(0).getTypePtr()->getName()
            auto p0t = ft->getParamType(0).getTypePtr();
            if (isPyObjectBase(p0t))
                return unique_ptr<FunctionSemantics>(new DefaultFunctionSemantics(CAPI));
        }

        callee_decl->dump();
        callee_decl->getType()->dump();
        RELEASE_ASSERT(0, "");
        // return unique_ptr<FunctionSemantics>(new DefaultFunctionSemantics());
    }

    assert(!is_member && "unimplemented");

    if (callee_fdecl->getName() == "PyArg_Parse" || callee_fdecl->getName() == "PyArg_ParseTuple"
        || callee_fdecl->getName() == "PyArg_ParseTupleAndKeywords" || callee_fdecl->getName() == "PyArg_ParseSingle"
        || callee_fdecl->getName() == "PyArg_UnpackTuple") {
        return unique_ptr<FunctionSemantics>(new PyArgFunctionSemantics(callee_fdecl));
    }

    return unique_ptr<FunctionSemantics>(new DeclFunctionSemantics(callee_fdecl));
}

class FunctionRefchecker {
private:
    bool done = false;
    AnnotationType return_ann;
    ExceptionStyle exc_style;


    bool isRefcountedName(StringRef name) {
        return name.startswith("Box") || ((name.startswith("Py") || name.startswith("_Py")) && name.endswith("Object"));
    }

    bool isRefcountedType(const QualType& t) {
        if (!t->isPointerType())
            return false;
        // errs() << '\n';

        auto pointed_to = t->getPointeeType();
        while (1) {
            // errs() << "is refcounted?   ";
            // pointed_to->dump();
            if (auto pt = dyn_cast<ParenType>(pointed_to)) {
                pointed_to = pt->getInnerType();
                continue;
            }

            if (auto pt = dyn_cast<TypedefType>(pointed_to)) {
                if (isRefcountedName(pt->getDecl()->getName()))
                    return true;
                pointed_to = pt->desugar();
                continue;
            }

            if (auto pt = dyn_cast<ElaboratedType>(pointed_to)) {
                pointed_to = pt->desugar();
                continue;
            }

            break;
        };

        // errs() << "final:   ";
        // pointed_to->dump();

        if (isa<BuiltinType>(pointed_to) || isa<FunctionType>(pointed_to))
            return false;

        if (isa<TemplateTypeParmType>(pointed_to)) {
            // TODO Hmm not sure what to do about templates
            return false;
        }

        if (pointed_to->isPointerType())
            return false;

        auto cxx_record_decl = pointed_to->getAsCXXRecordDecl();
        if (!cxx_record_decl)
            t->dump();
        assert(cxx_record_decl);

        auto name = cxx_record_decl->getName();
        return isRefcountedName(name);
    }

    Val handle(Expr* expr, unique_ptr<BlockState>& state) {
        state->checkSane();

        Val rtn = _handle(expr, state);
        if (rtn)
            rtn->checkBelongsTo(state->states);

        state->checkSane();

        return rtn;
    }

    Val _handle(Expr* expr, unique_ptr<BlockState>& state) {
        // TODO when does ob_refcnt decay to a value?

        if (isa<StringLiteral>(expr) || isa<IntegerLiteral>(expr) || isa<CXXBoolLiteralExpr>(expr)) {
            return NULL;
        }

        if (isa<UnresolvedLookupExpr>(expr) || isa<UnresolvedMemberExpr>(expr) || isa<CXXUnresolvedConstructExpr>(expr)
            || isa<CXXDependentScopeMemberExpr>(expr) || isa<DependentScopeDeclRefExpr>(expr)
            || isa<CXXConstructExpr>(expr) || isa<PredefinedExpr>(expr) || isa<PackExpansionExpr>(expr)) {
            // Not really sure about this:
            assert(!isRefcountedType(expr->getType()));

            // TODO is this ok?
            return NULL;
        }

        if (isa<CXXDefaultArgExpr>(expr)) {
            // Not really sure about this:
            assert(!isRefcountedType(expr->getType()));

            // TODO is this ok?
            return NULL;
        }

        if (isa<GNUNullExpr>(expr)) {
            if (isRefcountedType(expr->getType()))
                return Val(new NullType());

            // TODO is this ok?
            return NULL;
        }

        if (auto exprwc = dyn_cast<ExprWithCleanups>(expr)) {
            // TODO: probably will need to be checking things here
            errs() << "exprwithcleanup; " << exprwc->getNumObjects() << " cleanup objects\n";
            for (auto cleanup_object : exprwc->getObjects()) {
                errs() << "cleanup object:\n";
                for (auto param : cleanup_object->params()) {
                    errs() << "param: ";
                    param->dump();
                }
                for (auto capture : cleanup_object->captures()) {
                    errs() << "capture: ";
                    capture.getVariable()->dump();
                    errs() << "capture expr : ";
                    capture.getCopyExpr()->dump();
                }
            }
            return handle(exprwc->getSubExpr(), state);
        }

        if (auto mattmp = dyn_cast<MaterializeTemporaryExpr>(expr)) {
            // not sure about this
            return handle(mattmp->GetTemporaryExpr(), state);
        }

        if (auto bindtmp = dyn_cast<CXXBindTemporaryExpr>(expr)) {
            // not sure about this
            return handle(bindtmp->getSubExpr(), state);
        }



        if (auto unaryop = dyn_cast<UnaryOperator>(expr)) {
            // if (isRefcountedType(unaryop->getType())) {
            // if (unaryop->getOpcode() == UO_AddrOf)
            // return state->createBorrowed();
            //}

            if (unaryop->getOpcode() == UO_AddrOf) {
                auto refexpr = cast<DeclRefExpr>(unaryop->getSubExpr());
                auto decl = refexpr->getDecl();
                return Val(new DeclPointerType(decl, *state.get()));
            }

            auto val = handle(unaryop->getSubExpr(), state);

            if (val)
                return val->unaryOp(unaryop->getOpcode());

            ASSERT(!isRefcountedType(unaryop->getType()), "???");
            return NULL;
        }

        if (auto parenexpr = dyn_cast<ParenExpr>(expr)) {
            return handle(parenexpr->getSubExpr(), state);
        }

        if (auto binaryop = dyn_cast<BinaryOperator>(expr)) {
            if (binaryop->isAssignmentOp()) {
                auto rhs = handle(binaryop->getRHS(), state);
                if (rhs) {
                    if (auto refexpr = dyn_cast<DeclRefExpr>(binaryop->getLHS())) {
                        auto decl = refexpr->getDecl();
                        state->doAssign(decl, rhs);
                        return rhs;
                    }

                    binaryop->dump();
                    binaryop->dumpPretty(*Context);
                    assert(0);
                }
                auto lhs = handle(binaryop->getLHS(), state);
                assert(!lhs);
                return NULL;
            }

            auto lhs = handle(binaryop->getLHS(), state);
            auto rhs = handle(binaryop->getRHS(), state);
            ASSERT(!isRefcountedType(binaryop->getType()), "implement me");

            // TODO is this ok?
            return NULL;
        }

        if (auto castexpr = dyn_cast<CastExpr>(expr)) {
            // castexpr->getType()->dump();
            // castexpr->getSubExpr()->getType()->dump();

            auto cast_kind = castexpr->getCastKind();
            if (cast_kind == CK_NullToPointer) {
                auto r = handle(castexpr->getSubExpr(), state);
                assert(!r);
                return Val(new NullType());
            }

            if (cast_kind == CK_FunctionToPointerDecay) {
                auto r = handle(castexpr->getSubExpr(), state);
                assert(!r);
                return NULL;
            }
            /*
            if (castexpr->getCastKind() ==

            auto val = handle(castexpr->getSubExpr(), state);
            if (val)
                expr->dump();
            assert(!val);
            return NULL;
            */

            // castexpr->dump();
            // RELEASE_ASSERT(0, "%d", cast_kind);

            assert(!(isRefcountedType(castexpr->getType()) && !isRefcountedType(castexpr->getSubExpr()->getType())));
            return handle(castexpr->getSubExpr(), state);
        }

        if (auto membexpr = dyn_cast<MemberExpr>(expr)) {
            auto val = handle(membexpr->getBase(), state);

            // TODO: is this right?
            if (isRefcountedType(membexpr->getType())) {
                char buf[160];
                snprintf(buf, sizeof(buf), "Created as a borrowed reference to '%s'",
                         membexpr->getMemberNameInfo().getName().getAsIdentifierInfo()->getName().data());
                return state->createBorrowed(buf);
            }

            if (val)
                return val->getMember(membexpr->getMemberNameInfo().getName().getAsIdentifierInfo()->getName());

            return NULL;
        }

        if (auto thisexpr = dyn_cast<CXXThisExpr>(expr)) {
            // TODO is this ok?

            if (!isRefcountedType(thisexpr->getType()))
                return NULL;
            assert(0 && "should map all `this` exprs to the same refstate");
            return state->createBorrowed("Created as borrowed reference to 'this'");
        }

        if (auto refexpr = dyn_cast<DeclRefExpr>(expr)) {
            if (!isRefcountedType(refexpr->getType())) {
                return NULL;
            }

            auto decl = refexpr->getDecl();
            if (state->vars.count(decl)) {
                state->vars[decl]->checkBelongsTo(state->states);
                return state->vars[decl];
            }

            auto context = decl->getDeclContext();
            while (context->getDeclKind() == Decl::LinkageSpec)
                context = context->getParent();

            // A global variable:
            if (context->getDeclKind() == Decl::Namespace || context->getDeclKind() == Decl::TranslationUnit) {
                state->doAssign(decl, state->createBorrowed("Borrowed ref to global variable"));
                return state->vars[decl];
            }

            errs() << "\n\n";
            errs() << context->getDeclKindName() << '\n';
            expr->dump();
            dump(decl->getDeclContext());
            errs() << state->vars.size() << " known decls:\n";
            for (auto&& p : state->vars) {
                p.first->dump();
            }
            ASSERT(0, "Don't know how to handle");
        }

        if (auto callexpr = dyn_cast<CallExpr>(expr)) {
            auto callee = callexpr->getCallee();

            handle(callee, state);

            /*
            auto ft_ptr = callee->getType();
            const FunctionProtoType* ft;

            if (isa<BuiltinType>(ft_ptr)) {
                ft = NULL;
            } else if (isa<TemplateSpecializationType>(ft_ptr)) {
                // Not really sure about this:
                ft = NULL;
            } else {
                //ft_ptr->dump();
                assert(ft_ptr->isPointerType());
                auto pointed_to = ft_ptr->getPointeeType();
                while (auto pt = dyn_cast<ParenType>(pointed_to))
                    pointed_to = pt->getInnerType();
                assert(isa<FunctionProtoType>(pointed_to));
                ft = cast<FunctionProtoType>(pointed_to);
            }
            */

            auto semantics = functionSemanticsFromCallee(callee);

            std::vector<Val> args;
            for (auto arg : callexpr->arguments()) {
                args.push_back(handle(arg, state));
            }

            for (int i = 0; i < args.size(); i++) {
                if (!args[i])
                    continue;
                args[i]->useAsArg(semantics->getParamAnnotation(i));
            }

            if (semantics->canThrow()) {
                std::string s;
                raw_string_ostream os(s);
                os << "If this throws: '";
                expr->printPretty(os, NULL, PrintingPolicy(Context->getLangOpts()));
                os << "'";
                state->checkClean(os.str());
            }

            for (int i = 0; i < args.size(); i++) {
                if (!args[i])
                    continue;
                args[i]->useAsArgOut(semantics->getParamAnnotation(i));
            }

            if (isRefcountedType(callexpr->getType())) {
                std::string s;
                raw_string_ostream os(s);

                if (semantics->getReturnAnnotation() == AnnotationType::BORROWED)
                    os << "(Borrowed) result";
                else
                    os << "Result";
                os << " of function call: ";
                expr->printPretty(os, NULL, PrintingPolicy(Context->getLangOpts()));

                if (semantics->getReturnAnnotation() == AnnotationType::BORROWED)
                    return state->createBorrowed(os.str());
                else
                    return state->createOwned(os.str());
            }

            // TODO: not sure we can ignore all of these
            // TODO: look for incref/etc
            return NULL;
        }

        if (auto newexpr = dyn_cast<CXXNewExpr>(expr)) {
            assert(0 && "need to assert no stolen anns");
            for (auto plc :
                 iterator_range<CXXNewExpr::arg_iterator>(newexpr->placement_arg_begin(), newexpr->placement_arg_end()))
                handle(plc, state);

            if (newexpr->hasInitializer())
                handle(newexpr->getInitializer(), state);

            if (isRefcountedType(newexpr->getType()))
                return state->createOwned("As result of 'new' expression");
            return NULL;
        }

        if (auto condop = dyn_cast<ConditionalOperator>(expr)) {
            handle(condop->getCond(), state);

            auto false_state = state->copy();
            auto true_state = state->copy();
            Val s1 = handle(condop->getTrueExpr(), true_state);
            Val s2 = handle(condop->getFalseExpr(), false_state);

            assert((s1 == NULL) == (s2 == NULL));

            BlockState dummy_state;
            Val merged_val;
            if (s1)
                merged_val = s1->merge(s2.get(), dummy_state.states, false);

            state = BlockState::checkSameAndMerge(*true_state, *false_state, "Problem joining after ternary expression: ", expr);

            if (merged_val)
                return merged_val->copyTo(state->states);

            return NULL;
        }

        expr->dump();
        RELEASE_ASSERT(0, "unhandled expr type: %s\n", expr->getStmtClassName());
    }

    void handle(Stmt* stmt, unique_ptr<BlockState>& state) {
        assert(state);
        state->checkSane();

        _handle(stmt, state);

        if (state)
            state->checkSane();
    }

    void _handle(Stmt* stmt, unique_ptr<BlockState>& state) {
        assert(stmt);

        if (done)
            return;

        if (auto expr = dyn_cast<Expr>(stmt)) {
            handle(expr, state);
            return;
        }

        if (auto cstmt = dyn_cast<CompoundStmt>(stmt)) {
            for (auto sub_stmt : cstmt->body()) {
                handle(sub_stmt, state);
                if (!state)
                    break;
            }
            return;
        }

        if (auto dostmt = dyn_cast<DoStmt>(stmt)) {
            Expr* cond = dostmt->getCond();

            bool while_0 = false;
            auto cond_as_bool = dyn_cast<CXXBoolLiteralExpr>(cond);
            if (cond_as_bool && cond_as_bool->getValue() == false)
                while_0 = true;

            Expr* casted_cond = cond;
            do {
                if (auto castexpr = dyn_cast<CastExpr>(casted_cond)) {
                    casted_cond = castexpr->getSubExpr();
                    continue;
                }
            } while (0);

            auto cond_as_int = dyn_cast<IntegerLiteral>(casted_cond);
            if (cond_as_int && cond_as_int->getValue() == 0)
                while_0 = true;

            RELEASE_ASSERT(while_0, "Only support `do {} while(false);` statements for now");
            handle(dostmt->getBody(), state);
            return;
        }

        // Not really sure about these:
        if (auto forstmt = dyn_cast<ForStmt>(stmt)) {
            handle(forstmt->getInit(), state);
            handle(forstmt->getCond(), state);

            if (forstmt->getConditionVariable())
                assert(!isRefcountedType(forstmt->getConditionVariable()->getType()));

            auto old_state = state->copy();
            auto loop_state = state->copy();
            handle(forstmt->getBody(), loop_state);
            handle(forstmt->getInc(), loop_state);
            // Is this right?
            handle(forstmt->getCond(), loop_state);
            state = BlockState::checkSameAndMerge(*old_state, *loop_state, "Problem with loop body: ", stmt);
            return;
        }

        if (auto forstmt = dyn_cast<CXXForRangeStmt>(stmt)) {
            // Not really sure about these:
            handle(forstmt->getRangeInit(), state);
            handle(forstmt->getCond(), state);
            handle(forstmt->getInc(), state);

            auto old_state = state->copy();
            auto loop_state = state->copy();
            handle(forstmt->getBody(), loop_state);
            state = BlockState::checkSameAndMerge(*loop_state, *old_state, "Problem with loop body:", stmt);
            return;
        }

        if (auto whilestmt = dyn_cast<WhileStmt>(stmt)) {
            handle(whilestmt->getCond(), state);

            if (whilestmt->getConditionVariable())
                assert(!isRefcountedType(whilestmt->getConditionVariable()->getType()));

            auto old_state = state->copy();
            auto loop_state = state->copy();
            handle(whilestmt->getBody(), loop_state);
            state = BlockState::checkSameAndMerge(*loop_state, *old_state, "Problem with loop body:", stmt);
            return;
        }

        if (auto ifstmt = dyn_cast<IfStmt>(stmt)) {
            handle(ifstmt->getCond(), state);

            auto if_state = state->copy();
            auto else_state = state->copy();
            if (ifstmt->getThen())
                handle(ifstmt->getThen(), if_state);
            if (ifstmt->getElse())
                handle(ifstmt->getElse(), else_state);

            if (!if_state)
                std::swap(state, else_state);
            else if (!else_state)
                std::swap(state, if_state);
            else
                state = BlockState::checkSameAndMerge(*if_state, *else_state, "Problem with if statement: ", stmt);
            return;
        }

        if (auto declstmt = dyn_cast<DeclStmt>(stmt)) {
            for (auto decl : declstmt->decls()) {
                if (!isa<VarDecl>(decl))
                    errs() << decl->getDeclKindName() << '\n';
                auto vardecl = cast<VarDecl>(decl);
                assert(vardecl);

                assert(!state->vars.count(vardecl));

                bool is_refcounted = isRefcountedType(vardecl->getType());

                if (vardecl->hasInit()) {
                    Val assigning = handle(vardecl->getInit(), state);
                    state->doAssign(vardecl, assigning);
                }
            }
            return;
        }

        if (auto rtnstmt = dyn_cast<ReturnStmt>(stmt)) {
            auto rstate = handle(rtnstmt->getRetValue(), state);
            if (rstate) {
                rstate->useAsReturn(return_ann, exc_style);
            } else {
                assert(!isRefcountedType(rtnstmt->getRetValue()->getType()));
            }
            state.reset(NULL);
            return;
        }

        if (auto asmstmt = dyn_cast<AsmStmt>(stmt)) {
            for (auto input : asmstmt->inputs())
                handle(input, state);
            for (auto input : asmstmt->outputs())
                handle(input, state);
            return;
        }

        if (auto nullstmt = dyn_cast<NullStmt>(stmt)) {
            AnnotationType ann = getAnnotationType(nullstmt->getSemiLoc());
            if (ann == AnnotationType::SKIP) {
                done = true;
                return;
            }
            return;
        }

        stmt->dump();
        RELEASE_ASSERT(0, "unhandled statement type: %s\n", stmt->getStmtClassName());
    }

    void _checkFunction(FunctionDecl* func) {
        /*
        errs() << func->hasTrivialBody() << '\n';
        errs() << func->isDefined() << '\n';
        errs() << func->hasSkippedBody() << '\n';
        errs() << (func == func->getCanonicalDecl()) << '\n';
        errs() << func->isOutOfLine() << '\n';
        errs() << func->getBody() << '\n';
        errs() << func->isThisDeclarationADefinition() << '\n';
        errs() << func->doesThisDeclarationHaveABody() << '\n';
        */
        errs() << "printing:\n";
        func->print(errs());
        errs() << "dumping:\n";
        func->dump(errs());

        return_ann = getReturnAnnotationType(func);
        exc_style = determineExcStyle(func);

        auto param_anns = getParamAnnotations(func);
        for (auto ann : param_anns) {
            assert(ann != AnnotationType::STOLEN && "unsupported");
        }

        unique_ptr<BlockState> state(new BlockState());
        for (auto param : func->params()) {
            if (isRefcountedType(param->getType())) {
                auto& rstate = state->vars[param];
                assert(!rstate);
                rstate = state->createBorrowed("As function parameter");
            }
        }
        errs() << "Starting.  state has " << state->vars.size() << " vars\n";
        handle(func->getBody(), state);
        if (state)
            state->checkClean("At end of function");
    }

    explicit FunctionRefchecker() {}

public:
    static void checkFunction(FunctionDecl* func) { FunctionRefchecker()._checkFunction(func); }
};

class RefcheckingVisitor : public RecursiveASTVisitor<RefcheckingVisitor> {
private:
public:
    explicit RefcheckingVisitor() {}

    virtual ~RefcheckingVisitor() {}

    StringRef getFilename(SourceLocation Loc) {
        // From ASTDumper::dumpLocation:
        SourceLocation SpellingLoc = SM->getSpellingLoc(Loc);
        PresumedLoc PLoc = SM->getPresumedLoc(SpellingLoc);
        return PLoc.getFilename();
    }

    virtual bool VisitFunctionDecl(FunctionDecl* func) {
        if (!func->hasBody())
            return true /* keep going */;
        if (!func->isThisDeclarationADefinition())
            return true /* keep going */;

        // auto filename = Context->getSourceManager().getFilename(func->getSourceRange().getBegin());
        auto filename = getFilename(func->getSourceRange().getBegin());

        // Filter out functions defined in libraries:
        if (filename.find("include/c++") != StringRef::npos)
            return true;
        if (filename.find("include/x86_64-linux-gnu") != StringRef::npos)
            return true;
        if (filename.find("include/llvm") != StringRef::npos)
            return true;
        if (filename.find("lib/clang") != StringRef::npos)
            return true;

        if (filename.endswith(".h"))
            return true;

        // errs() << "filename:" << filename << '\n';

        // if (func->getNameInfo().getAsString() != "wrap_setattr2")
        // return true;

        FunctionRefchecker::checkFunction(func);

        return true /* keep going */;
    }
};

class RefcheckingASTConsumer : public ASTConsumer {
private:
    RefcheckingVisitor visitor;

public:
    explicit RefcheckingASTConsumer() {}

    virtual void HandleTranslationUnit(ASTContext& Context) {
        visitor.TraverseDecl(Context.getTranslationUnitDecl());
        // dumper()->TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class RefcheckingFrontendAction : public ASTFrontendAction {
public:
    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& CI, StringRef fname) {
        ::CI = &CI;
        ::SM = &CI.getSourceManager();
        ::Context = &CI.getASTContext();
        return std::unique_ptr<ASTConsumer>(new RefcheckingASTConsumer());
    }
};

// A way to inject refchecker-only compilation flags.
// Not currently used, but uncomment the line in getCompileCommands() to define the `REFCHECKER` directive.
class MyCompilationDatabase : public CompilationDatabase {
private:
    CompilationDatabase& base;

public:
    MyCompilationDatabase(CompilationDatabase& base) : base(base) {}

    virtual vector<CompileCommand> getCompileCommands(StringRef FilePath) const {
        auto rtn = base.getCompileCommands(FilePath);
        assert(rtn.size() == 1);
        // rtn[0].CommandLine.push_back("-DREFCHECKER");
        return rtn;
    }

    virtual vector<std::string> getAllFiles() const { assert(0); }

    virtual vector<CompileCommand> getAllCompileCommands() const { assert(0); }
};

int main(int argc, const char** argv) {
    CommonOptionsParser OptionsParser(argc, argv, RefcheckingToolCategory);
    ClangTool Tool(MyCompilationDatabase(OptionsParser.getCompilations()), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<RefcheckingFrontendAction>().get());
}
