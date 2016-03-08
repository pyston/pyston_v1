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

class MyVisitor : public RecursiveASTVisitor<MyVisitor> {
private:
    ASTContext *Context;

public:
    explicit MyVisitor(ASTContext *Context) : Context(Context) {
    }

    virtual ~MyVisitor() {
    }

    virtual bool VisitFunctionDecl(FunctionDecl* func) {
        errs() << func->getNameInfo().getName() << '\n';
        return true;
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
