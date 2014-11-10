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

ICSlotRewrite::ICSlotRewrite(ICInfo* ic, const char* debug_name) : ic(ic), debug_name(debug_name) {
    buf = (uint8_t*)malloc(ic->getSlotSize());
    assembler = new Assembler(buf, ic->getSlotSize());
    assembler->nop();

    if (VERBOSITY())
        printf("starting %s icentry\n", debug_name);
}

ICSlotRewrite::~ICSlotRewrite() {
    delete assembler;
    free(buf);
}

void ICSlotRewrite::abort() {
    ic->failed = true;
}

void ICSlotRewrite::commit(uint64_t decision_path, CommitHook* hook) {
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
        if (VERBOSITY())
            printf("not committing %s icentry since a dependency got updated before commit\n", debug_name);
        return;
    }

    ICSlotInfo* ic_entry = ic->pickEntryForRewrite(decision_path, debug_name);
    if (ic_entry == NULL)
        return;

    for (int i = 0; i < dependencies.size(); i++) {
        ICInvalidator* invalidator = dependencies[i].first;
        invalidator->addDependent(ic_entry);
    }

    uint8_t* slot_start = (uint8_t*)ic->start_addr + ic_entry->idx * ic->getSlotSize();
    uint8_t* continue_point = (uint8_t*)ic->continue_addr;

    hook->finishAssembly(continue_point - slot_start);

    assert(assembler->isExactlyFull());

    // if (VERBOSITY()) printf("Commiting to %p-%p\n", start, start + ic->slot_size);
    memcpy(slot_start, buf, ic->getSlotSize());

    llvm::sys::Memory::InvalidateInstructionCache(slot_start, ic->getSlotSize());
}

void ICSlotRewrite::addDependenceOn(ICInvalidator& invalidator) {
    dependencies.push_back(std::make_pair(&invalidator, invalidator.version()));
}

int ICSlotRewrite::getSlotSize() {
    return ic->getSlotSize();
}

int ICSlotRewrite::getFuncStackSize() {
    return ic->stack_info.stack_size;
}

int ICSlotRewrite::getScratchRbpOffset() {
    assert(ic->stack_info.scratch_bytes);
    return ic->stack_info.scratch_rbp_offset;
}

int ICSlotRewrite::getScratchBytes() {
    assert(ic->stack_info.scratch_bytes);
    return ic->stack_info.scratch_bytes;
}

TypeRecorder* ICSlotRewrite::getTypeRecorder() {
    return ic->type_recorder;
}

assembler::GenericRegister ICSlotRewrite::returnRegister() {
    return ic->return_register;
}



ICSlotRewrite* ICInfo::startRewrite(const char* debug_name) {
    return new ICSlotRewrite(this, debug_name);
}

ICSlotInfo* ICInfo::pickEntryForRewrite(uint64_t decision_path, const char* debug_name) {
    for (int i = 0; i < getNumSlots(); i++) {
        SlotInfo& sinfo = slots[i];
        if (!sinfo.is_patched) {
            if (VERBOSITY()) {
                printf("committing %s icentry to unused slot %d at %p\n", debug_name, i, start_addr);
            }

            sinfo.is_patched = true;
            sinfo.decision_path = decision_path;
            return &sinfo.entry;
        }
    }

    int num_slots = getNumSlots();
    for (int _i = 0; _i < num_slots; _i++) {
        int i = (_i + next_slot_to_try) % num_slots;

        SlotInfo& sinfo = slots[i];
        if (sinfo.is_patched && sinfo.decision_path != decision_path) {
            continue;
        }

        if (VERBOSITY()) {
            printf("committing %s icentry to in-use slot %d at %p\n", debug_name, i, start_addr);
        }
        next_slot_to_try++;

        sinfo.is_patched = true;
        sinfo.decision_path = decision_path;
        return &sinfo.entry;
    }
    if (VERBOSITY())
        printf("not committing %s icentry since it is not compatible (%lx)\n", debug_name, decision_path);
    return NULL;
}



ICInfo::ICInfo(void* start_addr, void* continue_addr, StackInfo stack_info, int num_slots, int slot_size,
               llvm::CallingConv::ID calling_conv, const std::unordered_set<int>& live_outs,
               assembler::GenericRegister return_register, TypeRecorder* type_recorder)
    : next_slot_to_try(0), stack_info(stack_info), num_slots(num_slots), slot_size(slot_size),
      calling_conv(calling_conv), live_outs(live_outs.begin(), live_outs.end()), return_register(return_register),
      type_recorder(type_recorder), failed(false), start_addr(start_addr), continue_addr(continue_addr) {
    for (int i = 0; i < num_slots; i++) {
        slots.push_back(SlotInfo(this, i));
    }
}

static std::unordered_map<void*, ICInfo*> ics_by_return_addr;
ICInfo* registerCompiledPatchpoint(uint8_t* start_addr, uint8_t* slowpath_start_addr, uint8_t* continue_addr,
                                   uint8_t* slowpath_rtn_addr, const ICSetupInfo* ic, StackInfo stack_info,
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

        std::unique_ptr<Assembler> writer(new Assembler(start, ic->slot_size));
        writer->nop();
        // writer->trap();
        // writer->jmp(JumpDestination::fromStart(ic->slot_size * (ic->num_slots - i)));
        writer->jmp(JumpDestination::fromStart(slowpath_start_addr - start));
    }

    ICInfo* icinfo = new ICInfo(start_addr, continue_addr, stack_info, ic->num_slots, ic->slot_size,
                                ic->getCallingConvention(), live_outs, return_register, ic->type_recorder);

    ics_by_return_addr[slowpath_rtn_addr] = icinfo;

    return icinfo;
}

ICInfo* getICInfo(void* rtn_addr) {
    // TODO: load this from the CF instead of tracking it separately
    std::unordered_map<void*, ICInfo*>::iterator it = ics_by_return_addr.find(rtn_addr);
    if (it == ics_by_return_addr.end())
        return NULL;
    return it->second;
}

void ICInfo::clear(ICSlotInfo* icentry) {
    assert(icentry);

    uint8_t* start = (uint8_t*)start_addr + icentry->idx * getSlotSize();

    if (VERBOSITY())
        printf("clearing patchpoint %p, slot at %p\n", start_addr, start);

    std::unique_ptr<Assembler> writer(new Assembler(start, getSlotSize()));
    writer->nop();
    writer->jmp(JumpDestination::fromStart(getSlotSize()));
    // std::unique_ptr<MCWriter> writer(createMCWriter(start, getSlotSize(), 0));
    // writer->emitNop();
    // writer->emitGuardFalse();

    // writer->endWithSlowpath();
    llvm::sys::Memory::InvalidateInstructionCache(start, getSlotSize());
}

bool ICInfo::shouldAttempt() {
    return !failed;
}
}
