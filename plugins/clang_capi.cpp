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

static llvm::cl::OptionCategory ToolingSampleCategory("Tooling Sample");

//auto matcher = memberExpr().bind("member_expr");
auto matcher = memberExpr(member(fieldDecl(hasName("ob_refcnt")).bind("field"))).bind("member_expr");

MatchFinder Finder;
Rewriter TheRewriter;

class Replacer : public MatchFinder::MatchCallback {
public:
    virtual void run (const MatchFinder::MatchResult &Result) {
        errs() << "matched!\n";
        if (const MemberExpr* ME = Result.Nodes.getNodeAs<clang::MemberExpr>("member_expr")) {
            ME->dump();
            //cast<FieldDecl>(ME->getMemberDecl())->dump();
            TheRewriter.InsertTextBefore(ME->getSourceRange().getBegin(), "/* Pyston change, was:  ");
            TheRewriter.InsertTextAfter(ME->getSourceRange().getEnd(), "*/ 2");
            for (auto c : ME->Stmt::children()) {
                c->dump();
            }
        }
        if (const FieldDecl* ME = Result.Nodes.getNodeAs<clang::FieldDecl>("field")) {
            ME->dump();
            TheRewriter.InsertTextAfter(ME->getLocStart(), "/**/");
        }
        //Result.dump();
    }
};

class MyFrontendAction : public ASTFrontendAction {
public:
    MyFrontendAction() {}

    void EndSourceFileAction() override {
        SourceManager &SM = TheRewriter.getSourceMgr();
        // Now emit the rewritten buffer.
        TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& CI, StringRef file) override {
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return Finder.newASTConsumer();
    }
};

int main(int argc, const char **argv) {
  CommonOptionsParser op(argc, argv, ToolingSampleCategory);
  ClangTool Tool(op.getCompilations(), op.getSourcePathList());

  Replacer replacer;
  Finder.addMatcher(matcher, &replacer);

  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
