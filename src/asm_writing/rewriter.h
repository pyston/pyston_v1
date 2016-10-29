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

#ifndef PYSTON_ASMWRITING_REWRITER_H
#define PYSTON_ASMWRITING_REWRITER_H

#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <memory>
#include <tuple>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"

#include "asm_writing/assembler.h"
#include "asm_writing/icinfo.h"
#include "asm_writing/types.h"
#include "core/threading.h"
#include "core/types.h"

namespace pyston {

class TypeRecorder;

class ICInfo;
struct ICSlotInfo;
class ICSlotRewrite;
class ICInvalidator;

class RewriterVar;

// Replacement for unordered_map<Location, T>
template <class T> class LocMap {
private:
    static const int N_REGS = assembler::Register::numRegs();
    static const int N_XMM = assembler::XMMRegister::numRegs();
    static const int N_SCRATCH = 32;
    static const int N_STACK = 16;

    T map_reg[N_REGS];
    T map_xmm[N_XMM];
    T map_scratch[N_SCRATCH];
    T map_stack[N_STACK];

public:
    LocMap() {
        memset(map_reg, 0, sizeof(map_reg));
        memset(map_xmm, 0, sizeof(map_xmm));
        memset(map_scratch, 0, sizeof(map_scratch));
        memset(map_stack, 0, sizeof(map_stack));
    }

    T& operator[](Location l) {
        switch (l.type) {
            case Location::Register:
                assert(0 <= l.regnum);
                assert(l.regnum < N_REGS);
                return map_reg[l.regnum];
            case Location::XMMRegister:
                assert(0 <= l.regnum);
                assert(l.regnum < N_XMM);
                return map_xmm[l.regnum];
            case Location::Stack:
                assert(0 <= l.stack_offset / 8);
                assert(l.stack_offset / 8 < N_STACK);
                return map_stack[l.stack_offset / 8];
            case Location::Scratch:
                assert(0 <= l.scratch_offset / 8);
                assert(l.scratch_offset / 8 < N_SCRATCH);
                return map_scratch[l.scratch_offset / 8];
            default:
                RELEASE_ASSERT(0, "%d", l.type);
        }
    };

    const T& operator[](Location l) const { return const_cast<T&>(*this)[l]; };

    size_t count(Location l) { return ((*this)[l] != NULL ? 1 : 0); }

    void erase(Location l) { (*this)[l] = NULL; }

#ifndef NDEBUG
    // For iterating
    // Slow so only use it in debug mode plz
    std::unordered_map<Location, T> getAsMap() {
        std::unordered_map<Location, T> m;
        for (int i = 0; i < N_REGS; i++) {
            if (map_reg[i] != NULL) {
                m.emplace(Location(Location::Register, i), map_reg[i]);
            }
        }
        for (int i = 0; i < N_XMM; i++) {
            if (map_xmm[i] != NULL) {
                m.emplace(Location(Location::XMMRegister, i), map_xmm[i]);
            }
        }
        for (int i = 0; i < N_SCRATCH; i++) {
            if (map_scratch[i] != NULL) {
                m.emplace(Location(Location::Scratch, i * 8), map_scratch[i]);
            }
        }
        for (int i = 0; i < N_STACK; i++) {
            if (map_stack[i] != NULL) {
                m.emplace(Location(Location::Stack, i * 8), map_stack[i]);
            }
        }
        return m;
    }
#endif
};

class Rewriter;
class RewriterVar;
class RewriterAction;

// This might make more sense as an inner class of Rewriter, but
// you can't forward-declare that :/
class RewriterVar {
private:
    // Fields for automatic refcounting:
    int num_refs_consumed = 0; // The number of "refConsumed()" calls on this RewriterVar
    int last_refconsumed_numuses
        = 0; // The number of uses in the `uses` array when the last refConsumed() call was made.
    RefType reftype = RefType::UNKNOWN;
    bool nullable = false;
    // Helper function: whether there is a ref that got consumed but came from the consumption of the
    // initial (owned) reference.
    bool refHandedOff();

public:
    typedef llvm::SmallVector<RewriterVar*, 8> SmallVector;

    void addGuard(uint64_t val);
    void addGuardNotEq(uint64_t val);
    void addGuardNotLt0();
    void addAttrGuard(int offset, uint64_t val, bool negate = false);
    RewriterVar* getAttr(int offset, Location loc = Location::any(), assembler::MovType type = assembler::MovType::Q);
    // getAttrFloat casts to double (maybe I should make that separate?)
    RewriterVar* getAttrFloat(int offset, Location loc = Location::any());
    RewriterVar* getAttrDouble(int offset, Location loc = Location::any());

    // SetattrType: a guardrail against the easy-to-make mistake of having a reference
    // stored in a memory location.  The refcount-tracker can't see this type of usage, so it will
    // end up decrefing the object after the store, even if the memory location is later used.
    //
    // For refcount-tracked objects, you need to specify one of two subsequent calls you will make:
    // either HANDED_OFF if the ref got handed off and you will call refConsumed(), or REF_USED if the stored ref
    // is borrowed, in which case you need to call refUsed() after the usage of the memory location.
    enum class SetattrType {
        UNKNOWN,
        HANDED_OFF,
        REF_USED,
    };
    void setAttr(int offset, RewriterVar* other, SetattrType type = SetattrType::UNKNOWN);

    // Replaces an owned ref with another one.  Does the equivalent of:
    // Box* prev = this[offset];
    // this[offset] = new_val;
    // Py_[X]DECREF(prev);
    //
    // Calls new_val->refConsumed() for you.
    void replaceAttr(int offset, RewriterVar* new_val, bool prev_nullable);

    RewriterVar* cmp(AST_TYPE::AST_TYPE cmp_type, RewriterVar* other, Location loc = Location::any());
    RewriterVar* toBool(Location loc = Location::any());

    RefType getType() const { return reftype; }
    RewriterVar* setType(RefType type);
    RewriterVar* setNullable(bool nullable) {
        this->nullable = nullable;
        return this;
    }

    // Call this to let the automatic refcount machinery know that a refcount
    // got "consumed", ie passed off.  Such as to a function that steals a reference,
    // or when stored into a memory location that is an owned reference, etc.
    // This should get called *after* the ref got consumed, ie something like
    //   r_array->setAttr(0, r_val);
    //   r_val->refConsumed()
    // if no action is specified it will assume the last action consumed the reference
    void refConsumed(RewriterAction* action = NULL);

    // registerOwnedAttr tells the refcounter that a certain memory location holds a pointer
    // to an owned reference.  This must be paired with a call to deregisterOwnedAttr
    // Call these right before emitting the store (for register) or decref (for deregister).
    void registerOwnedAttr(int byte_offset);
    void deregisterOwnedAttr(int byte_offset);


    template <typename Src, typename Dst> inline RewriterVar* getAttrCast(int offset, Location loc = Location::any());

    bool isConstant() const { return is_constant; }
    bool isContantNull() const { return isConstant() && constant_value == 0; }

protected:
    void incref();
    void decref();
    void xdecref();

private:
    Rewriter* rewriter;

    llvm::SmallVector<Location, 4> locations;
    bool isInLocation(Location l);

    // uses is a vector of the indices into the Rewriter::actions vector
    // indicated the actions that use this variable.
    // During the assembly-emitting phase, next_use is used to keep track of the next
    // use (so next_use is an index into uses).
    // Every action that uses a variable should call bumpUse on it when it's "done" with it
    // Here "done" means that it would be okay to release all of the var's locations and
    // thus allocate new variables in that same location. To be safe, you can always just
    // only call bumpUse at the end, but in some cases it may be possible earlier.
    llvm::SmallVector<int, 32> uses;
    int next_use;
    void bumpUse();

    // Helper functions for a common optimization.
    // We want to be able to call bumpUse() as soon as the register is able to be used.
    // But if we have an owned ref in that variable, we need to hold on to it until
    // the end of the operation, even though its register is theoretically available to use.
    // So before we would just call bumpUse() early.  Now we can do
    // bumpUseEarlyIfPossible();
    // /* some code */
    // bumpUseLateIfNecessary();
    void bumpUseEarlyIfPossible() {
        if (reftype != RefType::OWNED && !isScratchAllocation())
            bumpUse();
    }
    void bumpUseLateIfNecessary() {
        if (reftype == RefType::OWNED || isScratchAllocation())
            bumpUse();
    }

    // Call this on the result at the end of the action in which it's created
    // TODO we should have a better way of dealing with variables that have 0 uses
    void releaseIfNoUses();
    // Helper function to release all the resources that this var is using.
    // Don't call it directly: call bumpUse and releaseIfNoUses instead.
    void _release();
    bool isDoneUsing() { return next_use == uses.size(); }
    bool isScratchAllocation() const { return scratch_allocation.second > 0; }
    void resetIsScratchAllocation() { scratch_allocation = std::make_pair(0, 0); }
    Location getScratchLocation(int additional_offset_in_bytes = 0);
    bool needsDecref(int current_action_index);

    // Indicates if this variable is an arg, and if so, what location the arg is from.
    bool is_arg;
    bool is_constant;

    uint64_t constant_value;
    Location arg_loc;
    std::pair<int /*offset*/, int /*size*/> scratch_allocation;

    llvm::SmallSet<std::tuple<int, uint64_t, bool>, 4> attr_guards;  // used to detect duplicate guards
    llvm::SmallDenseMap<std::pair<int, int>, RewriterVar*> getattrs; // used to detect duplicate getAttrs

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
    RewriterVar(Rewriter* rewriter) : rewriter(rewriter), next_use(0), is_arg(false), is_constant(false) {
        assert(rewriter);
    }

#ifndef NDEBUG
    // For testing, reset these on deallocation so that we will see the next time they get set.
    ~RewriterVar() {
        rewriter = (Rewriter*)-1;
        reftype = (RefType)-1;
        num_refs_consumed = -11;
    }
#endif

    Rewriter* getRewriter() { return rewriter; }

    friend class Rewriter;
    friend class JitFragmentWriter;
};

// A utility class that is similar to std::function, but stores any closure data inline rather
// than in a separate allocation.  It's similar to SmallVector, but will just fail to compile if
// you try to put more bytes in than you allocated.
// Currently, it only works for functions with the signature "void()"
template <int N = 24> class SmallFunction {
private:
    void (*func)(void*);
    char data[N];

    template <typename Functor> struct Caller {
        static void call(void* data) { (*(Functor*)data)(); }
    };

public:
    template <typename Functor> SmallFunction(Functor&& f) noexcept {
// workaround missing "is_trivially_copy_constructible" in g++ < 5.0
#if __GNUG__ && __GNUC__ < 5
#define IS_TRIVIALLY_COPY_CONSTRUCTIBLE(T) std::has_trivial_copy_constructor<T>::value
#else
#define IS_TRIVIALLY_COPY_CONSTRUCTIBLE(T) std::is_trivially_copy_constructible<T>::value
#endif
        static_assert(IS_TRIVIALLY_COPY_CONSTRUCTIBLE(typename std::remove_reference<Functor>::type),
                      "SmallFunction currently only works with simple types");
        static_assert(std::is_trivially_destructible<typename std::remove_reference<Functor>::type>::value,
                      "SmallFunction currently only works with simple types");
        static_assert(sizeof(Functor) <= sizeof(data), "Please increase N");
        new (data) typename std::remove_reference<Functor>::type(std::forward<Functor>(f));
        func = Caller<Functor>::call;
    }

    SmallFunction() = default;
    SmallFunction(const SmallFunction<N>& rhs) = default;
    SmallFunction(SmallFunction<N>&& rhs) = default;
    SmallFunction& operator=(const SmallFunction<N>& rhs) = default;
    SmallFunction& operator=(SmallFunction<N>&& rhs) = default;

    void operator()() { func(data); }
};

class RewriterAction {
public:
    SmallFunction<48> action;
    std::forward_list<RewriterVar*> consumed_refs;


    template <typename F> RewriterAction(F&& action) : action(std::forward<F>(action)) {}

    RewriterAction() = default;
    RewriterAction(const RewriterAction& rhs) = default;
    RewriterAction(RewriterAction&& rhs) = default;
    RewriterAction& operator=(const RewriterAction& rhs) = default;
    RewriterAction& operator=(RewriterAction&& rhs) = default;
};

enum class ActionType { NORMAL, GUARD, MUTATION };

// non-NULL fake pointer, definitely legit
#define LOCATION_PLACEHOLDER ((RewriterVar*)1)

class Rewriter : public ICSlotRewrite::CommitHook {
private:
    // This needs to be the first member:
    llvm::BumpPtrAllocatorImpl<llvm::MallocAllocator, 512> allocator;

    // The rewriter has refcounting logic for handling owned RewriterVars.  If we own memory inside those RewriterVars,
    // we need to register that via registerOwnedAttr, which ends up here:
    llvm::DenseSet<std::pair<RewriterVar*, int /* offset */>> owned_attrs;

protected:
    // Allocates `bytes` bytes of data.  The allocation will get freed when the rewriter gets freed.
    void* regionAlloc(size_t bytes, int alignment = 16) { return allocator.Allocate(bytes, alignment); }
    template <typename T> llvm::MutableArrayRef<T> regionAlloc(size_t num_elements) {
        return llvm::MutableArrayRef<T>(allocator.Allocate<T>(num_elements), num_elements);
    }

    // This takes a variable number of llvm::ArrayRef<RewriterVar*> and copies in all elements into a single contiguous
    // memory location.
    template <typename... Args>
    llvm::MutableArrayRef<RewriterVar*> regionAllocArgs(llvm::ArrayRef<RewriterVar*> arg1, Args... args) {
        size_t num_total_args = 0;
        for (auto&& array : { arg1, args... }) {
            num_total_args += array.size();
        }
        if (num_total_args == 0)
            return llvm::MutableArrayRef<RewriterVar*>();

        auto args_array_ref = regionAlloc<RewriterVar*>(num_total_args);
        auto insert_point = args_array_ref;
        for (auto&& array : { arg1, args... }) {
            if (!array.empty()) {
                memcpy(insert_point.data(), array.data(), array.size() * sizeof(RewriterVar*));
                insert_point = insert_point.slice(array.size());
            }
        }
        assert(insert_point.size() == 0);
        return args_array_ref;
    }

    // Helps generating the best code for loading a const integer value.
    // By keeping track of the last known value of every register and reusing it.
    class ConstLoader {
    private:
        const uint64_t unknown_value = 0;
        Rewriter* rewriter;

        bool tryRegRegMove(uint64_t val, assembler::Register dst_reg);
        bool tryLea(uint64_t val, assembler::Register dst_reg);
        void moveImmediate(uint64_t val, assembler::Register dst_reg);

    public:
        ConstLoader(Rewriter* rewriter);

        // Searches if the specified value is already loaded into a register and if so it return the register
        assembler::Register findConst(uint64_t val, bool& found_value);

        // Loads the constant into the specified register
        void loadConstIntoReg(uint64_t val, assembler::Register reg);

        llvm::SmallVector<std::pair<uint64_t, RewriterVar*>, 16> consts;
    };


    std::unique_ptr<ICSlotRewrite> rewrite;
    assembler::Assembler* assembler;
    ICSlotInfo* picked_slot;

    ConstLoader const_loader;
    std::deque<RewriterVar> vars;

    const Location return_location;

    bool failed;   // if we tried to generate an invalid rewrite.
    bool finished; // committed or aborted
    const bool needs_invalidation_support;

#ifndef NDEBUG
    bool phase_emitting;
    void initPhaseCollecting() { phase_emitting = false; }
    void initPhaseEmitting() { phase_emitting = true; }
    void assertPhaseCollecting() { assert(!phase_emitting && "you should only call this in the collecting phase"); }
    void assertPhaseEmitting() { assert(phase_emitting && "you should only call this in the assembly-emitting phase"); }
#else
    void initPhaseCollecting() {}
    void initPhaseEmitting() {}
    void assertPhaseCollecting() {}
    void assertPhaseEmitting() {}
#endif

    llvm::SmallVector<int, 8> live_out_regs;

    LocMap<RewriterVar*> vars_by_location;
    llvm::SmallVector<RewriterVar*, 8> args;
    llvm::SmallVector<RewriterVar*, 8> live_outs;

    // needs_invalidation_support: whether we do some extra work to make sure that the code that this Rewriter
    // produces will support invalidation.  Normally we want this, but the baseline jit needs to turn
    // this off (for the non-IC assembly it generates).
    Rewriter(std::unique_ptr<ICSlotRewrite> rewrite, int num_args, const LiveOutSet& live_outs,
             bool needs_invalidation_support = true);

    std::deque<RewriterAction> actions;
    int current_action_idx; // in the emitting phase get's set to index of currently executed action

    template <typename F> RewriterAction* addAction(F&& action, llvm::ArrayRef<RewriterVar*> vars, ActionType type) {
        assertPhaseCollecting();
        for (RewriterVar* var : vars) {
            assert(var != NULL);
            var->uses.push_back(actions.size());
        }
        if (type == ActionType::MUTATION) {
            added_changing_action = true;
        } else if (type == ActionType::GUARD) {
            if (added_changing_action) {
                failed = true;
                return NULL;
            }
            for (RewriterVar* arg : args) {
                arg->uses.push_back(actions.size());
            }
            assert(!added_changing_action);
            last_guard_action = (int)actions.size();
        }
        actions.emplace_back(std::forward<F>(action));
        return &actions.back();
    }
    RewriterAction* getLastAction() {
        assert(!actions.empty());
        return &actions.back();
    }

    bool added_changing_action;
    bool marked_inside_ic;
    std::vector<void*> gc_references;
    std::vector<std::pair<uint64_t, std::vector<Location>>> decref_infos;

    bool done_guarding;
    bool isDoneGuarding() {
        assertPhaseEmitting();
        return done_guarding;
    }

    int last_guard_action;

    // keeps track of all jumps to the next slot so we can patch them if the size of the current slot changes
    std::vector<NextSlotJumpInfo> next_slot_jmps;

    // Move the original IC args back into their original registers:
    void restoreArgs();
    // Assert that our original args are correctly placed in case we need to
    // bail out of the IC:
    void assertArgsInPlace();

    // Allocates a register.  dest must be of type Register or AnyReg
    // If otherThan is a register, guaranteed to not use that register.
    assembler::Register allocReg(Location dest, Location otherThan = Location::any());
    assembler::Register allocReg(Location dest, Location otherThan, assembler::RegisterSet valid_registers);
    assembler::XMMRegister allocXMMReg(Location dest, Location otherThan = Location::any());
    // Allocates an 8-byte region in the scratch space
    Location allocScratch();
    assembler::Indirect indirectFor(Location l);
    // Spills a specified register.
    // If there are open callee-save registers, takes one of those, otherwise goes on the stack
    void spillRegister(assembler::Register reg, Location preserve = Location::any());
    // Similar, but for XMM registers (always go on the stack)
    void spillRegister(assembler::XMMRegister reg);

    // Create a new var with no location.
    RewriterVar* createNewVar();
    RewriterVar* createNewConstantVar(uint64_t val);

    // Do the bookkeeping to say that var is now also in location l
    void addLocationToVar(RewriterVar* var, Location l);
    // Do the bookkeeping to say that var is no longer in location l
    void removeLocationFromVar(RewriterVar* var, Location l);

    bool finishAssembly(int continue_offset, bool& should_fill_with_nops, bool& variable_size_slots) override;

    void _nextSlotJump(assembler::ConditionCode condition);
    void _trap();
    void _loadConst(RewriterVar* result, int64_t val);
    void _setupCall(bool has_side_effects, llvm::ArrayRef<RewriterVar*> args = {},
                    llvm::ArrayRef<RewriterVar*> args_xmm = {}, Location preserve = Location::any(),
                    llvm::ArrayRef<RewriterVar*> bump_if_possible = {});
    // _call does not call bumpUse on its arguments:
    void _call(RewriterVar* result, bool has_side_effects, bool can_throw, void* func_addr,
               llvm::ArrayRef<RewriterVar*> args, llvm::ArrayRef<RewriterVar*> args_xmm = {},
               llvm::ArrayRef<RewriterVar*> vars_to_bump = {});
    void _add(RewriterVar* result, RewriterVar* a, int64_t b, Location dest);
    int _allocate(RewriterVar* result, int n);
    void _allocateAndCopy(RewriterVar* result, RewriterVar* array, int n);
    void _allocateAndCopyPlus1(RewriterVar* result, RewriterVar* first_elem, RewriterVar* rest, int n_rest);
    void _checkAndThrowCAPIException(RewriterVar* r, int64_t exc_val, assembler::MovType size);

    // The public versions of these are in RewriterVar
    void _addGuard(RewriterVar* var, RewriterVar* val_constant, bool negate = false);
    void _addAttrGuard(RewriterVar* var, int offset, RewriterVar* val_constant, bool negate = false);
    void _getAttr(RewriterVar* result, RewriterVar* var, int offset, Location loc = Location::any(),
                  assembler::MovType type = assembler::MovType::Q);
    void _getAttrFloat(RewriterVar* result, RewriterVar* var, int offset, Location loc = Location::any());
    void _getAttrDouble(RewriterVar* result, RewriterVar* var, int offset, Location loc = Location::any());
    void _setAttr(RewriterVar* var, int offset, RewriterVar* other);
    void _cmp(RewriterVar* result, RewriterVar* var1, AST_TYPE::AST_TYPE cmp_type, RewriterVar* var2,
              Location loc = Location::any());
    void _toBool(RewriterVar* result, RewriterVar* var, Location loc = Location::any());

    // These do not call bumpUse on their arguments:
    void _incref(RewriterVar* var, int num_refs = 1);
    void _decref(RewriterVar* var, llvm::ArrayRef<RewriterVar*> vars_to_bump = {});
    void _xdecref(RewriterVar* var, llvm::ArrayRef<RewriterVar*> vars_to_bump = {});

    // emits a call instruction using the smallest encoding.
    // either doing a relative call if or otherwise loading the address into the supplied register and calling it.
    // the caller of this function must make sure that the supplied register can be safely overwriten.
    void _callOptimalEncoding(assembler::Register tmp_reg, void* func_addr);

    void assertConsistent() {

        if (failed)
            return;
#ifndef NDEBUG
        for (RewriterVar& var : vars) {
            for (Location l : var.locations) {
                assert(vars_by_location[l] == &var);
            }
        }
        for (std::pair<Location, RewriterVar*> p : vars_by_location.getAsMap()) {
            assert(p.second != NULL);
            if (p.second != LOCATION_PLACEHOLDER) {
                bool found = false;
                for (auto& v : vars) {
                    if (&v == p.second) {
                        found = true;
                        break;
                    }
                }
                assert(found);
                assert(p.second->isInLocation(p.first));
            }
        }
        if (!done_guarding) {
            for (RewriterVar* arg : args) {
                assert(!arg->locations.empty());
            }
        }
#endif
    }

    assembler::RegisterSet allocatable_regs;

public:
    // This should be called exactly once for each argument
    RewriterVar* getArg(int argnum);

    ~Rewriter() {
        if (!finished)
            this->abort();
        assert(finished);
        assert(gc_references.empty());
    }

    Location getReturnDestination();

    TypeRecorder* getTypeRecorder();

    const char* debugName() { return rewrite->debugName(); }

    // Register that this rewrite will embed a reference to a particular gc object.
    // TODO: come up with an implementation that is performant enough that we can automatically
    // infer these from loadConst calls.
    void addGCReference(void* obj);

#ifndef NDEBUG
    void comment(const llvm::Twine& msg);
#else
    void comment(const llvm::Twine& msg) {}
#endif
    // returns a vector of locations of variables which need to get decrefed if the last action throwes
    std::vector<Location> getDecrefLocations();
    // calls getDecrefLocations and registers the current assembler address with the retrieved decref info
    void registerDecrefInfoHere();

    void trap();
    RewriterVar* loadConst(int64_t val, Location loc = Location::any());
    // has_side_effects: whether this call could have "side effects".  the exact side effects we've
    // been concerned about have changed over time, so it's better to err on the side of saying "true",
    // but currently you can only set it to false if 1) you will not call into Python code, which basically
    // can have any sorts of side effects, but in particular could result in the IC being reentrant, and
    // 2) does not have any side-effects that would be user-visible if we bailed out from the middle of the
    // inline cache.  (Extra allocations don't count even though they're potentially visible if you look
    // hard enough.)
    RewriterVar* call(bool has_side_effects, void* func_addr, llvm::ArrayRef<RewriterVar*> args = {},
                      llvm::ArrayRef<RewriterVar*> args_xmm = {}, llvm::ArrayRef<RewriterVar*> additional_uses = {});
    template <typename... Args>
    RewriterVar* call(bool has_side_effects, void* func_addr, RewriterVar* arg1, Args... args) {
        return call(has_side_effects, func_addr, llvm::ArrayRef<RewriterVar*>({ arg1, args... }), {});
    }

    RewriterVar* add(RewriterVar* a, int64_t b, Location dest);
    // Allocates n pointer-sized stack slots:
    RewriterVar* allocate(int n);
    RewriterVar* allocateAndCopy(RewriterVar* array, int n);
    RewriterVar* allocateAndCopyPlus1(RewriterVar* first_elem, RewriterVar* rest, int n_rest);

    // This emits `if (r == exc_val) throwCAPIException()`
    // type should be either MovType::Q if you want a 64bit comparison or MovType::L for a 32bit comparison.
    void checkAndThrowCAPIException(RewriterVar* r, int64_t exc_val, assembler::MovType type);
    void checkAndThrowCAPIException(RewriterVar* r) { checkAndThrowCAPIException(r, 0, assembler::MovType::Q); }

    void abort();

    // note: commitReturning decref all of the args variables, whereas commit() does not.
    // This should probably be made more consistent, but functions that use commitReturning
    // usually want this behavior but ones that use
    void commit();
    void commitReturning(RewriterVar* rtn);
    void commitReturningNonPython(RewriterVar* rtn);

    void addDependenceOn(ICInvalidator&);

    static Rewriter* createRewriter(void* rtn_addr, int num_args, const char* debug_name);

    static bool isLargeConstant(int64_t val) { return !fitsInto<int32_t>(val); }

    // The "aggressiveness" with which we should try to do this rewrite.  It starts high, and decreases over time.
    // The values are nominally in the range 0-100, with 0 being no aggressiveness and 100 being fully aggressive,
    // but callers should be prepared to receive values from a larger range.
    //
    // It would be nice to have this be stateful so that we could support things like "Lower the aggressiveness for
    // this sub-call and then increase it back afterwards".
    int aggressiveness() {
        const ICInfo* ic = rewrite->getICInfo();
        return 100 - ic->percentBackedoff() - ic->percentMegamorphic();
    }

    friend class RewriterVar;
};

void setSlowpathFunc(uint8_t* pp_addr, void* func);

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

struct PatchpointInitializationInfo {
    uint8_t* slowpath_start;
    uint8_t* slowpath_rtn_addr;
    uint8_t* continue_addr;
    LiveOutSet live_outs;

    PatchpointInitializationInfo(uint8_t* slowpath_start, uint8_t* slowpath_rtn_addr, uint8_t* continue_addr,
                                 LiveOutSet live_outs)
        : slowpath_start(slowpath_start),
          slowpath_rtn_addr(slowpath_rtn_addr),
          continue_addr(continue_addr),
          live_outs(std::move(live_outs)) {}
};

PatchpointInitializationInfo initializePatchpoint3(void* slowpath_func, uint8_t* start_addr, uint8_t* end_addr,
                                                   int scratch_offset, int scratch_size, LiveOutSet live_outs,
                                                   SpillMap& remapped);

template <> inline RewriterVar* RewriterVar::getAttrCast<bool, bool>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::ZBL);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<char, char>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::SBL);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<int8_t, int64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::SBQ);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<int16_t, int64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::SWQ);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<int32_t, int64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::SLQ);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<int64_t, int64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::Q);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<uint8_t, uint64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::ZBQ);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<uint16_t, uint64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::ZWQ);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<uint32_t, uint64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::ZLQ);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<uint64_t, uint64_t>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::Q);
}
template <> inline RewriterVar* RewriterVar::getAttrCast<long long, long long>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::Q);
}
template <>
inline RewriterVar* RewriterVar::getAttrCast<unsigned long long, unsigned long long>(int offset, Location loc) {
    return getAttr(offset, loc, assembler::MovType::Q);
}
}

#endif
