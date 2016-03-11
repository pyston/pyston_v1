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

class FunctionRefchecker {
private:
    CompilerInstance& CI;
    ASTContext* Context;
    SourceManager* SM;

    bool done = false;

    enum class AnnotationType {
        NONE,
        BORROWED,
        STOLEN,
        SKIP,
    };
    AnnotationType return_ann;

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
            for (auto&& state : bstate.states) {
                if (state.num_refs == 0)
                    continue;

                bool found = false;
                for (auto&& p : bstate.vars) {
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

    void checkUsable(RefState* rstate) {
        if (!rstate)
            return;

        assert(rstate->num_refs >= 0);
        assert(rstate->type == BORROWED || rstate->num_refs > 0);
    }

    bool isRefcountedName(StringRef name) {
        return name.startswith("Box") || ((name.startswith("Py") || name.startswith("_Py")) && name.endswith("Object"));
    }

    bool isRefcountedType(const QualType& t) {
        if (!t->isPointerType())
            return false;
        //errs() << '\n';

        auto pointed_to = t->getPointeeType();
        while (1) {
            //errs() << "is refcounted?   ";
            //pointed_to->dump();
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

        //errs() << "final:   ";
        //pointed_to->dump();

        if (isa<BuiltinType>(pointed_to) || isa<FunctionType>(pointed_to))
            return false;

        if (isa<TemplateTypeParmType>(pointed_to)) {
            // TODO Hmm not sure what to do about templates
            return false;
        }

        auto cxx_record_decl = pointed_to->getAsCXXRecordDecl();
        if (!cxx_record_decl)
            t->dump();
        assert(cxx_record_decl);

        auto name = cxx_record_decl->getName();
        return isRefcountedName(name);
    }

    AnnotationType getAnnotationType(SourceLocation loc) {
        // see clang::DiagnosticRenderer::emitMacroExpansions for more info:
        if (!loc.isMacroID())
            return AnnotationType::NONE;
        StringRef MacroName = Lexer::getImmediateMacroName(loc, *SM, CI.getLangOpts());
        if (MacroName == "BORROWED")
            return AnnotationType::BORROWED;
        if (MacroName == "STOLEN")
            return AnnotationType::STOLEN;
        if (MacroName == "NOREFCHECK")
            return AnnotationType::SKIP;
        return getAnnotationType(SM->getImmediateMacroCallerLoc(loc));
    }

    AnnotationType getReturnAnnotationType(FunctionDecl* fdecl) {
        return getAnnotationType(fdecl->getReturnTypeSourceRange().getBegin());
    }

    RefState* handle(Expr* expr, BlockState& state) {
        if (isa<StringLiteral>(expr) || isa<IntegerLiteral>(expr) || isa<CXXBoolLiteralExpr>(expr)) {
            return NULL;
        }

        if (isa<UnresolvedLookupExpr>(expr) || isa<UnresolvedMemberExpr>(expr) || isa<CXXUnresolvedConstructExpr>(expr)
            || isa<CXXDependentScopeMemberExpr>(expr) || isa<DependentScopeDeclRefExpr>(expr)
            || isa<CXXConstructExpr>(expr) || isa<PredefinedExpr>(expr) || isa<PackExpansionExpr>(expr)) {
            // Not really sure about this:
            assert(!isRefcountedType(expr->getType()));
            return NULL;
        }

        if (isa<CXXDefaultArgExpr>(expr)) {
            // Not really sure about this:
            assert(!isRefcountedType(expr->getType()));
            return NULL;
        }

        if (isa<GNUNullExpr>(expr)) {
            if (isRefcountedType(expr->getType()))
                return state.createBorrowed();
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
            handle(unaryop->getSubExpr(), state);
            if (isRefcountedType(unaryop->getType())) {
                if (unaryop->getOpcode() == UO_AddrOf)
                    return state.createBorrowed();
            }
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
            //castexpr->getType()->dump();
            //castexpr->getSubExpr()->getType()->dump();
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
                //ft_ptr->dump();
                assert(ft_ptr->isPointerType());
                auto pointed_to = ft_ptr->getPointeeType();
                while (auto pt = dyn_cast<ParenType>(pointed_to))
                    pointed_to = pt->getInnerType();
                assert(isa<FunctionProtoType>(pointed_to));
                ft = cast<FunctionProtoType>(pointed_to);
            }

            do {
                if (auto castexpr = dyn_cast<CastExpr>(callee)) {
                    callee = castexpr->getSubExpr();
                    continue;
                }
            } while (0);

            bool is_member;
            ValueDecl* callee_decl;
            if (auto me = dyn_cast<MemberExpr>(callee)) {
                callee_decl = me->getMemberDecl();
                is_member = true;
            } else if (auto ref = dyn_cast<DeclRefExpr>(callee)) {
                callee_decl = ref->getDecl();
                is_member = false;
            } else {
                RELEASE_ASSERT(0, "%s", callee->getStmtClassName());
            }
            //errs() << callee_decl->getDeclKindName() << '\n';

            auto callee_fdecl = dyn_cast<FunctionDecl>(callee_decl);
            assert(callee_fdecl);

            handle(callee, state);

            for (auto arg : callexpr->arguments()) {
                handle(arg, state);
            }

            bool can_throw = ft && !isUnresolvedExceptionSpec(ft->getExceptionSpecType())
                             && !ft->isNothrow(*Context, false);
            if (can_throw)
                checkClean(state);

            if (isRefcountedType(callexpr->getType())) {
                auto return_ann = getReturnAnnotationType(callee_fdecl);

                if (return_ann == AnnotationType::BORROWED)
                    return state.createBorrowed();
                else
                    return state.createOwned();
            }
            return NULL;
        }

        if (auto newexpr = dyn_cast<CXXNewExpr>(expr)) {
            for (auto plc :
                 iterator_range<CXXNewExpr::arg_iterator>(newexpr->placement_arg_begin(), newexpr->placement_arg_end()))
                handle(plc, state);

            if (newexpr->hasInitializer())
                handle(newexpr->getInitializer(), state);

            if (isRefcountedType(newexpr->getType()))
                return state.createBorrowed();
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

        if (done)
            return;

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

            // Not really sure about these:
        if (auto forstmt = dyn_cast<ForStmt>(stmt)) {
            handle(forstmt->getInit(), state);
            handle(forstmt->getCond(), state);

            if (forstmt->getConditionVariable())
                assert(!isRefcountedType(forstmt->getConditionVariable()->getType()));

            BlockState old_state(state);
            handle(forstmt->getBody(), state);
            handle(forstmt->getInc(), state);
            checkSameAndMerge(state, old_state);
            return;
        }

        if (auto forstmt = dyn_cast<CXXForRangeStmt>(stmt)) {
            // Not really sure about these:
            handle(forstmt->getRangeInit(), state);
            handle(forstmt->getCond(), state);
            handle(forstmt->getInc(), state);

            BlockState old_state(state);
            handle(forstmt->getBody(), state);
            checkSameAndMerge(state, old_state);
            return;
        }

        if (auto whilestmt = dyn_cast<WhileStmt>(stmt)) {
            handle(whilestmt->getCond(), state);

            if (whilestmt->getConditionVariable())
                assert(!isRefcountedType(whilestmt->getConditionVariable()->getType()));

            BlockState old_state(state);
            handle(whilestmt->getBody(), state);
            checkSameAndMerge(state, old_state);
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
            for (auto decl : declstmt->decls()) {
                if (!isa<VarDecl>(decl))
                    errs() << decl->getDeclKindName() << '\n';
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
            }
            return;
        }

        if (auto rtnstmt = dyn_cast<ReturnStmt>(stmt)) {
            auto rstate = handle(rtnstmt->getRetValue(), state);
            if (isRefcountedType(rtnstmt->getRetValue()->getType())) {
                if (return_ann != AnnotationType::BORROWED) {
                    ASSERT(rstate->num_refs > 0, "Returning an object with 0 refs!");
                    rstate->num_refs--;
                }
            }
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

        return_ann = getReturnAnnotationType(func);

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

    explicit FunctionRefchecker(CompilerInstance& CI)
        : CI(CI), Context(&CI.getASTContext()), SM(&CI.getSourceManager()) {}

public:
    static void checkFunction(FunctionDecl* func, CompilerInstance& CI) {
        FunctionRefchecker(CI).checkFunction(func);
    }
};

class RefcheckingVisitor : public RecursiveASTVisitor<RefcheckingVisitor> {
private:
    CompilerInstance& CI;
    ASTContext* Context;
    SourceManager* SM;

public:
    explicit RefcheckingVisitor(CompilerInstance& CI)
        : CI(CI), Context(&CI.getASTContext()), SM(&CI.getSourceManager()) {}

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

        //auto filename = Context->getSourceManager().getFilename(func->getSourceRange().getBegin());
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

        // if (func->getNameInfo().getAsString() != "firstlineno")
        // return true;

        FunctionRefchecker::checkFunction(func, CI);

        return true /* keep going */;
    }
};

class RefcheckingASTConsumer : public ASTConsumer {
private:
    RefcheckingVisitor visitor;

public:
    explicit RefcheckingASTConsumer(CompilerInstance& CI) : visitor(CI) {}

    virtual void HandleTranslationUnit(ASTContext& Context) {
        visitor.TraverseDecl(Context.getTranslationUnitDecl());
        // dumper()->TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class RefcheckingFrontendAction : public ASTFrontendAction {
public:
    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& CI, StringRef fname) {
        return std::unique_ptr<ASTConsumer>(new RefcheckingASTConsumer(CI));
    }
};

// A way to inject refchecker-only compilation flags.
// Not currently used, but uncomment the line in getCompileCommands() to define the `REFCHECKER` directive.
class MyCompilationDatabase : public CompilationDatabase {
private:
    CompilationDatabase& base;
public:
    MyCompilationDatabase(CompilationDatabase& base) : base(base) {
    }

    virtual vector<CompileCommand> getCompileCommands(StringRef FilePath) const {
        auto rtn = base.getCompileCommands(FilePath);
        assert(rtn.size() == 1);
        // rtn[0].CommandLine.push_back("-DREFCHECKER");
        return rtn;
    }

    virtual vector<std::string> getAllFiles() const {
        assert(0);
    }

    virtual vector<CompileCommand> getAllCompileCommands() const {
        assert(0);
    }
};

int main(int argc, const char** argv) {
    CommonOptionsParser OptionsParser(argc, argv, RefcheckingToolCategory);
    ClangTool Tool(MyCompilationDatabase(OptionsParser.getCompilations()), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<RefcheckingFrontendAction>().get());
}
