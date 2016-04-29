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

#ifndef PYSTON_ASMWRITING_TYPES_H
#define PYSTON_ASMWRITING_TYPES_H

#include "core/common.h"

namespace pyston {

struct StackInfo {
    int scratch_size;
    int scratch_rsp_offset;

    StackInfo(int scratch_size, int scratch_rsp_offset)
        : scratch_size(scratch_size), scratch_rsp_offset(scratch_rsp_offset) {
        assert(scratch_rsp_offset >= 0);
    }
};

namespace assembler {

class Assembler;

struct Register {
    int regnum;

    explicit constexpr Register(int regnum) : regnum(regnum) {}

    bool isCalleeSave();

    bool operator==(const Register& rhs) const { return regnum == rhs.regnum; }

    bool operator!=(const Register& rhs) const { return !(*this == rhs); }

    void dump() const;

    static Register fromDwarf(int dwarf_regnum);

    static constexpr int numRegs() { return 16; }
};

const Register RAX(0);
const Register RCX(1);
const Register RDX(2);
const Register RBX(3);
const Register RSP(4);
const Register RBP(5);
const Register RSI(6);
const Register RDI(7);
const Register R8(8);
const Register R9(9);
const Register R10(10);
const Register R11(11);
const Register R12(12);
const Register R13(13);
const Register R14(14);
const Register R15(15);

inline bool Register::isCalleeSave() {
    return *this == RBX || *this == RSP || *this == RBP || regnum >= 12;
}

struct Indirect {
public:
    const Register base;
    const int64_t offset;

    Indirect(const Register base, int64_t offset) : base(base), offset(offset) {}
};

struct XMMRegister {
    int regnum;

    explicit constexpr XMMRegister(int regnum) : regnum(regnum) {}

    bool operator==(const XMMRegister& rhs) const { return regnum == rhs.regnum; }

    bool operator!=(const XMMRegister& rhs) const { return !(*this == rhs); }

    void dump() const { printf("XMM%d\n", regnum); }

    static constexpr int numRegs() { return 16; }
};

const XMMRegister XMM0(0);
const XMMRegister XMM1(1);
const XMMRegister XMM2(2);
const XMMRegister XMM3(3);
const XMMRegister XMM4(4);
const XMMRegister XMM5(5);
const XMMRegister XMM6(6);
const XMMRegister XMM7(7);
const XMMRegister XMM8(8);
const XMMRegister XMM9(9);
const XMMRegister XMM10(10);
const XMMRegister XMM11(11);
const XMMRegister XMM12(12);
const XMMRegister XMM13(13);
const XMMRegister XMM14(14);
const XMMRegister XMM15(15);

struct GenericRegister {
    union {
        Register gp;
        XMMRegister xmm;
    };

    enum Type {
        GP,
        XMM,
        None,
    } type;

    /*
    int size() const {
        switch (type) {
            case GP:
                return Register::SIZE;
            case XMM:
                return XMMRegister::SIZE;
            default:
                assert(0);
        }
        abort();
    }
    */

    explicit constexpr GenericRegister() : gp(0), type(None) {}
    constexpr GenericRegister(const Register r) : gp(r), type(GP) {}
    constexpr GenericRegister(const XMMRegister r) : xmm(r), type(XMM) {}

    void dump() const {
        if (type == GP)
            gp.dump();
        else if (type == XMM)
            xmm.dump();
        else
            abort();
    }

    static GenericRegister fromDwarf(int dwarf_regnum);
};

struct Immediate {
    uint64_t val;

    explicit Immediate(uint64_t val) : val(val) {}
    explicit Immediate(void* val) : val((uint64_t)val) {}

    bool fitsInto32Bit() const {
        uint32_t val_32bit = (uint32_t)val;
        return val_32bit == val;
    }
};

struct JumpDestination {
    enum OffsetType {
        FROM_START,
    } type;

    int offset;

    JumpDestination(OffsetType type, int64_t offset) : type(type), offset(offset) { assert(fitsInto<int32_t>(offset)); }
    static JumpDestination fromStart(int offset) { return JumpDestination(FROM_START, offset); }
};
}

struct Location {
public:
    enum LocationType : uint8_t {
        Register,
        XMMRegister,
        Stack,
        Scratch, // stack location, relative to the scratch start

        // For representing constants that fit in 32-bits, that can be encoded as immediates
        AnyReg,        // special type for use when specifying a location as a destination
        None,          // special type that represents the lack of a location, ex where a "ret void" gets returned
        Uninitialized, // special type for an uninitialized (and invalid) location
    };

public:
    LocationType type;

    union {
        // only valid if type==Register; uses X86 numbering, not dwarf numbering.
        // also valid if type==XMMRegister
        int32_t regnum;
        // only valid if type==Stack; this is the offset from bottom of the original frame.
        // ie argument #6 will have a stack_offset of 0, #7 will have a stack offset of 8, etc
        int32_t stack_offset;
        // only valid if type == Scratch; offset from the beginning of the scratch area
        int32_t scratch_offset;

        int32_t _data;
    };

    constexpr Location() noexcept : type(Uninitialized), _data(-1) {}
    constexpr Location(const Location& r) = default;
    Location& operator=(const Location& r) = default;

    constexpr Location(LocationType type, int32_t data) : type(type), _data(data) {}

    constexpr Location(assembler::Register reg) : type(Register), regnum(reg.regnum) {}

    constexpr Location(assembler::XMMRegister reg) : type(XMMRegister), regnum(reg.regnum) {}

    constexpr Location(assembler::GenericRegister reg)
        : type(reg.type == assembler::GenericRegister::GP ? Register : reg.type == assembler::GenericRegister::XMM
                                                                           ? XMMRegister
                                                                           : None),
          regnum(reg.type == assembler::GenericRegister::GP ? reg.gp.regnum : reg.xmm.regnum) {}

    assembler::Register asRegister() const;
    assembler::XMMRegister asXMMRegister() const;
    bool isClobberedByCall() const;

    static constexpr Location any() { return Location(AnyReg, 0); }
    static constexpr Location none() { return Location(None, 0); }
    static Location forArg(int argnum);
    static Location forXMMArg(int argnum);

    bool operator==(const Location rhs) const { return this->asInt() == rhs.asInt(); }

    bool operator!=(const Location rhs) const { return !(*this == rhs); }

    bool operator<(const Location& rhs) const { return this->asInt() < rhs.asInt(); }

    uint64_t asInt() const { return (int)type + ((uint64_t)_data << 4); }

    void dump() const;
};
static_assert(sizeof(Location) <= 8, "");
}

namespace std {
template <> struct hash<pyston::Location> {
    size_t operator()(const pyston::Location p) const { return p.asInt(); }
};
}

#endif
