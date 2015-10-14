// Copyright (c) 2014-2015 Dropbox, Inc.  //
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

#include "asm_writing/rewriter.h"

#include <vector>

#include "asm_writing/icinfo.h"
#include "codegen/unwinding.h"
#include "core/common.h"
#include "core/stats.h"
#include "gc/heap.h"
#include "runtime/types.h"

namespace pyston {

static const assembler::Register allocatable_regs[] = {
    assembler::RAX, assembler::RCX, assembler::RDX,
    // no RSP
    // no RBP
    assembler::RDI, assembler::RSI, assembler::R8,  assembler::R9, assembler::R10, assembler::R11,

    // For now, cannot allocate callee-save registers since we do not restore them properly
    // at potentially-throwing callsites.
    // Also, if we wanted to allow spilling of existing values in callee-save registers (which
    // adding them to this list would by default enable), we would need to somehow tell our frame
    // introspection code where we spilled them to.
    //
    // TODO fix that behavior, or create an unwinder that knows how to unwind through our
    // inline caches.
    /*
    assembler::RBX, assembler::R12, assembler::R13, assembler::R14, assembler::R15,
    */
};

static const assembler::XMMRegister allocatable_xmm_regs[]
    = { assembler::XMM0,  assembler::XMM1,  assembler::XMM2,  assembler::XMM3, assembler::XMM4,  assembler::XMM5,
        assembler::XMM6,  assembler::XMM7,  assembler::XMM8,  assembler::XMM9, assembler::XMM10, assembler::XMM11,
        assembler::XMM12, assembler::XMM13, assembler::XMM14, assembler::XMM15 };

Location Location::forArg(int argnum) {
    assert(argnum >= 0);
    switch (argnum) {
        case 0:
            return assembler::RDI;
        case 1:
            return assembler::RSI;
        case 2:
            return assembler::RDX;
        case 3:
            return assembler::RCX;
        case 4:
            return assembler::R8;
        case 5:
            return assembler::R9;
        default:
            break;
    }
    int offset = (argnum - 6) * 8;
    return Location(Stack, offset);
}

assembler::Register Location::asRegister() const {
    assert(type == Register);
    return assembler::Register(regnum);
}

assembler::XMMRegister Location::asXMMRegister() const {
    assert(type == XMMRegister);
    return assembler::XMMRegister(regnum);
}

bool Location::isClobberedByCall() const {
    if (type == Register) {
        return !asRegister().isCalleeSave();
    }

    if (type == XMMRegister)
        return true;

    if (type == Scratch)
        return false;

    if (type == Stack)
        return false;

    RELEASE_ASSERT(0, "%d", type);
}

void Location::dump() const {
    if (type == Register) {
        asRegister().dump();
        return;
    }

    if (type == XMMRegister) {
        printf("%%xmm%d\n", regnum);
        return;
    }

    if (type == Scratch) {
        printf("scratch(%d)\n", scratch_offset);
        return;
    }

    if (type == Stack) {
        printf("stack(%d)\n", stack_offset);
        return;
    }

    if (type == AnyReg) {
        printf("anyreg\n");
        return;
    }

    if (type == None) {
        printf("none\n");
        return;
    }

    if (type == Uninitialized) {
        printf("uninitialized\n");
        return;
    }

    RELEASE_ASSERT(0, "%d", type);
}

Rewriter::ConstLoader::ConstLoader(Rewriter* rewriter) : rewriter(rewriter) {
}

bool Rewriter::ConstLoader::tryRegRegMove(uint64_t val, assembler::Register dst_reg) {
    assert(rewriter->phase_emitting);

    // copy the value if there is a register which contains already the value
    bool found_value = false;
    assembler::Register src_reg = findConst(val, found_value);
    if (found_value) {
        if (src_reg != dst_reg)
            rewriter->assembler->mov(src_reg, dst_reg);
        return true;
    }
    return false;
}

bool Rewriter::ConstLoader::tryLea(uint64_t val, assembler::Register dst_reg) {
    assert(rewriter->phase_emitting);

    // for large constants it maybe beneficial to create the value with a LEA from a known const value
    if (isLargeConstant(val)) {
        for (int reg_num = 0; reg_num < assembler::Register::numRegs(); ++reg_num) {
            RewriterVar* var = rewriter->vars_by_location[assembler::Register(reg_num)];
            if (var == NULL)
                continue;
            if (!var->is_constant)
                continue;

            int64_t offset = val - var->constant_value;
            if (isLargeConstant(offset))
                continue; // LEA can only handle small offsets

            rewriter->assembler->lea(assembler::Indirect(assembler::Register(reg_num), offset), dst_reg);
            return true;
        }
        // TODO: maybe add RIP relative LEA
    }
    return false;
}

void Rewriter::ConstLoader::moveImmediate(uint64_t val, assembler::Register dst_reg) {
    assert(rewriter->phase_emitting);

    // fallback use a normal: mov reg, imm
    rewriter->assembler->mov(assembler::Immediate(val), dst_reg);
}

assembler::Register Rewriter::ConstLoader::findConst(uint64_t val, bool& found_value) {
    assert(rewriter->phase_emitting);

    for (auto& p : consts) {
        if (p.first != val)
            continue;

        RewriterVar* var = p.second;
        for (Location l : var->locations) {
            if (l.type == Location::Register) {
                found_value = true;
                return l.asRegister();
            }
        }
    }

    found_value = false;
    return assembler::Register(0);
}

void Rewriter::ConstLoader::loadConstIntoReg(uint64_t val, assembler::Register dst_reg) {
    assert(rewriter->phase_emitting);

    if (val == 0) {
        rewriter->assembler->clear_reg(dst_reg);
        return;
    }

    if (tryRegRegMove(val, dst_reg))
        return;

    if (tryLea(val, dst_reg))
        return;

    moveImmediate(val, dst_reg);
}

void Rewriter::restoreArgs() {
    ASSERT(!done_guarding, "this will probably work but why are we calling this at this time");

    for (int i = 0; i < args.size(); i++) {
        args[i]->bumpUse();

        Location l = Location::forArg(i);
        if (l.type == Location::Stack)
            continue;

        assert(l.type == Location::Register);
        assembler::Register r = l.asRegister();

        if (!args[i]->isInLocation(l)) {
            allocReg(r);
            args[i]->getInReg(r);
        }
    }

    for (int i = 0; i < live_outs.size(); i++) {
        assembler::GenericRegister gr = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
        if (gr.type == assembler::GenericRegister::GP) {
            assembler::Register r = gr.gp;
            if (!live_outs[i]->isInLocation(Location(r))) {
                allocReg(r);
                live_outs[i]->getInReg(r);
                assert(live_outs[i]->isInLocation(r));
            }
        }
    }

    assertArgsInPlace();
}

void Rewriter::assertArgsInPlace() {
    ASSERT(!done_guarding, "this will probably work but why are we calling this at this time");

    for (int i = 0; i < args.size(); i++) {
        assert(args[i]->isInLocation(args[i]->arg_loc));
    }
    for (int i = 0; i < live_outs.size(); i++) {
        assembler::GenericRegister r = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
        assert(live_outs[i]->isInLocation(r));
    }
}

void RewriterVar::addGuard(uint64_t val) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    if (isConstant()) {
        RELEASE_ASSERT(constant_value == val, "added guard which is always false");
        return;
    }

    RewriterVar* val_var = rewriter->loadConst(val);
    rewriter->addAction([=]() { rewriter->_addGuard(this, val_var); }, { this, val_var }, ActionType::GUARD);
}

void Rewriter::_slowpathJump(bool condition_eq) {
    // If a jump offset is larger then 0x80 the instruction encoding requires 6bytes instead of 2bytes.
    // This adds up quickly, thats why we will try to find another jump to the slowpath with the same condition with a
    // smaller offset and jump to it / use it as a trampoline.
    // The benchmark show that this increases the performance slightly even though it introduces additional jumps.
    int& last_jmp_offset = condition_eq ? offset_eq_jmp_slowpath : offset_ne_jmp_slowpath;
    auto condition = condition_eq ? assembler::COND_EQUAL : assembler::COND_NOT_EQUAL;

    assert(assembler->bytesWritten() + assembler->bytesLeft() == rewrite->getSlotSize());
    if (last_jmp_offset != -1 && assembler->bytesLeft() >= 0x80 && assembler->bytesWritten() - last_jmp_offset < 0x80) {
        assembler->jmp_cond(assembler::JumpDestination::fromStart(last_jmp_offset), condition);
    } else {
        last_jmp_offset = assembler->bytesWritten();
        assembler->jmp_cond(assembler::JumpDestination::fromStart(rewrite->getSlotSize()), condition);
    }
}

void Rewriter::_addGuard(RewriterVar* var, RewriterVar* val_constant) {
    assembler->comment("_addGuard");

    assert(val_constant->is_constant);
    uint64_t val = val_constant->constant_value;

    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }

    restoreArgs(); // can only do movs, doesn't affect flags, so it's safe
    assertArgsInPlace();
    _slowpathJump(false /*= not equal jmp */);

    var->bumpUse();
    val_constant->bumpUse();

    assertConsistent();
}

void RewriterVar::addGuardNotEq(uint64_t val) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* val_var = rewriter->loadConst(val);
    rewriter->addAction([=]() { rewriter->_addGuardNotEq(this, val_var); }, { this, val_var }, ActionType::GUARD);
}

void Rewriter::_addGuardNotEq(RewriterVar* var, RewriterVar* val_constant) {
    assembler->comment("_addGuardNotEq");

    assert(val_constant->is_constant);
    uint64_t val = val_constant->constant_value;

    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }

    restoreArgs(); // can only do movs, doesn't affect flags, so it's safe
    assertArgsInPlace();
    _slowpathJump(true /*= equal jmp */);

    var->bumpUse();
    val_constant->bumpUse();

    assertConsistent();
}

void RewriterVar::addAttrGuard(int offset, uint64_t val, bool negate) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    if (!attr_guards.insert(std::make_tuple(offset, val, negate)).second)
        return; // duplicate guard detected

    RewriterVar* val_var = rewriter->loadConst(val);
    rewriter->addAction([=]() { rewriter->_addAttrGuard(this, offset, val_var, negate); }, { this, val_var },
                        ActionType::GUARD);
}

void Rewriter::_addAttrGuard(RewriterVar* var, int offset, RewriterVar* val_constant, bool negate) {
    assembler->comment("_addAttrGuard");

    assert(val_constant->is_constant);
    uint64_t val = val_constant->constant_value;

    // TODO if var is a constant, we will end up emitting something like
    //   mov $0x123, %rax
    //   cmp $0x10(%rax), %rdi
    // when we could just do
    //   cmp ($0x133), %rdi
    assembler::Register var_reg = var->getInReg(Location::any(), /* allow_constant_in_reg */ true);

    if (isLargeConstant(val)) {
        assembler::Register reg(0);

        if (val_constant == var) {
            // TODO This case actually shows up, but it's stuff like guarding that type_cls->cls == type_cls
            // I think we can optimize this case out, and in general, we can probably optimize out
            // any case where var is constant.
            reg = var_reg;
        } else {
            reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        }

        assembler->cmp(assembler::Indirect(var_reg, offset), reg);
    } else {
        assembler->cmp(assembler::Indirect(var_reg, offset), assembler::Immediate(val));
    }

    restoreArgs(); // can only do movs, doesn't affect flags, so it's safe
    assertArgsInPlace();
    _slowpathJump(negate);

    var->bumpUse();
    val_constant->bumpUse();

    assertConsistent();
}

RewriterVar* RewriterVar::getAttr(int offset, Location dest, assembler::MovType type) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_getAttr(result, this, offset, dest, type); }, { this }, ActionType::NORMAL);
    return result;
}

void Rewriter::_getAttr(RewriterVar* result, RewriterVar* ptr, int offset, Location dest, assembler::MovType type) {
    assembler->comment("_getAttr");

    // TODO if var is a constant, we will end up emitting something like
    //   mov $0x123, %rax
    //   mov $0x10(%rax), %rdi
    // when we could just do
    //   mov ($0x133), %rdi
    assembler::Register ptr_reg = ptr->getInReg(Location::any(), /* allow_constant_in_reg */ true);

    // It's okay to bump the use now, since it's fine to allocate the result
    // in the same register as ptr
    ptr->bumpUse();

    assembler::Register newvar_reg = result->initializeInReg(dest);
    assembler->mov_generic(assembler::Indirect(ptr_reg, offset), newvar_reg, type);

    result->releaseIfNoUses();
    assertConsistent();
}

RewriterVar* RewriterVar::getAttrDouble(int offset, Location dest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_getAttrDouble(result, this, offset, dest); }, { this }, ActionType::NORMAL);
    return result;
}

void Rewriter::_getAttrDouble(RewriterVar* result, RewriterVar* ptr, int offset, Location dest) {
    assembler->comment("_getAttrDouble");

    assembler::Register ptr_reg = ptr->getInReg();

    ptr->bumpUse();

    assembler::XMMRegister newvar_reg = result->initializeInXMMReg(dest);
    assembler->movsd(assembler::Indirect(ptr_reg, offset), newvar_reg);

    result->releaseIfNoUses();
    assertConsistent();
}

RewriterVar* RewriterVar::getAttrFloat(int offset, Location dest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_getAttrFloat(result, this, offset, dest); }, { this }, ActionType::NORMAL);
    return result;
}

void Rewriter::_getAttrFloat(RewriterVar* result, RewriterVar* ptr, int offset, Location dest) {
    assembler->comment("_getAttrFloat");

    assembler::Register ptr_reg = ptr->getInReg();

    ptr->bumpUse();

    assembler::XMMRegister newvar_reg = result->initializeInXMMReg(dest);
    assembler->movss(assembler::Indirect(ptr_reg, offset), newvar_reg);

    // cast to double
    assembler->cvtss2sd(newvar_reg, newvar_reg);

    result->releaseIfNoUses();
    assertConsistent();
}

RewriterVar* RewriterVar::cmp(AST_TYPE::AST_TYPE cmp_type, RewriterVar* other, Location dest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_cmp(result, this, cmp_type, other, dest); }, { this, other },
                        ActionType::NORMAL);
    return result;
}

void Rewriter::_cmp(RewriterVar* result, RewriterVar* v1, AST_TYPE::AST_TYPE cmp_type, RewriterVar* v2, Location dest) {
    assembler->comment("_cmp");

    assembler::Register v1_reg = v1->getInReg();
    assembler::Register v2_reg = v2->getInReg();
    assert(v1_reg != v2_reg); // TODO how do we ensure this?

    v1->bumpUse();
    v2->bumpUse();

    assembler::Register newvar_reg = allocReg(dest);
    result->initializeInReg(newvar_reg);
    assembler->cmp(v1_reg, v2_reg);
    switch (cmp_type) {
        case AST_TYPE::Eq:
            assembler->sete(newvar_reg);
            break;
        case AST_TYPE::NotEq:
            assembler->setne(newvar_reg);
            break;
        default:
            RELEASE_ASSERT(0, "%d", cmp_type);
    }

    result->releaseIfNoUses();
    assertConsistent();
}

RewriterVar* RewriterVar::toBool(Location dest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_toBool(result, this, dest); }, { this }, ActionType::NORMAL);
    return result;
}

void Rewriter::_toBool(RewriterVar* result, RewriterVar* var, Location dest) {
    assembler->comment("_toBool");

    assembler::Register this_reg = var->getInReg();

    var->bumpUse();

    assembler::Register result_reg = allocReg(dest);
    result->initializeInReg(result_reg);

    assembler->test(this_reg, this_reg);
    assembler->setnz(result_reg);

    result->releaseIfNoUses();
    assertConsistent();
}

void RewriterVar::setAttr(int offset, RewriterVar* val) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    rewriter->addAction([=]() { rewriter->_setAttr(this, offset, val); }, { this, val }, ActionType::MUTATION);
}

void Rewriter::_setAttr(RewriterVar* ptr, int offset, RewriterVar* val) {
    assembler->comment("_setAttr");

    assembler::Register ptr_reg = ptr->getInReg();

    bool is_immediate;
    assembler::Immediate imm = val->tryGetAsImmediate(&is_immediate);

    if (is_immediate) {
        assembler->movq(imm, assembler::Indirect(ptr_reg, offset));
    } else {
        assembler::Register val_reg = val->getInReg(Location::any(), false, /* otherThan */ ptr_reg);
        assert(ptr_reg != val_reg);

        assembler->mov(val_reg, assembler::Indirect(ptr_reg, offset));
    }

    ptr->bumpUse();

    // If the value is a scratch allocated memory array we have to make sure we won't release it immediately.
    // Because this setAttr stored a reference to it inside a field and the rewriter can't currently track this uses and
    // will think it's unused.
    if (val->hasScratchAllocation())
        val->resetHasScratchAllocation();
    val->bumpUse();

    assertConsistent();
}

void RewriterVar::dump() {
    printf("RewriterVar at %p: %ld locations:\n", this, locations.size());
    for (Location l : locations)
        l.dump();
}

assembler::Immediate RewriterVar::tryGetAsImmediate(bool* is_immediate) {
    if (this->is_constant && !Rewriter::isLargeConstant(this->constant_value)) {
        *is_immediate = true;
        return assembler::Immediate(this->constant_value);
    } else {
        *is_immediate = false;
        return assembler::Immediate((uint64_t)0);
    }
}

assembler::Register RewriterVar::getInReg(Location dest, bool allow_constant_in_reg, Location otherThan) {
    assert(dest.type == Location::Register || dest.type == Location::AnyReg);

#ifndef NDEBUG
    if (!allow_constant_in_reg) {
        assert(!is_constant || Rewriter::isLargeConstant(constant_value));
    }
#endif

    if (locations.size() == 0 && this->is_constant) {
        assembler::Register reg = rewriter->allocReg(dest, otherThan);
        rewriter->const_loader.loadConstIntoReg(this->constant_value, reg);
        rewriter->addLocationToVar(this, reg);
        return reg;
    }

    assert(locations.size());

    // Not sure if this is worth it,
    // but first try to see if we're already in this specific register
    for (Location l : locations) {
        if (l == dest)
            return l.asRegister();
    }

    // Then, see if we're in another register
    for (Location l : locations) {
        if (l.type == Location::Register) {
            assembler::Register reg = l.asRegister();
            if (dest.type != Location::AnyReg) {
                assembler::Register dest_reg = dest.asRegister();
                assert(dest_reg != reg); // should have been caught by the previous case

                rewriter->allocReg(dest, otherThan);

                rewriter->assembler->mov(reg, dest_reg);
                rewriter->addLocationToVar(this, dest_reg);
                return dest_reg;
            } else {
                // Probably don't need to handle `reg == otherThan` case?
                // It would mean someone is trying to allocate the same variable to
                // two different registers.
                assert(Location(reg) != otherThan);
                return reg;
            }
        }
    }

    assert(locations.size() == 1);
    Location l(*locations.begin());

    assembler::Register reg = rewriter->allocReg(dest, otherThan);
    assert(rewriter->vars_by_location.count(reg) == 0);

    if (l.type == Location::Scratch || l.type == Location::Stack) {
        assembler::Indirect mem = rewriter->indirectFor(l);
        rewriter->assembler->mov(mem, reg);
    } else {
        abort();
    }
    rewriter->addLocationToVar(this, reg);

    return reg;
}

assembler::XMMRegister RewriterVar::getInXMMReg(Location dest) {
    assert(dest.type == Location::XMMRegister || dest.type == Location::AnyReg);

    assert(!this->is_constant);
    assert(locations.size());

    // Not sure if this is worth it,
    // but first try to see if we're already in this specific register
    for (Location l : locations) {
        if (l == dest)
            return l.asXMMRegister();
    }

    // Then, see if we're in another register
    for (Location l : locations) {
        if (l.type == Location::XMMRegister) {
            assembler::XMMRegister reg = l.asXMMRegister();
            if (dest.type != Location::AnyReg) {
                assembler::XMMRegister dest_reg = dest.asXMMRegister();
                assert(dest_reg != reg); // should have been caught by the previous case

                rewriter->assembler->movsd(reg, dest_reg);
                rewriter->addLocationToVar(this, dest_reg);
                return dest_reg;
            }
            return reg;
        }
    }

    assert(locations.size() == 1);
    Location l(*locations.begin());
    assert(l.type == Location::Scratch);


    assert(dest.type == Location::XMMRegister);
    assembler::XMMRegister reg = dest.asXMMRegister();
    assert(rewriter->vars_by_location.count(reg) == 0);

    assembler::Indirect mem = rewriter->indirectFor(l);
    rewriter->assembler->movsd(mem, reg);
    rewriter->addLocationToVar(this, reg);
    return reg;
}

bool RewriterVar::isInLocation(Location l) {
    return std::find(locations.begin(), locations.end(), l) != locations.end();
}

RewriterVar* Rewriter::getArg(int argnum) {
    assert(argnum >= 0 && argnum < args.size());
    return args[argnum];
}

Location Rewriter::getReturnDestination() {
    return return_location;
}

void Rewriter::trap() {
    addAction([=]() { this->_trap(); }, {}, ActionType::NORMAL);
}

void Rewriter::_trap() {
    assembler->trap();
}

void Rewriter::addGCReference(void* obj) {
    assert(gc::isValidGCObject(obj));
    gc_references.push_back(obj);
}

RewriterVar* Rewriter::loadConst(int64_t val, Location dest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

#if MOVING_GC
    // Moving GCs need to know all existing references in the program, including those that end
    // up in compiled code.
    auto al = gc::global_heap.getAllocationFromInteriorPointer((void*)val);
    if (al) {
        gc_references.push_back(al->user_data);
    }
#endif

    for (auto& p : const_loader.consts) {
        if (p.first != val)
            continue;

        return p.second;
    }

    RewriterVar* const_loader_var = createNewConstantVar(val);
    const_loader.consts.push_back(std::make_pair(val, const_loader_var));
    return const_loader_var;
}

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar::SmallVector args;
    RewriterVar::SmallVector args_xmm;
    return call(has_side_effects, func_addr, args, args_xmm);
}

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr, RewriterVar* arg0) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar::SmallVector args;
    RewriterVar::SmallVector args_xmm;
    args.push_back(arg0);
    return call(has_side_effects, func_addr, args, args_xmm);
}

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr, RewriterVar* arg0, RewriterVar* arg1) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar::SmallVector args;
    RewriterVar::SmallVector args_xmm;
    args.push_back(arg0);
    args.push_back(arg1);
    return call(has_side_effects, func_addr, args, args_xmm);
}

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr, RewriterVar* arg0, RewriterVar* arg1,
                            RewriterVar* arg2) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar::SmallVector args;
    RewriterVar::SmallVector args_xmm;
    args.push_back(arg0);
    args.push_back(arg1);
    args.push_back(arg2);
    return call(has_side_effects, func_addr, args, args_xmm);
}

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr, RewriterVar* arg0, RewriterVar* arg1,
                            RewriterVar* arg2, RewriterVar* arg3) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar::SmallVector args;
    RewriterVar::SmallVector args_xmm;
    args.push_back(arg0);
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    return call(has_side_effects, func_addr, args, args_xmm);
}

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr, RewriterVar* arg0, RewriterVar* arg1,
                            RewriterVar* arg2, RewriterVar* arg3, RewriterVar* arg4) {
    RewriterVar::SmallVector args;
    RewriterVar::SmallVector args_xmm;
    args.push_back(arg0);
    args.push_back(arg1);
    args.push_back(arg2);
    args.push_back(arg3);
    args.push_back(arg4);
    return call(has_side_effects, func_addr, args, args_xmm);
}

static const Location caller_save_registers[]{
    assembler::RAX,   assembler::RCX,   assembler::RDX,   assembler::RSI,   assembler::RDI,
    assembler::R8,    assembler::R9,    assembler::R10,   assembler::R11,   assembler::XMM0,
    assembler::XMM1,  assembler::XMM2,  assembler::XMM3,  assembler::XMM4,  assembler::XMM5,
    assembler::XMM6,  assembler::XMM7,  assembler::XMM8,  assembler::XMM9,  assembler::XMM10,
    assembler::XMM11, assembler::XMM12, assembler::XMM13, assembler::XMM14, assembler::XMM15,
};

RewriterVar* Rewriter::call(bool has_side_effects, void* func_addr, const RewriterVar::SmallVector& args,
                            const RewriterVar::SmallVector& args_xmm) {
    RewriterVar* result = createNewVar();
    RewriterVar::SmallVector uses;
    for (RewriterVar* v : args) {
        assert(v != NULL);
        uses.push_back(v);
    }
    for (RewriterVar* v : args_xmm) {
        assert(v != NULL);
        uses.push_back(v);
    }

    ActionType type;
    if (has_side_effects)
        type = ActionType::MUTATION;
    else
        type = ActionType::NORMAL;

    // It's not nice to pass llvm::SmallVectors through a closure, especially with our SmallFunction
    // optimization, so just regionAlloc them and copy the data in:
    RewriterVar** _args = (RewriterVar**)this->regionAlloc(sizeof(RewriterVar*) * args.size());
    memcpy(_args, args.begin(), sizeof(RewriterVar*) * args.size());
    RewriterVar** _args_xmm = (RewriterVar**)this->regionAlloc(sizeof(RewriterVar*) * args_xmm.size());
    memcpy(_args_xmm, args_xmm.begin(), sizeof(RewriterVar*) * args_xmm.size());

    int args_size = args.size();
    assert(args_xmm.size() <= 0x7fff);
    // Hack: pack this into a short to make sure it fits in the closure
    short xmm_args_size = args_xmm.size();

    // Hack: explicitly order the closure arguments so they pad nicer
    addAction([args_size, xmm_args_size, has_side_effects, this, result, func_addr, _args, _args_xmm]() {
        this->_call(result, has_side_effects, func_addr, llvm::ArrayRef<RewriterVar*>(_args, args_size),
                    llvm::ArrayRef<RewriterVar*>(_args_xmm, xmm_args_size));
    }, uses, type);

    return result;
}

void Rewriter::_setupCall(bool has_side_effects, llvm::ArrayRef<RewriterVar*> args,
                          llvm::ArrayRef<RewriterVar*> args_xmm, Location preserve) {
    if (has_side_effects)
        assert(done_guarding);

    if (has_side_effects) {
        // We need some fixed amount of space at the beginning of the IC that we can use to invalidate
        // it by writing a jmp.
        // TODO this check is conservative, since actually we just have to verify that the return
        // address is at least IC_INVALDITION_HEADER_SIZE bytes past the beginning, but we're
        // checking based on the beginning of the call.  I think the load+call might actually
        // always larger than the invalidation jmp.
        while (assembler->bytesWritten() < IC_INVALDITION_HEADER_SIZE)
            assembler->nop();

        assert(assembler->bytesWritten() >= IC_INVALDITION_HEADER_SIZE);
    }

    if (has_side_effects) {
        if (!marked_inside_ic) {
            uintptr_t counter_addr = (uintptr_t)(&picked_slot->num_inside);
            if (isLargeConstant(counter_addr)) {
                assembler::Register reg = allocReg(Location::any(), preserve);
                assembler->mov(assembler::Immediate(counter_addr), reg);
                assembler->incl(assembler::Indirect(reg, 0));
            } else {
                assembler->incl(assembler::Immediate(counter_addr));
            }

            assertConsistent();
            marked_inside_ic = true;
        }
    }

    for (int i = 0; i < args.size(); i++) {
        Location l(Location::forArg(i));
        RewriterVar* var = args[i];

        if (!var->isInLocation(l)) {
            assembler::Register r = l.asRegister();

            {
                // this forces the register allocator to spill this register:
                assembler::Register r2 = allocReg(l, preserve);
                if (failed)
                    return;
                assert(r == r2);
                assert(vars_by_location.count(l) == 0);
            }

            // FIXME: get rid of tryGetAsImmediate
            // instead do that work here; ex this could be a stack location
            bool is_immediate;
            assembler::Immediate imm = var->tryGetAsImmediate(&is_immediate);

            if (is_immediate) {
                if (imm.val == 0)
                    assembler->clear_reg(r);
                else
                    assembler->mov(imm, r);
                addLocationToVar(var, l);
            } else {
                assembler::Register r2 = var->getInReg(l);
                assert(var->isInLocation(r2));
                assert(r2 == r);
            }
        }

        assert(var->isInLocation(Location::forArg(i)));
    }

    assertConsistent();

    for (int i = 0; i < args_xmm.size(); i++) {
        Location l((assembler::XMMRegister(i)));
        assert(args_xmm[i]->isInLocation(l));
    }

#ifndef NDEBUG
    for (int i = 0; i < args.size(); i++) {
        RewriterVar* var = args[i];
        if (!var->isInLocation(Location::forArg(i))) {
            var->dump();
        }
        assert(var->isInLocation(Location::forArg(i)));
    }
#endif

    // Spill caller-saved registers:
    for (auto check_reg : caller_save_registers) {
        // check_reg.dump();
        assert(check_reg.isClobberedByCall());

        RewriterVar*& var = vars_by_location[check_reg];
        if (var == NULL)
            continue;

        bool need_to_spill = true;
        for (Location l : var->locations) {
            if (!l.isClobberedByCall()) {
                need_to_spill = false;
                break;
            }
        }
        if (need_to_spill) {
            for (int i = 0; i < args.size(); i++) {
                if (args[i] == var) {
                    if (var->isDoneUsing()) {
                        // If we hold the only usage of this arg var, we are
                        // going to kill all of its usages soon anyway,
                        // so we have no need to spill it.
                        need_to_spill = false;
                    }
                    break;
                }
            }
        }

        if (need_to_spill) {
            if (check_reg.type == Location::Register) {
                spillRegister(check_reg.asRegister(), preserve);
                if (failed)
                    return;
            } else {
                assert(check_reg.type == Location::XMMRegister);
                assert(var->locations.size() == 1);
                spillRegister(check_reg.asXMMRegister());
                if (failed)
                    return;
            }
        } else {
            removeLocationFromVar(var, check_reg);
        }
    }

    assertConsistent();

#ifndef NDEBUG
    for (const auto& p : vars_by_location.getAsMap()) {
        Location l = p.first;
        // l.dump();
        if (l.isClobberedByCall()) {
            p.second->dump();
        }
        assert(!l.isClobberedByCall());
    }
#endif
}

void Rewriter::_call(RewriterVar* result, bool has_side_effects, void* func_addr, llvm::ArrayRef<RewriterVar*> args,
                     llvm::ArrayRef<RewriterVar*> args_xmm) {
    assembler->comment("_call");

    // RewriterVarUsage scratch = createNewVar(Location::any());
    assembler::Register r = allocReg(assembler::R11);
    if (failed)
        return;

    _setupCall(has_side_effects, args, args_xmm, assembler::R11);

    for (RewriterVar* arg : args) {
        arg->bumpUse();
    }
    for (RewriterVar* arg_xmm : args_xmm) {
        arg_xmm->bumpUse();
    }

    assertConsistent();

    // make sure setupCall doesn't use R11
    assert(vars_by_location.count(assembler::R11) == 0);

    uint64_t asm_address = (uint64_t)assembler->curInstPointer() + 5;
    uint64_t real_asm_address = asm_address + (uint64_t)rewrite->getSlotStart() - (uint64_t)assembler->startAddr();
    int64_t offset = (int64_t)((uint64_t)func_addr - real_asm_address);
    if (isLargeConstant(offset)) {
        const_loader.loadConstIntoReg((uint64_t)func_addr, r);
        assembler->callq(r);
    } else {
        assembler->call(assembler::Immediate(offset));
        assert(assembler->hasFailed() || asm_address == (uint64_t)assembler->curInstPointer());
    }

    if (!failed) {
        assert(vars_by_location.count(assembler::RAX) == 0);
        result->initializeInReg(assembler::RAX);

        assertConsistent();
    }

    result->releaseIfNoUses();
}

void Rewriter::abort() {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    assert(!finished);
    finished = true;
    rewrite->abort();

    static StatCounter ic_rewrites_aborted("ic_rewrites_aborted");
    ic_rewrites_aborted.log();
}

void RewriterVar::bumpUse() {
    rewriter->assertPhaseEmitting();

    next_use++;
    assert(next_use <= uses.size());
    if (next_use == uses.size()) {
        // shouldn't be clearing an arg unless we are done guarding
        if (!rewriter->done_guarding && this->is_arg) {
            return;
        }

        for (Location loc : locations) {
            rewriter->vars_by_location.erase(loc);
        }

        // releases allocated scratch space
        if (hasScratchAllocation()) {
            for (int i = 0; i < scratch_allocation.second; ++i) {
                Location l = Location(Location::Scratch, (scratch_allocation.first + i) * 8);
                assert(rewriter->vars_by_location[l] == LOCATION_PLACEHOLDER);
                rewriter->vars_by_location.erase(l);
            }
            resetHasScratchAllocation();
        }

        this->locations.clear();
    }
}

// Call this on the result at the end of the action in which it's created
// TODO we should have a better way of dealing with variables that have 0 uses
void RewriterVar::releaseIfNoUses() {
    rewriter->assertPhaseEmitting();

    if (uses.size() == 0) {
        assert(next_use == 0);
        for (Location loc : locations) {
            rewriter->vars_by_location.erase(loc);
        }

        // releases allocated scratch space
        if (hasScratchAllocation()) {
            for (int i = 0; i < scratch_allocation.second; ++i) {
                Location l = Location(Location::Scratch, (scratch_allocation.first + i) * 8);
                assert(rewriter->vars_by_location[l] == LOCATION_PLACEHOLDER);
                rewriter->vars_by_location.erase(l);
            }
            resetHasScratchAllocation();
        }

        this->locations.clear();
    }
}

void Rewriter::commit() {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    assert(!finished);
    initPhaseEmitting();

    static StatCounter ic_rewrites_aborted_assemblyfail("ic_rewrites_aborted_assemblyfail");
    static StatCounter ic_rewrites_aborted_failed("ic_rewrites_aborted_failed");

    if (failed) {
        ic_rewrites_aborted_failed.log();
        this->abort();
        return;
    }

    auto on_assemblyfail = [&]() {
        ic_rewrites_aborted_assemblyfail.log();
#if 0
        std::string per_name_stat_name = "ic_rewrites_aborted_assemblyfail_" + std::string(debugName());
        uint64_t* counter = Stats::getStatCounter(per_name_stat_name);
        Stats::log(counter);
#endif
        this->abort();
    };

    if (assembler->hasFailed()) {
        on_assemblyfail();
        return;
    }

    // Add uses for the live_outs
    for (int i = 0; i < live_outs.size(); i++) {
        live_outs[i]->uses.push_back(actions.size());
    }
    for (RewriterVar& var : vars) {
        // Add a use for every constant. This helps make constants available for the lea stuff
        // But since "spilling" a constant has no cost, it shouldn't add register pressure.
        if (var.is_constant) {
            var.uses.push_back(actions.size());
        }
    }

    assertConsistent();

    // Emit assembly for each action, and set done_guarding when
    // we reach the last guard.

    // Note: If an arg finishes its uses before we're done guarding, we don't release it at that point;
    // instead, we release it here, at the point when we set done_guarding.
    // An alternate, maybe cleaner, way to accomplish this would be to add a use for each arg
    // at each guard in the var's `uses` list.

    // First: check if we're done guarding before we even begin emitting.

    auto on_done_guarding = [&]() {
        done_guarding = true;
        for (RewriterVar* arg : args) {
            if (arg->next_use == arg->uses.size()) {
                for (Location loc : arg->locations) {
                    vars_by_location.erase(loc);
                }
                arg->locations.clear();
            }
        }
        assertConsistent();
    };

    if (last_guard_action == -1) {
        on_done_guarding();
    }

    picked_slot = rewrite->prepareEntry();
    if (picked_slot == NULL) {
        on_assemblyfail();
        return;
    }

    // Now, start emitting assembly; check if we're dong guarding after each.
    for (int i = 0; i < actions.size(); i++) {
        actions[i].action();

        if (failed) {
            ic_rewrites_aborted_failed.log();
            this->abort();
            return;
        }

        assertConsistent();
        if (i == last_guard_action) {
            on_done_guarding();
        }
    }

    if (marked_inside_ic) {
        assembler->comment("mark inside ic");

        uintptr_t counter_addr = (uintptr_t)(&picked_slot->num_inside);
        if (isLargeConstant(counter_addr)) {
            assembler::Register reg = allocReg(Location::any(), getReturnDestination());
            assembler->mov(assembler::Immediate(counter_addr), reg);
            assembler->decl(assembler::Indirect(reg, 0));
        } else {
            assembler->decl(assembler::Immediate(counter_addr));
        }
    }

    assembler->comment("live outs");

// Make sure that we have been calling bumpUse correctly.
// All uses should have been accounted for, other than the live outs
#ifndef NDEBUG
    for (RewriterVar& var : vars) {
        int num_as_live_out = 0;
        for (RewriterVar* live_out : live_outs) {
            if (live_out == &var) {
                num_as_live_out++;
            }
        }
        assert(var.next_use + num_as_live_out + (var.is_constant ? 1 : 0) == var.uses.size());
    }
#endif

    assert(live_out_regs.size() == live_outs.size());

    for (RewriterVar& var : vars) {
        if (var.is_constant) {
            var.bumpUse();
        }
    }

    // Live-outs placement: sometimes a live out can be placed into the location of a different live-out,
    // so we need to reshuffle and solve those conflicts.
    // For now, just use a simple approach, and iteratively try to move variables into place, and skip
    // them if there's a conflict.  Doesn't handle conflict cycles, but I would be very curious
    // to see us generate one of those.
    int num_to_move = live_outs.size();
    std::vector<bool> moved(num_to_move, false);
    while (num_to_move) {
        int _start_move = num_to_move;

        for (int i = 0; i < live_outs.size(); i++) {
            if (moved[i])
                continue;

            assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
            Location expected(ru);

            RewriterVar* var = live_outs[i];

            if (var->isInLocation(expected)) {
                moved[i] = true;
                num_to_move--;
                continue;
            }

            if (vars_by_location.count(expected))
                continue;

            assert(vars_by_location.count(expected) == 0);

            if (ru.type == assembler::GenericRegister::GP) {
                assembler::Register reg = var->getInReg(ru.gp);
                assert(reg == ru.gp);
            } else if (ru.type == assembler::GenericRegister::XMM) {
                assembler::XMMRegister reg = var->getInXMMReg(ru.xmm);
                assert(reg == ru.xmm);
            } else {
                RELEASE_ASSERT(0, "%d", ru.type);
            }

            // silly, but need to make a copy due to the mutations:
            for (auto l : llvm::SmallVector<Location, 8>(var->locations.begin(), var->locations.end())) {
                if (l == expected)
                    continue;
                removeLocationFromVar(var, l);
            }

            moved[i] = true;
            num_to_move--;
        }

#ifndef NDEBUG
        if (num_to_move >= _start_move) {
            for (int i = 0; i < live_outs.size(); i++) {
                printf("\n");
                assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
                Location expected(ru);
                expected.dump();

                RewriterVar* var = live_outs[i];
                for (auto l : var->locations) {
                    l.dump();
                }
            }
        }
#endif
        RELEASE_ASSERT(num_to_move < _start_move, "algorithm isn't going to terminate!");
    }

#ifndef NDEBUG
    for (int i = 0; i < live_outs.size(); i++) {
        assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
        RewriterVar* var = live_outs[i];
        assert(var->isInLocation(ru));
    }

    for (RewriterVar* live_out : live_outs) {
        live_out->bumpUse();
    }

    // At this point, all real variables should have been removed. Check that
    // anything left is the fake LOCATION_PLACEHOLDER.
    for (std::pair<Location, RewriterVar*> p : vars_by_location.getAsMap()) {
        assert(p.second == LOCATION_PLACEHOLDER);
    }
#endif

    if (assembler->hasFailed()) {
        on_assemblyfail();
        return;
    }

    uint64_t asm_size_bytes = assembler->bytesWritten();
#ifndef NDEBUG
    std::string asm_dump;
    if (ASSEMBLY_LOGGING) {
        assembler->comment("size in bytes: " + std::to_string(asm_size_bytes));
        asm_dump = assembler->dump();
    }
#endif

    rewrite->commit(this, std::move(gc_references));

    if (assembler->hasFailed()) {
        on_assemblyfail();
        return;
    }

    finished = true;

#ifndef NDEBUG
    if (ASSEMBLY_LOGGING) {
        fprintf(stderr, "%s\n\n", asm_dump.c_str());
    }
#endif

    static StatCounter ic_rewrites_committed("ic_rewrites_committed");
    ic_rewrites_committed.log();

    static StatCounter ic_rewrites_total_bytes("ic_rewrites_total_bytes");
    ic_rewrites_total_bytes.log(asm_size_bytes);
}

bool Rewriter::finishAssembly(int continue_offset) {
    assert(picked_slot);

    assembler->jmp(assembler::JumpDestination::fromStart(continue_offset));

    assembler->fillWithNops();

    return !assembler->hasFailed();
}

void Rewriter::commitReturning(RewriterVar* var) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    addAction([=]() {
        assembler->comment("commitReturning");
        var->getInReg(getReturnDestination(), true /* allow_constant_in_reg */);
        var->bumpUse();
    }, { var }, ActionType::NORMAL);

    commit();
}

void Rewriter::addDependenceOn(ICInvalidator& invalidator) {
    rewrite->addDependenceOn(invalidator);
}

void Rewriter::gc_visit(GCVisitor* visitor) {
    rewrite->gc_visit(visitor);

    // A GC could happen during a rewrite, so we need to scan the list of references
    // both as it is being built and after the rewrite commits.
    for (auto& reference : gc_references) {
        visitor->visitNonRelocatable(reference);
    }
}

Location Rewriter::allocScratch() {
    assertPhaseEmitting();

    int scratch_size = rewrite->getScratchSize();
    for (int i = 0; i < scratch_size; i += 8) {
        Location l(Location::Scratch, i);
        if (vars_by_location.count(l) == 0) {
            return l;
        }
    }
    failed = true;
    return Location(Location::None, 0);
}

RewriterVar* Rewriter::add(RewriterVar* a, int64_t b, Location dest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = createNewVar();
    addAction([=]() { this->_add(result, a, b, dest); }, { a }, ActionType::NORMAL);
    return result;
}

void Rewriter::_add(RewriterVar* result, RewriterVar* a, int64_t b, Location dest) {
    assembler->comment("_add");

    // TODO better reg alloc (e.g., mov `a` directly to the dest reg)

    assembler::Register newvar_reg = allocReg(dest);
    assembler::Register a_reg
        = a->getInReg(Location::any(), /* allow_constant_in_reg */ true, /* otherThan */ newvar_reg);
    assert(a_reg != newvar_reg);

    result->initializeInReg(newvar_reg);

    assembler->mov(a_reg, newvar_reg);

    // TODO we can't rely on this being true, so we need to support the full version
    assert(!isLargeConstant(b));
    assembler->add(assembler::Immediate(b), newvar_reg);

    a->bumpUse();

    result->releaseIfNoUses();
    assertConsistent();
}

RewriterVar* Rewriter::allocate(int n) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = createNewVar();
    addAction([=]() { this->_allocate(result, n); }, {}, ActionType::NORMAL);
    return result;
}

int Rewriter::_allocate(RewriterVar* result, int n) {
    assembler->comment("_allocate");

    assert(n >= 1);

    int scratch_size = rewrite->getScratchSize();
    int consec = 0;
    for (int i = 0; i < scratch_size; i += 8) {
        Location l(Location::Scratch, i);
        if (vars_by_location.count(l) == 0) {
            consec++;
            if (consec == n) {
                int a = i / 8 - n + 1;
                int b = i / 8;
                // Put placeholders in so the array space doesn't get re-allocated.
                // This won't get collected, but that's fine.
                // Note: make sure to do this marking before the initializeInReg call
                for (int j = a; j <= b; j++) {
                    Location m(Location::Scratch, j * 8);
                    assert(vars_by_location.count(m) == 0);
                    vars_by_location[m] = LOCATION_PLACEHOLDER;
                }

                assert(result->scratch_allocation == std::make_pair(0, 0));
                result->scratch_allocation = std::make_pair(a, n);

                assembler::Register r = result->initializeInReg();

                // TODO we could do something like we do for constants and only load
                // this when necessary, so it won't spill. Is that worth?
                assembler->lea(assembler::Indirect(assembler::RSP, 8 * a + rewrite->getScratchRspOffset()), r);

                assertConsistent();
                result->releaseIfNoUses();
                return a;
            }
        } else {
            consec = 0;
        }
    }
    failed = true;
    return 0;
}

RewriterVar* Rewriter::allocateAndCopy(RewriterVar* array_ptr, int n) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    RewriterVar* result = createNewVar();
    addAction([=]() { this->_allocateAndCopy(result, array_ptr, n); }, { array_ptr }, ActionType::NORMAL);
    return result;
}

void Rewriter::_allocateAndCopy(RewriterVar* result, RewriterVar* array_ptr, int n) {
    assembler->comment("_allocateAndCopy");

    // TODO smart register allocation

    int offset = _allocate(result, n);

    assembler::Register src_ptr = array_ptr->getInReg();
    assembler::Register tmp = allocReg(Location::any(), /* otherThan */ src_ptr);
    assert(tmp != src_ptr); // TODO how to ensure this?

    for (int i = 0; i < n; i++) {
        assembler->mov(assembler::Indirect(src_ptr, 8 * i), tmp);
        assembler->mov(tmp, assembler::Indirect(assembler::RSP, 8 * (offset + i) + rewrite->getScratchRspOffset()));
    }

    array_ptr->bumpUse();

    result->releaseIfNoUses();
    assertConsistent();
}

RewriterVar* Rewriter::allocateAndCopyPlus1(RewriterVar* first_elem, RewriterVar* rest_ptr, int n_rest) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    if (n_rest > 0)
        assert(rest_ptr != NULL);
    else
        assert(rest_ptr == NULL);

    RewriterVar* result = createNewVar();

    RewriterVar::SmallVector uses;
    uses.push_back(first_elem);
    if (rest_ptr)
        uses.push_back(rest_ptr);
    addAction([=]() { this->_allocateAndCopyPlus1(result, first_elem, rest_ptr, n_rest); }, uses, ActionType::NORMAL);
    return result;
}

void Rewriter::_allocateAndCopyPlus1(RewriterVar* result, RewriterVar* first_elem, RewriterVar* rest_ptr, int n_rest) {
    assembler->comment("_allocateAndCopyPlus1");

    int offset = _allocate(result, n_rest + 1);

    assembler::Register first_reg = first_elem->getInReg();
    assembler->mov(first_reg, assembler::Indirect(assembler::RSP, 8 * offset + rewrite->getScratchRspOffset()));

    if (n_rest > 0) {
        assembler::Register src_ptr = rest_ptr->getInReg();
        assembler::Register tmp = allocReg(Location::any(), /* otherThan */ src_ptr);
        assert(tmp != src_ptr);

        for (int i = 0; i < n_rest; i++) {
            assembler->mov(assembler::Indirect(src_ptr, 8 * i), tmp);
            assembler->mov(tmp,
                           assembler::Indirect(assembler::RSP, 8 * (offset + i + 1) + rewrite->getScratchRspOffset()));
        }
        rest_ptr->bumpUse();
    }

    first_elem->bumpUse();

    result->releaseIfNoUses();
    assertConsistent();
}

void Rewriter::checkAndThrowCAPIException(RewriterVar* r, int64_t exc_val) {
    STAT_TIMER(t0, "us_timer_rewriter", 10);

    addAction([=]() { this->_checkAndThrowCAPIException(r, exc_val); }, { r }, ActionType::MUTATION);
}

void Rewriter::_checkAndThrowCAPIException(RewriterVar* r, int64_t exc_val) {
    assembler->comment("_checkAndThrowCAPIException");

    assembler::Register var_reg = r->getInReg();
    if (exc_val == 0)
        assembler->test(var_reg, var_reg);
    else
        assembler->cmp(var_reg, assembler::Immediate(exc_val));

    {
        assembler::ForwardJump jnz(*assembler, assembler::COND_NOT_ZERO);
        assembler->mov(assembler::Immediate((void*)throwCAPIException), assembler::R11);
        assembler->callq(assembler::R11);
    }

    r->bumpUse();

    assertConsistent();
}

assembler::Indirect Rewriter::indirectFor(Location l) {
    assert(l.type == Location::Scratch || l.type == Location::Stack);

    if (l.type == Location::Scratch)
        return assembler::Indirect(assembler::RSP, rewrite->getScratchRspOffset() + l.scratch_offset);
    else
        return assembler::Indirect(assembler::RSP, l.stack_offset);
}

void Rewriter::spillRegister(assembler::Register reg, Location preserve) {
    assert(preserve.type == Location::Register || preserve.type == Location::AnyReg);

    RewriterVar* var = vars_by_location[reg];
    assert(var);

    // There may be no need to spill if the var is held in a different location already.
    // There is no need to spill if it is a constant
    if (var->locations.size() > 1 || var->is_constant) {
        removeLocationFromVar(var, reg);
        return;
    }

    // First, try to spill into a callee-save register:
    for (assembler::Register new_reg : allocatable_regs) {
        if (!new_reg.isCalleeSave())
            continue;
        if (vars_by_location.count(new_reg))
            continue;
        if (Location(new_reg) == preserve)
            continue;

        assembler->mov(reg, new_reg);

        addLocationToVar(var, new_reg);
        removeLocationFromVar(var, reg);
        return;
    }

    Location scratch = allocScratch();
    if (failed)
        return;

    assembler::Indirect mem = indirectFor(scratch);
    assembler->mov(reg, mem);
    addLocationToVar(var, scratch);
    removeLocationFromVar(var, reg);
}

void Rewriter::spillRegister(assembler::XMMRegister reg) {
    assertPhaseEmitting();

    RewriterVar* var = vars_by_location[reg];
    assert(var);

    assert(var->locations.size() == 1);

    Location scratch = allocScratch();
    assembler::Indirect mem = indirectFor(scratch);
    assembler->movsd(reg, mem);
    addLocationToVar(var, scratch);
    removeLocationFromVar(var, reg);
}

assembler::Register Rewriter::allocReg(Location dest, Location otherThan) {
    assertPhaseEmitting();

    if (dest.type == Location::AnyReg) {
        int best = -1;
        bool found = false;
        assembler::Register best_reg(0);

        // TODO prioritize spilling a constant register?
        for (assembler::Register reg : allocatable_regs) {
            if (Location(reg) != otherThan) {
                if (vars_by_location.count(reg) == 0) {
                    return reg;
                }
                RewriterVar* var = vars_by_location[reg];
                if (!done_guarding && var->is_arg && var->arg_loc == Location(reg)) {
                    continue;
                }
                if (var->uses[var->next_use] > best) {
                    found = true;
                    best = var->uses[var->next_use];
                    best_reg = reg;
                }
            }
        }

        // Spill the register whose next use is farthest in the future
        assert(found);
        spillRegister(best_reg, /* preserve */ otherThan);
        assert(failed || vars_by_location.count(best_reg) == 0);
        return best_reg;
    } else if (dest.type == Location::Register) {
        assembler::Register reg(dest.regnum);

        if (vars_by_location.count(reg)) {
            spillRegister(reg, otherThan);
        }

        assert(failed || vars_by_location.count(reg) == 0);
        return reg;
    } else {
        RELEASE_ASSERT(0, "%d", dest.type);
    }
}

assembler::XMMRegister Rewriter::allocXMMReg(Location dest, Location otherThan) {
    assertPhaseEmitting();

    if (dest.type == Location::AnyReg) {
        for (assembler::XMMRegister reg : allocatable_xmm_regs) {
            if (Location(reg) != otherThan && vars_by_location.count(reg) == 0) {
                return reg;
            }
        }
        // TODO we can have a smarter eviction strategy - we know when every variable
        // will be next used, so we should choose the one farthest in the future to evict.
        return allocXMMReg(otherThan == assembler::XMM1 ? assembler::XMM2 : assembler::XMM1);
    } else if (dest.type == Location::XMMRegister) {
        assembler::XMMRegister reg(dest.regnum);

        if (vars_by_location.count(reg)) {
            spillRegister(reg);
        }

        assert(vars_by_location.count(reg) == 0);
        return reg;
    } else {
        RELEASE_ASSERT(0, "%d", dest.type);
    }
}

void Rewriter::addLocationToVar(RewriterVar* var, Location l) {
    if (failed)
        return;
    assert(!var->isInLocation(l));
    assert(vars_by_location.count(l) == 0);

    ASSERT(l.type == Location::Register || l.type == Location::XMMRegister || l.type == Location::Scratch
               || l.type == Location::Stack,
           "%d", l.type);

    var->locations.push_back(l);
    vars_by_location[l] = var;

#ifndef NDEBUG
    // Check that the var is not in more than one of: stack, scratch, const
    int count = 0;
    if (var->is_constant && !isLargeConstant(var->constant_value)) {
        count++;
    }
    for (Location l : var->locations) {
        if (l.type == Location::Stack || l.type == Location::Scratch) {
            count++;
        }
    }
    assert(count <= 1);
#endif
}

void Rewriter::removeLocationFromVar(RewriterVar* var, Location l) {
    assert(var->isInLocation(l));
    assert(vars_by_location[l] == var);

    vars_by_location.erase(l);
    for (auto it = var->locations.begin(), it_end = var->locations.end(); it != it_end; ++it) {
        if (*it == l) {
            it = var->locations.erase(it);
            break;
        }
    }
}

RewriterVar* Rewriter::createNewVar() {
    assertPhaseCollecting();

    vars.emplace_back(this);
    return &vars.back();
}

RewriterVar* Rewriter::createNewConstantVar(uint64_t val) {
    RewriterVar* var = createNewVar();
    var->is_constant = true;
    var->constant_value = val;
    return var;
}

assembler::Register RewriterVar::initializeInReg(Location l) {
    rewriter->assertPhaseEmitting();

    // TODO um should we check this in more places, or what?
    // The thing is: if we aren't done guarding, and the register we want to use
    // is taken by an arg, we can't spill it, so we shouldn't ask to alloc it.
    if (l.type == Location::Register && !rewriter->done_guarding && rewriter->vars_by_location[l] != NULL
        && rewriter->vars_by_location[l]->is_arg) {
        l = Location::any();
    }

    assembler::Register reg = rewriter->allocReg(l);
    l = Location(reg);

    // Add this to vars_by_locations
    RewriterVar*& var = rewriter->vars_by_location[l];
    assert(!var);
    var = this;

    // Add the location to this
    assert(!isInLocation(l));
    this->locations.push_back(l);

    return reg;
}

assembler::XMMRegister RewriterVar::initializeInXMMReg(Location l) {
    rewriter->assertPhaseEmitting();

    assembler::XMMRegister reg = rewriter->allocXMMReg(l);
    l = Location(reg);

    // Add this to vars_by_locations
    RewriterVar*& var = rewriter->vars_by_location[l];
    assert(!var);
    var = this;

    // Add the location to this
    assert(!isInLocation(l));
    this->locations.push_back(l);

    return reg;
}

TypeRecorder* Rewriter::getTypeRecorder() {
    return rewrite->getTypeRecorder();
}

Rewriter::Rewriter(std::unique_ptr<ICSlotRewrite> rewrite, int num_args, const LiveOutSet& live_outs)
    : rewrite(std::move(rewrite)),
      assembler(this->rewrite->getAssembler()),
      picked_slot(NULL),
      const_loader(this),
      return_location(this->rewrite->returnRegister()),
      failed(false),
      added_changing_action(false),
      marked_inside_ic(false),
      done_guarding(false),
      last_guard_action(-1),
      offset_eq_jmp_slowpath(-1),
      offset_ne_jmp_slowpath(-1) {
    initPhaseCollecting();

    finished = false;

    for (int i = 0; i < num_args; i++) {
        Location l = Location::forArg(i);
        RewriterVar* var = createNewVar();
        addLocationToVar(var, l);

        var->is_arg = true;
        var->arg_loc = l;

        args.push_back(var);
    }

    static StatCounter ic_rewrites_starts("ic_rewrites");
    ic_rewrites_starts.log();
    static StatCounter rewriter_spillsavoided("rewriter_spillsavoided");

    // Calculate the list of live-ins based off the live-outs list,
    // and create a Use of them so that they get preserved
    for (int dwarf_regnum : live_outs) {
        assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(dwarf_regnum);

        Location l(ru);

        // We could handle this here, but for now we're assuming that the return destination
        // will get removed from this list before it gets handed to us.
        assert(l != getReturnDestination());

        if (l.isClobberedByCall()) {
            rewriter_spillsavoided.log();
        }

        RewriterVar*& var = vars_by_location[l];
        if (!var) {
            var = createNewVar();
            var->locations.push_back(l);
        }

        // Make sure there weren't duplicates in the live_outs list.
        // Probably not a big deal if there were, but we shouldn't be generating those.
        assert(std::find(this->live_out_regs.begin(), this->live_out_regs.end(), dwarf_regnum)
               == this->live_out_regs.end());

        this->live_outs.push_back(var);
        this->live_out_regs.push_back(dwarf_regnum);
    }

    // Getting the scratch space location/size wrong could be disastrous and hard to track down,
    // so here's a "forcefully check it" mode, which starts every inline cache by overwriting
    // the entire scratch space.
    bool VALIDATE_SCRATCH_SPACE = false;
    if (VALIDATE_SCRATCH_SPACE) {
        int scratch_size = this->rewrite->getScratchSize();
        for (int i = 0; i < scratch_size; i += 8) {
            assembler->movq(assembler::Immediate(0x12345678UL),
                            assembler::Indirect(assembler::RSP, i + this->rewrite->getScratchRspOffset()));
        }
    }
}

#define IC_ATTEMPTS_NAME "ic_attempts"
#define IC_ATTEMPTS_NOPATCH_NAME "ic_attempts_nopatch"
#define IC_ATTEMPTS_SKIPPED_NAME "ic_attempts_skipped"
#define IC_ATTEMPTS_SKIPPED_MEGAMORPHIC_NAME "ic_attempts_skipped_megamorphic"
#define IC_ATTEMPTS_STARTED_NAME "ic_attempts_started"

static StatCounter ic_attempts(IC_ATTEMPTS_NAME);
static StatCounter ic_attempts_nopatch(IC_ATTEMPTS_NOPATCH_NAME);
static StatCounter ic_attempts_skipped(IC_ATTEMPTS_SKIPPED_NAME);
static StatCounter ic_attempts_skipped_megamorphic(IC_ATTEMPTS_SKIPPED_MEGAMORPHIC_NAME);
static StatCounter ic_attempts_started(IC_ATTEMPTS_STARTED_NAME);

static inline void log_ic_attempts(const char* debug_name) {
    ic_attempts.log();
#if STAT_ICS
    StatCounter per_type_count(std::string(IC_ATTEMPTS_NAME) + "." + debug_name);
    per_type_count.log();
#endif
}

static inline void log_ic_attempts_nopatch(const char* debug_name) {
    ic_attempts_nopatch.log();
#if STAT_ICS
    StatCounter per_type_count(std::string(IC_ATTEMPTS_NOPATCH_NAME) + "." + debug_name);
    per_type_count.log();
#endif
}

static inline void log_ic_attempts_skipped(const char* debug_name) {
    ic_attempts_skipped.log();
#if STAT_ICS
    std::string stat_name = std::string(IC_ATTEMPTS_SKIPPED_NAME) + "." + debug_name;
    Stats::log(Stats::getStatCounter(stat_name));
#if STAT_ICS_LOCATION
    logByCurrentPythonLine(stat_name);
#endif
#endif
}

static inline void log_ic_attempts_skipped_megamorphic(const char* debug_name) {
    ic_attempts_skipped_megamorphic.log();
#if STAT_ICS
    std::string stat_name = std::string(IC_ATTEMPTS_SKIPPED_MEGAMORPHIC_NAME) + "." + debug_name;
    Stats::log(Stats::getStatCounter(stat_name));
#if STAT_ICS_LOCATION
    logByCurrentPythonLine(stat_name);
#endif
#endif
}

static inline void log_ic_attempts_started(const char* debug_name) {
    ic_attempts_started.log();
#if STAT_ICS
    StatCounter per_type_count(std::string(IC_ATTEMPTS_STARTED_NAME) + "." + debug_name);
    per_type_count.log();
#endif
}

Rewriter* Rewriter::createRewriter(void* rtn_addr, int num_args, const char* debug_name) {
    STAT_TIMER(t0, "us_timer_createrewriter", 10);

    ICInfo* ic = NULL;

    // Horrible non-robust optimization: addresses below this address are probably in the binary (ex the interpreter),
    // so don't do the more-expensive hash table lookup to find it.
    if (rtn_addr > (void*)0x1000000) {
        ic = getICInfo(rtn_addr);
    } else {
        ASSERT(!getICInfo(rtn_addr), "%p", rtn_addr);
    }

    log_ic_attempts(debug_name);

    if (!ic) {
        log_ic_attempts_nopatch(debug_name);
        return NULL;
    }

    if (!ic->shouldAttempt()) {
        log_ic_attempts_skipped(debug_name);

        if (ic->isMegamorphic())
            log_ic_attempts_skipped_megamorphic(debug_name);
        return NULL;
    }

    log_ic_attempts_started(debug_name);
    return new Rewriter(ic->startRewrite(debug_name), num_args, ic->getLiveOuts());
}

static const int INITIAL_CALL_SIZE = 13;
static const int DWARF_RBP_REGNUM = 6;
bool spillFrameArgumentIfNecessary(StackMap::Record::Location& l, uint8_t*& inst_addr, uint8_t* inst_end,
                                   int& scratch_offset, int& scratch_size, SpillMap& remapped) {
    switch (l.type) {
        case StackMap::Record::Location::LocationType::Direct:
        case StackMap::Record::Location::LocationType::Indirect:
        case StackMap::Record::Location::LocationType::Constant:
        case StackMap::Record::Location::LocationType::ConstIndex:
            return false;
        case StackMap::Record::Location::LocationType::Register: {
            assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(l.regnum);

            if (!Location(ru).isClobberedByCall())
                return false;

            auto it = remapped.find(ru);
            if (it != remapped.end()) {
                if (VERBOSITY() >= 3) {
                    printf("Already spilled ");
                    ru.dump();
                }
                l = it->second;
                return false;
            }

            if (VERBOSITY() >= 3) {
                printf("Spilling reg ");
                ru.dump();
            }

            assembler::Assembler assembler(inst_addr, inst_end - inst_addr);

            int bytes_pushed;
            if (ru.type == assembler::GenericRegister::GP) {
                auto dest = assembler::Indirect(assembler::RBP, scratch_offset);
                assembler.mov(ru.gp, dest);
                bytes_pushed = 8;
            } else if (ru.type == assembler::GenericRegister::XMM) {
                auto dest = assembler::Indirect(assembler::RBP, scratch_offset);
                assembler.movsd(ru.xmm, dest);
                bytes_pushed = 8;
            } else {
                abort();
            }

            assert(scratch_size >= bytes_pushed);
            assert(!assembler.hasFailed());

            uint8_t* cur_addr = assembler.curInstPointer();
            inst_addr = cur_addr;

            l.type = StackMap::Record::Location::LocationType::Indirect;
            l.regnum = DWARF_RBP_REGNUM;
            l.offset = scratch_offset;

            scratch_offset += bytes_pushed;
            scratch_size -= bytes_pushed;

            remapped[ru] = l;

            return true;
        }
        default:
            abort();
    }
}

void setSlowpathFunc(uint8_t* pp_addr, void* func) {
#ifndef NDEBUG
    // mov $imm, %r11:
    ASSERT(pp_addr[0] == 0x49, "%x", pp_addr[0]);
    assert(pp_addr[1] == 0xbb);
    // 8 bytes of the addr

    // callq *%r11:
    assert(pp_addr[10] == 0x41);
    assert(pp_addr[11] == 0xff);
    assert(pp_addr[12] == 0xd3);

    int i = INITIAL_CALL_SIZE;
    while (*(pp_addr + i) == 0x66 || *(pp_addr + i) == 0x0f || *(pp_addr + i) == 0x2e)
        i++;
    assert(*(pp_addr + i) == 0x90 || *(pp_addr + i) == 0x1f);
#endif

    *(void**)&pp_addr[2] = func;
}

PatchpointInitializationInfo initializePatchpoint3(void* slowpath_func, uint8_t* start_addr, uint8_t* end_addr,
                                                   int scratch_offset, int scratch_size, LiveOutSet live_outs,
                                                   SpillMap& remapped) {
    assert(start_addr < end_addr);

    int est_slowpath_size = INITIAL_CALL_SIZE;

    std::vector<assembler::GenericRegister> regs_to_spill;
    std::vector<assembler::Register> regs_to_reload;

    for (int dwarf_regnum : live_outs) {
        assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(dwarf_regnum);

        assert(!(ru.type == assembler::GenericRegister::GP && ru.gp == assembler::R11) && "We assume R11 is free!");

        if (ru.type == assembler::GenericRegister::GP) {
            if (ru.gp == assembler::RSP || ru.gp.isCalleeSave()) {
                live_outs.set(dwarf_regnum);
                continue;
            }
        }

        // Location(ru).dump();

        if (ru.type == assembler::GenericRegister::GP && remapped.count(ru)) {
            // printf("already spilled!\n");

            regs_to_reload.push_back(ru.gp);
            est_slowpath_size += 7; // 7 bytes for a single mov

            continue;
        }

        live_outs.set(dwarf_regnum);

        regs_to_spill.push_back(ru);

        if (ru.type == assembler::GenericRegister::GP)
            est_slowpath_size += 14; // 7 bytes for a mov with 4-byte displacement, needed twice
        else if (ru.type == assembler::GenericRegister::XMM)
            est_slowpath_size += 18; // (up to) 9 bytes for a movsd with 4-byte displacement, needed twice
        else
            abort();
    }

    if (VERBOSITY() >= 3)
        printf("Have to spill %ld regs around the slowpath\n", regs_to_spill.size());

    // TODO: some of these registers could already have been pushed via the frame saving code

    uint8_t* slowpath_start = end_addr - est_slowpath_size;
    ASSERT(slowpath_start >= start_addr, "Used more slowpath space than expected; change ICSetupInfo::totalSize()?");

    assembler::Assembler _a(start_addr, slowpath_start - start_addr);
    //_a.trap();
    if (slowpath_start - start_addr > 20)
        _a.jmp(assembler::JumpDestination::fromStart(slowpath_start - start_addr));
    _a.fillWithNops();

    assembler::Assembler assem(slowpath_start, end_addr - slowpath_start);
    // if (regs_to_spill.size())
    // assem.trap();
    assem.emitBatchPush(scratch_offset, scratch_size, regs_to_spill);
    uint8_t* slowpath_rtn_addr = assem.emitCall(slowpath_func, assembler::R11);
    assem.emitBatchPop(scratch_offset, scratch_size, regs_to_spill);

    // The place we should continue if we took a fast path.
    // If we have to reload things, make sure to set it to the beginning
    // of the reloading section.
    // If there's nothing to reload, as a small optimization, set it to the end of
    // the patchpoint, past any nops.
    // (Actually I think the calculations of the size above were exact so there should
    // always be 0 nops, but this optimization shouldn't hurt.)
    uint8_t* continue_addr;
    if (regs_to_reload.empty())
        continue_addr = end_addr;
    else
        continue_addr = assem.curInstPointer();

    for (assembler::Register r : regs_to_reload) {
        StackMap::Record::Location& l = remapped[r];
        assert(l.type == StackMap::Record::Location::LocationType::Indirect);
        assert(l.regnum == DWARF_RBP_REGNUM);

        assem.mov(assembler::Indirect(assembler::RBP, l.offset), r);
    }

    assem.fillWithNops();
    assert(!assem.hasFailed());

    return PatchpointInitializationInfo(slowpath_start, slowpath_rtn_addr, continue_addr, std::move(live_outs));
}

void* Rewriter::RegionAllocator::alloc(size_t bytes) {
    assert(bytes <= BLOCK_SIZE);
    if (cur_offset + bytes > BLOCK_SIZE) {
        blocks.emplace_back();
        cur_offset = 0;
    }

    char* rtn = blocks.back() + cur_offset;
    cur_offset += bytes;
    return rtn;
}
}
