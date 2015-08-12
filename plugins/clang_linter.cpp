// Originally from https://github.com/eliben/llvm-clang-samples/blob/master/src_clang/tooling_sample.cpp with the following header.  Modifications copyright Dropbox, Inc.
//
//------------------------------------------------------------------------------
// Tooling sample. Demonstrates:
//
// * How to write a simple source tool using libTooling.
// * How to use RecursiveASTVisitor to find interesting AST nodes.
// * How to use the Rewriter API to rewrite the source code.
//
// Eli Bendersky (eliben@gmail.com)
// This code is in the public domain
//------------------------------------------------------------------------------
#include <sstream>
#include <string>

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace clang::ast_matchers;

auto matcher = callExpr(
        callee(functionDecl(hasName("pyston::isSubclass"))),
        hasArgument(0, memberExpr(member(fieldDecl(hasName("cls"))))),
        hasArgument(1, declRefExpr(to(namedDecl(matchesName("(int|long|list|tuple|string|unicode|dict|baseexc|type)_cls")))).bind("parent_cls"))
    ).bind("call");

MatchFinder Finder;
DiagnosticsEngine* diagnostics;

// Not sure why the build wants us to add this:
extern "C" void AnnotateHappensAfter(const char* file, int line, const volatile void *cv) {}

std::set<SourceLocation> reported;

class Replacer : public MatchFinder::MatchCallback {
public:
    virtual void run (const MatchFinder::MatchResult &Result) {
        const CallExpr* ME = Result.Nodes.getNodeAs<clang::CallExpr>("call");

        //errs() << "matched!\n";
        //ME->dump();
        //Result.Nodes.getNodeAs<clang::DeclRefExpr>("parent_cls")->dump();

        unsigned diag_id = diagnostics->getCustomDiagID(
            DiagnosticsEngine::Error, "perf issue: use PyFoo_Check instead of isSubclass(obj->cls, foo_cls)");
        auto source = ME->getSourceRange().getBegin();
        if (!reported.count(source)) {
            reported.insert(source);
            diagnostics->Report(source, diag_id);
        }
    }
};

Replacer replacer;

class MyFrontendAction : public PluginASTAction {
public:
    MyFrontendAction() {}

    bool ParseArgs(const CompilerInstance &CI, const std::vector<std::string> &args) override {
        diagnostics = &CI.getDiagnostics();
        return true;
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& CI, StringRef file) override {
        Finder.addMatcher(matcher, &replacer);

        return Finder.newASTConsumer();
    }
};

static FrontendPluginRegistry::Add<MyFrontendAction> X("pyston-linter", "run some Pyston-specific lint checks");
