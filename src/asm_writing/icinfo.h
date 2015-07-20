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

#ifndef PYSTON_ASMWRITING_ICINFO_H
#define PYSTON_ASMWRITING_ICINFO_H

#include <memory>
#include <unordered_set>
#include <vector>

#include "llvm/IR/CallingConv.h"

#include "asm_writing/assembler.h"
#include "asm_writing/types.h"

namespace pyston {

class TypeRecorder;

class ICInfo;
class ICInvalidator;

#define IC_INVALDITION_HEADER_SIZE 6

struct ICSlotInfo {
public:
    ICSlotInfo(ICInfo* ic, int idx) : ic(ic), idx(idx), num_inside(0) {}

    ICInfo* ic;
    int idx;        // the index inside the ic
    int num_inside; // the number of stack frames that are currently inside this slot

    void clear();
};

class ICSlotRewrite {
public:
    class CommitHook {
    public:
        virtual ~CommitHook() {}
        virtual bool finishAssembly(int fastpath_offset) = 0;
    };

private:
    ICInfo* ic;
    const char* debug_name;

    uint8_t* buf;

    assembler::Assembler assembler;

    llvm::SmallVector<std::pair<ICInvalidator*, int64_t>, 4> dependencies;

    ICSlotInfo* ic_entry;

public:
    ICSlotRewrite(ICInfo* ic, const char* debug_name);
    ~ICSlotRewrite();

    assembler::Assembler* getAssembler() { return &assembler; }
    int getSlotSize();
    int getScratchRspOffset();
    int getScratchSize();
    uint8_t* getSlotStart();

    TypeRecorder* getTypeRecorder();

    assembler::GenericRegister returnRegister();

    ICSlotInfo* prepareEntry();

    void addDependenceOn(ICInvalidator&);
    void commit(CommitHook* hook);
    void abort();

    const ICInfo* getICInfo() { return ic; }

    const char* debugName() { return debug_name; }

    friend class ICInfo;
};

class ICInfo {
private:
    std::vector<ICSlotInfo> slots;
    // For now, just use a round-robin eviction policy.
    // This is probably a bunch worse than LRU, but it's also
    // probably a bunch better than the "always evict slot #0" policy
    // that it's replacing.
    // TODO: experiment with different IC eviction strategies.
    int next_slot_to_try;

    const StackInfo stack_info;
    const int num_slots;
    const int slot_size;
    const llvm::CallingConv::ID calling_conv;
    const std::vector<int> live_outs;
    const assembler::GenericRegister return_register;
    TypeRecorder* const type_recorder;
    int retry_in, retry_backoff;
    int times_rewritten;

    // for ICSlotRewrite:
    ICSlotInfo* pickEntryForRewrite(const char* debug_name);

public:
    ICInfo(void* start_addr, void* slowpath_rtn_addr, void* continue_addr, StackInfo stack_info, int num_slots,
           int slot_size, llvm::CallingConv::ID calling_conv, const std::unordered_set<int>& live_outs,
           assembler::GenericRegister return_register, TypeRecorder* type_recorder);
    void* const start_addr, *const slowpath_rtn_addr, *const continue_addr;

    int getSlotSize() { return slot_size; }
    int getNumSlots() { return num_slots; }
    llvm::CallingConv::ID getCallingConvention() { return calling_conv; }
    const std::vector<int>& getLiveOuts() { return live_outs; }

    std::unique_ptr<ICSlotRewrite> startRewrite(const char* debug_name);
    void clear(ICSlotInfo* entry);

    bool shouldAttempt();
    bool isMegamorphic();

    friend class ICSlotRewrite;
};

class ICSetupInfo;
struct CompiledFunction;
std::unique_ptr<ICInfo> registerCompiledPatchpoint(uint8_t* start_addr, uint8_t* slowpath_start_addr,
                                                   uint8_t* continue_addr, uint8_t* slowpath_rtn_addr,
                                                   const ICSetupInfo*, StackInfo stack_info,
                                                   std::unordered_set<int> live_outs);
void deregisterCompiledPatchpoint(ICInfo* ic);

ICInfo* getICInfo(void* rtn_addr);
}

#endif
