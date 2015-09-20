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

#include "llvm/ADT/StringSwitch.h"

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
    int call_size = CALL_ONLY_SIZE;
    if (getCallingConvention() != llvm::CallingConv::C) {
        // 14 bytes per reg that needs to be spilled
        call_size += 14 * 4;
    }
    return num_slots * slot_size + call_size;
}

ICSetupInfo* ICSetupInfo::initialize(bool has_return_value, int num_slots, int slot_size, ICType type,
                                     TypeRecorder* type_recorder) {
    ICSetupInfo* rtn = new ICSetupInfo(type, num_slots, slot_size, has_return_value, type_recorder);

    // We use size == CALL_ONLY_SIZE to imply that the call isn't patchable
    assert(rtn->totalSize() > CALL_ONLY_SIZE);

    return rtn;
}

ICSetupInfo* ICSetupInfo::initialize(llvm::StringRef str) {
    llvm::SmallVector<llvm::StringRef, 16> v;
    str.split(v, "|", -1, false);
    assert(v.size() >= 4);
    int type_as_int = 0;
    int num_slots = 0;
    int slot_size = 0;
    if (!v[0].getAsInteger(10, type_as_int) && !v[2].getAsInteger(10, num_slots) && !v[3].getAsInteger(10, slot_size) && num_slots && slot_size)
        return initialize(v[1] == "1", num_slots, slot_size, (ICType)type_as_int, NULL);
    return NULL;
}

std::string ICSetupInfo::toString() const {
    return (llvm::Twine((int)type) + "|" + llvm::Twine(hasReturnValue()) + "|" + llvm::Twine(num_slots) + "|" + llvm::Twine(slot_size)).str();
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

    StackMap::Record::Location frame_info_location = r->locations[cur_arg];
    cur_arg++;
    // We could allow the frame_info to exist in a different location for each callsite,
    // but in reality it will always live at a fixed stack offset.
    if (map->frameInfoFound()) {
        assert(frame_info_location == map->frame_info_location);
    } else {
        map->frame_info_location = frame_info_location;
    }

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
    assert(cur_arg - frameStackmapArgsStart() == numFrameStackmapArgs() -1);
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


        auto& f = r->locations[r->locations.size()-1];
        assert(f.type == StackMap::Record::Location::ConstIndex);
        char* c = (char*)stackmap->constants[f.offset];
        PatchpointInfo* pp = PatchpointInfo::create(cf, c);
        assert(pp);

        if (VERBOSITY() >= 2) {
            printf("Processing pp %ld; [%d, %d)\n", reinterpret_cast<int64_t>(pp), r->offset,
                   r->offset + pp->patchpointSize());
        }

        if (!(r->locations.size() == pp->totalStackmapArgs())){
            printf("%zu %d\n", r->locations.size(), pp->totalStackmapArgs());
        }
        assert(r->locations.size() == pp->totalStackmapArgs());

        int scratch_rbp_offset = extractScratchOffset(pp, r);
        int scratch_size = pp->scratchSize();
        assert(scratch_size % sizeof(void*) == 0);
        assert(scratch_rbp_offset % sizeof(void*) == 0);

        uint8_t* start_addr = (uint8_t*)pp->parentFunction()->code + r->offset;
        uint8_t* end_addr = start_addr + pp->patchpointSize();

#if LLVMREV < 235483
        void* slowpath_func = PatchpointInfo::getSlowpathAddr(r->id);
        if (ENABLE_JIT_OBJECT_CACHE)
            setSlowpathFunc(start_addr, slowpath_func);
#else
        void* slowpath_func = getSlowpathFunc(start_addr);
#endif

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
        ASSERT(nspills <= MAX_FRAME_SPILLS, "did %d spills but expected only %d!", nspills, MAX_FRAME_SPILLS);

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
}

PatchpointInfo* PatchpointInfo::create(CompiledFunction* parent_cf, const ICSetupInfo* icinfo, int num_ic_stackmap_args) {
    if (icinfo == NULL)
        assert(num_ic_stackmap_args == 0);
    return new PatchpointInfo(parent_cf, icinfo, num_ic_stackmap_args);
}

CompilerType* getTypeFromString(llvm::StringRef type_str, int& num_parsed) {
    CompilerType* type = llvm::StringSwitch<CompilerType*>(type_str)
            .Case("i64", INT)
            .Case("AnyBox", UNKNOWN)
            .Case("bool", BOOL)
            .Case("double", FLOAT)
            .Case("generator", GENERATOR)
            .Case("NormalType(dict)", DICT)
            .Case("NormalType(list)", LIST)
            .Case("NormalType(str)", STR)
            .Case("NormalType(tuple)", BOXED_TUPLE)
            .Case("NormalType(function)", typeFromClass(function_cls))
            .Case("closure", CLOSURE)
            .Case("FrameInfo", FRAME_INFO)
            .Case("undefType", UNDEF)
            .Default(nullptr);

    if (!type) {
        if (type_str.startswith("tuple(")) {
            llvm::StringRef vars_str = type_str.substr(strlen("tuple("));
            llvm::SmallVector<llvm::StringRef, 8> vars;
            vars_str.rtrim(")").split(vars, ",", -1, false);
            std::vector<CompilerType*> types;
            for (llvm::StringRef tuple_var : vars) {
                RELEASE_ASSERT(!tuple_var.trim().startswith("tuple"), "parser cant't currently handled nested tuples");
                types.push_back(getTypeFromString(tuple_var.trim(), num_parsed));
            }
            type = makeTupleType(types);
            --num_parsed; // "tuple(" doen't count
        } else if (type == 0 && type_str.startswith("NormalType("))
            type = UNKNOWN;

        RELEASE_ASSERT(type, "unknown type %s", type_str.str().c_str());
    }
    ++num_parsed;
    return type;
}

std::string PatchpointInfo::toString() {
    std::string str;
    llvm::raw_string_ostream stream(str);
    if (getICInfo())
        stream << getICInfo()->toString();
    stream << "^";
    for (auto&& v : getFrameVars()) {
        stream << v.name;
        stream << ":";
        stream << v.type->debugName();
        stream << "|";
    }
    return stream.str();
}

PatchpointInfo* PatchpointInfo::create(CompiledFunction* parent_cf, llvm::StringRef frame_str) {
    llvm::SmallVector<llvm::StringRef, 2> v;
    frame_str.split(v, "^", -1, true);
    assert(v.size() == 2);
    llvm::StringRef icinfo_str = v[0];
    llvm::StringRef frame_vars_str = v[1];

    ICSetupInfo* icinfo = icinfo_str.empty() ? NULL : ICSetupInfo::initialize(icinfo_str);
    llvm::SmallVector<llvm::StringRef, 16> frame_vars;
    frame_vars_str.split(frame_vars, "|", -1, false);
    int num_frame_args = 0;
    auto* pp = new PatchpointInfo(parent_cf, icinfo, 0);
    for (llvm::StringRef name_type_str : frame_vars) {
        llvm::SmallVector<llvm::StringRef, 2> name_type;
        name_type_str.split(name_type, ":", -1, false);
        assert(name_type.size() == 2);
        int num_parsed = 0;
        CompilerType* type = getTypeFromString(name_type[1], num_parsed);
        num_frame_args += num_parsed;
        pp->addFrameVar(name_type[0], type);
    }
    pp->setNumFrameArgs(num_frame_args + icStackmapArgsStart() + 1);
    return pp;
}

ICSetupInfo* createGenericIC(TypeRecorder* type_recorder, bool has_return_value, int size) {
    return ICSetupInfo::initialize(has_return_value, 1, size, ICSetupInfo::Generic, type_recorder);
}

ICSetupInfo* createGetattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 512, ICSetupInfo::Getattr, type_recorder);
}

ICSetupInfo* createGetitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 512, ICSetupInfo::Getitem, type_recorder);
}

ICSetupInfo* createSetitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 512, ICSetupInfo::Setitem, type_recorder);
}

ICSetupInfo* createDelitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 1, 512, ICSetupInfo::Delitem, type_recorder);
}

ICSetupInfo* createSetattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 2, 512, ICSetupInfo::Setattr, type_recorder);
}

ICSetupInfo* createDelattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 1, 144, ICSetupInfo::Delattr, type_recorder);
}

ICSetupInfo* createCallsiteIC(TypeRecorder* type_recorder, int num_args) {
    // TODO These are very large, but could probably be made much smaller with IC optimizations
    // - using rewriter2 for better code
    // - not emitting duplicate guards
    return ICSetupInfo::initialize(true, 4, 640 + 48 * num_args, ICSetupInfo::Callsite, type_recorder);
}

ICSetupInfo* createGetGlobalIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 128, ICSetupInfo::GetGlobal, type_recorder);
}

ICSetupInfo* createBinexpIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 4, 512, ICSetupInfo::Binexp, type_recorder);
}

ICSetupInfo* createNonzeroIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 512, ICSetupInfo::Nonzero, type_recorder);
}

ICSetupInfo* createHasnextIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 64, ICSetupInfo::Hasnext, type_recorder);
}

} // namespace pyston
