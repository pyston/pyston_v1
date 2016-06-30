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

#include "codegen/baseline_jit.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <sys/mman.h>

#include "codegen/irgen/hooks.h"
#include "codegen/memmgr.h"
#include "codegen/type_recording.h"
#include "core/cfg.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

static llvm::DenseSet<CFGBlock*> blocks_aborted;
static llvm::DenseMap<CFGBlock*, std::vector<void*>> block_patch_locations;

// The EH table is copied from the one clang++ generated for:
//
// long foo(char* c);
// void bjit() {
//   asm volatile ("" ::: "r15");
//   asm volatile ("" ::: "r14");
//   asm volatile ("" ::: "r13");
//   asm volatile ("" ::: "r12");
//   char scratch[256+16];
//   foo(scratch);
// }
//
// It omits the frame pointer but saves r12, r13, r14 and r15
// use 'objdump -s -j .eh_frame <obj.file>' to dump it
const unsigned char eh_info[]
    = { 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7a, 0x52, 0x00, 0x01, 0x78, 0x10, 0x01,
        0x1b, 0x0c, 0x07, 0x08, 0x90, 0x01, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x42, 0x0e, 0x10, 0x42, 0x0e, 0x18, 0x42,
        0x0e, 0x20, 0x42, 0x0e, 0x28, 0x47, 0x0e, 0xc0, 0x02, 0x8c, 0x05, 0x8d, 0x04, 0x8e, 0x03, 0x8f,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static_assert(JitCodeBlock::num_stack_args == 2, "have to update EH table!");
static_assert(JitCodeBlock::scratch_size == 256, "have to update EH table!");

constexpr int code_size = JitCodeBlock::memory_size - sizeof(eh_info);

JitCodeBlock::MemoryManager::MemoryManager() {
    int protection = PROT_READ | PROT_WRITE | PROT_EXEC;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if ENABLE_BASELINEJIT_MAP_32BIT
    flags |= MAP_32BIT;
#endif
    addr = (uint8_t*)mmap(NULL, JitCodeBlock::memory_size, protection, flags, -1, 0);
}

JitCodeBlock::MemoryManager::~MemoryManager() {
    munmap(addr, JitCodeBlock::memory_size);
    addr = NULL;

    RELEASE_ASSERT(0, "we have to unregister this block from g.func_addr_registry");
}

JitCodeBlock::JitCodeBlock(llvm::StringRef name)
    : entry_offset(0), a(memory.get() + sizeof(eh_info), code_size), is_currently_writing(false), asm_failed(false) {
    static StatCounter num_jit_code_blocks("num_baselinejit_code_blocks");
    num_jit_code_blocks.log();
    static StatCounter num_jit_total_bytes("num_baselinejit_total_bytes");
    num_jit_total_bytes.log(memory_size);

    uint8_t* code = a.curInstPointer();

    // emit prolog
    a.push(assembler::R15);
    a.push(assembler::R14);
    a.push(assembler::R13);
    a.push(assembler::R12);
    static_assert(sp_adjustment % 16 == 8, "stack isn't aligned");
    a.sub(assembler::Immediate(sp_adjustment), assembler::RSP);
    a.mov(assembler::RDI, assembler::R13);                                // interpreter pointer
    a.mov(assembler::RDX, assembler::R14);                                // vreg array
    a.jmp(assembler::Indirect(assembler::RSI, offsetof(CFGBlock, code))); // jump to block

    entry_offset = a.bytesWritten();

    // generate the eh frame...
    const int size = sizeof(eh_info);
    void* eh_frame_addr = memory.get();
    memcpy(eh_frame_addr, eh_info, size);

    int32_t* offset_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x20);
    int32_t* size_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x24);
    int64_t offset = (int8_t*)code - (int8_t*)offset_ptr;
    assert(offset >= INT_MIN && offset <= INT_MAX);
    *offset_ptr = offset;
    *size_ptr = code_size;

    registerDynamicEhFrame((uint64_t)code, code_size, (uint64_t)eh_frame_addr, size - 4);
    registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, size);

    static int num_block = 0;
    auto unique_name = ("bjit_" + name + "_" + llvm::Twine(num_block++)).str();
    g.func_addr_registry.registerFunction(unique_name, code, code_size, NULL);
}

std::unique_ptr<JitFragmentWriter> JitCodeBlock::newFragment(CFGBlock* block, int patch_jump_offset) {
    if (is_currently_writing || blocks_aborted.count(block))
        return std::unique_ptr<JitFragmentWriter>();

    is_currently_writing = true;

    int scratch_offset = num_stack_args * 8;
    StackInfo stack_info(scratch_size, scratch_offset);
    LiveOutSet live_outs;

    void* fragment_start = a.curInstPointer() - patch_jump_offset;
    long fragment_offset = a.bytesWritten() - patch_jump_offset;
    long bytes_left = a.bytesLeft() + patch_jump_offset;
    std::unique_ptr<ICInfo> ic_info(new ICInfo(fragment_start, nullptr, nullptr, stack_info, bytes_left,
                                               llvm::CallingConv::C, live_outs, assembler::RAX, 0,
                                               std::vector<Location>()));
    std::unique_ptr<ICSlotRewrite> rewrite = ic_info->startRewrite("");

    return std::unique_ptr<JitFragmentWriter>(new JitFragmentWriter(
        block, std::move(ic_info), std::move(rewrite), fragment_offset, patch_jump_offset, a.getStartAddr(), *this));
}

void JitCodeBlock::fragmentAbort(bool not_enough_space) {
    asm_failed = not_enough_space;
    is_currently_writing = false;
}

void JitCodeBlock::fragmentFinished(int bytes_written, int num_bytes_overlapping, void* next_fragment_start,
                                    ICInfo& ic_info) {
    assert(next_fragment_start == bytes_written + a.curInstPointer() - num_bytes_overlapping);
    a.setCurInstPointer((uint8_t*)next_fragment_start);

    asm_failed = false;
    is_currently_writing = false;

    ic_info.appendDecrefInfosTo(decref_infos);
}

static const assembler::Register bjit_allocatable_regs[]
    = { assembler::RAX, assembler::RCX, assembler::RDX,
        // no RSP
        // no RBP
        assembler::RDI, assembler::RSI, assembler::R8,  assembler::R9,
        assembler::R10, assembler::R11, assembler::R12, assembler::R15 };

JitFragmentWriter::JitFragmentWriter(CFGBlock* block, std::unique_ptr<ICInfo> ic_info,
                                     std::unique_ptr<ICSlotRewrite> rewrite, int code_offset, int num_bytes_overlapping,
                                     void* entry_code, JitCodeBlock& code_block)
    : Rewriter(std::move(rewrite), 0, {}, /* needs_invalidation_support = */ false),
      block(block),
      code_offset(code_offset),
      exit_info(),
      num_bytes_overlapping(num_bytes_overlapping),
      entry_code(entry_code),
      code_block(code_block),
      interp(0),
      ic_info(std::move(ic_info)) {
    allocatable_regs = bjit_allocatable_regs;

    added_changing_action = true;

    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: JitFragmentWriter() start");
    interp = createNewVar();
    addLocationToVar(interp, assembler::R13);
    interp->setAttr(ASTInterpreterJitInterface::getCurrentBlockOffset(), imm(block));

    vregs_array = createNewVar();
    addLocationToVar(vregs_array, assembler::R14);
    addAction([=]() { vregs_array->bumpUse(); }, vregs_array, ActionType::NORMAL);
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: JitFragmentWriter() end");
}

RewriterVar* JitFragmentWriter::getInterp() {
    return interp;
}

RewriterVar* JitFragmentWriter::imm(uint64_t val) {
    return loadConst(val);
}

RewriterVar* JitFragmentWriter::imm(void* val) {
    return loadConst((uint64_t)val);
}

RewriterVar* JitFragmentWriter::emitAugbinop(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type) {
    return emitPPCall((void*)augbinop, { lhs, rhs, imm(op_type) }, 2 * 320, node).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitApplySlice(RewriterVar* target, RewriterVar* lower, RewriterVar* upper) {
    if (!lower)
        lower = imm(0ul);
    if (!upper)
        upper = imm(0ul);
    return emitPPCall((void*)applySlice, { target, lower, upper }, 256).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitBinop(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type) {
    return emitPPCall((void*)binop, { lhs, rhs, imm(op_type) }, 2 * 240, node).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitCallattr(AST_expr* node, RewriterVar* obj, BoxedString* attr, CallattrFlags flags,
                                             const llvm::ArrayRef<RewriterVar*> args,
                                             std::vector<BoxedString*>* keyword_names) {
    TypeRecorder* type_recorder = getTypeRecorderForNode(node);

#if ENABLE_BASELINEJIT_ICS
    RewriterVar* attr_var = imm(attr);
    RewriterVar* flags_var = imm(flags.asInt());
    RewriterVar::SmallVector call_args;
    call_args.push_back(obj);
    call_args.push_back(attr_var);
    call_args.push_back(flags_var);
    call_args.push_back(args.size() > 0 ? args[0] : imm(0ul));
    call_args.push_back(args.size() > 1 ? args[1] : imm(0ul));
    call_args.push_back(args.size() > 2 ? args[2] : imm(0ul));

    llvm::ArrayRef<RewriterVar*> additional_uses;
    if (args.size() > 3) {
        additional_uses = args.slice(3);
        RewriterVar* scratch = allocArgs(additional_uses, RewriterVar::SetattrType::REF_USED);
        call_args.push_back(scratch);
    } else if (keyword_names) {
        call_args.push_back(imm(0ul));
    }

    if (keyword_names)
        call_args.push_back(imm(keyword_names));

    return emitPPCall((void*)callattr, call_args, 2 * 640, node, type_recorder, additional_uses)
        .first->setType(RefType::OWNED);
#else
    // We could make this faster but for now: keep it simple, stupid...
    RewriterVar* attr_var = imm(attr);
    RewriterVar* flags_var = imm(flags.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size())
        args_array = allocArgs(args, RewriterVar::SetattrType::REF_USED);
    else
        RELEASE_ASSERT(!keyword_names_var, "0 args but keyword names are set");

    RewriterVar::SmallVector call_args;
    call_args.push_back(obj);
    call_args.push_back(attr_var);
    call_args.push_back(flags_var);

    call_args.push_back(imm(type_recorder));

    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

    return emitCallWithAllocatedArgs((void*)callattrHelper, call_args, args)->setType(RefType::OWNED);
#endif
}

RewriterVar* JitFragmentWriter::emitCompare(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type) {
    if (op_type == AST_TYPE::Is || op_type == AST_TYPE::IsNot) {
        RewriterVar* cmp_result = lhs->cmp(op_type == AST_TYPE::IsNot ? AST_TYPE::NotEq : AST_TYPE::Eq, rhs);
        return call(false, (void*)boxBool, cmp_result)->setType(RefType::OWNED);
    }
    return emitPPCall((void*)compare, { lhs, rhs, imm(op_type) }, 2 * 240, node).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitCreateDict() {
    return call(false, (void*)createDict)->setType(RefType::OWNED);
}

void JitFragmentWriter::emitDictSet(RewriterVar* dict, RewriterVar* k, RewriterVar* v) {
    call(true, (void*)dictSetInternal, dict, k, v);
    k->refConsumed();
    v->refConsumed();
}

RewriterVar* JitFragmentWriter::emitCallWithAllocatedArgs(void* func_addr, const llvm::ArrayRef<RewriterVar*> args,
                                                          const llvm::ArrayRef<RewriterVar*> additional_uses) {
    return call(false, func_addr, args, {}, additional_uses);
}

RewriterVar* JitFragmentWriter::emitCreateList(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)createList)->setType(RefType::OWNED);

    auto rtn = emitCallWithAllocatedArgs((void*)createListHelper,
                                         { imm(num), allocArgs(values, RewriterVar::SetattrType::REF_USED) },
                                         values)->setType(RefType::OWNED);
    for (RewriterVar* v : values) {
        v->refConsumed();
    }
    return rtn;
}

RewriterVar* JitFragmentWriter::emitCreateSet(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)createSet)->setType(RefType::OWNED);
    auto rtn = emitCallWithAllocatedArgs((void*)createSetHelper,
                                         { imm(num), allocArgs(values, RewriterVar::SetattrType::REF_USED) },
                                         values)->setType(RefType::OWNED);
    for (RewriterVar* v : values) {
        v->refConsumed();
    }
    return rtn;
}

RewriterVar* JitFragmentWriter::emitCreateSlice(RewriterVar* start, RewriterVar* stop, RewriterVar* step) {
    return call(false, (void*)createSlice, start, stop, step)->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitCreateTuple(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    RewriterVar* r;
    if (num == 0) {
        r = imm(EmptyTuple)->setType(RefType::BORROWED);
    } else if (num == 1)
        r = call(false, (void*)BoxedTuple::create1, values[0])->setType(RefType::OWNED);
    else if (num == 2)
        r = call(false, (void*)BoxedTuple::create2, values[0], values[1])->setType(RefType::OWNED);
    else if (num == 3)
        r = call(false, (void*)BoxedTuple::create3, values[0], values[1], values[2])->setType(RefType::OWNED);
    else {
        r = emitCallWithAllocatedArgs((void*)createTupleHelper,
                                      { imm(num), allocArgs(values, RewriterVar::SetattrType::REF_USED) },
                                      values)->setType(RefType::OWNED);
    }
    return r;
}

RewriterVar* JitFragmentWriter::emitDeref(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::derefHelper, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second))
#else
                imm(asUInt(s)))
#endif
        ->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitExceptionMatches(RewriterVar* v, RewriterVar* cls) {
    return call(false, (void*)exceptionMatchesHelper, v, cls)->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitGetAttr(RewriterVar* obj, BoxedString* s, AST_expr* node) {
    return emitPPCall((void*)getattr, { obj, imm(s) }, 2 * 256, node, getTypeRecorderForNode(node))
        .first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitGetBlockLocal(InternedString s, int vreg) {
    auto it = local_syms.find(s);
    if (it == local_syms.end()) {
        auto r = emitGetLocal(s, vreg);
        assert(r->reftype == RefType::OWNED);
        emitSetBlockLocal(s, vreg, r);
        return r;
    }
    return it->second;
}

void JitFragmentWriter::emitKillTemporary(InternedString s, int vreg) {
    if (!local_syms.count(s))
        emitSetLocal(s, vreg, false, imm(nullptr));
}

RewriterVar* JitFragmentWriter::emitGetBoxedLocal(BoxedString* s) {
    RewriterVar* boxed_locals = emitGetBoxedLocals();
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    return call(false, (void*)boxedLocalsGet, boxed_locals, imm(s), globals)->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitGetBoxedLocals() {
    return getInterp()->getAttr(ASTInterpreterJitInterface::getBoxedLocalsOffset())->setType(RefType::BORROWED);
}

RewriterVar* JitFragmentWriter::emitGetClsAttr(RewriterVar* obj, BoxedString* s) {
    return emitPPCall((void*)getclsattr, { obj, imm(s) }, 2 * 256).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitGetGlobal(BoxedString* s) {
    if (s->s() == "None") {
        RewriterVar* r = imm(None)->setType(RefType::BORROWED);
        return r;
    }

    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    return emitPPCall((void*)getGlobal, { globals, imm(s) }, 128).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitGetItem(AST_expr* node, RewriterVar* value, RewriterVar* slice) {
    return emitPPCall((void*)getitem, { value, slice }, 256, node).first->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitGetLocal(InternedString s, int vreg) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitGetLocal start");
    assert(vreg >= 0);
    // TODO Can we use BORROWED here? Not sure if there are cases when we can't rely on borrowing the ref
    // from the vregs array.  Safer like this.
    RewriterVar* val_var = vregs_array->getAttr(vreg * 8);
    if (known_non_null_vregs.count(vreg) == 0) {
        addAction([=]() { _emitGetLocal(val_var, s.c_str()); }, { val_var }, ActionType::NORMAL);
        known_non_null_vregs.insert(vreg);
    } else {
        val_var->incref();
    }
    val_var->setType(RefType::OWNED);
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitGetLocal end");
    return val_var;
}

RewriterVar* JitFragmentWriter::emitGetPystonIter(RewriterVar* v) {
    return call(false, (void*)getPystonIter, v)->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitHasnext(RewriterVar* v) {
    auto rtn = call(false, (void*)hasnextHelper, v)->setType(RefType::BORROWED);
    var_is_a_python_bool.insert(rtn);
    return rtn;
}

RewriterVar* JitFragmentWriter::emitImportFrom(RewriterVar* module, BoxedString* name) {
    return call(false, (void*)importFrom, module, imm(name))->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitImportName(int level, RewriterVar* from_imports, llvm::StringRef module_name) {
    return call(false, (void*)import, imm(level), from_imports, imm(const_cast<char*>(module_name.data())),
                imm(module_name.size()))->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitImportStar(RewriterVar* module) {
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    return call(false, (void*)importStar, module, globals)->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitLandingpad() {
    return call(false, (void*)ASTInterpreterJitInterface::landingpadHelper, getInterp())->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitNonzero(RewriterVar* v) {
    if (var_is_a_python_bool.count(v))
        return v;
    // nonzeroHelper returns bool
    auto rtn = call(false, (void*)nonzeroHelper, v)->setType(RefType::BORROWED);
    var_is_a_python_bool.insert(rtn);
    return rtn;
}

RewriterVar* JitFragmentWriter::emitNotNonzero(RewriterVar* v) {
    auto rtn = call(false, (void*)notHelper, v)->setType(RefType::BORROWED);
    var_is_a_python_bool.insert(rtn);
    return rtn;
}

RewriterVar* JitFragmentWriter::emitRepr(RewriterVar* v) {
    return call(false, (void*)repr, v)->setType(RefType::OWNED);
}

RewriterVar* JitFragmentWriter::emitRuntimeCall(AST_expr* node, RewriterVar* obj, ArgPassSpec argspec,
                                                const llvm::ArrayRef<RewriterVar*> args,
                                                std::vector<BoxedString*>* keyword_names) {
    TypeRecorder* type_recorder = getTypeRecorderForNode(node);

#if ENABLE_BASELINEJIT_ICS
    RewriterVar* argspec_var = imm(argspec.asInt());
    RewriterVar::SmallVector call_args;
    call_args.push_back(obj);
    call_args.push_back(argspec_var);
    call_args.push_back(args.size() > 0 ? args[0] : imm(0ul));
    call_args.push_back(args.size() > 1 ? args[1] : imm(0ul));
    call_args.push_back(args.size() > 2 ? args[2] : imm(0ul));

    llvm::ArrayRef<RewriterVar*> additional_uses;
    if (args.size() > 3) {
        additional_uses = args.slice(3);
        RewriterVar* scratch = allocArgs(additional_uses, RewriterVar::SetattrType::REF_USED);
        call_args.push_back(scratch);
    } else
        call_args.push_back(imm(0ul));
    if (keyword_names)
        call_args.push_back(imm(keyword_names));

    return emitPPCall((void*)runtimeCall, call_args, 2 * 640, node, type_recorder, additional_uses)
        .first->setType(RefType::OWNED);
#else
    RewriterVar* argspec_var = imm(argspec.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size()) {
        args_array = allocArgs(args, RewriterVar::SetattrType::REF_USED);
    } else
        RELEASE_ASSERT(!keyword_names_var, "0 args but keyword names are set");

    RewriterVar::SmallVector call_args;
    call_args.push_back(obj);
    call_args.push_back(argspec_var);
    call_args.push_back(imm(type_recorder));
    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

    return emitCallWithAllocatedArgs((void*)runtimeCallHelper, call_args, args)->setType(RefType::OWNED);
#endif
}

RewriterVar* JitFragmentWriter::emitUnaryop(RewriterVar* v, int op_type) {
    return emitPPCall((void*)unaryop, { v, imm(op_type) }, 2 * 160).first->setType(RefType::OWNED);
}

std::vector<RewriterVar*> JitFragmentWriter::emitUnpackIntoArray(RewriterVar* v, uint64_t num) {
    RewriterVar* keep_alive = allocate(1);

    RewriterVar* array = call(false, (void*)unpackIntoArray, v, imm(num), keep_alive);

    std::vector<RewriterVar*> rtn;
    for (int i = 0; i < num; i++) {
        rtn.push_back(array->getAttr(i * sizeof(void*))->setType(RefType::OWNED));
    }

    keep_alive->getAttr(0)->setType(RefType::OWNED);
    return rtn;
}

RewriterVar* JitFragmentWriter::emitYield(RewriterVar* v) {
    llvm::SmallVector<RewriterVar*, 16> local_args;
    local_args.push_back(interp->getAttr(ASTInterpreterJitInterface::getCreatedClosureOffset()));
    // we have to pass all owned references which are not stored in the vregs to yield() so that the GC can traverse it
    for (auto&& sym : local_syms) {
        if (sym.second == v)
            continue;
        if (sym.second->reftype == RefType::OWNED)
            local_args.push_back(sym.second);
    }
    // erase duplicate entries
    std::sort(local_args.begin(), local_args.end());
    local_args.erase(std::unique(local_args.begin(), local_args.end()), local_args.end());

    auto&& args = allocArgs(local_args, RewriterVar::SetattrType::REF_USED);
    RewriterVar* generator = interp->getAttr(ASTInterpreterJitInterface::getGeneratorOffset());
    auto rtn = call(false, (void*)yield, { generator, v, args, imm(local_args.size()) }, {}, local_args)
                   ->setType(RefType::OWNED);
    v->refConsumed();
    return rtn;
}

void JitFragmentWriter::emitAssignSlice(RewriterVar* target, RewriterVar* lower, RewriterVar* upper,
                                        RewriterVar* value) {
    if (!lower)
        lower = imm(0ul);
    if (!upper)
        upper = imm(0ul);
    emitPPCall((void*)assignSlice, { target, lower, upper, value }, 256).first;
}

void JitFragmentWriter::emitDelAttr(RewriterVar* target, BoxedString* attr) {
    emitPPCall((void*)delattr, { target, imm(attr) }, 144).first;
}

void JitFragmentWriter::emitDelGlobal(BoxedString* name) {
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    // does not get rewriten yet
    // emitPPCall((void*)delGlobal, { globals, imm(name) }, 512).first;
    call(false, (void*)delGlobal, globals, imm(name));
}

void JitFragmentWriter::emitDelItem(RewriterVar* target, RewriterVar* slice) {
    emitPPCall((void*)delitem, { target, slice }, 256).first;
}

void JitFragmentWriter::emitDelName(InternedString name) {
    call(false, (void*)ASTInterpreterJitInterface::delNameHelper, getInterp(),
#ifndef NDEBUG
         imm(asUInt(name).first), imm(asUInt(name).second));
#else
         imm(asUInt(name)));
#endif
}

void JitFragmentWriter::emitExec(RewriterVar* code, RewriterVar* globals, RewriterVar* locals, FutureFlags flags) {
    if (!globals)
        globals = imm(0ul);
    if (!locals)
        locals = imm(0ul);
    call(false, (void*)exec, code, globals, locals, imm(flags));
}

void JitFragmentWriter::emitJump(CFGBlock* b) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitJump() start");

    RewriterVar* next = imm(b);
    addAction([=]() { _emitJump(b, next, exit_info); }, { next }, ActionType::NORMAL);
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitJump() end");
}

void JitFragmentWriter::emitOSRPoint(AST_Jump* node) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitOSRPoint() start");
    addAction([=]() { _emitOSRPoint(); }, { getInterp() }, ActionType::NORMAL);
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitOSRPoint() end");
}

void JitFragmentWriter::emitPendingCallsCheck() {
#if ENABLE_SIGNAL_CHECKING
    call(false, (void*)ASTInterpreterJitInterface::pendingCallsCheckHelper);
#endif
}

void JitFragmentWriter::emitPrint(RewriterVar* dest, RewriterVar* var, bool nl) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitPrint() start");
    if (!dest)
        dest = imm(0ul);
    if (!var)
        var = imm(0ul);
    call(false, (void*)printHelper, dest, var, imm(nl));
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitPrint() end");
}

void JitFragmentWriter::emitRaise0() {
    call(false, (void*)ASTInterpreterJitInterface::raise0Helper, getInterp());
}

void JitFragmentWriter::emitRaise3(RewriterVar* arg0, RewriterVar* arg1, RewriterVar* arg2) {
    call(false, (void*)raise3, arg0, arg1, arg2);
    arg0->refConsumed();
    arg1->refConsumed();
    arg2->refConsumed();
}

void JitFragmentWriter::emitReturn(RewriterVar* v) {
    addAction([=]() { _emitReturn(v); }, { v }, ActionType::NORMAL);
    v->refConsumed();
}

void JitFragmentWriter::emitSetAttr(AST_expr* node, RewriterVar* obj, BoxedString* s, STOLEN(RewriterVar*) attr) {
    auto rtn = emitPPCall((void*)setattr, { obj, imm(s), attr }, 2 * 256, node);
    attr->refConsumed(rtn.second);
}

void JitFragmentWriter::emitSetBlockLocal(InternedString s, int vreg, STOLEN(RewriterVar*) v) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitSetBlockLocal() start");
    RewriterVar* prev = local_syms[s];
    // if we never set this sym before in this BB and the symbol gets accessed in several blocks clear it because it
    // could have been set in a previous block.
    if (!prev && !block->cfg->getVRegInfo().isBlockLocalVReg(vreg))
        emitSetLocal(s, vreg, false, imm(nullptr)); // clear out the vreg
    local_syms[s] = v;
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitSetBlockLocal() end");
}

void JitFragmentWriter::emitSetCurrentInst(AST_stmt* node) {
    getInterp()->setAttr(ASTInterpreterJitInterface::getCurrentInstOffset(), imm(node));
}

void JitFragmentWriter::emitSetExcInfo(RewriterVar* type, RewriterVar* value, RewriterVar* traceback) {
    call(false, (void*)ASTInterpreterJitInterface::setExcInfoHelper, getInterp(), type, value, traceback);
    type->refConsumed();
    value->refConsumed();
    traceback->refConsumed();
}

void JitFragmentWriter::emitSetGlobal(BoxedString* s, STOLEN(RewriterVar*) v, bool are_globals_from_module) {
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    std::pair<RewriterVar*, RewriterAction*> rtn;
    if (are_globals_from_module)
        rtn = emitPPCall((void*)setattr, { globals, imm(s), v }, 256);
    else
        rtn = emitPPCall((void*)setGlobal, { globals, imm(s), v }, 256);
    v->refConsumed(rtn.second);
}

void JitFragmentWriter::emitSetItem(RewriterVar* target, RewriterVar* slice, RewriterVar* value) {
    emitPPCall((void*)setitem, { target, slice, value }, 2 * 512);
}

void JitFragmentWriter::emitSetItemName(BoxedString* s, RewriterVar* v) {
    emitSetItem(emitGetBoxedLocals(), imm(s), v);
}

void JitFragmentWriter::emitSetLocal(InternedString s, int vreg, bool set_closure, STOLEN(RewriterVar*) v) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitSetLocal() start");
    assert(vreg >= 0);
    if (set_closure) {
        call(false, (void*)ASTInterpreterJitInterface::setLocalClosureHelper, getInterp(), imm(vreg),
#ifndef NDEBUG
             imm(asUInt(s).first), imm(asUInt(s).second),
#else
             imm(asUInt(s)),
#endif
             v);
        v->refConsumed();
    } else {
        // TODO With definedness analysis, we could know whether we needed to emit an decref/xdecref/neither.
        // The issue is that definedness analysis is somewhat expensive to compute, so we don't compute it
        // for the bjit.  We could try calculating it (which would require some re-plumbing), which might help
        // but I suspect is not that big a deal as long as the llvm jit implements this kind of optimization.
        bool prev_nullable = known_non_null_vregs.count(vreg) == 0;

        assert(!block->cfg->getVRegInfo().isBlockLocalVReg(vreg));
        vregs_array->replaceAttr(8 * vreg, v, prev_nullable);
        if (v->isContantNull())
            known_non_null_vregs.erase(vreg);
        else
            known_non_null_vregs.insert(vreg);
    }
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitSetLocal() end");
}

void JitFragmentWriter::emitSideExit(STOLEN(RewriterVar*) v, Box* cmp_value, CFGBlock* next_block) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitSideExit start");
    RewriterVar* var = imm(cmp_value);
    RewriterVar* next_block_var = imm(next_block);

    llvm::SmallVector<RewriterVar*, 16> vars;
    vars.push_back(v);
    vars.push_back(var);
    vars.push_back(next_block_var);

    addAction([=]() { _emitSideExit(v, var, next_block, next_block_var); }, { v, var, next_block_var },
              ActionType::NORMAL);
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitSideExit end");
}

void JitFragmentWriter::emitUncacheExcInfo() {
    call(false, (void*)ASTInterpreterJitInterface::uncacheExcInfoHelper, getInterp());
}


void JitFragmentWriter::abortCompilation() {
    blocks_aborted.insert(block);
    code_block.fragmentAbort(false);
    abort();
}

int JitFragmentWriter::finishCompilation() {
    RELEASE_ASSERT(!assembler->hasFailed(), "");

    commit();
    if (failed) {
        blocks_aborted.insert(block);
        code_block.fragmentAbort(false);
        return 0;
    }

    if (assembler->hasFailed()) {
        int bytes_written = assembler->bytesWritten();

        // don't retry JITing very large blocks
        const auto large_block_threshold = code_size - 4096;
        if (bytes_written > large_block_threshold) {
            static StatCounter num_jit_large_blocks("num_baselinejit_skipped_large_blocks");
            num_jit_large_blocks.log();

            blocks_aborted.insert(block);
            code_block.fragmentAbort(false);
        } else {
            // we ran out of space - we allow a retry and set shouldCreateNewBlock to true in order to allocate a new
            // block for the next attempt.
            code_block.fragmentAbort(true /* not_enough_space */);
        }
        return 0;
    }

    block->code = (void*)((uint64_t)entry_code + code_offset);
    block->entry_code = (decltype(block->entry_code))entry_code;

    if (LOG_BJIT_ASSEMBLY) {
        printf("\n");
        printf("Successfully bjit'd code for cfg block %d\n", block->idx);
        printf("Code goes from %p-%p\n", block->code, (char*)block->code + assembler->bytesWritten());
    }

    // if any side exits point to this block patch them to a direct jump to this block
    auto it = block_patch_locations.find(block);
    if (it != block_patch_locations.end()) {
        for (void* patch_location : it->second) {
            assembler::Assembler patch_asm((uint8_t*)patch_location, min_patch_size);
            int64_t offset = (uint64_t)block->code - (uint64_t)patch_location;
            if (isLargeConstant(offset)) {
                patch_asm.mov(assembler::Immediate(block->code), assembler::R11);
                patch_asm.jmpq(assembler::R11);
            } else
                patch_asm.jmp(assembler::JumpDestination::fromStart(offset));
            RELEASE_ASSERT(!patch_asm.hasFailed(), "you may have to increase 'min_patch_size'");
        }
        block_patch_locations.erase(it);
    }

    // if we have a side exit, remember its location for patching
    if (side_exit_patch_location.first) {
        void* patch_location = (uint8_t*)block->code + side_exit_patch_location.second;
        block_patch_locations[side_exit_patch_location.first].push_back(patch_location);
    }

    for (auto&& pp_info : pp_infos) {
        SpillMap _spill_map;
        uint8_t* start_addr = pp_info.start_addr;
        uint8_t* end_addr = pp_info.end_addr;
        PatchpointInitializationInfo initialization_info
            = initializePatchpoint3(pp_info.func_addr, start_addr, end_addr, 0 /* scratch_offset */,
                                    0 /* scratch_size */, LiveOutSet(), _spill_map);
        uint8_t* slowpath_start = initialization_info.slowpath_start;
        uint8_t* slowpath_rtn_addr = initialization_info.slowpath_rtn_addr;

        std::unique_ptr<ICInfo> pp = registerCompiledPatchpoint(
            start_addr, slowpath_start, initialization_info.continue_addr, slowpath_rtn_addr, pp_info.ic.get(),
            pp_info.stack_info, LiveOutSet(), std::move(pp_info.decref_infos));
        pp->associateNodeWithICInfo(pp_info.node);
        pp.release();
    }

#ifndef NDEBUG
    if (LOG_BJIT_ASSEMBLY) {
        auto s = assembler->dump((uint8_t*)block->code /*, (uint8_t*)block->code + assembler->bytesWritten()*/);
        printf("%s\n", s.c_str());
    }
#endif

    void* next_fragment_start = (uint8_t*)block->code + assembler->bytesWritten();
    if (exit_info.num_bytes)
        ASSERT(assembler->curInstPointer() == (uint8_t*)exit_info.exit_start + exit_info.num_bytes,
               "Error! wrote more bytes out after the 'retq' that we thought was going to be the end of the assembly.  "
               "We will end up overwriting those instructions.");
    code_block.fragmentFinished(assembler->bytesWritten(), num_bytes_overlapping, next_fragment_start, *ic_info);

#if MOVING_GC
    // If JitFragmentWriter is destroyed, we don't necessarily want the ICInfo to be destroyed also,
    // because it may contain a list of references to pointers in generated code that still exists
    // and we need to keep those around.
    // TODO: When should these ICInfo be freed?
    registerGCTrackedICInfo(ic_info.release());
#endif

    return exit_info.num_bytes;
}

bool JitFragmentWriter::finishAssembly(int continue_offset, bool& should_fill_with_nops, bool& variable_size_slots) {
    should_fill_with_nops = false;
    variable_size_slots = false;
    return !assembler->hasFailed();
}


RewriterVar* JitFragmentWriter::allocArgs(const llvm::ArrayRef<RewriterVar*> args,
                                          RewriterVar::SetattrType setattr_type) {
    auto num = args.size();
    assert(num);
    RewriterVar* array = allocate(num);
    for (int i = 0; i < num; ++i)
        array->setAttr(sizeof(void*) * i, args[i], setattr_type);
    return array;
}

#ifndef NDEBUG
std::pair<uint64_t, uint64_t> JitFragmentWriter::asUInt(InternedString s) {
    static_assert(sizeof(InternedString) == sizeof(uint64_t) * 2, "");
    union U {
        U(InternedString is) : is(is) {}
        InternedString is;
        uint64_t u[2];
    } u(s);
    return std::make_pair(u.u[0], u.u[1]);
}
#else
uint64_t JitFragmentWriter::asUInt(InternedString s) {
    static_assert(sizeof(InternedString) == sizeof(uint64_t), "");
    union U {
        U(InternedString is) : is(is) {}
        InternedString is;
        uint64_t u;
    } u(s);
    return u.u;
}
#endif

std::pair<RewriterVar*, RewriterAction*>
JitFragmentWriter::emitPPCall(void* func_addr, llvm::ArrayRef<RewriterVar*> args, unsigned short pp_size, AST* ast_node,
                              TypeRecorder* type_recorder, llvm::ArrayRef<RewriterVar*> additional_uses) {
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitPPCall() start");
    RewriterVar::SmallVector args_vec(args.begin(), args.end());
#if ENABLE_BASELINEJIT_ICS
    RewriterVar* result = createNewVar();

    auto args_array_ref = regionAllocArgs(args, additional_uses);

    assert(args.size() < 1ul << 32);
    int args_size = args.size();
    assert(additional_uses.size() < 1 << 8);
    unsigned char num_additional = additional_uses.size();
    RewriterVar** args_array = args_array_ref.data();

    RewriterAction* call_action
        = addAction([this, result, func_addr, ast_node, args_array, args_size, pp_size, num_additional]() {
            auto all_args = llvm::makeArrayRef(args_array, args_size + num_additional);
            auto args = all_args.slice(0, args_size);
            this->_emitPPCall(result, func_addr, args, pp_size, ast_node, all_args);
        }, args_array_ref, ActionType::NORMAL);

    if (type_recorder) {
        RewriterVar* type_recorder_var = imm(type_recorder);
        RewriterVar* obj_cls_var = result->getAttr(offsetof(Box, cls));
        addAction([=]() {
            _emitRecordType(type_recorder_var, obj_cls_var);
            result->bumpUse();
        }, { type_recorder_var, obj_cls_var, result }, ActionType::NORMAL);

        emitPendingCallsCheck();
        return std::make_pair(result, call_action);
    }
    emitPendingCallsCheck();
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitPPCall() end");
    return std::make_pair(result, call_action);
#else
    assert(args_vec.size() < 7);
    RewriterVar* result = call(false, func_addr, args_vec);
    RewriterAction* call_action = getLastAction();
    emitPendingCallsCheck();
    if (LOG_BJIT_ASSEMBLY)
        comment("BJIT: emitPPCall() end");
    return std::make_pair(result, call_action);
#endif
}

void JitFragmentWriter::assertNameDefinedHelper(const char* id) {
    assertNameDefined(0, id, UnboundLocalError, true);
}

Box* JitFragmentWriter::callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, TypeRecorder* type_recorder,
                                       Box** args, std::vector<BoxedString*>* keyword_names) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], flags.argspec.totalPassed());
    Box* r = callattr(obj, attr, flags, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                      std::get<3>(arg_tuple), keyword_names);
    return recordType(type_recorder, r);
}

Box* JitFragmentWriter::createDictHelper(uint64_t num, Box** keys, Box** values) {
    BoxedDict* dict = (BoxedDict*)createDict();
    for (uint64_t i = 0; i < num; ++i) {
        int ret = PyDict_SetItem(dict, autoDecref(keys[i]), autoDecref(values[i]));
        if (ret == -1)
            throwCAPIException();
    }
    return dict;
}

Box* JitFragmentWriter::createListHelper(uint64_t num, Box** data) {
    BoxedList* list = (BoxedList*)createList();
    list->ensure(num);
    for (uint64_t i = 0; i < num; ++i) {
        listAppendInternalStolen(list, data[i]);
    }
    return list;
}

Box* JitFragmentWriter::createSetHelper(uint64_t num, Box** data) {
    try {
        BoxedSet* set = (BoxedSet*)createSet();
        for (int i = 0; i < num; ++i) {
            _setAddStolen(set, data[i]);
        }
        return set;
    } catch (ExcInfo e) {
        RELEASE_ASSERT(0, "this leaks in case of an exception");
    }
}

Box* JitFragmentWriter::createTupleHelper(uint64_t num, Box** data) {
    return BoxedTuple::create(num, data);
}

Box* JitFragmentWriter::exceptionMatchesHelper(Box* obj, Box* cls) {
    return boxBool(exceptionMatches(obj, cls));
}

BORROWED(Box*) JitFragmentWriter::hasnextHelper(Box* b) {
    return pyston::hasnext(b) ? Py_True : Py_False;
}

BORROWED(Box*) JitFragmentWriter::nonzeroHelper(Box* b) {
    return b->nonzeroIC() ? Py_True : Py_False;
}

BORROWED(Box*) JitFragmentWriter::notHelper(Box* b) {
    return b->nonzeroIC() ? Py_False : Py_True;
}

Box* JitFragmentWriter::runtimeCallHelper(Box* obj, ArgPassSpec argspec, TypeRecorder* type_recorder, Box** args,
                                          std::vector<BoxedString*>* keyword_names) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    Box* r = runtimeCall(obj, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                         std::get<3>(arg_tuple), keyword_names);
    return recordType(type_recorder, r);
}

void JitFragmentWriter::_emitGetLocal(RewriterVar* val_var, const char* name) {
    assembler::Register var_reg = val_var->getInReg();
    assembler->test(var_reg, var_reg);

    _setupCall(false, {});
    {
        assembler::ForwardJump jnz(*assembler, assembler::COND_NOT_ZERO);
        const_loader.loadConstIntoReg((uint64_t)name, assembler::RDI);
        _callOptimalEncoding(assembler::R11, (void*)assertNameDefinedHelper);

        registerDecrefInfoHere();
    }

    _incref(val_var);
    val_var->bumpUse();
}

void JitFragmentWriter::_emitJump(CFGBlock* b, RewriterVar* block_next, ExitInfo& exit_info) {
    assert(exit_info.num_bytes == 0);
    assert(exit_info.exit_start == NULL);
    if (b->code) {
        int64_t offset = (uint64_t)b->code - ((uint64_t)entry_code + code_offset);
        if (isLargeConstant(offset)) {
            assembler->mov(assembler::Immediate(b->code), assembler::R11);
            assembler->jmpq(assembler::R11);
        } else
            assembler->jmp(assembler::JumpDestination::fromStart(offset));
    } else {
        int num_bytes = assembler->bytesWritten();
        exit_info.exit_start = assembler->curInstPointer();
        block_next->getInReg(assembler::RAX, true);
        assembler->add(assembler::Immediate(JitCodeBlock::sp_adjustment), assembler::RSP);
        assembler->pop(assembler::R12);
        assembler->pop(assembler::R13);
        assembler->pop(assembler::R14);
        assembler->pop(assembler::R15);
        assembler->retq();

        // make sure we have at least 'min_patch_size' of bytes available.
        for (int i = assembler->bytesWritten() - num_bytes; i < min_patch_size; ++i)
            assembler->trap(); // we could use nops but traps may help if something goes wrong

        exit_info.num_bytes = assembler->bytesWritten() - num_bytes;
        assert(assembler->hasFailed() || exit_info.num_bytes >= min_patch_size);
    }
    block_next->bumpUse();
}

void JitFragmentWriter::_emitOSRPoint() {
    // We can't directly do OSR from the bjit frame because it will cause issues with exception handling.
    // Reason is that the bjit and the OSRed code share the same python frame and the way invokes are implemented in the
    // bjit. During unwinding we will see the OSR frame and will remove it and continue to unwind but the try catch
    // block inside ASTInterpreter::execJITedBlock will rethrow the exception which causes another frame deinit,
    // which is wrong because it already got removed.
    // Instead we return back to the interpreter loop with special value (osr_dummy_value) which will trigger the OSR.
    // this generates code for:
    // if (++interpreter.edgecount < OSR_THRESHOLD_BASELINE)
    //     return std::make_pair((CFGBlock*)0, ASTInterpreterJitInterface::osr_dummy_value);
    assembler::Register interp_reg = getInterp()->getInReg(); // will always be r13
    assembler::Indirect edgecount = assembler::Indirect(interp_reg, ASTInterpreterJitInterface::getEdgeCountOffset());
    assembler->incl(edgecount);                                               // 32bit inc
    assembler->cmpl(edgecount, assembler::Immediate(OSR_THRESHOLD_BASELINE)); // 32bit cmp
    {
        assembler::ForwardJump jl(*assembler, assembler::COND_BELOW);
        assembler->clear_reg(assembler::RAX); // = next block to execute
        assembler->mov(assembler::Immediate(ASTInterpreterJitInterface::osr_dummy_value), assembler::RDX);
        assembler->add(assembler::Immediate(JitCodeBlock::sp_adjustment), assembler::RSP);
        assembler->pop(assembler::R12);
        assembler->pop(assembler::R13);
        assembler->pop(assembler::R14);
        assembler->pop(assembler::R15);
        assembler->retq();
    }
    interp->bumpUse();
    assertConsistent();
}

void JitFragmentWriter::_emitPPCall(RewriterVar* result, void* func_addr, llvm::ArrayRef<RewriterVar*> args,
                                    unsigned short pp_size, AST* ast_node, llvm::ArrayRef<RewriterVar*> vars_to_bump) {
    assembler::Register r = allocReg(assembler::R11);

    if (args.size() > 6) { // only 6 args can get passed in registers.
        assert(args.size() <= 6 + JitCodeBlock::num_stack_args);
        for (int i = 6; i < args.size(); ++i) {
            assembler::Register reg = args[i]->getInReg(Location::any(), true);
            assembler->mov(reg, assembler::Indirect(assembler::RSP, sizeof(void*) * (i - 6)));
        }
        _setupCall(false, args.slice(0, 6), {}, Location::any(), vars_to_bump);
    } else
        _setupCall(false, args, {}, Location::any(), vars_to_bump);

    if (failed)
        return;

    // make sure setupCall doesn't use R11
    assert(vars_by_location.count(assembler::R11) == 0);

    // make space for patchpoint
    uint8_t* pp_start = rewrite->getSlotStart() + assembler->bytesWritten();
    constexpr int call_size = 13;
    assembler->skipBytes(pp_size + call_size);
    uint8_t* pp_end = rewrite->getSlotStart() + assembler->bytesWritten();
    assert(assembler->hasFailed() || (pp_start + pp_size + call_size == pp_end));

    std::unique_ptr<ICSetupInfo> setup_info(ICSetupInfo::initialize(true, pp_size, ICSetupInfo::Generic, NULL));

    // calculate available scratch space
    int pp_scratch_size = 0;
    int pp_scratch_location = rewrite->getScratchRspOffset() + rewrite->getScratchSize();
    for (int i = rewrite->getScratchSize() - 8; i >= 0; i -= 8) {
        Location l(Location::Scratch, i);
        if (vars_by_location.count(l))
            break;

        pp_scratch_size += 8;
        pp_scratch_location -= 8;
    }

    assertConsistent();

    StackInfo stack_info(pp_scratch_size, pp_scratch_location);
    auto&& decref_infos = getDecrefLocations();
    pp_infos.emplace_back(
        PPInfo{ func_addr, pp_start, pp_end, std::move(setup_info), stack_info, ast_node, std::move(decref_infos) });

    assert(vars_by_location.count(assembler::RAX) == 0);
    result->initializeInReg(assembler::RAX);
    assertConsistent();

    result->releaseIfNoUses();

    for (RewriterVar* use : vars_to_bump) {
        use->bumpUseLateIfNecessary();
    }
}

void JitFragmentWriter::_emitRecordType(RewriterVar* type_recorder_var, RewriterVar* obj_cls_var) {
    // This directly emits the instructions of the recordType() function.

    assembler::Register obj_cls_reg = obj_cls_var->getInReg();
    assembler::Register type_recorder_reg = type_recorder_var->getInReg(Location::any(), true, obj_cls_reg);
    assembler::Indirect last_seen_count = assembler::Indirect(type_recorder_reg, offsetof(TypeRecorder, last_count));
    assembler::Indirect last_seen_indirect = assembler::Indirect(type_recorder_reg, offsetof(TypeRecorder, last_seen));

    assembler->cmp(last_seen_indirect, obj_cls_reg);
    {
        assembler::ForwardJump je(*assembler, assembler::COND_EQUAL);
        assembler->mov(obj_cls_reg, last_seen_indirect);
        assembler->movq(assembler::Immediate(0ul), last_seen_count);
    }
    assembler->incq(last_seen_count);

    type_recorder_var->bumpUse();
    obj_cls_var->bumpUse();
}

void JitFragmentWriter::_emitReturn(RewriterVar* return_val) {
    return_val->getInReg(assembler::RDX, true);
    assembler->clear_reg(assembler::RAX);
    assembler->add(assembler::Immediate(JitCodeBlock::sp_adjustment), assembler::RSP);
    assembler->pop(assembler::R12);
    assembler->pop(assembler::R13);
    assembler->pop(assembler::R14);
    assembler->pop(assembler::R15);
    assembler->retq();
    return_val->bumpUse();
}

void JitFragmentWriter::_emitSideExit(STOLEN(RewriterVar*) var, RewriterVar* val_constant, CFGBlock* next_block,
                                      RewriterVar* next_block_var) {
    assert(val_constant->is_constant);
    assert(next_block_var->is_constant);
    uint64_t val = val_constant->constant_value;

    assert(val == (uint64_t)Py_True || val == (uint64_t)Py_False);

    // HAXX ahead:
    // Override the automatic refcounting system, to force a decref to happen before the jump.
    // Really, we should probably do a decref on either side post-jump.
    // But the automatic refcounter doesn't support that, and since the value is either True or False,
    // we can get away with doing the decref early.
    if (var->reftype == RefType::OWNED) {
        _decref(var);
        // Hax: override the automatic refcount system
        var->reftype = RefType::BORROWED;
    }

    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }

    {
        assembler::ForwardJump jne(*assembler, assembler::COND_EQUAL);

        ExitInfo exit_info;
        _emitJump(next_block, next_block_var, exit_info);
        if (exit_info.num_bytes) {
            assert(assembler->curInstPointer() == (uint8_t*)exit_info.exit_start + exit_info.num_bytes);
            RELEASE_ASSERT(!side_exit_patch_location.first,
                           "if we start to emit more than one side exit we should make this a vector");
            side_exit_patch_location = std::make_pair(next_block, assembler->bytesWritten() - exit_info.num_bytes);
        }
    }

    var->bumpUse();
    val_constant->bumpUse();

    assertConsistent();
}
}
