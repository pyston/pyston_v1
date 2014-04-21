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

static cl::opt<bool>
Publicize("p", cl::desc("Make all private-linkage variables public"));

class MyObjectCache : public llvm::ObjectCache {
    private:
    public:
        MyObjectCache() {
        }

        virtual void notifyObjectCompiled(const llvm::Module *M, const llvm::MemoryBuffer *Obj) {
            std::string ErrStr;
            llvm::raw_fd_ostream IRObjectFile(OutputFilename.c_str(), ErrStr, llvm::sys::fs::F_Binary);
            if (!Force && CheckBitcodeOutputToConsole(IRObjectFile))
                exit(1);
            IRObjectFile << Obj->getBuffer();
        }

        virtual llvm::MemoryBuffer* getObject(const llvm::Module* M) {
            return NULL;
        }

};

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);

    LLVMContext &Context = getGlobalContext();
    llvm_shutdown_obj Y;
    cl::ParseCommandLineOptions(argc, argv, "mcjit pre-cacher");

    SMDiagnostic Err;

    OwningPtr<Module> M;
    M.reset(ParseIRFile(InputFilename, Err, Context));

    if (M.get() == 0) {
        Err.print(argv[0], errs());
        return 1;
    }

    if (Publicize) {
        for (Module::global_iterator I = M->global_begin(), E = M->global_end(); I != E; ++I) {
            if (I->getLinkage() == GlobalVariable::PrivateLinkage) {
                I->setLinkage(GlobalVariable::ExternalLinkage);
            }
        }
    }

    Triple TheTriple(M->getTargetTriple());

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    EngineBuilder eb(M.get());
    eb.setEngineKind(EngineKind::JIT);
    eb.setUseMCJIT(true);

    TargetMachine *tm = eb.selectTarget();
    assert(tm);
    ExecutionEngine *engine = eb.create(tm);
    assert(engine);

    engine->setObjectCache(new MyObjectCache());
    engine->generateCodeForModule(M.get());

    return 0;
}

