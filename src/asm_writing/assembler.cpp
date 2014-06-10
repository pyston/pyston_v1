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

#include "asm_writing/assembler.h"

#include <cstring>

#include "core/common.h"

namespace pyston {
namespace assembler {

const char* regnames[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

void Register::dump() const {
    printf("%s\n", regnames[regnum]);
}

const int dwarf_to_gp[] = {
    // http://www.x86-64.org/documentation/abi.pdf#page=57
    0,  // 0
    2,  // 1
    1,  // 2 -> rcx
    3,  // 3 -> rbx
    6,  // 4
    7,  // 5
    5,  // 6 -> rbp
    4,  // 7
    8,  // 8 -> r8
    9,  // 9 -> r9
    10, // 10 -> r10
    11, // 11 -> r11
    12, // 12 -> r12
    13, // 13 -> r13
    14, // 14 -> r14
    15, // 15 -> r15

    // Others:
    // 16 -> ReturnAddress RA (??)
    // 17-32: xmm0-xmm15
};

GenericRegister GenericRegister::fromDwarf(int dwarf_regnum) {
    assert(dwarf_regnum >= 0);

    if (dwarf_regnum < 16) {
        return GenericRegister(Register(dwarf_to_gp[dwarf_regnum]));
    }

    if (17 <= dwarf_regnum && dwarf_regnum <= 32) {
        return GenericRegister(XMMRegister(dwarf_regnum - 17));
    }

    abort();
}



void Assembler::emitArith(Immediate imm, Register r, int opcode) {
    // assert(r != RSP && "This breaks unwinding, please don't use.");

    int64_t amount = imm.val;
    RELEASE_ASSERT((-1L << 31) <= amount && amount < (1L << 31) - 1, "");
    assert(0 <= opcode && opcode < 8);

    int rex = REX_W;

    int reg_idx = r.regnum;
    if (reg_idx >= 8) {
        rex |= REX_B;
        reg_idx -= 8;
    }

    emitRex(rex);
    if (-0x80 <= amount && amount < 0x80) {
        emitByte(0x83);
        emitModRM(0b11, opcode, reg_idx);
        emitByte(amount);
    } else {
        emitByte(0x81);
        emitModRM(0b11, opcode, reg_idx);
        emitInt(amount, 4);
    }
}


void Assembler::emitByte(uint8_t b) {
    assert(addr < end_addr);
    *addr = b;
    ++addr;
}

void Assembler::emitInt(int64_t n, int bytes) {
    assert(bytes > 0 && bytes <= 8);
    assert((-1L << (8 * bytes - 1)) <= n && n <= ((1L << (8 * bytes - 1)) - 1));
    for (int i = 0; i < bytes; i++) {
        emitByte(n & 0xff);
        n >>= 8;
    }
    ASSERT(n == 0 || n == -1, "%ld", n);
}

void Assembler::emitRex(uint8_t rex) {
    emitByte(rex | 0x40);
}

void Assembler::emitModRM(uint8_t mod, uint8_t reg, uint8_t rm) {
    assert(mod < 4);
    assert(reg < 8);
    assert(rm < 8);
    emitByte((mod << 6) | (reg << 3) | rm);
}

void Assembler::emitSIB(uint8_t scalebits, uint8_t index, uint8_t base) {
    assert(scalebits < 4);
    assert(index < 8);
    assert(base < 8);
    emitByte((scalebits << 6) | (index << 3) | base);
}



void Assembler::mov(Immediate val, Register dest) {
    int rex = REX_W;

    int dest_idx = dest.regnum;
    if (dest_idx >= 8) {
        rex |= REX_B;
        dest_idx -= 8;
    }

    emitRex(rex);
    emitByte(0xb8 + dest_idx);
    emitInt(val.val, 8);
}

void Assembler::movq(Immediate src, Indirect dest) {
    int64_t src_val = src.val;
    assert((-1L << 31) <= src_val && src_val < (1L << 31) - 1);

    int rex = REX_W;

    int dest_idx = dest.base.regnum;

    if (dest_idx >= 8) {
        rex |= REX_B;
        dest_idx -= 8;
    }

    emitRex(rex);
    emitByte(0xc7);

    bool needssib = (dest_idx == 0b100);

    int mode;
    if (dest.offset == 0)
        mode = 0b00;
    else if (-0x80 <= dest.offset && dest.offset < 0x80)
        mode = 0b01;
    else
        mode = 0b10;

    emitModRM(mode, 0, dest_idx);

    if (needssib)
        emitSIB(0b00, 0b100, dest_idx);

    if (mode == 0b01) {
        emitByte(dest.offset);
    } else if (mode == 0b10) {
        emitInt(dest.offset, 4);
    }

    emitInt(src_val, 4);
}

void Assembler::mov(Register src, Register dest) {
    ASSERT(src != dest, "probably better to avoid calling this?");

    int src_idx = src.regnum;
    int dest_idx = dest.regnum;

    uint8_t rex = REX_W;
    if (dest_idx >= 8) {
        rex |= REX_B;
        dest_idx -= 8;
    }
    if (src_idx >= 8) {
        rex |= REX_R;
        src_idx -= 8;
    }

    assert(0 <= src_idx && src_idx < 8);
    assert(0 <= dest_idx && dest_idx < 8);

    emitRex(rex);
    emitByte(0x89);
    emitModRM(0b11, src_idx, dest_idx);
}

void Assembler::mov(Register src, Indirect dest) {
    int rex = REX_W;

    int src_idx = src.regnum;
    int dest_idx = dest.base.regnum;

    assert(src_idx != dest_idx && "while valid this is almost certainly a register allocator bug");

    if (src_idx >= 8) {
        rex |= REX_R;
        src_idx -= 8;
    }
    if (dest_idx >= 8) {
        rex |= REX_B;
        dest_idx -= 8;
    }

    emitRex(rex);
    emitByte(0x89);

    bool needssib = (dest_idx == 0b100);

    int mode;
    if (dest.offset == 0)
        mode = 0b00;
    else if (-0x80 <= dest.offset && dest.offset < 0x80)
        mode = 0b01;
    else
        mode = 0b10;

    emitModRM(mode, src_idx, dest_idx);

    if (needssib)
        emitSIB(0b00, 0b100, dest_idx);

    if (mode == 0b01) {
        emitByte(dest.offset);
    } else if (mode == 0b10) {
        emitInt(dest.offset, 4);
    }
}

void Assembler::mov(Indirect src, Register dest) {
    int rex = REX_W;

    int src_idx = src.base.regnum;
    int dest_idx = dest.regnum;

    if (src_idx >= 8) {
        rex |= REX_B;
        src_idx -= 8;
    }
    if (dest_idx >= 8) {
        rex |= REX_R;
        dest_idx -= 8;
    }

    emitRex(rex);
    emitByte(0x8b); // opcode

    bool needssib = (src_idx == 0b100);

    int mode;
    if (src.offset == 0)
        mode = 0b00;
    else if (-0x80 <= src.offset && src.offset < 0x80)
        mode = 0b01;
    else
        mode = 0b10;

    emitModRM(mode, dest_idx, src_idx);

    if (needssib)
        emitSIB(0b00, 0b100, src_idx);

    if (mode == 0b01) {
        emitByte(src.offset);
    } else if (mode == 0b10) {
        emitInt(src.offset, 4);
    }
}

void Assembler::movsd(XMMRegister src, XMMRegister dest) {
    int rex = 0;
    int src_idx = src.regnum;
    int dest_idx = dest.regnum;

    if (src_idx >= 8) {
        trap();
        rex |= REX_R;
        src_idx -= 8;
    }
    if (dest_idx >= 8) {
        trap();
        rex |= REX_B;
        dest_idx -= 8;
    }

    emitByte(0xf2);
    if (rex)
        emitRex(rex);
    emitByte(0x0f);
    emitByte(0x10);

    emitModRM(0b11, src_idx, dest_idx);
}

void Assembler::movsd(XMMRegister src, Indirect dest) {
    int rex = 0;
    int src_idx = src.regnum;
    int dest_idx = dest.base.regnum;

    if (src_idx >= 8) {
        trap();
        rex |= REX_R;
        src_idx -= 8;
    }
    if (dest_idx >= 8) {
        trap();
        rex |= REX_B;
        dest_idx -= 8;
    }

    emitByte(0xf2);
    if (rex)
        emitRex(rex);
    emitByte(0x0f);
    emitByte(0x11);

    bool needssib = (dest_idx == 0b100);

    int mode;
    if (dest.offset == 0)
        mode = 0b00;
    else if (-0x80 <= dest.offset && dest.offset < 0x80)
        mode = 0b01;
    else
        mode = 0b10;

    emitModRM(mode, src_idx, dest_idx);

    if (needssib)
        emitSIB(0b00, 0b100, dest_idx);

    if (mode == 0b01) {
        emitByte(dest.offset);
    } else if (mode == 0b10) {
        emitInt(dest.offset, 4);
    }
}

void Assembler::movsd(Indirect src, XMMRegister dest) {
    int rex = 0;
    int src_idx = src.base.regnum;
    int dest_idx = dest.regnum;

    if (src_idx >= 8) {
        trap();
        rex |= REX_R;
        src_idx -= 8;
    }
    if (dest_idx >= 8) {
        trap();
        rex |= REX_B;
        dest_idx -= 8;
    }

    emitByte(0xf2);
    if (rex)
        emitRex(rex);
    emitByte(0x0f);
    emitByte(0x10);

    bool needssib = (src_idx == 0b100);

    int mode;
    if (src.offset == 0)
        mode = 0b00;
    else if (-0x80 <= src.offset && src.offset < 0x80)
        mode = 0b01;
    else
        mode = 0b10;

    emitModRM(mode, dest_idx, src_idx);

    if (needssib)
        emitSIB(0b00, 0b100, src_idx);

    if (mode == 0b01) {
        emitByte(src.offset);
    } else if (mode == 0b10) {
        emitInt(src.offset, 4);
    }
}


void Assembler::push(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    int reg_idx = reg.regnum;
    if (reg_idx >= 8) {
        emitRex(REX_B);
        reg_idx -= 8;
    }
    assert(reg_idx < 8);

    emitByte(0x50 + reg_idx);
}

void Assembler::pop(Register reg) {
    // assert(0 && "This breaks unwinding, please don't use.");

    assert(reg != RSP); // this might work but most likely a bug

    int reg_idx = reg.regnum;
    if (reg_idx >= 8) {
        emitRex(REX_B);
        reg_idx -= 8;
    }
    assert(reg_idx < 8);

    emitByte(0x58 + reg_idx);
}



void Assembler::add(Immediate imm, Register reg) {
    emitArith(imm, reg, OPCODE_ADD);
}

void Assembler::sub(Immediate imm, Register reg) {
    emitArith(imm, reg, OPCODE_SUB);
}

void Assembler::inc(Register reg) {
    UNIMPLEMENTED();
}

void Assembler::inc(Indirect mem) {
    UNIMPLEMENTED();
}



void Assembler::callq(Register r) {
    assert(r == R11 && "untested");

    emitRex(REX_B);
    emitByte(0xff);
    emitByte(0xd3);
}



void Assembler::cmp(Register reg1, Register reg2) {
    int reg1_idx = reg1.regnum;
    int reg2_idx = reg2.regnum;

    int rex = REX_W;
    if (reg1_idx >= 8) {
        rex |= REX_R;
        reg1_idx -= 8;
    }
    if (reg2_idx >= 8) {
        trap();
        rex |= REX_B;
        reg2_idx -= 8;
    }

    assert(reg1_idx >= 0 && reg1_idx < 8);
    assert(reg2_idx >= 0 && reg2_idx < 8);

    emitRex(rex);
    emitByte(0x39);
    emitModRM(0b11, reg1_idx, reg2_idx);
}

void Assembler::cmp(Register reg, Immediate imm) {
    int64_t val = imm.val;
    assert((-1L << 31) <= val && val < (1L << 31) - 1);

    int reg_idx = reg.regnum;

    int rex = REX_W;
    if (reg_idx > 8) {
        rex |= REX_B;
        reg_idx -= 8;
    }
    assert(0 <= reg_idx && reg_idx < 8);

    emitRex(rex);
    emitByte(0x81);
    emitModRM(0b11, 7, reg_idx);
    emitInt(val, 4);
}

void Assembler::cmp(Indirect mem, Immediate imm) {
    int64_t val = imm.val;
    assert((-1L << 31) <= val && val < (1L << 31) - 1);

    int src_idx = mem.base.regnum;

    int rex = REX_W;
    if (src_idx >= 8) {
        rex |= REX_B;
        src_idx -= 8;
    }

    assert(src_idx >= 0 && src_idx < 8);

    emitRex(rex);
    emitByte(0x81);

    assert(-0x80 <= mem.offset && mem.offset < 0x80);
    if (mem.offset == 0) {
        emitModRM(0b00, 7, src_idx);
    } else {
        emitModRM(0b01, 7, src_idx);
        emitByte(mem.offset);
    }

    emitInt(val, 4);
}

void Assembler::cmp(Indirect mem, Register reg) {
    int mem_idx = mem.base.regnum;
    int reg_idx = reg.regnum;

    int rex = REX_W;
    if (mem_idx >= 8) {
        rex |= REX_B;
        mem_idx -= 8;
    }
    if (reg_idx >= 8) {
        rex |= REX_R;
        reg_idx -= 8;
    }

    assert(mem_idx >= 0 && mem_idx < 8);
    assert(reg_idx >= 0 && reg_idx < 8);

    emitRex(rex);
    emitByte(0x3B);

    assert(-0x80 <= mem.offset && mem.offset < 0x80);
    if (mem.offset == 0) {
        emitModRM(0b00, reg_idx, mem_idx);
    } else {
        emitModRM(0b01, reg_idx, mem_idx);
        emitByte(mem.offset);
    }
}

void Assembler::test(Register reg1, Register reg2) {
    int reg1_idx = reg1.regnum;
    int reg2_idx = reg2.regnum;

    int rex = REX_W;
    if (reg1_idx >= 8) {
        rex |= REX_R;
        reg1_idx -= 8;
    }
    if (reg2_idx >= 8) {
        trap();
        rex |= REX_B;
        reg2_idx -= 8;
    }

    assert(reg1_idx >= 0 && reg1_idx < 8);
    assert(reg2_idx >= 0 && reg2_idx < 8);

    emitRex(rex);
    emitByte(0x85);
    emitModRM(0b11, reg1_idx, reg2_idx);
}



void Assembler::jmp_cond(JumpDestination dest, ConditionCode condition) {
    bool unlikely = false;

    assert(dest.type == JumpDestination::FROM_START);
    int offset = dest.offset - (addr - start_addr) - 2;
    if (unlikely)
        offset--;

    if (offset >= -0x80 && offset < 0x80) {
        if (unlikely)
            emitByte(0x2e);

        emitByte(0x70 | condition);
        emitByte(offset);
    } else {
        offset -= 4;

        if (unlikely)
            emitByte(0x2e);

        emitByte(0x0f);
        emitByte(0x80 | condition);
        emitInt(offset, 4);
    }
}

void Assembler::jmp(JumpDestination dest) {
    assert(dest.type == JumpDestination::FROM_START);
    int offset = dest.offset - (addr - start_addr) - 2;

    if (offset >= -0x80 && offset < 0x80) {
        emitByte(0xeb);
        emitByte(offset);
    } else {
        offset -= 3;
        emitByte(0xe9);
        emitInt(offset, 4);
    }
}

void Assembler::jne(JumpDestination dest) {
    jmp_cond(dest, COND_NOT_EQUAL);
}

void Assembler::je(JumpDestination dest) {
    jmp_cond(dest, COND_EQUAL);
}



void Assembler::set_cond(Register reg, ConditionCode condition) {
    int reg_idx = reg.regnum;

    assert(0 <= reg_idx && reg_idx < 8);

    int rex = 0;
    // Have to emit a blank REX when accessing RSP/RBP/RDI/RSI,
    // since without it this instruction will refer to ah/bh/ch/dh.
    if (reg_idx >= 4 || rex)
        emitRex(rex);

    emitByte(0x0f);
    emitByte(0x90 + condition);
    emitModRM(0b11, 0, reg_idx);
}

void Assembler::sete(Register reg) {
    set_cond(reg, COND_EQUAL);
}

void Assembler::setne(Register reg) {
    set_cond(reg, COND_NOT_EQUAL);
}

uint8_t* Assembler::emitCall(void* ptr, Register scratch) {
    mov(Immediate(ptr), scratch);
    callq(scratch);
    return addr;
}

void Assembler::emitBatchPush(StackInfo stack_info, const std::vector<GenericRegister>& to_push) {
    assert(stack_info.has_scratch);
    int offset = 0;

    for (const GenericRegister& r : to_push) {
        assert(stack_info.scratch_bytes >= offset + 8);
        Indirect next_slot(RBP, offset + stack_info.scratch_rbp_offset);

        if (r.type == GenericRegister::GP) {
            Register gp = r.gp;
            assert(gp.regnum >= 0 && gp.regnum < 16);
            mov(gp, next_slot);
            offset += 8;
        } else if (r.type == GenericRegister::XMM) {
            XMMRegister reg = r.xmm;
            movsd(reg, next_slot);
            offset += 8;
        } else {
            RELEASE_ASSERT(0, "%d", r.type);
        }
    }
}

void Assembler::emitBatchPop(StackInfo stack_info, const std::vector<GenericRegister>& to_push) {
    assert(stack_info.has_scratch);
    int offset = 0;

    for (const GenericRegister& r : to_push) {
        assert(stack_info.scratch_bytes >= offset + 8);
        Indirect next_slot(RBP, offset + stack_info.scratch_rbp_offset);

        if (r.type == GenericRegister::GP) {
            Register gp = r.gp;
            assert(gp.regnum >= 0 && gp.regnum < 16);
            mov(next_slot, gp);
            offset += 8;
        } else if (r.type == GenericRegister::XMM) {
            XMMRegister reg = r.xmm;
            movsd(next_slot, reg);
            offset += 8;
        } else {
            RELEASE_ASSERT(0, "%d", r.type);
        }
    }
}

void Assembler::fillWithNops() {
    assert(addr <= end_addr);
    memset(addr, 0x90, end_addr - addr);
    addr = end_addr;
}

void Assembler::fillWithNopsExcept(int bytes) {
    assert(end_addr - addr >= bytes);
    memset(addr, 0x90, end_addr - addr - bytes);
    addr = end_addr - bytes;
}

void Assembler::emitAnnotation(int num) {
    nop();
    cmp(RAX, Immediate(num));
    nop();
}



uint8_t* initializePatchpoint2(uint8_t* start_addr, uint8_t* slowpath_start, uint8_t* end_addr, StackInfo stack_info,
                               const std::unordered_set<int>& live_outs) {
    assert(start_addr < slowpath_start);
    static const int INITIAL_CALL_SIZE = 13;
    assert(end_addr > slowpath_start + INITIAL_CALL_SIZE);
#ifndef NDEBUG
    // if (VERBOSITY()) printf("initializing patchpoint at %p - %p\n", addr, addr + size);
    // for (int i = 0; i < size; i++) {
    // printf("%02x ", *(addr + i));
    //}
    // printf("\n");

    // Check the exact form of the patchpoint call.
    // It's important to make sure that the only live registers
    // are the ones that are used as arguments; ie it wouldn't
    // matter if the call happened on %r10 instead of %r11,
    // but it would matter if there wasn't a mov immediately before
    // the call, since then %r11 would be live and we couldn't
    // use it as a temporary.

    // mov $imm, %r11:
    ASSERT(start_addr[0] == 0x49, "%x", start_addr[0]);
    assert(start_addr[1] == 0xbb);
    // 8 bytes of the addr

    // callq *%r11:
    assert(start_addr[10] == 0x41);
    assert(start_addr[11] == 0xff);
    assert(start_addr[12] == 0xd3);

    int i = INITIAL_CALL_SIZE;
    while (*(start_addr + i) == 0x66 || *(start_addr + i) == 0x0f || *(start_addr + i) == 0x2e)
        i++;
    assert(*(start_addr + i) == 0x90 || *(start_addr + i) == 0x1f);
#endif

    void* call_addr = *(void**)&start_addr[2];

    Assembler(start_addr, slowpath_start - start_addr).fillWithNops();

    std::vector<GenericRegister> regs_to_spill;
    for (int dwarf_regnum : live_outs) {
        GenericRegister ru = GenericRegister::fromDwarf(dwarf_regnum);

        if (ru.type == GenericRegister::GP) {
            if (ru.gp == RSP || ru.gp.isCalleeSave())
                continue;
        }

        regs_to_spill.push_back(ru);
    }

    Assembler assem(slowpath_start, end_addr - slowpath_start);

    // if (regs_to_spill.size())
    // assem.trap();
    assem.emitBatchPush(stack_info, regs_to_spill);
    uint8_t* rtn = assem.emitCall(call_addr, R11);
    assem.emitBatchPop(stack_info, regs_to_spill);
    assem.fillWithNops();

    return rtn;
}
}
}
