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

static const int DECREF_PP_ID = 1000000;
extern const int DECREF_PP_SIZE;
static const int XDECREF_PP_ID = 1000001;
extern const int XDECREF_PP_SIZE;

static const int MAX_FRAME_SPILLS = 9; // TODO this shouldn't have to be larger than the set of non-callee-save args (9)
                                       // except that will we currently spill the same reg multiple times
static const int CALL_ONLY_SIZE = 13 + 1; // 13 for the call, + 1 if we want to nop/trap

static const int DEOPT_CALL_ONLY_SIZE
    = 13 + (MAX_FRAME_SPILLS * 9)
      + 1; // 13 for the call, 9 bytes per spill (7 for GP, 9 for XMM), + 1 if we want to nop/trap

void processStackmap(CompiledFunction* cf, StackMap* stackmap);

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
        Deopt,
    };

private:
    ICSetupInfo(ICType type, int size, bool has_return_value, TypeRecorder* type_recorder)
        : type(type), size(size), has_return_value(has_return_value), type_recorder(type_recorder) {}

public:
    const ICType type;
    const int size;
    const bool has_return_value;
    TypeRecorder* const type_recorder;

    int totalSize() const;
    bool hasReturnValue() const { return has_return_value; }
    bool isDeopt() const { return type == Deopt; }

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

    static ICSetupInfo* initialize(bool has_return_value, int size, ICType type, TypeRecorder* type_recorder);
};

struct PatchpointInfo {
public:
    struct FrameVarInfo {
        int vreg;
        CompilerType* type;
    };
    struct FrameInfoDesc {
        std::vector<FrameVarInfo> vars;
        llvm::SmallVector<int, 2> potentially_undefined;
    };

private:
    CompiledFunction* const parent_cf;
    const ICSetupInfo* icinfo;
    int num_ic_stackmap_args;
    int num_frame_stackmap_args;
    bool is_frame_info_stackmap;
    unsigned int id;

    FrameInfoDesc frame_info_desc;

    PatchpointInfo(CompiledFunction* parent_cf, const ICSetupInfo* icinfo, int num_ic_stackmap_args)
        : parent_cf(parent_cf),
          icinfo(icinfo),
          num_ic_stackmap_args(num_ic_stackmap_args),
          num_frame_stackmap_args(-1),
          is_frame_info_stackmap(false),
          id(0) {}


public:
    const ICSetupInfo* getICInfo() { return icinfo; }

    int patchpointSize();
    CompiledFunction* parentFunction() { return parent_cf; }

    FrameInfoDesc& getFrameDesc() { return frame_info_desc; }

    int scratchStackmapArg() { return 0; }
    int scratchSize() { return isDeopt() ? MAX_FRAME_SPILLS * sizeof(void*) : 96; }
    bool isDeopt() const { return icinfo ? icinfo->isDeopt() : false; }
    bool isFrameInfoStackmap() const { return is_frame_info_stackmap; }
    int numFrameSpillsSupported() const { return isDeopt() ? MAX_FRAME_SPILLS : 0; }

    void addFrameVar(int vreg, CompilerType* type) {
        frame_info_desc.vars.push_back(FrameVarInfo({.vreg = vreg, .type = type }));
    }
    void addPotentiallyUndefined(int vreg) { frame_info_desc.potentially_undefined.push_back(vreg); }

    void setNumFrameArgs(int num_frame_args) {
        assert(num_frame_stackmap_args == -1);
        num_frame_stackmap_args = num_frame_args;
    }
    void setIsFrameInfoStackmap(bool b = true) { is_frame_info_stackmap = b; }

    int icStackmapArgsStart() { return isFrameInfoStackmap() ? 0 : 1; }
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

class ICInfo;
ICSetupInfo* createGenericIC(TypeRecorder* type_recorder, bool has_return_value, int size);
ICSetupInfo* createCallsiteIC(TypeRecorder* type_recorder, int num_args, ICInfo* bjit_ic_info);
ICSetupInfo* createGetGlobalIC(TypeRecorder* type_recorder);
ICSetupInfo* createGetattrIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info);
ICSetupInfo* createSetattrIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info);
ICSetupInfo* createDelattrIC(TypeRecorder* type_recorder);
ICSetupInfo* createGetitemIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info);
ICSetupInfo* createSetitemIC(TypeRecorder* type_recorder);
ICSetupInfo* createDelitemIC(TypeRecorder* type_recorder);
ICSetupInfo* createBinexpIC(TypeRecorder* type_recorder, ICInfo* bjit_ic_info);
ICSetupInfo* createNonzeroIC(TypeRecorder* type_recorder);
ICSetupInfo* createHasnextIC(TypeRecorder* type_recorder);
ICSetupInfo* createDeoptIC();

} // namespace pyston

#endif
