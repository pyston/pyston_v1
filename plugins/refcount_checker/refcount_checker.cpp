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
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include "core/common.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace std;

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
    if (ctx->isClosure()) errs() << "a closure\n";
    if (ctx->isFunctionOrMethod()) errs() << "a function / method\n";
    //if (ctx->isLookupContext()) errs() << "a lookup context\n";
    if (ctx->isFileContext()) errs() << "a file context\n";
    if (ctx->isTranslationUnit()) errs() << "a translation unit\n";
    if (ctx->isRecord()) errs() << "a record\n";
    if (ctx->isNamespace()) errs() << "a namespace\n";
    if (ctx->isStdNamespace()) errs() << "a std namespace\n";
    if (ctx->isInlineNamespace()) errs() << "an inline namespace\n";
    if (ctx->isDependentContext()) errs() << "a dependent context\n";
    if (ctx->isTransparentContext()) errs() << "a transparent context\n";
    if (ctx->isExternCContext()) errs() << "an extern-C context\n";
    if (ctx->isExternCXXContext()) errs() << "an extern-C++ context\n";
    //ctx->dumpLookups();
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

class RefcheckingVisitor : public RecursiveASTVisitor<RefcheckingVisitor> {
private:
    ASTContext* Context;

    enum RefType {
        UNKNOWN,
        BORROWED,
        OWNED,
    };
    struct RefState {
        RefType type;
        int num_refs;
    };
    struct BlockState {
        deque<RefState> states;
        DenseMap<ValueDecl*, RefState*> vars;

        RefState* addState() {
            states.emplace_back();
            return &states.back();
        }

        RefState* createBorrowed() {
            auto rtn = addState();
            rtn->type = BORROWED;
            rtn->num_refs = 0;
            return rtn;
        }

        RefState* createOwned() {
            auto rtn = addState();
            rtn->type = OWNED;
            rtn->num_refs = 1;
            return rtn;
        }

        void doAssign(VarDecl* decl, RefState* newstate) {
            assert(newstate);
            assert(vars.count(decl));
            assert(vars[decl]->num_refs == 0);

            vars[decl]->type = newstate->type;
            std::swap(vars[decl]->num_refs, newstate->num_refs);
        }

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
    };

    void checkSameAndMerge(BlockState& state1, BlockState& state2) {
        DenseSet<ValueDecl*> decls;
        for (auto&& p : state1.vars)
            decls.insert(p.first);
        for (auto&& p : state2.vars)
            decls.insert(p.first);

        for (auto&& decl : decls) {
            if (!state2.vars.count(decl)) {
                assert(state1.vars[decl]->num_refs == 0);
                state1.vars.erase(decl);
            } else if (!state1.vars.count(decl)) {
                assert(state2.vars[decl]->num_refs == 0);
                state2.vars.erase(decl);
            } else {
                auto s1 = state1.vars[decl];
                auto s2 = state2.vars[decl];

                assert(s1->num_refs == s2->num_refs);

                if (s1->type != s2->type) {
                    assert(s1->type != UNKNOWN);
                    assert(s2->type != UNKNOWN);

                    s1->type = OWNED;
                    s2->type = OWNED;
                }
            }
        }

        for (auto&& bstate : { state1, state2 }) {
            for (auto&& state : state1.states) {
                if (state.num_refs == 0)
                    continue;

                bool found = false;
                for (auto&& p : state1.vars) {
                    if (p.second == &state) {
                        found = true;
                        break;
                    }
                }

                assert(found);
            }
        }
    }

    void checkClean(const BlockState& state) {
        for (auto&& s : state.states) {
            assert(s.num_refs == 0);
        }
    }

    bool isRefcountedType(const QualType& t) {
        if (!t->isPointerType())
            return false;

        auto pointed_to = t->getPointeeType();
        while (auto pt = dyn_cast<ParenType>(pointed_to))
            pointed_to = pt->getInnerType();

        if (isa<BuiltinType>(pointed_to) || isa<FunctionType>(pointed_to))
            return false;

        auto cxx_record_decl = pointed_to->getAsCXXRecordDecl();
        if (!cxx_record_decl)
            t->dump();
        assert(cxx_record_decl);

        return cxx_record_decl->getName().startswith("Box");
    }

    RefState* handle(Expr* expr, BlockState& state) {
        if (isa<StringLiteral>(expr) || isa<IntegerLiteral>(expr)) {
            return NULL;
        }

        if (isa<UnresolvedLookupExpr>(expr) || isa<CXXUnresolvedConstructExpr>(expr)
            || isa<CXXDependentScopeMemberExpr>(expr) || isa<DependentScopeDeclRefExpr>(expr)
            || isa<CXXConstructExpr>(expr) || isa<PredefinedExpr>(expr)) {
            // Not really sure about this:
            assert(!isRefcountedType(expr->getType()));
            return NULL;
        }

        if (auto unaryop = dyn_cast<UnaryOperator>(expr)) {
            handle(unaryop->getSubExpr(), state);
            ASSERT(!isRefcountedType(unaryop->getType()), "implement me");
            return NULL;
        }

        if (auto parenexpr = dyn_cast<ParenExpr>(expr)) {
            return handle(parenexpr->getSubExpr(), state);
        }

        if (auto binaryop = dyn_cast<BinaryOperator>(expr)) {
            handle(binaryop->getLHS(), state);
            handle(binaryop->getRHS(), state);
            ASSERT(!isRefcountedType(binaryop->getType()), "implement me");
            return NULL;
        }

        if (auto castexpr = dyn_cast<CastExpr>(expr)) {
            assert(!(isRefcountedType(castexpr->getType()) && !isRefcountedType(castexpr->getSubExpr()->getType())));
            return handle(castexpr->getSubExpr(), state);
        }

        if (auto membexpr = dyn_cast<MemberExpr>(expr)) {
            handle(membexpr->getBase(), state);
            if (!isRefcountedType(membexpr->getType()))
                return NULL;
            return state.createBorrowed();
        }

        if (auto thisexpr = dyn_cast<CXXThisExpr>(expr)) {
            if (!isRefcountedType(thisexpr->getType()))
                return NULL;
            return state.createBorrowed();
        }

        if (auto refexpr = dyn_cast<DeclRefExpr>(expr)) {
            if (!isRefcountedType(refexpr->getType())) {
                return NULL;
            }

            auto decl = refexpr->getDecl();
            if (state.vars.count(decl))
                return state.vars[decl];

            auto context = decl->getDeclContext();
            while (context->getDeclKind() == Decl::LinkageSpec)
                context = context->getParent();

            // A global variable:
            if (context->getDeclKind() == Decl::Namespace) {
                state.vars[decl] = state.createBorrowed();
                return state.vars[decl];
            }

            errs() << "\n\n";
            expr->dump();
            dump(decl->getDeclContext());
            errs() << state.vars.size() << " known decls:\n";
            for (auto&& p : state.vars) {
                p.first->dump();
            }
            ASSERT(0, "Don't know how to handle");
        }

        if (auto callexpr = dyn_cast<CallExpr>(expr)) {
            auto callee = callexpr->getCallee();

            auto ft_ptr = callee->getType();
            const FunctionProtoType* ft;

            if (isa<BuiltinType>(ft_ptr)) {
                ft = NULL;
            } else if (isa<TemplateSpecializationType>(ft_ptr)) {
                // Not really sure about this:
                ft = NULL;
            } else {
                ft_ptr->dump();
                assert(ft_ptr->isPointerType());
                auto pointed_to = ft_ptr->getPointeeType();
                while (auto pt = dyn_cast<ParenType>(pointed_to))
                    pointed_to = pt->getInnerType();
                assert(isa<FunctionProtoType>(pointed_to));
                ft = cast<FunctionProtoType>(pointed_to);
            }

            handle(callee, state);

            if (ft) {
                for (auto param : ft->param_types()) {
                    // TODO: process stolen-ness
                }
            }

            for (auto arg : callexpr->arguments()) {
                handle(arg, state);
            }

            bool can_throw = ft && !ft->isNothrow(*Context, false);
            if (can_throw)
                checkClean(state);

            if (isRefcountedType(callexpr->getType())) {
                // TODO: look for BORROWED annotations
                return state.createOwned();
            }
            return NULL;
        }

        if (auto condop = dyn_cast<ConditionalOperator>(expr)) {
            handle(condop->getCond(), state);

            BlockState false_state(state);
            RefState* s1 = handle(condop->getTrueExpr(), state);
            RefState* s2 = handle(condop->getFalseExpr(), false_state);
            checkSameAndMerge(state, false_state);

            assert((s1 == NULL) == (s2 == NULL));
            if (s1) {
                assert(s1->num_refs == s2->num_refs);
                ASSERT(s1->type == s2->type, "maybe could deal with this");
            }
            return s1;
        }

        expr->dump();
        RELEASE_ASSERT(0, "unhandled expr type: %s\n", expr->getStmtClassName());
    }

    void handle(Stmt* stmt, BlockState& state) {
        assert(stmt);

        if (auto expr = dyn_cast<Expr>(stmt)) {
            handle(expr, state);
            return;
        }

        if (auto cstmt = dyn_cast<CompoundStmt>(stmt)) {
            for (auto sub_stmt : cstmt->body())
                handle(sub_stmt, state);
            return;
        }

        if (auto dostmt = dyn_cast<DoStmt>(stmt)) {
            Expr* cond = dostmt->getCond();
            auto cond_casted = dyn_cast<CXXBoolLiteralExpr>(cond);
            RELEASE_ASSERT(cond_casted && cond_casted->getValue() == false,
                           "Only support `do {} while(false);` statements for now");
            handle(dostmt->getBody(), state);
            return;
        }

        if (auto ifstmt = dyn_cast<IfStmt>(stmt)) {
            handle(ifstmt->getCond(), state);

            BlockState else_state(state);
            if (ifstmt->getThen())
                handle(ifstmt->getThen(), state);
            if (ifstmt->getElse())
                handle(ifstmt->getElse(), else_state);
            checkSameAndMerge(state, else_state);
            return;
        }

        if (auto declstmt = dyn_cast<DeclStmt>(stmt)) {
            assert(declstmt->isSingleDecl());

            auto decl = declstmt->getSingleDecl();
            assert(decl);

            auto vardecl = cast<VarDecl>(decl);
            assert(vardecl);

            assert(!state.vars.count(vardecl));

            bool is_refcounted = isRefcountedType(vardecl->getType());

            if (is_refcounted)
                state.vars[vardecl] = state.createBorrowed();

            if (vardecl->hasInit()) {
                RefState* assigning = handle(vardecl->getInit(), state);
                if (is_refcounted)
                    state.doAssign(vardecl, assigning);
            }
            return;
        }

        if (auto rtnstmt = dyn_cast<ReturnStmt>(stmt)) {
            auto rstate = handle(rtnstmt->getRetValue(), state);
            if (isRefcountedType(rtnstmt->getRetValue()->getType())) {
                ASSERT(rstate->num_refs > 0, "Returning an object with 0 refs!");
                // TODO: handle borrowed returns
                rstate->num_refs--;
            }
            return;
        }

        stmt->dump();
        RELEASE_ASSERT(0, "unhandled statement type: %s\n", stmt->getStmtClassName());
    }

    void checkFunction(FunctionDecl* func) {
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

        BlockState state;
        for (auto param : func->params()) {
            if (isRefcountedType(param->getType())) {
                auto& rstate = state.vars[param];
                assert(!rstate);
                rstate = state.createBorrowed();
            }
        }
        errs() << "Starting.  state has " << state.vars.size() << " vars\n";
        handle(func->getBody(), state);
        checkClean(state);
    }

public:
    explicit RefcheckingVisitor(ASTContext* Context) : Context(Context) {}

    virtual ~RefcheckingVisitor() {}

    virtual bool VisitFunctionDecl(FunctionDecl* func) {
        if (!func->hasBody())
            return true /* keep going */;
        if (!func->isThisDeclarationADefinition())
            return true /* keep going */;

        auto filename = Context->getSourceManager().getFilename(func->getNameInfo().getLoc());

        // Filter out functions defined in libraries:
        if (filename.find("include/c++") != StringRef::npos)
            return true;
        if (filename.find("include/x86_64-linux-gnu") != StringRef::npos)
            return true;
        if (filename.find("include/llvm") != StringRef::npos)
            return true;
        if (filename.find("lib/clang") != StringRef::npos)
            return true;

        // errs() << filename << '\n';

        // if (func->getNameInfo().getAsString() != "firstlineno")
        // return true;

        checkFunction(func);

        return true /* keep going */;
    }
};

class RefcheckingASTConsumer : public ASTConsumer {
private:
    RefcheckingVisitor visitor;

public:
    explicit RefcheckingASTConsumer(ASTContext* Context) : visitor(Context) {}

    virtual void HandleTranslationUnit(ASTContext& Context) {
        visitor.TraverseDecl(Context.getTranslationUnitDecl());
        // dumper()->TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class RefcheckingFrontendAction : public ASTFrontendAction {
public:
    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& CI, StringRef fname) {
        return std::unique_ptr<ASTConsumer>(new RefcheckingASTConsumer(&CI.getASTContext()));
    }
};

int main(int argc, const char** argv) {
    CommonOptionsParser OptionsParser(argc, argv, RefcheckingToolCategory);
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<RefcheckingFrontendAction>().get());
}
