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

#include "codegen/memmgr.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/Process.h"

#include "codegen/irgen/util.h"
#include "codegen/unwinding.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/util.h"

// This code was copy-pasted from SectionMemoryManager.cpp;
// TODO eventually I should remove this using directive
// and recode in my style
using namespace llvm;

namespace pyston {

class PystonMemoryManager : public RTDyldMemoryManager {
public:
    PystonMemoryManager() {}
    ~PystonMemoryManager() override;

    uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                                 StringRef SectionName) override;

    uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID, StringRef SectionName,
                                 bool isReadOnly) override;

    bool finalizeMemory(std::string* ErrMsg = 0) override;

private:
    void invalidateInstructionCache();

    struct FreeMemBlock {
        sys::MemoryBlock Free;
        unsigned PendingPrefixIndex;
    };
    struct MemoryGroup {
        SmallVector<sys::MemoryBlock, 16> PendingMem;
        SmallVector<sys::MemoryBlock, 16> AllocatedMem;
        SmallVector<FreeMemBlock, 16> FreeMem;
        sys::MemoryBlock Near;
    };

    uint8_t* allocateSection(MemoryGroup& MemGroup, uintptr_t Size, unsigned Alignment, StringRef SectionName);

    llvm_error_code applyMemoryGroupPermissions(MemoryGroup& MemGroup, unsigned Permissions);

    uint64_t getSymbolAddress(const std::string& Name) override;

    MemoryGroup CodeMem;
    MemoryGroup RWDataMem;
    MemoryGroup RODataMem;
};

uint8_t* PystonMemoryManager::allocateDataSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                                                  StringRef SectionName, bool IsReadOnly) {
    // printf("allocating data section: %ld %d %d %s %d\n", Size, Alignment, SectionID, SectionName.data(), IsReadOnly);
    // assert(SectionName != ".llvm_stackmaps");
    if (IsReadOnly)
        return allocateSection(RODataMem, Size, Alignment, SectionName);
    return allocateSection(RWDataMem, Size, Alignment, SectionName);
}

uint8_t* PystonMemoryManager::allocateCodeSection(uintptr_t Size, unsigned Alignment, unsigned SectionID,
                                                  StringRef SectionName) {
    // printf("allocating code section: %ld %d %d %s\n", Size, Alignment, SectionID, SectionName.data());
    return allocateSection(CodeMem, Size, Alignment, SectionName);
}

uint8_t* PystonMemoryManager::allocateSection(MemoryGroup& MemGroup, uintptr_t Size, unsigned Alignment,
                                              StringRef SectionName) {
    if (!Alignment)
        Alignment = 16;

    assert(!(Alignment & (Alignment - 1)) && "Alignment must be a power of two.");

    uintptr_t RequiredSize = Alignment * ((Size + Alignment - 1) / Alignment + 1);
    uintptr_t Addr = 0;

    // Look in the list of free memory regions and use a block there if one
    // is available.
    for (FreeMemBlock& FreeMB : MemGroup.FreeMem) {
        if (FreeMB.Free.size() >= RequiredSize) {
            Addr = (uintptr_t)FreeMB.Free.base();
            uintptr_t EndOfBlock = Addr + FreeMB.Free.size();
            // Align the address.
            Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

            if (FreeMB.PendingPrefixIndex == (unsigned)-1) {
                // The part of the block we're giving out to the user is now pending
                MemGroup.PendingMem.push_back(sys::MemoryBlock((void*)Addr, Size));

                // Remember this pending block, such that future allocations can just
                // modify it rather than creating a new one
                FreeMB.PendingPrefixIndex = MemGroup.PendingMem.size() - 1;
            } else {
                sys::MemoryBlock& PendingMB = MemGroup.PendingMem[FreeMB.PendingPrefixIndex];
                PendingMB = sys::MemoryBlock(PendingMB.base(), Addr + Size - (uintptr_t)PendingMB.base());
            }

            // Remember how much free space is now left in this block
            FreeMB.Free = sys::MemoryBlock((void*)(Addr + Size), EndOfBlock - Addr - Size);
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
    llvm_error_code ec;
    sys::MemoryBlock MB = sys::Memory::allocateMappedMemory(RequiredSize, &MemGroup.Near,
                                                            sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
    if (ec) {
        // FIXME: Add error propagation to the interface.
        return nullptr;
    }

    std::string stat_name = "mem_section_" + std::string(SectionName);
    Stats::log(Stats::getStatCounter(stat_name), MB.size());

    // Save this address as the basis for our next request
    MemGroup.Near = MB;

    // Remember that we allocated this memory
    MemGroup.AllocatedMem.push_back(MB);
    Addr = (uintptr_t)MB.base();
    uintptr_t EndOfBlock = Addr + MB.size();

    // Align the address.
    Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

    // The part of the block we're giving out to the user is now pending
    MemGroup.PendingMem.push_back(sys::MemoryBlock((void*)Addr, Size));

    // The allocateMappedMemory may allocate much more memory than we need. In
    // this case, we store the unused memory as a free memory block.
    unsigned FreeSize = EndOfBlock - Addr - Size;
    if (FreeSize > 16) {
        FreeMemBlock FreeMB;
        FreeMB.Free = sys::MemoryBlock((void*)(Addr + Size), FreeSize);
        FreeMB.PendingPrefixIndex = (unsigned)-1;
        MemGroup.FreeMem.push_back(FreeMB);
    }

    // Return aligned address
    return (uint8_t*)Addr;
}

bool PystonMemoryManager::finalizeMemory(std::string* ErrMsg) {
    // FIXME: Should in-progress permissions be reverted if an error occurs?
    llvm_error_code ec;

    // Make code memory executable.
    // pyston: also make it writeable so we can patch it later
    ec = applyMemoryGroupPermissions(CodeMem, sys::Memory::MF_READ | sys::Memory::MF_EXEC | sys::Memory::MF_WRITE);
    if (ec) {
        if (ErrMsg) {
            *ErrMsg = ec.message();
        }
        RELEASE_ASSERT(0, "finalizeMemory failed");
    }

    // Make read-only data memory read-only.
    ec = applyMemoryGroupPermissions(RODataMem, sys::Memory::MF_READ | sys::Memory::MF_EXEC);
    if (ec) {
        if (ErrMsg) {
            *ErrMsg = ec.message();
        }
        RELEASE_ASSERT(0, "finalizeMemory failed");
    }

    // Read-write data memory already has the correct permissions

    // Some platforms with separate data cache and instruction cache require
    // explicit cache flush, otherwise JIT code manipulations (like resolved
    // relocations) will get to the data cache but not to the instruction cache.
    invalidateInstructionCache();

    return false;
}

static sys::MemoryBlock trimBlockToPageSize(sys::MemoryBlock M) {
    static const size_t PageSize = sys::Process::getPageSize();

    size_t StartOverlap = (PageSize - ((uintptr_t)M.base() % PageSize)) % PageSize;

    size_t TrimmedSize = M.size();
    TrimmedSize -= StartOverlap;
    TrimmedSize -= TrimmedSize % PageSize;

    sys::MemoryBlock Trimmed((void*)((uintptr_t)M.base() + StartOverlap), TrimmedSize);

    assert(((uintptr_t)Trimmed.base() % PageSize) == 0);
    assert((Trimmed.size() % PageSize) == 0);
    assert(M.base() <= Trimmed.base() && Trimmed.size() <= M.size());

    return Trimmed;
}

llvm_error_code PystonMemoryManager::applyMemoryGroupPermissions(MemoryGroup& MemGroup, unsigned Permissions) {

    for (sys::MemoryBlock& MB : MemGroup.PendingMem) {
        llvm_error_code ec;
        ec = sys::Memory::protectMappedMemory(MB, Permissions);
        if (ec) {
            return ec;
        }
    }
    MemGroup.PendingMem.clear();

    // Now go through free blocks and trim any of them that don't span the entire
    // page because one of the pending blocks may have overlapped it.
    for (FreeMemBlock& FreeMB : MemGroup.FreeMem) {
        FreeMB.Free = trimBlockToPageSize(FreeMB.Free);
        // We cleared the PendingMem list, so all these pointers are now invalid
        FreeMB.PendingPrefixIndex = (unsigned)-1;
    }

    // Remove all blocks which are now empty
    MemGroup.FreeMem.erase(remove_if(MemGroup.FreeMem, [](FreeMemBlock& FreeMB) { return FreeMB.Free.size() == 0; }),
                           MemGroup.FreeMem.end());

#if LLVMREV < 209952
    return llvm_error_code::success();
#else
    return llvm_error_code();
#endif
}

void PystonMemoryManager::invalidateInstructionCache() {
    for (sys::MemoryBlock& Block : CodeMem.PendingMem)
        sys::Memory::InvalidateInstructionCache(Block.base(), Block.size());
}

uint64_t PystonMemoryManager::getSymbolAddress(const std::string& name) {
    uint64_t base = (uint64_t)getValueOfRelocatableSym(name);
    if (base)
        return base;

    // make sure our own c++ exc implementations symbols get used instead of gcc ones.
    base = getCXXUnwindSymbolAddress(name);
    if (base)
        return base;

    base = RTDyldMemoryManager::getSymbolAddress(name);
    if (base)
        return base;

    if (startswith(name, "__PRETTY_FUNCTION__")) {
        return getSymbolAddress(".L" + name);
    }

    RELEASE_ASSERT(0, "Could not find sym: %s", name.c_str());
    return 0;
}

PystonMemoryManager::~PystonMemoryManager() {
    for (MemoryGroup* Group : { &CodeMem, &RWDataMem, &RODataMem }) {
        for (sys::MemoryBlock& Block : Group->AllocatedMem)
            sys::Memory::releaseMappedMemory(Block);
    }
}

std::unique_ptr<llvm::RTDyldMemoryManager> createMemoryManager() {
    return std::unique_ptr<llvm::RTDyldMemoryManager>(new PystonMemoryManager());
}
}
