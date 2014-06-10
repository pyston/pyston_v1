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

#ifndef PYSTON_ASMWRITING_ASSEMBLER_H
#define PYSTON_ASMWRITING_ASSEMBLER_H

#include <unordered_set>

#include "asm_writing/types.h"
#include "core/ast.h"

namespace pyston {
namespace assembler {

class BoxedClass;

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

class Assembler {
private:
    uint8_t* const start_addr, *const end_addr;
    uint8_t* addr;

    static const uint8_t OPCODE_ADD = 0b000, OPCODE_SUB = 0b101;
    static const uint8_t REX_B = 1, REX_X = 2, REX_R = 4, REX_W = 8;

private:
    void emitByte(uint8_t b);
    void emitInt(int64_t n, int bytes);
    void emitRex(uint8_t rex);
    void emitModRM(uint8_t mod, uint8_t reg, uint8_t rm);
    void emitSIB(uint8_t scalebits, uint8_t index, uint8_t base);
    void emitArith(Immediate imm, Register reg, int opcode);

public:
    Assembler(uint8_t* start, int size) : start_addr(start), end_addr(start + size), addr(start_addr) {}

    void nop() { emitByte(0x90); }
    void trap() { emitByte(0xcc); }

    // some things (such as objdump) call this "movabs" if the immediate is 64-bit
    void mov(Immediate imm, Register dest);
    // not sure if we should use the 'q' suffix here, but this is the most ambiguous one;
    // this does a 64-bit store of a 32-bit value.
    void movq(Immediate imm, Indirect dest);
    void mov(Register src, Register dest);
    void mov(Register src, Indirect dest);
    void mov(Indirect src, Register dest);
    void movsd(XMMRegister src, XMMRegister dest);
    void movsd(XMMRegister src, Indirect dest);
    void movsd(Indirect src, XMMRegister dest);

    void push(Register reg);
    void pop(Register reg);

    void add(Immediate imm, Register reg);
    void sub(Immediate imm, Register reg);
    void inc(Register reg);
    void inc(Indirect mem);

    void callq(Register reg);

    void cmp(Register reg1, Register reg2);
    void cmp(Register reg, Immediate imm);
    void cmp(Indirect mem, Immediate imm);
    void cmp(Indirect mem, Register reg);

    void test(Register reg1, Register reg2);

    void jmp_cond(JumpDestination dest, ConditionCode condition);
    void jmp(JumpDestination dest);
    void je(JumpDestination dest);
    void jne(JumpDestination dest);

    void set_cond(Register reg, ConditionCode condition);
    void sete(Register reg);
    void setz(Register reg) { sete(reg); }
    void setne(Register reg);
    void setnz(Register reg) { setne(reg); }


    // Macros:
    uint8_t* emitCall(void* func_addr, Register scratch);
    void emitBatchPop(StackInfo stack_info, const std::vector<GenericRegister>& to_push);
    void emitBatchPush(StackInfo stack_info, const std::vector<GenericRegister>& to_push);
    void fillWithNops();
    void fillWithNopsExcept(int bytes);
    void emitAnnotation(int num);

    bool isExactlyFull() { return addr == end_addr; }
};

uint8_t* initializePatchpoint2(uint8_t* start_addr, uint8_t* slowpath_start, uint8_t* end_addr, StackInfo stack_info,
                               const std::unordered_set<int>& live_outs);
}
}

#endif
