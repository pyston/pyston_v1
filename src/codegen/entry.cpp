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

#include "codegen/entry.h"

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/dis.h"
#include "codegen/memmgr.h"
#include "codegen/profiling/profiling.h"
#include "codegen/stackmaps.h"
#include "core/options.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/types.h"

namespace pyston {

GlobalState g;

extern "C" {
#ifndef BINARY_SUFFIX
#error Must define BINARY_SUFFIX
#endif
#ifndef BINARY_STRIPPED_SUFFIX
#error Must define BINARY_STRIPPED_SUFFIX
#endif

#define _CONCAT3(a, b, c) a##b##c
#define CONCAT3(a, b, c) _CONCAT3(a, b, c)
#define _CONCAT4(a, b, c, d) a##b##c##d
#define CONCAT4(a, b, c, d) _CONCAT4(a, b, c, d)

#define STDLIB_BC_START CONCAT3(_binary_stdlib, BINARY_SUFFIX, _bc_start)
#define STDLIB_BC_SIZE CONCAT3(_binary_stdlib, BINARY_SUFFIX, _bc_size)
extern char STDLIB_BC_START[];
extern int STDLIB_BC_SIZE;

#define STRIPPED_STDLIB_BC_START CONCAT4(_binary_stdlib, BINARY_SUFFIX, BINARY_STRIPPED_SUFFIX, _bc_start)
#define STRIPPED_STDLIB_BC_SIZE CONCAT4(_binary_stdlib, BINARY_SUFFIX, BINARY_STRIPPED_SUFFIX, _bc_size)
extern char STRIPPED_STDLIB_BC_START[];
extern int STRIPPED_STDLIB_BC_SIZE;
}

static llvm::Module* loadStdlib() {
    Timer _t("to load stdlib");

    llvm::StringRef data;
    if (!USE_STRIPPED_STDLIB) {
        // Make sure the stdlib got linked in correctly; check the magic number at the beginning:
        assert(STDLIB_BC_START[0] == 'B');
        assert(STDLIB_BC_START[1] == 'C');
        intptr_t size = (intptr_t)&STDLIB_BC_SIZE;
        assert(size > 0 && size < 1 << 30); // make sure the size is being loaded correctly
        data = llvm::StringRef(STDLIB_BC_START, size);
    } else {
        // Make sure the stdlib got linked in correctly; check the magic number at the beginning:
        assert(STRIPPED_STDLIB_BC_START[0] == 'B');
        assert(STRIPPED_STDLIB_BC_START[1] == 'C');
        intptr_t size = (intptr_t)&STRIPPED_STDLIB_BC_SIZE;
        assert(size > 0 && size < 1 << 30); // make sure the size is being loaded correctly
        data = llvm::StringRef(STRIPPED_STDLIB_BC_START, size);
    }

    llvm::MemoryBuffer* buffer = llvm::MemoryBuffer::getMemBuffer(data, "", false);

    // llvm::ErrorOr<llvm::Module*> m_or = llvm::parseBitcodeFile(buffer, g.context);
    llvm::ErrorOr<llvm::Module*> m_or = llvm::getLazyBitcodeModule(buffer, g.context);
    RELEASE_ASSERT(m_or, "");
    llvm::Module* m = m_or.get();
    assert(m);

    for (llvm::Module::global_iterator I = m->global_begin(), E = m->global_end(); I != E; ++I) {
        if (I->getLinkage() == llvm::GlobalVariable::PrivateLinkage)
            I->setLinkage(llvm::GlobalVariable::ExternalLinkage);
    }
    m->setModuleIdentifier("  stdlib  ");
    return m;
}

class MyObjectCache : public llvm::ObjectCache {
private:
    bool loaded;

public:
    MyObjectCache() : loaded(false) {}

    virtual void notifyObjectCompiled(const llvm::Module* M, const llvm::MemoryBuffer* Obj) {}

    virtual llvm::MemoryBuffer* getObject(const llvm::Module* M) {
        assert(!loaded);
        loaded = true;
        g.engine->setObjectCache(NULL);
        std::unique_ptr<MyObjectCache> del_at_end(this);

#if 0
            if (!USE_STRIPPED_STDLIB) {
                stajt = STDLIB_CACHE_START;
                size = (intptr_t)&STDLIB_CACHE_SIZE;
            } else {
                start = STRIPPED_STDLIB_CACHE_START;
                size = (intptr_t)&STRIPPED_STDLIB_CACHE_SIZE;
            }
#else
        RELEASE_ASSERT(0, "");
        char* start = NULL;
        intptr_t size = 0;
#endif

        // Make sure the stdlib got linked in correctly; check the magic number at the beginning:
        assert(start[0] == 0x7f);
        assert(start[1] == 'E');
        assert(start[2] == 'L');
        assert(start[3] == 'F');

        assert(size > 0 && size < 1 << 30); // make sure the size is being loaded correctly

        llvm::StringRef data(start, size);
        return llvm::MemoryBuffer::getMemBufferCopy(data, "");
    }
};

static void handle_sigfpe(int signum) {
    assert(signum == SIGFPE);
    fprintf(stderr, "ZeroDivisionError: integer division or modulo by zero\n");
    exit(1);
}

void initCodegen() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    g.stdlib_module = loadStdlib();

    llvm::EngineBuilder eb(new llvm::Module("empty_initial_module", g.context));
    eb.setEngineKind(llvm::EngineKind::JIT); // specify we only want the JIT, and not the interpreter fallback
    eb.setUseMCJIT(true);
    eb.setMCJITMemoryManager(createMemoryManager());
    // eb.setOptLevel(llvm::CodeGenOpt::None); // -O0
    // eb.setOptLevel(llvm::CodeGenOpt::Less); // -O1
    // eb.setOptLevel(llvm::CodeGenOpt::Default); // -O2, -Os
    // eb.setOptLevel(llvm::CodeGenOpt::Aggressive); // -O3

    llvm::TargetOptions target_options;
    target_options.NoFramePointerElim = true;
    // target_options.EnableFastISel = true;
    eb.setTargetOptions(target_options);

    g.tm = eb.selectTarget();
    assert(g.tm && "failed to get a target machine");
    g.engine = eb.create(g.tm);
    assert(g.engine && "engine creation failed?");

    // g.engine->setObjectCache(new MyObjectCache());

    g.i1 = llvm::Type::getInt1Ty(g.context);
    g.i8 = llvm::Type::getInt8Ty(g.context);
    g.i8_ptr = g.i8->getPointerTo();
    g.i32 = llvm::Type::getInt32Ty(g.context);
    g.i64 = llvm::Type::getInt64Ty(g.context);
    g.void_ = llvm::Type::getVoidTy(g.context);
    g.double_ = llvm::Type::getDoubleTy(g.context);

    std::vector<llvm::JITEventListener*> listeners = makeJITEventListeners();
    for (int i = 0; i < listeners.size(); i++) {
        g.jit_listeners.push_back(listeners[i]);
        g.engine->RegisterJITEventListener(listeners[i]);
    }

    llvm::JITEventListener* stackmap_listener = makeStackMapListener();
    g.jit_listeners.push_back(stackmap_listener);
    g.engine->RegisterJITEventListener(stackmap_listener);

#if ENABLE_INTEL_JIT_EVENTS
    llvm::JITEventListener* intel_listener = llvm::JITEventListener::createIntelJITEventListener();
    g.jit_listeners.push_back(intel_listener);
    g.engine->RegisterJITEventListener(intel_listener);
#endif

    llvm::JITEventListener* registry_listener = makeRegistryListener();
    g.jit_listeners.push_back(registry_listener);
    g.engine->RegisterJITEventListener(registry_listener);

    llvm::JITEventListener* tracebacks_listener = makeTracebacksListener();
    g.jit_listeners.push_back(tracebacks_listener);
    g.engine->RegisterJITEventListener(tracebacks_listener);

    if (SHOW_DISASM) {
        llvm::JITEventListener* listener = new PystonJITEventListener();
        g.jit_listeners.push_back(listener);
        g.engine->RegisterJITEventListener(listener);
    }

    initGlobalFuncs(g);

    setupRuntime();

    signal(SIGFPE, &handle_sigfpe);

    // There are some parts of llvm that are only configurable through command line args,
    // so construct a fake argc/argv pair and pass it to the llvm command line machinery:
    const char* llvm_args[] = {
        "fake_name", "--enable-stackmap-liveness", "--enable-patchpoint-liveness",

// Enabling and debugging fast-isel:
//"--fast-isel",
//"--fast-isel-verbose",
////"--fast-isel-abort",
#ifndef NDEBUG
//"--debug-only=debug-ir",
//"--debug-only=regalloc",
//"--debug-only=stackmaps",
#endif
        //"--print-after-all",
        //"--print-machineinstrs",
    };
    int num_llvm_args = sizeof(llvm_args) / sizeof(llvm_args[0]);
    llvm::cl::ParseCommandLineOptions(num_llvm_args, llvm_args, "<you should never see this>\n");
}

void teardownCodegen() {
    for (int i = 0; i < g.jit_listeners.size(); i++) {
        g.engine->UnregisterJITEventListener(g.jit_listeners[i]);
        delete g.jit_listeners[i];
    }
    g.jit_listeners.clear();
    delete g.engine;
}

void printAllIR() {
    assert(0 && "unimplemented");
    fprintf(stderr, "==============\n");
}

int joinRuntime() {
    // In the future this will have to wait for non-daemon
    // threads to finish

    if (PROFILE)
        g.func_addr_registry.dumpPerfMap();

    teardownRuntime();
    teardownCodegen();

    return 0;
}
}
