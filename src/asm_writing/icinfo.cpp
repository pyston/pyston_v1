// Copyright (c) 2014-2015 Dropbox, Inc.
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

#include "asm_writing/icinfo.h"

#include <cstring>
#include <memory>

#include "llvm/Support/Memory.h"

#include "asm_writing/assembler.h"
#include "asm_writing/mc_writer.h"
#include "codegen/patchpoints.h"
#include "core/common.h"
#include "core/options.h"
#include "core/types.h"

namespace pyston {

using namespace pyston::assembler;

#define MEGAMORPHIC_THRESHOLD 100
#define MAX_RETRY_BACKOFF 1024

// TODO not right place for this...
int64_t ICInvalidator::version() {
    return cur_version;
}

void ICInvalidator::addDependent(ICSlotInfo* entry_info) {
    dependents.insert(entry_info);
}

void ICInvalidator::invalidateAll() {
    cur_version++;
    for (ICSlotInfo* slot : dependents) {
        slot->clear();
    }
    dependents.clear();
}



void ICSlotInfo::clear() {
    ic->clear(this);
}

ICSlotRewrite::ICSlotRewrite(ICInfo* ic, const char* debug_name)
    : ic(ic), debug_name(debug_name), buf((uint8_t*)malloc(ic->getSlotSize())), assembler(buf, ic->getSlotSize()) {
    assembler.nop();

    if (VERBOSITY() >= 4)
        printf("starting %s icentry\n", debug_name);
}

ICSlotRewrite::~ICSlotRewrite() {
    free(buf);
}

void ICSlotRewrite::abort() {
    ic->retry_backoff = std::min(MAX_RETRY_BACKOFF, 2 * ic->retry_backoff);
    ic->retry_in = ic->retry_backoff;
}

ICSlotInfo* ICSlotRewrite::prepareEntry() {
    this->ic_entry = ic->pickEntryForRewrite(debug_name);
    return this->ic_entry;
}

uint8_t* ICSlotRewrite::getSlotStart() {
    assert(ic_entry != NULL);
    return (uint8_t*)ic->start_addr + ic_entry->idx * ic->getSlotSize();
}

void ICSlotRewrite::commit(CommitHook* hook) {
    bool still_valid = true;
    for (int i = 0; i < dependencies.size(); i++) {
        int orig_version = dependencies[i].second;
        ICInvalidator* invalidator = dependencies[i].first;
        if (orig_version != invalidator->version()) {
            still_valid = false;
            break;
        }
    }
    if (!still_valid) {
        if (VERBOSITY() >= 3)
            printf("not committing %s icentry since a dependency got updated before commit\n", debug_name);
        return;
    }

    uint8_t* slot_start = getSlotStart();
    uint8_t* continue_point = (uint8_t*)ic->continue_addr;

    bool do_commit = hook->finishAssembly(continue_point - slot_start);

    if (!do_commit)
        return;

    assert(!assembler.hasFailed());

    for (int i = 0; i < dependencies.size(); i++) {
        ICInvalidator* invalidator = dependencies[i].first;
        invalidator->addDependent(ic_entry);
    }

    ic->next_slot_to_try++;

    // if (VERBOSITY()) printf("Commiting to %p-%p\n", start, start + ic->slot_size);
    memcpy(slot_start, buf, ic->getSlotSize());

    ic->times_rewritten++;

    if (ic->times_rewritten == MEGAMORPHIC_THRESHOLD) {
        static StatCounter megamorphic_ics("megamorphic_ics");
        megamorphic_ics.log();
    }

    llvm::sys::Memory::InvalidateInstructionCache(slot_start, ic->getSlotSize());
}

void ICSlotRewrite::addDependenceOn(ICInvalidator& invalidator) {
    dependencies.push_back(std::make_pair(&invalidator, invalidator.version()));
}

int ICSlotRewrite::getSlotSize() {
    return ic->getSlotSize();
}

int ICSlotRewrite::getScratchRspOffset() {
    assert(ic->stack_info.scratch_size);
    return ic->stack_info.scratch_rsp_offset;
}

int ICSlotRewrite::getScratchSize() {
    return ic->stack_info.scratch_size;
}

TypeRecorder* ICSlotRewrite::getTypeRecorder() {
    return ic->type_recorder;
}

assembler::GenericRegister ICSlotRewrite::returnRegister() {
    return ic->return_register;
}



std::unique_ptr<ICSlotRewrite> ICInfo::startRewrite(const char* debug_name) {
    return std::unique_ptr<ICSlotRewrite>(new ICSlotRewrite(this, debug_name));
}

ICSlotInfo* ICInfo::pickEntryForRewrite(const char* debug_name) {
    int num_slots = getNumSlots();
    for (int _i = 0; _i < num_slots; _i++) {
        int i = (_i + next_slot_to_try) % num_slots;

        ICSlotInfo& sinfo = slots[i];
        assert(sinfo.num_inside >= 0);
        if (sinfo.num_inside)
            continue;

        if (VERBOSITY() >= 4) {
            printf("picking %s icentry to in-use slot %d at %p\n", debug_name, i, start_addr);
        }

        next_slot_to_try = i;
        return &sinfo;
    }
    if (VERBOSITY() >= 4)
        printf("not committing %s icentry since there are no available slots\n", debug_name);
    return NULL;
}

ICInfo::ICInfo(void* start_addr, void* slowpath_rtn_addr, void* continue_addr, StackInfo stack_info, int num_slots,
               int slot_size, llvm::CallingConv::ID calling_conv, const std::unordered_set<int>& live_outs,
               assembler::GenericRegister return_register, TypeRecorder* type_recorder)
    : next_slot_to_try(0),
      stack_info(stack_info),
      num_slots(num_slots),
      slot_size(slot_size),
      calling_conv(calling_conv),
      live_outs(live_outs.begin(), live_outs.end()),
      return_register(return_register),
      type_recorder(type_recorder),
      retry_in(0),
      retry_backoff(1),
      times_rewritten(0),
      start_addr(start_addr),
      slowpath_rtn_addr(slowpath_rtn_addr),
      continue_addr(continue_addr) {
    for (int i = 0; i < num_slots; i++) {
        slots.push_back(ICSlotInfo(this, i));
    }
}

static llvm::DenseMap<void*, ICInfo*> ics_by_return_addr;
std::unique_ptr<ICInfo> registerCompiledPatchpoint(uint8_t* start_addr, uint8_t* slowpath_start_addr,
                                                   uint8_t* continue_addr, uint8_t* slowpath_rtn_addr,
                                                   const ICSetupInfo* ic, StackInfo stack_info,
                                                   std::unordered_set<int> live_outs) {
    assert(slowpath_start_addr - start_addr >= ic->num_slots * ic->slot_size);
    assert(slowpath_rtn_addr > slowpath_start_addr);
    assert(slowpath_rtn_addr <= start_addr + ic->totalSize());

    assembler::GenericRegister return_register;
    assert(ic->getCallingConvention() == llvm::CallingConv::C
           || ic->getCallingConvention() == llvm::CallingConv::PreserveAll);

    if (ic->hasReturnValue()) {
        static const int DWARF_RAX = 0;
        // It's possible that the return value doesn't get used, in which case
        // we can avoid copying back into RAX at the end
        if (live_outs.count(DWARF_RAX)) {
            live_outs.erase(DWARF_RAX);
        }

        // TODO we only need to do this if 0 was in live_outs, since if it wasn't, that indicates
        // the return value won't be used and we can optimize based on that.
        return_register = assembler::RAX;
    }

    // we can let the user just slide down the nop section, but instead
    // emit jumps to the end.
    // Not sure if this is worth it or not?
    for (int i = 0; i < ic->num_slots; i++) {
        uint8_t* start = start_addr + i * ic->slot_size;
        // std::unique_ptr<MCWriter> writer(createMCWriter(start, ic->slot_size * (ic->num_slots - i), 0));
        // writer->emitNop();
        // writer->emitGuardFalse();

        Assembler writer(start, ic->slot_size);
        writer.nop();
        // writer.trap();
        // writer.jmp(JumpDestination::fromStart(ic->slot_size * (ic->num_slots - i)));
        writer.jmp(JumpDestination::fromStart(slowpath_start_addr - start));
    }

    ICInfo* icinfo = new ICInfo(start_addr, slowpath_rtn_addr, continue_addr, stack_info, ic->num_slots, ic->slot_size,
                                ic->getCallingConvention(), live_outs, return_register, ic->type_recorder);

    ics_by_return_addr[slowpath_rtn_addr] = icinfo;

    return std::unique_ptr<ICInfo>(icinfo);
}

void deregisterCompiledPatchpoint(ICInfo* ic) {
    assert(ics_by_return_addr.count(ic->slowpath_rtn_addr));
    ics_by_return_addr.erase(ic->slowpath_rtn_addr);
}

ICInfo* getICInfo(void* rtn_addr) {
    // TODO: load this from the CF instead of tracking it separately
    auto&& it = ics_by_return_addr.find(rtn_addr);
    if (it == ics_by_return_addr.end())
        return NULL;
    return it->second;
}

void ICInfo::clear(ICSlotInfo* icentry) {
    assert(icentry);

    uint8_t* start = (uint8_t*)start_addr + icentry->idx * getSlotSize();

    if (VERBOSITY() >= 4)
        printf("clearing patchpoint %p, slot at %p\n", start_addr, start);

    Assembler writer(start, getSlotSize());
    writer.nop();
    writer.jmp(JumpDestination::fromStart(getSlotSize()));
    assert(writer.bytesWritten() <= IC_INVALDITION_HEADER_SIZE);

    // std::unique_ptr<MCWriter> writer(createMCWriter(start, getSlotSize(), 0));
    // writer->emitNop();
    // writer->emitGuardFalse();

    // writer->endWithSlowpath();
    llvm::sys::Memory::InvalidateInstructionCache(start, getSlotSize());
}

bool ICInfo::shouldAttempt() {
    if (retry_in) {
        retry_in--;
        return false;
    }
    // Note(kmod): in some pathological deeply-recursive cases, it's important that we set the
    // retry counter even if we attempt it again.  We could probably handle this by setting
    // the backoff to 0 on commit, and then setting the retry to the backoff here.

    return !isMegamorphic();
}

bool ICInfo::isMegamorphic() {
    return times_rewritten >= MEGAMORPHIC_THRESHOLD;
}
}
