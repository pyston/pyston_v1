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

#include "runtime/ics.h"

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "codegen/compvars.h"
#include "codegen/memmgr.h"
#include "codegen/patchpoints.h"
#include "codegen/stackmaps.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

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
// extern void foo();
// int bar() {
//   foo();
//   return 1;
// }
//
// objdump -s -j .eh_frame test
// readelf -w test
//

#if RUNTIMEICS_OMIT_FRAME_PTR
// clang++ test.cpp -o test -O3 -fomit-frame-pointer -c
// The generated assembly is:
//
//  0:   50                      push   %rax
//  1:   e8 00 00 00 00          callq  6 <_Z3barv+0x6>
//  6:   b8 01 00 00 00          mov    $0x1,%eax
//  b:   5a                      pop    %rdx
//  c:   c3                      retq
//
//  (I believe the push/pop are for stack alignment)
//
static const char _eh_frame_template[] =
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
    "\x41\x0e\x10"
    // Instructions:
    // - DW_CFA_advance_loc: 1 to 00000001
    // - DW_CFA_def_cfa_offset: 16
    "\x00\x00\x00\x00" // padding

    "\x00\x00\x00\x00" // terminator
    ;
#else
// clang++ test.cpp -o test -O3 -fno-omit-frame-pointer -c
// The generated assembly is:
//
//  0:   55                      push   %rbp
//  1:   48 89 e5                mov    %rsp,%rbp
//  4:   e8 00 00 00 00          callq  9 <_Z3barv+0x9>
//  9:   b8 01 00 00 00          mov    $0x1,%eax
//  e:   5d                      pop    %rbp
//  f:   c3                      retq
//
static const char _eh_frame_template[] =
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
#endif
#define EH_FRAME_SIZE sizeof(_eh_frame_template)

static void writeTrivialEhFrame(void* eh_frame_addr, void* func_addr, uint64_t func_size) {
    memcpy(eh_frame_addr, _eh_frame_template, sizeof(_eh_frame_template));

    int32_t* offset_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x20);
    int32_t* size_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x24);

    int64_t offset = (int8_t*)func_addr - (int8_t*)offset_ptr;
    assert(offset >= INT_MIN && offset <= INT_MAX);
    *offset_ptr = offset;

    assert(func_size <= UINT_MAX);
    *size_ptr = func_size;
}

RuntimeIC::RuntimeIC(void* func_addr, int num_slots, int slot_size) {
    static StatCounter sc("runtime_ics_num");
    sc.log();

    if (ENABLE_RUNTIME_ICS) {
#if RUNTIMEICS_OMIT_FRAME_PTR
        static const int PROLOGUE_SIZE = 1;
#else
        /*
         * We emit a prologue since we want to align the stack pointer,
         * and also use RBP.
         * It's not clear if we need to use RBP or not, since we emit the .eh_frame section anyway.
         *
         * The prologue looks like:
         * push %rbp # 55
         * mov %rsp, %rbp # 48 89 e5
         *
         * The epilogue is:
         * pop %rbp # 5d
         * retq # c3
         */
        static const int PROLOGUE_SIZE = 4;
#endif
        static const int CALL_SIZE = 13;
        static const int EPILOGUE_SIZE = 2;

        int patchable_size = num_slots * slot_size;
        int total_size = PROLOGUE_SIZE + patchable_size + CALL_SIZE + EPILOGUE_SIZE;
        addr = malloc(total_size);

        // printf("Allocated runtime IC at %p\n", addr);

        std::unique_ptr<ICSetupInfo> setup_info(
            ICSetupInfo::initialize(true, num_slots, slot_size, ICSetupInfo::Generic, NULL));
        uint8_t* pp_start = (uint8_t*)addr + PROLOGUE_SIZE;
        uint8_t* pp_end = pp_start + patchable_size + CALL_SIZE;


        SpillMap _spill_map;
        std::pair<uint8_t*, uint8_t*> p
            = initializePatchpoint3(func_addr, pp_start, pp_end, 0 /* scratch_offset */, 0 /* scratch_size */,
                                    std::unordered_set<int>(), _spill_map);
        assert(_spill_map.size() == 0);
        assert(p.first == pp_start + patchable_size);
        assert(p.second == pp_end);

        icinfo = registerCompiledPatchpoint(pp_start, pp_start + patchable_size, pp_end, pp_end, setup_info.get(),
                                            StackInfo(), std::unordered_set<int>());

        assembler::Assembler prologue_assem((uint8_t*)addr, PROLOGUE_SIZE);
#if RUNTIMEICS_OMIT_FRAME_PTR
        prologue_assem.push(assembler::RAX);
#else
        prologue_assem.push(assembler::RBP);
        prologue_assem.mov(assembler::RSP, assembler::RBP);
#endif
        assert(!prologue_assem.hasFailed());
        assert(prologue_assem.isExactlyFull());

        assembler::Assembler epilogue_assem(pp_end, EPILOGUE_SIZE);
#if RUNTIMEICS_OMIT_FRAME_PTR
        epilogue_assem.pop(assembler::RDX);
#else
        epilogue_assem.pop(assembler::RBP);
#endif
        epilogue_assem.retq();
        assert(!epilogue_assem.hasFailed());
        assert(epilogue_assem.isExactlyFull());

        // TODO: ideally would be more intelligent about allocation strategies.
        // The code sections should be together and the eh sections together
        eh_frame_addr = malloc(EH_FRAME_SIZE);
        writeTrivialEhFrame(eh_frame_addr, addr, total_size);
        registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);

    } else {
        addr = func_addr;
    }
}

RuntimeIC::~RuntimeIC() {
    if (ENABLE_RUNTIME_ICS) {
        deregisterCompiledPatchpoint(icinfo.get());
        deregisterEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);
        free(addr);
        free(eh_frame_addr);
    } else {
    }
}
}
