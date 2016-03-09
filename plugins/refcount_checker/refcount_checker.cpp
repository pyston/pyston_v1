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

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static cl::OptionCategory MyToolCategory("my-tool options");

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

namespace {
  class ASTPrinter : public ASTConsumer,
                     public RecursiveASTVisitor<ASTPrinter> {
    typedef RecursiveASTVisitor<ASTPrinter> base;

  public:
    ASTPrinter(raw_ostream *Out = nullptr, bool Dump = false,
               StringRef FilterString = "", bool DumpLookups = false)
        : Out(Out ? *Out : llvm::outs()), Dump(Dump),
          FilterString(FilterString), DumpLookups(DumpLookups) {}

    void HandleTranslationUnit(ASTContext &Context) override {
      TranslationUnitDecl *D = Context.getTranslationUnitDecl();

      if (FilterString.empty())
        return print(D);

      TraverseDecl(D);
    }

    bool shouldWalkTypesOfTypeLocs() const { return false; }

    bool TraverseDecl(Decl *D) {
      if (D && filterMatches(D)) {
        bool ShowColors = Out.has_colors();
        if (ShowColors)
          Out.changeColor(raw_ostream::BLUE);
        Out << ((Dump || DumpLookups) ? "Dumping " : "Printing ") << getName(D)
            << ":\n";
        if (ShowColors)
          Out.resetColor();
        print(D);
        Out << "\n";
        // Don't traverse child nodes to avoid output duplication.
        return true;
      }
      return base::TraverseDecl(D);
    }

    std::string getName(Decl *D) {
      if (isa<NamedDecl>(D))
        return cast<NamedDecl>(D)->getQualifiedNameAsString();
      return "";
    }
    bool filterMatches(Decl *D) {
      return getName(D).find(FilterString) != std::string::npos;
    }
    void print(Decl *D) {
      if (DumpLookups) {
        if (DeclContext *DC = dyn_cast<DeclContext>(D)) {
          if (DC == DC->getPrimaryContext())
            DC->dumpLookups(Out, Dump);
          else
            Out << "Lookup map is in primary DeclContext "
                << DC->getPrimaryContext() << "\n";
        } else
          Out << "Not a DeclContext\n";
      } else if (Dump)
        D->dump(Out);
      else
        D->print(Out, /*Indentation=*/0, /*PrintInstantiation=*/true);
    }

    raw_ostream &Out;
    bool Dump;
    std::string FilterString;
    bool DumpLookups;
  };

  class ASTDeclNodeLister : public ASTConsumer,
                     public RecursiveASTVisitor<ASTDeclNodeLister> {
  public:
    ASTDeclNodeLister(raw_ostream *Out = nullptr)
        : Out(Out ? *Out : llvm::outs()) {}

    void HandleTranslationUnit(ASTContext &Context) override {
      TraverseDecl(Context.getTranslationUnitDecl());
    }

    bool shouldWalkTypesOfTypeLocs() const { return false; }

    bool VisitNamedDecl(NamedDecl *D) {
      D->printQualifiedName(Out);
      Out << '\n';
      return true;
    }

  private:
    raw_ostream &Out;
  };

static std::unique_ptr<ASTPrinter> dumper() {
    return std::unique_ptr<ASTPrinter>(new ASTPrinter(&errs(), true, "", true));
}
} // end anonymous namespace

class MyVisitor : public RecursiveASTVisitor<MyVisitor> {
private:
    ASTContext *Context;

    struct RefState {
        enum {
            UNKNOWN,
            BORROWED,
            OWNED,
        } type;
        int num_refs;
    };
    struct BlockState {
        DenseMap<void*, RefState> vars;
    };

    void handle(Stmt* stmt) {
        assert(0);
    }

    void checkFunction(FunctionDecl* func) {
        errs() << "printing:\n";
        func->print(errs());
        errs() << "dumping:\n";
        func->dump(errs());

        BlockState state;
        handle(func->getBody());
    }

public:
    explicit MyVisitor(ASTContext *Context) : Context(Context) {
    }

    virtual ~MyVisitor() {
    }

    virtual bool VisitFunctionDecl(FunctionDecl* func) {
        if (!func->hasBody())
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

        //errs() << filename << '\n';

        if (func->getNameInfo().getAsString() != "firstlineno")
            return true;

        checkFunction(func);

        return true /* keep going */;
    }
};

class MyASTConsumer : public ASTConsumer {
private:
    MyVisitor visitor;

public:
    explicit MyASTConsumer(ASTContext *Context) : visitor(Context) {
    }

    virtual void HandleTranslationUnit(ASTContext &Context) {
        visitor.TraverseDecl(Context.getTranslationUnitDecl());
        //dumper()->TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class MyFrontendAction : public ASTFrontendAction {
public:
    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef fname) {
        return std::unique_ptr<ASTConsumer>(new MyASTConsumer(&CI.getASTContext()));
    }
};

int main(int argc, const char **argv) {
  CommonOptionsParser OptionsParser(argc, argv, MyToolCategory);
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
