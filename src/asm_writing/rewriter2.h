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

#ifndef PYSTON_ASMWRITING_REWRITER2_H
#define PYSTON_ASMWRITING_REWRITER2_H

#include <memory>

#include "asm_writing/assembler.h"
#include "asm_writing/icinfo.h"

namespace pyston {

class TypeRecorder;

class ICInfo;
class ICSlotInfo;
class ICSlotRewrite;
class ICInvalidator;

class RewriterVar2;

struct Location {
public:
    enum LocationType : uint8_t {
        Register,
        XMMRegister,
        // Stack,
        Scratch, // stack location, relative to the scratch start

        // For representing constants that fit in 32-bits, that can be encoded as immediates
        Constant,
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

        // only valid if type==Constant
        int32_t constant_val;

        int32_t _data;
    };

    constexpr Location() : type(Uninitialized), _data(-1) {}
    constexpr Location(const Location& r) : type(r.type), _data(r._data) {}
    Location operator=(const Location& r) {
        type = r.type;
        _data = r._data;
        return *this;
    }

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

    bool operator==(const Location rhs) const { return this->asInt() == rhs.asInt(); }

    bool operator!=(const Location rhs) const { return !(*this == rhs); }

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

namespace pyston {

class RewriterVarUsage2 {
public:
    enum KillFlag {
        NoKill,
        Kill,
    };

private:
    RewriterVar2* var;
    bool done_using;

    RewriterVarUsage2();
    RewriterVarUsage2(const RewriterVarUsage2&) = delete;
    RewriterVarUsage2& operator=(const RewriterVarUsage2&) = delete;

    void assertValid() {
        assert(var);
        assert(!done_using);
    }

public:
    // Creates a new Usage object of this var; ownership of
    // one use of the var gets passed to this new object.
    RewriterVarUsage2(RewriterVar2* var);

    // Move constructor; don't need it for performance reasons, but because
    // semantically we have to pass the ownership of the use.
    RewriterVarUsage2(RewriterVarUsage2&& usage);
    RewriterVarUsage2& operator=(RewriterVarUsage2&& usage);

    static RewriterVarUsage2 empty();

#ifndef NDEBUG
    ~RewriterVarUsage2() {
        if (!std::uncaught_exception())
            assert(done_using);
    }
#endif

    void setDoneUsing();

    // RewriterVarUsage2 addUse() { return var->addUse(); }
    RewriterVarUsage2 addUse();

    void addAttrGuard(int offset, uint64_t val);
    RewriterVarUsage2 getAttr(int offset, KillFlag kill, Location loc = Location::any());
    void setAttr(int offset, RewriterVarUsage2 other);

    friend class Rewriter2;
};

class Rewriter2;
// This might make more sense as an inner class of Rewriter2, but
// you can't forward-declare that :/
class RewriterVar2 {
private:
    Rewriter2* rewriter;
    int num_uses;

    std::unordered_set<Location> locations;
    bool isInLocation(Location l);

    // Gets a copy of this variable in a register, spilling/reloading if necessary.
    // TODO have to be careful with the result since the interface doesn't guarantee
    // that the register will still contain your value when you go to use it
    assembler::Register getInReg(Location l = Location::any());
    assembler::XMMRegister getInXMMReg(Location l = Location::any());

    // If this is an immediate, try getting it as one
    assembler::Immediate tryGetAsImmediate(bool* is_immediate);

    void dump();

public:
    void incUse();
    void decUse();

    RewriterVar2(Rewriter2* rewriter, Location location) : rewriter(rewriter), num_uses(1) {
        assert(rewriter);
        locations.insert(location);
    }

    friend class RewriterVarUsage2;
    friend class Rewriter2;
};

class Rewriter2 : public ICSlotRewrite::CommitHook {
private:
    std::unique_ptr<ICSlotRewrite> rewrite;
    assembler::Assembler* assembler;

    const Location return_location;

    bool done_guarding;

    std::vector<int> live_out_regs;

    std::unordered_map<Location, RewriterVar2*> vars_by_location;
    std::vector<RewriterVar2*> args;
    std::vector<RewriterVar2*> live_outs;

    Rewriter2(ICSlotRewrite* rewrite, int num_args, const std::vector<int>& live_outs);

    void assertChangesOk() { assert(done_guarding); }

    void kill(RewriterVar2* var);

    // Allocates a register.  dest must be of type Register or AnyReg
    assembler::Register allocReg(Location dest);
    // Allocates an 8-byte region in the scratch space
    Location allocScratch();
    assembler::Indirect indirectFor(Location l);
    // Spills a specified register.
    // If there are open callee-save registers, takes one of those, otherwise goes on the stack
    void spillRegister(assembler::Register reg);
    // Similar, but for XMM registers (always go on the stack)
    void spillRegister(assembler::XMMRegister reg);

    // Given an empty location, do the internal bookkeeping to create a new var out of that location.
    RewriterVarUsage2 createNewVar(Location dest);
    // Do the bookkeeping to say that var is now also in location l
    void addLocationToVar(RewriterVar2* var, Location l);
    // Do the bookkeeping to say that var is no longer in location l
    void removeLocationFromVar(RewriterVar2* var, Location l);

    void finishAssembly(int continue_offset) override;

public:
    // This should be called exactly once for each argument
    RewriterVarUsage2 getArg(int argnum);

    Location getReturnDestination();

    bool isDoneGuarding() { return done_guarding; }
    void setDoneGuarding();

    TypeRecorder* getTypeRecorder();

    void trap();
    RewriterVarUsage2 loadConst(int64_t val, Location loc = Location::any());
    RewriterVarUsage2 call(bool can_call_into_python, void* func_addr, std::vector<RewriterVarUsage2> args);
    RewriterVarUsage2 call(bool can_call_into_python, void* func_addr, RewriterVarUsage2 arg0);
    RewriterVarUsage2 call(bool can_call_into_python, void* func_addr, RewriterVarUsage2 arg0, RewriterVarUsage2 arg1);

    void commit();
    void commitReturning(RewriterVarUsage2 rtn);

    void addDependenceOn(ICInvalidator&);

    static Rewriter2* createRewriter(void* rtn_addr, int num_args, const char* debug_name);

    friend class RewriterVar2;
    friend class RewriterVarUsage2;
};
}

#endif
