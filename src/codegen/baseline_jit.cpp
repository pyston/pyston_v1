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
#include "core/cfg.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"

namespace pyston {

static llvm::DenseSet<CFGBlock*> blocks_aborted;
static llvm::DenseMap<CFGBlock*, std::vector<void*>> block_patch_locations;

JitCodeBlock::JitCodeBlock(llvm::StringRef name)
    : frame_manager(false /* don't omit frame pointers */),
      code(new uint8_t[code_size]),
      entry_offset(0),
      a(code.get(), code_size),
      is_currently_writing(false),
      asm_failed(false) {
    static StatCounter num_jit_code_blocks("num_baselinejit_code_blocks");
    num_jit_code_blocks.log();
    static StatCounter num_jit_total_bytes("num_baselinejit_total_bytes");
    num_jit_total_bytes.log(code_size);

    // emit prolog
    a.push(assembler::RBP);
    a.mov(assembler::RSP, assembler::RBP);

    static_assert(scratch_size % 16 == 0, "stack aligment code depends on this");
    // subtract scratch size + 8bytes to align stack after the push.
    a.sub(assembler::Immediate(scratch_size + 8), assembler::RSP);
    a.push(assembler::RDI);                                               // push interpreter pointer
    a.jmp(assembler::Indirect(assembler::RSI, offsetof(CFGBlock, code))); // jump to block

    entry_offset = a.bytesWritten();

    // generate eh frame...
    frame_manager.writeAndRegister(code.get(), code_size);

    g.func_addr_registry.registerFunction(("bjit: " + name).str(), code.get(), code_size, NULL);
}

std::unique_ptr<JitFragmentWriter> JitCodeBlock::newFragment(CFGBlock* block, int patch_jump_offset) {
    if (is_currently_writing || blocks_aborted.count(block))
        return std::unique_ptr<JitFragmentWriter>();

    is_currently_writing = true;

    StackInfo stack_info(scratch_size, 16);
    std::unordered_set<int> live_outs;

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
    addLocationToVar(interp, Location(Location::Stack, 0));
    interp->setAttr(ASTInterpreterJitInterface::getCurrentBlockOffset(), imm(block));
}

RewriterVar* JitFragmentWriter::imm(uint64_t val) {
    return loadConst(val);
}

RewriterVar* JitFragmentWriter::imm(void* val) {
    return loadConst((uint64_t)val);
}

RewriterVar* JitFragmentWriter::emitAugbinop(RewriterVar* lhs, RewriterVar* rhs, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)augbinopICHelper, imm(new AugBinopIC), lhs, rhs, imm(op_type));
#else
    return call(false, (void*)augbinop, lhs, rhs, imm(op_type));
#endif
}

RewriterVar* JitFragmentWriter::emitBinop(RewriterVar* lhs, RewriterVar* rhs, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)binopICHelper, imm(new BinopIC), lhs, rhs, imm(op_type));
#else
    return call(false, (void*)binop, lhs, rhs, imm(op_type));
#endif
}

RewriterVar* JitFragmentWriter::emitCallattr(RewriterVar* obj, BoxedString* attr, CallattrFlags flags,
                                             const llvm::ArrayRef<RewriterVar*> args,
                                             std::vector<BoxedString*>* keyword_names) {
    // We could make this faster but for now: keep it simple, stupid...
    RewriterVar* attr_var = imm(attr);
    RewriterVar* flags_var = imm(flags.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size())
        args_array = allocArgs(args);
    else
        RELEASE_ASSERT(!keyword_names_var, "0 args but keyword names are set");

    bool use_ic = false;

    RewriterVar::SmallVector call_args;
    call_args.push_back(obj);
    call_args.push_back(attr_var);
    call_args.push_back(flags_var);

#if ENABLE_BASELINEJIT_ICS
    if (!keyword_names_var
        && flags.argspec.totalPassed() < 4) { // looks like runtime ICs with 7 or more args don't work right now..
        use_ic = true;
        call_args.push_back(imm(new CallattrIC));
    }
#endif

    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

#if ENABLE_BASELINEJIT_ICS
    if (use_ic)
        return call(false, (void*)callattrHelperIC, call_args);
#endif
    return call(false, (void*)callattrHelper, call_args);
}

RewriterVar* JitFragmentWriter::emitCompare(RewriterVar* lhs, RewriterVar* rhs, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)compareICHelper, imm(new CompareIC), lhs, rhs, imm(op_type));
#else
    return call(false, (void*)compare, lhs, rhs, imm(op_type));
#endif
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
    return call(false, (void*)createSetHelper, imm(values.size()), allocArgs(values));
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

RewriterVar* JitFragmentWriter::emitGetAttr(RewriterVar* obj, BoxedString* s) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)getAttrICHelper, imm(new GetAttrIC), obj, imm(s));
#else
    return call(false, (void*)getattr, obj, imm(s));
#endif
}

RewriterVar* JitFragmentWriter::emitGetBlockLocal(InternedString s) {
    auto it = local_syms.find(s);
    if (it == local_syms.end())
        return emitGetLocal(s);
    return it->second;
}

RewriterVar* JitFragmentWriter::emitGetBoxedLocal(BoxedString* s) {
    return call(false, (void*)ASTInterpreterJitInterface::getBoxedLocalHelper, getInterp(), imm(s));
}

RewriterVar* JitFragmentWriter::emitGetBoxedLocals() {
    return call(false, (void*)ASTInterpreterJitInterface::getBoxedLocalsHelper, getInterp());
}

RewriterVar* JitFragmentWriter::emitGetClsAttr(RewriterVar* obj, BoxedString* s) {
    return call(false, (void*)getclsattr, obj, imm(s));
}

RewriterVar* JitFragmentWriter::emitGetGlobal(Box* global, BoxedString* s) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)getGlobalICHelper, imm(new GetGlobalIC), imm(global), imm(s));
#else
    return call(false, (void*)getGlobal, imm(global), imm(s));
#endif
}

RewriterVar* JitFragmentWriter::emitGetItem(RewriterVar* value, RewriterVar* slice) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)getitemICHelper, imm(new GetItemIC), value, slice);
#else
    return call(false, (void*)getitem, value, slice);
#endif
}

RewriterVar* JitFragmentWriter::emitGetLocal(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::getLocalHelper, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second));
#else
                imm(asUInt(s)));
#endif
}

RewriterVar* JitFragmentWriter::emitGetPystonIter(RewriterVar* v) {
    return call(false, (void*)getPystonIter, v);
}

RewriterVar* JitFragmentWriter::emitHasnext(RewriterVar* v) {
    return call(false, (void*)hasnextHelper, v);
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

RewriterVar* JitFragmentWriter::emitRuntimeCall(RewriterVar* obj, ArgPassSpec argspec,
                                                const llvm::ArrayRef<RewriterVar*> args,
                                                std::vector<BoxedString*>* keyword_names) {
    // We could make this faster but for now: keep it simple, stupid..
    RewriterVar* argspec_var = imm(argspec.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size()) {
        args_array = allocArgs(args);
    } else
        RELEASE_ASSERT(!keyword_names_var, "0 args but keyword names are set");

    bool use_ic = false;

    RewriterVar::SmallVector call_args;
    call_args.push_back(obj);
    call_args.push_back(argspec_var);


#if ENABLE_BASELINEJIT_ICS
    if (!keyword_names) { // looks like runtime ICs with 7 or more args don't work right now..
        use_ic = true;
        call_args.push_back(imm(new RuntimeCallIC));
    }
#endif

    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

#if ENABLE_BASELINEJIT_ICS
    if (use_ic)
        return call(false, (void*)runtimeCallHelperIC, call_args);
#endif
    return call(false, (void*)runtimeCallHelper, call_args);
}

RewriterVar* JitFragmentWriter::emitUnaryop(RewriterVar* v, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)unaryopICHelper, imm(new UnaryopIC), v, imm(op_type));
#else
    return call(false, (void*)unaryop, v, imm(op_type));
#endif
}

RewriterVar* JitFragmentWriter::emitUnpackIntoArray(RewriterVar* v, uint64_t num) {
    RewriterVar* array = call(false, (void*)unpackIntoArray, v, imm(num));
    return array;
}

RewriterVar* JitFragmentWriter::emitYield(RewriterVar* v) {
    return call(false, (void*)ASTInterpreterJitInterface::yieldHelper, getInterp(), v);
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
    call(false, (void*)raise0);
}

void JitFragmentWriter::emitRaise3(RewriterVar* arg0, RewriterVar* arg1, RewriterVar* arg2) {
    call(false, (void*)raise3, arg0, arg1, arg2);
}

void JitFragmentWriter::emitReturn(RewriterVar* v) {
    addAction([=]() { _emitReturn(v); }, { v }, ActionType::NORMAL);
}

void JitFragmentWriter::emitSetAttr(RewriterVar* obj, BoxedString* s, RewriterVar* attr) {
#if ENABLE_BASELINEJIT_ICS
    call(false, (void*)setAttrICHelper, imm(new SetAttrIC), obj, imm(s), attr);
#else
    call(false, (void*)setattr, obj, imm(s), attr);
#endif
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
#if ENABLE_BASELINEJIT_ICS
    call(false, (void*)setGlobalICHelper, imm(new SetGlobalIC), imm(global), imm(s), v);
#else
    call(false, (void*)setGlobal, imm(global), imm(s), v);
#endif
}

void JitFragmentWriter::emitSetItem(RewriterVar* target, RewriterVar* slice, RewriterVar* value) {
#if ENABLE_BASELINEJIT_ICS
    call(false, (void*)setitemICHelper, imm(new SetItemIC), target, slice, value);
#else
    call(false, (void*)setitem, target, slice, value);
#endif
}

void JitFragmentWriter::emitSetItemName(BoxedString* s, RewriterVar* v) {
    call(false, (void*)ASTInterpreterJitInterface::setItemNameHelper, getInterp(), imm(s), v);
}

void JitFragmentWriter::emitSetLocal(InternedString s, bool set_closure, RewriterVar* v) {
    void* func = set_closure ? (void*)ASTInterpreterJitInterface::setLocalClosureHelper
                             : (void*)ASTInterpreterJitInterface::setLocalHelper;

    call(false, func, getInterp(),
#ifndef NDEBUG
         imm(asUInt(s).first), imm(asUInt(s).second),
#else
         imm(asUInt(s)),
#endif
         v);
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
        code_block.fragmentAbort(true /* not_enough_space */);
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

    void* next_fragment_start = (uint8_t*)block->code + assembler->bytesWritten();
    code_block.fragmentFinished(assembler->bytesWritten(), num_bytes_overlapping, next_fragment_start);
    return num_bytes_exit;
}

bool JitFragmentWriter::finishAssembly(int continue_offset) {
    return !assembler->hasFailed();
}


RewriterVar* JitFragmentWriter::allocArgs(const llvm::ArrayRef<RewriterVar*> args) {
    auto num = args.size();
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

RewriterVar* JitFragmentWriter::getInterp() {
    return interp;
}


Box* JitFragmentWriter::augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}


Box* JitFragmentWriter::binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}

#if ENABLE_BASELINEJIT_ICS
Box* JitFragmentWriter::callattrHelperIC(Box* obj, BoxedString* attr, CallattrFlags flags, CallattrIC* ic, Box** args) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], flags.argspec.totalPassed());
    return ic->call(obj, attr, flags, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple), NULL,
                    NULL);
}
#endif
Box* JitFragmentWriter::callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, Box** args,
                                       std::vector<BoxedString*>* keyword_names) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], flags.argspec.totalPassed());
    Box* r = callattr(obj, attr, flags, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                      std::get<3>(arg_tuple), keyword_names);
    assert(gc::isValidGCObject(r));
    return r;
}

Box* JitFragmentWriter::compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
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

Box* JitFragmentWriter::getAttrICHelper(GetAttrIC* ic, Box* o, BoxedString* attr) {
    return ic->call(o, attr);
}

Box* JitFragmentWriter::getGlobalICHelper(GetGlobalIC* ic, Box* o, BoxedString* s) {
    return ic->call(o, s);
}

Box* JitFragmentWriter::getitemICHelper(GetItemIC* ic, Box* o, Box* attr) {
    return ic->call(o, attr);
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

#if ENABLE_BASELINEJIT_ICS
Box* JitFragmentWriter::runtimeCallHelperIC(Box* obj, ArgPassSpec argspec, RuntimeCallIC* ic, Box** args) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    return ic->call(obj, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                    std::get<3>(arg_tuple));
}
#endif
Box* JitFragmentWriter::runtimeCallHelper(Box* obj, ArgPassSpec argspec, Box** args,
                                          std::vector<BoxedString*>* keyword_names) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    return runtimeCall(obj, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                       std::get<3>(arg_tuple), keyword_names);
}

Box* JitFragmentWriter::setAttrICHelper(SetAttrIC* ic, Box* o, BoxedString* attr, Box* value) {
    return ic->call(o, attr, value);
}

Box* JitFragmentWriter::setGlobalICHelper(SetGlobalIC* ic, Box* o, BoxedString* s, Box* v) {
    return ic->call(o, s, v);
}

Box* JitFragmentWriter::setitemICHelper(SetItemIC* ic, Box* o, Box* attr, Box* value) {
    return ic->call(o, attr, value);
}

Box* JitFragmentWriter::unaryopICHelper(UnaryopIC* ic, Box* obj, int op) {
    return ic->call(obj, op);
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
        assembler->leave();
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
        assembler->mov(assembler::Immediate(0ul), assembler::RAX); // TODO: use xor
        assembler->leave();
        assembler->retq();
    }

    assertConsistent();
}

void JitFragmentWriter::_emitReturn(RewriterVar* return_val) {
    return_val->getInReg(assembler::RDX, true);
    assembler->mov(assembler::Immediate(0ul), assembler::RAX); // TODO: use xor
    assembler->leave();
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
