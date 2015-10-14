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

#include "asm_writing/assembler.h"

#include <cstring>

#include "core/common.h"
#include "core/options.h"

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
    0,  // 0 -> rax
    2,  // 1 -> rdx
    1,  // 2 -> rcx
    3,  // 3 -> rbx
    6,  // 4 -> rsi
    7,  // 5 -> rdi
    5,  // 6 -> rbp
    4,  // 7 -> rsp
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

Register Register::fromDwarf(int dwarf_regnum) {
    assert(dwarf_regnum >= 0 && dwarf_regnum <= 16);

    return Register(dwarf_to_gp[dwarf_regnum]);
}

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
    if (addr >= end_addr) {
        failed = true;
        return;
    }

    assert(addr < end_addr);
    *addr = b;
    ++addr;
}

void Assembler::emitInt(int64_t n, int bytes) {
    assert(bytes > 0 && bytes <= 8);
    if (bytes < 8)
        assert((-1L << (8 * bytes - 1)) <= n && n <= ((1L << (8 * bytes - 1)) - 1));

    for (int i = 0; i < bytes; i++) {
        emitByte(n & 0xff);
        n >>= 8;
    }
    ASSERT(n == 0 || n == -1, "%ld", n);
}

void Assembler::emitUInt(uint64_t n, int bytes) {
    assert(bytes > 0 && bytes <= 8);
    if (bytes < 8)
        assert(n < ((1UL << (8 * bytes))));

    for (int i = 0; i < bytes; i++) {
        emitByte(n & 0xff);
        n >>= 8;
    }
    ASSERT(n == 0, "%lu", n);
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

int Assembler::getModeFromOffset(int offset) const {
    if (offset == 0)
        return 0b00;
    else if (-0x80 <= offset && offset < 0x80)
        return 0b01;
    else
        return 0b10;
}

void Assembler::mov(Immediate val, Register dest, bool force_64bit_load) {
    force_64bit_load = force_64bit_load || !val.fitsInto32Bit();

    int rex = force_64bit_load ? REX_W : 0;
    int dest_idx = dest.regnum;
    if (dest_idx >= 8) {
        rex |= REX_B;
        dest_idx -= 8;
    }

    if (rex)
        emitRex(rex);
    emitByte(0xb8 + dest_idx);
    emitUInt(val.val, force_64bit_load ? 8 : 4);
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
    int mode = getModeFromOffset(dest.offset);
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
    int mode = getModeFromOffset(dest.offset);
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
    mov_generic(src, dest, MovType::Q);
}
void Assembler::movq(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::Q);
}
void Assembler::movl(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::L);
}
void Assembler::movb(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::B);
}
void Assembler::movzbl(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::ZBL);
}
void Assembler::movsbl(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::SBL);
}
void Assembler::movzwl(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::ZBL);
}
void Assembler::movswl(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::SBL);
}
void Assembler::movzbq(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::ZBQ);
}
void Assembler::movsbq(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::SBQ);
}
void Assembler::movzwq(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::ZWQ);
}
void Assembler::movswq(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::SWQ);
}
void Assembler::movslq(Indirect src, Register dest) {
    mov_generic(src, dest, MovType::SLQ);
}

void Assembler::clear_reg(Register reg) {
    int reg_idx = reg.regnum;
    // we don't need to generate a REX_W because 32bit instructions will clear the upper 32bits.
    if (reg_idx >= 8) {
        emitRex(REX_R | REX_B);
        reg_idx -= 8;
    }
    emitByte(0x31);
    emitModRM(0b11, reg_idx, reg_idx);
}

void Assembler::mov_generic(Indirect src, Register dest, MovType type) {
    int rex;
    switch (type) {
        case MovType::Q:
        case MovType::ZBQ:
        case MovType::SBQ:
        case MovType::ZWQ:
        case MovType::SWQ:
        case MovType::SLQ:
            rex = REX_W;
            break;
        case MovType::L:
        case MovType::B:
        case MovType::ZBL:
        case MovType::SBL:
        case MovType::ZWL:
        case MovType::SWL:
            rex = 0;
            break;
        default:
            RELEASE_ASSERT(false, "unrecognized MovType");
    }

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

    if (rex)
        emitRex(rex);

    // opcode
    switch (type) {
        case MovType::Q:
        case MovType::L:
            emitByte(0x8b);
            break;
        case MovType::B:
            emitByte(0x8a);
            break;
        case MovType::ZBQ:
        case MovType::ZBL:
            emitByte(0x0f);
            emitByte(0xb6);
            break;
        case MovType::SBQ:
        case MovType::SBL:
            emitByte(0x0f);
            emitByte(0xbe);
            break;
        case MovType::ZWQ:
        case MovType::ZWL:
            emitByte(0x0f);
            emitByte(0xb7);
            break;
        case MovType::SWQ:
        case MovType::SWL:
            emitByte(0x0f);
            emitByte(0xbf);
            break;
        case MovType::SLQ:
            emitByte(0x63);
            break;
        default:
            RELEASE_ASSERT(false, "unrecognized MovType");
    }

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
    int mode = getModeFromOffset(dest.offset);
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

void Assembler::movss(Indirect src, XMMRegister dest) {
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

    emitByte(0xf3);
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

void Assembler::cvtss2sd(XMMRegister src, XMMRegister dest) {
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

    emitByte(0xf3);
    if (rex)
        emitRex(rex);
    emitByte(0x0f);
    emitByte(0x5a);

    emitModRM(0b11, src_idx, dest_idx);
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

void Assembler::incl(Indirect mem) {
    int src_idx = mem.base.regnum;

    int rex = 0;
    if (src_idx >= 8) {
        rex |= REX_B;
        src_idx -= 8;
    }

    assert(src_idx >= 0 && src_idx < 8);

    if (rex)
        emitRex(rex);
    emitByte(0xff);

    assert(-0x80 <= mem.offset && mem.offset < 0x80);
    if (mem.offset == 0) {
        emitModRM(0b00, 0, src_idx);
    } else {
        emitModRM(0b01, 0, src_idx);
        emitByte(mem.offset);
    }
}

void Assembler::decl(Indirect mem) {
    int src_idx = mem.base.regnum;

    int rex = 0;
    if (src_idx >= 8) {
        rex |= REX_B;
        src_idx -= 8;
    }

    assert(src_idx >= 0 && src_idx < 8);

    if (rex)
        emitRex(rex);
    emitByte(0xff);

    assert(-0x80 <= mem.offset && mem.offset < 0x80);
    if (mem.offset == 0) {
        emitModRM(0b00, 1, src_idx);
    } else {
        emitModRM(0b01, 1, src_idx);
        emitByte(mem.offset);
    }
}

void Assembler::incl(Immediate imm) {
    emitByte(0xff);
    emitByte(0x04);
    emitByte(0x25);
    emitInt(imm.val, 4);
}

void Assembler::decl(Immediate imm) {
    emitByte(0xff);
    emitByte(0x0c);
    emitByte(0x25);
    emitInt(imm.val, 4);
}

void Assembler::call(Immediate imm) {
    emitByte(0xe8);
    emitInt(imm.val, 4);
}

void Assembler::callq(Register r) {
    assert(r == R11 && "untested");

    emitRex(REX_B);
    emitByte(0xff);
    emitByte(0xd3);
}

void Assembler::retq() {
    emitByte(0xc3);
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
    emitArith(imm, reg, OPCODE_CMP);
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

    if (mem.offset == 0) {
        emitModRM(0b00, 7, src_idx);
    } else if (-0x80 <= mem.offset && mem.offset < 0x80) {
        emitModRM(0b01, 7, src_idx);
        emitByte(mem.offset);
    } else {
        assert((-1L << 31) <= mem.offset && mem.offset < (1L << 31) - 1);
        emitModRM(0b10, 7, src_idx);
        emitInt(mem.offset, 4);
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

    if (mem.offset == 0) {
        emitModRM(0b00, reg_idx, mem_idx);
    } else if (-0x80 <= mem.offset && mem.offset < 0x80) {
        emitModRM(0b01, reg_idx, mem_idx);
        emitByte(mem.offset);
    } else {
        assert((-1L << 31) <= mem.offset && mem.offset < (1L << 31) - 1);
        emitModRM(0b10, reg_idx, mem_idx);
        emitInt(mem.offset, 4);
    }
}

void Assembler::lea(Indirect mem, Register reg) {
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
    emitByte(0x8D);

    bool needssib = (mem_idx == 0b100);
    int mode = getModeFromOffset(mem.offset);
    emitModRM(mode, reg_idx, mem_idx);

    if (needssib)
        emitSIB(0b00, 0b100, mem_idx);

    if (mode == 0b01) {
        emitByte(mem.offset);
    } else if (mode == 0b10) {
        assert((-1L << 31) <= mem.offset && mem.offset < (1L << 31) - 1);
        emitInt(mem.offset, 4);
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

void Assembler::jmp(Indirect dest) {
    int reg_idx = dest.base.regnum;

    assert(reg_idx >= 0 && reg_idx < 8 && "not yet implemented");
    emitByte(0xFF);
    if (dest.offset == 0) {
        emitModRM(0b00, 0b100, reg_idx);
    } else if (-0x80 <= dest.offset && dest.offset < 0x80) {
        emitModRM(0b01, 0b100, reg_idx);
        emitByte(dest.offset);
    } else {
        assert((-1L << 31) <= dest.offset && dest.offset < (1L << 31) - 1);
        emitModRM(0b10, 0b100, reg_idx);
        emitInt(dest.offset, 4);
    }
}

void Assembler::jne(JumpDestination dest) {
    jmp_cond(dest, COND_NOT_EQUAL);
}

void Assembler::je(JumpDestination dest) {
    jmp_cond(dest, COND_EQUAL);
}

void Assembler::jmpq(Register dest) {
    int reg_idx = dest.regnum;

    if (reg_idx >= 8) {
        emitRex(REX_B);
        reg_idx -= 8;
    }

    assert(0 <= reg_idx && reg_idx < 8);

    emitByte(0xff);
    emitModRM(0b11, 0b100, reg_idx);
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

void Assembler::leave() {
    emitByte(0xC9);
}

uint8_t* Assembler::emitCall(void* ptr, Register scratch) {
    // emit a 64bit movabs because some caller expect a fixed number of bytes.
    // until they are fixed use the largest encoding.
    mov(Immediate(ptr), scratch, true /* force_64bit_load */);
    callq(scratch);
    return addr;
}

void Assembler::emitBatchPush(int scratch_rbp_offset, int scratch_size, const std::vector<GenericRegister>& to_push) {
    int offset = 0;

    for (const GenericRegister& r : to_push) {
        Indirect next_slot(RBP, offset + scratch_rbp_offset);

        if (r.type == GenericRegister::GP) {
            Register gp = r.gp;
            assert(gp.regnum >= 0 && gp.regnum < 16);
            assert(scratch_size >= offset + 8);
            mov(gp, next_slot);
            offset += 8;
        } else if (r.type == GenericRegister::XMM) {
            XMMRegister reg = r.xmm;
            assert(scratch_size >= offset + 8);
            movsd(reg, next_slot);
            offset += 8;
        } else {
            RELEASE_ASSERT(0, "%d", r.type);
        }
    }
}

void Assembler::emitBatchPop(int scratch_rbp_offset, int scratch_size, const std::vector<GenericRegister>& to_push) {
    int offset = 0;

    for (const GenericRegister& r : to_push) {
        assert(scratch_size >= offset + 8);
        Indirect next_slot(RBP, offset + scratch_rbp_offset);

        if (r.type == GenericRegister::GP) {
            Register gp = r.gp;
            assert(gp.regnum >= 0 && gp.regnum < 16);
            movq(next_slot, gp);
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

void Assembler::skipBytes(int num) {
    if (addr + num >= end_addr) {
        addr = end_addr;
        failed = true;
        return;
    }

    addr += num;
}

ForwardJump::ForwardJump(Assembler& assembler, ConditionCode condition)
    : assembler(assembler), condition(condition), jmp_inst(assembler.curInstPointer()) {
    assembler.jmp_cond(JumpDestination::fromStart(assembler.bytesWritten() + max_jump_size), condition);
}

ForwardJump::~ForwardJump() {
    uint8_t* new_pos = assembler.curInstPointer();
    int offset = new_pos - jmp_inst;
    RELEASE_ASSERT(offset < max_jump_size, "");
    assembler.setCurInstPointer(jmp_inst);
    assembler.jmp_cond(JumpDestination::fromStart(assembler.bytesWritten() + offset), condition);
    assembler.setCurInstPointer(new_pos);
}
}
}
