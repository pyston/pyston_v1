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

#include "asm_writing/mc_writer.h"

#include <cstring>

#include "core/ast.h"
#include "core/common.h"
#include "core/options.h"

namespace pyston {

#if !1
#define POINTER_SIZE 8

namespace X86 {
const char* regnames[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

const bool is_callee_save[] = {
    false,
    true, // %rbx
    false, false, false,
    true, // %rbp
    false, false, false, false, false, false, true, true, true, true,
};

const int DwarfRegToX86[] = {
    0,  // 0
    2,  // 1
    1,  // 2 -> rcx
    3,  // 3 -> rbx
    6,  // 4
    7,  // 5
    5,  // 6
    4,  // 7
    8,  // 8 -> r8
    9,  // 9 -> r9
    10, // 10 -> r10
    11, // 11 -> r11
    12, // 12 -> r12
    13, // 13 -> r13
    14, // 14 -> r14
    15, // 15 -> r15

    // http://www.x86-64.org/documentation/abi.pdf#page=57
    // 16 -> ReturnAddress RA (??)
    // 17-32: xmm0-xmm15
};

const int NUM_ARG_REGS = 6;
const int arg_regs[] = {
    7, // rdi
    6, // rsi
    2, // rdx
    1, // rcx
    8, // r8
    9, // r9
};

const uint8_t REX_B = 1, REX_X = 2, REX_R = 4, REX_W = 8;

const int REG_RTN = 0;
const int REG_STACK_POINTER = 4;
// enum RexFlags {
// REX_B = 1, REX_X = 2, REX_R = 4, REX_W = 8
//};

const int BYTES_PER_POP = 1;

// Any time we emit a call, make sure that we align the stack to a multiple of this.
// Required to be a multiple of 16 to support SSE:
const int CALL_STACK_ALIGNMENT = 16;
// The consequence is we need a multiple of this many pushes:
const int PUSH_MULT = CALL_STACK_ALIGNMENT / POINTER_SIZE;

enum ConditionCode {
    COND_OVERFLOW = 0,     // OF=1: O
    COND_NOT_OVERFLOW = 1, // OF=0: NO
    // next 4 are unsigned:
    COND_BELOW = 2,         // CF=1: B/NAE/C
    COND_NOT_BELOW = 3,     // CF=0: NB/AE/C
    COND_EQUAL = 4,         // ZF=0: Z/E
    COND_NOT_EQUAL = 5,     // ZF=1: NZ/NE
    COND_NOT_ZERO = 5,      // ZF=1: NZ/NE
    COND_NOT_ABOVE = 6,     // CF=1: ZF=1: BE/NA
    COND_ABOVE = 7,         // CF=0: ZF=0: NBE/A
    COND_SIGN = 8,          // SF=1: S
    COND_NOT_SIGN = 9,      // SF=0: NS
    COND_PARITY_EVEN = 0xA, // PF=1: P/PE
    COND_PARITY_ODD = 0xB,  // PF=0: NP/PO
    // next 4 are signed:
    COND_LESS = 0xC,        // SF!=OF: L/NGE
    COND_NOT_LESS = 0xD,    // SF==OF: NL/GE
    COND_NOT_GREATER = 0xE, // ZF=1 || SF!=OF: LE/NG
    COND_GREATER = 0xF,     // ZF=0 && SF==OF: NLE/G
};

const int OPCODE_ADD = 0b000, OPCODE_SUB = 0b101;
}

#define CALL_SIZE 13

class X86MCWriter : public MCWriter {
private:
    uint8_t* addr;
    uint8_t* const start_addr, *const end_addr;
    int pops_required;

    inline void _emitByte(uint8_t b) {
        if (TRAP) {
            printf(" %02x", b);
            fflush(stdout);
        }
        assert(addr < end_addr);
        *addr++ = b;
    }

    inline void _emitRex(uint8_t flags) {
        assert(0 <= flags && flags < 16);
        _emitByte(0x40 | flags);
    }

    inline void _emitModRM(uint8_t mod, uint8_t reg, uint8_t rm) {
        assert(mod < 4);
        assert(reg < 8);
        assert(rm < 8);
        _emitByte((mod << 6) | (reg << 3) | rm);
    }

    inline void _emitSIB(uint8_t scalebits, uint8_t index, uint8_t base) {
        assert(scalebits < 4);
        assert(index < 8);
        assert(base < 8);
        _emitByte((scalebits << 6) | (index << 3) | base);
    }

    inline void _emitPush(uint8_t reg) {
        assert(reg != X86::REG_STACK_POINTER); // this might work but most likely a bug
        assert(0 <= reg && reg < 16);

        if (reg >= 8) {
            _emitRex(X86::REX_B);
            reg -= 8;
        }
        assert(reg < 8);
        _emitByte(0x50 + reg);
    }

    inline void _emitPop(uint8_t reg) {
        assert(reg != X86::REG_STACK_POINTER); // this might work but most likely a bug
        assert(0 <= reg && reg < 16);

        if (reg >= 8) {
            _emitRex(X86::REX_B);
            reg -= 8;
        }
        assert(reg < 8);
        _emitByte(0x58 + reg);
    }

    /// mov %source, $displacement(%dest)
    inline void _emitStoreRegIndirect(int source, int dest, int displacement) {
        bool usesib = false;
        if (dest == 0b100 || dest == 0b1100) {
            usesib = true;
        }

        uint8_t flags = X86::REX_W;
        if (dest >= 8) {
            flags |= X86::REX_B;
            dest &= 0b111;
        }
        if (source >= 8) {
            flags |= X86::REX_R;
            source &= 0b111;
        }

        _emitRex(flags);
        _emitByte(0x89);

        int mode;
        if (displacement == 0)
            mode = 0b00;
        else if (-0x80 <= displacement && displacement < 0x79)
            mode = 0b01;
        else
            mode = 0b10;

        _emitModRM(mode, source, dest);
        if (usesib)
            _emitSIB(0b00, 0b100, dest);
        if (mode == 0b01) {
            _emitByte(displacement);
        } else if (mode == 0b10) {
            for (int i = 0; i < 4; i++) {
                _emitByte(displacement & 0xff);
                displacement >>= 8;
            }
        }
    }

    // incq offset(%reg)
    virtual void _emitIncattr(int reg, int offset) {
        assert(offset >= -0x80 && offset < 0x80);

        int rex = X86::REX_W;
        if (reg >= 8) {
            rex |= X86::REX_B;
            reg -= 8;
        }

        _emitRex(rex);
        _emitByte(0xff);
        _emitModRM(0b01, 0b000, reg);
        _emitByte(offset);
    }

    /// mov $displacement(%source) %dest
    inline void _emitLoadRegIndirect(int source, int displacement, int dest) {
        bool usesib = false;
        if (source == 0b100 || dest == 0b1100) {
            usesib = true;
        }

        uint8_t flags = X86::REX_W;
        if (dest >= 8) {
            flags |= X86::REX_R;
            dest &= 0b111;
        }
        if (source >= 8) {
            flags |= X86::REX_B;
            source &= 0b111;
        }

        _emitRex(flags);
        _emitByte(0x8b);

        int mode;
        if (displacement == 0)
            mode = 0b00;
        else if (-0x80 <= displacement && displacement < 0x79)
            mode = 0b01;
        else
            mode = 0b10;

        _emitModRM(mode, dest, source);
        if (usesib)
            _emitSIB(0b00, 0b100, source);

        if (mode == 0b01) {
            _emitByte(displacement);
        } else if (mode == 0b10) {
            for (int i = 0; i < 4; i++) {
                _emitByte(displacement & 0xff);
                displacement >>= 8;
            }
        }
    }

    inline void _emitMoveReg(int source, int dest) {
        uint8_t flags = 0;
        flags |= X86::REX_W;
        if (dest >= 8) {
            flags |= X86::REX_B;
            dest &= 0b111;
        }
        if (source >= 8) {
            flags |= X86::REX_R;
            source &= 0b111;
        }

        _emitRex(flags);
        _emitByte(0x89);
        _emitModRM(0b11, source, dest);
    }

    inline void _emitMoveImm64(uint8_t reg, uint64_t value) {
        assert(reg >= 0 && reg < 8);
        _emitRex(X86::REX_W);
        _emitByte(0xb8 + reg);

        for (int i = 0; i < 8; i++) {
            _emitByte(value & 0xff);
            value >>= 8;
        }
    }

    // TODO verify that the arguments are being compared in the right order
    // cmpq %reg1, %reg2 # or maybe they're reversed?
    inline void _emitCmp(int reg1, int reg2) {
        int rex = X86::REX_W;
        if (reg1 >= 8) {
            rex |= X86::REX_R;
            reg1 -= 8;
        }

        assert(reg1 >= 0 && reg1 < 8);
        assert(reg2 >= 0 && reg2 < 8);

        _emitRex(rex);
        _emitByte(0x39);
        _emitModRM(0b11, reg1, reg2);
    }

    // TODO verify that the arguments are being compared in the right order
    // cmp $val, %reg
    inline void _emitCmpImm(int reg, int64_t val) {
        assert((-1L << 31) <= val && val < (1L << 31) - 1);

        int rex = X86::REX_W;
        if (reg > 8) {
            rex |= X86::REX_B;
            reg -= 8;
        }
        assert(0 <= reg && reg < 8);

        _emitRex(rex);
        _emitByte(0x81);
        _emitModRM(0b11, 7, reg);
        for (int i = 0; i < 4; i++) {
            _emitByte(val & 0xff);
            val >>= 8;
        }
    }

    // TODO verify that the arguments are being compared in the right order
    // cmpq offset(%reg1), %reg2
    inline void _emitAttrCmp(int reg1, int reg1_offset, int reg2) {
        int rex = X86::REX_W;
        if (reg1 >= 8) {
            rex |= X86::REX_B;
            reg1 -= 8;
        }

        assert(reg1 >= 0 && reg1 < 8);
        assert(reg2 >= 0 && reg2 < 8);

        _emitRex(rex);
        _emitByte(0x3B);

        assert(-0x80 <= reg1_offset && reg1_offset < 0x80);
        if (reg1_offset == 0) {
            _emitModRM(0b00, reg2, reg1);
        } else {
            _emitModRM(0b01, reg2, reg1);
            _emitByte(reg1_offset);
        }
    }

    // TODO verify that the arguments are being compared in the right order
    // cmpq offset(%reg), $imm
    inline void _emitAttrCmpImm(int reg, int offset, int64_t val) {
        assert((-1L << 31) <= val && val < (1L << 31) - 1);

        int rex = X86::REX_W;
        if (reg >= 8) {
            rex |= X86::REX_B;
            reg -= 8;
        }

        assert(reg >= 0 && reg < 8);

        _emitRex(rex);
        _emitByte(0x81);

        assert(-0x80 <= offset && offset < 0x80);
        if (offset == 0) {
            _emitModRM(0b00, 7, reg);
        } else {
            _emitModRM(0b01, 7, reg);
            _emitByte(offset);
        }

        for (int i = 0; i < 4; i++) {
            _emitByte(val & 0xff);
            val >>= 8;
        }
    }

    // test[q] %reg1, %reg2
    inline void _emitTest(int reg1, int reg2) {
        assert(reg1 >= 0 && reg1 < 8);
        assert(reg2 >= 0 && reg2 < 8);

        _emitRex(X86::REX_W);
        _emitByte(0x85);
        _emitModRM(0b11, reg1, reg2);
    }

    inline void _emitCmpDisplacement(uint8_t reg1, uint64_t reg2, int displacement) {
        // TODO if it's bigger, we could use a larger scale since
        // things are most likely aligned
        assert(displacement >= -0x80 && displacement < 0x80);

        uint8_t flags = 0;
        flags |= X86::REX_W;

        if (reg1 >= 8) {
            flags |= X86::REX_R;
            reg1 &= 0b111;
        }
        if (reg2 >= 8) {
            flags |= X86::REX_B;
            reg2 &= 0b111;
        }

        _emitRex(flags);
        _emitByte(0x39);
        if (displacement == 0) {
            // Since we're emitting into a fixed-size section I guess there might not be
            // too much benifit to the more compact encoding, but it makes me feel better:
            _emitModRM(0b00, reg1, reg2);
        } else {
            _emitModRM(0b01, reg1, reg2);
            _emitByte(displacement);
        }
    }

    inline void _emitJmpCond(uint8_t* dest_addr, X86::ConditionCode condition, bool unlikely) {
        int offset = dest_addr - addr - 2;
        if (unlikely)
            offset -= 1;

        if (offset >= -0x80 && offset < 0x80) {
            if (unlikely)
                _emitByte(0x2e);
            _emitByte(0x75);
            _emitByte(offset);
        } else {
            offset -= 4;

            if (unlikely)
                _emitByte(0x2e);

            _emitByte(0x0f);
            _emitByte(0x80 | condition);
            for (int i = 0; i < 4; i++) {
                _emitByte(offset & 0xff);
                offset >>= 8;
            }
        }
    }

    inline void _emitJne(uint8_t* dest_addr, bool unlikely) {
        _emitJmpCond(dest_addr, X86::COND_NOT_EQUAL, unlikely);
        int offset = dest_addr - addr - 2;
        if (unlikely)
            offset -= 1;

        if (offset >= -0x80 && offset < 0x80) {
            if (unlikely)
                _emitByte(0x2e);
            _emitByte(0x75);
            _emitByte(offset);
        } else {
            offset -= 4;

            if (unlikely)
                _emitByte(0x2e);

            _emitByte(0x0f);
            _emitByte(0x85);
            for (int i = 0; i < 4; i++) {
                _emitByte(offset & 0xff);
                offset >>= 8;
            }
        }
    }

    /// "op $val, %reg"
    inline void _emitArith(int reg, int val, int opcode) {
        assert(val >= -0x80 && val < 0x80);
        assert(opcode < 8);

        uint8_t flags = 0;
        flags |= X86::REX_W;

        if (reg >= 8) {
            flags |= X86::REX_B;
            reg &= 0b111;
        }

        _emitRex(flags);
        _emitByte(0x83);
        _emitModRM(0b11, opcode, reg);
        _emitByte(val);
    }

    /// "add $val, %reg"
    inline void _emitAdd(int reg, int val) { _emitArith(reg, val, X86::OPCODE_ADD); }

    /// "sub %val, %reg"
    inline void _emitSub(int reg, int val) { _emitArith(reg, val, X86::OPCODE_SUB); }

    int convertArgnum(int argnum) {
        ASSERT(argnum >= -3 && argnum < X86::NUM_ARG_REGS, "%d", argnum);
        if (argnum == -1)
            return X86::REG_RTN;
        else if (argnum == -2)
            return 10;
        else if (argnum == -3)
            return 11;
        else
            return X86::arg_regs[argnum];
    }

    void _emitJmp(void* dest_addr) {
        long offset = (uint8_t*)dest_addr - addr - 2;
        if (offset >= -0x80 && offset < 0x80) {
            _emitByte(0xeb);
            _emitByte(offset);
        } else {
            assert(offset >= -1L << 31 && offset < 1L << 31);

            offset -= 3;
            _emitByte(0xe9);
            for (int i = 0; i < 4; i++) {
                _emitByte(offset & 0xff);
                offset >>= 8;
            }
        }
    }

    void _emitCondSet(int dest_reg, int cond_code) {
        assert(0 <= dest_reg && dest_reg < 8);
        assert(0 <= cond_code && cond_code < 16);

        if (dest_reg >= 4)
            _emitRex(0);
        _emitByte(0x0f);
        _emitByte(0x90 + cond_code);
        _emitModRM(0b11, 0, dest_reg);
    }

    // movzbq %src_reg, %dest_reg
    void _emitZeroExtend(int src_reg, int dest_reg) {
        assert(0 <= src_reg && src_reg < 8);
        assert(0 <= dest_reg && dest_reg < 8);

        _emitRex(X86::REX_W);
        _emitByte(0x0f);
        _emitByte(0xb6);
        _emitModRM(0b11, dest_reg, src_reg);
    }

    virtual void _emitGuard(int argnum, int64_t value, int npops, X86::ConditionCode slowpath_condition) {
        assert(slowpath_condition == X86::COND_EQUAL
               || slowpath_condition == X86::COND_NOT_EQUAL && "not sure if the cmp operands are in the right order");

        assert(argnum <= X86::NUM_ARG_REGS);
        int argreg = convertArgnum(argnum);

        if (value < (-1l << 31) || value >= (1l << 31)) {
            // assert(0 && "can use r10 or r11");
            int cmpreg = 5;
            assert(argreg != cmpreg);

            _emitPush(cmpreg);
            _emitMoveImm64(cmpreg, value);
            _emitCmp(argreg, cmpreg);
            _emitPop(cmpreg);
        } else {
            _emitCmpImm(argreg, value);
        }

        this->pops_required = std::max(this->pops_required, npops);
        _emitJmpCond(end_addr - X86::BYTES_PER_POP * npops, slowpath_condition, true);
    }

    virtual void _emitAttrGuard(int argnum, int offset, int64_t value, int npops,
                                X86::ConditionCode slowpath_condition) {
        assert(slowpath_condition == X86::COND_EQUAL
               || slowpath_condition == X86::COND_NOT_EQUAL && "not sure if the cmp operands are in the right order");

        assert(argnum <= X86::NUM_ARG_REGS);
        int argreg = convertArgnum(argnum);

        if (value < (-1l << 31) || value >= (1l << 31)) {
            // assert(0 && "can use r10 or r11");
            int cmpreg = 5;
            assert(argreg != cmpreg);

            _emitPush(cmpreg);
            _emitMoveImm64(cmpreg, value);
            _emitAttrCmp(argreg, offset, cmpreg);
            _emitPop(cmpreg);
        } else {
            _emitAttrCmpImm(argreg, offset, value);
        }

        this->pops_required = std::max(this->pops_required, npops);
        _emitJmpCond(end_addr - X86::BYTES_PER_POP * npops, slowpath_condition, true);
    }

public:
    X86MCWriter(uint8_t* addr, int size) : addr(addr), start_addr(addr), end_addr(addr + size), pops_required(0) {}

    virtual int numArgRegs() { return X86::NUM_ARG_REGS; }

    virtual int numTempRegs() { return 2; }

    virtual void emitNop() { _emitByte(0x90); }

    virtual void emitTrap() { _emitByte(0xcc); }

    virtual void emitAnnotation(int num) {
        emitNop();
        _emitCmpImm(0, num);
        emitNop();
        //_emitAdd(0, num);
        //_emitSub(0, num);
    }

    // TODO this is unclear
    virtual void endFastPath(void* success_dest, void* will_relocate_to) {
        void* dest = ((uint8_t*)success_dest - (uint8_t*)will_relocate_to) + start_addr;
        _emitJmp(dest);
    }

    // TODO this is unclear
    virtual void endWithSlowpath() {
        int pop_bytes = pops_required * X86::BYTES_PER_POP;
        uint8_t* pop_start = end_addr - pop_bytes;

        // printf("end addr is %p; pop_start is %p for %d pops\n", end_addr, pop_start, pops_required);

        assert(addr <= pop_start);
        memset(addr, 0x90, pop_start - addr);
        addr = pop_start;

        // We don't havy any result to return, so clobber %rax:
        const int POP_REG = 0; // %rax
        assert(!X86::is_callee_save[POP_REG]);
        for (int i = 0; i < pops_required; i++) {
            _emitPop(POP_REG);
        }
        assert(addr == end_addr);

        addr = NULL;
    }

    virtual void emitGuard(int argnum, int64_t value, int npops) {
        _emitGuard(argnum, value, npops, X86::COND_NOT_EQUAL);
    }

    virtual void emitAttrGuard(int argnum, int offset, int64_t value, int npops) {
        _emitAttrGuard(argnum, offset, value, npops, X86::COND_NOT_EQUAL);
    }

    virtual void emitGuardFalse() { _emitJmp(end_addr); }

    virtual void emitGuardNotEq(int argnum, int64_t value, int npops) {
        _emitGuard(argnum, value, npops, X86::COND_EQUAL);
    }

    virtual uint8_t* emitCall(void* new_addr, int npushes) {
        // Use pushes and pops to align the stack.  There could be a better way, but
        // realistically we'll only be pushing or popping once at a time.
        assert(npushes >= 0);
        // The pushes can come from any reg; for pops,
        // use %rdi, or arg0, since the arguments should be safe to clobber
        // after the call:
        const int POP_REG = 7;
        int pushes_needed = X86::PUSH_MULT - (npushes + X86::PUSH_MULT - 1) % X86::PUSH_MULT - 1;
        // printf("emitting %d pushes to align to %d bytes\n", pushes_needed, X86::CALL_STACK_ALIGNMENT);
        for (int i = 0; i < pushes_needed; i++) {
            _emitPush(POP_REG);
        }

        assert(new_addr);
        _emitRex(X86::REX_W | X86::REX_B);
        _emitByte(0xbb);

        uint8_t* rtn = addr;
        uintptr_t addr_int = (uintptr_t)new_addr;
        for (int i = 0; i < POINTER_SIZE; i++) {
            _emitByte(addr_int & 0xff);
            addr_int >>= 8;
        }
        // printf("\n");

        _emitRex(X86::REX_B);
        _emitByte(0xff);
        _emitByte(0xd3);

        for (int i = 0; i < pushes_needed; i++) {
            _emitPop(POP_REG);
        }

        return rtn;
    }

    virtual void emitAlloca(int bytes, int dest_argnum) {
        int destreg = convertArgnum(dest_argnum);

        assert(bytes);
        _emitSub(X86::REG_STACK_POINTER, bytes);
        _emitMoveReg(X86::REG_STACK_POINTER, destreg);
    }

    virtual void emitMove(int src_argnum, int dest_argnum, int npushed) {
        if (src_argnum >= X86::NUM_ARG_REGS) {
            // Note: no function call happened so rip didn't get pushed
            int orig_offset = (src_argnum - X86::NUM_ARG_REGS) * POINTER_SIZE;
            int offset = orig_offset + npushed * POINTER_SIZE;
            int destreg = convertArgnum(dest_argnum);
            _emitLoadRegIndirect(X86::REG_STACK_POINTER, offset, destreg);
        } else {
            int srcreg = convertArgnum(src_argnum);
            int destreg = convertArgnum(dest_argnum);
            _emitMoveReg(srcreg, destreg);
        }
    }

    virtual void emitGetattr(int src_argnum, int src_offset, int dest_argnum) {
        int srcreg = convertArgnum(src_argnum);
        int destreg = convertArgnum(dest_argnum);
        _emitLoadRegIndirect(srcreg, src_offset, destreg);
    }

    virtual void emitIncattr(int argnum, int offset) {
        int reg = convertArgnum(argnum);
        _emitIncattr(reg, offset);
    }

    virtual void emitSetattr(int src_argnum, int dest_argnum, int dest_offset) {
        int srcreg = convertArgnum(src_argnum);
        int destreg = convertArgnum(dest_argnum);
        _emitStoreRegIndirect(srcreg, destreg, dest_offset);
    }

    virtual void emitPush(int argnum) { _emitPush(convertArgnum(argnum)); }

    virtual void emitPop(int argnum) { _emitPop(convertArgnum(argnum)); }

    virtual void emitLoadConst(int argnum, int64_t val) {
        int reg = convertArgnum(argnum);
        _emitMoveImm64(reg, val);
    }

    virtual void emitCmp(AST_TYPE::AST_TYPE cmp_type, int lhs_argnum, int rhs_argnum, int dest_argnum) {
        int lhs_reg = convertArgnum(lhs_argnum);
        int rhs_reg = convertArgnum(rhs_argnum);
        int dest_reg = convertArgnum(dest_argnum);

        _emitCmp(lhs_reg, rhs_reg);

        int condition_code;
        switch (cmp_type) {
            case AST_TYPE::Eq:
            case AST_TYPE::Is:
                condition_code = X86::COND_EQUAL;
                break;
            case AST_TYPE::NotEq:
            case AST_TYPE::IsNot:
                condition_code = X86::COND_NOT_EQUAL;
                break;
            default:
                RELEASE_ASSERT(0, "%d", cmp_type);
        }

        // TODO if we do this on rdi/rsi, which will be common,
        // it'd be more efficient to clobber rax/rbx/rcx or rdx
        // as a temporary, since it would save the two REX bytes.
        // For now, let's just emit the lower-efficiency but
        // easier-to-maintain code.
        _emitCondSet(dest_reg, condition_code);
        _emitZeroExtend(dest_reg, dest_reg);
    }

    virtual void emitToBool(int argnum, int dest_argnum) {
        int reg = convertArgnum(argnum);
        int dest_reg = convertArgnum(dest_argnum);

        _emitTest(reg, reg);
        _emitCondSet(dest_reg, X86::COND_NOT_ZERO);
    }
};
#endif

void initializePatchpoint(uint8_t* addr, int size) {
#define CALL_SIZE 13
#ifndef NDEBUG
    assert(size >= CALL_SIZE);

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
    ASSERT(addr[0] == 0x49, "%x", addr[0]);
    assert(addr[1] == 0xbb);
    // 8 bytes of the addr

    // callq *%r11:
    assert(addr[10] == 0x41);
    assert(addr[11] == 0xff);
    assert(addr[12] == 0xd3);

    int i = CALL_SIZE;
    while (*(addr + i) == 0x66 || *(addr + i) == 0x0f || *(addr + i) == 0x2e)
        i++;
    assert(*(addr + i) == 0x90 || *(addr + i) == 0x1f);
#endif

    memcpy(addr + size - CALL_SIZE, addr, CALL_SIZE);
    memset(addr, 0x90, size - CALL_SIZE);
    // addr[0] = 0xcc;

    //// Move the call to the end of the region:
    // char scratch[CALL_SIZE];
    // memcpy(scratch, addr, CALL_SIZE);
    // std::memmove(addr, addr + CALL_SIZE, size - CALL_SIZE);
    // memcpy(addr + size - CALL_SIZE, scratch, CALL_SIZE);
}

/*
MCWriter* createMCWriter(uint8_t* addr, int size, int num_temp_regs) {
    assert(num_temp_regs >= 0);

    // The X86MCWriter will automatically use %r10 and %r11, so don't need
    // to pass that along.  But if the client requested more than two
    // temporaries, err out.
    assert(num_temp_regs <= 2 && "unsupported");

    return new X86MCWriter(addr, size);
}
*/
}
