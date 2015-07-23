// Copyright (c) 2014-2015 Dropbox, Inc.
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
#include <lz4frame.h>
#include <openssl/evp.h>
#include <unordered_map>

#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "codegen/codegen.h"
#include "codegen/memmgr.h"
#include "codegen/profiling/profiling.h"
#include "codegen/stackmaps.h"
#include "core/options.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

/*
 * Include this file to force the linking of non-default algorithms, such as the "basic" register allocator
 */
#include "llvm/CodeGen/LinkAllCodegenComponents.h"

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

#if LLVMREV < 216583
    llvm::MemoryBuffer* buffer = llvm::MemoryBuffer::getMemBuffer(data, "", false);
#else
    std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(data, "", false);
#endif

    // llvm::ErrorOr<llvm::Module*> m_or = llvm::parseBitcodeFile(buffer, g.context);
    llvm::ErrorOr<llvm::Module*> m_or = llvm::getLazyBitcodeModule(std::move(buffer), g.context);
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

class CompressedFile {
public:
    static bool writeFile(llvm::StringRef file_name, llvm::StringRef data) {
        std::error_code error_code;
        llvm::raw_fd_ostream file(file_name, error_code, llvm::sys::fs::F_RW);
        if (error_code)
            return false;

        int uncompressed_size = data.size();
        // Write the uncompressed size to the beginning of the file as a simple checksum.
        // It looks like each lz4 block has its own data checksum, but we need to also
        // make sure that we have all the blocks that we expected.
        // In particular, without this, an empty file seems to be a valid lz4 stream.
        file.write(reinterpret_cast<const char*>(&uncompressed_size), 4);

        LZ4F_preferences_t preferences;
        memset(&preferences, 0, sizeof(preferences));
        preferences.frameInfo.contentChecksumFlag = contentChecksumEnabled;
        preferences.frameInfo.contentSize = data.size();

        std::vector<char> compressed;
        size_t max_size = LZ4F_compressFrameBound(data.size(), &preferences);
        compressed.resize(max_size);
        size_t compressed_size = LZ4F_compressFrame(&compressed[0], max_size, data.data(), data.size(), &preferences);
        if (LZ4F_isError(compressed_size))
            return false;
        file.write(compressed.data(), compressed_size);
        return true;
    }

    static std::unique_ptr<llvm::MemoryBuffer> getFile(llvm::StringRef file_name) {
        auto compressed_content = llvm::MemoryBuffer::getFile(file_name, -1, false);
        if (!compressed_content)
            return std::unique_ptr<llvm::MemoryBuffer>();

        LZ4F_decompressionContext_t context;
        LZ4F_createDecompressionContext(&context, LZ4F_VERSION);

        LZ4F_frameInfo_t frame_info;
        memset(&frame_info, 0, sizeof(frame_info));

        const char* start = (*compressed_content)->getBufferStart();
        size_t pos = 0;
        size_t compressed_size = (*compressed_content)->getBufferSize();
        if (compressed_size < 4)
            return std::unique_ptr<llvm::MemoryBuffer>();

        int orig_uncompressed_size = *reinterpret_cast<const int*>(start);
        pos += 4;

        size_t remaining = compressed_size - pos;
        LZ4F_getFrameInfo(context, &frame_info, start + pos, &remaining);
        pos += remaining;

        std::vector<char> uncompressed;
        uncompressed.reserve(frame_info.contentSize);
        while (pos < compressed_size) {
            unsigned char buff[4096];
            size_t buff_size = sizeof(buff);
            remaining = compressed_size - pos;
            size_t error_code = LZ4F_decompress(context, buff, &buff_size, start + pos, &remaining, NULL);
            if (LZ4F_isError(error_code)) {
                LZ4F_freeDecompressionContext(context);
                return std::unique_ptr<llvm::MemoryBuffer>();
            }
            pos += remaining;
            if (buff_size != 0)
                uncompressed.insert(uncompressed.end(), buff, buff + buff_size);
        }

        LZ4F_freeDecompressionContext(context);
        if (uncompressed.size() != frame_info.contentSize)
            return std::unique_ptr<llvm::MemoryBuffer>();

        if (uncompressed.size() != orig_uncompressed_size)
            return std::unique_ptr<llvm::MemoryBuffer>();

        return llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(uncompressed.data(), uncompressed.size()));
    }
};

class PystonObjectCache : public llvm::ObjectCache {
private:
    // Stream which calculates the SHA256 hash of the data writen to.
    class HashOStream : public llvm::raw_ostream {
        EVP_MD_CTX* md_ctx;

        void write_impl(const char* ptr, size_t size) override { EVP_DigestUpdate(md_ctx, ptr, size); }
        uint64_t current_pos() const override { return 0; }

    public:
        HashOStream() {
            md_ctx = EVP_MD_CTX_create();
            RELEASE_ASSERT(md_ctx, "");
            int ret = EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL);
            RELEASE_ASSERT(ret == 1, "");
        }
        ~HashOStream() { EVP_MD_CTX_destroy(md_ctx); }

        std::string getHash() {
            flush();
            unsigned char md_value[EVP_MAX_MD_SIZE];
            unsigned int md_len = 0;
            int ret = EVP_DigestFinal_ex(md_ctx, md_value, &md_len);
            RELEASE_ASSERT(ret == 1, "");

            std::string str;
            str.reserve(md_len * 2 + 1);
            llvm::raw_string_ostream stream(str);
            for (int i = 0; i < md_len; ++i)
                stream.write_hex(md_value[i]);
            return stream.str();
        }
    };

    llvm::SmallString<128> cache_dir;
    std::string module_identifier;
    std::string hash_before_codegen;

public:
    PystonObjectCache() {
        llvm::sys::path::home_directory(cache_dir);
        llvm::sys::path::append(cache_dir, ".cache");
        llvm::sys::path::append(cache_dir, "pyston");
        llvm::sys::path::append(cache_dir, "object_cache");

        cleanupCacheDirectory();
    }


#if LLVMREV < 216002
    virtual void notifyObjectCompiled(const llvm::Module* M, const llvm::MemoryBuffer* Obj)
#else
    virtual void notifyObjectCompiled(const llvm::Module* M, llvm::MemoryBufferRef Obj)
#endif
    {
        RELEASE_ASSERT(module_identifier == M->getModuleIdentifier(), "");
        RELEASE_ASSERT(!hash_before_codegen.empty(), "");

        llvm::SmallString<128> cache_file = cache_dir;
        llvm::sys::path::append(cache_file, hash_before_codegen);
        if (!llvm::sys::fs::exists(cache_dir.str()) && llvm::sys::fs::create_directories(cache_dir.str()))
            return;

        CompressedFile::writeFile(cache_file, Obj.getBuffer());
    }

#if LLVMREV < 215566
    virtual llvm::MemoryBuffer* getObject(const llvm::Module* M)
#else
    virtual std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* M)
#endif
    {
        static StatCounter jit_objectcache_hits("num_jit_objectcache_hits");
        static StatCounter jit_objectcache_misses("num_jit_objectcache_misses");

        module_identifier = M->getModuleIdentifier();

        // Generate a hash for the module
        HashOStream hash_stream;
        llvm::WriteBitcodeToFile(M, hash_stream);
        hash_before_codegen = hash_stream.getHash();

        llvm::SmallString<128> cache_file = cache_dir;
        llvm::sys::path::append(cache_file, hash_before_codegen);
        if (!llvm::sys::fs::exists(cache_file.str())) {
#if 0
            // This code helps with identifying why we got a cache miss for a file.
            // - clear the cache directory
            // - run pyston
            // - run pyston a second time
            // - Now look for "*_second.ll" files in the cache directory and compare them to the "*_first.ll" IR dump
            std::string llvm_ir;
            llvm::raw_string_ostream sstr(llvm_ir);
            M->print(sstr, 0);
            sstr.flush();

            llvm::sys::fs::create_directories(cache_dir.str());
            std::string filename = cache_dir.str().str() + "/" + module_identifier + "_first.ll";
            if (llvm::sys::fs::exists(filename))
                filename = cache_dir.str().str() + "/" + module_identifier + "_second.ll";
            FILE* f = fopen(filename.c_str(), "wt");
            ASSERT(f, "%s", strerror(errno));
            fwrite(llvm_ir.c_str(), 1, llvm_ir.size(), f);
            fclose(f);
#endif

            // This file isn't in our cache
            jit_objectcache_misses.log();
            return NULL;
        }

        std::unique_ptr<llvm::MemoryBuffer> mem_buff = CompressedFile::getFile(cache_file);
        if (!mem_buff) {
            jit_objectcache_misses.log();
            return NULL;
        }

        jit_objectcache_hits.log();
        return mem_buff;
    }

    void cleanupCacheDirectory() {
        // Find all files inside the cache directory, if the number of files is larger than
        // MAX_OBJECT_CACHE_ENTRIES,
        // sort them by last modification time and remove the oldest excessive ones.
        typedef std::pair<std::string, llvm::sys::TimeValue> CacheFileEntry;
        std::vector<CacheFileEntry> cache_files;

        std::error_code ec;
        for (llvm::sys::fs::directory_iterator file(cache_dir.str(), ec), end; !ec && file != end; file.increment(ec)) {
            llvm::sys::fs::file_status status;
            if (file->status(status))
                continue; // ignore files where we can't retrieve the file status.
            cache_files.emplace_back(std::make_pair(file->path(), status.getLastModificationTime()));
        }

        int num_expired = cache_files.size() - MAX_OBJECT_CACHE_ENTRIES;
        if (num_expired <= 0)
            return;

        std::stable_sort(cache_files.begin(), cache_files.end(),
                         [](const CacheFileEntry& lhs, const CacheFileEntry& rhs) { return lhs.second < rhs.second; });

        for (int i = 0; i < num_expired; ++i)
            llvm::sys::fs::remove(cache_files[i].first);
    }
};

static void handle_sigusr1(int signum) {
    assert(signum == SIGUSR1);
    fprintf(stderr, "SIGUSR1, printing stack trace\n");
    _printStacktrace();
}

#if ENABLE_SAMPLING_PROFILER
int sigprof_pending = 0;

static void handle_sigprof(int signum) {
    sigprof_pending++;
}
#endif

//#define INVESTIGATE_STAT_TIMER "us_timer_in_jitted_code"
#ifdef INVESTIGATE_STAT_TIMER
static_assert(STAT_TIMERS, "Stat timers need to be enabled to investigate them");
static uint64_t* stat_counter = Stats::getStatCounter(INVESTIGATE_STAT_TIMER);
static void handle_sigprof_investigate_stattimer(int signum) {
    if (StatTimer::getCurrentCounter() == stat_counter)
        raise(SIGTRAP);
}
#endif

static void handle_sigint(int signum) {
    assert(signum == SIGINT);
    // TODO: this should set a flag saying a KeyboardInterrupt is pending.
    // For now, just call abort(), so that we get a traceback at least.
    fprintf(stderr, "SIGINT!\n");
    joinRuntime();
    Stats::dump(false);
    abort();
}

void initCodegen() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    g.stdlib_module = loadStdlib();

#if LLVMREV < 215967
    llvm::EngineBuilder eb(new llvm::Module("empty_initial_module", g.context));
#else
    llvm::EngineBuilder eb(std::unique_ptr<llvm::Module>(new llvm::Module("empty_initial_module", g.context)));
#endif

#if LLVMREV < 216982
    eb.setUseMCJIT(true);
#endif

    eb.setEngineKind(llvm::EngineKind::JIT); // specify we only want the JIT, and not the interpreter fallback
#if LLVMREV < 223183
    eb.setMCJITMemoryManager(createMemoryManager().release());
#else
    eb.setMCJITMemoryManager(createMemoryManager());
#endif
    // eb.setOptLevel(llvm::CodeGenOpt::None); // -O0
    // eb.setOptLevel(llvm::CodeGenOpt::Less); // -O1
    // eb.setOptLevel(llvm::CodeGenOpt::Default); // -O2, -Os
    // eb.setOptLevel(llvm::CodeGenOpt::Aggressive); // -O3

    llvm::TargetOptions target_options;
    target_options.NoFramePointerElim = true;
    // target_options.EnableFastISel = true;
    eb.setTargetOptions(target_options);

    // TODO enable this?  should let us get better code:
    // eb.setMCPU(llvm::sys::getHostCPUName());

    g.tm = eb.selectTarget();
    assert(g.tm && "failed to get a target machine");
    g.engine = eb.create(g.tm);
    assert(g.engine && "engine creation failed?");

    if (ENABLE_JIT_OBJECT_CACHE)
        g.engine->setObjectCache(new PystonObjectCache());

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
#if LLVMREV < 216983
        llvm::JITEventListener* listener = new DisassemblerJITEventListener();
        g.jit_listeners.push_back(listener);
        g.engine->RegisterJITEventListener(listener);
#else
        fprintf(stderr, "The LLVM disassembler has been removed\n");
        abort();
#endif
    }

    initGlobalFuncs(g);

    setupRuntime();

    // signal(SIGFPE, &handle_sigfpe);
    signal(SIGUSR1, &handle_sigusr1);
    signal(SIGINT, &handle_sigint);

#if ENABLE_SAMPLING_PROFILER
    struct itimerval prof_timer;
    prof_timer.it_value.tv_sec = prof_timer.it_interval.tv_sec = 0;
    prof_timer.it_value.tv_usec = prof_timer.it_interval.tv_usec = 1000;
    signal(SIGPROF, handle_sigprof);
    setitimer(ITIMER_PROF, &prof_timer, NULL);
#endif

#ifdef INVESTIGATE_STAT_TIMER
    struct itimerval prof_timer;
    prof_timer.it_value.tv_sec = prof_timer.it_interval.tv_sec = 0;
    prof_timer.it_value.tv_usec = prof_timer.it_interval.tv_usec = 1000;
    signal(SIGPROF, handle_sigprof_investigate_stattimer);
    setitimer(ITIMER_PROF, &prof_timer, NULL);
#endif

    // There are some parts of llvm that are only configurable through command line args,
    // so construct a fake argc/argv pair and pass it to the llvm command line machinery:
    std::vector<const char*> llvm_args = { "fake_name" };

    llvm_args.push_back("--enable-patchpoint-liveness");
    if (0) {
        // Enabling and debugging fast-isel:
        // llvm_args.push_back("--fast-isel");
        // llvm_args.push_back("--fast-isel-verbose");
        ////llvm_args.push_back("--fast-isel-abort");
    }

#ifndef NDEBUG
// llvm_args.push_back("--debug-only=debug-ir");
// llvm_args.push_back("--debug-only=regalloc");
// llvm_args.push_back("--debug-only=stackmaps");
#endif

    // llvm_args.push_back("--time-passes");

    // llvm_args.push_back("--print-after-all");
    // llvm_args.push_back("--print-machineinstrs");
    if (USE_REGALLOC_BASIC)
        llvm_args.push_back("--regalloc=basic");

    llvm::cl::ParseCommandLineOptions(llvm_args.size(), &llvm_args[0], "<you should never see this>\n");
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
