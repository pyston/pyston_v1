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

#include "asm_writing/icinfo.h"

#include <cstring>
#include <memory>

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Memory.h"

#include "asm_writing/assembler.h"
#include "asm_writing/mc_writer.h"
#include "codegen/patchpoints.h"
#include "codegen/type_recording.h"
#include "codegen/unwinding.h"
#include "core/common.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

using namespace pyston::assembler;

#define MAX_RETRY_BACKOFF 1024

int64_t ICInvalidator::version() {
    return cur_version;
}

ICInvalidator::~ICInvalidator() {
    // It's not clear what we should do if there are any dependencies still tracked
    // when this object is deleted.  The most likely thing is that we should invalidate
    // them, since whatever caused the destruction is most likely an invalidation event
    // as well.
    // For now, let's just assert on this to know if it happens.  In the unlikely
    // case that we want to just ignore any existing dependents (old behavior), we
    // can add a new API call to forget them (and remove from the dependents' `invalidators` list)
    ASSERT(dependents.empty(), "dependents left when ICInvalidator destructed");
}

void ICInvalidator::addDependent(ICSlotInfo* entry_info) {
    auto p = dependents.insert(entry_info);
    bool was_inserted = p.second;
    if (was_inserted)
        entry_info->invalidators.push_back(this);
}

void ICInvalidator::invalidateAll() {
    cur_version++;
    for (ICSlotInfo* slot : dependents) {
        bool found_self = false;
        for (auto invalidator : slot->invalidators) {
            if (invalidator == this) {
                assert(!found_self);
                found_self = true;
            } else {
                assert(invalidator->dependents.count(slot));
                invalidator->dependents.erase(slot);
            }
        }
        assert(found_self);

        slot->invalidators.clear();
        slot->clear();
    }
    dependents.clear();
}

void ICSlotInfo::clear(bool should_invalidate) {
    if (should_invalidate)
        ic->invalidate(this);
    used = false;

    // Have to be careful here: DECREF can end up recursively clearing this slot
    std::vector<void*> saved_gc_references;
    std::swap(saved_gc_references, gc_references);
    for (auto p : saved_gc_references) {
        Py_DECREF(p);
    }
    saved_gc_references.clear();

    for (auto&& invalidator : invalidators) {
        invalidator->remove(this);
    }
    invalidators.clear();

    if (num_inside == 0)
        decref_infos.clear();
}

std::unique_ptr<ICSlotRewrite> ICSlotRewrite::create(ICInfo* ic, const char* debug_name) {
    auto ic_entry = ic->pickEntryForRewrite(debug_name);
    if (!ic_entry)
        return NULL;
    return std::unique_ptr<ICSlotRewrite>(new ICSlotRewrite(ic_entry, debug_name));
}

ICSlotRewrite::ICSlotRewrite(ICSlotInfo* ic_entry, const char* debug_name)
    : ic_entry(ic_entry),
      debug_name(debug_name),
      buf((uint8_t*)malloc(ic_entry->size)),
      assembler(buf, ic_entry->size) {
    // set num_inside = 1 to make sure that we will not have multiple rewriters at the same time rewriting the same slot
    assert(ic_entry->num_inside == 0);
    ++ic_entry->num_inside;

    assembler.nop();
    if (VERBOSITY() >= 4)
        printf("starting %s icentry\n", debug_name);
}

ICSlotRewrite::~ICSlotRewrite() {
    free(buf);
    --ic_entry->num_inside;
}

void ICSlotRewrite::abort() {
    auto ic = getICInfo();
    ic->retry_backoff = std::min(MAX_RETRY_BACKOFF, 2 * ic->retry_backoff);
    ic->retry_in = ic->retry_backoff;
}



void ICSlotRewrite::commit(CommitHook* hook, std::vector<void*> gc_references,
                           std::vector<std::pair<uint64_t, std::vector<Location>>> decref_infos,
                           llvm::ArrayRef<NextSlotJumpInfo> next_slot_jumps) {
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
        for (auto p : gc_references)
            Py_DECREF(p);
        return;
    }

    // I think this can happen if another thread enters the IC?
    RELEASE_ASSERT(ic_entry->num_inside == 1, "picked IC slot is somehow used again");

    auto ic = getICInfo();
    uint8_t* slot_start = getSlotStart();
    uint8_t* continue_point = (uint8_t*)ic->continue_addr;

    bool should_fill_with_nops = true;
    bool variable_size_slots = true;
    bool do_commit = hook->finishAssembly(continue_point - slot_start, should_fill_with_nops, variable_size_slots);

    if (!do_commit) {
        for (auto p : gc_references)
            Py_DECREF(p);
        return;
    }

    assert(!assembler.hasFailed());
    int original_size = ic_entry->size;
    int actual_size = assembler.bytesWritten();
    int empty_space = original_size - actual_size;
    assert(actual_size <= original_size);
    assert(assembler.size() == original_size);
    if (should_fill_with_nops) {
        assembler.fillWithNops();
        assert(original_size == assembler.bytesWritten());
    }

    ic->next_slot_to_try++;

    // we can create a new IC slot if this is the last slot in the IC in addition we are checking that the new slot is
    // at least as big as the current one.
    bool should_create_new_slot = variable_size_slots && &ic->slots.back() == ic_entry && empty_space >= actual_size;
    if (should_create_new_slot) {
        // reduce size of the current slot to the real size
        ic_entry->size = actual_size;

        // after resizing this slot we need to patch the jumps to the next slot
        Assembler new_asm(assembler.getStartAddr(), original_size);
        for (auto&& jump : next_slot_jumps) {
            auto jmp_inst_offset = std::get<0>(jump);
            auto jmp_inst_end = std::get<1>(jump);
            auto jmp_condition = std::get<2>(jump);
            new_asm.setCurInstPointer(assembler.getStartAddr() + jmp_inst_offset);
            new_asm.jmp_cond(assembler::JumpDestination::fromStart(actual_size), jmp_condition);

            // we often end up using a smaller encoding so we have to make sure we fill the space with nops
            while (new_asm.bytesWritten() < jmp_inst_end)
                new_asm.nop();
        }

        // put a jump to the slowpath at the beginning of the new slot
        Assembler asm_next_slot(assembler.getStartAddr() + actual_size, empty_space);
        asm_next_slot.jmp(JumpDestination::fromStart(empty_space));

        // add the new slot
        ic->slots.emplace_back(ic, ic_entry->start_addr + actual_size, empty_space);
    }

    // if (VERBOSITY()) printf("Committing to %p-%p\n", start, start + ic->slot_size);
    memcpy(slot_start, buf, original_size);

    ic_entry->clear(false /* don't invalidate */);

    ic_entry->gc_references = std::move(gc_references);
    ic_entry->used = true;
    ic->times_rewritten++;

    for (int i = 0; i < dependencies.size(); i++) {
        ICInvalidator* invalidator = dependencies[i].first;
        invalidator->addDependent(ic_entry);
    }

    if (ic->times_rewritten == IC_MEGAMORPHIC_THRESHOLD) {
        static StatCounter megamorphic_ics("megamorphic_ics");
        megamorphic_ics.log();
    }

    // deregister old decref infos
    ic_entry->decref_infos.clear();

    // register new decref info
    for (auto&& decref_info : decref_infos) {
        // add decref locations which are always to decref inside this IC
        auto&& merged_locations = decref_info.second;
        merged_locations.insert(merged_locations.end(), ic->ic_global_decref_locations.begin(),
                                ic->ic_global_decref_locations.end());
        if (merged_locations.empty())
            continue;

        ic_entry->decref_infos.emplace_back(decref_info.first, std::move(merged_locations));
    }

    llvm::sys::Memory::InvalidateInstructionCache(slot_start, original_size);
}

void ICSlotRewrite::addDependenceOn(ICInvalidator& invalidator) {
    dependencies.push_back(std::make_pair(&invalidator, invalidator.version()));
}

int ICInfo::calculateSuggestedSize() {
    // if we never rewrote this IC just return the whole IC size for now
    if (!times_rewritten)
        return slots.begin()->size;

    int additional_space_per_slot = 50;
    // if there are less rewrites than slots we can give a very accurate estimate
    if (times_rewritten < slots.size()) {
        // add up the sizes of all used slots
        int size = 0;
        int i = 0;
        for (auto&& slot : slots) {
            if (i >= times_rewritten)
                break;
            size += slot.size + additional_space_per_slot;
            ++i;
        }
        return size;
    }

    // get total size of IC
    int size = 0;
    for (auto&& slot : slots) {
        size += slot.size;
    }
    // make it bigger
    if (isMegamorphic())
        size *= 4;
    else
        size *= 2;
    return std::min(size, 4096);
}

std::unique_ptr<ICSlotRewrite> ICInfo::startRewrite(const char* debug_name) {
    return ICSlotRewrite::create(this, debug_name);
}

ICSlotInfo* ICInfo::pickEntryForRewrite(const char* debug_name) {
    int num_slots = slots.size();
    int fallback_to_in_use_slot = -1;

    llvm::SmallVector<ICSlotInfo*, 8> slots_vec;
    for (auto&& slot : slots) {
        slots_vec.push_back(&slot);
    }

    // we prefer to use a unused slot and if non is available we will fallback to a slot which is in use (but no one is
    // inside)
    for (int _i = 0; _i < num_slots; _i++) {
        int i = (_i + next_slot_to_try) % num_slots;

        ICSlotInfo* sinfo = slots_vec[i];
        assert(sinfo->num_inside >= 0);

        if (sinfo->num_inside || sinfo->size == 0)
            continue;

        if (sinfo->used) {
            if (fallback_to_in_use_slot == -1)
                fallback_to_in_use_slot = i;
            continue;
        }

        if (VERBOSITY() >= 4) {
            printf("picking %s icentry to in-use slot %d at %p\n", debug_name, i, start_addr);
        }

        next_slot_to_try = i;
        return sinfo;
    }

    if (fallback_to_in_use_slot != -1) {
        if (VERBOSITY() >= 4) {
            printf("picking %s icentry to in-use slot %d at %p\n", debug_name, fallback_to_in_use_slot, start_addr);
        }

        next_slot_to_try = fallback_to_in_use_slot;
        return slots_vec[fallback_to_in_use_slot];
    }

    if (VERBOSITY() >= 4)
        printf("not committing %s icentry since there are no available slots\n", debug_name);
    return NULL;
}

static llvm::DenseMap<void*, ICInfo*> ics_by_return_addr;
static llvm::DenseMap<BST_stmt*, ICInfo*> ics_by_ast_node;

ICInfo::ICInfo(void* start_addr, void* slowpath_rtn_addr, void* continue_addr, StackInfo stack_info, int size,
               llvm::CallingConv::ID calling_conv, LiveOutSet _live_outs, assembler::GenericRegister return_register,
               std::vector<Location> ic_global_decref_locations, assembler::RegisterSet allocatable_registers)
    : next_slot_to_try(0),
      stack_info(stack_info),
      calling_conv(calling_conv),
      live_outs(std::move(_live_outs)),
      return_register(return_register),
      retry_in(0),
      retry_backoff(1),
      times_rewritten(0),
      allocatable_registers(allocatable_registers),
      ic_global_decref_locations(std::move(ic_global_decref_locations)),
      node(NULL),
      start_addr(start_addr),
      slowpath_rtn_addr(slowpath_rtn_addr),
      continue_addr(continue_addr) {
    slots.emplace_back(this, (uint8_t*)start_addr, size);
    if (slowpath_rtn_addr && !this->ic_global_decref_locations.empty())
        slowpath_decref_info = DecrefInfo((uint64_t)slowpath_rtn_addr, this->ic_global_decref_locations);
}

ICInfo::~ICInfo() {
    // if this ICInfo got created with registerCompiledPatchpoint we have to unregister this
    ics_by_return_addr.erase(slowpath_rtn_addr);
    if (node)
        ics_by_ast_node.erase(node);

    for (auto& slot : slots) {
        // Calling a full clear() might be overkill here, but probably better safe than sorry:
        slot.clear(false);
    }
}

DecrefInfo::DecrefInfo(uint64_t ip, std::vector<Location> locations) : ip(ip) {
    addDecrefInfoEntry(ip, std::move(locations));
}

void DecrefInfo::reset() {
    if (ip) {
        removeDecrefInfoEntry(ip);
        ip = 0;
    }
}

std::unique_ptr<ICInfo> registerCompiledPatchpoint(uint8_t* start_addr, uint8_t* slowpath_start_addr,
                                                   uint8_t* continue_addr, uint8_t* slowpath_rtn_addr,
                                                   const ICSetupInfo* ic, StackInfo stack_info, LiveOutSet live_outs,
                                                   std::vector<Location> decref_info) {
    assert(slowpath_start_addr - start_addr >= ic->size);
    assert(slowpath_rtn_addr > slowpath_start_addr);
    assert(slowpath_rtn_addr <= start_addr + ic->totalSize());

    assembler::GenericRegister return_register;
    assert(ic->getCallingConvention() == llvm::CallingConv::C
           || ic->getCallingConvention() == llvm::CallingConv::PreserveAll);

    if (ic->hasReturnValue()) {
        static const int DWARF_RAX = 0;
        // It's possible that the return value doesn't get used, in which case
        // we can avoid copying back into RAX at the end
        live_outs.clear(DWARF_RAX);

        // TODO we only need to do this if 0 was in live_outs, since if it wasn't, that indicates
        // the return value won't be used and we can optimize based on that.
        return_register = assembler::RAX;
    }

    // we can let the user just slide down the nop section, but instead
    // emit jumps to the end.
    // Not sure if this is worth it or not?
    Assembler writer(start_addr, ic->size);
    writer.nop();
    writer.jmp(JumpDestination::fromStart(slowpath_start_addr - start_addr));

    ICInfo* icinfo
        = new ICInfo(start_addr, slowpath_rtn_addr, continue_addr, stack_info, ic->size, ic->getCallingConvention(),
                     std::move(live_outs), return_register, decref_info, ic->allocatable_regs);

    assert(!ics_by_return_addr.count(slowpath_rtn_addr));
    ics_by_return_addr[slowpath_rtn_addr] = icinfo;

    return std::unique_ptr<ICInfo>(icinfo);
}

ICInfo* getICInfo(void* rtn_addr) {
    // TODO: load this from the CF instead of tracking it separately
    auto&& it = ics_by_return_addr.find(rtn_addr);
    if (it == ics_by_return_addr.end())
        return NULL;
    return it->second;
}

void ICInfo::invalidate(ICSlotInfo* icentry) {
    assert(icentry);

    uint8_t* start = (uint8_t*)icentry->start_addr;

    if (VERBOSITY() >= 4)
        printf("clearing patchpoint %p, slot at %p\n", start_addr, start);

    Assembler writer(start, icentry->size);
    writer.nop();
    writer.jmp(JumpDestination::fromStart(icentry->size));
    assert(writer.bytesWritten() <= IC_INVALDITION_HEADER_SIZE);

    // std::unique_ptr<MCWriter> writer(createMCWriter(start, getSlotSize(), 0));
    // writer->emitNop();
    // writer->emitGuardFalse();

    // writer->endWithSlowpath();

    llvm::sys::Memory::InvalidateInstructionCache(start, icentry->size);

    int i = 0;
    for (auto&& slot : slots) {
        if (&slot == icentry) {
            next_slot_to_try = i;
            break;
        }
        ++i;
    }

    icentry->used = false;
}

bool ICInfo::shouldAttempt() {
    if (retry_in) {
        retry_in--;
        return false;
    }
    // Note(kmod): in some pathological deeply-recursive cases, it's important that we set the
    // retry counter even if we attempt it again.  We could probably handle this by setting
    // the backoff to 0 on commit, and then setting the retry to the backoff here.

    return !isMegamorphic() && ENABLE_ICS;
}

bool ICInfo::isMegamorphic() {
    return times_rewritten >= IC_MEGAMORPHIC_THRESHOLD;
}

ICInfo* ICInfo::getICInfoForNode(BST_stmt* node) {
    auto&& it = ics_by_ast_node.find(node);
    if (it != ics_by_ast_node.end())
        return it->second;
    return NULL;
}
void ICInfo::associateNodeWithICInfo(BST_stmt* node, std::unique_ptr<TypeRecorder> type_recorder) {
    assert(!this->node);
    this->node = node;
    this->type_recorder = std::move(type_recorder);
    ics_by_ast_node[node] = this;
}
void ICInfo::appendDecrefInfosTo(std::vector<DecrefInfo>& dest_decref_infos) {
    if (slowpath_decref_info.ip)
        dest_decref_infos.emplace_back(std::move(slowpath_decref_info));
    for (auto&& slot : slots) {
        for (DecrefInfo& decref_info : slot.decref_infos) {
            dest_decref_infos.emplace_back(std::move(decref_info));
            assert(decref_info.ip == 0 && "this can only happen if we copied instead of moved the value");
        }
        slot.decref_infos.clear();
    }
}

void clearAllICs() {
    for (auto&& p : ics_by_return_addr) {
        p.second->clearAll();
    }
}
}
