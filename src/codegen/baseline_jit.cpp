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

#include "codegen/baseline_jit.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

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
//   asm volatile ("" ::: "r14");
//   asm volatile ("" ::: "r12");
//   char scratch[256+16];
//   foo(scratch);
// }
//
// It omits the frame pointer but saves R12 and R14
const unsigned char eh_info[]
    = { 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7a, 0x52, 0x00, 0x01, 0x78, 0x10,
        0x01, 0x1b, 0x0c, 0x07, 0x08, 0x90, 0x01, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x1c, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x42, 0x0e, 0x10, 0x42,
        0x0e, 0x18, 0x47, 0x0e, 0xb0, 0x02, 0x8c, 0x03, 0x8e, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
static_assert(JitCodeBlock::num_stack_args == 2, "have to update EH table!");
static_assert(JitCodeBlock::scratch_size == 256, "have to update EH table!");

JitCodeBlock::JitCodeBlock(llvm::StringRef name)
    : code(new uint8_t[code_size]),
      eh_frame(new uint8_t[sizeof(eh_info)]),
      entry_offset(0),
      a(code.get(), code_size),
      is_currently_writing(false),
      asm_failed(false) {
    static StatCounter num_jit_code_blocks("num_baselinejit_code_blocks");
    num_jit_code_blocks.log();
    static StatCounter num_jit_total_bytes("num_baselinejit_total_bytes");
    num_jit_total_bytes.log(code_size);

    // emit prolog
    a.push(assembler::R14);
    a.push(assembler::R12);
    static_assert(sp_adjustment % 16 == 8, "stack isn't aligned");
    a.sub(assembler::Immediate(sp_adjustment), assembler::RSP);
    a.mov(assembler::RDI, assembler::R12);                                // interpreter pointer
    a.mov(assembler::RDX, assembler::R14);                                // vreg array
    a.jmp(assembler::Indirect(assembler::RSI, offsetof(CFGBlock, code))); // jump to block

    entry_offset = a.bytesWritten();

    // generate the eh frame...
    const int size = sizeof(eh_info);
    void* eh_frame_addr = eh_frame.get();
    memcpy(eh_frame_addr, eh_info, size);

    int32_t* offset_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x20);
    int32_t* size_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x24);
    int64_t offset = (int8_t*)code.get() - (int8_t*)offset_ptr;
    assert(offset >= INT_MIN && offset <= INT_MAX);
    *offset_ptr = offset;
    *size_ptr = code_size;

    registerDynamicEhFrame((uint64_t)code.get(), code_size, (uint64_t)eh_frame_addr, size - 4);
    registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, size);

    g.func_addr_registry.registerFunction(("bjit_" + name).str(), code.get(), code_size, NULL);
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
    std::unique_ptr<ICInfo> ic_info(new ICInfo(fragment_start, nullptr, nullptr, stack_info, 1, bytes_left,
                                               llvm::CallingConv::C, live_outs, assembler::RAX, 0));
    std::unique_ptr<ICSlotRewrite> rewrite(new ICSlotRewrite(ic_info.get(), ""));

    return std::unique_ptr<JitFragmentWriter>(new JitFragmentWriter(
        block, std::move(ic_info), std::move(rewrite), fragment_offset, patch_jump_offset, a.getStartAddr(), *this));
}

void JitCodeBlock::fragmentAbort(bool not_enough_space) {
    asm_failed = not_enough_space;
    is_currently_writing = false;
}

void JitCodeBlock::fragmentFinished(int bytes_written, int num_bytes_overlapping, void* next_fragment_start) {
    assert(next_fragment_start == bytes_written + a.curInstPointer() - num_bytes_overlapping);
    a.setCurInstPointer((uint8_t*)next_fragment_start);

    asm_failed = false;
    is_currently_writing = false;
}


JitFragmentWriter::JitFragmentWriter(CFGBlock* block, std::unique_ptr<ICInfo> ic_info,
                                     std::unique_ptr<ICSlotRewrite> rewrite, int code_offset, int num_bytes_overlapping,
                                     void* entry_code, JitCodeBlock& code_block)
    : Rewriter(std::move(rewrite), 0, {}),
      block(block),
      code_offset(code_offset),
      num_bytes_exit(0),
      num_bytes_overlapping(num_bytes_overlapping),
      entry_code(entry_code),
      code_block(code_block),
      interp(0),
      ic_info(std::move(ic_info)) {
    interp = createNewVar();
    addLocationToVar(interp, assembler::R12);
    interp->setAttr(ASTInterpreterJitInterface::getCurrentBlockOffset(), imm(block));

    vregs_array = createNewVar();
    addLocationToVar(vregs_array, assembler::R14);
    addAction([=]() { vregs_array->bumpUse(); }, vregs_array, ActionType::NORMAL);
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
    return emitPPCall((void*)augbinop, { lhs, rhs, imm(op_type) }, 2, 320, node);
}

RewriterVar* JitFragmentWriter::emitBinop(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type) {
    return emitPPCall((void*)binop, { lhs, rhs, imm(op_type) }, 2, 240, node);
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

    if (args.size() > 3) {
        RewriterVar* scratch = allocate(args.size() - 3);
        for (int i = 0; i < args.size() - 3; ++i)
            scratch->setAttr(i * sizeof(void*), args[i + 3]);
        call_args.push_back(scratch);
    } else if (keyword_names) {
        call_args.push_back(imm(0ul));
    }

    if (keyword_names)
        call_args.push_back(imm(keyword_names));

    return emitPPCall((void*)callattr, call_args, 2, 640, node, type_recorder);
#else
    // We could make this faster but for now: keep it simple, stupid...
    RewriterVar* attr_var = imm(attr);
    RewriterVar* flags_var = imm(flags.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size())
        args_array = allocArgs(args);
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

    return call(false, (void*)callattrHelper, call_args);
#endif
}

RewriterVar* JitFragmentWriter::emitCompare(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type) {
    // TODO: can directly emit the assembly for Is/IsNot
    return emitPPCall((void*)compare, { lhs, rhs, imm(op_type) }, 2, 240, node);
}

RewriterVar* JitFragmentWriter::emitCreateDict(const llvm::ArrayRef<RewriterVar*> keys,
                                               const llvm::ArrayRef<RewriterVar*> values) {
    assert(keys.size() == values.size());
    if (keys.empty())
        return call(false, (void*)createDict);
    else
        return call(false, (void*)createDictHelper, imm(keys.size()), allocArgs(keys), allocArgs(values));
}

RewriterVar* JitFragmentWriter::emitCreateList(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)createList);
    else
        return call(false, (void*)createListHelper, imm(num), allocArgs(values));
}

RewriterVar* JitFragmentWriter::emitCreateSet(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)createSet);
    else
        return call(false, (void*)createSetHelper, imm(num), allocArgs(values));
}

RewriterVar* JitFragmentWriter::emitCreateSlice(RewriterVar* start, RewriterVar* stop, RewriterVar* step) {
    return call(false, (void*)createSlice, start, stop, step);
}

RewriterVar* JitFragmentWriter::emitCreateTuple(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return imm(EmptyTuple);
    else if (num == 1)
        return call(false, (void*)BoxedTuple::create1, values[0]);
    else if (num == 2)
        return call(false, (void*)BoxedTuple::create2, values[0], values[1]);
    else if (num == 3)
        return call(false, (void*)BoxedTuple::create3, values[0], values[1], values[2]);
    else
        return call(false, (void*)createTupleHelper, imm(num), allocArgs(values));
}

RewriterVar* JitFragmentWriter::emitDeref(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::derefHelper, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second));
#else
                imm(asUInt(s)));
#endif
}

RewriterVar* JitFragmentWriter::emitExceptionMatches(RewriterVar* v, RewriterVar* cls) {
    return call(false, (void*)exceptionMatchesHelper, v, cls);
}

RewriterVar* JitFragmentWriter::emitGetAttr(RewriterVar* obj, BoxedString* s, AST_expr* node) {
    return emitPPCall((void*)getattr, { obj, imm(s) }, 2, 512, node, getTypeRecorderForNode(node));
}

RewriterVar* JitFragmentWriter::emitGetBlockLocal(InternedString s, int vreg) {
    auto it = local_syms.find(s);
    if (it == local_syms.end())
        return emitGetLocal(s, vreg);
    return it->second;
}

RewriterVar* JitFragmentWriter::emitGetBoxedLocal(BoxedString* s) {
    RewriterVar* boxed_locals = emitGetBoxedLocals();
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    return call(false, (void*)boxedLocalsGet, boxed_locals, imm(s), globals);
}

RewriterVar* JitFragmentWriter::emitGetBoxedLocals() {
    return getInterp()->getAttr(ASTInterpreterJitInterface::getBoxedLocalsOffset());
}

RewriterVar* JitFragmentWriter::emitGetClsAttr(RewriterVar* obj, BoxedString* s) {
    return emitPPCall((void*)getclsattr, { obj, imm(s) }, 2, 512);
}

RewriterVar* JitFragmentWriter::emitGetGlobal(Box* global, BoxedString* s) {
    if (s->s() == "None")
        return imm(None);
    return emitPPCall((void*)getGlobal, { imm(global), imm(s) }, 2, 512);
}

RewriterVar* JitFragmentWriter::emitGetItem(AST_expr* node, RewriterVar* value, RewriterVar* slice) {
    return emitPPCall((void*)getitem, { value, slice }, 2, 512, node);
}

RewriterVar* JitFragmentWriter::emitGetLocal(InternedString s, int vreg) {
    assert(vreg >= 0);
    RewriterVar* val_var = vregs_array->getAttr(vreg * 8);
    addAction([=]() { _emitGetLocal(val_var, s.c_str()); }, { val_var }, ActionType::NORMAL);
    return val_var;
}

RewriterVar* JitFragmentWriter::emitGetPystonIter(RewriterVar* v) {
    return call(false, (void*)getPystonIter, v);
}

RewriterVar* JitFragmentWriter::emitHasnext(RewriterVar* v) {
    return call(false, (void*)hasnextHelper, v);
}

RewriterVar* JitFragmentWriter::emitImportFrom(RewriterVar* module, BoxedString* name) {
    return call(false, (void*)importFrom, module, imm(name));
}

RewriterVar* JitFragmentWriter::emitImportName(int level, RewriterVar* from_imports, llvm::StringRef module_name) {
    return call(false, (void*)import, imm(level), from_imports, imm(const_cast<char*>(module_name.data())),
                imm(module_name.size()));
}

RewriterVar* JitFragmentWriter::emitImportStar(RewriterVar* module) {
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    return call(false, (void*)importStar, module, globals);
}

RewriterVar* JitFragmentWriter::emitLandingpad() {
    return call(false, (void*)ASTInterpreterJitInterface::landingpadHelper, getInterp());
}

RewriterVar* JitFragmentWriter::emitNonzero(RewriterVar* v) {
    return call(false, (void*)nonzeroHelper, v);
}

RewriterVar* JitFragmentWriter::emitNotNonzero(RewriterVar* v) {
    return call(false, (void*)notHelper, v);
}

RewriterVar* JitFragmentWriter::emitRepr(RewriterVar* v) {
    return call(false, (void*)repr, v);
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

    if (args.size() > 3) {
        RewriterVar* scratch = allocate(args.size() - 3);
        for (int i = 0; i < args.size() - 3; ++i)
            scratch->setAttr(i * sizeof(void*), args[i + 3]);
        call_args.push_back(scratch);
    } else
        call_args.push_back(imm(0ul));
    if (keyword_names)
        call_args.push_back(imm(keyword_names));

    return emitPPCall((void*)runtimeCall, call_args, 2, 640, node, type_recorder);
#else
    RewriterVar* argspec_var = imm(argspec.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size()) {
        args_array = allocArgs(args);
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

    return call(false, (void*)runtimeCallHelper, call_args);
#endif
}

RewriterVar* JitFragmentWriter::emitUnaryop(RewriterVar* v, int op_type) {
    return emitPPCall((void*)unaryop, { v, imm(op_type) }, 2, 160);
}

RewriterVar* JitFragmentWriter::emitUnpackIntoArray(RewriterVar* v, uint64_t num) {
    RewriterVar* array = call(false, (void*)unpackIntoArray, v, imm(num));
    return array;
}

RewriterVar* JitFragmentWriter::emitYield(RewriterVar* v) {
    RewriterVar* generator = getInterp()->getAttr(ASTInterpreterJitInterface::getGeneratorOffset());
    return call(false, (void*)yield, generator, v);
}

void JitFragmentWriter::emitDelAttr(RewriterVar* target, BoxedString* attr) {
    emitPPCall((void*)delattr, { target, imm(attr) }, 1, 512);
}

void JitFragmentWriter::emitDelGlobal(BoxedString* name) {
    RewriterVar* globals = getInterp()->getAttr(ASTInterpreterJitInterface::getGlobalsOffset());
    emitPPCall((void*)delGlobal, { globals, imm(name) }, 1, 512);
}

void JitFragmentWriter::emitDelItem(RewriterVar* target, RewriterVar* slice) {
    emitPPCall((void*)delitem, { target, slice }, 1, 512);
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
    RewriterVar* next = imm(b);
    addAction([=]() { _emitJump(b, next, num_bytes_exit); }, { next }, ActionType::NORMAL);
}

void JitFragmentWriter::emitOSRPoint(AST_Jump* node) {
    RewriterVar* node_var = imm(node);
    RewriterVar* result = createNewVar();
    addAction([=]() { _emitOSRPoint(result, node_var); }, { result, node_var, getInterp() }, ActionType::NORMAL);
}

void JitFragmentWriter::emitPrint(RewriterVar* dest, RewriterVar* var, bool nl) {
    if (!dest)
        dest = call(false, (void*)getSysStdout);
    if (!var)
        var = imm(0ul);
    call(false, (void*)printHelper, dest, var, imm(nl));
}

void JitFragmentWriter::emitRaise0() {
    call(false, (void*)ASTInterpreterJitInterface::raise0Helper, getInterp());
}

void JitFragmentWriter::emitRaise3(RewriterVar* arg0, RewriterVar* arg1, RewriterVar* arg2) {
    call(false, (void*)raise3, arg0, arg1, arg2);
}

void JitFragmentWriter::emitReturn(RewriterVar* v) {
    addAction([=]() { _emitReturn(v); }, { v }, ActionType::NORMAL);
}

void JitFragmentWriter::emitSetAttr(AST_expr* node, RewriterVar* obj, BoxedString* s, RewriterVar* attr) {
    emitPPCall((void*)setattr, { obj, imm(s), attr }, 2, 512, node);
}

void JitFragmentWriter::emitSetBlockLocal(InternedString s, RewriterVar* v) {
    local_syms[s] = v;
}

void JitFragmentWriter::emitSetCurrentInst(AST_stmt* node) {
    getInterp()->setAttr(ASTInterpreterJitInterface::getCurrentInstOffset(), imm(node));
}

void JitFragmentWriter::emitSetExcInfo(RewriterVar* type, RewriterVar* value, RewriterVar* traceback) {
    call(false, (void*)ASTInterpreterJitInterface::setExcInfoHelper, getInterp(), type, value, traceback);
}

void JitFragmentWriter::emitSetGlobal(Box* global, BoxedString* s, RewriterVar* v) {
    emitPPCall((void*)setGlobal, { imm(global), imm(s), v }, 2, 512);
}

void JitFragmentWriter::emitSetItem(RewriterVar* target, RewriterVar* slice, RewriterVar* value) {
    emitPPCall((void*)setitem, { target, slice, value }, 2, 512);
}

void JitFragmentWriter::emitSetItemName(BoxedString* s, RewriterVar* v) {
    emitSetItem(emitGetBoxedLocals(), imm(s), v);
}

void JitFragmentWriter::emitSetLocal(InternedString s, int vreg, bool set_closure, RewriterVar* v) {
    assert(vreg >= 0);
    if (set_closure) {
        call(false, (void*)ASTInterpreterJitInterface::setLocalClosureHelper, getInterp(), imm(vreg),
#ifndef NDEBUG
             imm(asUInt(s).first), imm(asUInt(s).second),
#else
             imm(asUInt(s)),
#endif
             v);
    } else {
        vregs_array->setAttr(8 * vreg, v);
    }
}

void JitFragmentWriter::emitSideExit(RewriterVar* v, Box* cmp_value, CFGBlock* next_block) {
    RewriterVar* var = imm(cmp_value);
    RewriterVar* next_block_var = imm(next_block);
    addAction([=]() { _emitSideExit(v, var, next_block, next_block_var); }, { v, var, next_block_var },
              ActionType::NORMAL);
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
        const auto large_block_threshold = JitCodeBlock::code_size - 4096;
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

        std::unique_ptr<ICInfo> pp
            = registerCompiledPatchpoint(start_addr, slowpath_start, initialization_info.continue_addr,
                                         slowpath_rtn_addr, pp_info.ic.get(), pp_info.stack_info, LiveOutSet());
        pp->associateNodeWithICInfo(pp_info.node);
        pp.release();
    }

    void* next_fragment_start = (uint8_t*)block->code + assembler->bytesWritten();
    code_block.fragmentFinished(assembler->bytesWritten(), num_bytes_overlapping, next_fragment_start);

#if MOVING_GC
    // If JitFragmentWriter is destroyed, we don't necessarily want the ICInfo to be destroyed also,
    // because it may contain a list of references to pointers in generated code that still exists
    // and we need to keep those around.
    // TODO: When should these ICInfo be freed?
    registerGCTrackedICInfo(ic_info.release());
#endif

    return num_bytes_exit;
}

bool JitFragmentWriter::finishAssembly(int continue_offset) {
    return !assembler->hasFailed();
}


RewriterVar* JitFragmentWriter::allocArgs(const llvm::ArrayRef<RewriterVar*> args) {
    auto num = args.size();
    assert(num);
    RewriterVar* array = allocate(num);
    for (int i = 0; i < num; ++i)
        array->setAttr(sizeof(void*) * i, args[i]);
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

RewriterVar* JitFragmentWriter::emitPPCall(void* func_addr, llvm::ArrayRef<RewriterVar*> args, int num_slots,
                                           int slot_size, AST* ast_node, TypeRecorder* type_recorder) {
    RewriterVar::SmallVector args_vec(args.begin(), args.end());
#if ENABLE_BASELINEJIT_ICS
    RewriterVar* result = createNewVar();

    int args_size = args.size();
    RewriterVar** _args = (RewriterVar**)regionAlloc(sizeof(RewriterVar*) * args_size);
    memcpy(_args, args.begin(), sizeof(RewriterVar*) * args_size);

    addAction([=]() {
        this->_emitPPCall(result, func_addr, llvm::ArrayRef<RewriterVar*>(_args, args_size), num_slots, slot_size,
                          ast_node);
    }, args, ActionType::NORMAL);

    if (type_recorder) {
        RewriterVar* type_recorder_var = imm(type_recorder);
        RewriterVar* obj_cls_var = result->getAttr(offsetof(Box, cls));
        addAction([=]() { _emitRecordType(type_recorder_var, obj_cls_var); }, { type_recorder_var, obj_cls_var },
                  ActionType::NORMAL);
        return result;
    }
    return result;
#else
    assert(args_vec.size() < 7);
    return call(false, func_addr, args_vec);
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
    assert(gc::isValidGCObject(r));
    return recordType(type_recorder, r);
}

Box* JitFragmentWriter::createDictHelper(uint64_t num, Box** keys, Box** values) {
    BoxedDict* dict = (BoxedDict*)createDict();
    for (uint64_t i = 0; i < num; ++i) {
        assert(gc::isValidGCObject(keys[i]));
        assert(gc::isValidGCObject(values[i]));
        dict->d[keys[i]] = values[i];
    }
    return dict;
}

Box* JitFragmentWriter::createListHelper(uint64_t num, Box** data) {
    BoxedList* list = (BoxedList*)createList();
    list->ensure(num);
    for (uint64_t i = 0; i < num; ++i) {
        assert(gc::isValidGCObject(data[i]));
        listAppendInternal(list, data[i]);
    }
    return list;
}

Box* JitFragmentWriter::createSetHelper(uint64_t num, Box** data) {
    BoxedSet* set = (BoxedSet*)createSet();
    for (int i = 0; i < num; ++i)
        set->s.insert(data[i]);
    return set;
}

Box* JitFragmentWriter::createTupleHelper(uint64_t num, Box** data) {
    return BoxedTuple::create(num, data);
}

Box* JitFragmentWriter::exceptionMatchesHelper(Box* obj, Box* cls) {
    return boxBool(exceptionMatches(obj, cls));
}

Box* JitFragmentWriter::hasnextHelper(Box* b) {
    return boxBool(pyston::hasnext(b));
}

Box* JitFragmentWriter::nonzeroHelper(Box* b) {
    return boxBool(b->nonzeroIC());
}

Box* JitFragmentWriter::notHelper(Box* b) {
    return boxBool(!b->nonzeroIC());
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
    val_var->bumpUse();

    {
        assembler::ForwardJump jnz(*assembler, assembler::COND_NOT_ZERO);
        assembler->mov(assembler::Immediate((uint64_t)name), assembler::RDI);
        assembler->mov(assembler::Immediate((void*)assertNameDefinedHelper), assembler::R11);
        assembler->callq(assembler::R11);
    }
}

void JitFragmentWriter::_emitJump(CFGBlock* b, RewriterVar* block_next, int& size_of_exit_to_interp) {
    size_of_exit_to_interp = 0;
    if (b->code) {
        int64_t offset = (uint64_t)b->code - ((uint64_t)entry_code + code_offset);
        if (isLargeConstant(offset)) {
            assembler->mov(assembler::Immediate(b->code), assembler::R11);
            assembler->jmpq(assembler::R11);
        } else
            assembler->jmp(assembler::JumpDestination::fromStart(offset));
    } else {
        int num_bytes = assembler->bytesWritten();
        block_next->getInReg(assembler::RAX, true);
        assembler->add(assembler::Immediate(JitCodeBlock::sp_adjustment), assembler::RSP);
        assembler->pop(assembler::R12);
        assembler->pop(assembler::R14);
        assembler->retq();

        // make sure we have at least 'min_patch_size' of bytes available.
        for (int i = assembler->bytesWritten() - num_bytes; i < min_patch_size; ++i)
            assembler->trap(); // we could use nops but traps may help if something goes wrong

        size_of_exit_to_interp = assembler->bytesWritten() - num_bytes;
        assert(assembler->hasFailed() || size_of_exit_to_interp >= min_patch_size);
    }
    block_next->bumpUse();
}

void JitFragmentWriter::_emitOSRPoint(RewriterVar* result, RewriterVar* node_var) {
    RewriterVar::SmallVector args;
    args.push_back(getInterp());
    args.push_back(node_var);
    _call(result, false, (void*)ASTInterpreterJitInterface::doOSRHelper, args, RewriterVar::SmallVector());
    auto result_reg = result->getInReg(assembler::RDX);
    result->bumpUse();

    assembler->test(result_reg, result_reg);
    {
        assembler::ForwardJump je(*assembler, assembler::COND_EQUAL);
        assembler->clear_reg(assembler::RAX);
        assembler->add(assembler::Immediate(JitCodeBlock::sp_adjustment), assembler::RSP);
        assembler->pop(assembler::R12);
        assembler->pop(assembler::R14);
        assembler->retq();
    }

    assertConsistent();
}

void JitFragmentWriter::_emitPPCall(RewriterVar* result, void* func_addr, llvm::ArrayRef<RewriterVar*> args,
                                    int num_slots, int slot_size, AST* ast_node) {
    assembler::Register r = allocReg(assembler::R11);

    if (args.size() > 6) { // only 6 args can get passed in registers.
        assert(args.size() <= 6 + JitCodeBlock::num_stack_args);
        for (int i = 6; i < args.size(); ++i) {
            assembler::Register reg = args[i]->getInReg(Location::any(), true);
            assembler->mov(reg, assembler::Indirect(assembler::RSP, sizeof(void*) * (i - 6)));
        }
        RewriterVar::SmallVector reg_args(args.begin(), args.begin() + 6);
        assert(reg_args.size() == 6);
        _setupCall(false, reg_args, RewriterVar::SmallVector());
    } else
        _setupCall(false, args, RewriterVar::SmallVector());

    if (failed)
        return;

    // make sure setupCall doesn't use R11
    assert(vars_by_location.count(assembler::R11) == 0);

    int pp_size = slot_size * num_slots;

    // make space for patchpoint
    uint8_t* pp_start = rewrite->getSlotStart() + assembler->bytesWritten();
    constexpr int call_size = 13;
    assembler->skipBytes(pp_size + call_size);
    uint8_t* pp_end = rewrite->getSlotStart() + assembler->bytesWritten();
    assert(assembler->hasFailed() || (pp_start + pp_size + call_size == pp_end));

    std::unique_ptr<ICSetupInfo> setup_info(
        ICSetupInfo::initialize(true, num_slots, slot_size, ICSetupInfo::Generic, NULL));

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

    for (RewriterVar* arg : args) {
        arg->bumpUse();
    }

    assertConsistent();

    StackInfo stack_info(pp_scratch_size, pp_scratch_location);
    pp_infos.emplace_back(PPInfo{ func_addr, pp_start, pp_end, std::move(setup_info), stack_info, ast_node });

    assert(vars_by_location.count(assembler::RAX) == 0);
    result->initializeInReg(assembler::RAX);
    assertConsistent();

    result->releaseIfNoUses();
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
    assembler->incl(last_seen_count);

    type_recorder_var->bumpUse();
    obj_cls_var->bumpUse();
}

void JitFragmentWriter::_emitReturn(RewriterVar* return_val) {
    return_val->getInReg(assembler::RDX, true);
    assembler->clear_reg(assembler::RAX);
    assembler->add(assembler::Immediate(JitCodeBlock::sp_adjustment), assembler::RSP);
    assembler->pop(assembler::R12);
    assembler->pop(assembler::R14);
    assembler->retq();
    return_val->bumpUse();
}

void JitFragmentWriter::_emitSideExit(RewriterVar* var, RewriterVar* val_constant, CFGBlock* next_block,
                                      RewriterVar* next_block_var) {
    assert(val_constant->is_constant);
    assert(next_block_var->is_constant);
    uint64_t val = val_constant->constant_value;

    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }

    {
        assembler::ForwardJump jne(*assembler, assembler::COND_EQUAL);
        int exit_size = 0;
        _emitJump(next_block, next_block_var, exit_size);
        if (exit_size) {
            RELEASE_ASSERT(!side_exit_patch_location.first,
                           "if we start to emit more than one side exit we should make this a vector");
            side_exit_patch_location = std::make_pair(next_block, assembler->bytesWritten() - exit_size);
        }
    }

    var->bumpUse();
    val_constant->bumpUse();

    assertConsistent();
}
}
