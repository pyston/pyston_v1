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

#ifndef PYSTON_CODEGEN_PATCHPOINTS_H
#define PYSTON_CODEGEN_PATCHPOINTS_H

#include <stddef.h>
#include <stdint.h>

#include "llvm/IR/CallingConv.h"

namespace pyston {

class TypeRecorder;

namespace patchpoints {

enum PatchpointType {
    Generic,
    Callsite,
    GetGlobal,
    Getattr,
    Setattr,
    Getitem,
    Setitem,
    Delitem,
    Binexp,
    Nonzero,
};
}

class CompiledFunction;

class PatchpointSetupInfo {
private:
    PatchpointSetupInfo(int64_t pp_id, patchpoints::PatchpointType type, int num_slots, int slot_size,
                        CompiledFunction* parent_cf, bool has_return_value, TypeRecorder* type_recorder)
        : pp_id(pp_id), type(type), num_slots(num_slots), slot_size(slot_size), has_return_value(has_return_value),
          parent_cf(parent_cf), type_recorder(type_recorder) {}

    const int64_t pp_id;

public:
    const patchpoints::PatchpointType type;

    const int num_slots, slot_size;
    const bool has_return_value;
    CompiledFunction* const parent_cf;
    TypeRecorder* const type_recorder;

    int totalSize() const;
    int64_t getPatchpointId() const;
    bool hasReturnValue() const { return has_return_value; }

    int numScratchBytes() const { return 64; }

    llvm::CallingConv::ID getCallingConvention() const {
        // The plan is to switch probably everything over to PreseveAll (and potentially AnyReg),
        // but for only switch Getattr so the testing can be localized:
        if (type == patchpoints::Getattr || type == patchpoints::Setattr)
            return llvm::CallingConv::PreserveAll;

        return llvm::CallingConv::C;
    }

    static PatchpointSetupInfo* initialize(bool has_return_value, int num_slots, int slot_size,
                                           CompiledFunction* parent_cf, patchpoints::PatchpointType type,
                                           TypeRecorder* type_recorder);
};

struct StackMap;

namespace patchpoints {

void processStackmap(StackMap* stackmap);

PatchpointSetupInfo* createGenericPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder,
                                             bool has_return_value, int size);
PatchpointSetupInfo* createCallsitePatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder, int num_args);
PatchpointSetupInfo* createGetGlobalPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createGetattrPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createSetattrPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createGetitemPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createSetitemPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createDelitemPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createBinexpPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
PatchpointSetupInfo* createNonzeroPatchpoint(CompiledFunction* parent_cf, TypeRecorder* type_recorder);
}

} // namespace pyston

#endif
