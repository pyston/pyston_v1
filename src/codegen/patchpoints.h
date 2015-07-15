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

#ifndef PYSTON_CODEGEN_PATCHPOINTS_H
#define PYSTON_CODEGEN_PATCHPOINTS_H

#include <stddef.h>
#include <stdint.h>

#include "llvm/IR/CallingConv.h"

#include "codegen/stackmaps.h"
#include "core/common.h"

namespace pyston {

struct CompiledFunction;
class CompilerType;
struct StackMap;
class TypeRecorder;
class ICSetupInfo;

static const int MAX_FRAME_SPILLS = 9; // TODO this shouldn't have to be larger than the set of non-callee-save args (9)
                                       // except that will we currently spill the same reg multiple times
static const int CALL_ONLY_SIZE
    = 13 + (MAX_FRAME_SPILLS * 9)
      + 1; // 13 for the call, 9 bytes per spill (7 for GP, 9 for XMM), + 1 if we want to nop/trap

void processStackmap(CompiledFunction* cf, StackMap* stackmap);

struct PatchpointInfo {
public:
    struct FrameVarInfo {
        std::string name;
        CompilerType* type;
    };

private:
    CompiledFunction* const parent_cf;
    const ICSetupInfo* icinfo;
    int num_ic_stackmap_args;
    int num_frame_stackmap_args;

    std::vector<FrameVarInfo> frame_vars;
    unsigned int id;

    PatchpointInfo(CompiledFunction* parent_cf, const ICSetupInfo* icinfo, int num_ic_stackmap_args)
        : parent_cf(parent_cf),
          icinfo(icinfo),
          num_ic_stackmap_args(num_ic_stackmap_args),
          num_frame_stackmap_args(-1),
          id(0) {}


public:
    const ICSetupInfo* getICInfo() { return icinfo; }

    int patchpointSize();
    CompiledFunction* parentFunction() { return parent_cf; }

    const std::vector<FrameVarInfo>& getFrameVars() { return frame_vars; }

    int scratchStackmapArg() { return 0; }
    int scratchSize() { return 80 + MAX_FRAME_SPILLS * sizeof(void*); }

    void addFrameVar(const std::string& name, CompilerType* type);
    void setNumFrameArgs(int num_frame_args) {
        assert(num_frame_stackmap_args == -1);
        num_frame_stackmap_args = num_frame_args;
    }

    int icStackmapArgsStart() { return 1; }
    int numICStackmapArgs() { return num_ic_stackmap_args; }

    int frameStackmapArgsStart() { return icStackmapArgsStart() + numICStackmapArgs(); }
    int numFrameStackmapArgs() {
        assert(num_frame_stackmap_args >= 0);
        return num_frame_stackmap_args;
    }

    unsigned int getId() const { return id; }

    void parseLocationMap(StackMap::Record* r, LocationMap* map);

    int totalStackmapArgs() { return frameStackmapArgsStart() + numFrameStackmapArgs(); }

    static PatchpointInfo* create(CompiledFunction* parent_cf, const ICSetupInfo* icinfo, int num_ic_stackmap_args,
                                  void* func_addr);
    static void* getSlowpathAddr(unsigned int pp_id);
};

class ICSetupInfo {
public:
    enum ICType {
        Generic,
        Callsite,
        GetGlobal,
        Getattr,
        Setattr,
        Delattr,
        Getitem,
        Setitem,
        Delitem,
        Binexp,
        Nonzero,
        Hasnext,
    };

private:
    ICSetupInfo(ICType type, int num_slots, int slot_size, bool has_return_value, TypeRecorder* type_recorder)
        : type(type),
          num_slots(num_slots),
          slot_size(slot_size),
          has_return_value(has_return_value),
          type_recorder(type_recorder) {}

public:
    const ICType type;

    const int num_slots, slot_size;
    const bool has_return_value;
    TypeRecorder* const type_recorder;

    int totalSize() const;
    bool hasReturnValue() const { return has_return_value; }

    llvm::CallingConv::ID getCallingConvention() const {
// FIXME: we currently have some issues with using PreserveAll (the rewriter currently
// does not completely preserve live outs), so disable it temporarily.
#if 0
        // The plan is to switch probably everything over to PreseveAll (and potentially AnyReg),
        // but for only switch Getattr so the testing can be localized:
        if (type == Getattr || type == Setattr)
            return llvm::CallingConv::PreserveAll;
#endif

        return llvm::CallingConv::C;
    }

    static ICSetupInfo* initialize(bool has_return_value, int num_slots, int slot_size, ICType type,
                                   TypeRecorder* type_recorder);
};

ICSetupInfo* createGenericIC(TypeRecorder* type_recorder, bool has_return_value, int size);
ICSetupInfo* createCallsiteIC(TypeRecorder* type_recorder, int num_args);
ICSetupInfo* createGetGlobalIC(TypeRecorder* type_recorder);
ICSetupInfo* createGetattrIC(TypeRecorder* type_recorder);
ICSetupInfo* createSetattrIC(TypeRecorder* type_recorder);
ICSetupInfo* createDelattrIC(TypeRecorder* type_recorder);
ICSetupInfo* createGetitemIC(TypeRecorder* type_recorder);
ICSetupInfo* createSetitemIC(TypeRecorder* type_recorder);
ICSetupInfo* createDelitemIC(TypeRecorder* type_recorder);
ICSetupInfo* createBinexpIC(TypeRecorder* type_recorder);
ICSetupInfo* createNonzeroIC(TypeRecorder* type_recorder);
ICSetupInfo* createHasnextIC(TypeRecorder* type_recorder);

} // namespace pyston

#endif
