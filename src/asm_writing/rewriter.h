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

#ifndef PYSTON_ASMWRITING_REWRITER_H
#define PYSTON_ASMWRITING_REWRITER_H

#include <map>
#include <memory>

#include "asm_writing/assembler.h"
#include "asm_writing/icinfo.h"

namespace pyston {

class TypeRecorder;

class ICInfo;
class ICSlotInfo;
class ICSlotRewrite;
class ICInvalidator;

class RewriterVar;

struct Location {
public:
    enum LocationType : uint8_t {
        Register,
        XMMRegister,
        Stack,
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

class RewriterVarUsage {
public:
    enum KillFlag {
        NoKill,
        Kill,
    };

private:
    RewriterVar* var;
    bool done_using;

    RewriterVarUsage();
    RewriterVarUsage(const RewriterVarUsage&) = delete;
    RewriterVarUsage& operator=(const RewriterVarUsage&) = delete;

    void assertValid() {
        assert(var);
        assert(!done_using);
    }

public:
    // Creates a new Usage object of this var; ownership of
    // one use of the var gets passed to this new object.
    RewriterVarUsage(RewriterVar* var);

    // Move constructor; don't need it for performance reasons, but because
    // semantically we have to pass the ownership of the use.
    RewriterVarUsage(RewriterVarUsage&& usage);
    RewriterVarUsage& operator=(RewriterVarUsage&& usage);

    static RewriterVarUsage empty();

    ~RewriterVarUsage() {
        if (!done_using) {
            setDoneUsing();
        }
    }

    void setDoneUsing();
    bool isDoneUsing();
    void ensureDoneUsing();

    RewriterVarUsage addUse();

    void addGuard(uint64_t val);
    void addGuardNotEq(uint64_t val);
    void addAttrGuard(int offset, uint64_t val, bool negate = false);
    RewriterVarUsage getAttr(int offset, KillFlag kill, Location loc = Location::any());
    void setAttr(int offset, RewriterVarUsage other);
    RewriterVarUsage cmp(AST_TYPE::AST_TYPE cmp_type, RewriterVarUsage other, Location loc = Location::any());
    RewriterVarUsage toBool(KillFlag kill, Location loc = Location::any());

    friend class Rewriter;
};

class Rewriter;
// This might make more sense as an inner class of Rewriter, but
// you can't forward-declare that :/
class RewriterVar {
private:
    Rewriter* rewriter;
    int num_uses;

    std::unordered_set<Location> locations;
    bool isInLocation(Location l);

    // Indicates that this value is a pointer to a fixed-size range in the scratch space.
    // This is a vector of variable usages that keep the range allocated.
    std::vector<RewriterVarUsage> scratch_range;

    // Gets a copy of this variable in a register, spilling/reloading if necessary.
    // TODO have to be careful with the result since the interface doesn't guarantee
    // that the register will still contain your value when you go to use it
    assembler::Register getInReg(Location l = Location::any(), bool allow_constant_in_reg = false);
    assembler::XMMRegister getInXMMReg(Location l = Location::any());

    // If this is an immediate, try getting it as one
    assembler::Immediate tryGetAsImmediate(bool* is_immediate);

    void dump();

public:
#ifndef NDEBUG
    static int nvars;
#endif
    void incUse();
    void decUse();

    RewriterVar(Rewriter* rewriter, Location location) : rewriter(rewriter), num_uses(0) {
#ifndef NDEBUG
        nvars++;
#endif
        assert(rewriter);
        locations.insert(location);
    }

#ifndef NDEBUG
    ~RewriterVar() { nvars--; }
#endif

    friend class RewriterVarUsage;
    friend class Rewriter;
};

class Rewriter : public ICSlotRewrite::CommitHook {
private:
    std::unique_ptr<ICSlotRewrite> rewrite;
    assembler::Assembler* assembler;

    const Location return_location;

    bool done_guarding;

    bool finished; // committed or aborted
#ifndef NDEBUG
    int start_vars;
#endif

    std::vector<int> live_out_regs;

    std::unordered_map<Location, RewriterVar*> vars_by_location;
    std::vector<RewriterVar*> args;
    std::vector<RewriterVar*> live_outs;

    Rewriter(ICSlotRewrite* rewrite, int num_args, const std::vector<int>& live_outs);

    void assertChangesOk() { assert(done_guarding); }

    void kill(RewriterVar* var);

    // Allocates a register.  dest must be of type Register or AnyReg
    // If otherThan is a register, guaranteed to not use that register.
    assembler::Register allocReg(Location dest, Location otherThan = Location::any());
    // Allocates an 8-byte region in the scratch space
    Location allocScratch();
    assembler::Indirect indirectFor(Location l);
    // Spills a specified register.
    // If there are open callee-save registers, takes one of those, otherwise goes on the stack
    void spillRegister(assembler::Register reg);
    // Similar, but for XMM registers (always go on the stack)
    void spillRegister(assembler::XMMRegister reg);

    // Given an empty location, do the internal bookkeeping to create a new var out of that location.
    RewriterVarUsage createNewVar(Location dest);
    // Do the bookkeeping to say that var is now also in location l
    void addLocationToVar(RewriterVar* var, Location l);
    // Do the bookkeeping to say that var is no longer in location l
    void removeLocationFromVar(RewriterVar* var, Location l);

    void finishAssembly(int continue_offset) override;

    std::pair<RewriterVarUsage, int> _allocate(int n);

    int ndecisions;
    uint64_t decision_path;

public:
    // This should be called exactly once for each argument
    RewriterVarUsage getArg(int argnum);

    ~Rewriter() {
        if (!finished)
            this->abort();
        assert(finished);

        // This check isn't thread safe and should be fine to remove if it causes
        // issues (along with the nvars/start_vars accounting)
        ASSERT(RewriterVar::nvars == start_vars, "%d %d", RewriterVar::nvars, start_vars);
    }

    Location getReturnDestination();

    bool isDoneGuarding() { return done_guarding; }
    void setDoneGuarding();

    TypeRecorder* getTypeRecorder();

    void trap();
    RewriterVarUsage loadConst(int64_t val, Location loc = Location::any());
    RewriterVarUsage call(bool can_call_into_python, void* func_addr, std::vector<RewriterVarUsage> args);
    RewriterVarUsage call(bool can_call_into_python, void* func_addr, RewriterVarUsage arg0);
    RewriterVarUsage call(bool can_call_into_python, void* func_addr, RewriterVarUsage arg0, RewriterVarUsage arg1);
    RewriterVarUsage allocate(int n);
    RewriterVarUsage allocateAndCopy(RewriterVarUsage array, int n);
    RewriterVarUsage allocateAndCopyPlus1(RewriterVarUsage first_elem, RewriterVarUsage rest, int n_rest);
    void deallocateStack(int nbytes);

    void abort();
    void commit();
    void commitReturning(RewriterVarUsage rtn);

    void addDependenceOn(ICInvalidator&);

    static Rewriter* createRewriter(void* rtn_addr, int num_args, const char* debug_name);

    void addDecision(int way);

    friend class RewriterVar;
    friend class RewriterVarUsage;
};

void* extractSlowpathFunc(uint8_t* pp_addr);

struct GRCompare {
    bool operator()(assembler::GenericRegister gr1, assembler::GenericRegister gr2) const {
        if (gr1.type != gr2.type)
            return gr1.type < gr2.type;

        if (gr1.type == assembler::GenericRegister::GP)
            return gr1.gp.regnum < gr2.gp.regnum;
        if (gr1.type == assembler::GenericRegister::XMM)
            return gr1.xmm.regnum < gr2.xmm.regnum;
        abort();
    }
};
typedef std::map<assembler::GenericRegister, StackMap::Record::Location, GRCompare> SpillMap;
// Spills the stackmap argument and guarantees that it will be readable by the unwinder.
// Updates the arguments if it did any spilling, and returns whether spilling happened.
bool spillFrameArgumentIfNecessary(StackMap::Record::Location& l, uint8_t*& inst_addr, uint8_t* inst_end,
                                   int& scratch_offset, int& scratch_size, SpillMap& remapped);

// returns (start_of_slowpath, return_addr_of_slowpath_call)
std::pair<uint8_t*, uint8_t*> initializePatchpoint3(void* slowpath_func, uint8_t* start_addr, uint8_t* end_addr,
                                                    int scratch_offset, int scratch_size,
                                                    const std::unordered_set<int>& live_outs, SpillMap& remapped);
}

#endif
