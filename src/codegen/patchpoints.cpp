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
#include "runtime/types.h"

namespace pyston {

int ICSetupInfo::totalSize() const {
    if (isDeopt())
        return DEOPT_CALL_ONLY_SIZE;

    int call_size = CALL_ONLY_SIZE;
    if (getCallingConvention() != llvm::CallingConv::C) {
        // 14 bytes per reg that needs to be spilled
        call_size += 14 * 4;
    }
    return size + call_size;
}

static std::vector<std::pair<std::unique_ptr<PatchpointInfo>, void* /* addr of func to call */>> new_patchpoints;

std::unique_ptr<ICSetupInfo> ICSetupInfo::initialize(bool has_return_value, int size, ICType type,
                                                     assembler::RegisterSet allocatable_regs) {
    auto rtn = std::unique_ptr<ICSetupInfo>(new ICSetupInfo(type, size, has_return_value, allocatable_regs));


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
    if (!this->isDeopt())
        return;

    assert(r->locations.size() == totalStackmapArgs());

    int cur_arg = frameStackmapArgsStart();

    // printf("parsing pp %ld:\n", reinterpret_cast<int64_t>(this));

    auto parse_type = [&](CompilerType* type) {
        int num_args = type->numFrameArgs();

        llvm::SmallVector<StackMap::Record::Location, 1> locations;
        locations.append(r->locations.data() + cur_arg, r->locations.data() + cur_arg + num_args);
        cur_arg += num_args;

        return LocationMap::LocationTable::LocationEntry({._debug_pp_id = (uint64_t) this,
                                                          .offset = r->offset,
                                                          .length = patchpointSize(),
                                                          .type = type,
                                                          .locations = std::move(locations) });
    };

    auto&& source = parentFunction()->code_obj->source;
    if (source->is_generator)
        map->generator.locations.push_back(parse_type(GENERATOR));
    if (source->scoping.takesClosure())
        map->passed_closure.locations.push_back(parse_type(CLOSURE));
    if (source->scoping.createsClosure())
        map->created_closure.locations.push_back(parse_type(CLOSURE));

    for (FrameVarInfo& frame_var : frame_info_desc.vars) {
        map->vars[frame_var.vreg].locations.push_back(parse_type(frame_var.type));
    }
    for (int vreg : frame_info_desc.potentially_undefined) {
        map->definedness_vars[vreg].locations.push_back(parse_type(BOOL));
    }

    ASSERT(cur_arg - frameStackmapArgsStart() == numFrameStackmapArgs(), "%d %d %d", cur_arg, frameStackmapArgsStart(),
           numFrameStackmapArgs());
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

#if !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS)

static char decref_code[] = "\x48\xff\x0f"     // decq (%rdi)
                            "\x75\x07"         // jne +7
                            "\x48\x8b\x47\x08" // mov 0x8(%rdi),%rax
                            "\xff\x50\x30"     // callq *0x30(%rax)
    ;

static char xdecref_code[] = "\x48\x85\xff"     // test %rdi,%rdi
                             "\x74\x0c"         // je +12
                             "\x48\xff\x0f"     // decq (%rdi)
                             "\x75\x07"         // jne +7
                             "\x48\x8b\x47\x08" // mov 0x8(%rdi),%rax
                             "\xff\x50\x30"     // callq *0x30(%rax)
    ;

#else

static void _decref(Box* b) {
    Py_DECREF(b);
}
static char decref_code[] = "\x48\xb8\x00\x00\x00\x00\x00\x00\x00\x00" // movabs $0x00, %rax
                            "\xff\xd0"                                 // callq *%rax
    ;

static void _xdecref(Box* b) {
    Py_XDECREF(b);
}
static char xdecref_code[] = "\x48\xb8\x00\x00\x00\x00\x00\x00\x00\x00" // movabs $0x00, %rax
                             "\xff\xd0"                                 // callq *%rax
    ;

namespace {
class _Initializer {
public:
    _Initializer() {
        void* p = (void*)&_decref;
        memcpy(decref_code + 2, &p, sizeof(p));

        p = (void*)&_xdecref;
        memcpy(xdecref_code + 2, &p, sizeof(p));
    }
} _i;
}
#endif

const int DECREF_PP_SIZE = sizeof(decref_code) - 1;   // -1 for the NUL byte
const int XDECREF_PP_SIZE = sizeof(xdecref_code) - 1; // -1 for the NUL byte

void emitDecref(void* addr) {
    memcpy(addr, decref_code, DECREF_PP_SIZE);
}

void emitXDecref(void* addr) {
    memcpy(addr, xdecref_code, XDECREF_PP_SIZE);
}

void processStackmap(CompiledFunction* cf, StackMap* stackmap) {
    int nrecords = stackmap ? stackmap->records.size() : 0;

    assert(cf->location_map == NULL);
    cf->location_map = llvm::make_unique<LocationMap>();
    if (stackmap)
        cf->location_map->constants = stackmap->constants;

    for (int i = 0; i < nrecords; i++) {
        StackMap::Record* r = &stackmap->records[i];

        assert(stackmap->stack_size_records.size() == 1);
        const StackMap::StackSizeRecord& stack_size_record = stackmap->stack_size_records[0];
        int stack_size = stack_size_record.stack_size;

        if (r->id == DECREF_PP_ID) {
            emitDecref((uint8_t*)cf->code + r->offset);
            continue;
        }

        if (r->id == XDECREF_PP_ID) {
            emitXDecref((uint8_t*)cf->code + r->offset);
            continue;
        }

        RELEASE_ASSERT(new_patchpoints.size() > r->id, "");
        std::unique_ptr<PatchpointInfo>& pp = new_patchpoints[r->id].first;
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
            printf("Processing pp %p; [%d, %d)\n", pp.get(), r->offset, r->offset + pp->patchpointSize());
        }

        assert(r->locations.size() == pp->totalStackmapArgs());

        int scratch_rbp_offset = extractScratchOffset(pp.get(), r);
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

        pp->parseLocationMap(r, cf->location_map.get());

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

        ASSERT(initialization_info.slowpath_start - start_addr >= ic->size,
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
        cf->ics.push_back(std::move(icinfo));
    }

    new_patchpoints.clear();
}

PatchpointInfo* PatchpointInfo::create(CompiledFunction* parent_cf, std::unique_ptr<const ICSetupInfo> icinfo,
                                       int num_ic_stackmap_args, void* func_addr) {
    if (icinfo == NULL)
        assert(num_ic_stackmap_args == 0);

    auto pp_info
        = std::unique_ptr<PatchpointInfo>(new PatchpointInfo(parent_cf, std::move(icinfo), num_ic_stackmap_args));
    pp_info->id = new_patchpoints.size();
    auto* r = pp_info.get();
    new_patchpoints.emplace_back(std::move(pp_info), func_addr);

    assert(r->id != DECREF_PP_ID);
    assert(r->id != XDECREF_PP_ID);
    return r;
}

void* PatchpointInfo::getSlowpathAddr(unsigned int pp_id) {
    RELEASE_ASSERT(pp_id < new_patchpoints.size(), "");
    return new_patchpoints[pp_id].second;
}

int slotSize(ICInfo* bjit_ic_info, int default_size) {
    if (!bjit_ic_info)
        return default_size;

    int suggested_size = bjit_ic_info->calculateSuggestedSize();
    if (suggested_size <= 0)
        return default_size;

    // round up to make it more likely that we will find a entry in the object cache
    if (suggested_size & 31)
        suggested_size += (suggested_size + 32) & 31;
    return suggested_size;
}

std::unique_ptr<ICSetupInfo> createGenericIC(bool has_return_value, int size) {
    return ICSetupInfo::initialize(has_return_value, size, ICSetupInfo::Generic);
}

std::unique_ptr<ICSetupInfo> createGetattrIC(ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, slotSize(bjit_ic_info, 1024), ICSetupInfo::Getattr);
}

std::unique_ptr<ICSetupInfo> createGetitemIC(ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, slotSize(bjit_ic_info, 512), ICSetupInfo::Getitem);
}

std::unique_ptr<ICSetupInfo> createSetitemIC() {
    return ICSetupInfo::initialize(true, 512, ICSetupInfo::Setitem);
}

std::unique_ptr<ICSetupInfo> createDelitemIC() {
    return ICSetupInfo::initialize(false, 512, ICSetupInfo::Delitem);
}

std::unique_ptr<ICSetupInfo> createSetattrIC(ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(false, slotSize(bjit_ic_info, 1024), ICSetupInfo::Setattr);
}

std::unique_ptr<ICSetupInfo> createDelattrIC() {
    return ICSetupInfo::initialize(false, 144, ICSetupInfo::Delattr);
}

std::unique_ptr<ICSetupInfo> createCallsiteIC(int num_args, ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, slotSize(bjit_ic_info, 4 * (640 + 48 * num_args)), ICSetupInfo::Callsite);
}

std::unique_ptr<ICSetupInfo> createGetGlobalIC() {
    return ICSetupInfo::initialize(true, 128, ICSetupInfo::GetGlobal);
}

std::unique_ptr<ICSetupInfo> createBinexpIC(ICInfo* bjit_ic_info) {
    return ICSetupInfo::initialize(true, slotSize(bjit_ic_info, 2048), ICSetupInfo::Binexp);
}

std::unique_ptr<ICSetupInfo> createNonzeroIC() {
    return ICSetupInfo::initialize(true, 1024, ICSetupInfo::Nonzero);
}

std::unique_ptr<ICSetupInfo> createHasnextIC() {
    return ICSetupInfo::initialize(true, 128, ICSetupInfo::Hasnext);
}

std::unique_ptr<ICSetupInfo> createDeoptIC() {
    return ICSetupInfo::initialize(true, 0, ICSetupInfo::Deopt);
}

} // namespace pyston
