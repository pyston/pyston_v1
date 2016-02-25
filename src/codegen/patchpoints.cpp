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

#include "codegen/patchpoints.h"

#include <memory>
#include <unordered_map>

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "codegen/compvars.h"
#include "codegen/irgen/util.h"
#include "codegen/stackmaps.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

namespace pyston {

void PatchpointInfo::addFrameVar(llvm::StringRef name, CompilerType* type) {
    frame_vars.push_back(FrameVarInfo({.name = name, .type = type }));
}

int ICSetupInfo::totalSize() const {
    if (isDeopt())
        return DEOPT_CALL_ONLY_SIZE;

    int call_size = CALL_ONLY_SIZE;
    if (getCallingConvention() != llvm::CallingConv::C) {
        // 14 bytes per reg that needs to be spilled
        call_size += 14 * 4;
    }
    return num_slots * slot_size + call_size;
}

static std::vector<std::pair<PatchpointInfo*, void* /* addr of func to call */>> new_patchpoints;

ICSetupInfo* ICSetupInfo::initialize(bool has_return_value, int num_slots, int slot_size, ICType type,
                                     TypeRecorder* type_recorder) {
    ICSetupInfo* rtn = new ICSetupInfo(type, num_slots, slot_size, has_return_value, type_recorder);

    // We use size == CALL_ONLY_SIZE to imply that the call isn't patchable
    assert(rtn->totalSize() > CALL_ONLY_SIZE);

    return rtn;
}

int PatchpointInfo::patchpointSize() {
    if (icinfo) {
        int r = icinfo->totalSize();
        assert(r > CALL_ONLY_SIZE);
        return r;
    }

    return CALL_ONLY_SIZE;
}

bool StackMap::Record::Location::operator==(const StackMap::Record::Location& rhs) {
    // TODO: this check is overly-strict.  Some fields are not used depending
    // on the value of type, and I don't think "flags" is used at all currently.
    return (type == rhs.type) && (flags == rhs.flags) && (regnum == rhs.regnum) && (offset == rhs.offset);
}

void PatchpointInfo::parseLocationMap(StackMap::Record* r, LocationMap* map) {
    assert(r->locations.size() == totalStackmapArgs());

    int cur_arg = frameStackmapArgsStart();

    // printf("parsing pp %ld:\n", reinterpret_cast<int64_t>(this));

    for (FrameVarInfo& frame_var : frame_vars) {
        int num_args = frame_var.type->numFrameArgs();

        llvm::SmallVector<StackMap::Record::Location, 1> locations;
        locations.append(r->locations.data() + cur_arg, r->locations.data() + cur_arg + num_args);

        // printf("%s %d %d\n", frame_var.name.c_str(), r->locations[cur_arg].type, r->locations[cur_arg].regnum);

        map->names[frame_var.name].locations.push_back(
            LocationMap::LocationTable::LocationEntry({._debug_pp_id = (uint64_t) this,
                                                       .offset = r->offset,
                                                       .length = patchpointSize(),
                                                       .type = frame_var.type,
                                                       .locations = std::move(locations) }));

        cur_arg += num_args;
    }
    assert(cur_arg - frameStackmapArgsStart() == numFrameStackmapArgs());
}

static int extractScratchOffset(PatchpointInfo* pp, StackMap::Record* r) {
    StackMap::Record::Location l = r->locations[pp->scratchStackmapArg()];

    static const int DWARF_RBP_REGNUM = 6;

    assert(l.type == StackMap::Record::Location::LocationType::Direct);
    assert(l.regnum == DWARF_RBP_REGNUM);
    return l.offset;
}

static LiveOutSet extractLiveOuts(StackMap::Record* r, llvm::CallingConv::ID cc) {
    LiveOutSet live_outs;

    // Using the C calling convention, there shouldn't be any non-callee-save registers in here,
    // but LLVM is conservative and will add some.  So with the C cc, ignored the specified live outs
    if (cc != llvm::CallingConv::C) {
        for (const auto& live_out : r->live_outs) {
            live_outs.set(live_out.regnum);
        }
    }

    // llvm doesn't consider callee-save registers to be live
    // if they're never allocated, but I think it makes much more
    // sense to track them as live_outs.
    // Unfortunately this means we need to be conservative about it unless
    // we can change llvm's behavior.
    live_outs.set(3);
    live_outs.set(12);
    live_outs.set(13);
    live_outs.set(14);
    live_outs.set(15);

    return live_outs;
}

void processStackmap(CompiledFunction* cf, StackMap* stackmap) {
    int nrecords = stackmap ? stackmap->records.size() : 0;

    assert(cf->location_map == NULL);
    cf->location_map = new LocationMap();
    if (stackmap)
        cf->location_map->constants = stackmap->constants;

    for (int i = 0; i < nrecords; i++) {
        StackMap::Record* r = &stackmap->records[i];

        assert(stackmap->stack_size_records.size() == 1);
        const StackMap::StackSizeRecord& stack_size_record = stackmap->stack_size_records[0];
        int stack_size = stack_size_record.stack_size;


        RELEASE_ASSERT(new_patchpoints.size() > r->id, "");
        PatchpointInfo* pp = new_patchpoints[r->id].first;
        assert(pp);

        if (pp->isFrameInfoStackmap()) {
            assert(r->locations.size() == pp->totalStackmapArgs());
            StackMap::Record::Location frame_info_location = r->locations[0];
            assert(!cf->location_map->frameInfoFound());
            assert(frame_info_location.type == StackMap::Record::Location::Direct);
            assert(frame_info_location.regnum == 6 /* must be rbp based */);
            cf->location_map->frame_info_location = frame_info_location;
            continue;
        }

        void* slowpath_func = PatchpointInfo::getSlowpathAddr(r->id);
        if (VERBOSITY() >= 2) {
            printf("Processing pp %ld; [%d, %d)\n", reinterpret_cast<int64_t>(pp), r->offset,
                   r->offset + pp->patchpointSize());
        }

        assert(r->locations.size() == pp->totalStackmapArgs());

        int scratch_rbp_offset = extractScratchOffset(pp, r);
        int scratch_size = pp->scratchSize();
        assert(scratch_size % sizeof(void*) == 0);
        assert(scratch_rbp_offset % sizeof(void*) == 0);

        uint8_t* start_addr = (uint8_t*)pp->parentFunction()->code + r->offset;
        uint8_t* end_addr = start_addr + pp->patchpointSize();

        if (ENABLE_JIT_OBJECT_CACHE)
            setSlowpathFunc(start_addr, slowpath_func);

        //*start_addr = 0xcc;
        // start_addr++;

        int nspills = 0;
        SpillMap frame_remapped;
        // TODO: if we pass the same llvm::Value as the stackmap args, we will get the same reg.
        // we shouldn't then spill that multiple times.
        // we could either deal with it here, or not put the same Value into the patchpoint
        for (int j = pp->frameStackmapArgsStart(), e = j + pp->numFrameStackmapArgs(); j < e; j++) {
            StackMap::Record::Location& l = r->locations[j];

            // updates l, start_addr, scratch_rbp_offset, scratch_size:
            bool spilled = spillFrameArgumentIfNecessary(l, start_addr, end_addr, scratch_rbp_offset, scratch_size,
                                                         frame_remapped);
            if (spilled)
                nspills++;
        }
        RELEASE_ASSERT(nspills <= pp->numFrameSpillsSupported(), "did %d spills but expected only %d!", nspills,
                       pp->numFrameSpillsSupported());

        assert(scratch_size % sizeof(void*) == 0);
        assert(scratch_rbp_offset % sizeof(void*) == 0);

        // TODO: is something like this necessary?
        // llvm::sys::Memory::InvalidateInstructionCache(start, getSlotSize());

        pp->parseLocationMap(r, cf->location_map);

        const ICSetupInfo* ic = pp->getICInfo();
        if (ic == NULL) {
            // We have to be using the C calling convention here, so we don't need to check the live outs
            // or save them across the call.
            initializePatchpoint3(slowpath_func, start_addr, end_addr, scratch_rbp_offset, scratch_size, LiveOutSet(),
                                  frame_remapped);
            continue;
        }
        LiveOutSet live_outs(extractLiveOuts(r, ic->getCallingConvention()));

        if (ic->hasReturnValue()) {
            assert(ic->getCallingConvention() == llvm::CallingConv::C
                   || ic->getCallingConvention() == llvm::CallingConv::PreserveAll);

            static const int DWARF_RAX = 0;
            // It's possible that the return value doesn't get used, in which case
            // we can avoid copying back into RAX at the end
            live_outs.clear(DWARF_RAX);
        }


        auto initialization_info = initializePatchpoint3(slowpath_func, start_addr, end_addr, scratch_rbp_offset,
                                                         scratch_size, std::move(live_outs), frame_remapped);

        ASSERT(initialization_info.slowpath_start - start_addr >= ic->num_slots * ic->slot_size,
               "Used more slowpath space than expected; change ICSetupInfo::totalSize()?");

        assert(pp->numICStackmapArgs() == 0); // don't do anything with these for now

        // We currently specify the scratch's location as an RSP offset, but LLVM gives it
        // to us as an RBP offset.  It's easy to convert between them if the function has a static
        // stack size, but if the function doesn't have a fixed stack size (which happens if there
        // is a non-static alloca), then we can't convert.
        // Internally, it's easy enough to handle either rsp-relative or rbp-relative offsets
        // for the scratch array, but there are some places that require the use of rsp-relative
        // offsets, and we don't (yet) have the ability to specify on a per-patchpoint basis
        // which one we want to use.
        RELEASE_ASSERT(stack_size >= 0, "function does not have static stack size!");

        // (rbp - rsp) == (stack_size - 8)  -- the "-8" is from the value of rbp being pushed onto the stack
        int scratch_rsp_offset = scratch_rbp_offset + (stack_size - 8);

        std::unique_ptr<ICInfo> icinfo = registerCompiledPatchpoint(
            start_addr, initialization_info.slowpath_start, initialization_info.continue_addr,
            initialization_info.slowpath_rtn_addr, ic, StackInfo(scratch_size, scratch_rsp_offset),
            std::move(initialization_info.live_outs));

        assert(cf);
        // TODO: unsafe.  hard to use a unique_ptr here though.
        cf->ics.push_back(icinfo.release());
    }

    for (auto& e : new_patchpoints) {
        PatchpointInfo* pp = e.first;
        const ICSetupInfo* ic = pp->getICInfo();
        if (ic)
            delete ic;
        delete pp;
    }
    new_patchpoints.clear();
}

PatchpointInfo* PatchpointInfo::create(CompiledFunction* parent_cf, const ICSetupInfo* icinfo, int num_ic_stackmap_args,
                                       void* func_addr) {
    if (icinfo == NULL)
        assert(num_ic_stackmap_args == 0);

    auto* r = new PatchpointInfo(parent_cf, icinfo, num_ic_stackmap_args);
    r->id = new_patchpoints.size();
    new_patchpoints.push_back(std::make_pair(r, func_addr));
    return r;
}

void* PatchpointInfo::getSlowpathAddr(unsigned int pp_id) {
    RELEASE_ASSERT(pp_id < new_patchpoints.size(), "");
    return new_patchpoints[pp_id].second;
}

int numSlots(ICInfo* bjit_ic_info, int default_num_slots) {
    if (!bjit_ic_info)
        return default_num_slots;

    // this thresholds are chosen by running the benchmarks several times with different settings.
    int num_slots = std::max(bjit_ic_info->getNumSlots(), default_num_slots);
    if (bjit_ic_info->isMegamorphic())
        num_slots *= 3;
    else if (bjit_ic_info->timesRewritten() > IC_MEGAMORPHIC_THRESHOLD / 2)
        num_slots += 2;
    else
        num_slots = std::min(std::max(bjit_ic_info->timesRewritten(), 1), default_num_slots);
    return std::min(num_slots, 10);
}

ICSetupInfo* createGenericIC(TypeRecorder* type_recorder, bool has_return_value, int size) {
    return ICSetupInfo::initialize(has_return_value, 1, size, ICSetupInfo::Generic, type_recorder);
}

ICSetupInfo* createGetattrIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, numSlots(bjit_ic_info, 2), 512, ICSetupInfo::Getattr, type_recorder);
}

ICSetupInfo* createGetitemIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, numSlots(bjit_ic_info, 1), 512, ICSetupInfo::Getitem, type_recorder);
}

ICSetupInfo* createSetitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 512, ICSetupInfo::Setitem, type_recorder);
}

ICSetupInfo* createDelitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 1, 512, ICSetupInfo::Delitem, type_recorder);
}

ICSetupInfo* createSetattrIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(false, numSlots(bjit_ic_info, 2), 512, ICSetupInfo::Setattr, type_recorder);
}

ICSetupInfo* createDelattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 1, 144, ICSetupInfo::Delattr, type_recorder);
}

ICSetupInfo* createCallsiteIC(TypeRecorder* type_recorder, int num_args, ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, numSlots(bjit_ic_info, 4), 640 + 48 * num_args, ICSetupInfo::Callsite,
                                   type_recorder);
}

ICSetupInfo* createGetGlobalIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 128, ICSetupInfo::GetGlobal, type_recorder);
}

ICSetupInfo* createBinexpIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, numSlots(bjit_ic_info, 4), 512, ICSetupInfo::Binexp, type_recorder);
}

ICSetupInfo* createNonzeroIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 512, ICSetupInfo::Nonzero, type_recorder);
}

ICSetupInfo* createHasnextIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 64, ICSetupInfo::Hasnext, type_recorder);
}

ICSetupInfo* createDeoptIC() {
    return ICSetupInfo::initialize(true, 1, 0, ICSetupInfo::Deopt, NULL);
}

} // namespace pyston
