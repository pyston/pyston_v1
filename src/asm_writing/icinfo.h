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

#ifndef PYSTON_ASMWRITING_ICINFO_H
#define PYSTON_ASMWRITING_ICINFO_H

#include <list>
#include <memory>
#include <unordered_set>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/IR/CallingConv.h"

#include "asm_writing/assembler.h"
#include "asm_writing/types.h"
#include "core/util.h"

namespace pyston {

class TypeRecorder;

class ICInfo;
class ICInvalidator;

#define IC_INVALDITION_HEADER_SIZE 6
#define IC_MEGAMORPHIC_THRESHOLD 100

// This registers a decref info in the constructor and deregisters it in the destructor.
struct DecrefInfo {
    uint64_t ip;

    DecrefInfo() : ip(0) {}
    DecrefInfo(uint64_t ip, std::vector<Location> locations);

    // we only allow moves
    DecrefInfo(DecrefInfo&& other) : ip(0) { std::swap(other.ip, ip); }
    DecrefInfo& operator=(DecrefInfo&& other) {
        std::swap(other.ip, ip);
        return *this;
    }
    ~DecrefInfo() { reset(); }

    void reset();
};

struct ICSlotInfo {
public:
    ICSlotInfo(ICInfo* ic, uint8_t* addr, int size)
        : ic(ic), start_addr(addr), num_inside(0), size(size), used(false) {}

    ICInfo* ic;
    uint8_t* start_addr;

    std::vector<void*> gc_references;
    std::vector<DecrefInfo> decref_infos;
    llvm::TinyPtrVector<ICInvalidator*> invalidators; // ICInvalidators that reference this slotinfo

    int num_inside; // the number of stack frames that are currently inside this slot will also get increased during a
                    // rewrite
    int size;
    bool used; // if this slot is empty or got invalidated

    void clear(bool should_invalidate = true);
};

typedef BitSet<16> LiveOutSet;

class ICSlotRewrite;
class ICInfo {
private:
    std::list<ICSlotInfo> slots;
    // For now, just use a round-robin eviction policy.
    // This is probably a bunch worse than LRU, but it's also
    // probably a bunch better than the "always evict slot #0" policy
    // that it's replacing.
    // TODO: experiment with different IC eviction strategies.
    int next_slot_to_try;

    const StackInfo stack_info;
    const llvm::CallingConv::ID calling_conv;
    LiveOutSet live_outs;
    const assembler::GenericRegister return_register;
    TypeRecorder* const type_recorder;
    int retry_in, retry_backoff;
    int times_rewritten;
    assembler::RegisterSet allocatable_registers;

    DecrefInfo slowpath_decref_info;
    // This is a vector of locations which always need to get decrefed inside this IC.
    // Calls inside the ICSlots may need to decref additional locations but they will always contain at least the IC
    // global ones.
    std::vector<Location> ic_global_decref_locations;

    // associated AST node for this IC
    AST* node;

    // for ICSlotRewrite:
    ICSlotInfo* pickEntryForRewrite(const char* debug_name);

public:
    ICInfo(void* start_addr, void* slowpath_rtn_addr, void* continue_addr, StackInfo stack_info, int size,
           llvm::CallingConv::ID calling_conv, LiveOutSet live_outs, assembler::GenericRegister return_register,
           TypeRecorder* type_recorder, std::vector<Location> ic_global_decref_locations,
           assembler::RegisterSet allocatable_registers = assembler::RegisterSet::stdAllocatable());
    ~ICInfo();
    void* const start_addr, *const slowpath_rtn_addr, *const continue_addr;

    // returns a suggestion about how large this IC should be based on the number of used slots
    int calculateSuggestedSize();

    llvm::CallingConv::ID getCallingConvention() { return calling_conv; }
    const LiveOutSet& getLiveOuts() { return live_outs; }

    std::unique_ptr<ICSlotRewrite> startRewrite(const char* debug_name);
    void invalidate(ICSlotInfo* entry);
    void clearAll() {
        for (ICSlotInfo& slot_info : slots) {
            slot_info.clear();
        }
    }

    bool shouldAttempt();
    bool isMegamorphic();

    // For use of the rewriter for computing aggressiveness:
    int percentMegamorphic() const { return times_rewritten * 100 / IC_MEGAMORPHIC_THRESHOLD; }
    int percentBackedoff() const { return retry_backoff; }
    int timesRewritten() const { return times_rewritten; }

    assembler::RegisterSet getAllocatableRegs() const { return allocatable_registers; }

    friend class ICSlotRewrite;

    static ICInfo* getICInfoForNode(AST* node);
    void associateNodeWithICInfo(AST* node);

    void appendDecrefInfosTo(std::vector<DecrefInfo>& dest_decref_infos);
};

typedef std::tuple<int /*offset of jmp instr*/, int /*end of jmp instr*/, assembler::ConditionCode> NextSlotJumpInfo;

class ICSlotRewrite {
public:
    class CommitHook {
    public:
        virtual ~CommitHook() {}
        virtual bool finishAssembly(int fastpath_offset, bool& should_fill_with_nops, bool& variable_size_slots) = 0;
    };

private:
    ICSlotInfo* ic_entry;
    const char* debug_name;
    uint8_t* buf;
    assembler::Assembler assembler;
    llvm::SmallVector<std::pair<ICInvalidator*, int64_t>, 4> dependencies;

    ICSlotRewrite(ICSlotInfo* ic_entry, const char* debug_name);

public:
    static std::unique_ptr<ICSlotRewrite> create(ICInfo* ic, const char* debug_name);

    ~ICSlotRewrite();

    const char* debugName() { return debug_name; }
    assembler::Assembler* getAssembler() { return &assembler; }
    ICInfo* getICInfo() { return ic_entry->ic; }
    int getSlotSize() { return ic_entry->size; }
    uint8_t* getSlotStart() { return ic_entry->start_addr; }
    TypeRecorder* getTypeRecorder() { return getICInfo()->type_recorder; }
    assembler::GenericRegister returnRegister() { return getICInfo()->return_register; }
    int getScratchSize() { return getICInfo()->stack_info.scratch_size; }
    int getScratchRspOffset() {
        assert(getICInfo()->stack_info.scratch_size);
        return getICInfo()->stack_info.scratch_rsp_offset;
    }

    ICSlotInfo* prepareEntry() { return (ic_entry && ic_entry->num_inside == 1) ? ic_entry : NULL; }

    void addDependenceOn(ICInvalidator&);
    void commit(CommitHook* hook, std::vector<void*> gc_references,
                std::vector<std::pair<uint64_t, std::vector<Location>>> decref_infos,
                llvm::ArrayRef<NextSlotJumpInfo> next_slot_jumps);
    void abort();

    friend class ICInfo;
};

class ICSetupInfo;
struct CompiledFunction;
std::unique_ptr<ICInfo>
registerCompiledPatchpoint(uint8_t* start_addr, uint8_t* slowpath_start_addr, uint8_t* continue_addr,
                           uint8_t* slowpath_rtn_addr, const ICSetupInfo*, StackInfo stack_info, LiveOutSet live_outs,
                           std::vector<Location> ic_global_decref_locations = std::vector<Location>());

ICInfo* getICInfo(void* rtn_addr);

void clearAllICs(); // mostly for refcount debugging
}

#endif
