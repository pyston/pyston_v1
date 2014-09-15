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
    static Location forXMMArg(int argnum);

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

class Rewriter;
class RewriterVar;
class RewriterAction;

// This might make more sense as an inner class of Rewriter, but
// you can't forward-declare that :/
class RewriterVar {
public:
    void addGuard(uint64_t val);
    void addGuardNotEq(uint64_t val);
    void addAttrGuard(int offset, uint64_t val, bool negate = false);
    RewriterVar* getAttr(int offset, Location loc = Location::any(), assembler::MovType type = assembler::MovType::Q);
    void setAttr(int offset, RewriterVar* other);
    RewriterVar* cmp(AST_TYPE::AST_TYPE cmp_type, RewriterVar* other, Location loc = Location::any());
    RewriterVar* toBool(Location loc = Location::any());

private:
    Rewriter* rewriter;

    std::unordered_set<Location> locations;
    bool isInLocation(Location l);

    std::vector<int> uses;
    int next_use;
    void bumpUse();
    bool isDoneUsing() { return next_use == uses.size(); }

    bool is_arg;

    // Gets a copy of this variable in a register, spilling/reloading if necessary.
    // TODO have to be careful with the result since the interface doesn't guarantee
    // that the register will still contain your value when you go to use it
    assembler::Register getInReg(Location l = Location::any(), bool allow_constant_in_reg = false,
                                 Location otherThan = Location::any());
    assembler::XMMRegister getInXMMReg(Location l = Location::any());

    assembler::Register initializeInReg(Location l = Location::any());
    assembler::XMMRegister initializeInXMMReg(Location l = Location::any());

    // If this is an immediate, try getting it as one
    assembler::Immediate tryGetAsImmediate(bool* is_immediate);

    void dump();

    RewriterVar(const RewriterVar&) = delete;
    RewriterVar& operator=(const RewriterVar&) = delete;

public:
#ifndef NDEBUG
    static int nvars;
#endif

    RewriterVar(Rewriter* rewriter) : rewriter(rewriter), next_use(0), is_arg(false) {
#ifndef NDEBUG
        nvars++;
#endif
        assert(rewriter);
    }

#ifndef NDEBUG
    ~RewriterVar() { nvars--; }
#endif

    friend class Rewriter;
};

class RewriterAction {
public:
    std::function<void()> action;

    RewriterAction(std::function<void()> f) : action(f) {}
};

enum class ActionType { NORMAL, GUARD, MUTATION };

// non-NULL fake pointer, definitely legit
#define LOCATION_PLACEHOLDER ((RewriterVar*)1)

class Rewriter : public ICSlotRewrite::CommitHook {
private:
    std::unique_ptr<ICSlotRewrite> rewrite;
    assembler::Assembler* assembler;

    std::vector<RewriterVar*> vars;

    const Location return_location;

    bool finished; // committed or aborted
#ifndef NDEBUG
    int start_vars;
#endif

    std::vector<int> live_out_regs;

    std::unordered_map<Location, RewriterVar*> vars_by_location;
    std::vector<RewriterVar*> args;
    std::vector<RewriterVar*> live_outs;

    Rewriter(ICSlotRewrite* rewrite, int num_args, const std::vector<int>& live_outs);

    std::vector<RewriterAction> actions;
    void addAction(const std::function<void()>& action, std::vector<RewriterVar*> const& vars, ActionType type) {
        for (RewriterVar* var : vars) {
            assert(var != NULL);
            var->uses.push_back(actions.size());
        }
        if (type == ActionType::MUTATION) {
            added_changing_action = true;
        } else if (type == ActionType::GUARD) {
            assert(!added_changing_action);
            last_guard_action = (int)actions.size();
        }
        actions.emplace_back(action);
    }
    bool added_changing_action;
    int last_guard_action;

    bool done_guarding;
    bool isDoneGuarding() { return done_guarding; }

    // Allocates a register.  dest must be of type Register or AnyReg
    // If otherThan is a register, guaranteed to not use that register.
    assembler::Register allocReg(Location dest, Location otherThan = Location::any());
    assembler::XMMRegister allocXMMReg(Location dest, Location otherThan = Location::any());
    // Allocates an 8-byte region in the scratch space
    Location allocScratch();
    assembler::Indirect indirectFor(Location l);
    // Spills a specified register.
    // If there are open callee-save registers, takes one of those, otherwise goes on the stack
    void spillRegister(assembler::Register reg);
    // Similar, but for XMM registers (always go on the stack)
    void spillRegister(assembler::XMMRegister reg);

    // Create a new var with no location.
    RewriterVar* createNewVar();
    // Do the bookkeeping to say that var is now also in location l
    void addLocationToVar(RewriterVar* var, Location l);
    // Do the bookkeeping to say that var is no longer in location l
    void removeLocationFromVar(RewriterVar* var, Location l);

    void finishAssembly(int continue_offset) override;

    int ndecisions;
    uint64_t decision_path;

    void _trap();
    void _loadConst(RewriterVar* result, int64_t val, Location loc);
    void _call(RewriterVar* result, bool can_call_into_python, void* func_addr, const std::vector<RewriterVar*>& args,
               const std::vector<RewriterVar*>& args_xmm);
    int _allocate(RewriterVar* result, int n);
    void _allocateAndCopy(RewriterVar* result, RewriterVar* array, int n);
    void _allocateAndCopyPlus1(RewriterVar* result, RewriterVar* first_elem, RewriterVar* rest, int n_rest);

    // The public versions of these are in RewriterVar
    void _addGuard(RewriterVar* var, uint64_t val);
    void _addGuardNotEq(RewriterVar* var, uint64_t val);
    void _addAttrGuard(RewriterVar* var, int offset, uint64_t val, bool negate = false);
    void _getAttr(RewriterVar* result, RewriterVar* var, int offset, Location loc = Location::any(),
                  assembler::MovType type = assembler::MovType::Q);
    void _setAttr(RewriterVar* var, int offset, RewriterVar* other);
    void _cmp(RewriterVar* result, RewriterVar* var1, AST_TYPE::AST_TYPE cmp_type, RewriterVar* var2,
              Location loc = Location::any());
    void _toBool(RewriterVar* result, RewriterVar* var, Location loc = Location::any());

    void assertConsistent() {
#ifndef NDEBUG
        for (RewriterVar* var : vars) {
            for (Location l : var->locations) {
                assert(vars_by_location[l] == var);
            }
        }
        for (std::pair<Location, RewriterVar*> p : vars_by_location) {
            assert(p.second != NULL);
            if (p.second != LOCATION_PLACEHOLDER) {
                assert(std::find(vars.begin(), vars.end(), p.second) != vars.end());
                assert(p.second->locations.count(p.first) == 1);
            }
        }
#endif
    }

public:
    // This should be called exactly once for each argument
    RewriterVar* getArg(int argnum);

    ~Rewriter() {
        if (!finished)
            this->abort();
        assert(finished);

        for (RewriterVar* var : vars) {
            delete var;
        }

        // This check isn't thread safe and should be fine to remove if it causes
        // issues (along with the nvars/start_vars accounting)
        ASSERT(RewriterVar::nvars == start_vars, "%d %d", RewriterVar::nvars, start_vars);
    }

    Location getReturnDestination();

    TypeRecorder* getTypeRecorder();

    void trap();
    RewriterVar* loadConst(int64_t val, Location loc = Location::any());
    RewriterVar* call(bool can_call_into_python, void* func_addr, const std::vector<RewriterVar*>& args,
                      const std::vector<RewriterVar*>& args_xmm = std::vector<RewriterVar*>());
    RewriterVar* call(bool can_call_into_python, void* func_addr, RewriterVar* arg0);
    RewriterVar* call(bool can_call_into_python, void* func_addr, RewriterVar* arg0, RewriterVar* arg1);
    RewriterVar* allocate(int n);
    RewriterVar* allocateAndCopy(RewriterVar* array, int n);
    RewriterVar* allocateAndCopyPlus1(RewriterVar* first_elem, RewriterVar* rest, int n_rest);

    void abort();
    void commit();
    void commitReturning(RewriterVar* rtn);

    void addDependenceOn(ICInvalidator&);

    static Rewriter* createRewriter(void* rtn_addr, int num_args, const char* debug_name);

    void addDecision(int way);

    friend class RewriterVar;
};
}

#endif
