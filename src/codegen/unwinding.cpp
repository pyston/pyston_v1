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

#include "codegen/unwinding.h"

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include "llvm/DebugInfo/DIContext.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"
#include "llvm/IR/DebugInfo.h"

#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/llvm_interpreter.h"


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

class CFRegistry {
private:
    // TODO use a binary search tree
    std::vector<CompiledFunction*> cfs;

public:
    void registerCF(CompiledFunction* cf) { cfs.push_back(cf); }

    CompiledFunction* getCFForAddress(uint64_t addr) {
        for (auto* cf : cfs) {
            if (cf->code_start <= addr && addr < cf->code_start + cf->code_size)
                return cf;
        }
        return NULL;
    }
};

static CFRegistry cf_registry;

CompiledFunction* getCFForAddress(uint64_t addr) {
    return cf_registry.getCFForAddress(addr);
}

class TracebacksEventListener : public llvm::JITEventListener {
public:
    void NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
        std::unique_ptr<llvm::DIContext> Context(llvm::DIContext::getDWARFContext(*Obj.getObjectFile()));

        assert(g.cur_cf);
        assert(!g.cur_cf->line_table);
        LineTable* line_table = g.cur_cf->line_table = new LineTable();

        llvm_error_code ec;
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

                assert(g.cur_cf->code_start == 0);
                g.cur_cf->code_start = Addr;
                g.cur_cf->code_size = Size;
                cf_registry.registerCF(g.cur_cf);

                for (const auto& p : lines) {
                    line_table->entries.push_back(std::make_pair(
                        p.first, LineInfo(p.second.Line, p.second.Column, p.second.FileName, p.second.FunctionName)));
                }
            }
        }

        // Currently-unused libunwind support:
        llvm_error_code code;
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

struct PythonFrameId {
    enum FrameType {
        COMPILED,
        INTERPRETED,
    } type;

    union {
        uint64_t ip; // if type == COMPILED
        uint64_t bp; // if type == INTERPRETED
    };
};

class PythonFrameIterator {
private:
    PythonFrameId id;

    unw_context_t ctx;
    unw_cursor_t cursor;
    CompiledFunction* cf;

    PythonFrameIterator() {}

    // not copyable or movable, since 'cursor' holds an internal pointer to 'ctx'
    PythonFrameIterator(const PythonFrameIterator&) = delete;
    void operator=(const PythonFrameIterator&) = delete;
    PythonFrameIterator(const PythonFrameIterator&&) = delete;
    void operator=(const PythonFrameIterator&&) = delete;

public:
    CompiledFunction* getCF() const {
        assert(cf);
        return cf;
    }

    const PythonFrameId& getId() const { return id; }

    static std::unique_ptr<PythonFrameIterator> end() { return std::unique_ptr<PythonFrameIterator>(nullptr); }

    static std::unique_ptr<PythonFrameIterator> begin() {
        std::unique_ptr<PythonFrameIterator> rtn(new PythonFrameIterator());

        unw_getcontext(&rtn->ctx);
        unw_init_local(&rtn->cursor, &rtn->ctx);

        bool found = rtn->incr();
        if (!found)
            return NULL;
        return rtn;
    }

    uint64_t getReg(int dwarf_num) {
        assert(0 <= dwarf_num && dwarf_num < 16);

        // for x86_64, at least, libunwind seems to use the dwarf numbering

        unw_word_t rtn;
        int code = unw_get_reg(&cursor, dwarf_num, &rtn);
        assert(code == 0);
        return rtn;
    }

    bool incr() {
        while (true) {
            int r = unw_step(&this->cursor);

            if (r <= 0) {
                return false;
            }

            unw_word_t ip;
            unw_get_reg(&this->cursor, UNW_REG_IP, &ip);

            CompiledFunction* cf = getCFForAddress(ip);
            this->cf = cf;
            if (cf) {
                this->id.type = PythonFrameId::COMPILED;
                this->id.ip = ip;

                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                return true;
            }

            // TODO shouldn't need to do this expensive-looking query, if we
            // knew the bounds of the interpretFunction() function:
            unw_proc_info_t pip;
            int code = unw_get_proc_info(&this->cursor, &pip);
            RELEASE_ASSERT(code == 0, "%d", code);

            if (pip.start_ip == (intptr_t)interpretFunction) {
                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                this->id.type = PythonFrameId::INTERPRETED;
                this->id.bp = bp;
                return true;
            }

            // keep unwinding
        }
    }

    // Adapter classes to be able to use this in a C++11 range for loop.
    // (Needed because we do more memory management than typical.)
    // TODO: maybe the memory management should be handled by the Manager?
    class Manager {
    public:
        class Holder {
        public:
            std::unique_ptr<PythonFrameIterator> it;

            Holder(std::unique_ptr<PythonFrameIterator> it) : it(std::move(it)) {}

            bool operator!=(const Holder& rhs) const {
                assert(rhs.it.get() == NULL); // this is the only intended use case, for comparing to end()
                return it != rhs.it;
            }

            Holder& operator++() {
                assert(it.get());

                bool found = it->incr();
                if (!found)
                    it.release();
                return *this;
            }

            const PythonFrameIterator& operator*() const {
                assert(it.get());
                return *it.get();
            }
        };

        Holder begin() { return PythonFrameIterator::begin(); }

        Holder end() { return PythonFrameIterator::end(); }
    };
};

PythonFrameIterator::Manager unwindPythonFrames() {
    return PythonFrameIterator::Manager();
}

static std::unique_ptr<PythonFrameIterator> getTopPythonFrame() {
    std::unique_ptr<PythonFrameIterator> fr = PythonFrameIterator::begin();
    RELEASE_ASSERT(fr != PythonFrameIterator::end(), "no valid python frames??");

    return fr;
}

static const LineInfo* lineInfoForFrame(const PythonFrameIterator& frame_it) {
    auto& id = frame_it.getId();
    switch (id.type) {
        case PythonFrameId::COMPILED:
            return frame_it.getCF()->line_table->getLineInfoFor(id.ip);
        case PythonFrameId::INTERPRETED:
            return getLineInfoForInterpretedFrame((void*)id.bp);
    }
    abort();
}

std::vector<const LineInfo*> getTracebackEntries() {
    std::vector<const LineInfo*> entries;

    for (auto& frame_info : unwindPythonFrames()) {
        entries.push_back(lineInfoForFrame(frame_info));
    }

    std::reverse(entries.begin(), entries.end());
    return entries;
}

const LineInfo* getMostRecentLineInfo() {
    std::unique_ptr<PythonFrameIterator> frame = getTopPythonFrame();
    return lineInfoForFrame(*frame);
}

CompiledFunction* getTopCompiledFunction() {
    // TODO This is a bad way to do this...
    const LineInfo* last_entry = getMostRecentLineInfo();
    assert(last_entry->func.size());

    CompiledFunction* cf = cfForMachineFunctionName(last_entry->func);
    assert(cf);
    return cf;
}

BoxedModule* getCurrentModule() {
    return getTopCompiledFunction()->clfunc->source->parent_module;
}


llvm::JITEventListener* makeTracebacksListener() {
    return new TracebacksEventListener();
}
}
