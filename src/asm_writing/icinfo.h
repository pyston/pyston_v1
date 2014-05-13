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

#ifndef PYSTON_ASMWRITING_ICINFO_H
#define PYSTON_ASMWRITING_ICINFO_H

#include <unordered_set>
#include <vector>

#include "llvm/IR/CallingConv.h"

#include "asm_writing/types.h"

namespace pyston {

class TypeRecorder;

class ICInfo;
class ICInvalidator;

struct ICSlotInfo {
public:
    ICSlotInfo(ICInfo* ic, int idx) : ic(ic), idx(idx) {}

    ICInfo* ic;
    int idx;

    void clear();
};

class ICSlotRewrite {
public:
    class CommitHook {
    public:
        virtual ~CommitHook() {}
        virtual void finishAssembly(int fastpath_offset) = 0;
    };

private:
    ICInfo* ic;
    assembler::Assembler* assembler;
    const char* debug_name;

    uint8_t* buf;

    std::vector<std::pair<ICInvalidator*, int64_t> > dependencies;

    ICSlotRewrite(ICInfo* ic, const char* debug_name);

public:
    ~ICSlotRewrite();

    assembler::Assembler* getAssembler() { return assembler; }
    int getSlotSize();
    int getFuncStackSize();
    int getScratchRbpOffset();
    int getScratchBytes();

    TypeRecorder* getTypeRecorder();

    assembler::GenericRegister returnRegister();

    void addDependenceOn(ICInvalidator&);
    void commit(uint64_t decision_path, CommitHook* hook);

    friend class ICInfo;
};

class ICInfo {
private:
    struct SlotInfo {
        bool is_patched;
        uint64_t decision_path;
        ICSlotInfo entry;

        SlotInfo(ICInfo* ic, int idx) : is_patched(false), decision_path(0), entry(ic, idx) {}
    };
    std::vector<SlotInfo> slots;
    // For now, just use a round-robin eviction policy.
    // This is probably a bunch worse than LRU, but it's also
    // probably a bunch better than the "always evict slot #0" policy
    // that it's replacing.
    int next_slot_to_try;

    const StackInfo stack_info;
    const int num_slots;
    const int slot_size;
    const llvm::CallingConv::ID calling_conv;
    const std::vector<int> live_outs;
    const assembler::GenericRegister return_register;
    TypeRecorder* const type_recorder;

    // for ICSlotRewrite:
    ICSlotInfo* pickEntryForRewrite(uint64_t decision_path, const char* debug_name);

    void* getSlowpathStart();

public:
    ICInfo(void* start_addr, void* continue_addr, StackInfo stack_info, int num_slots, int slot_size,
           llvm::CallingConv::ID calling_conv, const std::unordered_set<int>& live_outs,
           assembler::GenericRegister return_register, TypeRecorder* type_recorder);
    void* const start_addr, *const continue_addr;

    int getSlotSize() { return slot_size; }
    int getNumSlots() { return num_slots; }
    llvm::CallingConv::ID getCallingConvention() { return calling_conv; }
    const std::vector<int>& getLiveOuts() { return live_outs; }

    ICSlotRewrite* startRewrite(const char* debug_name);
    void clear(ICSlotInfo* entry);

    friend class ICSlotRewrite;
};

class PatchpointSetupInfo;
void registerCompiledPatchpoint(uint8_t* start_addr, PatchpointSetupInfo*, StackInfo stack_info,
                                std::unordered_set<int> live_outs);

ICInfo* getICInfo(void* rtn_addr);
}

#endif
