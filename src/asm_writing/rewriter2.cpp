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

#include "asm_writing/rewriter2.h"

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
    RELEASE_ASSERT(0, "the following is untested");
    // int offset = (argnum - 6) * 8;
    // return Location(Stack, offset);
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
        printf("scratch(%d)\n", stack_offset);
        return;
    }

    if (type == Constant) {
        printf("imm(%d)\n", constant_val);
        return;
    }

    RELEASE_ASSERT(0, "%d", type);
}



RewriterVarUsage2::RewriterVarUsage2(RewriterVar2* var) : var(var), done_using(false) {
    assert(var->rewriter);
}

void RewriterVarUsage2::addAttrGuard(int offset, uint64_t val) {
    Rewriter2* rewriter = var->rewriter;
    assembler::Assembler* assembler = rewriter->assembler;

    assert(!rewriter->done_guarding && "too late to add a guard!");
    assertValid();

    assembler::Register this_reg = var->getInReg();
    if (val < (-1L << 31) || val >= (1L << 31) - 1) {
        assembler::Register reg = rewriter->allocReg(Location::any());
        assembler->mov(assembler::Immediate(val), reg);
        assembler->cmp(assembler::Indirect(this_reg, offset), reg);
    } else {
        assembler->cmp(assembler::Indirect(this_reg, offset), assembler::Immediate(val));
    }
    assembler->jne(assembler::JumpDestination::fromStart(rewriter->rewrite->getSlotSize()));
}

RewriterVarUsage2 RewriterVarUsage2::getAttr(int offset, KillFlag kill, Location dest) {
    assertValid();

    // Save these, since if we kill this register the var might disappear:
    assembler::Register this_reg = var->getInReg();
    Rewriter2* rewriter = var->rewriter;

    if (kill == Kill) {
        setDoneUsing();
    }

    assembler::Register newvar_reg = rewriter->allocReg(dest);
    RewriterVarUsage2 newvar = rewriter->createNewVar(newvar_reg);
    rewriter->assembler->mov(assembler::Indirect(this_reg, offset), newvar_reg);
    return std::move(newvar);
}

void RewriterVarUsage2::setAttr(int offset, RewriterVarUsage2 val) {
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

void RewriterVarUsage2::setDoneUsing() {
    assertValid();
    done_using = true;
    var->decUse();
    var = NULL;
}

RewriterVarUsage2::RewriterVarUsage2(RewriterVarUsage2&& usage) {
    assert(!usage.done_using);
    assert(usage.var != NULL);

    var = usage.var;
    done_using = usage.done_using;

    usage.var = NULL;
    usage.done_using = true;
}

RewriterVarUsage2& RewriterVarUsage2::operator=(RewriterVarUsage2&& usage) {
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

void RewriterVar2::dump() {
    printf("RewriterVar2 at %p: %d uses.  %ld locations:\n", this, num_uses, locations.size());
    for (Location l : locations)
        l.dump();
}

assembler::Immediate RewriterVar2::tryGetAsImmediate(bool* is_immediate) {
    for (Location l : locations) {
        if (l.type == Location::Constant) {
            *is_immediate = true;
            return assembler::Immediate(l.constant_val);
        }
    }
    *is_immediate = false;
    return assembler::Immediate((uint64_t)0);
}

assembler::Register RewriterVar2::getInReg(Location dest) {
    assert(dest.type == Location::Register || dest.type == Location::AnyReg);

    // assembler::Register reg = var->rewriter->allocReg(l);
    // var->rewriter->addLocationToVar(var, reg);
    // return reg;
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
    assert(l.type == Location::Scratch);


    assembler::Register reg = rewriter->allocReg(dest);
    assert(rewriter->vars_by_location.count(reg) == 0);

    assembler::Indirect mem = rewriter->indirectFor(l);
    rewriter->assembler->mov(mem, reg);
    rewriter->addLocationToVar(this, reg);
    return reg;
}

assembler::XMMRegister RewriterVar2::getInXMMReg(Location dest) {
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

RewriterVarUsage2::RewriterVarUsage2() : var(NULL), done_using(true) {
}

RewriterVarUsage2 RewriterVarUsage2::empty() {
    return RewriterVarUsage2();
}



void RewriterVar2::decUse() {
    num_uses--;
    if (num_uses == 0) {
        rewriter->kill(this);
        delete this;
    }
}

void RewriterVar2::incUse() {
    num_uses++;
}

bool RewriterVar2::isInLocation(Location l) {
    return locations.count(l) != 0;
}



void Rewriter2::setDoneGuarding() {
    assert(!done_guarding);
    done_guarding = true;

    for (RewriterVar2* var : args) {
        var->decUse();
    }
    args.clear();
}

RewriterVarUsage2 Rewriter2::getArg(int argnum) {
    assert(!done_guarding);
    assert(argnum >= 0 && argnum < args.size());

    RewriterVar2* var = args[argnum];
    var->incUse();
    return RewriterVarUsage2(var);
}

Location Rewriter2::getReturnDestination() {
    return return_location;
}

void Rewriter2::trap() {
    assembler->trap();
}

RewriterVarUsage2 Rewriter2::loadConst(int64_t val, Location dest) {
    if (val >= (-1L << 31) && val < (1L << 31) - 1) {
        Location l(Location::Constant, val);
        return createNewVar(l);
    }

    assembler::Register reg = allocReg(dest);
    RewriterVarUsage2 var = createNewVar(reg);
    assembler->mov(assembler::Immediate(val), reg);
    // I guess you don't need std::move here:
    return var;
}

RewriterVarUsage2 Rewriter2::call(bool can_call_into_python, void* func_addr, RewriterVarUsage2 arg0) {
    std::vector<RewriterVarUsage2> args;
    args.push_back(std::move(arg0));
    return call(can_call_into_python, func_addr, std::move(args));
}

RewriterVarUsage2 Rewriter2::call(bool can_call_into_python, void* func_addr, RewriterVarUsage2 arg0,
                                  RewriterVarUsage2 arg1) {
    std::vector<RewriterVarUsage2> args;
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

RewriterVarUsage2 Rewriter2::call(bool can_call_into_python, void* func_addr, std::vector<RewriterVarUsage2> args) {
    assert(!can_call_into_python);

    assertChangesOk();

    // RewriterVarUsage2 scratch = createNewVar(Location::any());
    assembler::Register r = allocReg(assembler::R11);

    for (int i = 0; i < args.size(); i++) {
        Location l(Location::forArg(i));
        RewriterVar2* var = args[i].var;

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
        RewriterVar2* var = args[i].var;
        if (!var->isInLocation(Location::forArg(i))) {
            var->dump();
        }
        assert(var->isInLocation(Location::forArg(i)));
    }
#endif

    // This is kind of hacky: we release the use of these right now,
    // and then expect that everything else will not clobber any of the arguments.
    // Naively moving this below the reg spilling will always spill the arguments;
    // but sometimes you need to do that if the argument lives past the call.
    // Hacky, but the right way to do it requires a bit of reworking so that it can
    // spill but keep its current use.
    for (int i = 0; i < args.size(); i++) {
        args[i].setDoneUsing();
    }

    // Spill caller-saved registers:
    for (auto check_reg : caller_save_registers) {
        // check_reg.dump();
        assert(check_reg.isClobberedByCall());

        auto it = vars_by_location.find(check_reg);
        if (it == vars_by_location.end())
            continue;

        RewriterVar2* var = it->second;
        bool need_to_spill = true;
        for (Location l : var->locations) {
            if (!l.isClobberedByCall()) {
                need_to_spill = false;
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
    RewriterVar2* var = vars_by_location[assembler::RAX] = new RewriterVar2(this, assembler::RAX);
    return RewriterVarUsage2(var);
}

void Rewriter2::commit() {
    static StatCounter rewriter2_commits("rewriter2_commits");
    rewriter2_commits.log();

    assert(done_guarding && "Could call setDoneGuarding for you, but probably best to do it yourself");
    // if (!done_guarding)
    // setDoneGuarding();

    assert(live_out_regs.size() == live_outs.size());
    for (int i = 0; i < live_outs.size(); i++) {
        assembler::GenericRegister ru = assembler::GenericRegister::fromDwarf(live_out_regs[i]);
        Location expected(ru);

        RewriterVar2* var = live_outs[i];
        // for (Location l : var->locations) {
        // printf("%d %d\n", l.type, l._data);
        //}
        if (!var->isInLocation(expected)) {
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
        }

        assert(var->isInLocation(ru));
        var->decUse();
    }

    assert(vars_by_location.size() == 0);

    rewrite->commit(0, this);
}

void Rewriter2::finishAssembly(int continue_offset) {
    assembler->jmp(assembler::JumpDestination::fromStart(continue_offset));

    assembler->fillWithNops();
}

void Rewriter2::commitReturning(RewriterVarUsage2 usage) {
    assert(usage.var->isInLocation(getReturnDestination()));

    /*
    Location l = usage.var->location;
    Location expected = getReturnDestination();

    if (l != expected) {
        assert(l.type == Location::Register);
        assert(expected.type == Location::Register);

        assembler->mov(l.asRegister(), expected.asRegister());
    }
    */

    usage.setDoneUsing();
    commit();
}

void Rewriter2::addDependenceOn(ICInvalidator& invalidator) {
    rewrite->addDependenceOn(invalidator);
}

void Rewriter2::kill(RewriterVar2* var) {
    for (Location l : var->locations) {
        assert(vars_by_location[l] == var);
        vars_by_location.erase(l);
    }
}

Location Rewriter2::allocScratch() {
    int scratch_bytes = rewrite->getScratchBytes();
    for (int i = 0; i < scratch_bytes; i += 8) {
        Location l(Location::Scratch, i);
        if (vars_by_location.count(l) == 0)
            return l;
    }
    RELEASE_ASSERT(0, "Using all %d bytes of scratch!", scratch_bytes);
}

assembler::Indirect Rewriter2::indirectFor(Location l) {
    assert(l.type == Location::Scratch);

    // TODO it can sometimes be more efficient to do RSP-relative addressing?
    int rbp_offset = rewrite->getScratchRbpOffset() + l.scratch_offset;
    return assembler::Indirect(assembler::RBP, rbp_offset);
}

void Rewriter2::spillRegister(assembler::Register reg) {
    assert(done_guarding);

    RewriterVar2* var = vars_by_location[reg];
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

void Rewriter2::spillRegister(assembler::XMMRegister reg) {
    assert(done_guarding);

    RewriterVar2* var = vars_by_location[reg];
    assert(var);

    assert(var->locations.size() == 1);

    Location scratch = allocScratch();
    assembler::Indirect mem = indirectFor(scratch);
    assembler->movsd(reg, mem);
    addLocationToVar(var, scratch);
    removeLocationFromVar(var, reg);
}

assembler::Register Rewriter2::allocReg(Location dest) {
    if (dest.type == Location::AnyReg) {
        for (assembler::Register reg : allocatable_regs) {
            if (vars_by_location.count(reg) == 0)
                return reg;
        }
        RELEASE_ASSERT(0, "couldn't find a reg to allocate and haven't added spilling");
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

void Rewriter2::addLocationToVar(RewriterVar2* var, Location l) {
    assert(!var->isInLocation(l));
    assert(vars_by_location.count(l) == 0);

    ASSERT(l.type == Location::Register || l.type == Location::XMMRegister || l.type == Location::Scratch, "%d",
           l.type);

    var->locations.insert(l);
    vars_by_location[l] = var;
}

void Rewriter2::removeLocationFromVar(RewriterVar2* var, Location l) {
    assert(var->isInLocation(l));
    assert(vars_by_location[l] = var);

    vars_by_location.erase(l);
    var->locations.erase(l);
}

RewriterVarUsage2 Rewriter2::createNewVar(Location dest) {
    RewriterVar2*& var = vars_by_location[dest];
    assert(!var);

    var = new RewriterVar2(this, dest);
    return var;
}

TypeRecorder* Rewriter2::getTypeRecorder() {
    return rewrite->getTypeRecorder();
}

Rewriter2::Rewriter2(ICSlotRewrite* rewrite, int num_args, const std::vector<int>& live_outs)
    : rewrite(rewrite), assembler(rewrite->getAssembler()), return_location(rewrite->returnRegister()),
      done_guarding(false) {
    // assembler->trap();

    for (int i = 0; i < num_args; i++) {
        Location l = Location::forArg(i);
        RewriterVar2* var = new RewriterVar2(this, l);
        vars_by_location[l] = var;

        args.push_back(var);
    }

    static StatCounter rewriter_starts("rewriter2_starts");
    rewriter_starts.log();
    static StatCounter rewriter_spillsavoided("rewriter2_spillsavoided");

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

        RewriterVar2*& var = vars_by_location[l];
        if (var) {
            var->incUse();
        } else {
            var = new RewriterVar2(this, l);
        }

        this->live_outs.push_back(var);
        this->live_out_regs.push_back(dwarf_regnum);
    }
}

Rewriter2* Rewriter2::createRewriter(void* rtn_addr, int num_args, const char* debug_name) {
    ICInfo* ic = getICInfo(rtn_addr);

    static StatCounter rewriter_nopatch("rewriter_nopatch");

    if (!ic) {
        rewriter_nopatch.log();
        return NULL;
    }

    return new Rewriter2(ic->startRewrite(debug_name), num_args, ic->getLiveOuts());
}
}
