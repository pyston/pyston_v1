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

#include "runtime/ics.h"

#include <sys/mman.h>

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "codegen/compvars.h"
#include "codegen/memmgr.h"
#include "codegen/patchpoints.h"
#include "codegen/stackmaps.h"
#include "codegen/unwinding.h" // registerDynamicEhFrame
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

#ifndef NVALGRIND
#define PAGE_SIZE 4096
#endif

namespace pyston {

// I am not sure if we should use the equivalent of -fomit-frame-pointer for these trampolines.
// If we don't use it, we break gdb backtraces.
// If we do use it, we have an inconsistency with the rest of the code, which runs with -fno-omit-frame-pointer.
//
// I haven't detected anything go that wrong, so let's just enable it for now.
#define RUNTIMEICS_OMIT_FRAME_PTR 1


// Useful links for understanding the eh_frame format:
// - http://www.dwarfstd.org/doc/Dwarf3.pdf
// - https://refspecs.linuxfoundation.org/LSB_3.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
// - Generating with clang then "readelf -w"
//
// This template is generated from this C++ file:
//
// extern void foo(void*);
// int bar() {
//   char buf[N];
//   foo(&buf);
//   return 1;
// }
//
// (where N is the extra bytes of stack to allocate)
//
// objdump -s -j .eh_frame test
// readelf -w test
//


// clang++ test.cpp -o test -O3 -fomit-frame-pointer -c -DN=40
// The generated assembly is:
//
//  0:   48 83 ec 28             sub    $0x28,%rsp
//  4:   48 8d 3c 24             lea    (%rsp),%rdi
//  8:   e8 00 00 00 00          callq  d <_Z3barv+0xd>
//  d:   b8 01 00 00 00          mov    $0x1,%eax
// 12:   48 83 c4 28             add    $0x28,%rsp
// 16:   c3                      retq
//
//  (I believe the push/pop are for stack alignment)
//
static const char _eh_frame_template_ofp[] =
    // CIE
    "\x14\x00\x00\x00" // size of the CIE
    "\x00\x00\x00\x00" // specifies this is an CIE
    "\x03"             // version number
    "\x7a\x52\x00"     // augmentation string "zR"
    "\x01\x78\x10"     // code factor 1, data factor -8, return address 16
    "\x01\x1b"         // augmentation data: 1b (CIE pointers as 4-byte-signed pcrel values)
    "\x0c\x07\x08\x90\x01\x00\x00"
    // Instructions:
    // - DW_CFA_def_cfa: r7 (rsp) ofs 8
    // - DW_CFA_offset: r16 (rip) at cfa-8
    // - nop, nop

    // FDE:
    "\x14\x00\x00\x00" // size of the FDE
    "\x1c\x00\x00\x00" // offset to the CIE
    "\x00\x00\x00\x00" // prcel offset to function address [to be filled in]
    "\x0d\x00\x00\x00" // function size [to be filled in]
    "\x00"             // augmentation data (none)
    "\x44\x0e\x30"
    // Instructions:
    // - DW_CFA_advance_loc: 4 to 00000004
    // - DW_CFA_def_cfa_offset: 48
    "\x00\x00\x00\x00" // padding

    "\x00\x00\x00\x00" // terminator
    ;

// clang++ test.cpp -o test -O3 -fno-omit-frame-pointer -c -DN=40
// The generated assembly is:
//  0:   55                      push   %rbp
//  1:   48 89 e5                mov    %rsp,%rbp
//  4:   48 83 ec 30             sub    $0x30,%rsp
//  8:   48 8d 7d d0             lea    -0x30(%rbp),%rdi
//  c:   e8 00 00 00 00          callq  11 <_Z3barv+0x11>
// 11:   b8 01 00 00 00          mov    $0x1,%eax
// 16:   48 83 c4 30             add    $0x30,%rsp
// 1a:   5d                      pop    %rbp
// 1b:   c3                      retq
//
static const char _eh_frame_template_fp[] =
    // CIE
    "\x14\x00\x00\x00" // size of the CIE
    "\x00\x00\x00\x00" // specifies this is an CIE
    "\x03"             // version number
    "\x7a\x52\x00"     // augmentation string "zR"
    "\x01\x78\x10"     // code factor 1, data factor -8, return address 16
    "\x01\x1b"         // augmentation data: 1b (CIE pointers as 4-byte-signed pcrel values)
    "\x0c\x07\x08\x90\x01\x00\x00"
    // Instructions:
    // - DW_CFA_def_cfa: r7 (rsp) ofs 8
    // - DW_CFA_offset: r16 (rip) at cfa-8
    // - nop, nop

    // FDE:
    "\x1c\x00\x00\x00" // size of the FDE
    "\x1c\x00\x00\x00" // offset to the CIE
    "\x00\x00\x00\x00" // prcel offset to function address [to be filled in]
    "\x10\x00\x00\x00" // function size [to be filled in]
    "\x00"             // augmentation data (none)
    "\x41\x0e\x10\x86\x02\x43\x0d\x06"
    // Instructions:
    // - DW_CFA_advance_loc: 1 to 00000001
    // - DW_CFA_def_cfa_offset: 16
    // - DW_CFA_offset: r6 (rbp) at cfa-16
    // - DW_CFA_advance_loc: 3 to 00000004
    // - DW_CFA_def_cfa_register: r6 (rbp)
    // - nops
    "\x00\x00\x00\x00\x00\x00\x00" // padding

    "\x00\x00\x00\x00" // terminator
    ;

static constexpr int _eh_frame_template_ofp_size = sizeof(_eh_frame_template_ofp) - 1;
static constexpr int _eh_frame_template_fp_size = sizeof(_eh_frame_template_fp) - 1;

#if RUNTIMEICS_OMIT_FRAME_PTR
#define EH_FRAME_SIZE _eh_frame_template_ofp_size
#else
#define EH_FRAME_SIZE _eh_frame_template_fp_size;
#endif


static_assert(sizeof("") == 1, "strings are null-terminated");

static void writeTrivialEhFrame(void* eh_frame_addr, void* func_addr, uint64_t func_size, bool omit_frame_pointer) {
    if (omit_frame_pointer)
        memcpy(eh_frame_addr, _eh_frame_template_ofp, _eh_frame_template_ofp_size);
    else
        memcpy(eh_frame_addr, _eh_frame_template_fp, _eh_frame_template_fp_size);

    int32_t* offset_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x20);
    int32_t* size_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x24);

    int64_t offset = (int8_t*)func_addr - (int8_t*)offset_ptr;
    RELEASE_ASSERT(offset >= INT_MIN && offset <= INT_MAX, "");
    *offset_ptr = offset;

    assert(func_size <= UINT_MAX);
    *size_ptr = func_size;
}

#if RUNTIMEICS_OMIT_FRAME_PTR
// If you change this, you *must* update the value in _eh_frame_template
// (set the -9'th byte to this value plus 8)
#define SCRATCH_BYTES 0x28
#else
#define SCRATCH_BYTES 0x30
#endif

template <int chunk_size> class RuntimeICMemoryManager {
private:
    static constexpr int region_size = 4096;
    static_assert(chunk_size < region_size, "");
    static_assert(region_size % chunk_size == 0, "");

    std::vector<void*> memory_regions;
    llvm::SmallVector<void*, region_size / chunk_size> free_chunks;

public:
    void* alloc() {
        if (free_chunks.empty()) {
            int protection = PROT_READ | PROT_WRITE | PROT_EXEC;
            int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT;
            char* addr = (char*)mmap(NULL, region_size, protection, flags, -1, 0);
            for (int i = 0; i < region_size / chunk_size; ++i) {
                free_chunks.push_back(&addr[i * chunk_size]);
            }
        }
        return free_chunks.pop_back_val();
    }
    void dealloc(void* ptr) {
        free_chunks.push_back(ptr); // TODO: we should probably delete some regions if this list gets to large
    }
};

static RuntimeICMemoryManager<512> memory_manager_512b;

RuntimeIC::RuntimeIC(void* func_addr, int total_size) {
    static StatCounter sc("num_runtime_ics");
    sc.log();

    if (ENABLE_RUNTIME_ICS) {
        assert(SCRATCH_BYTES >= 0);
        assert(SCRATCH_BYTES < 0x80); // This would break both the instruction encoding and the dwarf encoding
        assert(SCRATCH_BYTES % 8 == 0);

#if RUNTIMEICS_OMIT_FRAME_PTR
        /*
         * prologue:
         * sub $0x28, %rsp  # 48 83 ec 28
         *
         * epilogue:
         * add $0x28, %rsp  # 48 83 c4 28
         * retq             # c3
         *
         */
        static const int PROLOGUE_SIZE = 4;
        static const int EPILOGUE_SIZE = 5;
        assert(SCRATCH_BYTES % 16 == 8);
#else
        /*
         * The prologue looks like:
         * push %rbp        # 55
         * mov %rsp, %rbp   # 48 89 e5
         * sub $0x30, %rsp  # 48 83 ec 30
         *
         * The epilogue is:
         * add $0x30, %rsp  # 48 83 c4 30
         * pop %rbp         # 5d
         * retq             # c3
         */
        static const int PROLOGUE_SIZE = 8;
        static const int EPILOGUE_SIZE = 6;
        assert(SCRATCH_BYTES % 16 == 0);
#endif
        static const int CALL_SIZE = 13;

        int total_code_size = total_size - EH_FRAME_SIZE;
        int patchable_size = total_code_size - (PROLOGUE_SIZE + CALL_SIZE + EPILOGUE_SIZE);

        int total_size = total_code_size + EH_FRAME_SIZE;
        assert(total_size == 512 && "we currently only have a 512 byte block memory manager");
        addr = memory_manager_512b.alloc();

        // the memory block contains the EH frame directly followed by the generated machine code.
        void* eh_frame_addr = addr;
        addr = (char*)addr + EH_FRAME_SIZE;

        // printf("Allocated runtime IC at %p\n", addr);

        std::unique_ptr<ICSetupInfo> setup_info(
            ICSetupInfo::initialize(true, patchable_size, ICSetupInfo::Generic, NULL));
        uint8_t* pp_start = (uint8_t*)addr + PROLOGUE_SIZE;
        uint8_t* pp_end = pp_start + patchable_size + CALL_SIZE;


        SpillMap _spill_map;
        PatchpointInitializationInfo initialization_info = initializePatchpoint3(
            func_addr, pp_start, pp_end, 0 /* scratch_offset */, 0 /* scratch_size */, LiveOutSet(), _spill_map);
        assert(_spill_map.size() == 0);
        assert(initialization_info.slowpath_start == pp_start + patchable_size);
        assert(initialization_info.slowpath_rtn_addr == pp_end);
        assert(initialization_info.continue_addr == pp_end);

        StackInfo stack_info(SCRATCH_BYTES, 0);
        icinfo = registerCompiledPatchpoint(pp_start, pp_start + patchable_size, pp_end, pp_end, setup_info.get(),
                                            stack_info, LiveOutSet());

        assembler::Assembler prologue_assem((uint8_t*)addr, PROLOGUE_SIZE);
#if RUNTIMEICS_OMIT_FRAME_PTR
        // If SCRATCH_BYTES is 8 or less, we could use more compact instruction encodings
        // (push instead of sub), but it doesn't seem worth it for now.
        prologue_assem.sub(assembler::Immediate(SCRATCH_BYTES), assembler::RSP);
#else
        prologue_assem.push(assembler::RBP);
        prologue_assem.mov(assembler::RSP, assembler::RBP);
        prologue_assem.sub(assembler::Immediate(SCRATCH_BYTES), assembler::RSP);
#endif
        assert(!prologue_assem.hasFailed());
        assert(prologue_assem.isExactlyFull());

        assembler::Assembler epilogue_assem(pp_end, EPILOGUE_SIZE);
#if RUNTIMEICS_OMIT_FRAME_PTR
        epilogue_assem.add(assembler::Immediate(SCRATCH_BYTES), assembler::RSP);
#else
        epilogue_assem.add(assembler::Immediate(SCRATCH_BYTES), assembler::RSP);
        epilogue_assem.pop(assembler::RBP);
#endif
        epilogue_assem.retq();
        assert(!epilogue_assem.hasFailed());
        assert(epilogue_assem.isExactlyFull());

        writeTrivialEhFrame(eh_frame_addr, addr, total_code_size, RUNTIMEICS_OMIT_FRAME_PTR);
        // (EH_FRAME_SIZE - 4) to omit the 4-byte null terminator, otherwise we trip an assert in parseEhFrame.
        // TODO: can we omit the terminator in general?
        registerDynamicEhFrame((uint64_t)addr, total_code_size, (uint64_t)eh_frame_addr, EH_FRAME_SIZE - 4);
        registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);
    } else {
        addr = func_addr;
    }
}

RuntimeIC::~RuntimeIC() {
    if (ENABLE_RUNTIME_ICS) {
        deregisterCompiledPatchpoint(icinfo.get());
        uint8_t* eh_frame_addr = (uint8_t*)addr - EH_FRAME_SIZE;
        deregisterEHFrames(eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);
        memory_manager_512b.dealloc(eh_frame_addr);
    } else {
    }
}
}
