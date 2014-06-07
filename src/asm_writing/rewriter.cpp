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

#include "asm_writing/assembler.h"
#include "asm_writing/icinfo.h"
#include "core/common.h"
#include "core/stats.h"

namespace pyston {

using namespace pyston::assembler;

#define MAX_ARGS 16

Register fromArgnum(int argnum) {
    switch (argnum) {
        case -5:
            return RBP;
        case -4:
            return RSP;
        case -3:
            return R11;
        case -2:
            return R10;
        case -1:
            return RAX;
        case 0:
            return RDI;
        case 1:
            return RSI;
        case 2:
            return RDX;
        case 3:
            return RCX;
        case 4:
            return R8;
        case 5:
            return R9;
    }
    RELEASE_ASSERT(0, "%d", argnum);
}

RewriterVar::RewriterVar(Rewriter* rewriter, int argnum, int version)
    : rewriter(rewriter), argnum(argnum), version(version) {
    // assert(rewriter.icentry.get());
}

RewriterVar& RewriterVar::operator=(const RewriterVar& rhs) {
    assert(rewriter == NULL || rewriter == rhs.rewriter);
    rhs.assertValid();
    rewriter = rhs.rewriter;
    argnum = rhs.argnum;
    version = rhs.version;
    return *this;
}

#ifndef NDEBUG
void RewriterVar::assertValid() const {
    assert(rewriter);
    rewriter->checkVersion(argnum, version);
}

void RewriterVar::lock() {
    assertValid();
    rewriter->lock(argnum);
}

void RewriterVar::unlock() {
    assertValid();
    rewriter->unlock(argnum);
}
#endif

int RewriterVar::getArgnum() {
    // assert(rewriter);
    return argnum;
}

RewriterVar RewriterVar::getAttr(int offset, int dest) {
    assertValid();

    rewriter->assembler->mov(Indirect(fromArgnum(this->argnum), offset), fromArgnum(dest));
    int version = rewriter->mutate(dest);
    return RewriterVar(rewriter, dest, version);
}

void RewriterVar::incAttr(int offset) {
    assertValid();

    rewriter->assembler->inc(Indirect(fromArgnum(this->argnum), offset));

#ifndef NDEBUG
    rewriter->changed_something = true;
#endif
}

void RewriterVar::setAttr(int offset, const RewriterVar& val, bool user_visible) {
    assertValid();
    val.assertValid();

    rewriter->assembler->mov(fromArgnum(val.argnum), Indirect(fromArgnum(this->argnum), offset));

#ifndef NDEBUG
    if (user_visible)
        rewriter->changed_something = true;
#endif
}

RewriterVar RewriterVar::move(int dest_argnum) {
    assertValid();

    int version;
    if (dest_argnum != this->argnum) {
        assert(dest_argnum < 6);

        if (this->argnum >= 6) {
            if (1) {
                int offset = (this->argnum - 6) * 8 + rewriter->pushes.size() * 8 + rewriter->alloca_bytes;
                rewriter->assembler->mov(Indirect(RSP, offset), fromArgnum(dest_argnum));
            } else {
                int stack_size = rewriter->rewrite->getFuncStackSize();
                ASSERT(stack_size > 0 && stack_size < (1 << 30), "%d", stack_size);
                int offset = (this->argnum - 6) * 8 - (stack_size - 8);
                rewriter->assembler->mov(Indirect(RBP, offset), fromArgnum(dest_argnum));
            }
        } else {
            rewriter->assembler->mov(fromArgnum(this->argnum), fromArgnum(dest_argnum));
        }

        version = rewriter->mutate(dest_argnum);
    } else {
        version = this->version;
    }
    return RewriterVar(rewriter, dest_argnum, version);
}

void RewriterVar::addGuard(intptr_t val) {
    assert(!rewriter->changed_something && "too late to add a guard!");
    assertValid();

    rewriter->checkArgsValid();

    int bytes = 8 * rewriter->pushes.size() + rewriter->alloca_bytes;

    if (val < (-1L << 31) || val >= (1L << 31) - 1) {
        rewriter->assembler->push(RBP);
        rewriter->assembler->mov(Immediate(val), RBP);
        rewriter->assembler->cmp(fromArgnum(this->argnum), RBP);
        rewriter->assembler->pop(RBP);
    } else {
        rewriter->assembler->cmp(fromArgnum(this->argnum), Immediate(val));
    }
    rewriter->assembler->jne(JumpDestination::fromStart(rewriter->rewrite->getSlotSize() - bytes / 8));
}

void RewriterVar::addAttrGuard(int offset, intptr_t val) {
    assert(!rewriter->changed_something && "too late to add a guard!");
    assertValid();

    rewriter->checkArgsValid();

    int bytes = 8 * rewriter->pushes.size() + rewriter->alloca_bytes;

    if (val < (-1L << 31) || val >= (1L << 31) - 1) {
        rewriter->assembler->push(RBP);
        rewriter->assembler->mov(Immediate(val), RBP);
        rewriter->assembler->cmp(Indirect(fromArgnum(this->argnum), offset), RBP);
        rewriter->assembler->pop(RBP);
    } else {
        rewriter->assembler->cmp(Indirect(fromArgnum(this->argnum), offset), Immediate(val));
    }
    rewriter->assembler->jne(JumpDestination::fromStart(rewriter->rewrite->getSlotSize() - bytes / 8));
}

void RewriterVar::addGuardNotEq(intptr_t val) {
    assert(!rewriter->changed_something && "too late to add a guard!");
    assertValid();

    rewriter->checkArgsValid();

    int bytes = 8 * rewriter->pushes.size() + rewriter->alloca_bytes;
    rewriter->assembler->cmp(fromArgnum(this->argnum), Immediate(val));
    rewriter->assembler->je(JumpDestination::fromStart(rewriter->rewrite->getSlotSize() - bytes / 8));
}

bool RewriterVar::isInReg() {
    int num_arg_regs = 6;
    return argnum < num_arg_regs;
}

void RewriterVar::push() {
    assertValid();
    assert(isInReg());

    rewriter->assembler->push(fromArgnum(this->argnum));
    rewriter->addPush(this->version);
}

RewriterVar RewriterVar::cmp(AST_TYPE::AST_TYPE cmp_type, const RewriterVar& val, int dest) {
    assertValid();

    rewriter->assembler->cmp(fromArgnum(this->argnum), fromArgnum(val.argnum));
    switch (cmp_type) {
        case AST_TYPE::Eq:
            rewriter->assembler->sete(fromArgnum(dest));
            break;
        case AST_TYPE::NotEq:
            rewriter->assembler->setne(fromArgnum(dest));
            break;
        default:
            RELEASE_ASSERT(0, "%d", cmp_type);
    }

    int version = rewriter->mutate(dest);
    return RewriterVar(rewriter, dest, version);
}

RewriterVar RewriterVar::toBool(int dest) {
    assertValid();

    rewriter->assembler->test(fromArgnum(this->argnum), fromArgnum(this->argnum));
    rewriter->assembler->setnz(fromArgnum(dest));

    int version = rewriter->mutate(dest);
    return RewriterVar(rewriter, dest, version);
}

RewriterVar RewriterVar::add(int64_t amount) {
    assertValid();

    if (amount > 0)
        rewriter->assembler->add(assembler::Immediate(amount), fromArgnum(this->argnum));
    else
        rewriter->assembler->sub(assembler::Immediate(-amount), fromArgnum(this->argnum));
    int new_version = rewriter->mutate(this->argnum);
    return RewriterVar(rewriter, this->argnum, new_version);
}

Rewriter* Rewriter::createRewriter(void* ic_rtn_addr, int num_orig_args, int num_temp_regs, const char* debug_name) {
    assert(num_temp_regs <= 2 && "unsupported");

    static StatCounter rewriter_nopatch("rewriter_nopatch");

    ICInfo* ic = getICInfo(ic_rtn_addr);
    if (ic == NULL) {
        rewriter_nopatch.log();
        return NULL;
    }

    assert(ic->getCallingConvention() == llvm::CallingConv::C && "Rewriter[1] only supports the C calling convention!");
    return new Rewriter(ic->startRewrite(debug_name), num_orig_args, num_temp_regs);
}

Rewriter::Rewriter(ICSlotRewrite* rewrite, int num_orig_args, int num_temp_regs)
    : rewrite(rewrite), assembler(rewrite->getAssembler()), num_orig_args(num_orig_args), num_temp_regs(num_temp_regs),
      alloca_bytes(0), max_pushes(0)
#ifndef NDEBUG
      ,
      next_version(2), changed_something(false)
#endif
      ,
      ndecisions(0), decision_path(1) {

// printf("trapping here\n");
// assembler->trap();

// for (int i = 0; i < num_temp_regs; i++) {
// icentry->push(-2 - i);
//}

#ifndef NDEBUG
    for (int i = -5; i < MAX_ARGS; i++) {
        versions[i] = next_version++;
    }
#endif
}

void Rewriter::addPush(int version) {
    pushes.push_back(version);
    max_pushes = std::max(max_pushes, (int)pushes.size());
}

RewriterVar Rewriter::alloca_(int bytes, int dest_argnum) {
    // TODO should check to make sure we aren't crossing push+pops and allocas
    // printf("alloca()ing %d bytes\n", bytes);
    assert(bytes % sizeof(void*) == 0);
    alloca_bytes += bytes;

    assembler->sub(Immediate(bytes), RSP);
    assembler->mov(RSP, fromArgnum(dest_argnum));

    int version = mutate(dest_argnum);
    return RewriterVar(this, dest_argnum, version);
}

RewriterVar Rewriter::getArg(int argnum) {
    assert(argnum >= -1);
    assert(argnum < MAX_ARGS);
#ifndef NDEBUG
    int version = versions[argnum];
    assert(version);
    assert(version == argnum + 7);
#else
    int version = 0;
#endif
    return RewriterVar(this, argnum, version);
}

RewriterVar Rewriter::getRsp() {
    int argnum = -4;
#ifndef NDEBUG
    int version = versions[argnum];
#else
    int version = 0;
#endif
    assert(version);
    return RewriterVar(this, argnum, version);
}

RewriterVar Rewriter::getRbp() {
    int argnum = -5;
#ifndef NDEBUG
    int version = versions[argnum];
#else
    int version = 0;
#endif
    assert(version);
    return RewriterVar(this, argnum, version);
}

#ifndef NDEBUG
void Rewriter::checkArgsValid() {
    for (int i = 0; i < num_orig_args; i++)
        checkVersion(i, i + 7);
}

int Rewriter::mutate(int argnum) {
    ASSERT(locked.count(argnum) == 0, "arg %d is locked!", argnum);
    assert(versions.count(argnum));

    int rtn_version = ++next_version;
    // printf("mutating %d to %d\n", argnum, rtn_version);
    versions[argnum] = rtn_version;
    return rtn_version;
}

void Rewriter::lock(int argnum) {
    assert(locked.count(argnum) == 0);
    locked.insert(argnum);
}

void Rewriter::unlock(int argnum) {
    assert(locked.count(argnum) == 1);
    locked.erase(argnum);
}

void Rewriter::checkVersion(int argnum, int version) {
    assert(version > 0);
    ASSERT(version == versions[argnum], "arg %d got updated from %d to %d", argnum, version, versions[argnum]);
}
#endif

void Rewriter::trap() {
    assembler->trap();
}

void Rewriter::nop() {
    assembler->nop();
}

void Rewriter::annotate(int num) {
    assembler->emitAnnotation(num);
}

RewriterVar Rewriter::loadConst(int argnum, intptr_t val) {
    assembler->mov(Immediate(val), fromArgnum(argnum));
    int version = mutate(argnum);
    return RewriterVar(this, argnum, version);
}

RewriterVar Rewriter::call(void* func_addr) {
#ifndef NDEBUG
    changed_something = true;
#endif
    // printf("%ld pushes, %d alloca bytes\n", pushes.size(), alloca_bytes);

    int bytes = 8 * pushes.size() + alloca_bytes;
    bool didpush;
    if (bytes % 16 == 8) {
        assembler->push(RDI);
        didpush = true;
    } else {
        assert(bytes % 16 == 0);
        didpush = false;
    }

    assembler->emitCall(func_addr, R11);

    if (didpush)
        assembler->pop(RDI);

#ifndef NDEBUG
    int num_arg_regs = 6;
    for (int i = -3; i < num_arg_regs; i++) {
        mutate(i);
    }
#endif
    return RewriterVar(this, -1, mutate(-1));
}

RewriterVar Rewriter::pop(int argnum) {
    assert(pushes.size() > 0);

    int version = pushes.back();
    pushes.pop_back();
#ifndef NDEBUG
    versions[argnum] = version;
#endif
    // printf("popping %d to %d\n", version, argnum);

    assembler->pop(fromArgnum(argnum));
    return RewriterVar(this, argnum, version);
}

void Rewriter::addDecision(int way) {
    assert(ndecisions < 60);
    ndecisions++;
    decision_path = (decision_path << 1) | way;
}

void Rewriter::addDependenceOn(ICInvalidator& invalidator) {
    rewrite->addDependenceOn(invalidator);
}

void Rewriter::commit() {
    static StatCounter rewriter_commits("rewriter_commits");
    rewriter_commits.log();

    // make sure we left the stack the way we found it:
    assert(pushes.size() == 0);
    assert(alloca_bytes == 0);

    rewrite->commit(decision_path, this);
}

void Rewriter::finishAssembly(int continue_offset) {
    assembler->jmp(JumpDestination::fromStart(continue_offset));

    assembler->fillWithNopsExcept(max_pushes);
    for (int i = 0; i < max_pushes; i++) {
        assembler->pop(RAX);
    }
}
}
