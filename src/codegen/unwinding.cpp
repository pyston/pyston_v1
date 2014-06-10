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

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include "llvm/DebugInfo/DIContext.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/IR/DebugInfo.h"

#include "codegen/codegen.h"


#define UNW_LOCAL_ONLY
#include <libunwind.h>

// Definition from libunwind, but standardized I suppose by the format of the .eh_frame_hdr section:
struct uw_table_entry {
    int32_t start_ip_offset;
    int32_t fde_offset;
};

namespace pyston {

// Parse an .eh_frame section, and construct a "binary search table" such as you would find in a .eh_frame_hdr section.
// Currently only supports .eh_frame sections with exactly one fde.
void parseEhFrame(uint64_t start_addr, uint64_t size, uint64_t* out_data, uint64_t* out_len) {
    union {
        uint8_t* u8;
        uint32_t* u32;
    };
    u32 = (uint32_t*)start_addr;

    int cie_length = *u32;
    *u32++;

    assert(*u32 == 0); // CIE ID

    u8 += cie_length;

    int fde_length = *u32;
    u32++;

    assert(cie_length + fde_length + 8 == size && "more than one fde! (supportable, but not implemented)");

    int nentries = 1;
    uw_table_entry* table_data = new uw_table_entry[nentries];
    table_data->start_ip_offset = 0;
    table_data->fde_offset = 4 + cie_length;

    *out_data = (uintptr_t)table_data;
    *out_len = nentries;
}

class LineTableRegistry {
private:
    struct LineTableRegistryEntry {
        const uint64_t addr, size;
        std::vector<std::pair<uint64_t, LineInfo> > linetable;
        LineTableRegistryEntry(uint64_t addr, uint64_t size) : addr(addr), size(size) {}
    };

    std::vector<LineTableRegistryEntry> entries;

public:
    void registerLineTable(uint64_t addr, uint64_t size, llvm::DILineInfoTable& lines) {
        entries.push_back(LineTableRegistryEntry(addr, size));

        auto& entry = entries.back();
        for (int i = 0; i < lines.size(); i++) {
            entry.linetable.push_back(
                std::make_pair(lines[i].first, LineInfo(lines[i].second.Line, lines[i].second.Column,
                                                        lines[i].second.FileName, lines[i].second.FunctionName)));
        }
    }

    const LineInfo* getLineInfoFor(uint64_t addr) {
        for (const auto& entry : entries) {
            if (addr < entry.addr || addr >= entry.addr + entry.size)
                continue;

            const auto& linetable = entry.linetable;
            for (int i = linetable.size() - 1; i >= 0; i--) {
                if (linetable[i].first < addr)
                    return &linetable[i].second;
            }
            abort();
        }
        return NULL;
    }
};
static LineTableRegistry line_table_registry;

const LineInfo* getLineInfoFor(uint64_t addr) {
    return line_table_registry.getLineInfoFor(addr);
}

class TracebacksEventListener : public llvm::JITEventListener {
public:
    void NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
        llvm::DIContext* Context = llvm::DIContext::getDWARFContext(Obj.getObjectFile());

        llvm::error_code ec;
        for (llvm::object::symbol_iterator I = Obj.begin_symbols(), E = Obj.end_symbols(); I != E && !ec; ++I) {
            llvm::object::SymbolRef::Type SymType;
            if (I->getType(SymType))
                continue;
            if (SymType == llvm::object::SymbolRef::ST_Function) {
                llvm::StringRef Name;
                uint64_t Addr;
                uint64_t Size;
                if (I->getName(Name))
                    continue;
                if (I->getAddress(Addr))
                    continue;
                if (I->getSize(Size))
                    continue;

// TODO this should be the Python name, not the C name:
#if LLVMREV < 208921
                llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(
                    Addr, Size, llvm::DILineInfoSpecifier::FunctionName | llvm::DILineInfoSpecifier::FileLineInfo
                                | llvm::DILineInfoSpecifier::AbsoluteFilePath);
#else
                llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(
                    Addr, Size, llvm::DILineInfoSpecifier(llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                                                          llvm::DILineInfoSpecifier::FunctionNameKind::LinkageName));
#endif
                if (VERBOSITY() >= 2) {
                    for (int i = 0; i < lines.size(); i++) {
                        printf("%s:%d, %s: %lx\n", lines[i].second.FileName.c_str(), lines[i].second.Line,
                               lines[i].second.FunctionName.c_str(), lines[i].first);
                    }
                }
                line_table_registry.registerLineTable(Addr, Size, lines);
            }
        }
        delete Context;

        // Currently-unused libunwind support:
        llvm::error_code code;
        bool found_text = false, found_eh_frame = false;
        uint64_t text_addr, text_size;
        uint64_t eh_frame_addr, eh_frame_size;

        for (llvm::object::section_iterator I = Obj.begin_sections(), E = Obj.end_sections(); I != E; ++I) {
            llvm::StringRef name;
            code = I->getName(name);
            assert(!code);

            uint64_t addr, size;
            if (name == ".eh_frame") {
                assert(!found_eh_frame);
                if (I->getAddress(eh_frame_addr))
                    continue;
                if (I->getSize(eh_frame_size))
                    continue;

                if (VERBOSITY())
                    printf("eh_frame: %lx %lx\n", eh_frame_addr, eh_frame_size);
                found_eh_frame = true;
            } else if (name == ".text") {
                assert(!found_text);
                if (I->getAddress(text_addr))
                    continue;
                if (I->getSize(text_size))
                    continue;

                if (VERBOSITY())
                    printf("text: %lx %lx\n", text_addr, text_size);
                found_text = true;
            }
        }

        assert(found_text);
        assert(found_eh_frame);

        unw_dyn_info_t* dyn_info = new unw_dyn_info_t();
        dyn_info->start_ip = text_addr;
        dyn_info->end_ip = text_addr + text_size;
        dyn_info->format = UNW_INFO_FORMAT_REMOTE_TABLE;

        dyn_info->u.rti.name_ptr = 0;
        dyn_info->u.rti.segbase = eh_frame_addr;
        parseEhFrame(eh_frame_addr, eh_frame_size, &dyn_info->u.rti.table_data, &dyn_info->u.rti.table_len);

        if (VERBOSITY())
            printf("dyn_info = %p, table_data = %p\n", dyn_info, (void*)dyn_info->u.rti.table_data);
        _U_dyn_register(dyn_info);

        // TODO: it looks like libunwind does a linear search over anything dynamically registered,
        // as opposed to the binary search it can do within a dyn_info.
        // If we're registering a lot of dyn_info's, it might make sense to coalesce them into a single
        // dyn_info that contains a binary search table.
    }
};

std::string getPythonFuncAt(void* addr, void* sp) {
    return "";
}

llvm::JITEventListener* makeTracebacksListener() {
    return new TracebacksEventListener();
}
}
