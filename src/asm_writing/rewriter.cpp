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

#include "asm_writing/rewriter.h"

#include <vector>

#include "asm_writing/icinfo.h"
#include "core/common.h"
#include "core/stats.h"

namespace pyston {

static const assembler::Register allocatable_regs[] = {
    assembler::RAX, assembler::RCX, assembler::RBX, assembler::RDX,
    // no RSP
    // no RBP
    assembler::RDI, assembler::RSI, assembler::R8,  assembler::R9,  assembler::R10,
    assembler::R11, assembler::R12, assembler::R13, assembler::R14, assembler::R15,
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

    if (type == Constant)
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

    if (type == Constant) {
        printf("imm(%d)\n", constant_val);
        return;
    }

    if (type == Stack) {
        printf("stack(%d)\n", stack_offset);
        return;
    }

    RELEASE_ASSERT(0, "%d", type);
}

static bool isLargeConstant(uint64_t val) {
    return (val < (-1L << 31) || val >= (1L << 31) - 1);
}

void RewriterVar::addGuard(uint64_t val) {
    rewriter->addAction([=]() { rewriter->_addGuard(this, val); }, { this }, ActionType::GUARD);
}

void Rewriter::_addGuard(RewriterVar* var, uint64_t val) {
    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = allocReg(Location::any());
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }
    assembler->jne(assembler::JumpDestination::fromStart(rewrite->getSlotSize()));

    var->bumpUse();
}

void RewriterVar::addGuardNotEq(uint64_t val) {
    rewriter->addAction([=]() { rewriter->_addGuardNotEq(this, val); }, { this }, ActionType::GUARD);
}

void Rewriter::_addGuardNotEq(RewriterVar* var, uint64_t val) {
    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = allocReg(Location::any());
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }
    assembler->je(assembler::JumpDestination::fromStart(rewrite->getSlotSize()));

    var->bumpUse();
}

void RewriterVar::addAttrGuard(int offset, uint64_t val, bool negate) {
    rewriter->addAction([=]() { rewriter->_addAttrGuard(this, offset, val, negate); }, { this }, ActionType::GUARD);
}

void Rewriter::_addAttrGuard(RewriterVar* var, int offset, uint64_t val, bool negate) {
    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = allocReg(Location::any(), /* otherThan */ var_reg);
        assert(reg != var_reg);
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(assembler::Indirect(var_reg, offset), reg);
    } else {
        assembler->cmp(assembler::Indirect(var_reg, offset), assembler::Immediate(val));
    }
    if (negate)
        assembler->je(assembler::JumpDestination::fromStart(rewrite->getSlotSize()));
    else
        assembler->jne(assembler::JumpDestination::fromStart(rewrite->getSlotSize()));

    var->bumpUse();
}

RewriterVar* RewriterVar::getAttr(int offset, Location dest, assembler::MovType type) {
    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_getAttr(result, this, offset, dest, type); }, { this }, ActionType::NORMAL);
    return result;
}

void Rewriter::_getAttr(RewriterVar* result, RewriterVar* ptr, int offset, Location dest, assembler::MovType type) {
    assembler::Register ptr_reg = ptr->getInReg();

    // It's okay to bump the use now, since it's fine to allocate the result
    // in the same register as ptr
    ptr->bumpUse();

    if (dest.type == Location::XMMRegister) {
        assembler::XMMRegister newvar_reg = result->initializeInXMMReg(dest);
        assembler->movsd(assembler::Indirect(ptr_reg, offset), newvar_reg);
    } else {
        assembler::Register newvar_reg = result->initializeInReg(dest);
        assembler->mov(assembler::Indirect(ptr_reg, offset), newvar_reg, type);
    }
}

RewriterVar* RewriterVar::cmp(AST_TYPE::AST_TYPE cmp_type, RewriterVar* other, Location dest) {
    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_cmp(result, this, cmp_type, other, dest); }, { this, other },
                        ActionType::NORMAL);
    return result;
}

void Rewriter::_cmp(RewriterVar* result, RewriterVar* v1, AST_TYPE::AST_TYPE cmp_type, RewriterVar* v2, Location dest) {
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
}

RewriterVar* RewriterVar::toBool(Location dest) {
    RewriterVar* result = rewriter->createNewVar();
    rewriter->addAction([=]() { rewriter->_toBool(result, this, dest); }, { this }, ActionType::NORMAL);
    return result;
}

void Rewriter::_toBool(RewriterVar* result, RewriterVar* var, Location dest) {
    assembler::Register this_reg = var->getInReg();

    var->bumpUse();

    assembler::Register result_reg = allocReg(dest);
    result->initializeInReg(result_reg);

    assembler->test(this_reg, this_reg);
    assembler->setnz(result_reg);
}

void RewriterVar::setAttr(int offset, RewriterVar* val) {
    rewriter->addAction([=]() { rewriter->_setAttr(this, offset, val); }, { this, val }, ActionType::MUTATION);
}

void Rewriter::_setAttr(RewriterVar* ptr, int offset, RewriterVar* val) {
    assembler::Register ptr_reg = ptr->getInReg();

    bool is_immediate;
    assembler::Immediate imm = ptr->tryGetAsImmediate(&is_immediate);

    if (is_immediate) {
        assembler->movq(imm, assembler::Indirect(ptr_reg, offset));
    } else {
        assembler::Register val_reg = val->getInReg(Location::any(), false, /* otherThan */ ptr_reg);
        assert(ptr_reg != val_reg);

        assembler->mov(val_reg, assembler::Indirect(ptr_reg, offset));
    }

    ptr->bumpUse();
    val->bumpUse();
}

void RewriterVar::dump() {
    printf("RewriterVar at %p: %ld locations:\n", this, locations.size());
    for (Location l : locations)
        l.dump();
}

assembler::Immediate RewriterVar::tryGetAsImmediate(bool* is_immediate) {
    for (Location l : locations) {
        if (l.type == Location::Constant) {
            *is_immediate = true;
            return assembler::Immediate(l.constant_val);
        }
    }
    *is_immediate = false;
    return assembler::Immediate((uint64_t)0);
}

assembler::Register RewriterVar::getInReg(Location dest, bool allow_constant_in_reg, Location otherThan) {
    assert(dest.type == Location::Register || dest.type == Location::AnyReg);

    // assembler::Register reg = var->rewriter->allocReg(l);
    // var->rewriter->addLocationToVar(var, reg);
    // return reg;
    assert(locations.size());
#ifndef NDEBUG
    if (!allow_constant_in_reg) {
        for (Location l : locations) {
            ASSERT(l.type != Location::Constant, "why do you want this in a register?");
        }
    }
#endif

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

    if (l.type == Location::Constant) {
        rewriter->assembler->mov(assembler::Immediate(l.constant_val), reg);
    } else if (l.type == Location::Scratch || l.type == Location::Stack) {
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

    assert(locations.size());
#ifndef NDEBUG
    for (Location l : locations) {
        ASSERT(l.type != Location::Constant, "why do you want this in a register?");
    }
#endif

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
    return locations.count(l) != 0;
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

RewriterVar* Rewriter::loadConst(int64_t val, Location dest) {
    if (!isLargeConstant(val)) {
        Location l(Location::Constant, val);
        RewriterVar*& var = vars_by_location[l];
        if (!var) {
            var = createNewVar();
            var->locations.insert(l);
        }
        return var;
    } else {
        RewriterVar* result = createNewVar();
        addAction([=]() { this->_loadConst(result, val, dest); }, {}, ActionType::NORMAL);
        return result;
    }
}

void Rewriter::_loadConst(RewriterVar* result, int64_t val, Location dest) {
    assembler::Register reg = allocReg(dest);
    assembler->mov(assembler::Immediate(val), reg);
    result->initializeInReg(reg);
}

RewriterVar* Rewriter::call(bool can_call_into_python, void* func_addr, RewriterVar* arg0) {
    std::vector<RewriterVar*> args = { arg0 };
    std::vector<RewriterVar*> args_xmm = {};
    return call(can_call_into_python, func_addr, args, args_xmm);
}

RewriterVar* Rewriter::call(bool can_call_into_python, void* func_addr, RewriterVar* arg0, RewriterVar* arg1) {
    std::vector<RewriterVar*> args = { arg0, arg1 };
    std::vector<RewriterVar*> args_xmm = {};
    return call(can_call_into_python, func_addr, args, args_xmm);
}

static const Location caller_save_registers[]{
    assembler::RAX,   assembler::RCX,   assembler::RDX,   assembler::RSI,   assembler::RDI,
    assembler::R8,    assembler::R9,    assembler::R10,   assembler::R11,   assembler::XMM0,
    assembler::XMM1,  assembler::XMM2,  assembler::XMM3,  assembler::XMM4,  assembler::XMM5,
    assembler::XMM6,  assembler::XMM7,  assembler::XMM8,  assembler::XMM9,  assembler::XMM10,
    assembler::XMM11, assembler::XMM12, assembler::XMM13, assembler::XMM14, assembler::XMM15,
};

RewriterVar* Rewriter::call(bool can_call_into_python, void* func_addr, const std::vector<RewriterVar*>& args,
                            const std::vector<RewriterVar*>& args_xmm) {
    RewriterVar* result = createNewVar();
    std::vector<RewriterVar*> uses;
    for (RewriterVar* v : args) {
        assert(v != NULL);
        uses.push_back(v);
    }
    for (RewriterVar* v : args_xmm) {
        assert(v != NULL);
        uses.push_back(v);
    }
    addAction([=]() { this->_call(result, can_call_into_python, func_addr, args, args_xmm); }, uses,
              ActionType::MUTATION);
    return result;
}

void Rewriter::_call(RewriterVar* result, bool can_call_into_python, void* func_addr,
                     const std::vector<RewriterVar*>& args, const std::vector<RewriterVar*>& args_xmm) {
    // TODO figure out why this is here -- what needs to be done differently
    // if can_call_into_python is true?
    // assert(!can_call_into_python);

    // RewriterVarUsage scratch = createNewVar(Location::any());
    assembler::Register r = allocReg(assembler::R11);

    for (int i = 0; i < args.size(); i++) {
        Location l(Location::forArg(i));
        RewriterVar* var = args[i];

        if (!var->isInLocation(l)) {
            assembler::Register r = l.asRegister();

            {
                // this forces the register allocator to spill this register:
                assembler::Register r2 = allocReg(l);
                assert(r == r2);
                assert(vars_by_location.count(l) == 0);
            }

            // FIXME: get rid of tryGetAsImmediate
            // instead do that work here; ex this could be a stack location
            bool is_immediate;
            assembler::Immediate imm = var->tryGetAsImmediate(&is_immediate);

            if (is_immediate) {
                assembler->mov(imm, r);
                addLocationToVar(var, l);
            } else {
                assembler::Register r2 = var->getInReg(l);
                assert(var->locations.count(r2));
                assert(r2 == r);
            }
        }

        assert(var->isInLocation(Location::forArg(i)));
    }

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

    for (RewriterVar* arg : args) {
        arg->bumpUse();
    }
    for (RewriterVar* arg_xmm : args_xmm) {
        arg_xmm->bumpUse();
    }

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
                spillRegister(check_reg.asRegister());
            } else {
                assert(check_reg.type == Location::XMMRegister);
                assert(var->locations.size() == 1);
                spillRegister(check_reg.asXMMRegister());
            }
        } else {
            removeLocationFromVar(var, check_reg);
        }
    }

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

    assembler->mov(assembler::Immediate(func_addr), r);
    assembler->callq(r);

    assert(vars_by_location.count(assembler::RAX) == 0);
    result->initializeInReg(assembler::RAX);
}

void Rewriter::abort() {
    assert(!finished);
    finished = true;
    rewrite->abort();

    static StatCounter rewriter_aborts("rewriter_aborts");
    rewriter_aborts.log();
}

void RewriterVar::bumpUse() {
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
        this->locations.clear();
    }
}

void Rewriter::commit() {
    assert(!finished);

    if (assembler->hasFailed()) {
        static StatCounter rewriter_assemblyfail("rewriter_assemblyfail");
        rewriter_assemblyfail.log();

        this->abort();
        return;
    }

    finished = true;

    static StatCounter rewriter_commits("rewriter_commits");
    rewriter_commits.log();

    // Add uses for the live_outs
    for (int i = 0; i < live_outs.size(); i++) {
        live_outs[i]->uses.push_back(actions.size());
    }

    assertConsistent();

    // Emit assembly for each action, and set done_guarding when
    // we reach the last guard.
    if (last_guard_action == -1) {
        done_guarding = true;

        // maybe we should track this like we do the other uses
        for (RewriterVar* arg : args) {
            if (arg->next_use == arg->uses.size()) {
                for (Location loc : arg->locations) {
                    vars_by_location.erase(loc);
                }
                arg->locations.clear();
            }
        }
    }

    assertConsistent();

    for (int i = 0; i < actions.size(); i++) {
        actions[i].action();

        assertConsistent();
        if (i == last_guard_action) {
            done_guarding = true;
            for (RewriterVar* arg : args) {
                if (arg->next_use == arg->uses.size()) {
                    for (Location loc : arg->locations) {
                        vars_by_location.erase(loc);
                    }
                    arg->locations.clear();
                }
            }
        }
        assertConsistent();
    }

// Make sure that we have been calling bumpUse correctly.
// All uses should have been accounted for, other than the live outs
#ifndef NDEBUG
    for (RewriterVar* var : vars) {
        int num_as_live_out = 0;
        for (RewriterVar* live_out : live_outs) {
            if (live_out == var) {
                num_as_live_out++;
            }
        }
        assert(var->next_use + num_as_live_out == var->uses.size());
    }
#endif

    assert(live_out_regs.size() == live_outs.size());

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
            for (auto l : std::vector<Location>(var->locations.begin(), var->locations.end())) {
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

    // To make this check work, we need to kill vars which have 0 uses
    // TODO we should not emit any code for these variables...
    for (RewriterVar* var : vars) {
        if (var->uses.size() == 0) {
            for (Location l : var->locations) {
                vars_by_location.erase(l);
            }
            var->locations.clear();
        }
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

    rewrite->commit(decision_path, this);
}

void Rewriter::finishAssembly(int continue_offset) {
    assembler->jmp(assembler::JumpDestination::fromStart(continue_offset));

    assembler->fillWithNops();
}

void Rewriter::commitReturning(RewriterVar* var) {
    addAction([=]() {
                  var->getInReg(getReturnDestination(), true /* allow_constant_in_reg */);
                  var->bumpUse();
              },
              { var }, ActionType::NORMAL);

    commit();
}

void Rewriter::addDecision(int way) {
    assert(ndecisions < 60);
    ndecisions++;
    decision_path = (decision_path << 1) | way;
}

void Rewriter::addDependenceOn(ICInvalidator& invalidator) {
    rewrite->addDependenceOn(invalidator);
}

Location Rewriter::allocScratch() {
    int scratch_bytes = rewrite->getScratchBytes();
    for (int i = 0; i < scratch_bytes; i += 8) {
        Location l(Location::Scratch, i);
        if (vars_by_location.count(l) == 0) {
            return l;
        }
    }
    RELEASE_ASSERT(0, "Using all %d bytes of scratch!", scratch_bytes);
}

RewriterVar* Rewriter::allocate(int n) {
    RewriterVar* result = createNewVar();
    addAction([=]() { this->_allocate(result, n); }, {}, ActionType::NORMAL);
    return result;
}

int Rewriter::_allocate(RewriterVar* result, int n) {
    assert(n >= 1);

    int scratch_bytes = rewrite->getScratchBytes();
    int consec = 0;
    for (int i = 0; i < scratch_bytes; i += 8) {
        Location l(Location::Scratch, i);
        if (vars_by_location.count(l) == 0) {
            consec++;
            if (consec == n) {
                int a = i / 8 - n + 1;
                int b = i / 8;
                assembler::Register r = result->initializeInReg();

                // TODO should be a LEA instruction
                // In fact, we could do something like we do for constants and only load
                // this when necessary, so it won't spill. Is that worth?
                assembler->mov(assembler::RBP, r);
                assembler->add(assembler::Immediate(8 * a + rewrite->getScratchRbpOffset()), r);

                // Put placeholders in so the array space doesn't get re-allocated.
                // This won't get collected, but that's fine.
                for (int j = a; j <= b; j++) {
                    Location m(Location::Scratch, j * 8);
                    vars_by_location[m] = LOCATION_PLACEHOLDER;
                }

                return a;
            }
        } else {
            consec = 0;
        }
    }
    RELEASE_ASSERT(0, "Using all %d bytes of scratch!", scratch_bytes);
}

RewriterVar* Rewriter::allocateAndCopy(RewriterVar* array_ptr, int n) {
    RewriterVar* result = createNewVar();
    addAction([=]() { this->_allocateAndCopy(result, array_ptr, n); }, { array_ptr }, ActionType::NORMAL);
    return result;
}

void Rewriter::_allocateAndCopy(RewriterVar* result, RewriterVar* array_ptr, int n) {
    // TODO smart register allocation

    int offset = _allocate(result, n);

    assembler::Register src_ptr = array_ptr->getInReg();
    assembler::Register tmp = allocReg(Location::any(), /* otherThan */ src_ptr);
    assert(tmp != src_ptr); // TODO how to ensure this?

    for (int i = 0; i < n; i++) {
        assembler->mov(assembler::Indirect(src_ptr, 8 * i), tmp);
        assembler->mov(tmp, assembler::Indirect(assembler::RBP, 8 * (offset + i) + rewrite->getScratchRbpOffset()));
    }

    array_ptr->bumpUse();
}

RewriterVar* Rewriter::allocateAndCopyPlus1(RewriterVar* first_elem, RewriterVar* rest_ptr, int n_rest) {
    if (n_rest > 0)
        assert(rest_ptr != NULL);
    else
        assert(rest_ptr == NULL);

    RewriterVar* result = createNewVar();
    addAction([=]() { this->_allocateAndCopyPlus1(result, first_elem, rest_ptr, n_rest); },
              rest_ptr ? std::vector<RewriterVar*>({ first_elem, rest_ptr })
                       : std::vector<RewriterVar*>({ first_elem }),
              ActionType::NORMAL);
    return result;
}

void Rewriter::_allocateAndCopyPlus1(RewriterVar* result, RewriterVar* first_elem, RewriterVar* rest_ptr, int n_rest) {
    int offset = _allocate(result, n_rest + 1);

    assembler::Register tmp = first_elem->getInReg();
    assembler->mov(tmp, assembler::Indirect(assembler::RBP, 8 * offset + rewrite->getScratchRbpOffset()));

    if (n_rest > 0) {
        assembler::Register src_ptr = rest_ptr->getInReg();
        // TODO if this triggers we'll need a way to allocate two distinct registers
        assert(tmp != src_ptr);

        for (int i = 0; i < n_rest; i++) {
            assembler->mov(assembler::Indirect(src_ptr, 8 * i), tmp);
            assembler->mov(tmp,
                           assembler::Indirect(assembler::RBP, 8 * (offset + i + 1) + rewrite->getScratchRbpOffset()));
        }
        rest_ptr->bumpUse();
    }

    first_elem->bumpUse();
}

assembler::Indirect Rewriter::indirectFor(Location l) {
    assert(l.type == Location::Scratch || l.type == Location::Stack);

    if (l.type == Location::Scratch)
        // TODO it can sometimes be more efficient to do RSP-relative addressing?
        return assembler::Indirect(assembler::RBP, rewrite->getScratchRbpOffset() + l.scratch_offset);
    else
        return assembler::Indirect(assembler::RSP, l.stack_offset);
}

void Rewriter::spillRegister(assembler::Register reg) {
    if (!done_guarding) {
        for (int i = 0; i < args.size(); i++) {
            assert(!args[i]->isInLocation(Location(reg)));
        }
    }

    RewriterVar* var = vars_by_location[reg];
    assert(var);

    // First, try to spill into a callee-save register:
    for (assembler::Register new_reg : allocatable_regs) {
        if (!new_reg.isCalleeSave())
            continue;
        if (vars_by_location.count(new_reg))
            continue;

        assembler->mov(reg, new_reg);
        addLocationToVar(var, new_reg);
        removeLocationFromVar(var, reg);
        return;
    }

    Location scratch = allocScratch();
    assembler::Indirect mem = indirectFor(scratch);
    assembler->mov(reg, mem);
    addLocationToVar(var, scratch);
    removeLocationFromVar(var, reg);
}

void Rewriter::spillRegister(assembler::XMMRegister reg) {
    if (!done_guarding) {
        for (int i = 0; i < args.size(); i++) {
            assert(!args[i]->isInLocation(Location(reg)));
        }
    }

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
    if (dest.type == Location::AnyReg) {
        int best = -1;
        assembler::Register best_reg(0);
        for (assembler::Register reg : allocatable_regs) {
            if (Location(reg) != otherThan) {
                if (vars_by_location.count(reg) == 0) {
                    return reg;
                }
                RewriterVar* var = vars_by_location[reg];
                if (var->is_arg && !done_guarding) {
                    continue;
                }
                if (var->next_use > best) {
                    best = var->next_use;
                    best_reg = reg;
                }
            }
        }

        // Spill the register whose next use is farthest in the future
        spillRegister(best_reg);
        assert(vars_by_location.count(best_reg) == 0);
        return best_reg;
    } else if (dest.type == Location::Register) {
        assembler::Register reg(dest.regnum);

        if (vars_by_location.count(reg)) {
            spillRegister(reg);
        }

        assert(vars_by_location.count(reg) == 0);
        return reg;
    } else {
        RELEASE_ASSERT(0, "%d", dest.type);
    }
}

assembler::XMMRegister Rewriter::allocXMMReg(Location dest, Location otherThan) {
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
    assert(!var->isInLocation(l));
    assert(vars_by_location.count(l) == 0);

    ASSERT(l.type == Location::Register || l.type == Location::XMMRegister || l.type == Location::Scratch
           || l.type == Location::Stack,
           "%d", l.type);

    var->locations.insert(l);
    vars_by_location[l] = var;
}

void Rewriter::removeLocationFromVar(RewriterVar* var, Location l) {
    assert(var->isInLocation(l));
    assert(vars_by_location[l] == var);

    vars_by_location.erase(l);
    var->locations.erase(l);
}

RewriterVar* Rewriter::createNewVar() {
    RewriterVar* var = new RewriterVar(this);
    vars.push_back(var);
    return var;
}

assembler::Register RewriterVar::initializeInReg(Location l) {
    assembler::Register reg = rewriter->allocReg(l);
    l = Location(reg);

    // Add this to vars_by_locations
    RewriterVar*& var = rewriter->vars_by_location[l];
    assert(!var);
    var = this;

    // Add the location to this
    this->locations.insert(l);

    return reg;
}

assembler::XMMRegister RewriterVar::initializeInXMMReg(Location l) {
    assembler::XMMRegister reg = rewriter->allocXMMReg(l);
    l = Location(reg);

    // Add this to vars_by_locations
    RewriterVar*& var = rewriter->vars_by_location[l];
    assert(!var);
    var = this;

    // Add the location to this
    this->locations.insert(l);

    return reg;
}

TypeRecorder* Rewriter::getTypeRecorder() {
    return rewrite->getTypeRecorder();
}

Rewriter::Rewriter(ICSlotRewrite* rewrite, int num_args, const std::vector<int>& live_outs)
    : rewrite(rewrite), assembler(rewrite->getAssembler()), return_location(rewrite->returnRegister()),
      added_changing_action(false), last_guard_action(-1), done_guarding(false), ndecisions(0), decision_path(1) {
#ifndef NDEBUG
    start_vars = RewriterVar::nvars;
#endif
    finished = false;

    for (int i = 0; i < num_args; i++) {
        Location l = Location::forArg(i);
        RewriterVar* var = createNewVar();
        addLocationToVar(var, l);

        var->is_arg = true;

        args.push_back(var);
    }

    static StatCounter rewriter_starts("rewriter_starts");
    rewriter_starts.log();
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
            var->locations.insert(l);
        }

        this->live_outs.push_back(var);
        this->live_out_regs.push_back(dwarf_regnum);
    }
}

Rewriter* Rewriter::createRewriter(void* rtn_addr, int num_args, const char* debug_name) {
    ICInfo* ic = getICInfo(rtn_addr);

    static StatCounter rewriter_attempts("rewriter_attempts");
    rewriter_attempts.log();

    static StatCounter rewriter_nopatch("rewriter_nopatch");
    static StatCounter rewriter_skipped("rewriter_skipped");

    if (!ic) {
        rewriter_nopatch.log();
        return NULL;
    }

    if (!ic->shouldAttempt()) {
        rewriter_skipped.log();
        return NULL;
    }

    return new Rewriter(ic->startRewrite(debug_name), num_args, ic->getLiveOuts());
}

#ifndef NDEBUG
int RewriterVar::nvars = 0;
#endif
}
