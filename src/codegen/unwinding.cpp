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
#include <sstream>
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

#include "asm_writing/types.h"
#include "analysis/scoping_analysis.h"
#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/stackmaps.h"
#include "core/cfg.h"
#include "core/util.h"
#include "runtime/ctxswitching.h"
#include "runtime/objmodel.h"
#include "runtime/generator.h"
#include "runtime/traceback.h"
#include "runtime/types.h"


#define UNW_LOCAL_ONLY
#include <libunwind.h>
#undef UNW_LOCAL_ONLY

namespace {
int _dummy_ = unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
}

// Definition from libunwind, but standardized I suppose by the format of the .eh_frame_hdr section:
struct uw_table_entry {
    int32_t start_ip_offset;
    int32_t fde_offset;
};

namespace pyston {

static BoxedClass* unwind_session_cls;

// Parse an .eh_frame section, and construct a "binary search table" such as you would find in a .eh_frame_hdr section.
// Currently only supports .eh_frame sections with exactly one fde.
// See http://www.airs.com/blog/archives/460 for some useful info.
void parseEhFrame(uint64_t start_addr, uint64_t size, uint64_t func_addr, uint64_t* out_data, uint64_t* out_len) {
    // NB. according to sully@msully.net, this is not legal C++ b/c type-punning through unions isn't allowed.
    // But I can't find a compiler flag that warns on it, and it seems to work.
    union {
        uint8_t* u8;
        uint32_t* u32;
    };
    u32 = (uint32_t*)start_addr;

    int32_t cie_length = *u32;
    assert(cie_length != 0xffffffff); // 0xffffffff would indicate a 64-bit DWARF format
    u32++;

    assert(*u32 == 0); // CIE ID

    u8 += cie_length;

    int fde_length = *u32;
    u32++;

    assert(cie_length + fde_length + 8 == size && "more than one fde! (supportable, but not implemented)");

    int nentries = 1;
    uw_table_entry* table_data = new uw_table_entry[nentries];
    table_data->start_ip_offset = func_addr - start_addr;
    table_data->fde_offset = 4 + cie_length;

    *out_data = (uintptr_t)table_data;
    *out_len = nentries;
}

void registerDynamicEhFrame(uint64_t code_addr, size_t code_size, uint64_t eh_frame_addr, size_t eh_frame_size) {
    unw_dyn_info_t* dyn_info = new unw_dyn_info_t();
    dyn_info->start_ip = code_addr;
    dyn_info->end_ip = code_addr + code_size;
    // TODO: It's not clear why we use UNW_INFO_FORMAT_REMOTE_TABLE instead of UNW_INFO_FORMAT_TABLE. kmod reports that
    // he tried FORMAT_TABLE and it didn't work, but it wasn't clear why. However, using FORMAT_REMOTE_TABLE forces
    // indirection through an access_mem() callback, and indeed, a function named access_mem() shows up in our `perf`
    // results! So it's possible there's a performance win lurking here.
    dyn_info->format = UNW_INFO_FORMAT_REMOTE_TABLE;

    dyn_info->u.rti.name_ptr = 0;
    dyn_info->u.rti.segbase = eh_frame_addr;
    parseEhFrame(eh_frame_addr, eh_frame_size, code_addr, &dyn_info->u.rti.table_data, &dyn_info->u.rti.table_len);

    if (VERBOSITY() >= 2)
        printf("dyn_info = %p, table_data = %p\n", dyn_info, (void*)dyn_info->u.rti.table_data);
    _U_dyn_register(dyn_info);

    // TODO: it looks like libunwind does a linear search over anything dynamically registered,
    // as opposed to the binary search it can do within a dyn_info.
    // If we're registering a lot of dyn_info's, it might make sense to coalesce them into a single
    // dyn_info that contains a binary search table.
}

struct compare_cf {
    int operator()(const uint64_t& key, const CompiledFunction* item) const {
        // key is the return address of the callsite, so we will check it against
        // the region (start, end] (opposite-endedness of normal half-open regions)
        if (key <= item->code_start)
            return -1;
        else if (key > item->code_start + item->code_size)
            return 1;
        return 0;
    }
};

class CFRegistry {
private:
    std::vector<CompiledFunction*> cfs;

public:
    void registerCF(CompiledFunction* cf) {
        if (cfs.empty()) {
            cfs.push_back(cf);
            return;
        }

        int idx = binarySearch((uint64_t)cf->code_start, cfs.begin(), cfs.end(), compare_cf());
        if (idx >= 0)
            RELEASE_ASSERT(0, "CompiledFunction registered twice?");

        cfs.insert(cfs.begin() + (-idx - 1), cf);
    }

    CompiledFunction* getCFForAddress(uint64_t addr) {
        if (cfs.empty())
            return NULL;

        int idx = binarySearch(addr, cfs.begin(), cfs.end(), compare_cf());
        if (idx >= 0)
            return cfs[idx];

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

        uint64_t func_addr = 0; // remains 0 until we find a function

        // Search through the symbols to find the function that got JIT'ed.
        // (We only JIT one function at a time.)
        for (const auto& sym : Obj.symbols()) {
            llvm::object::SymbolRef::Type SymType;
            if (sym.getType(SymType) || SymType != llvm::object::SymbolRef::ST_Function)
                continue;

            llvm::StringRef Name;
            uint64_t Size;
            if (sym.getName(Name) || sym.getSize(Size))
                continue;

            // Found a function!
            assert(!func_addr);
            func_addr = L.getSymbolLoadAddress(Name);
            assert(func_addr);

// TODO this should be the Python name, not the C name:
#if LLVMREV < 208921
            llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(
                func_addr, Size, llvm::DILineInfoSpecifier::FunctionName | llvm::DILineInfoSpecifier::FileLineInfo
                                     | llvm::DILineInfoSpecifier::AbsoluteFilePath);
#else
            llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(
                func_addr, Size,
                llvm::DILineInfoSpecifier(llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                                          llvm::DILineInfoSpecifier::FunctionNameKind::LinkageName));
#endif
            if (VERBOSITY() >= 3) {
                for (int i = 0; i < lines.size(); i++) {
                    printf("%s:%d, %s: %lx\n", lines[i].second.FileName.c_str(), lines[i].second.Line,
                           lines[i].second.FunctionName.c_str(), lines[i].first);
                }
            }

            assert(g.cur_cf->code_start == 0);
            g.cur_cf->code_start = func_addr;
            g.cur_cf->code_size = Size;
            cf_registry.registerCF(g.cur_cf);
        }

        assert(func_addr);

        // Libunwind support:
        bool found_text = false, found_eh_frame = false;
        uint64_t text_addr = -1, text_size = -1;
        uint64_t eh_frame_addr = -1, eh_frame_size = -1;

        for (const auto& sec : Obj.sections()) {
            llvm::StringRef name;
            llvm_error_code code = sec.getName(name);
            assert(!code);

            uint64_t addr, size;
            if (name == ".eh_frame") {
                assert(!found_eh_frame);
                eh_frame_addr = L.getSectionLoadAddress(name);
                eh_frame_size = sec.getSize();

                if (VERBOSITY() >= 2)
                    printf("eh_frame: %lx %lx\n", eh_frame_addr, eh_frame_size);
                found_eh_frame = true;
            } else if (name == ".text") {
                assert(!found_text);
                text_addr = L.getSectionLoadAddress(name);
                text_size = sec.getSize();

                if (VERBOSITY() >= 2)
                    printf("text: %lx %lx\n", text_addr, text_size);
                found_text = true;
            }
        }

        assert(found_text);
        assert(found_eh_frame);
        assert(text_addr == func_addr);

        registerDynamicEhFrame(text_addr, text_size, eh_frame_addr, eh_frame_size);
    }
};

struct PythonFrameId {
    enum FrameType {
        COMPILED,
        INTERPRETED,
    } type;

    uint64_t ip;
    uint64_t bp;

    PythonFrameId() {}

    PythonFrameId(FrameType type, uint64_t ip, uint64_t bp) : type(type), ip(ip), bp(bp) {}

    bool operator==(const PythonFrameId& rhs) const { return (this->type == rhs.type) && (this->ip == rhs.ip); }
};

class PythonFrameIteratorImpl {
public:
    PythonFrameId id;
    FunctionMetadata* md; // always exists

    // These only exist if id.type==COMPILED:
    CompiledFunction* cf;
    // We have to save a copy of the regs since it's very difficult to keep the unw_context_t
    // structure valid.
    intptr_t regs[16];
    uint16_t regs_valid;

    PythonFrameIteratorImpl() : regs_valid(0) {}

    PythonFrameIteratorImpl(PythonFrameId::FrameType type, uint64_t ip, uint64_t bp, FunctionMetadata* md,
                            CompiledFunction* cf)
        : id(PythonFrameId(type, ip, bp)), md(md), cf(cf), regs_valid(0) {
        assert(md);
        assert((type == PythonFrameId::COMPILED) == (cf != NULL));
    }

    CompiledFunction* getCF() const {
        assert(cf);
        return cf;
    }

    FunctionMetadata* getMD() const {
        assert(md);
        return md;
    }

    uint64_t readLocation(const StackMap::Record::Location& loc) {
        assert(id.type == PythonFrameId::COMPILED);

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

    llvm::ArrayRef<StackMap::Record::Location> findLocations(llvm::StringRef name) {
        assert(id.type == PythonFrameId::COMPILED);

        CompiledFunction* cf = getCF();
        uint64_t ip = getId().ip;

        assert(ip > cf->code_start);
        unsigned offset = ip - cf->code_start;

        assert(cf->location_map);
        const LocationMap::LocationTable& table = cf->location_map->names[name];
        assert(table.locations.size());

        auto entry = table.findEntry(offset);
        if (!entry)
            return {};
        assert(entry->locations.size());
        return entry->locations;
    }

    AST_stmt* getCurrentStatement() {
        if (id.type == PythonFrameId::COMPILED) {
            auto locations = findLocations("!current_stmt");
            RELEASE_ASSERT(locations.size() == 1, "%ld", locations.size());
            return reinterpret_cast<AST_stmt*>(readLocation(locations[0]));
        } else if (id.type == PythonFrameId::INTERPRETED) {
            return getCurrentStatementForInterpretedFrame((void*)id.bp);
        }
        abort();
    }

    Box* getGlobals() {
        if (id.type == PythonFrameId::COMPILED) {
            CompiledFunction* cf = getCF();
            if (cf->md->source->scoping->areGlobalsFromModule())
                return cf->md->source->parent_module;
            auto locations = findLocations(PASSED_GLOBALS_NAME);
            assert(locations.size() == 1);
            Box* r = (Box*)readLocation(locations[0]);
            ASSERT(gc::isValidGCObject(r), "%p", r);
            return r;
        } else if (id.type == PythonFrameId::INTERPRETED) {
            return getGlobalsForInterpretedFrame((void*)id.bp);
        }
        abort();
    }

    Box* getGlobalsDict() {
        Box* globals = getGlobals();
        if (!globals)
            return NULL;

        if (PyModule_Check(globals))
            return globals->getAttrWrapper();
        return globals;
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

    uint64_t getReg(int dwarf_num) {
        // for x86_64, at least, libunwind seems to use the dwarf numbering

        assert(0 <= dwarf_num && dwarf_num < 16);
        assert(regs_valid & (1 << dwarf_num));
        assert(id.type == PythonFrameId::COMPILED);

        return regs[dwarf_num];
    }

    bool pointsToTheSameAs(const PythonFrameIteratorImpl& rhs) const {
        return this->id.type == rhs.id.type && this->id.bp == rhs.id.bp;
    }
};

static unw_word_t getFunctionEnd(unw_word_t ip) {
    unw_proc_info_t pip;
    int ret = unw_get_proc_info_by_ip(unw_local_addr_space, ip, &pip, NULL);
    RELEASE_ASSERT(ret == 0 && pip.end_ip, "");
    return pip.end_ip;
}

static bool inASTInterpreterExecuteInner(unw_word_t ip) {
    static unw_word_t interpreter_instr_end = getFunctionEnd((unw_word_t)interpreter_instr_addr);
    return ((unw_word_t)interpreter_instr_addr < ip && ip <= interpreter_instr_end);
}

static bool inGeneratorEntry(unw_word_t ip) {
    static unw_word_t generator_entry_end = getFunctionEnd((unw_word_t)generatorEntry);
    return ((unw_word_t)generatorEntry < ip && ip <= generator_entry_end);
}

static bool isDeopt(unw_word_t ip) {
    // Check for astInterpretDeopt() instead of deopt(), since deopt() will do some
    // unwinding and we don't want it to skip things.
    static unw_word_t deopt_end = getFunctionEnd((unw_word_t)astInterpretDeopt);
    return ((unw_word_t)astInterpretDeopt < ip && ip <= deopt_end);
}


static inline unw_word_t get_cursor_reg(unw_cursor_t* cursor, int reg) {
    unw_word_t v;
    unw_get_reg(cursor, reg, &v);
    return v;
}
static inline unw_word_t get_cursor_ip(unw_cursor_t* cursor) {
    return get_cursor_reg(cursor, UNW_REG_IP);
}
static inline unw_word_t get_cursor_bp(unw_cursor_t* cursor) {
    return get_cursor_reg(cursor, UNW_TDEP_BP);
}

// if the given ip/bp correspond to a jitted frame or
// ASTInterpreter::execute_inner frame, return true and return the
// frame information through the PythonFrameIteratorImpl* info arg.
bool frameIsPythonFrame(unw_word_t ip, unw_word_t bp, unw_cursor_t* cursor, PythonFrameIteratorImpl* info) {
    CompiledFunction* cf = getCFForAddress(ip);
    FunctionMetadata* md = cf ? cf->md : NULL;
    bool jitted = cf != NULL;
    bool interpreted = !jitted && inASTInterpreterExecuteInner(ip);
    if (interpreted)
        md = getMDForInterpretedFrame((void*)bp);

    if (!jitted && !interpreted)
        return false;

    *info = PythonFrameIteratorImpl(jitted ? PythonFrameId::COMPILED : PythonFrameId::INTERPRETED, ip, bp, md, cf);
    if (jitted) {
        // Try getting all the callee-save registers, and save the ones we were able to get.
        // Some of them may be inaccessible, I think because they weren't defined by that
        // stack frame, which can show up as a -UNW_EBADREG return code.
        for (int i = 0; i < 16; i++) {
            if (!assembler::Register::fromDwarf(i).isCalleeSave())
                continue;
            unw_word_t r;
            int code = unw_get_reg(cursor, i, &r);
            ASSERT(code == 0 || code == -UNW_EBADREG, "%d %d", code, i);
            if (code == 0) {
                info->regs[i] = r;
                info->regs_valid |= (1 << i);
            }
        }
    }

    return true;
}

static const LineInfo lineInfoForFrame(PythonFrameIteratorImpl* frame_it) {
    AST_stmt* current_stmt = frame_it->getCurrentStatement();
    auto* md = frame_it->getMD();
    assert(md);

    auto source = md->source.get();

    return LineInfo(current_stmt->lineno, current_stmt->col_offset, source->getFn(), source->getName());
}

// A class that converts a C stack trace to a Python stack trace.
// It allows for different ways of driving the C stack trace; it just needs
// to have handleCFrame called once per frame.
// If you want to do a normal (non-destructive) stack walk, use unwindPythonStack
// which will use this internally.
class PythonStackExtractor {
private:
    bool skip_next_pythonlike_frame = false;

public:
    bool handleCFrame(unw_cursor_t* cursor, PythonFrameIteratorImpl* frame_iter) {
        unw_word_t ip = get_cursor_ip(cursor);
        unw_word_t bp = get_cursor_bp(cursor);

        bool rtn = false;

        if (isDeopt(ip)) {
            assert(!skip_next_pythonlike_frame);
            skip_next_pythonlike_frame = true;
        } else if (frameIsPythonFrame(ip, bp, cursor, frame_iter)) {
            if (!skip_next_pythonlike_frame)
                rtn = true;

            // frame_iter->cf->entry_descriptor will be non-null for OSR frames.
            bool was_osr = (frame_iter->getId().type == PythonFrameId::COMPILED) && (frame_iter->cf->entry_descriptor);
            skip_next_pythonlike_frame = was_osr;
        }
        return rtn;
    }
};

class PythonUnwindSession : public Box {
    ExcInfo exc_info;
    PythonStackExtractor pystack_extractor;
    Box* prev_frame;

    Timer t;

public:
    DEFAULT_CLASS_SIMPLE(unwind_session_cls);

    PythonUnwindSession() : exc_info(NULL, NULL, NULL), prev_frame(NULL), t(/*min_usec=*/10000) {}

    ExcInfo* getExcInfoStorage() { return &exc_info; }

    void begin() {
        prev_frame = NULL;
        exc_info = ExcInfo(NULL, NULL, NULL);
        t.restart();

        static StatCounter stat("unwind_sessions");
        stat.log();
    }
    void end() {
        prev_frame = NULL;
        static StatCounter stat("us_unwind_session");
        stat.log(t.end());
    }

    void handleCFrame(unw_cursor_t* cursor) {
        PythonFrameIteratorImpl frame_iter;

        if (prev_frame) {
            deinitFrame((BoxedFrame*)prev_frame);
            prev_frame = NULL;
        }

        bool found_frame = pystack_extractor.handleCFrame(cursor, &frame_iter);
        if (found_frame) {
            frame_iter.getMD()->propagated_cxx_exceptions++;

            if (exceptionAtLineCheck()) {
                // TODO: shouldn't fetch this multiple times?
                frame_iter.getCurrentStatement()->cxx_exception_count++;
                auto line_info = lineInfoForFrame(&frame_iter);
                prev_frame = getFrame(std::unique_ptr<PythonFrameIteratorImpl>(new PythonFrameIteratorImpl(frame_iter)),
                                      false);
                exceptionAtLine(line_info, &exc_info.traceback, prev_frame);
            }
        }
    }

    static void gcHandler(GCVisitor* v, Box* _o) {
        assert(_o->cls == unwind_session_cls);

        PythonUnwindSession* o = static_cast<PythonUnwindSession*>(_o);

        if (o->exc_info.type)
            v->visit(&o->exc_info.type);
        if (o->exc_info.value)
            v->visit(&o->exc_info.value);
        if (o->exc_info.traceback)
            v->visit(&o->exc_info.traceback);
    }
};
static __thread PythonUnwindSession* cur_unwind;

static PythonUnwindSession* getUnwindSession() {
    if (!cur_unwind) {
        cur_unwind = new PythonUnwindSession();
        pyston::gc::registerPermanentRoot(cur_unwind);
    }
    return cur_unwind;
}

PythonUnwindSession* beginPythonUnwindSession() {
    getUnwindSession();
    cur_unwind->begin();
    return cur_unwind;
}

PythonUnwindSession* getActivePythonUnwindSession() {
    ASSERT(cur_unwind, "");
    return cur_unwind;
}

void endPythonUnwindSession(PythonUnwindSession* unwind) {
    RELEASE_ASSERT(unwind && unwind == cur_unwind, "");
    unwind->end();
}

void* getPythonUnwindSessionExceptionStorage(PythonUnwindSession* unwind) {
    RELEASE_ASSERT(unwind && unwind == cur_unwind, "");
    PythonUnwindSession* state = static_cast<PythonUnwindSession*>(unwind);
    return state->getExcInfoStorage();
}

void unwindingThroughFrame(PythonUnwindSession* unwind_session, unw_cursor_t* cursor) {
    unwind_session->handleCFrame(cursor);
}

// While I'm not a huge fan of the callback-passing style, libunwind cursors are only valid for
// the stack frame that they were created in, so we need to use this approach (as opposed to
// C++11 range loops, for example).
// Return true from the handler to stop iteration at that frame.
template <typename Func> void unwindPythonStack(Func func) {
    unw_context_t ctx;
    unw_cursor_t cursor;
    unw_getcontext(&ctx);
    unw_init_local(&cursor, &ctx);

    PythonStackExtractor pystack_extractor;

    while (true) {
        int r = unw_step(&cursor);

        assert(r >= 0);
        if (r == 0)
            break;

        PythonFrameIteratorImpl frame_iter;
        bool found_frame = pystack_extractor.handleCFrame(&cursor, &frame_iter);

        if (found_frame) {
            bool stop_unwinding = func(&frame_iter);
            if (stop_unwinding)
                break;
        }

        unw_word_t ip = get_cursor_ip(&cursor);
        if (inGeneratorEntry(ip)) {
            unw_word_t bp = get_cursor_bp(&cursor);

            // for generators continue unwinding in the context in which the generator got called
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

static std::unique_ptr<PythonFrameIteratorImpl> getTopPythonFrame() {
    STAT_TIMER(t0, "us_timer_getTopPythonFrame", 10);
    std::unique_ptr<PythonFrameIteratorImpl> rtn(nullptr);
    unwindPythonStack([&](PythonFrameIteratorImpl* iter) {
        rtn = std::unique_ptr<PythonFrameIteratorImpl>(new PythonFrameIteratorImpl(*iter));
        return true;
    });
    return rtn;
}

// To produce a traceback, we:
//
// 1. Use libunwind to produce a cursor into our stack.
//
// 2. Grab the next frame in the stack and check what function it is from. There are four options:
//
//    (a) A JIT-compiled Python function.
//    (b) ASTInterpreter::execute() in codegen/ast_interpreter.cpp.
//    (c) generatorEntry() in runtime/generator.cpp.
//    (d) Something else.
//
//    By cases:
//
//    (2a, 2b) If the previous frame we visited was an OSR frame (which we know from its CompiledFunction*), then we
//    skip this frame (it's the frame we replaced on-stack) and keep unwinding. (FIXME: Why are we guaranteed that we
//    on-stack-replaced at most one frame?) Otherwise, we found a frame for our traceback! Proceed to step 3.
//
//    (2c) Continue unwinding in the stack of whatever called the generator. This involves some hairy munging of
//    undocumented fields in libunwind structs to swap the context.
//
//    (2d) Ignore it and keep unwinding. It's some C or C++ function that we don't want in our traceback.
//
// 3. We've found a frame for our traceback, along with a CompiledFunction* and some other information about it.
//
//    We grab the current statement it is in (as an AST_stmt*) and use it and the CompiledFunction*'s source info to
//    produce the line information for the traceback. For JIT-compiled functions, getting the statement involves the
//    CF's location_map.
//
// 4. Unless we've hit the end of the stack, go to 2 and keep unwinding.
//
static StatCounter us_gettraceback("us_gettraceback");
Box* getTraceback() {
    STAT_TIMER(t0, "us_timer_gettraceback", 20);
    if (!ENABLE_FRAME_INTROSPECTION) {
        static bool printed_warning = false;
        if (!printed_warning) {
            printed_warning = true;
            fprintf(stderr, "Warning: can't get traceback since ENABLE_FRAME_INTROSPECTION=0\n");
        }
        return None;
    }

    if (!ENABLE_TRACEBACKS) {
        static bool printed_warning = false;
        if (!printed_warning) {
            printed_warning = true;
            fprintf(stderr, "Warning: can't get traceback since ENABLE_TRACEBACKS=0\n");
        }
        return None;
    }

    Timer _t("getTraceback", 1000);

    Box* tb = None;
    unwindPythonStack([&](PythonFrameIteratorImpl* frame_iter) {
        Box* frame
            = getFrame(std::unique_ptr<PythonFrameIteratorImpl>(new PythonFrameIteratorImpl(*frame_iter)), false);
        BoxedTraceback::here(lineInfoForFrame(frame_iter), &tb, frame);
        return false;
    });

    long us = _t.end();
    us_gettraceback.log(us);

    return static_cast<BoxedTraceback*>(tb);
}

ExcInfo* getFrameExcInfo() {
    std::vector<ExcInfo*> to_update;
    ExcInfo* copy_from_exc = NULL;
    ExcInfo* cur_exc = NULL;

    unwindPythonStack([&](PythonFrameIteratorImpl* frame_iter) {
        FrameInfo* frame_info = frame_iter->getFrameInfo();

        copy_from_exc = &frame_info->exc;
        if (!cur_exc)
            cur_exc = copy_from_exc;

        if (!copy_from_exc->type) {
            to_update.push_back(copy_from_exc);
            return false;
        }

        return true;
    });

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

void updateFrameExcInfoIfNeeded(ExcInfo* latest) {
    if (latest->type)
        return;

    ExcInfo* updated = getFrameExcInfo();
    assert(updated == latest);
    return;
}

FunctionMetadata* getTopPythonFunction() {
    auto rtn = getTopPythonFrame();
    if (!rtn)
        return NULL;
    return getTopPythonFrame()->getMD();
}

Box* getGlobals() {
    auto it = getTopPythonFrame();
    if (!it)
        return NULL;
    return it->getGlobals();
}

Box* getGlobalsDict() {
    return getTopPythonFrame()->getGlobalsDict();
}

BoxedModule* getCurrentModule() {
    FunctionMetadata* md = getTopPythonFunction();
    if (!md)
        return NULL;
    return md->source->parent_module;
}

PythonFrameIterator getPythonFrame(int depth) {
    std::unique_ptr<PythonFrameIteratorImpl> rtn(nullptr);
    unwindPythonStack([&](PythonFrameIteratorImpl* frame_iter) {
        if (depth == 0) {
            rtn = std::unique_ptr<PythonFrameIteratorImpl>(new PythonFrameIteratorImpl(*frame_iter));
            return true;
        }
        depth--;
        return false;
    });
    return PythonFrameIterator(std::move(rtn));
}

PythonFrameIterator::~PythonFrameIterator() {
}

PythonFrameIterator::PythonFrameIterator(PythonFrameIterator&& rhs) {
    std::swap(this->impl, rhs.impl);
}

void PythonFrameIterator::operator=(PythonFrameIterator&& rhs) {
    std::swap(this->impl, rhs.impl);
}

PythonFrameIterator::PythonFrameIterator(std::unique_ptr<PythonFrameIteratorImpl> impl) {
    std::swap(this->impl, impl);
}


// TODO factor getDeoptState and fastLocalsToBoxedLocals
// because they are pretty ugly but have a pretty repetitive pattern.

DeoptState getDeoptState() {
    DeoptState rtn;
    bool found = false;
    unwindPythonStack([&](PythonFrameIteratorImpl* frame_iter) {
        BoxedDict* d;
        BoxedClosure* closure;
        CompiledFunction* cf;
        if (frame_iter->getId().type == PythonFrameId::COMPILED) {
            d = new BoxedDict();

            cf = frame_iter->getCF();
            uint64_t ip = frame_iter->getId().ip;

            assert(ip > cf->code_start);
            unsigned offset = ip - cf->code_start;

            assert(cf->location_map);

            // We have to detect + ignore any entries for variables that
            // could have been defined (so they have entries) but aren't (so the
            // entries point to uninitialized memory).
            std::unordered_set<std::string> is_undefined;

            for (const auto& p : cf->location_map->names) {
                if (!startswith(p.first(), "!is_defined_"))
                    continue;

                auto e = p.second.findEntry(offset);
                if (e) {
                    auto locs = e->locations;

                    assert(locs.size() == 1);
                    uint64_t v = frame_iter->readLocation(locs[0]);
                    if ((v & 1) == 0)
                        is_undefined.insert(p.first().substr(12));
                }
            }

            for (const auto& p : cf->location_map->names) {
                if (p.first()[0] == '!')
                    continue;

                if (is_undefined.count(p.first()))
                    continue;

                auto e = p.second.findEntry(offset);
                if (e) {
                    auto locs = e->locations;

                    llvm::SmallVector<uint64_t, 1> vals;
                    // printf("%s: %s\n", p.first().c_str(), e.type->debugName().c_str());

                    for (auto& loc : locs) {
                        vals.push_back(frame_iter->readLocation(loc));
                    }

                    Box* v = e->type->deserializeFromFrame(vals);
                    // printf("%s: (pp id %ld) %p\n", p.first().c_str(), e._debug_pp_id, v);
                    ASSERT(gc::isValidGCObject(v), "%p", v);
                    d->d[boxString(p.first())] = v;
                }
            }
        } else {
            abort();
        }

        rtn.frame_state = FrameStackState(d, frame_iter->getFrameInfo());
        rtn.cf = cf;
        rtn.current_stmt = frame_iter->getCurrentStatement();
        found = true;
        return true;
    });

    RELEASE_ASSERT(found, "Internal error: unable to find any python frames");
    return rtn;
}

Box* fastLocalsToBoxedLocals() {
    return getPythonFrame(0).fastLocalsToBoxedLocals();
}

Box* PythonFrameIterator::fastLocalsToBoxedLocals() {
    assert(impl.get());

    BoxedDict* d;
    BoxedClosure* closure;
    FrameInfo* frame_info;

    FunctionMetadata* md = impl->getMD();
    ScopeInfo* scope_info = md->source->getScopeInfo();

    if (scope_info->areLocalsFromModule()) {
        // TODO we should cache this in frame_info->locals or something so that locals()
        // (and globals() too) will always return the same dict
        RELEASE_ASSERT(md->source->scoping->areGlobalsFromModule(), "");
        return md->source->parent_module->getAttrWrapper();
    }

    if (impl->getId().type == PythonFrameId::COMPILED) {
        CompiledFunction* cf = impl->getCF();
        uint64_t ip = impl->getId().ip;

        assert(ip > cf->code_start);
        unsigned offset = ip - cf->code_start;

        frame_info = impl->getFrameInfo();
        d = localsForInterpretedFrame(frame_info->vregs, cf->md->source->cfg, true);
        closure = NULL;
        if (cf->location_map->names.count(PASSED_CLOSURE_NAME) > 0) {
            auto e = cf->location_map->names[PASSED_CLOSURE_NAME].findEntry(offset);
            if (e) {
                const auto& locs = e->locations;

                llvm::SmallVector<uint64_t, 1> vals;

                for (auto& loc : locs) {
                    vals.push_back(impl->readLocation(loc));
                }

                Box* v = e->type->deserializeFromFrame(vals);
                assert(gc::isValidGCObject(v));
                closure = static_cast<BoxedClosure*>(v);
            }
        }
    } else if (impl->getId().type == PythonFrameId::INTERPRETED) {
        d = localsForInterpretedFrame((void*)impl->getId().bp, true);
        closure = passedClosureForInterpretedFrame((void*)impl->getId().bp);
        frame_info = getFrameInfoForInterpretedFrame((void*)impl->getId().bp);
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
        Box* boxedName = name.getBox();
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
    if (frame_info->boxedLocals == dict_cls) {
        ((BoxedDict*)frame_info->boxedLocals)->d.insert(d->d.begin(), d->d.end());
    } else {
        for (const auto& p : *d) {
            Box* varname = p.first;
            Box* value = p.second;
            setitem(frame_info->boxedLocals, varname, value);
        }
    }

    return frame_info->boxedLocals;
}

AST_stmt* PythonFrameIterator::getCurrentStatement() {
    return impl->getCurrentStatement();
}

CompiledFunction* PythonFrameIterator::getCF() {
    return impl->getCF();
}

FunctionMetadata* PythonFrameIterator::getMD() {
    return impl->getMD();
}

Box* PythonFrameIterator::getGlobalsDict() {
    return impl->getGlobalsDict();
}

FrameInfo* PythonFrameIterator::getFrameInfo() {
    return impl->getFrameInfo();
}

PythonFrameIterator PythonFrameIterator::getCurrentVersion() {
    std::unique_ptr<PythonFrameIteratorImpl> rtn(nullptr);
    auto& impl = this->impl;
    unwindPythonStack([&](PythonFrameIteratorImpl* frame_iter) {
        if (frame_iter->pointsToTheSameAs(*impl.get())) {
            rtn = std::unique_ptr<PythonFrameIteratorImpl>(new PythonFrameIteratorImpl(*frame_iter));
            return true;
        }
        return false;
    });
    return PythonFrameIterator(std::move(rtn));
}

PythonFrameIterator PythonFrameIterator::back() {
    // TODO this is ineffecient: the iterator is no longer valid for libunwind iteration, so
    // we have to do a full stack crawl again.
    // Hopefully examination of f_back is uncommon.

    std::unique_ptr<PythonFrameIteratorImpl> rtn(nullptr);
    auto& impl = this->impl;
    bool found = false;
    unwindPythonStack([&](PythonFrameIteratorImpl* frame_iter) {
        if (found) {
            rtn = std::unique_ptr<PythonFrameIteratorImpl>(new PythonFrameIteratorImpl(*frame_iter));
            return true;
        }

        if (frame_iter->pointsToTheSameAs(*impl.get()))
            found = true;
        return false;
    });

    RELEASE_ASSERT(found, "this wasn't a valid frame?");
    return PythonFrameIterator(std::move(rtn));
}

std::string getCurrentPythonLine() {
    auto frame_iter = getTopPythonFrame();

    if (frame_iter.get()) {
        std::ostringstream stream;

        auto* md = frame_iter->getMD();
        auto source = md->source.get();

        auto current_stmt = frame_iter->getCurrentStatement();

        stream << source->getFn()->c_str() << ":" << current_stmt->lineno;
        return stream.str();
    }
    return "unknown:-1";
}

void logByCurrentPythonLine(const std::string& stat_name) {
    std::string stat = stat_name + "<" + getCurrentPythonLine() + ">";
    Stats::log(Stats::getStatCounter(stat));
}

void _printStacktrace() {
    static bool recursive = false;

    if (recursive) {
        fprintf(stderr, "_printStacktrace ran into an issue; refusing to try it again!\n");
        return;
    }

    recursive = true;
    printTraceback(getTraceback());
    recursive = false;
}

extern "C" void abort() {
    static void (*libc_abort)() = (void (*)())dlsym(RTLD_NEXT, "abort");

    // In case displaying the traceback recursively calls abort:
    static bool recursive = false;

    if (!recursive) {
        recursive = true;
        Stats::dump();
        fprintf(stderr, "Someone called abort!\n");

        // If traceback_cls is NULL, then we somehow died early on, and won't be able to display a traceback.
        if (traceback_cls) {

            // If we call abort(), things may be seriously wrong.  Set an alarm() to
            // try to handle cases that we would just hang.
            // (Ex if we abort() from a static constructor, and _printStackTrace uses
            // that object, _printStackTrace will hang waiting for the first construction
            // to finish.)
            alarm(1);
            try {
                _printStacktrace();
            } catch (ExcInfo) {
                fprintf(stderr, "error printing stack trace during abort()");
            }

            // Cancel the alarm.
            // This is helpful for when running in a debugger, since otherwise the debugger will catch the
            // abort and let you investigate, but the alarm will still come back to kill the program.
            alarm(0);
        }
    }

    if (PAUSE_AT_ABORT) {
        fprintf(stderr, "PID %d about to call libc abort; pausing for a debugger...\n", getpid());

        // Sometimes stderr isn't available (or doesn't immediately appear), so write out a file
        // just in case:
        FILE* f = fopen("pausing.txt", "w");
        if (f) {
            fprintf(f, "PID %d about to call libc abort; pausing for a debugger...\n", getpid());
            fclose(f);
        }

        while (true) {
            sleep(1);
        }
    }
    libc_abort();
    __builtin_unreachable();
}

#if 0
extern "C" void exit(int code) {
    static void (*libc_exit)(int) = (void (*)(int))dlsym(RTLD_NEXT, "exit");

    if (code == 0) {
        libc_exit(0);
        __builtin_unreachable();
    }

    fprintf(stderr, "Someone called exit with code=%d!\n", code);

    // In case something calls exit down the line:
    static bool recursive = false;
    if (!recursive) {
        recursive = true;

        _printStacktrace();
    }

    libc_exit(code);
    __builtin_unreachable();
}
#endif

llvm::JITEventListener* makeTracebacksListener() {
    return new TracebacksEventListener();
}

void setupUnwinding() {
    unwind_session_cls = BoxedClass::create(type_cls, object_cls, PythonUnwindSession::gcHandler, 0, 0,
                                            sizeof(PythonUnwindSession), false, "unwind_session");
    unwind_session_cls->freeze();
}
}
