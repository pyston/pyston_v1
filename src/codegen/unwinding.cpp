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

#include "codegen/unwinding.h"

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#if LLVMREV < 227586
#include "llvm/DebugInfo/DIContext.h"
#else
#include "llvm/DebugInfo/DWARF/DIContext.h"
#endif
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Object/ObjectFile.h"

#include "analysis/scoping_analysis.h"
#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/stackmaps.h"
#include "core/util.h"
#include "runtime/ctxswitching.h"
#include "runtime/objmodel.h"
#include "runtime/generator.h"
#include "runtime/traceback.h"
#include "runtime/types.h"


#define UNW_LOCAL_ONLY
#include <libunwind.h>
namespace {
int _dummy_ = unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
}

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
    u32++;

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

    // addr is the return address of the callsite, so we will check it against
    // the region (start, end] (opposite-endedness of normal half-open regions)
    CompiledFunction* getCFForAddress(uint64_t addr) {
        for (auto* cf : cfs) {
            if (cf->code_start < addr && addr <= cf->code_start + cf->code_size)
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
    virtual void NotifyObjectEmitted(const llvm::object::ObjectFile& Obj,
                                     const llvm::RuntimeDyld::LoadedObjectInfo& L) {
        std::unique_ptr<llvm::DIContext> Context(llvm::DIContext::getDWARFContext(Obj));

        assert(g.cur_cf);

        llvm_error_code ec;
        for (const auto& sym : Obj.symbols()) {
            llvm::object::SymbolRef::Type SymType;
            if (sym.getType(SymType))
                continue;
            if (SymType == llvm::object::SymbolRef::ST_Function) {
                llvm::StringRef Name;
                uint64_t Addr;
                uint64_t Size;
                if (sym.getName(Name))
                    continue;
                Addr = L.getSymbolLoadAddress(Name);
                assert(Addr);
                if (sym.getSize(Size))
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
            }
        }

        // Currently-unused libunwind support:
        llvm_error_code code;
        bool found_text = false, found_eh_frame = false;
        uint64_t text_addr = -1, text_size = -1;
        uint64_t eh_frame_addr = -1, eh_frame_size = -1;

        for (const auto& sec : Obj.sections()) {
            llvm::StringRef name;
            code = sec.getName(name);
            assert(!code);

            uint64_t addr, size;
            if (name == ".eh_frame") {
                assert(!found_eh_frame);
                eh_frame_addr = L.getSectionLoadAddress(name);
                eh_frame_size = sec.getSize();

                if (VERBOSITY())
                    printf("eh_frame: %lx %lx\n", eh_frame_addr, eh_frame_size);
                found_eh_frame = true;
            } else if (name == ".text") {
                assert(!found_text);
                text_addr = L.getSectionLoadAddress(name);
                text_size = sec.getSize();

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
    bool cur_is_osr;

    PythonFrameIterator() : cf(NULL), cur_is_osr(false) {}

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

    uint64_t readLocation(const StackMap::Record::Location& loc) {
        if (loc.type == StackMap::Record::Location::LocationType::Register) {
            // TODO: need to make sure we deal with patchpoints appropriately
            return getReg(loc.regnum);
        } else if (loc.type == StackMap::Record::Location::LocationType::Direct) {
            uint64_t reg_val = getReg(loc.regnum);
            return reg_val + loc.offset;
        } else if (loc.type == StackMap::Record::Location::LocationType::Indirect) {
            uint64_t reg_val = getReg(loc.regnum);
            uint64_t addr = reg_val + loc.offset;
            return *reinterpret_cast<uint64_t*>(addr);
        } else if (loc.type == StackMap::Record::Location::LocationType::Constant) {
            return loc.offset;
        } else if (loc.type == StackMap::Record::Location::LocationType::ConstIndex) {
            int const_idx = loc.offset;
            assert(const_idx >= 0);
            assert(const_idx < cf->location_map->constants.size());
            return getCF()->location_map->constants[const_idx];
        } else {
            printf("%d %d %d %d\n", loc.type, loc.flags, loc.regnum, loc.offset);
            abort();
        }
    }

    AST_stmt* getCurrentStatement() {
        if (id.type == PythonFrameId::COMPILED) {
            CompiledFunction* cf = getCF();
            uint64_t ip = getId().ip;

            assert(ip > cf->code_start);
            unsigned offset = ip - cf->code_start;

            assert(cf->location_map);
            const LocationMap::LocationTable& table = cf->location_map->names["!current_stmt"];
            assert(table.locations.size());

            // printf("Looking for something at offset %d (total ip: %lx)\n", offset, ip);
            for (const LocationMap::LocationTable::LocationEntry& e : table.locations) {
                // printf("(%d, %d]\n", e.offset, e.offset + e.length);
                if (e.offset < offset && offset <= e.offset + e.length) {
                    // printf("Found it\n");
                    assert(e.locations.size() == 1);
                    return reinterpret_cast<AST_stmt*>(readLocation(e.locations[0]));
                }
            }
            abort();
        } else if (id.type == PythonFrameId::INTERPRETED) {
            return getCurrentStatementForInterpretedFrame((void*)id.bp);
        }
        abort();
    }

    FrameInfo* getFrameInfo() {
        if (id.type == PythonFrameId::COMPILED) {
            CompiledFunction* cf = getCF();
            assert(cf->location_map->frameInfoFound());
            const auto& frame_info_loc = cf->location_map->frame_info_location;

            return reinterpret_cast<FrameInfo*>(readLocation(frame_info_loc));
        } else if (id.type == PythonFrameId::INTERPRETED) {
            return getFrameInfoForInterpretedFrame((void*)id.bp);
        }
        abort();
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

    unw_word_t getFunctionEnd(unw_word_t ip) {
        unw_proc_info_t pip;
        int ret = unw_get_proc_info_by_ip(unw_local_addr_space, ip, &pip, NULL);
        RELEASE_ASSERT(ret == 0 && pip.end_ip, "");
        return pip.end_ip;
    }

    bool incr() {
        static unw_word_t interpreter_instr_end = getFunctionEnd((unw_word_t)interpreter_instr_addr);
        static unw_word_t generator_entry_end = getFunctionEnd((unw_word_t)generatorEntry);

        bool was_osr = cur_is_osr;

        while (true) {
            int r = unw_step(&this->cursor);

            if (r <= 0) {
                return false;
            }

            unw_word_t ip;
            unw_get_reg(&this->cursor, UNW_REG_IP, &ip);

            cf = getCFForAddress(ip);
            if (cf) {
                this->id.type = PythonFrameId::COMPILED;
                this->id.ip = ip;

                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                cur_is_osr = (bool)cf->entry_descriptor;
                if (was_osr) {
                    // Skip the frame we just found if the previous one was its OSR
                    // TODO this will break if we start collapsing the OSR frames
                    return incr();
                }

                return true;
            }

            if ((unw_word_t)interpreter_instr_addr <= ip && ip < interpreter_instr_end) {
                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                this->id.type = PythonFrameId::INTERPRETED;
                this->id.bp = bp;
                cf = getCFForInterpretedFrame((void*)bp);

                cur_is_osr = (bool)cf->entry_descriptor;
                if (was_osr) {
                    // Skip the frame we just found if the previous one was its OSR
                    // TODO this will break if we start collapsing the OSR frames
                    return incr();
                }

                return true;
            }

            if ((unw_word_t)generatorEntry <= ip && ip < generator_entry_end) {
                // for generators continue unwinding in the context in which the generator got called
                unw_word_t bp;
                unw_get_reg(&this->cursor, UNW_TDEP_BP, &bp);

                Context* remote_ctx = getReturnContextForGeneratorFrame((void*)bp);
                // setup unw_context_t struct from the infos we have, seems like this is enough to make unwinding work.
                memset(&ctx, 0, sizeof(ctx));
                ctx.uc_mcontext.gregs[REG_R12] = remote_ctx->r12;
                ctx.uc_mcontext.gregs[REG_R13] = remote_ctx->r13;
                ctx.uc_mcontext.gregs[REG_R14] = remote_ctx->r14;
                ctx.uc_mcontext.gregs[REG_R15] = remote_ctx->r15;
                ctx.uc_mcontext.gregs[REG_RBX] = remote_ctx->rbx;
                ctx.uc_mcontext.gregs[REG_RBP] = remote_ctx->rbp;
                ctx.uc_mcontext.gregs[REG_RIP] = remote_ctx->rip;
                ctx.uc_mcontext.gregs[REG_RSP] = (greg_t)remote_ctx;
                unw_init_local(&cursor, &ctx);
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

            PythonFrameIterator& operator*() const {
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
    if (fr == PythonFrameIterator::end())
        return std::unique_ptr<PythonFrameIterator>();
    return fr;
}

static const LineInfo* lineInfoForFrame(PythonFrameIterator& frame_it) {
    AST_stmt* current_stmt = frame_it.getCurrentStatement();
    auto* cf = frame_it.getCF();
    assert(cf);

    auto source = cf->clfunc->source;

    // Hack: the "filename" for eval and exec statements is "<string>", not the filename
    // of the parent module.  We can't currently represent this the same way that CPython does
    // (but we probably should), so just check that here:
    const std::string* fn = &source->parent_module->fn;
    if (source->ast->type == AST_TYPE::Suite /* exec */ || source->ast->type == AST_TYPE::Expression /* eval */) {
        static const std::string string_str("<string>");
        fn = &string_str;
    }

    return new LineInfo(current_stmt->lineno, current_stmt->col_offset, *fn, source->getName());
}

static StatCounter us_gettraceback("us_gettraceback");
BoxedTraceback* getTraceback() {
    if (!ENABLE_FRAME_INTROSPECTION) {
        static bool printed_warning = false;
        if (!printed_warning) {
            printed_warning = true;
            fprintf(stderr, "Warning: can't get traceback since ENABLE_FRAME_INTROSPECTION=0\n");
        }
        return new BoxedTraceback();
    }

    Timer _t("getTraceback");

    std::vector<const LineInfo*> entries;
    for (auto& frame_iter : unwindPythonFrames()) {
        const LineInfo* line_info = lineInfoForFrame(frame_iter);
        if (line_info)
            entries.push_back(line_info);
    }

    std::reverse(entries.begin(), entries.end());

    long us = _t.end();
    us_gettraceback.log(us);

    return new BoxedTraceback(std::move(entries));
}

ExcInfo* getFrameExcInfo() {
    std::vector<ExcInfo*> to_update;
    ExcInfo* copy_from_exc = NULL;
    ExcInfo* cur_exc = NULL;
    for (PythonFrameIterator& frame_iter : unwindPythonFrames()) {
        FrameInfo* frame_info = frame_iter.getFrameInfo();

        copy_from_exc = &frame_info->exc;
        if (!cur_exc)
            cur_exc = copy_from_exc;

        if (!copy_from_exc->type) {
            to_update.push_back(copy_from_exc);
            continue;
        }

        break;
    }

    assert(copy_from_exc); // Only way this could still be NULL is if there weren't any python frames

    if (!copy_from_exc->type) {
        // No exceptions found:
        *copy_from_exc = ExcInfo(None, None, None);
    }

    assert(gc::isValidGCObject(copy_from_exc->type));
    assert(gc::isValidGCObject(copy_from_exc->value));
    assert(gc::isValidGCObject(copy_from_exc->traceback));

    for (auto* ex : to_update) {
        *ex = *copy_from_exc;
    }
    assert(cur_exc);
    return cur_exc;
}

CompiledFunction* getTopCompiledFunction() {
    auto rtn = getTopPythonFrame();
    if (!rtn)
        return NULL;
    return getTopPythonFrame()->getCF();
}

BoxedModule* getCurrentModule() {
    CompiledFunction* compiledFunction = getTopCompiledFunction();
    if (!compiledFunction)
        return NULL;
    return compiledFunction->clfunc->source->parent_module;
}

// TODO factor getStackLoclasIncludingUserHidden and fastLocalsToBoxedLocals
// because they are pretty ugly but have a pretty repetitive pattern.

FrameStackState getFrameStackState() {
    for (PythonFrameIterator& frame_iter : unwindPythonFrames()) {
        BoxedDict* d;
        BoxedClosure* closure;
        CompiledFunction* cf;
        if (frame_iter.getId().type == PythonFrameId::COMPILED) {
            d = new BoxedDict();

            cf = frame_iter.getCF();
            uint64_t ip = frame_iter.getId().ip;

            assert(ip > cf->code_start);
            unsigned offset = ip - cf->code_start;

            assert(cf->location_map);

            // We have to detect + ignore any entries for variables that
            // could have been defined (so they have entries) but aren't (so the
            // entries point to uninitialized memory).
            std::unordered_set<std::string> is_undefined;

            for (const auto& p : cf->location_map->names) {
                if (!startswith(p.first, "!is_defined_"))
                    continue;

                for (const LocationMap::LocationTable::LocationEntry& e : p.second.locations) {
                    if (e.offset < offset && offset <= e.offset + e.length) {
                        const auto& locs = e.locations;

                        assert(locs.size() == 1);
                        uint64_t v = frame_iter.readLocation(locs[0]);
                        if ((v & 1) == 0)
                            is_undefined.insert(p.first.substr(12));

                        break;
                    }
                }
            }

            for (const auto& p : cf->location_map->names) {
                if (p.first[0] == '!')
                    continue;

                if (is_undefined.count(p.first))
                    continue;

                for (const LocationMap::LocationTable::LocationEntry& e : p.second.locations) {
                    if (e.offset < offset && offset <= e.offset + e.length) {
                        const auto& locs = e.locations;

                        llvm::SmallVector<uint64_t, 1> vals;
                        // printf("%s: %s\n", p.first.c_str(), e.type->debugName().c_str());

                        for (auto& loc : locs) {
                            vals.push_back(frame_iter.readLocation(loc));
                        }

                        Box* v = e.type->deserializeFromFrame(vals);
                        // printf("%s: (pp id %ld) %p\n", p.first.c_str(), e._debug_pp_id, v);
                        assert(gc::isValidGCObject(v));
                        d->d[boxString(p.first)] = v;
                    }
                }
            }
        } else {
            abort();
        }

        return FrameStackState(d, frame_iter.getFrameInfo());
    }
    RELEASE_ASSERT(0, "Internal error: unable to find any python frames");
}

Box* fastLocalsToBoxedLocals(int framesToSkip) {
    for (PythonFrameIterator& frame_iter : unwindPythonFrames()) {
        if (--framesToSkip >= 0)
            continue;

        BoxedDict* d;
        BoxedClosure* closure;
        FrameInfo* frame_info;

        CompiledFunction* cf = frame_iter.getCF();
        ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();

        if (scope_info->areLocalsFromModule()) {
            // TODO we should cache this in frame_info->locals or something so that locals()
            // (and globals() too) will always return the same dict
            return makeAttrWrapper(getCurrentModule());
        }

        if (frame_iter.getId().type == PythonFrameId::COMPILED) {
            d = new BoxedDict();

            uint64_t ip = frame_iter.getId().ip;

            assert(ip > cf->code_start);
            unsigned offset = ip - cf->code_start;

            assert(cf->location_map);

            // We have to detect + ignore any entries for variables that
            // could have been defined (so they have entries) but aren't (so the
            // entries point to uninitialized memory).
            std::unordered_set<std::string> is_undefined;

            for (const auto& p : cf->location_map->names) {
                if (!startswith(p.first, "!is_defined_"))
                    continue;

                for (const LocationMap::LocationTable::LocationEntry& e : p.second.locations) {
                    if (e.offset < offset && offset <= e.offset + e.length) {
                        const auto& locs = e.locations;

                        assert(locs.size() == 1);
                        uint64_t v = frame_iter.readLocation(locs[0]);
                        if ((v & 1) == 0)
                            is_undefined.insert(p.first.substr(12));

                        break;
                    }
                }
            }

            for (const auto& p : cf->location_map->names) {
                if (p.first[0] == '!')
                    continue;

                if (p.first[0] == '#')
                    continue;

                if (is_undefined.count(p.first))
                    continue;

                for (const LocationMap::LocationTable::LocationEntry& e : p.second.locations) {
                    if (e.offset < offset && offset <= e.offset + e.length) {
                        const auto& locs = e.locations;

                        llvm::SmallVector<uint64_t, 1> vals;
                        // printf("%s: %s\n", p.first.c_str(), e.type->debugName().c_str());

                        for (auto& loc : locs) {
                            vals.push_back(frame_iter.readLocation(loc));
                        }

                        Box* v = e.type->deserializeFromFrame(vals);
                        // printf("%s: (pp id %ld) %p\n", p.first.c_str(), e._debug_pp_id, v);
                        assert(gc::isValidGCObject(v));
                        d->d[boxString(p.first)] = v;
                    }
                }
            }

            closure = NULL;
            if (cf->location_map->names.count(PASSED_CLOSURE_NAME) > 0) {
                for (const LocationMap::LocationTable::LocationEntry& e :
                     cf->location_map->names[PASSED_CLOSURE_NAME].locations) {
                    if (e.offset < offset && offset <= e.offset + e.length) {
                        const auto& locs = e.locations;

                        llvm::SmallVector<uint64_t, 1> vals;

                        for (auto& loc : locs) {
                            vals.push_back(frame_iter.readLocation(loc));
                        }

                        Box* v = e.type->deserializeFromFrame(vals);
                        assert(gc::isValidGCObject(v));
                        closure = static_cast<BoxedClosure*>(v);
                    }
                }
            }

            frame_info = frame_iter.getFrameInfo();
        } else if (frame_iter.getId().type == PythonFrameId::INTERPRETED) {
            d = localsForInterpretedFrame((void*)frame_iter.getId().bp, true);
            closure = passedClosureForInterpretedFrame((void*)frame_iter.getId().bp);
            frame_info = getFrameInfoForInterpretedFrame((void*)frame_iter.getId().bp);
        } else {
            abort();
        }

        assert(frame_info);
        if (frame_info->boxedLocals == NULL) {
            frame_info->boxedLocals = new BoxedDict();
        }
        assert(gc::isValidGCObject(frame_info->boxedLocals));

        // Add the locals from the closure
        // TODO in a ClassDef scope, we aren't supposed to add these
        size_t depth = 0;
        for (auto& p : scope_info->getAllDerefVarsAndInfo()) {
            InternedString name = p.first;
            DerefInfo derefInfo = p.second;
            while (depth < derefInfo.num_parents_from_passed_closure) {
                depth++;
                closure = closure->parent;
            }
            assert(closure != NULL);
            Box* val = closure->elts[derefInfo.offset];
            Box* boxedName = boxString(name.str());
            if (val != NULL) {
                d->d[boxedName] = val;
            } else {
                d->d.erase(boxedName);
            }
        }

        // Loop through all the values found above.
        // TODO Right now d just has all the python variables that are *initialized*
        // But we also need to loop through all the uninitialized variables that we have
        // access to and delete them from the locals dict
        for (const auto& p : d->d) {
            Box* varname = p.first;
            Box* value = p.second;
            setitem(frame_info->boxedLocals, varname, value);
        }

        return frame_info->boxedLocals;
    }
    RELEASE_ASSERT(0, "Internal error: unable to find any python frames");
}

ExecutionPoint getExecutionPoint() {
    auto frame = getTopPythonFrame();
    auto cf = frame->getCF();
    auto current_stmt = frame->getCurrentStatement();
    return ExecutionPoint({.cf = cf, .current_stmt = current_stmt });
}

std::unique_ptr<ExecutionPoint> getExecutionPoint(int framesToSkip) {
    CompiledFunction* cf = NULL;
    AST_stmt* stmt = NULL;

    for (PythonFrameIterator& frame_iter : unwindPythonFrames()) {
        // skip depth frames
        if (--framesToSkip >= 0)
            continue;

        cf = frame_iter.getCF();
        stmt = frame_iter.getCurrentStatement();
        break;
    }
    if (!cf)
        return std::unique_ptr<ExecutionPoint>();

    return std::unique_ptr<ExecutionPoint>(new ExecutionPoint({.cf = cf, .current_stmt = stmt }));
}

llvm::JITEventListener* makeTracebacksListener() {
    return new TracebacksEventListener();
}
}
