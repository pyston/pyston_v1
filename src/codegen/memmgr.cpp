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

#include "codegen/memmgr.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Memory.h"

#include "core/common.h"
#include "core/util.h"

// This code was copy-pasted from SectionMemoryManager.cpp;
// TODO eventually I should remove this using directive
// and recode in my style
using namespace llvm;

namespace pyston {

class PystonMemoryManager : public RTDyldMemoryManager {
public:
    PystonMemoryManager() {}
    virtual ~PystonMemoryManager();

    virtual uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, StringRef SectionName);

    virtual uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, StringRef SectionName,
                                         bool isReadOnly);

    virtual bool finalizeMemory(std::string* ErrMsg = 0);

    virtual void invalidateInstructionCache();

private:
    struct MemoryGroup {
        SmallVector<sys::MemoryBlock, 16> AllocatedMem;
        SmallVector<sys::MemoryBlock, 16> FreeMem;
        sys::MemoryBlock Near;
    };

    uint8_t* allocateSection(MemoryGroup& MemGroup, uintptr_t Size, unsigned Alignment);

    error_code applyMemoryGroupPermissions(MemoryGroup& MemGroup, unsigned Permissions);

    virtual uint64_t getSymbolAddress(const std::string& Name);

    MemoryGroup CodeMem;
    MemoryGroup RWDataMem;
    MemoryGroup RODataMem;
};

uint8_t* PystonMemoryManager::allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                                                  StringRef SectionName, bool IsReadOnly) {
    // printf("allocating data section: %ld %d %d %s %d\n", Size, Alignment, SectionID, SectionName.data(), IsReadOnly);
    // assert(SectionName != ".llvm_stackmaps");
    if (IsReadOnly)
        return allocateSection(RODataMem, Size, Alignment);
    return allocateSection(RWDataMem, Size, Alignment);
}

uint8_t* PystonMemoryManager::allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                                                  StringRef SectionName) {
    // printf("allocating code section: %ld %d %d %s\n", Size, Alignment, SectionID, SectionName.data());
    return allocateSection(CodeMem, Size, Alignment);
}

uint8_t* PystonMemoryManager::allocateSection(MemoryGroup& MemGroup, uintptr_t Size, unsigned Alignment) {
    if (!Alignment)
        Alignment = 16;

    assert(!(Alignment & (Alignment - 1)) && "Alignment must be a power of two.");

    uintptr_t RequiredSize = Alignment * ((Size + Alignment - 1) / Alignment + 1);
    uintptr_t Addr = 0;

    // Look in the list of free memory regions and use a block there if one
    // is available.
    for (int i = 0, e = MemGroup.FreeMem.size(); i != e; ++i) {
        sys::MemoryBlock& MB = MemGroup.FreeMem[i];
        if (MB.size() >= RequiredSize) {
            Addr = (uintptr_t)MB.base();
            uintptr_t EndOfBlock = Addr + MB.size();
            // Align the address.
            Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);
            // Store cutted free memory block.
            MemGroup.FreeMem[i] = sys::MemoryBlock((void*)(Addr + Size), EndOfBlock - Addr - Size);
            return (uint8_t*)Addr;
        }
    }

    // No pre-allocated free block was large enough. Allocate a new memory region.
    // Note that all sections get allocated as read-write.  The permissions will
    // be updated later based on memory group.
    //
    // FIXME: It would be useful to define a default allocation size (or add
    // it as a constructor parameter) to minimize the number of allocations.
    //
    // FIXME: Initialize the Near member for each memory group to avoid
    // interleaving.
    error_code ec;
    sys::MemoryBlock MB = sys::Memory::allocateMappedMemory(RequiredSize, &MemGroup.Near,
                                                            sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
    if (ec) {
        // FIXME: Add error propogation to the interface.
        return NULL;
    }

    // Save this address as the basis for our next request
    MemGroup.Near = MB;

    MemGroup.AllocatedMem.push_back(MB);
    Addr = (uintptr_t)MB.base();
    uintptr_t EndOfBlock = Addr + MB.size();

    // Align the address.
    Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

    // The allocateMappedMemory may allocate much more memory than we need. In
    // this case, we store the unused memory as a free memory block.
    unsigned FreeSize = EndOfBlock - Addr - Size;
    if (FreeSize > 16)
        MemGroup.FreeMem.push_back(sys::MemoryBlock((void*)(Addr + Size), FreeSize));

    // Return aligned address
    return (uint8_t*)Addr;
}

bool PystonMemoryManager::finalizeMemory(std::string* ErrMsg) {
    // FIXME: Should in-progress permissions be reverted if an error occurs?
    error_code ec;

    // Don't allow free memory blocks to be used after setting protection flags.
    CodeMem.FreeMem.clear();

    // Make code memory executable.
    // pyston: also make it writeable so we can patch it later
    ec = applyMemoryGroupPermissions(CodeMem, sys::Memory::MF_READ | sys::Memory::MF_EXEC | sys::Memory::MF_WRITE);
    if (ec) {
        if (ErrMsg) {
            *ErrMsg = ec.message();
        }
        return true;
    }

    // Don't allow free memory blocks to be used after setting protection flags.
    RODataMem.FreeMem.clear();

    // Make read-only data memory read-only.
    ec = applyMemoryGroupPermissions(RODataMem, sys::Memory::MF_READ | sys::Memory::MF_EXEC);
    if (ec) {
        if (ErrMsg) {
            *ErrMsg = ec.message();
        }
        return true;
    }

    // Read-write data memory already has the correct permissions

    // Some platforms with separate data cache and instruction cache require
    // explicit cache flush, otherwise JIT code manipulations (like resolved
    // relocations) will get to the data cache but not to the instruction cache.
    invalidateInstructionCache();

    return false;
}

error_code PystonMemoryManager::applyMemoryGroupPermissions(MemoryGroup& MemGroup, unsigned Permissions) {

    for (int i = 0, e = MemGroup.AllocatedMem.size(); i != e; ++i) {
        error_code ec;
        ec = sys::Memory::protectMappedMemory(MemGroup.AllocatedMem[i], Permissions);
        if (ec) {
            return ec;
        }
    }

#if LLVMREV < 209952
    return error_code::success();
#else
    return error_code();
#endif
}

void PystonMemoryManager::invalidateInstructionCache() {
    for (int i = 0, e = CodeMem.AllocatedMem.size(); i != e; ++i)
        sys::Memory::InvalidateInstructionCache(CodeMem.AllocatedMem[i].base(), CodeMem.AllocatedMem[i].size());
}

uint64_t PystonMemoryManager::getSymbolAddress(const std::string& name) {
    uint64_t base = RTDyldMemoryManager::getSymbolAddress(name);
    if (base)
        return base;

    if (startswith(name, "__PRETTY_FUNCTION__")) {
        return getSymbolAddress(".L" + name);
    }

    printf("getSymbolAddress(%s); %lx\n", name.c_str(), base);
    return 0;
}

PystonMemoryManager::~PystonMemoryManager() {
    for (unsigned i = 0, e = CodeMem.AllocatedMem.size(); i != e; ++i)
        sys::Memory::releaseMappedMemory(CodeMem.AllocatedMem[i]);
    for (unsigned i = 0, e = RWDataMem.AllocatedMem.size(); i != e; ++i)
        sys::Memory::releaseMappedMemory(RWDataMem.AllocatedMem[i]);
    for (unsigned i = 0, e = RODataMem.AllocatedMem.size(); i != e; ++i)
        sys::Memory::releaseMappedMemory(RODataMem.AllocatedMem[i]);
}

llvm::RTDyldMemoryManager* createMemoryManager() {
    return new PystonMemoryManager();
}
}
