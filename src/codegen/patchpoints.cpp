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

#include "codegen/patchpoints.h"

#include <memory>
#include <unordered_map>

#include "asm_writing/icinfo.h"
#include "codegen/stackmaps.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

namespace pyston {

int PatchpointSetupInfo::totalSize() const {
    int call_size = 13;
    if (getCallingConvention() != llvm::CallingConv::C) {
        // have no idea what the precise number is:
        call_size = 128;
    }
    return num_slots * slot_size + call_size;
}
int64_t PatchpointSetupInfo::getPatchpointId() const {
    return pp_id;
}

static std::unordered_map<int64_t, PatchpointSetupInfo*> new_patchpoints_by_id;

PatchpointSetupInfo* PatchpointSetupInfo::initialize(bool has_return_value, int num_slots, int slot_size,
                                                     CompiledFunction* parent_cf, patchpoints::PatchpointType type,
                                                     TypeRecorder* type_recorder) {
    static int64_t next_id = 100;
    int64_t id = next_id++;

    PatchpointSetupInfo* rtn
        = new PatchpointSetupInfo(id, type, num_slots, slot_size, parent_cf, has_return_value, type_recorder);
    new_patchpoints_by_id[id] = rtn;
    return rtn;
}

namespace patchpoints {

void processStackmap(StackMap* stackmap) {
    int nrecords = stackmap ? stackmap->records.size() : 0;

    for (int i = 0; i < nrecords; i++) {
        StackMap::Record* r = stackmap->records[i];

        assert(stackmap->stack_size_records.size() == 1);
        const StackMap::StackSizeRecord& stack_size_record = stackmap->stack_size_records[0];
        int stack_size = stack_size_record.stack_size;

        PatchpointSetupInfo* pp = new_patchpoints_by_id[r->id];
        assert(pp);

        bool has_scratch = (pp->numScratchBytes() != 0);
        int scratch_rbp_offset = 0;
        if (has_scratch) {
            assert(r->locations.size() == 1);

            StackMap::Record::Location l = r->locations[0];

            static const int DWARF_RBP_REGNUM = 6;

            assert(l.type == 2); // "Direct"
            assert(l.regnum == DWARF_RBP_REGNUM);
            scratch_rbp_offset = l.offset;
        } else {
            assert(r->locations.size() == 0);
        }

        uint8_t* func_addr = (uint8_t*)pp->parent_cf->code;
        assert(func_addr);
        uint8_t* start_addr = func_addr + r->offset;

        std::unordered_set<int> live_outs;
        for (const auto& live_out : r->live_outs) {
            live_outs.insert(live_out.regnum);
        }

        // llvm doesn't consider callee-save registers to be live
        // if they're never allocated, but I think it makes much more
        // sense to track them as live_outs.
        // Unfortunately this means we need to be conservative about it unless
        // we can change llvm's behavior.
        live_outs.insert(3);
        live_outs.insert(12);
        live_outs.insert(13);
        live_outs.insert(14);
        live_outs.insert(15);

        registerCompiledPatchpoint(start_addr, pp,
                                   StackInfo({ stack_size, has_scratch, pp->numScratchBytes(), scratch_rbp_offset }),
                                   std::move(live_outs));
    }

    for (const std::pair<int64_t, PatchpointSetupInfo*>& p : new_patchpoints_by_id) {
        delete p.second;
    }
    new_patchpoints_by_id.clear();
}

PatchpointSetupInfo* createGenericPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder,
                                             bool has_return_value, int size) {
    return PatchpointSetupInfo::initialize(has_return_value, 1, size, parent_cf, Generic, type_recorder);
}

PatchpointSetupInfo* createGetattrPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(true, 1, 144, parent_cf, Getattr, type_recorder);
}

PatchpointSetupInfo* createGetitemPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(true, 1, 128, parent_cf, Getitem, type_recorder);
}

PatchpointSetupInfo* createSetitemPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(true, 1, 144, parent_cf, Setitem, type_recorder);
}

PatchpointSetupInfo* createDelitemPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(false, 1, 144, parent_cf, Delitem, type_recorder);
}

PatchpointSetupInfo* createSetattrPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(false, 2, 128, parent_cf, Setattr, type_recorder);
}

PatchpointSetupInfo* createCallsitePatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder, int num_args) {
    // TODO These are very large, but could probably be made much smaller with IC optimizations
    // - using rewriter2 for better code
    // - not emitting duplicate guards
    return PatchpointSetupInfo::initialize(true, 3, 480 + 48 * num_args, parent_cf, Callsite, type_recorder);
}

PatchpointSetupInfo* createGetGlobalPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(true, 1, 128, parent_cf, GetGlobal, type_recorder);
}

PatchpointSetupInfo* createBinexpPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(true, 4, 320, parent_cf, Binexp, type_recorder);
}

PatchpointSetupInfo* createNonzeroPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder) {
    return PatchpointSetupInfo::initialize(true, 2, 64, parent_cf, Nonzero, type_recorder);
}

} // namespace patchpoints

} // namespace pyston
