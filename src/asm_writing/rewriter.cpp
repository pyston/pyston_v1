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

RewriterVarUsage::RewriterVarUsage(RewriterVar* var) : var(var), done_using(false) {
    var->incUse();
    assert(var->rewriter);
}

void RewriterVarUsage::addGuard(uint64_t val) {
    assertValid();

    Rewriter* rewriter = var->rewriter;
    assembler::Assembler* assembler = rewriter->assembler;

    assert(!rewriter->done_guarding && "too late to add a guard!");

    assembler::Register this_reg = var->getInReg();
    if (val < (-1L << 31) || val >= (1L << 31) - 1) {
        assembler::Register reg = rewriter->allocReg(Location::any());
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(this_reg, reg);
    } else {
        assembler->cmp(this_reg, assembler::Immediate(val));
    }
    assembler->jne(assembler::JumpDestination::fromStart(rewriter->rewrite->getSlotSize()));
}

void RewriterVarUsage::addGuardNotEq(uint64_t val) {
    assertValid();

    Rewriter* rewriter = var->rewriter;
    assembler::Assembler* assembler = rewriter->assembler;

    assert(!rewriter->done_guarding && "too late to add a guard!");

    assembler::Register this_reg = var->getInReg();
    if (val < (-1L << 31) || val >= (1L << 31) - 1) {
        assembler::Register reg = rewriter->allocReg(Location::any());
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(this_reg, reg);
    } else {
        assembler->cmp(this_reg, assembler::Immediate(val));
    }
    assembler->je(assembler::JumpDestination::fromStart(rewriter->rewrite->getSlotSize()));
}

void RewriterVarUsage::addAttrGuard(int offset, uint64_t val, bool negate) {
    assertValid();

    Rewriter* rewriter = var->rewriter;
    assembler::Assembler* assembler = rewriter->assembler;

    assert(!rewriter->done_guarding && "too late to add a guard!");

    assembler::Register this_reg = var->getInReg();
    if (val < (-1L << 31) || val >= (1L << 31) - 1) {
        assembler::Register reg = rewriter->allocReg(Location::any(), /* otherThan */ this_reg);
        assert(reg != this_reg);
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(assembler::Indirect(this_reg, offset), reg);
    } else {
        assembler->cmp(assembler::Indirect(this_reg, offset), assembler::Immediate(val));
    }
    if (negate)
        assembler->je(assembler::JumpDestination::fromStart(rewriter->rewrite->getSlotSize()));
    else
        assembler->jne(assembler::JumpDestination::fromStart(rewriter->rewrite->getSlotSize()));
}

RewriterVarUsage RewriterVarUsage::getAttr(int offset, KillFlag kill, Location dest) {
    assertValid();

    // Save these, since if we kill this register the var might disappear:
    assembler::Register this_reg = var->getInReg();
    Rewriter* rewriter = var->rewriter;

    if (kill == Kill) {
        setDoneUsing();
    }

    assembler::Register newvar_reg = rewriter->allocReg(dest);
    RewriterVarUsage newvar = rewriter->createNewVar(newvar_reg);
    rewriter->assembler->mov(assembler::Indirect(this_reg, offset), newvar_reg);
    return std::move(newvar);
}

RewriterVarUsage RewriterVarUsage::cmp(AST_TYPE::AST_TYPE cmp_type, RewriterVarUsage other, Location dest) {
    assertValid();

    assembler::Register this_reg = var->getInReg();
    assembler::Register other_reg = other.var->getInReg();
    assert(this_reg != other_reg); // TODO how do we ensure this?
    Rewriter* rewriter = var->rewriter;

    assembler::Register newvar_reg = rewriter->allocReg(dest);
    rewriter->assembler->cmp(this_reg, other_reg);
    RewriterVarUsage newvar = rewriter->createNewVar(newvar_reg);
    switch (cmp_type) {
        case AST_TYPE::Eq:
            rewriter->assembler->sete(newvar_reg);
            break;
        case AST_TYPE::NotEq:
            rewriter->assembler->setne(newvar_reg);
            break;
        default:
            RELEASE_ASSERT(0, "%d", cmp_type);
    }

    other.setDoneUsing();

    return std::move(newvar);
}

RewriterVarUsage RewriterVarUsage::toBool(KillFlag kill, Location dest) {
    assertValid();

    assembler::Register this_reg = var->getInReg();
    Rewriter* rewriter = var->rewriter;

    if (kill == Kill) {
        setDoneUsing();
    }

    rewriter->assembler->test(this_reg, this_reg);
    assembler::Register result_reg = rewriter->allocReg(dest);
    rewriter->assembler->setnz(result_reg);

    RewriterVarUsage result = rewriter->createNewVar(result_reg);
    return result;
}

void RewriterVarUsage::setAttr(int offset, RewriterVarUsage val) {
    assertValid();
    var->rewriter->assertChangesOk();

    assembler::Register this_reg = var->getInReg();

    bool is_immediate;
    assembler::Immediate imm = val.var->tryGetAsImmediate(&is_immediate);

    if (is_immediate) {
        var->rewriter->assembler->movq(imm, assembler::Indirect(this_reg, offset));
    } else {
        assembler::Register other_reg = val.var->getInReg();

        // TODO the allocator could choose to spill this_reg in order to load other_reg...
        // Hopefuly it won't make that decision, so we should just be able to guard on it for now:
        assert(this_reg != other_reg);

        var->rewriter->assembler->mov(other_reg, assembler::Indirect(this_reg, offset));
    }

    val.setDoneUsing();
}

void RewriterVarUsage::setDoneUsing() {
    assertValid();
    done_using = true;
    var->decUse();
    var = NULL;
}

bool RewriterVarUsage::isDoneUsing() {
    return done_using;
}

void RewriterVarUsage::ensureDoneUsing() {
    if (!done_using)
        setDoneUsing();
}

RewriterVarUsage::RewriterVarUsage(RewriterVarUsage&& usage) {
    assert(!usage.done_using);
    assert(usage.var != NULL);

    var = usage.var;
    done_using = usage.done_using;

    usage.var = NULL;
    usage.done_using = true;
}

RewriterVarUsage& RewriterVarUsage::operator=(RewriterVarUsage&& usage) {
    assert(done_using);
    assert(var == NULL);
    assert(!usage.done_using);
    assert(usage.var != NULL);

    var = usage.var;
    done_using = usage.done_using;

    usage.var = NULL;
    usage.done_using = true;

    return *this;
}

void RewriterVar::dump() {
    printf("RewriterVar at %p: %d uses.  %ld locations:\n", this, num_uses, locations.size());
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

assembler::Register RewriterVar::getInReg(Location dest, bool allow_constant_in_reg) {
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
            }
            return reg;
        }
    }

    assert(locations.size() == 1);
    Location l(*locations.begin());

    assembler::Register reg = rewriter->allocReg(dest);
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

RewriterVarUsage::RewriterVarUsage() : var(NULL), done_using(true) {
}

RewriterVarUsage RewriterVarUsage::empty() {
    return RewriterVarUsage();
}



void RewriterVar::decUse() {
    num_uses--;
    if (num_uses == 0) {
        rewriter->kill(this);
        delete this;
    }
}

void RewriterVar::incUse() {
    num_uses++;
}

bool RewriterVar::isInLocation(Location l) {
    return locations.count(l) != 0;
}



void Rewriter::setDoneGuarding() {
    assert(!done_guarding);
    done_guarding = true;

    for (RewriterVar* var : args) {
        var->decUse();
    }
    args.clear();
}

RewriterVarUsage Rewriter::getArg(int argnum) {
    assert(!done_guarding);
    assert(argnum >= 0 && argnum < args.size());

    RewriterVar* var = args[argnum];
    return RewriterVarUsage(var);
}

Location Rewriter::getReturnDestination() {
    return return_location;
}

void Rewriter::trap() {
    assembler->trap();
}

RewriterVarUsage Rewriter::loadConst(int64_t val, Location dest) {
    if (val >= (-1L << 31) && val < (1L << 31) - 1) {
        Location l(Location::Constant, val);
        RewriterVar*& var = vars_by_location[l];
        if (!var) {
            var = new RewriterVar(this, l);
        }
        return RewriterVarUsage(var);
    }

    assembler::Register reg = allocReg(dest);
    RewriterVarUsage var = createNewVar(reg);
    assembler->mov(assembler::Immediate(val), reg);
    // I guess you don't need std::move here:
    return var;
}

RewriterVarUsage Rewriter::call(bool can_call_into_python, void* func_addr, RewriterVarUsage arg0) {
    std::vector<RewriterVarUsage> args;
    args.push_back(std::move(arg0));
    return call(can_call_into_python, func_addr, std::move(args));
}

RewriterVarUsage Rewriter::call(bool can_call_into_python, void* func_addr, RewriterVarUsage arg0,
                                RewriterVarUsage arg1) {
    std::vector<RewriterVarUsage> args;
    args.push_back(std::move(arg0));
    args.push_back(std::move(arg1));
    return call(can_call_into_python, func_addr, std::move(args));
}

static const Location caller_save_registers[]{
    assembler::RAX,   assembler::RCX,   assembler::RDX,   assembler::RSI,   assembler::RDI,
    assembler::R8,    assembler::R9,    assembler::R10,   assembler::R11,   assembler::XMM0,
    assembler::XMM1,  assembler::XMM2,  assembler::XMM3,  assembler::XMM4,  assembler::XMM5,
    assembler::XMM6,  assembler::XMM7,  assembler::XMM8,  assembler::XMM9,  assembler::XMM10,
    assembler::XMM11, assembler::XMM12, assembler::XMM13, assembler::XMM14, assembler::XMM15,
};

RewriterVarUsage Rewriter::call(bool can_call_into_python, void* func_addr, std::vector<RewriterVarUsage> args) {
    // TODO figure out why this is here -- what needs to be done differently
    // if can_call_into_python is true?
    // assert(!can_call_into_python);

    assertChangesOk();

    // RewriterVarUsage scratch = createNewVar(Location::any());
    assembler::Register r = allocReg(assembler::R11);

    for (int i = 0; i < args.size(); i++) {
        Location l(Location::forArg(i));
        RewriterVar* var = args[i].var;

        // printf("%d ", i);
        // var->dump();
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

#ifndef NDEBUG
    for (int i = 0; i < args.size(); i++) {
        RewriterVar* var = args[i].var;
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

        auto it = vars_by_location.find(check_reg);
        if (it == vars_by_location.end())
            continue;

        RewriterVar* var = it->second;
        bool need_to_spill = true;
        for (Location l : var->locations) {
            if (!l.isClobberedByCall()) {
                need_to_spill = false;
                break;
            }
        }
        for (int i = 0; i < args.size(); i++) {
            if (args[i].var == var) {
                if (var->num_uses == 1) {
                    // If we hold the only usage of this arg var, we are
                    // going to kill all of its usages soon anyway,
                    // so we have no need to spill it.
                    need_to_spill = false;
                }
                break;
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

    // We call setDoneUsing after spilling because when we release these,
    // we might release a pointer to an array in the scratch space allocated
    // with _allocate. If we do that before spilling, we might spill into that
    // scratch space.
    for (int i = 0; i < args.size(); i++) {
        args[i].setDoneUsing();
    }


#ifndef NDEBUG
    for (const auto& p : vars_by_location) {
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
    RewriterVar* var = vars_by_location[assembler::RAX] = new RewriterVar(this, assembler::RAX);
    return RewriterVarUsage(var);
}

void Rewriter::abort() {
    assert(!finished);
    finished = true;
    rewrite->abort();

    static StatCounter rewriter_aborts("rewriter_aborts");
    rewriter_aborts.log();

    for (auto v : args) {
        v->decUse();
    }
    for (auto v : live_outs) {
        v->decUse();
    }
}

void Rewriter::commit() {
    assert(!finished);
    finished = true;

    static StatCounter rewriter_commits("rewriter_commits");
    rewriter_commits.log();

    assert(done_guarding && "Could call setDoneGuarding for you, but probably best to do it yourself");
    // if (!done_guarding)
    // setDoneGuarding();

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

    for (int i = 0; i < live_outs.size(); i++) {
        assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
        RewriterVar* var = live_outs[i];
        assert(var->isInLocation(ru));
        var->decUse();
    }

    assert(vars_by_location.size() == 0);

    rewrite->commit(decision_path, this);
}

void Rewriter::finishAssembly(int continue_offset) {
    assembler->jmp(assembler::JumpDestination::fromStart(continue_offset));

    assembler->fillWithNops();
}

void Rewriter::commitReturning(RewriterVarUsage usage) {
    // assert(usage.var->isInLocation(getReturnDestination()));
    usage.var->getInReg(getReturnDestination(), true /* allow_constant_in_reg */);

    usage.setDoneUsing();
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

void Rewriter::kill(RewriterVar* var) {
    for (RewriterVarUsage& scratch_range_usage : var->scratch_range) {
        // Should be the only usage for this particular var (we
        // hold the only usage) so it should cause the array to
        // be deallocated.
        scratch_range_usage.setDoneUsing();
    }
    var->scratch_range.clear();

    for (Location l : var->locations) {
        assert(vars_by_location[l] == var);
        vars_by_location.erase(l);
    }
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

std::pair<RewriterVarUsage, int> Rewriter::_allocate(int n) {
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
                assembler::Register r = allocReg(Location::any());
                // TODO should be a LEA instruction
                // In fact, we could do something like we do for constants and only load
                // this when necessary, so it won't spill. Is that worth?
                assembler->mov(assembler::RBP, r);
                assembler->add(assembler::Immediate(8 * a + rewrite->getScratchRbpOffset()), r);
                RewriterVarUsage usage = createNewVar(r);

                for (int j = a; j <= b; j++) {
                    Location m(Location::Scratch, j * 8);
                    RewriterVarUsage placeholder = createNewVar(m);
                    usage.var->scratch_range.push_back(std::move(placeholder));
                }

                return std::make_pair(std::move(usage), a);
            }
        } else {
            consec = 0;
        }
    }
    RELEASE_ASSERT(0, "Using all %d bytes of scratch!", scratch_bytes);
}

RewriterVarUsage Rewriter::allocate(int n) {
    return _allocate(n).first;
}

RewriterVarUsage Rewriter::allocateAndCopy(RewriterVarUsage array_ptr, int n) {
    // TODO smart register allocation
    array_ptr.assertValid();

    std::pair<RewriterVarUsage, int> allocation = _allocate(n);
    int offset = allocation.second;

    assembler::Register src_ptr = array_ptr.var->getInReg();
    assembler::Register tmp = allocReg(Location::any(), /* otherThan */ src_ptr);
    assert(tmp != src_ptr); // TODO how to ensure this?

    for (int i = 0; i < n; i++) {
        assembler->mov(assembler::Indirect(src_ptr, 8 * i), tmp);
        assembler->mov(tmp, assembler::Indirect(assembler::RBP, 8 * (offset + i) + rewrite->getScratchRbpOffset()));
    }

    array_ptr.setDoneUsing();

    return std::move(allocation.first);
}

RewriterVarUsage Rewriter::allocateAndCopyPlus1(RewriterVarUsage first_elem, RewriterVarUsage rest_ptr, int n_rest) {
    first_elem.assertValid();
    if (n_rest > 0)
        rest_ptr.assertValid();
    else
        assert(rest_ptr.isDoneUsing());

    std::pair<RewriterVarUsage, int> allocation = _allocate(n_rest + 1);
    int offset = allocation.second;

    assembler::Register tmp = first_elem.var->getInReg();
    assembler->mov(tmp, assembler::Indirect(assembler::RBP, 8 * offset + rewrite->getScratchRbpOffset()));

    if (n_rest > 0) {
        assembler::Register src_ptr = rest_ptr.var->getInReg();
        // TODO if this triggers we'll need a way to allocate two distinct registers
        assert(tmp != src_ptr);

        for (int i = 0; i < n_rest; i++) {
            assembler->mov(assembler::Indirect(src_ptr, 8 * i), tmp);
            assembler->mov(tmp,
                           assembler::Indirect(assembler::RBP, 8 * (offset + i + 1) + rewrite->getScratchRbpOffset()));
        }
        rest_ptr.setDoneUsing();
    }

    first_elem.setDoneUsing();

    return std::move(allocation.first);
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
    // Don't spill a register than an input argument is in, unless
    // we are done guarding (in which case `args` will be empty)
    for (int i = 0; i < args.size(); i++) {
        assert(!args[i]->isInLocation(Location(reg)));
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
    assert(done_guarding);

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
        for (assembler::Register reg : allocatable_regs) {
            if (Location(reg) != otherThan && vars_by_location.count(reg) == 0)
                return reg;
        }
        // TODO maybe should do some sort of round-robin or LRU eviction strategy?
        return allocReg(otherThan == assembler::R15 ? assembler::R14 : assembler::R15);
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

void Rewriter::addLocationToVar(RewriterVar* var, Location l) {
    assert(!var->isInLocation(l));
    assert(vars_by_location.count(l) == 0);

    ASSERT(l.type == Location::Register || l.type == Location::XMMRegister || l.type == Location::Scratch, "%d",
           l.type);

    var->locations.insert(l);
    vars_by_location[l] = var;
}

void Rewriter::removeLocationFromVar(RewriterVar* var, Location l) {
    assert(var->isInLocation(l));
    assert(vars_by_location[l] == var);

    vars_by_location.erase(l);
    var->locations.erase(l);
}

RewriterVarUsage Rewriter::createNewVar(Location dest) {
    RewriterVar*& var = vars_by_location[dest];
    assert(!var);

    var = new RewriterVar(this, dest);
    return var;
}

TypeRecorder* Rewriter::getTypeRecorder() {
    return rewrite->getTypeRecorder();
}

Rewriter::Rewriter(ICSlotRewrite* rewrite, int num_args, const std::vector<int>& live_outs)
    : rewrite(rewrite), assembler(rewrite->getAssembler()), return_location(rewrite->returnRegister()),
      done_guarding(false), ndecisions(0), decision_path(1) {
#ifndef NDEBUG
    start_vars = RewriterVar::nvars;
#endif
    finished = false;
    // assembler->trap();

    for (int i = 0; i < num_args; i++) {
        Location l = Location::forArg(i);
        RewriterVar* var = new RewriterVar(this, l);
        vars_by_location[l] = var;

        var->incUse();
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

        //// The return register is the only live-out that is not also a live-in.
        // if (l == getReturnDestination()) {
        // l.dump();
        // continue;
        //}

        if (l.isClobberedByCall()) {
            rewriter_spillsavoided.log();
        }

        RewriterVar*& var = vars_by_location[l];
        if (!var) {
            var = new RewriterVar(this, l);
        }
        var->incUse();

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

RewriterVarUsage RewriterVarUsage::addUse() {
    return RewriterVarUsage(var);
}

#ifndef NDEBUG
int RewriterVar::nvars = 0;
#endif
}
