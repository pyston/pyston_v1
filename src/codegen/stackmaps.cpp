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

#include "codegen/stackmaps.h"

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/Object/ObjectFile.h"

#include "codegen/codegen.h"
#include "core/options.h"

//#undef VERBOSITY
//#define VERBOSITY() 2

namespace pyston {

// TODO shouldn't be recording this in a global variable
static StackMap* cur_map = NULL;
StackMap* parseStackMap() {
    StackMap* rtn = cur_map;
    cur_map = NULL;
    return rtn;
}

class StackmapJITEventListener : public llvm::JITEventListener {
private:
public:
    virtual void NotifyObjectEmitted(const llvm::ObjectImage&);
};

// LLVM will silently not register the eh frames with libgcc if these functions don't exist;
// make sure that these functions exist.
// TODO I think this breaks it for windows, which apparently loads these dynamically?
// see llvm/lib/ExecutionEngine/RTDyldMemoryManager.cpp
extern "C" void __register_frame(void*);
extern "C" void __deregister_frame(void*);
extern void _force_link() {
    __register_frame(nullptr);
    __deregister_frame(nullptr);
}

void StackmapJITEventListener::NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
    // llvm::outs() << "An object has been emitted:\n";

    llvm::error_code code;

    for (llvm::object::symbol_iterator I = Obj.begin_symbols(), E = Obj.end_symbols(); I != E;) {
        llvm::StringRef name;
        code = I->getName(name);
        assert(!code);

        if (name == "__LLVM_StackMaps") {
            uint64_t stackmap_address = 0;
            code = I->getAddress(stackmap_address);
            assert(!code);
            // code = I->getSize(stackmap_size);
            // assert(stackmap_size > 0);
            // assert(!code);
            if (VERBOSITY() >= 2)
                printf("Found the stackmaps at stackmap_address 0x%lx\n", stackmap_address);

            assert(cur_map == NULL);
            cur_map = new StackMap();

            union {
                const int8_t* i8;
                const int16_t* i16;
                const int32_t* i32;
                const int64_t* i64;
                const uint8_t* u8;
                const uint16_t* u16;
                const uint32_t* u32;
                const uint64_t* u64;
                const StackMap::Record::Location* record_loc;
                const StackMap::Record::LiveOut* record_liveout;
                const StackMap::StackSizeRecord* size_record;
            } ptr;
            const int8_t* start_ptr = ptr.i8 = (const int8_t*)stackmap_address;

            cur_map->header = *ptr.u32++; // header

#if LLVMREV < 200481
            int nfunctions = 0;
#else
            int nfunctions = *ptr.u32++;
#endif
            int nconstants = *ptr.u32++;
            int nrecords = *ptr.u32++;

            if (VERBOSITY() >= 2)
                printf("%d functions\n", nfunctions);
            for (int i = 0; i < nfunctions; i++) {
                const StackMap::StackSizeRecord& size_record = *ptr.size_record++;
                cur_map->stack_size_records.push_back(size_record);
                if (VERBOSITY() >= 2)
                    printf("function %d: offset 0x%lx, stack size 0x%lx\n", i, size_record.offset,
                           size_record.stack_size);
            }

            if (VERBOSITY() >= 2)
                printf("%d constants\n", nconstants);

            for (int i = 0; i < nconstants; i++) {
                uint64_t constant = *ptr.u64++;
                if (VERBOSITY() >= 2)
                    printf("Constant %d: %ld\n", i, constant);
                cur_map->constants.push_back(constant);
            }

            if (VERBOSITY() >= 2)
                printf("%d records\n", nrecords);

            for (int i = 0; i < nrecords; i++) {
                StackMap::Record* record = new StackMap::Record();
                cur_map->records.push_back(record);

                record->id = *ptr.u64++;
                record->offset = *ptr.u32++;
                record->flags = *ptr.u16++; // reserved (record flags)

                int numlocations = *ptr.u16++;

                if (VERBOSITY() >= 2)
                    printf("Stackmap record %ld at 0x%x has %d locations:\n", record->id, record->offset, numlocations);
                for (int j = 0; j < numlocations; j++) {
                    assert(sizeof(StackMap::Record::Location) == sizeof(*ptr.u64));
                    const StackMap::Record::Location& r = *ptr.record_loc++;
                    record->locations.push_back(r);

                    // from http://lxr.free-electrons.com/source/tools/perf/arch/x86/util/dwarf-regs.c
                    // TODO this probably can be fetched more portably from the llvm target files
                    const char* dwarf_reg_names[] = {
                        "%rax", "%rdx", "%rcx", "%rbx", "%rsi", "%rdi", "%rbp", "%rsp",
                        "%r8",  "%r9",  "%r10", "%r11", "%r12", "%r13", "%r14", "%r15",
                    };
                    if (VERBOSITY() >= 2) {
                        if (r.type == 1) {
                            printf("Location %d: type %d (reg), reg %d (%s), offset %d\n", j, r.type, r.regnum,
                                   dwarf_reg_names[r.regnum], r.offset);
                        } else {
                            printf("Location %d: type %d, reg %d, offset %d\n", j, r.type, r.regnum, r.offset);
                        }
                    }
                }

                ptr.u16++; // padding
                int num_live_outs = *ptr.u16++;
                for (int i = 0; i < num_live_outs; i++) {
                    const StackMap::Record::LiveOut& r = *ptr.record_liveout++;
                    record->live_outs.push_back(r);

                    if (VERBOSITY() >= 2) {
                        printf("Live out %d: reg #%d (?), size %d\n", i, r.regnum, r.size);
                    }
                }
                if (num_live_outs % 2 == 0)
                    ptr.u32++; // pad to 8-byte boundary
            }

#ifndef NDEBUG
            uint64_t stackmap_size;
            llvm::object::section_iterator section(Obj.end_sections());
            code = I->getSection(section);
            assert(!code);
            code = section->getSize(stackmap_size);
            assert(stackmap_size > 0);
            assert(!code);

            ASSERT(ptr.i8 - start_ptr == stackmap_size, "%ld %ld", ptr.i8 - start_ptr, stackmap_size);
#endif
        }

#if LLVMREV < 200442
        I = I.increment(code);
#else
        ++I;
#endif
    }
}

llvm::JITEventListener* makeStackMapListener() {
    return new StackmapJITEventListener();
}
}
