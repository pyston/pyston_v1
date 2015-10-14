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

#ifndef PYSTON_ASMWRITING_ASSEMBLER_H
#define PYSTON_ASMWRITING_ASSEMBLER_H

#include <unordered_set>

#include "asm_writing/disassemble.h"
#include "asm_writing/types.h"
#include "codegen/stackmaps.h"
#include "core/ast.h"
#include "core/options.h"

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

enum class MovType {
    Q,
    L,
    B,
    ZBL,
    SBL,
    ZWL,
    SWL,
    ZBQ,
    SBQ,
    ZWQ,
    SWQ,
    SLQ,

    ZLQ = L,
};

class Assembler {
private:
    uint8_t* const start_addr, *const end_addr;
    uint8_t* addr;
    bool failed; // if the rewrite failed at the assembly-generation level for some reason

    static const uint8_t OPCODE_ADD = 0b000, OPCODE_SUB = 0b101, OPCODE_CMP = 0b111;
    static const uint8_t REX_B = 1, REX_X = 2, REX_R = 4, REX_W = 8;

#ifndef NDEBUG
    AssemblyLogger logger;
#endif

private:
    void emitByte(uint8_t b);
    void emitInt(int64_t n, int bytes);
    void emitUInt(uint64_t n, int bytes);
    void emitRex(uint8_t rex);
    void emitModRM(uint8_t mod, uint8_t reg, uint8_t rm);
    void emitSIB(uint8_t scalebits, uint8_t index, uint8_t base);
    void emitArith(Immediate imm, Register reg, int opcode);

    int getModeFromOffset(int offset) const;

public:
    Assembler(uint8_t* start, int size) : start_addr(start), end_addr(start + size), addr(start_addr), failed(false) {}

#ifndef NDEBUG
    inline void comment(const llvm::Twine& msg) {
        if (ASSEMBLY_LOGGING) {
            logger.log_comment(msg, addr - start_addr);
        }
    }
    inline std::string dump() {
        if (ASSEMBLY_LOGGING) {
            return logger.finalize_log(start_addr, addr);
        } else {
            return "";
        }
    }
#else
    inline void comment(const llvm::Twine& msg) {}
    inline std::string dump() { return ""; }
#endif

    bool hasFailed() { return failed; }

    void nop() { emitByte(0x90); }
    void trap() { emitByte(0xcc); }

    // emits a movabs if the immediate is a 64bit value or force_64bit_load = true otherwise it emits a 32bit mov
    void mov(Immediate imm, Register dest, bool force_64bit_load = false);
    // not sure if we should use the 'q' suffix here, but this is the most ambiguous one;
    // this does a 64-bit store of a 32-bit value.
    void movq(Immediate imm, Indirect dest);
    void mov(Register src, Register dest);
    void mov(Register src, Indirect dest);
    void movsd(XMMRegister src, XMMRegister dest);
    void movsd(XMMRegister src, Indirect dest);
    void movsd(Indirect src, XMMRegister dest);

    void movss(Indirect src, XMMRegister dest);
    void cvtss2sd(XMMRegister src, XMMRegister dest);

    void mov(Indirect scr, Register dest);
    void movq(Indirect scr, Register dest);
    void movl(Indirect scr, Register dest);
    void movb(Indirect scr, Register dest);
    void movzbl(Indirect scr, Register dest);
    void movsbl(Indirect scr, Register dest);
    void movzwl(Indirect scr, Register dest);
    void movswl(Indirect scr, Register dest);
    void movzbq(Indirect scr, Register dest);
    void movsbq(Indirect scr, Register dest);
    void movzwq(Indirect scr, Register dest);
    void movswq(Indirect scr, Register dest);
    void movslq(Indirect scr, Register dest);

    void clear_reg(Register reg); // = xor reg, reg

    void mov_generic(Indirect src, Register dest, MovType type);

    void push(Register reg);
    void pop(Register reg);

    void add(Immediate imm, Register reg);
    void sub(Immediate imm, Register reg);

    void incl(Indirect mem);
    void decl(Indirect mem);

    void incl(Immediate mem);
    void decl(Immediate mem);

    void call(Immediate imm); // the value is the offset
    void callq(Register reg);
    void retq();
    void leave();

    void cmp(Register reg1, Register reg2);
    void cmp(Register reg, Immediate imm);
    void cmp(Indirect mem, Immediate imm);
    void cmp(Indirect mem, Register reg);

    void lea(Indirect mem, Register reg);

    void test(Register reg1, Register reg2);

    void jmp_cond(JumpDestination dest, ConditionCode condition);
    void jmp(JumpDestination dest);
    void jmp(Indirect dest);
    void jmpq(Register dest);
    void je(JumpDestination dest);
    void jne(JumpDestination dest);

    void set_cond(Register reg, ConditionCode condition);
    void sete(Register reg);
    void setz(Register reg) { sete(reg); }
    void setne(Register reg);
    void setnz(Register reg) { setne(reg); }


    // Macros:
    uint8_t* emitCall(void* func_addr, Register scratch);
    void emitBatchPop(int scratch_rbp_offset, int scratch_size, const std::vector<GenericRegister>& to_push);
    void emitBatchPush(int scratch_rbp_offset, int scratch_size, const std::vector<GenericRegister>& to_push);
    void fillWithNops();
    void fillWithNopsExcept(int bytes);
    void emitAnnotation(int num);
    void skipBytes(int num);

    uint8_t* startAddr() const { return start_addr; }
    int bytesLeft() const { return end_addr - addr; }
    int bytesWritten() const { return addr - start_addr; }
    uint8_t* curInstPointer() { return addr; }
    void setCurInstPointer(uint8_t* ptr) { addr = ptr; }
    bool isExactlyFull() const { return addr == end_addr; }
    uint8_t* getStartAddr() { return start_addr; }
};

// This class helps generating a forward jump with a relative offset.
// It keeps track of the current assembler offset at construction time and in the destructor patches the
// generated conditional jump with the correct offset depending on the number of bytes emitted in between.
class ForwardJump {
private:
    const int max_jump_size = 128;
    Assembler& assembler;
    ConditionCode condition;
    uint8_t* jmp_inst;

public:
    ForwardJump(Assembler& assembler, ConditionCode condition);
    ~ForwardJump();
};

uint8_t* initializePatchpoint2(uint8_t* start_addr, uint8_t* slowpath_start, uint8_t* end_addr, StackInfo stack_info,
                               const std::unordered_set<int>& live_outs);
}
}

#endif
