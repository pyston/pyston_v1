// Copyright (c) 2014 Dropbox, Inc.
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

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
        cl::init("-"), cl::value_desc("filename"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Specify output filename"),
        cl::value_desc("filename"), cl::init("-"));

static cl::opt<bool>
Force("f", cl::desc("Enable binary output on terminals"));

// TODO should restructure this as a set of passes rather than a monolithic binary


// This function was originally gotten from http://llvm.org/svn/llvm-project/vmkit/branches/mcjit/lib/vmkit-prepare-code/adapt-linkage.cc
// It was released under the license here http://llvm.org/svn/llvm-project/vmkit/trunk/LICENSE.TXT
bool makeVisible(llvm::GlobalValue* gv) {
    llvm::GlobalValue::LinkageTypes linkage = gv->getLinkage();
    bool changed = false;

    if (linkage == llvm::GlobalValue::LinkOnceODRLinkage) {
        gv->setLinkage(llvm::GlobalValue::WeakODRLinkage);
        changed = true;
    } else if (linkage == llvm::GlobalValue::LinkOnceAnyLinkage) {
        gv->setLinkage(llvm::GlobalValue::WeakAnyLinkage);
        changed = true;
    } else if (linkage == llvm::GlobalValue::PrivateLinkage) {
        gv->setName(gv->getParent()->getModuleIdentifier() + gv->getName());
        gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
        changed = true;
    } else if (linkage == llvm::GlobalValue::InternalLinkage) {
        // Not sure if this is the right linkage here:
        gv->setLinkage(llvm::GlobalValue::WeakODRLinkage);
        changed = true;
    }

    // Hidden symbols won't end up as globals.
    // Worse, a hidden symbol, when linked with a default-visibility symbol,
    // will result in a non-visible symbol.
    // So it's not enough to just set the visibility here; instead we have to
    // set it to protected *and* change the name.
    // The only thing affected by this that I know about is __clang_call_terminate.
    llvm::GlobalValue::VisibilityTypes visibility = gv->getVisibility();
    if (visibility == llvm::GlobalValue::HiddenVisibility) {
        gv->setVisibility(llvm::GlobalValue::ProtectedVisibility);
        //gv->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
        gv->setName(gv->getName() + "_protected");

        changed = true;
    }

    return changed;
}

bool isConstant(MDNode* parent_type, int offset) {
    MDString *s = cast<MDString>(parent_type->getOperand(0));

    if (s->getString() == "_ZTSN6pyston19BoxedXrangeIteratorE") {
        return (offset == 16);
    }

    if (s->getString() == "_ZTSN6pyston8BoxedIntE") {
        return (offset == 16);
    }

    if (s->getString() == "_ZTSN6pyston10BoxedFloatE") {
        return (offset == 16);
    }

    if (s->getString() == "_ZTSN6pyston11BoxedXrangeE") {
        return offset == 16 || offset == 24 || offset == 32;
    }

    return false;
}

bool updateTBAA(Function* f) {
    bool changed = false;

    LLVMContext &c = f->getContext();

    for (auto it = inst_begin(f), end = inst_end(f); it != end; ++it) {
        MDNode *tbaa = it->getMetadata(LLVMContext::MD_tbaa);
        if (!tbaa)
            continue;
        //tbaa->dump();

        assert(tbaa->getNumOperands() == 3);

        if (!isConstant(llvm::cast<MDNode>(tbaa->getOperand(0)), llvm::cast<ConstantInt>(tbaa->getOperand(2))->getSExtValue())) {
            continue;
        }

        std::vector<Value*> operands;

        for (int i = 0; i < tbaa->getNumOperands(); i++) {
            operands.push_back(tbaa->getOperand(i));
        }
        operands.push_back(ConstantInt::get(Type::getInt64Ty(c), 1));

        MDNode *new_tbaa = MDNode::get(c, operands);
        it->setMetadata(LLVMContext::MD_tbaa, new_tbaa);
        //new_tbaa->dump();
    }

    return changed;
}

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);

    LLVMContext &Context = getGlobalContext();
    llvm_shutdown_obj Y;
    cl::ParseCommandLineOptions(argc, argv, "mcjit pre-cacher");

    SMDiagnostic Err;

    std::unique_ptr<Module> M(ParseIRFile(InputFilename, Err, Context));

    if (M.get() == 0) {
        Err.print(argv[0], errs());
        return 1;
    }

    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
        makeVisible(I);
    }

    for (Module::global_iterator I = M->global_begin(), E = M->global_end(); I != E; ++I) {
        makeVisible(I);
    }

    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
        updateTBAA(I);
    }

    if (OutputFilename.empty())
        OutputFilename = "-";

    std::string ErrorInfo;
    tool_output_file out(OutputFilename.c_str(), ErrorInfo, sys::fs::F_None);
    if (!ErrorInfo.empty()) {
        errs() << ErrorInfo << '\n';
        return 1;
    }

    WriteBitcodeToFile(M.get(), out.os());

    out.keep();

    return 0;
}

