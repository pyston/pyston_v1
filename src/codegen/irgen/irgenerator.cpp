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

#include "codegen/irgen/irgenerator.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "analysis/type_analysis.h"
#include "asm_writing/icinfo.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "codegen/patchpoints.h"
#include "codegen/type_recording.h"
#include "core/bst.h"
#include "core/cfg.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" void dumpLLVM(void* _v) {
    llvm::Value* v = (llvm::Value*)_v;
    v->getType()->dump();
    v->dump();
}

IRGenState::IRGenState(BoxedCode* code, CompiledFunction* cf, llvm::Function* func, SourceInfo* source_info,
                       std::unique_ptr<PhiAnalysis> phis, const ParamNames* param_names, GCBuilder* gc,
                       llvm::MDNode* func_dbg_info, RefcountTracker* refcount_tracker)
    : code(code),
      cf(cf),
      func(func),
      source_info(source_info),
      phis(std::move(phis)),
      param_names(param_names),
      gc(gc),
      func_dbg_info(func_dbg_info),
      refcount_tracker(refcount_tracker),
      scratch_space(NULL),
      frame_info(NULL),
      globals(NULL),
      vregs(NULL),
      stmt(NULL),
      scratch_size(0) {
    assert(func);
    assert(cf->code_obj->source.get() == source_info); // I guess this is duplicate now
}

IRGenState::~IRGenState() {
}

llvm::Value* IRGenState::getPassedClosure() {
    assert(getScopeInfo().takesClosure());
    assert(passed_closure);
    return passed_closure;
}
llvm::Value* IRGenState::getCreatedClosure() {
    assert(getScopeInfo().createsClosure());
    assert(created_closure);
    return created_closure;
}
llvm::Value* IRGenState::getPassedGenerator() {
    assert(source_info->is_generator);
    assert(passed_generator);
    return passed_generator;
}

void IRGenState::setPassedClosure(llvm::Value* v) {
    assert(getScopeInfo().takesClosure());
    assert(!passed_closure);
    passed_closure = v;
}
void IRGenState::setCreatedClosure(llvm::Value* v) {
    assert(getScopeInfo().createsClosure());
    assert(!created_closure);
    created_closure = v;
}
void IRGenState::setPassedGenerator(llvm::Value* v) {
    assert(source_info->is_generator);
    assert(!passed_generator);
    passed_generator = v;
}

llvm::Value* IRGenState::getScratchSpace(int min_bytes) {
    llvm::BasicBlock& entry_block = getLLVMFunction()->getEntryBlock();

    if (scratch_space) {
        assert(scratch_space->getParent() == &entry_block);
        assert(scratch_space->isStaticAlloca());
        if (scratch_size >= min_bytes)
            return scratch_space;
    }

    llvm::AllocaInst* new_scratch_space;
    // If the entry block is currently empty, we have to be more careful:
    if (entry_block.begin() == entry_block.end()) {
        new_scratch_space = new llvm::AllocaInst(g.i8, getConstantInt(min_bytes, g.i64), "scratch", &entry_block);
    } else {
        new_scratch_space = new llvm::AllocaInst(g.i8, getConstantInt(min_bytes, g.i64), "scratch",
                                                 entry_block.getFirstInsertionPt());
    }

    assert(new_scratch_space->isStaticAlloca());

    if (scratch_space)
        scratch_space->replaceAllUsesWith(new_scratch_space);

    scratch_size = min_bytes;
    scratch_space = new_scratch_space;

    return scratch_space;
}

ExceptionStyle UnwindInfo::preferredExceptionStyle() const {
    if (FORCE_LLVM_CAPI_CALLS)
        return CAPI;

    // TODO: I think this makes more sense as a relative percentage rather
    // than an absolute threshold, but currently we don't count how many
    // times a statement was executed but didn't throw.
    //
    // In theory this means that eventually anything that throws will be viewed
    // as a highly-throwing statement, but I think that this is less bad than
    // it might be because the denominator will be roughly fixed since we will
    // tend to run this check after executing the statement a somewhat-fixed
    // number of times.
    // We might want to zero these out after we are done compiling, though.
    if (current_stmt->cxx_exception_count >= 10)
        return CAPI;

    return CXX;
}

static llvm::Value* getClosureParentGep(IREmitter& emitter, llvm::Value* closure) {
    static_assert(sizeof(Box) == offsetof(BoxedClosure, parent), "");
    static_assert(offsetof(BoxedClosure, parent) + sizeof(BoxedClosure*) == offsetof(BoxedClosure, nelts), "");
    return emitter.getBuilder()->CreateConstInBoundsGEP2_32(closure, 0, 1);
}

static llvm::Value* getClosureElementGep(IREmitter& emitter, llvm::Value* closure, size_t index) {
    static_assert(sizeof(Box) == offsetof(BoxedClosure, parent), "");
    static_assert(offsetof(BoxedClosure, parent) + sizeof(BoxedClosure*) == offsetof(BoxedClosure, nelts), "");
    static_assert(offsetof(BoxedClosure, nelts) + sizeof(size_t) == offsetof(BoxedClosure, elts), "");
    return emitter.getBuilder()->CreateGEP(
        closure,
        { llvm::ConstantInt::get(g.i32, 0), llvm::ConstantInt::get(g.i32, 3), llvm::ConstantInt::get(g.i32, index) });
}

static llvm::Value* getBoxedLocalsGep(llvm::IRBuilder<true>& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, exc) == 0, "");
    static_assert(sizeof(ExcInfo) == 24, "");
    static_assert(offsetof(FrameInfo, boxedLocals) == 24, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 1);
}

static llvm::Value* getExcinfoGep(llvm::IRBuilder<true>& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, exc) == 0, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 0);
}

template <typename Builder> static llvm::Value* getFrameObjGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, exc) == 0, "");
    static_assert(sizeof(ExcInfo) == 24, "");
    static_assert(sizeof(Box*) == 8, "");
    static_assert(offsetof(FrameInfo, frame_obj) == 32, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 2);
    // TODO: this could be made more resilient by doing something like
    // gep->accumulateConstantOffset(g.tm->getDataLayout(), ap_offset)
}

template <typename Builder> static llvm::Value* getPassedClosureGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, passed_closure) == 40, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 3);
}

template <typename Builder> static llvm::Value* getVRegsGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, vregs) == 48, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 4);
}

template <typename Builder> static llvm::Value* getNumVRegsGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, num_vregs) == 56, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 5);
}

template <typename Builder> static llvm::Value* getStmtGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, stmt) == 64, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 6);
}

template <typename Builder> static llvm::Value* getGlobalsGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, globals) == 72, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 7);
}

template <typename Builder> static llvm::Value* getCodeGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, code) == 72 + 16, "");
    return builder.CreateConstInBoundsGEP2_32(v, 0, 9);
}

void IRGenState::setupFrameInfoVar(llvm::Value* passed_closure, llvm::Value* passed_globals,
                                   llvm::Value* frame_info_arg) {
    /*
        There is a matrix of possibilities here.

        For complete (non-OSR) functions, we initialize the FrameInfo* with an alloca and
        set this->frame_info to that llvm value.
        - If the function has NAME-scope, we initialize the frame_info.boxedLocals to a dictionary
          and set this->boxed_locals to an llvm value which is that dictionary.
        - If it is non-NAME-scope, we leave it NULL, because  most of the time it won't
          be initialized (unless someone calls locals() or something).
          this->boxed_locals is unused within the IR, so we don't set it.

        If this is an OSR function, then a FrameInfo* is passed in as an argument, so we don't
        need to initialize it with an alloca, and frame_info is already
        pointer.
        - If the function is NAME-scope, we extract the boxedLocals from the frame_info in order
          to set this->boxed_locals.
    */
    assert(!frame_info);
    llvm::BasicBlock& entry_block = getLLVMFunction()->getEntryBlock();

    llvm::IRBuilder<true> builder(&entry_block);

    if (entry_block.begin() != entry_block.end())
        builder.SetInsertPoint(&entry_block, entry_block.getFirstInsertionPt());

    if (entry_block.getTerminator())
        builder.SetInsertPoint(entry_block.getTerminator());
    else
        builder.SetInsertPoint(&entry_block);

    llvm::AllocaInst* al_pointer_to_frame_info
        = builder.CreateAlloca(g.llvm_frame_info_type->getPointerTo(), NULL, "frame_info_ptr");

    if (frame_info_arg) {
        assert(!passed_closure);
        assert(!passed_globals);

        // The OSR case
        this->frame_info = frame_info_arg;

        // use vrags array from the interpreter
        vregs = builder.CreateLoad(getVRegsGep(builder, frame_info_arg));
        this->globals = builder.CreateLoad(getGlobalsGep(builder, frame_info_arg));
        getRefcounts()->setType(this->globals, RefType::BORROWED);

        if (getScopeInfo().usesNameLookup()) {
            // load frame_info.boxedLocals
            this->boxed_locals = builder.CreateLoad(getBoxedLocalsGep(builder, this->frame_info));
            getRefcounts()->setType(this->boxed_locals, RefType::BORROWED);
        }

        if (getScopeInfo().takesClosure()) {
            passed_closure = builder.CreateLoad(getPassedClosureGep(builder, frame_info_arg));
            getRefcounts()->setType(passed_closure, RefType::BORROWED);
            this->setPassedClosure(passed_closure);
        }
    } else {
        // The "normal" case
        llvm::AllocaInst* al = builder.CreateAlloca(g.llvm_frame_info_type, NULL, "frame_info");
        assert(al->isStaticAlloca());

        assert(!vregs);
        int num_user_visible_vregs = getSourceInfo()->cfg->getVRegInfo().getNumOfUserVisibleVRegs();
        if (num_user_visible_vregs > 0) {
            auto* vregs_alloca
                = builder.CreateAlloca(g.llvm_value_type_ptr, getConstantInt(num_user_visible_vregs), "vregs");
            // Clear the vregs array because 0 means undefined valued.
            builder.CreateMemSet(vregs_alloca, getConstantInt(0, g.i8),
                                 getConstantInt(num_user_visible_vregs * sizeof(Box*)), vregs_alloca->getAlignment());
            vregs = vregs_alloca;
        } else
            vregs = getNullPtr(g.llvm_value_type_ptr_ptr);

        // frame_info.exc.type = NULL
        llvm::Constant* null_value = getNullPtr(g.llvm_value_type_ptr);
        getRefcounts()->setType(null_value, RefType::BORROWED);
        getRefcounts()->setNullable(null_value, true);
        llvm::Value* exc_info = getExcinfoGep(builder, al);
        builder.CreateStore(null_value,
                            builder.CreateConstInBoundsGEP2_32(exc_info, 0, offsetof(ExcInfo, type) / sizeof(Box*)));

        // frame_info.boxedLocals = NULL
        llvm::Value* boxed_locals_gep = getBoxedLocalsGep(builder, al);
        builder.CreateStore(getNullPtr(g.llvm_value_type_ptr), boxed_locals_gep);

        if (getScopeInfo().usesNameLookup()) {
            // frame_info.boxedLocals = createDict()
            // (Since this can call into the GC, we have to initialize it to NULL first as we did above.)
            this->boxed_locals = builder.CreateCall(g.funcs.createDict);
            getRefcounts()->setType(this->boxed_locals, RefType::OWNED);
            auto inst = builder.CreateStore(this->boxed_locals, boxed_locals_gep);
            getRefcounts()->refConsumed(this->boxed_locals, inst);
        }

        // frame_info.frame_obj = NULL
        static llvm::Type* llvm_frame_obj_type_ptr
            = llvm::cast<llvm::StructType>(g.llvm_frame_info_type)->getElementType(2);
        auto null_frame = getNullPtr(llvm_frame_obj_type_ptr);
        getRefcounts()->setType(null_frame, RefType::BORROWED);
        builder.CreateStore(null_frame, getFrameObjGep(builder, al));

        // set  frame_info.passed_closure
        builder.CreateStore(passed_closure, getPassedClosureGep(builder, al));
        // set frame_info.globals
        auto globals_store = builder.CreateStore(passed_globals, getGlobalsGep(builder, al));
        getRefcounts()->refConsumed(passed_globals, globals_store);
        // set frame_info.vregs
        builder.CreateStore(vregs, getVRegsGep(builder, al));
        builder.CreateStore(getConstantInt(num_user_visible_vregs, g.i32), getNumVRegsGep(builder, al));
        builder.CreateStore(
            getRefcounts()->setType(embedRelocatablePtr(getCode(), g.llvm_code_type_ptr), RefType::BORROWED),
            getCodeGep(builder, al));

        this->frame_info = al;
        this->globals = passed_globals;

        builder.CreateCall(g.funcs.initFrame, this->frame_info);
    }

    stmt = getStmtGep(builder, frame_info);
    builder.CreateStore(this->frame_info, al_pointer_to_frame_info);
    // Create stackmap to make a pointer to the frame_info location known
    PatchpointInfo* info = PatchpointInfo::create(getCurFunction(), 0, 0, 0);
    std::vector<llvm::Value*> args;
    args.push_back(getConstantInt(info->getId(), g.i64));
    args.push_back(getConstantInt(0, g.i32));
    args.push_back(al_pointer_to_frame_info);
    info->setNumFrameArgs(1);
    info->setIsFrameInfoStackmap();
    builder.CreateCall(llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::experimental_stackmap), args);
}

llvm::Value* IRGenState::getFrameInfoVar() {
    assert(frame_info);
    return frame_info;
}

llvm::Value* IRGenState::getBoxedLocalsVar() {
    assert(getScopeInfo().usesNameLookup());
    assert(this->boxed_locals != NULL);
    return this->boxed_locals;
}

llvm::Value* IRGenState::getVRegsVar() {
    assert(vregs);
    return vregs;
}

llvm::Value* IRGenState::getStmtVar() {
    assert(stmt);
    return stmt;
}

const ScopingResults& IRGenState::getScopeInfo() {
    return getSourceInfo()->scoping;
}

llvm::Value* IRGenState::getGlobals() {
    assert(globals);
    return globals;
}

llvm::Value* IRGenState::getGlobalsIfCustom() {
    if (source_info->scoping.areGlobalsFromModule())
        return getNullPtr(g.llvm_value_type_ptr);
    return getGlobals();
}

// This is a hacky little constant that should only be used by the underlying exception-propagation code.
// But it means that we are calling a function that 1) throws a C++ exception, and 2) we explicitly want to
// disable generation of a C++ fixup.  This is really just for propagating an exception after it had gotten caught.
#define NO_CXX_INTERCEPTION ((llvm::BasicBlock*)-1)

llvm::Value* IREmitter::ALWAYS_THROWS = ((llvm::Value*)1);

class IREmitterImpl : public IREmitter {
private:
    IRGenState* irstate;
    std::unique_ptr<IRBuilder> builder;
    llvm::BasicBlock*& curblock;
    IRGenerator* irgenerator;

    void emitPendingCallsCheck(llvm::BasicBlock* exc_dest) {
#if ENABLE_SIGNAL_CHECKING
        auto&& builder = *getBuilder();

        llvm::GlobalVariable* pendingcalls_to_do_gv = g.cur_module->getGlobalVariable("_pendingcalls_to_do");
        if (!pendingcalls_to_do_gv) {
            static_assert(sizeof(_pendingcalls_to_do) == 4, "");
            pendingcalls_to_do_gv = new llvm::GlobalVariable(
                *g.cur_module, g.i32, false, llvm::GlobalValue::ExternalLinkage, 0, "_pendingcalls_to_do");
            pendingcalls_to_do_gv->setAlignment(4);
        }

        llvm::BasicBlock* cur_block = builder.GetInsertBlock();

        llvm::BasicBlock* pendingcalls_set = createBasicBlock("_pendingcalls_set");
        pendingcalls_set->moveAfter(cur_block);
        llvm::BasicBlock* join_block = createBasicBlock("continue_after_pendingcalls_check");
        join_block->moveAfter(pendingcalls_set);

        llvm::Value* pendingcalls_to_do_val = builder.CreateLoad(pendingcalls_to_do_gv, true /* volatile */);
        llvm::Value* is_zero
            = builder.CreateICmpEQ(pendingcalls_to_do_val, getConstantInt(0, pendingcalls_to_do_val->getType()));

        llvm::Metadata* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), llvm::ConstantAsMetadata::get(getConstantInt(1000)),
                llvm::ConstantAsMetadata::get(getConstantInt(1)) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Metadata*>(md_vals));


        builder.CreateCondBr(is_zero, join_block, pendingcalls_set, branch_weights);
        {
            setCurrentBasicBlock(pendingcalls_set);

            if (exc_dest) {
                builder.CreateInvoke(g.funcs.makePendingCalls, join_block, exc_dest);
            } else {
                auto call = builder.CreateCall(g.funcs.makePendingCalls);
                irstate->getRefcounts()->setMayThrow(call);
                builder.CreateBr(join_block);
            }
        }

        cur_block = join_block;
        setCurrentBasicBlock(join_block);
#endif
    }

    void checkAndPropagateCapiException(const UnwindInfo& unw_info, llvm::Value* returned_val, llvm::Value* exc_val) {
        assert(exc_val);

        llvm::BasicBlock* normal_dest
            = llvm::BasicBlock::Create(g.context, curblock->getName(), irstate->getLLVMFunction());
        normal_dest->moveAfter(curblock);

        llvm::BasicBlock* exc_dest
            = irgenerator->getCAPIExcDest(curblock, unw_info.exc_dest, unw_info.current_stmt, unw_info.is_after_deopt);

        if (exc_val == ALWAYS_THROWS) {
            assert(returned_val->getType() == g.void_);

            llvm::BasicBlock* exc_dest = irgenerator->getCAPIExcDest(curblock, unw_info.exc_dest, unw_info.current_stmt,
                                                                     unw_info.is_after_deopt);
            getBuilder()->CreateBr(exc_dest);
        } else {
            assert(returned_val->getType() == exc_val->getType());
            llvm::Value* check_val = getBuilder()->CreateICmpEQ(returned_val, exc_val);
            llvm::BranchInst* nullcheck = getBuilder()->CreateCondBr(check_val, exc_dest, normal_dest);
        }

        setCurrentBasicBlock(normal_dest);
    }

    llvm::CallSite emitCall(const UnwindInfo& unw_info, llvm::Value* callee, const std::vector<llvm::Value*>& args,
                            ExceptionStyle target_exception_style, llvm::Value* capi_exc_value) {
        emitSetCurrentStmt(unw_info.current_stmt);

        if (target_exception_style == CAPI)
            assert(capi_exc_value);

        bool needs_cxx_interception;
        if (unw_info.exc_dest == NO_CXX_INTERCEPTION) {
            needs_cxx_interception = false;
        } else {
            bool needs_refcounting_fixup = false;
            needs_cxx_interception = (target_exception_style == CXX && (needs_refcounting_fixup || unw_info.hasHandler()
                                                                        || irstate->getExceptionStyle() == CAPI));
        }

        if (needs_cxx_interception) {
            // Create the invoke:
            llvm::BasicBlock* normal_dest
                = llvm::BasicBlock::Create(g.context, curblock->getName(), irstate->getLLVMFunction());

            llvm::BasicBlock* final_exc_dest;
            if (unw_info.hasHandler()) {
                final_exc_dest = unw_info.exc_dest;
            } else {
                final_exc_dest = NULL; // signal to reraise as a capi exception
            }

            llvm::BasicBlock* exc_dest = irgenerator->getCXXExcDest(unw_info);
            normal_dest->moveAfter(curblock);

            llvm::InvokeInst* rtn = getBuilder()->CreateInvoke(callee, normal_dest, exc_dest, args);

            ASSERT(target_exception_style == CXX, "otherwise need to call checkAndPropagateCapiException");

            // Note -- this code can often create critical edges between LLVM blocks.
            // The refcounting system has some support for handling this, but if we start generating
            // IR that it can't handle, we might have to break the critical edges here (or teach the
            // refcounting system how to do that.)

            // Normal case:
            getBuilder()->SetInsertPoint(normal_dest);
            curblock = normal_dest;
            if (unw_info.hasHandler())
                emitPendingCallsCheck(irgenerator->getCXXExcDest(unw_info));
            else
                emitPendingCallsCheck(NULL);
            return rtn;
        } else {
            llvm::CallInst* cs = getBuilder()->CreateCall(callee, args);

            if (target_exception_style == CXX)
                irstate->getRefcounts()->setMayThrow(cs);

            if (target_exception_style == CAPI)
                checkAndPropagateCapiException(unw_info, cs, capi_exc_value);

            emitPendingCallsCheck(NULL);
            return cs;
        }
    }

    llvm::CallSite emitPatchpoint(llvm::Type* return_type, std::unique_ptr<const ICSetupInfo> pp, llvm::Value* func,
                                  const std::vector<llvm::Value*>& args,
                                  const std::vector<llvm::Value*>& ic_stackmap_args, const UnwindInfo& unw_info,
                                  ExceptionStyle target_exception_style, llvm::Value* capi_exc_value) {
        if (pp == NULL)
            assert(ic_stackmap_args.size() == 0);

        if (target_exception_style == CAPI)
            assert(capi_exc_value);

        // Retrieve address of called function, currently handles the IR
        // embedConstantPtr() and embedRelocatablePtr() create.
        void* func_addr = nullptr;
        if (llvm::isa<llvm::ConstantExpr>(func)) {
            llvm::ConstantExpr* cast = llvm::cast<llvm::ConstantExpr>(func);
            auto opcode = cast->getOpcode();
            if (opcode == llvm::Instruction::IntToPtr) {
                auto operand = cast->getOperand(0);
                if (llvm::isa<llvm::ConstantInt>(operand))
                    func_addr = (void*)llvm::cast<llvm::ConstantInt>(operand)->getZExtValue();
            }
        }
        assert(func_addr);

        int pp_size = pp ? pp->totalSize() : CALL_ONLY_SIZE;
        auto calling_convention = pp ? pp->getCallingConvention() : (llvm::CallingConv::ID)llvm::CallingConv::C;
        PatchpointInfo* info
            = PatchpointInfo::create(currentFunction(), std::move(pp), ic_stackmap_args.size(), func_addr);

        int64_t pp_id = info->getId();


        std::vector<llvm::Value*> pp_args;
        pp_args.push_back(getConstantInt(pp_id, g.i64));
        pp_args.push_back(getConstantInt(pp_size, g.i32));
        if (ENABLE_JIT_OBJECT_CACHE)
            // add fixed dummy dest pointer, we will replace it with the correct address during stackmap processing
            pp_args.push_back(embedConstantPtr((void*)-1L, g.i8_ptr));
        else
            pp_args.push_back(func);
        pp_args.push_back(getConstantInt(args.size(), g.i32));

        pp_args.insert(pp_args.end(), args.begin(), args.end());

        int num_scratch_bytes = info->scratchSize();
        llvm::Value* scratch_space = irstate->getScratchSpace(num_scratch_bytes);
        pp_args.push_back(scratch_space);

        pp_args.insert(pp_args.end(), ic_stackmap_args.begin(), ic_stackmap_args.end());

        irgenerator->addFrameStackmapArgs(info, pp_args);

        llvm::Intrinsic::ID intrinsic_id;
        if (return_type->isIntegerTy() || return_type->isPointerTy()) {
            intrinsic_id = llvm::Intrinsic::experimental_patchpoint_i64;

            if (capi_exc_value && capi_exc_value->getType()->isPointerTy())
                capi_exc_value = getBuilder()->CreatePtrToInt(capi_exc_value, g.i64);
        } else if (return_type->isVoidTy()) {
            intrinsic_id = llvm::Intrinsic::experimental_patchpoint_void;
        } else if (return_type->isDoubleTy()) {
            intrinsic_id = llvm::Intrinsic::experimental_patchpoint_double;
        } else {
            return_type->dump();
            abort();
        }
        llvm::Function* patchpoint = this->getIntrinsic(intrinsic_id);
        llvm::CallSite rtn = this->emitCall(unw_info, patchpoint, pp_args, target_exception_style, capi_exc_value);
        rtn.setCallingConv(calling_convention);
        return rtn;
    }

public:
    explicit IREmitterImpl(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator)
        : irstate(irstate), builder(new IRBuilder(g.context)), curblock(curblock), irgenerator(irgenerator) {
        // Perf note: it seems to be more efficient to separately allocate the "builder" member,
        // even though we could allocate it in-line; maybe it's infrequently used enough that it's better
        // to not have it take up cache space.

        builder->setEmitter(this);
        builder->SetInsertPoint(curblock);
    }

    IRBuilder* getBuilder() override { return &*builder; }

    GCBuilder* getGC() override { return irstate->getGC(); }

    llvm::Function* getIntrinsic(llvm::Intrinsic::ID intrinsic_id) override {
        return llvm::Intrinsic::getDeclaration(g.cur_module, intrinsic_id);
    }

    llvm::Value* getScratch(int num_bytes) override { return irstate->getScratchSpace(num_bytes); }

    void releaseScratch(llvm::Value* scratch) override { assert(0); }

    CompiledFunction* currentFunction() override { return irstate->getCurFunction(); }
    llvm::BasicBlock* currentBasicBlock() override { return curblock; }

    void setCurrentBasicBlock(llvm::BasicBlock* bb) override {
        curblock = bb;
        getBuilder()->SetInsertPoint(curblock);
    }

    llvm::BasicBlock* createBasicBlock(const char* name) override {
        return llvm::BasicBlock::Create(g.context, name, irstate->getLLVMFunction());
    }

    // Our current frame introspection approach requires that we update the currently executed stmt before doing a call
    // to a function which could throw an exception, inspect the python call frame,...
    // Only patchpoint don't need to set the current statement because the stmt will be inluded in the stackmap args.
    void emitSetCurrentStmt(BST_stmt* stmt) {
        if (stmt)
            getBuilder()->CreateStore(stmt ? embedRelocatablePtr(stmt, g.llvm_bststmt_type_ptr)
                                           : getNullPtr(g.llvm_bststmt_type_ptr),
                                      irstate->getStmtVar());
    }

    llvm::Instruction* createCall(const UnwindInfo& unw_info, llvm::Value* callee,
                                  const std::vector<llvm::Value*>& args, ExceptionStyle target_exception_style = CXX,
                                  llvm::Value* capi_exc_value = NULL) override {
#ifndef NDEBUG
        // Copied the argument-type-checking from CallInst::init, since the patchpoint arguments don't
        // get checked.
        llvm::FunctionType* FTy
            = llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(callee->getType())->getElementType());

        assert((args.size() == FTy->getNumParams() || (FTy->isVarArg() && args.size() > FTy->getNumParams()))
               && "Calling a function with wrong number of args!");

        for (unsigned i = 0; i != args.size(); ++i) {
            if (!(i >= FTy->getNumParams() || FTy->getParamType(i) == args[i]->getType())) {
                llvm::errs() << "Expected: " << *FTy->getParamType(i) << '\n';
                llvm::errs() << "Got: " << *args[i]->getType() << '\n';
                ASSERT(0, "Calling a function with a bad type for argument %d!", i);
            }
        }
#endif
        return emitCall(unw_info, callee, args, target_exception_style, capi_exc_value).getInstruction();
    }

    llvm::Instruction* createCall(const UnwindInfo& unw_info, llvm::Value* callee,
                                  ExceptionStyle target_exception_style = CXX,
                                  llvm::Value* capi_exc_value = NULL) override {
        return createCall(unw_info, callee, std::vector<llvm::Value*>(), target_exception_style, capi_exc_value);
    }

    llvm::Instruction* createCall(const UnwindInfo& unw_info, llvm::Value* callee, llvm::Value* arg1,
                                  ExceptionStyle target_exception_style = CXX,
                                  llvm::Value* capi_exc_value = NULL) override {
        return createCall(unw_info, callee, std::vector<llvm::Value*>({ arg1 }), target_exception_style,
                          capi_exc_value);
    }

    llvm::Instruction* createCall2(const UnwindInfo& unw_info, llvm::Value* callee, llvm::Value* arg1,
                                   llvm::Value* arg2, ExceptionStyle target_exception_style = CXX,
                                   llvm::Value* capi_exc_value = NULL) override {
        return createCall(unw_info, callee, { arg1, arg2 }, target_exception_style, capi_exc_value);
    }

    llvm::Instruction* createCall3(const UnwindInfo& unw_info, llvm::Value* callee, llvm::Value* arg1,
                                   llvm::Value* arg2, llvm::Value* arg3, ExceptionStyle target_exception_style = CXX,
                                   llvm::Value* capi_exc_value = NULL) override {
        return createCall(unw_info, callee, { arg1, arg2, arg3 }, target_exception_style, capi_exc_value);
    }

    llvm::Instruction* createIC(std::unique_ptr<const ICSetupInfo> pp, void* func_addr,
                                const std::vector<llvm::Value*>& args, const UnwindInfo& unw_info,
                                ExceptionStyle target_exception_style = CXX,
                                llvm::Value* capi_exc_value = NULL) override {
        std::vector<llvm::Value*> stackmap_args;
        auto return_type = pp->hasReturnValue() ? g.i64 : g.void_;
        llvm::CallSite rtn
            = emitPatchpoint(return_type, std::move(pp), embedConstantPtr(func_addr, g.i8->getPointerTo()), args,
                             stackmap_args, unw_info, target_exception_style, capi_exc_value);
        return rtn.getInstruction();
    }

    llvm::Value* createDeopt(BST_stmt* current_stmt, llvm::Value* node_value) override {
        llvm::Instruction* v = createIC(createDeoptIC(), (void*)pyston::deopt, { node_value },
                                        UnwindInfo(current_stmt, NULL, /* is_after_deopt*/ true));
        llvm::Value* rtn = createAfter<llvm::IntToPtrInst>(v, v, g.llvm_value_type_ptr, "");
        setType(rtn, RefType::OWNED);
        return rtn;
    }

    Box* getIntConstant(int64_t n) override { return irstate->getSourceInfo()->parent_module->getIntConstant(n); }

    Box* getFloatConstant(double d) override { return irstate->getSourceInfo()->parent_module->getFloatConstant(d); }

    void refConsumed(llvm::Value* v, llvm::Instruction* inst) override {
        irstate->getRefcounts()->refConsumed(v, inst);
    }

    void refUsed(llvm::Value* v, llvm::Instruction* inst) override { irstate->getRefcounts()->refUsed(v, inst); }

    llvm::Value* setType(llvm::Value* v, RefType reftype) override {
        assert(llvm::isa<PointerType>(v->getType()));

        irstate->getRefcounts()->setType(v, reftype);
        return v;
    }

    llvm::Value* setNullable(llvm::Value* v, bool nullable) override {
        irstate->getRefcounts()->setNullable(v, nullable);
        return v;
    }

    ConcreteCompilerVariable* getNone() override {
        llvm::Constant* none = embedRelocatablePtr(Py_None, g.llvm_value_type_ptr, "cNone");
        setType(none, RefType::BORROWED);
        return new ConcreteCompilerVariable(typeFromClass(none_cls), none);
    }
};

IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator) {
    return new IREmitterImpl(irstate, curblock, irgenerator);
}

const std::string CREATED_CLOSURE_NAME = "#created_closure";
const std::string PASSED_CLOSURE_NAME = "#passed_closure";
const std::string PASSED_GENERATOR_NAME = "#passed_generator";
const std::string FRAME_INFO_PTR_NAME = "#frame_info_ptr";

bool isIsDefinedName(llvm::StringRef name) {
    return startswith(name, "!is_defined_");
}

InternedString getIsDefinedName(InternedString name, InternedStringPool& interned_strings) {
    // TODO could cache this
    return interned_strings.get(("!is_defined_" + name.s()).str());
}

class IRGeneratorImpl : public IRGenerator {
private:
    IRGenState* irstate;

    llvm::BasicBlock* curblock;
    IREmitterImpl emitter;
    // symbol_table tracks which (non-global) python variables are bound to which CompilerVariables
    SymbolTable symbol_table;
    DefinednessTable definedness_vars;

    std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks;
    CFGBlock* myblock;
    TypeAnalysis* types;

    // These are some special values used for passing exception data between blocks;
    // this transfer is not explicitly represented in the CFG which is why it has special
    // handling here.  ie these variables are how we handle the special "invoke->landingpad"
    // value transfer, which doesn't involve the normal symbol name handling.
    //
    // These are the values that are incoming to a landingpad block:
    llvm::SmallVector<ExceptionState, 2> incoming_exc_state;
    // These are the values that are outgoing of an invoke block:
    llvm::SmallVector<ExceptionState, 2> outgoing_exc_state;
    llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> capi_exc_dests;
    llvm::DenseMap<llvm::BasicBlock*, llvm::PHINode*> capi_phis;

    enum State {
        RUNNING,  // normal
        DEAD,     // passed a Return statement; still syntatically valid but the code should not be compiled
        FINISHED, // passed a pseudo-node such as Branch or Jump; internal error if there are any more statements
    } state;

public:
    IRGeneratorImpl(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                    CFGBlock* myblock, TypeAnalysis* types)
        : irstate(irstate),
          curblock(entry_blocks[myblock]),
          emitter(irstate, curblock, this),
          symbol_table(myblock->cfg->getVRegInfo().getTotalNumOfVRegs()),
          definedness_vars(myblock->cfg->getVRegInfo().getTotalNumOfVRegs()),
          entry_blocks(entry_blocks),
          myblock(myblock),
          types(types),
          state(RUNNING) {}

    virtual CFGBlock* getCFGBlock() override { return myblock; }

private:
    OpInfo getOpInfoForNode(BST_stmt* ast, const UnwindInfo& unw_info) {
        assert(ast);

        return OpInfo(irstate->getEffortLevel(), unw_info, ICInfo::getICInfoForNode(ast));
    }

    OpInfo getEmptyOpInfo(const UnwindInfo& unw_info) { return OpInfo(irstate->getEffortLevel(), unw_info, NULL); }

    void createExprTypeGuard(llvm::Value* check_val, llvm::Value* node_value, BST_stmt* current_statement) {
        assert(check_val->getType() == g.i1);

        llvm::Metadata* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), llvm::ConstantAsMetadata::get(getConstantInt(1000)),
                llvm::ConstantAsMetadata::get(getConstantInt(1)) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Metadata*>(md_vals));

        // For some reason there doesn't seem to be the ability to place the new BB
        // right after the current bb (can only place it *before* something else),
        // but we can put it somewhere arbitrary and then move it.
        llvm::BasicBlock* success_bb
            = llvm::BasicBlock::Create(g.context, "check_succeeded", irstate->getLLVMFunction());
        success_bb->moveAfter(curblock);

        llvm::BasicBlock* deopt_bb = llvm::BasicBlock::Create(g.context, "check_failed", irstate->getLLVMFunction());

        llvm::BranchInst* guard = emitter.getBuilder()->CreateCondBr(check_val, success_bb, deopt_bb, branch_weights);

        curblock = deopt_bb;
        emitter.getBuilder()->SetInsertPoint(curblock);
        llvm::Value* v = emitter.createDeopt(current_statement, node_value);
        llvm::Instruction* ret_inst = emitter.getBuilder()->CreateRet(v);
        irstate->getRefcounts()->refConsumed(v, ret_inst);

        curblock = success_bb;
        emitter.getBuilder()->SetInsertPoint(curblock);
    }

    CompilerVariable* evalCheckExcMatch(BST_CheckExcMatch* node, const UnwindInfo& unw_info) {
        CompilerVariable* obj = evalVReg(node->vreg_value);
        CompilerVariable* cls = evalVReg(node->vreg_cls);

        ConcreteCompilerVariable* converted_obj = obj->makeConverted(emitter, obj->getBoxType());
        ConcreteCompilerVariable* converted_cls = cls->makeConverted(emitter, cls->getBoxType());

        llvm::Value* v = emitter.createCall(unw_info, g.funcs.exceptionMatches,
                                            { converted_obj->getValue(), converted_cls->getValue() });
        assert(v->getType() == g.i1);

        return boolFromI1(emitter, v);
    }

    CompilerVariable* evalLandingpad(BST_Landingpad* node, const UnwindInfo& unw_info) {
        ConcreteCompilerVariable* exc_type;
        ConcreteCompilerVariable* exc_value;
        ConcreteCompilerVariable* exc_tb;

        if (this->incoming_exc_state.size()) {
            if (incoming_exc_state.size() == 1) {
                exc_type = this->incoming_exc_state[0].exc_type;
                exc_value = this->incoming_exc_state[0].exc_value;
                exc_tb = this->incoming_exc_state[0].exc_tb;
            } else {
                llvm::PHINode* phi_exc_type
                    = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, incoming_exc_state.size());
                llvm::PHINode* phi_exc_value
                    = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, incoming_exc_state.size());
                llvm::PHINode* phi_exc_tb
                    = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, incoming_exc_state.size());
                emitter.setType(phi_exc_type, RefType::OWNED);
                emitter.setType(phi_exc_value, RefType::OWNED);
                emitter.setType(phi_exc_tb, RefType::OWNED);
                for (auto e : this->incoming_exc_state) {
                    phi_exc_type->addIncoming(e.exc_type->getValue(), e.from_block);
                    phi_exc_value->addIncoming(e.exc_value->getValue(), e.from_block);
                    phi_exc_tb->addIncoming(e.exc_tb->getValue(), e.from_block);
                    emitter.refConsumed(e.exc_type->getValue(), e.from_block->getTerminator());
                    emitter.refConsumed(e.exc_value->getValue(), e.from_block->getTerminator());
                    emitter.refConsumed(e.exc_tb->getValue(), e.from_block->getTerminator());
                }
                exc_type = new ConcreteCompilerVariable(UNKNOWN, phi_exc_type);
                exc_value = new ConcreteCompilerVariable(UNKNOWN, phi_exc_value);
                exc_tb = new ConcreteCompilerVariable(UNKNOWN, phi_exc_tb);
            }
        } else {
            // There can be no incoming exception if the irgenerator was able to prove that
            // an exception would not get thrown.
            // For example, the cfg code will conservatively assume that any name-access can
            // trigger an exception, but the irgenerator will know that definitely-defined
            // local symbols will not throw.
            emitter.getBuilder()->CreateUnreachable();

            // Hacky: create a new BB for any more code that we might generate
            llvm::BasicBlock* continue_bb
                = llvm::BasicBlock::Create(g.context, "cant_reach", irstate->getLLVMFunction());
            emitter.setCurrentBasicBlock(continue_bb);

            exc_type = exc_value = exc_tb = undefVariable();
            // TODO: we should just call endBlock instead.  It looks like the implementation
            // of that has some issues though, and it might end up generating code as well.  ANd
            // then I'm not sure that the higher-level irgen.cpp would handle that well.
            // endBlock(DEAD);
        }

        // clear this out to signal that we consumed them:
        this->incoming_exc_state.clear();

        return makeTuple({ exc_type, exc_value, exc_tb });
    }

    CompilerVariable* evalLocals(BST_Locals* node, const UnwindInfo& unw_info) {
        return new ConcreteCompilerVariable(UNKNOWN, irstate->getBoxedLocalsVar());
    }

    CompilerVariable* evalGetIter(BST_GetIter* node, const UnwindInfo& unw_info) {
        CompilerVariable* obj = evalVReg(node->vreg_value);
        auto rtn = obj->getPystonIter(emitter, getOpInfoForNode(node, unw_info));
        return rtn;
    }

    CompilerVariable* evalImportFrom(BST_ImportFrom* node, const UnwindInfo& unw_info) {
        CompilerVariable* module = evalVReg(node->vreg_module);
        ConcreteCompilerVariable* converted_module = module->makeConverted(emitter, module->getBoxType());

        CompilerVariable* name = evalVReg(node->vreg_name);
        ConcreteCompilerVariable* converted_name = name->makeConverted(emitter, name->getBoxType());

        llvm::Value* r = emitter.createCall2(unw_info, g.funcs.importFrom, converted_module->getValue(),
                                             converted_name->getValue());
        emitter.setType(r, RefType::OWNED);

        CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r);
        return v;
    }

    CompilerVariable* evalImportStar(BST_ImportStar* node, const UnwindInfo& unw_info) {
        RELEASE_ASSERT(irstate->getSourceInfo()->ast_type == AST_TYPE::Module,
                       "import * not supported in functions (yet)");

        CompilerVariable* module = evalVReg(node->vreg_name);
        ConcreteCompilerVariable* converted_module = module->makeConverted(emitter, module->getBoxType());

        llvm::Value* r
            = emitter.createCall2(unw_info, g.funcs.importStar, converted_module->getValue(), irstate->getGlobals());
        emitter.setType(r, RefType::OWNED);
        CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r);
        return v;
    }

    CompilerVariable* evalImportName(BST_ImportName* node, const UnwindInfo& unw_info) {
        int level = node->level;
        CompilerVariable* froms = evalVReg(node->vreg_from);
        ConcreteCompilerVariable* converted_froms = froms->makeConverted(emitter, froms->getBoxType());

        CompilerVariable* name = evalVReg(node->vreg_name);
        ConcreteCompilerVariable* converted_name = name->makeConverted(emitter, name->getBoxType());

        llvm::Value* imported
            = emitter.createCall(unw_info, g.funcs.import, { getConstantInt(level, g.i32), converted_froms->getValue(),
                                                             converted_name->getValue() });
        emitter.setType(imported, RefType::OWNED);
        ConcreteCompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, imported);
        return v;
    }

    CompilerVariable* evalNonzero(BST_Nonzero* node, const UnwindInfo& unw_info) {
        CompilerVariable* obj = evalVReg(node->vreg_value);

        CompilerVariable* rtn = obj->nonzero(emitter, getOpInfoForNode(node, unw_info));
        return rtn;
    }

    CompilerVariable* evalHasNext(BST_HasNext* node, const UnwindInfo& unw_info) {
        CompilerVariable* obj = evalVReg(node->vreg_value);

        CompilerVariable* rtn = obj->hasnext(emitter, getOpInfoForNode(node, unw_info));
        return rtn;
    }

    void doSetExcInfo(BST_SetExcInfo* node, const UnwindInfo& unw_info) {
        CompilerVariable* type = evalVReg(node->vreg_type);
        CompilerVariable* value = evalVReg(node->vreg_value);
        CompilerVariable* traceback = evalVReg(node->vreg_traceback);

        auto* builder = emitter.getBuilder();

        llvm::Value* frame_info = irstate->getFrameInfoVar();

        ConcreteCompilerVariable* converted_type = type->makeConverted(emitter, UNKNOWN);
        ConcreteCompilerVariable* converted_value = value->makeConverted(emitter, UNKNOWN);
        ConcreteCompilerVariable* converted_traceback = traceback->makeConverted(emitter, UNKNOWN);

        auto inst = emitter.createCall(
            UnwindInfo::cantUnwind(), g.funcs.setFrameExcInfo,
            { frame_info, converted_type->getValue(), converted_value->getValue(), converted_traceback->getValue() },
            NOEXC);
        emitter.refConsumed(converted_type->getValue(), inst);
        emitter.refConsumed(converted_value->getValue(), inst);
        emitter.refConsumed(converted_traceback->getValue(), inst);
    }

    void doUncacheExcInfo(BST_UncacheExcInfo* node, const UnwindInfo& unw_info) {
        auto* builder = emitter.getBuilder();

        llvm::Value* frame_info = irstate->getFrameInfoVar();
        llvm::Constant* v = getNullPtr(g.llvm_value_type_ptr);
        emitter.setType(v, RefType::BORROWED);

        emitter.createCall(UnwindInfo::cantUnwind(), g.funcs.setFrameExcInfo, { frame_info, v, v, v }, NOEXC);
    }

    void doPrintExpr(BST_PrintExpr* node, const UnwindInfo& unw_info) {
        CompilerVariable* obj = evalVReg(node->vreg_value);
        ConcreteCompilerVariable* converted = obj->makeConverted(emitter, obj->getBoxType());

        emitter.createCall(unw_info, g.funcs.printExprHelper, converted->getValue());
    }

    CompilerVariable* _evalBinExp(BST_stmt* node, CompilerVariable* left, CompilerVariable* right,
                                  AST_TYPE::AST_TYPE type, BinExpType exp_type, const UnwindInfo& unw_info) {
        assert(left);
        assert(right);

        if (type == AST_TYPE::In || type == AST_TYPE::NotIn) {
            CompilerVariable* r = right->contains(emitter, getOpInfoForNode(node, unw_info), left);
            ASSERT(r->getType() == BOOL, "%s gave %s", right->getType()->debugName().c_str(),
                   r->getType()->debugName().c_str());
            if (type == AST_TYPE::NotIn) {
                ConcreteCompilerVariable* converted = r->makeConverted(emitter, BOOL);
                // TODO: would be faster to just do unboxBoolNegated
                llvm::Value* raw = i1FromBool(emitter, converted);
                raw = emitter.getBuilder()->CreateXor(raw, getConstantInt(1, g.i1));
                r = boolFromI1(emitter, raw);
            }
            return r;
        }

        return left->binexp(emitter, getOpInfoForNode(node, unw_info), right, type, exp_type);
    }

    CompilerVariable* evalBinOp(BST_BinOp* node, const UnwindInfo& unw_info) {
        CompilerVariable* left = evalVReg(node->vreg_left);
        CompilerVariable* right = evalVReg(node->vreg_right);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        return this->_evalBinExp(node, left, right, node->op_type, BinOp, unw_info);
    }

    CompilerVariable* evalAugBinOp(BST_AugBinOp* node, const UnwindInfo& unw_info) {
        CompilerVariable* left = evalVReg(node->vreg_left);
        CompilerVariable* right = evalVReg(node->vreg_right);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        return this->_evalBinExp(node, left, right, node->op_type, AugBinOp, unw_info);
    }

    CompilerVariable* evalCompare(BST_Compare* node, const UnwindInfo& unw_info) {
        CompilerVariable* left = evalVReg(node->vreg_left);
        CompilerVariable* right = evalVReg(node->vreg_comparator);

        assert(left);
        assert(right);

        CompilerVariable* rtn = NULL;
        if (node->op == AST_TYPE::Is || node->op == AST_TYPE::IsNot)
            rtn = doIs(emitter, left, right, node->op == AST_TYPE::IsNot);
        else
            rtn = _evalBinExp(node, left, right, node->op, Compare, unw_info);
        return rtn;
    }

    CompilerVariable* evalCall(BST_Call* node, const UnwindInfo& unw_info) {
        bool is_callattr;
        bool callattr_clsonly = false;
        InternedString attr;
        CompilerVariable* func;
        int* vreg_elts = NULL;
        if (node->type == BST_TYPE::CallAttr) {
            is_callattr = true;
            callattr_clsonly = false;
            auto* attr_ast = bst_cast<BST_CallAttr>(node);
            vreg_elts = bst_cast<BST_CallAttr>(node)->elts;
            func = evalVReg(attr_ast->vreg_value);
            attr = attr_ast->attr;
        } else if (node->type == BST_TYPE::CallClsAttr) {
            is_callattr = true;
            callattr_clsonly = true;
            auto* attr_ast = bst_cast<BST_CallClsAttr>(node);
            vreg_elts = bst_cast<BST_CallClsAttr>(node)->elts;
            func = evalVReg(attr_ast->vreg_value);
            attr = attr_ast->attr;
        } else {
            is_callattr = false;
            auto* attr_ast = bst_cast<BST_CallFunc>(node);
            vreg_elts = bst_cast<BST_CallFunc>(node)->elts;
            func = evalVReg(attr_ast->vreg_func);
        }

        std::vector<CompilerVariable*> args;
        std::vector<BoxedString*>* keyword_names = NULL;
        if (node->num_keywords)
            keyword_names = node->keywords_names.get();

        for (int i = 0; i < node->num_args + node->num_keywords; i++) {
            CompilerVariable* a = evalVReg(vreg_elts[i]);
            args.push_back(a);
        }

        if (node->vreg_starargs != VREG_UNDEFINED)
            args.push_back(evalVReg(node->vreg_starargs));
        if (node->vreg_kwargs != VREG_UNDEFINED)
            args.push_back(evalVReg(node->vreg_kwargs));

        struct ArgPassSpec argspec(node->num_args, node->num_keywords, node->vreg_starargs != VREG_UNDEFINED,
                                   node->vreg_kwargs != VREG_UNDEFINED);


        // if (VERBOSITY("irgen") >= 1)
        //_addAnnotation("before_call");

        CompilerVariable* rtn;
        if (is_callattr) {
            CallattrFlags flags = {.cls_only = callattr_clsonly, .null_on_nonexistent = false, .argspec = argspec };
            rtn = func->callattr(emitter, getOpInfoForNode(node, unw_info), attr.getBox(), flags, args, keyword_names);
        } else {
            rtn = func->call(emitter, getOpInfoForNode(node, unw_info), argspec, args, keyword_names);
        }

        return rtn;
    }

    CompilerVariable* evalDict(BST_Dict* node, const UnwindInfo& unw_info) {
        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
        emitter.setType(v, RefType::OWNED);
        return new ConcreteCompilerVariable(DICT, v);
    }

    void _addAnnotation(const char* message) {
        llvm::Instruction* inst = emitter.getBuilder()->CreateCall(
            llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::donothing));
        llvm::Metadata* md_vals[] = { llvm::ConstantAsMetadata::get(getConstantInt(0)) };
        llvm::MDNode* mdnode = llvm::MDNode::get(g.context, md_vals);
        inst->setMetadata(message, mdnode);
    }

    CompilerVariable* evalList(BST_List* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->num_elts; i++) {
            CompilerVariable* value = evalVReg(node->elts[i]);
            elts.push_back(value);
        }

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createList);
        emitter.setType(v, RefType::OWNED);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(LIST, v);

        llvm::Value* f = g.funcs.listAppendInternal;
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(
            v, *llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(f->getType())->getElementType())
                    ->param_begin());

        for (int i = 0; i < node->num_elts; i++) {
            CompilerVariable* elt = elts[i];
            ConcreteCompilerVariable* converted = elt->makeConverted(emitter, elt->getBoxType());

            emitter.createCall2(unw_info, f, bitcast, converted->getValue());
        }
        return rtn;
    }

    ConcreteCompilerVariable* getEllipsis() {
        llvm::Constant* ellipsis = embedRelocatablePtr(Ellipsis, g.llvm_value_type_ptr, "cEllipsis");
        emitter.setType(ellipsis, RefType::BORROWED);
        auto ellipsis_cls = Ellipsis->cls;
        return new ConcreteCompilerVariable(typeFromClass(ellipsis_cls), ellipsis);
    }

    ConcreteCompilerVariable* _getGlobal(BST_LoadName* node, const UnwindInfo& unw_info) {
        if (node->id.s() == "None")
            return emitter.getNone();

        bool do_patchpoint = ENABLE_ICGETGLOBALS;
        if (do_patchpoint) {
            auto pp = createGetGlobalIC();

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(irstate->getGlobals());
            llvm_args.push_back(emitter.setType(embedRelocatablePtr(node->id.getBox(), g.llvm_boxedstring_type_ptr),
                                                RefType::BORROWED));

            llvm::Instruction* uncasted
                = emitter.createIC(std::move(pp), (void*)pyston::getGlobal, llvm_args, unw_info);
            llvm::Value* r = createAfter<llvm::IntToPtrInst>(uncasted, uncasted, g.llvm_value_type_ptr, "");
            emitter.setType(r, RefType::OWNED);
            return new ConcreteCompilerVariable(UNKNOWN, r);
        } else {
            llvm::Value* r = emitter.createCall2(
                unw_info, g.funcs.getGlobal, irstate->getGlobals(),
                emitter.setType(embedRelocatablePtr(node->id.getBox(), g.llvm_boxedstring_type_ptr),
                                RefType::BORROWED));
            emitter.setType(r, RefType::OWNED);
            return new ConcreteCompilerVariable(UNKNOWN, r);
        }
    }

    CompilerVariable* evalVReg(int vreg, bool is_kill = true) {
        assert(vreg != VREG_UNDEFINED);
        if (vreg < 0) {
            Box* o = irstate->getCode()->constant_vregs.getConstant(vreg);
            if (o->cls == int_cls) {
                return makeInt(((BoxedInt*)o)->n);
            } else if (o->cls == float_cls) {
                return makeFloat(((BoxedFloat*)o)->d);
            } else if (o->cls == complex_cls) {
                return makePureImaginary(emitter, o);
            } else if (o->cls == long_cls) {
                return makeLong(emitter, o);
            } else if (o->cls == str_cls) {
                llvm::Value* rtn = embedRelocatablePtr(o, g.llvm_value_type_ptr);
                emitter.setType(rtn, RefType::BORROWED);
                return new ConcreteCompilerVariable(STR, rtn);
            } else if (o->cls == unicode_cls) {
                llvm::Value* rtn = embedRelocatablePtr(o, g.llvm_value_type_ptr);
                emitter.setType(rtn, RefType::BORROWED);
                return new ConcreteCompilerVariable(typeFromClass(unicode_cls), rtn);
            } else if (o->cls == none_cls) {
                return emitter.getNone();
            } else if (o->cls == ellipsis_cls) {
                return getEllipsis();
            } else {
                RELEASE_ASSERT(0, "");
            }
        }
        CompilerVariable* rtn = symbol_table[vreg];
        if (is_kill) {
            symbol_table[vreg] = NULL;
            popDefinedVar(vreg, true /* allow_missing */);
        }
        return rtn;
    }


    CompilerVariable* evalLoadName(BST_LoadName* node, const UnwindInfo& unw_info) {
        // LoadName is never a kill
        auto&& scope_info = irstate->getScopeInfo();

        ScopeInfo::VarScopeType vst = node->lookup_type;
        assert(vst != ScopeInfo::VarScopeType::UNKNOWN);
        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            return _getGlobal(node, unw_info);
        } else if (vst == ScopeInfo::VarScopeType::DEREF) {
            assert(scope_info.takesClosure());

            // This is the information on how to look up the variable in the closure object.
            DerefInfo deref_info = scope_info.getDerefInfo(node);

            // This code is basically:
            // closure = created_closure;
            // closure = closure->parent;
            // [...]
            // closure = closure->parent;
            // closure->elts[deref_info.offset]
            // Where the parent lookup is done `deref_info.num_parents_from_passed_closure` times
            llvm::Value* closureValue = irstate->getPassedClosure();
            assert(closureValue);
            for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
                closureValue = emitter.getBuilder()->CreateLoad(getClosureParentGep(emitter, closureValue));
                emitter.setType(closureValue, RefType::BORROWED);
            }
            llvm::Value* lookupResult
                = emitter.getBuilder()->CreateLoad(getClosureElementGep(emitter, closureValue, deref_info.offset));
            emitter.setType(lookupResult, RefType::BORROWED);

            // If the value is NULL, the variable is undefined.
            // Create a branch on if the value is NULL.
            llvm::BasicBlock* success_bb
                = llvm::BasicBlock::Create(g.context, "deref_defined", irstate->getLLVMFunction());
            success_bb->moveAfter(curblock);
            llvm::BasicBlock* fail_bb
                = llvm::BasicBlock::Create(g.context, "deref_undefined", irstate->getLLVMFunction());

            llvm::Constant* null_value = getNullPtr(g.llvm_value_type_ptr);
            emitter.setType(null_value, RefType::BORROWED);
            llvm::Value* check_val = emitter.getBuilder()->CreateICmpEQ(lookupResult, null_value);
            llvm::BranchInst* non_null_check = emitter.getBuilder()->CreateCondBr(check_val, fail_bb, success_bb);

            // Case that it is undefined: call the assert fail function.
            curblock = fail_bb;
            emitter.getBuilder()->SetInsertPoint(curblock);

            llvm::CallSite call = emitter.createCall(unw_info, g.funcs.assertFailDerefNameDefined,
                                                     embedRelocatablePtr(node->id.c_str(), g.i8_ptr));
            call.setDoesNotReturn();
            emitter.getBuilder()->CreateUnreachable();

            // Case that it is defined: carry on in with the retrieved value.
            curblock = success_bb;
            emitter.getBuilder()->SetInsertPoint(curblock);

            return new ConcreteCompilerVariable(UNKNOWN, lookupResult);
        } else if (vst == ScopeInfo::VarScopeType::NAME) {
            llvm::Value* boxedLocals = irstate->getBoxedLocalsVar();
            llvm::Value* attr = embedRelocatablePtr(node->id.getBox(), g.llvm_boxedstring_type_ptr);
            emitter.setType(attr, RefType::BORROWED);
            llvm::Value* module = irstate->getGlobals();
            llvm::Value* r = emitter.createCall3(unw_info, g.funcs.boxedLocalsGet, boxedLocals, attr, module);
            emitter.setType(r, RefType::OWNED);
            return new ConcreteCompilerVariable(UNKNOWN, r);
        } else {
            // vst is one of {FAST, CLOSURE}
            assert(node->vreg >= 0);
            if (!symbol_table[node->vreg]) {
                // TODO should mark as DEAD here, though we won't end up setting all the names appropriately
                // state = DEAD;
                llvm::CallSite call = emitter.createCall(
                    unw_info, g.funcs.assertNameDefined,
                    { getConstantInt(0, g.i1), embedRelocatablePtr(node->id.c_str(), g.i8_ptr),
                      emitter.setType(embedRelocatablePtr(UnboundLocalError, g.llvm_class_type_ptr), RefType::BORROWED),
                      getConstantInt(true, g.i1) });
                call.setDoesNotReturn();
                return undefVariable();
            }

            auto is_defined_var = getDefinedVar(node->vreg, true);

            if (is_defined_var) {
                emitter.createCall(
                    unw_info, g.funcs.assertNameDefined,
                    { i1FromLLVMBool(emitter, is_defined_var), embedRelocatablePtr(node->id.c_str(), g.i8_ptr),
                      emitter.setType(embedRelocatablePtr(UnboundLocalError, g.llvm_class_type_ptr), RefType::BORROWED),
                      getConstantInt(true, g.i1) });

                // At this point we know the name must be defined (otherwise the assert would have fired):
                popDefinedVar(node->vreg);
            }

            CompilerVariable* rtn = symbol_table[node->vreg];
            return rtn;
        }
    }

    CompilerVariable* evalRepr(BST_Repr* node, const UnwindInfo& unw_info) {
        CompilerVariable* var = evalVReg(node->vreg_value);
        ConcreteCompilerVariable* cvar = var->makeConverted(emitter, var->getBoxType());

        std::vector<llvm::Value*> args{ cvar->getValue() };
        llvm::Instruction* uncasted = emitter.createCall(unw_info, g.funcs.repr, args);
        emitter.setType(uncasted, RefType::BORROWED); // Well, really it's owned, and we handoff the ref to the bitcast
        auto rtn = createAfter<llvm::BitCastInst>(uncasted, uncasted, g.llvm_value_type_ptr, "");
        emitter.setType(rtn, RefType::OWNED);

        return new ConcreteCompilerVariable(STR, rtn);
    }

    CompilerVariable* evalSet(BST_Set* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->num_elts; i++) {
            CompilerVariable* value = evalVReg(node->elts[i]);
            elts.push_back(value);
        }

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createSet);
        emitter.setType(v, RefType::OWNED);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(SET, v);

        static BoxedString* add_str = getStaticString("add");

        // insert the elements in reverse like cpython does
        // important for {1, 1L}
        for (auto it = elts.rbegin(), it_end = elts.rend(); it != it_end; ++it) {
            CompilerVariable* elt = *it;
            CallattrFlags flags = {.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(1) };
            CompilerVariable* r
                = rtn->callattr(emitter, getOpInfoForNode(node, unw_info), add_str, flags, { elt }, NULL);
        }

        return rtn;
    }

    CompilerVariable* evalMakeSlice(BST_MakeSlice* node, const UnwindInfo& unw_info) {
        CompilerVariable* start, *stop, *step;
        start = node->vreg_lower != VREG_UNDEFINED ? evalVReg(node->vreg_lower) : NULL;
        stop = node->vreg_upper != VREG_UNDEFINED ? evalVReg(node->vreg_upper) : NULL;
        step = node->vreg_step != VREG_UNDEFINED ? evalVReg(node->vreg_step) : NULL;

        return makeSlice(start, stop, step);
    }


    CompilerVariable* evalLoadAttr(BST_LoadAttr* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalVReg(node->vreg_value);
        CompilerVariable* rtn
            = value->getattr(emitter, getOpInfoForNode(node, unw_info), node->attr.getBox(), node->clsonly);
        return rtn;
    }

    CompilerVariable* evalLoadSub(BST_LoadSub* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalVReg(node->vreg_value);
        CompilerVariable* slice = evalVReg(node->vreg_slice);
        CompilerVariable* rtn = value->getitem(emitter, getOpInfoForNode(node, unw_info), slice);
        return rtn;
    }

    CompilerVariable* evalLoadSubSlice(BST_LoadSubSlice* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalVReg(node->vreg_value);
        CompilerVariable* lower = node->vreg_lower != VREG_UNDEFINED ? evalVReg(node->vreg_lower) : NULL;
        CompilerVariable* upper = node->vreg_upper != VREG_UNDEFINED ? evalVReg(node->vreg_upper) : NULL;
        CompilerVariable* slice = makeSlice(lower, upper, NULL);
        CompilerVariable* rtn = value->getitem(emitter, getOpInfoForNode(node, unw_info), slice);
        return rtn;
    }

    CompilerVariable* evalTuple(BST_Tuple* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->num_elts; i++) {
            CompilerVariable* value = evalVReg(node->elts[i]);
            elts.push_back(value);
        }

        CompilerVariable* rtn = makeTuple(elts);
        return rtn;
    }

    CompilerVariable* evalUnaryOp(BST_UnaryOp* node, const UnwindInfo& unw_info) {
        CompilerVariable* operand = evalVReg(node->vreg_operand);

        if (node->op_type == AST_TYPE::Not) {
            CompilerVariable* rtn = operand->nonzero(emitter, getOpInfoForNode(node, unw_info));

            assert(rtn->getType() == BOOL);
            llvm::Value* v = i1FromBool(emitter, static_cast<ConcreteCompilerVariable*>(rtn));
            assert(v->getType() == g.i1);

            llvm::Value* negated = emitter.getBuilder()->CreateNot(v);
            return boolFromI1(emitter, negated);
        } else {
            CompilerVariable* rtn = operand->unaryop(emitter, getOpInfoForNode(node, unw_info), node->op_type);
            return rtn;
        }
    }

    CompilerVariable* evalYield(BST_Yield* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = node->vreg_value != VREG_UNDEFINED ? evalVReg(node->vreg_value) : emitter.getNone();
        ConcreteCompilerVariable* convertedValue = value->makeConverted(emitter, value->getBoxType());

        std::vector<llvm::Value*> args;
        args.push_back(irstate->getPassedGenerator());
        args.push_back(convertedValue->getValue());
        args.push_back(getConstantInt(0, g.i32)); // the refcounting inserter handles yields specially and adds all
                                                  // owned objects as additional arguments to it

        // put the yield call at the beginning of a new basic block to make it easier for the refcounting inserter.
        llvm::BasicBlock* yield_block = emitter.createBasicBlock("yield_block");
        emitter.getBuilder()->CreateBr(yield_block);
        emitter.setCurrentBasicBlock(yield_block);

        // we do a capi call because it makes it easier for the refcounter to replace the instruction
        llvm::Instruction* rtn
            = emitter.createCall(unw_info, g.funcs.yield_capi, args, CAPI, getNullPtr(g.llvm_value_type_ptr));
        emitter.refConsumed(convertedValue->getValue(), rtn);
        emitter.setType(rtn, RefType::OWNED);
        emitter.setNullable(rtn, true);

        return new ConcreteCompilerVariable(UNKNOWN, rtn);
    }

    CompilerVariable* evalMakeClass(BST_MakeClass* mkclass, const UnwindInfo& unw_info) {
        assert(mkclass->type == BST_TYPE::MakeClass && mkclass->class_def->type == BST_TYPE::ClassDef);
        BST_ClassDef* node = mkclass->class_def;

        CompilerVariable* _bases_tuple = evalVReg(node->vreg_bases_tuple);
        ConcreteCompilerVariable* bases_tuple = _bases_tuple->makeConverted(emitter, _bases_tuple->getBoxType());

        std::vector<CompilerVariable*> decorators;
        for (int i = 0; i < node->num_decorator; ++i) {
            decorators.push_back(evalVReg(node->decorator[i]));
        }

        BoxedCode* code = node->code;
        assert(code);
        const ScopingResults& scope_info = code->source->scoping;

        // TODO duplication with _createFunction:
        llvm::Value* this_closure = NULL;
        if (scope_info.takesClosure()) {
            if (irstate->getScopeInfo().createsClosure()) {
                this_closure = irstate->getCreatedClosure();
            } else {
                assert(irstate->getScopeInfo().passesThroughClosure());
                this_closure = irstate->getPassedClosure();
            }
            assert(this_closure);
        }

        // TODO kind of silly to create the function just to usually-delete it afterwards;
        // one reason to do this is to pass the closure through if necessary,
        // but since the classdef can't create its own closure, shouldn't need to explicitly
        // create that scope to pass the closure through.
        assert(irstate->getSourceInfo()->scoping.areGlobalsFromModule());
        CompilerVariable* func = makeFunction(emitter, code, this_closure, irstate->getGlobalsIfCustom(), {});

        CompilerVariable* attr_dict = func->call(emitter, getEmptyOpInfo(unw_info), ArgPassSpec(0), {}, NULL);

        ConcreteCompilerVariable* converted_attr_dict = attr_dict->makeConverted(emitter, attr_dict->getBoxType());

        llvm::Value* classobj = emitter.createCall3(
            unw_info, g.funcs.createUserClass,
            emitter.setType(embedRelocatablePtr(node->name.getBox(), g.llvm_boxedstring_type_ptr), RefType::BORROWED),
            bases_tuple->getValue(), converted_attr_dict->getValue());
        emitter.setType(classobj, RefType::OWNED);

        // Note: createuserClass is free to manufacture non-class objects
        CompilerVariable* cls = new ConcreteCompilerVariable(UNKNOWN, classobj);

        for (int i = decorators.size() - 1; i >= 0; i--) {
            cls = decorators[i]->call(emitter, getOpInfoForNode(node, unw_info), ArgPassSpec(1), { cls }, NULL);
        }

        return cls;
    }

    CompilerVariable* _createFunction(BST_FunctionDef* node, const UnwindInfo& unw_info) {
        BoxedCode* code = node->code;
        assert(code);

        std::vector<ConcreteCompilerVariable*> defaults;
        for (int i = 0; i < node->num_defaults; ++i) {
            CompilerVariable* e = evalVReg(node->elts[node->num_decorator + i]);
            ConcreteCompilerVariable* converted = e->makeConverted(emitter, e->getBoxType());
            defaults.push_back(converted);
        }

        bool takes_closure = code->source->scoping.takesClosure();

        llvm::Value* this_closure = NULL;
        if (takes_closure) {
            if (irstate->getScopeInfo().createsClosure()) {
                this_closure = irstate->getCreatedClosure();
            } else {
                assert(irstate->getScopeInfo().passesThroughClosure());
                this_closure = irstate->getPassedClosure();
            }
            assert(this_closure);
        }

        CompilerVariable* func = makeFunction(emitter, code, this_closure, irstate->getGlobalsIfCustom(), defaults);

        return func;
    }

    CompilerVariable* evalMakeFunction(BST_MakeFunction* mkfn, const UnwindInfo& unw_info) {
        BST_FunctionDef* node = mkfn->function_def;
        std::vector<CompilerVariable*> decorators;
        for (int i = 0; i < node->num_decorator; ++i) {
            decorators.push_back(evalVReg(node->elts[i]));
        }

        CompilerVariable* func = _createFunction(node, unw_info);

        for (int i = decorators.size() - 1; i >= 0; i--) {
            func = decorators[i]->call(emitter, getOpInfoForNode(node, unw_info), ArgPassSpec(1), { func }, NULL);
        }

        return func;
    }

    // Note: the behavior of this function must match type_analysis.cpp:unboxedType()
    CompilerVariable* unboxVar(ConcreteCompilerType* t, llvm::Value* v) {
        if (t == BOXED_INT) {
            return makeUnboxedInt(emitter, v);
        }
        if (t == BOXED_FLOAT) {
            return makeUnboxedFloat(emitter, v);
        }
        if (t == BOXED_BOOL) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, v);
            return boolFromI1(emitter, unboxed);
        }
        return new ConcreteCompilerVariable(t, v);
    }

    template <typename AstType>
    CompilerVariable* evalSliceExprPost(AstType* node, const UnwindInfo& unw_info, CompilerVariable* rtn) {
        assert(rtn);

        // Out-guarding:
        BoxedClass* speculated_class = types->speculatedExprClass(node);
        if (speculated_class != NULL) {
            assert(rtn);

            ConcreteCompilerType* speculated_type = typeFromClass(speculated_class);
            if (VERBOSITY("irgen") >= 2) {
                printf("Speculating that %s is actually %s, at ", rtn->getType()->debugName().c_str(),
                       speculated_type->debugName().c_str());
                fflush(stdout);
                print_bst(node, irstate->getCode()->constant_vregs);
                llvm::outs().flush();
                printf("\n");
            }

#ifndef NDEBUG
            // That's not really a speculation.... could potentially handle this here, but
            // I think it's better to just not generate bad speculations:
            if (rtn->canConvertTo(speculated_type)) {
                auto source = irstate->getSourceInfo();
                printf("On %s:%d, function %s:\n", irstate->getCode()->filename->c_str(),
                       irstate->getCode()->firstlineno, irstate->getCode()->name->c_str());
                irstate->getSourceInfo()->cfg->print(irstate->getCode()->constant_vregs);
            }
            RELEASE_ASSERT(!rtn->canConvertTo(speculated_type), "%s %s", rtn->getType()->debugName().c_str(),
                           speculated_type->debugName().c_str());
#endif

            ConcreteCompilerVariable* old_rtn = rtn->makeConverted(emitter, UNKNOWN);

            llvm::Value* guard_check = old_rtn->makeClassCheck(emitter, speculated_class);
            assert(guard_check->getType() == g.i1);
            createExprTypeGuard(guard_check, old_rtn->getValue(), unw_info.current_stmt);

            rtn = unboxVar(speculated_type, old_rtn->getValue());
        }

        assert(rtn);
        assert(rtn->getType()->isUsable());

        return rtn;
    }

    void setDefinedVar(int vreg, llvm::Value* val) {
        assert(vreg >= 0);
        // printf("Setting definedness var for %s\n", name.c_str());
        assert(val->getType() == BOOL->llvmType());
        llvm::Value*& cur = definedness_vars[vreg];
        assert(cur == NULL);
        cur = val;
    }

    llvm::Value* getDefinedVar(int vreg, bool allow_missing = false) {
        assert(vreg >= 0);
        auto r = definedness_vars[vreg];
        if (!r)
            assert(allow_missing);
        return r;
    }

    llvm::Value* popDefinedVar(int vreg, bool allow_missing = false) {
        assert(vreg >= 0);
        llvm::Value* rtn = getDefinedVar(vreg, allow_missing);
        definedness_vars[vreg] = NULL;
        if (!allow_missing)
            assert(rtn != NULL);
        return rtn;
    }

    template <typename GetLLVMValCB>
    void _setVRegIfUserVisible(int vreg, GetLLVMValCB get_llvm_val_cb, CompilerVariable* prev,
                               bool potentially_undefined) {
        auto cfg = irstate->getSourceInfo()->cfg;
        assert(vreg >= 0);

        if (cfg->getVRegInfo().isUserVisibleVReg(vreg)) {
            // looks like this store don't have to be volatile because llvm knows that the vregs are visible thru the
            // FrameInfo which escapes.
            auto* gep = emitter.getBuilder()->CreateConstInBoundsGEP1_64(irstate->getVRegsVar(), vreg);
            if (prev) {
                auto* old_value = emitter.getBuilder()->CreateLoad(gep);
                emitter.setType(old_value, RefType::OWNED);
                if (potentially_undefined)
                    emitter.setNullable(old_value, true);
            }

            llvm::Value* new_val = get_llvm_val_cb();
            auto* store = emitter.getBuilder()->CreateStore(new_val, gep);
            emitter.refConsumed(new_val, store);
        }
    }

    // only updates symbol_table if we're *not* setting a global
    void _doSet(BST_Name* node, CompilerVariable* val, const UnwindInfo& unw_info) {
        assert(node->id.s() != "None");
        assert(node->id.s() != FRAME_INFO_PTR_NAME);
        assert(val->getType()->isUsable());

        auto vst = node->lookup_type;
        assert(vst != ScopeInfo::VarScopeType::UNKNOWN);
        assert(vst != ScopeInfo::VarScopeType::DEREF);

        auto name = node->id;
        auto vreg = node->vreg;

        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            if (irstate->getSourceInfo()->scoping.areGlobalsFromModule()) {
                auto parent_module = irstate->getGlobals();
                ConcreteCompilerVariable* module = new ConcreteCompilerVariable(MODULE, parent_module);
                module->setattr(emitter, getEmptyOpInfo(unw_info), name.getBox(), val);
            } else {
                auto converted = val->makeConverted(emitter, val->getBoxType());
                auto cs = emitter.createCall3(
                    unw_info, g.funcs.setGlobal, irstate->getGlobals(),
                    emitter.setType(embedRelocatablePtr(name.getBox(), g.llvm_boxedstring_type_ptr), RefType::BORROWED),
                    converted->getValue());
                emitter.refConsumed(converted->getValue(), cs);
            }
        } else if (vst == ScopeInfo::VarScopeType::NAME) {
            // TODO inefficient
            llvm::Value* boxedLocals = irstate->getBoxedLocalsVar();
            llvm::Value* attr = embedRelocatablePtr(name.getBox(), g.llvm_boxedstring_type_ptr);
            emitter.setType(attr, RefType::BORROWED);
            emitter.createCall3(unw_info, g.funcs.boxedLocalsSet, boxedLocals, attr,
                                val->makeConverted(emitter, UNKNOWN)->getValue());
        } else {
            // FAST or CLOSURE

            CompilerVariable* prev = symbol_table[vreg];
            symbol_table[vreg] = val;

            // Clear out the is_defined name since it is now definitely defined:
            assert(!isIsDefinedName(name.s()));
            bool maybe_was_undefined = popDefinedVar(vreg, true);

            if (vst == ScopeInfo::VarScopeType::CLOSURE) {
                size_t offset = node->closure_offset;

                // This is basically `closure->elts[offset] = val;`
                llvm::Value* gep = getClosureElementGep(emitter, irstate->getCreatedClosure(), offset);
                if (prev) {
                    auto load = emitter.getBuilder()->CreateLoad(gep);
                    emitter.setType(load, RefType::OWNED);
                    if (maybe_was_undefined)
                        emitter.setNullable(load, true);
                }
                llvm::Value* v = val->makeConverted(emitter, UNKNOWN)->getValue();
                auto store = emitter.getBuilder()->CreateStore(v, gep);
                emitter.refConsumed(v, store);
            }

            auto&& get_llvm_val = [&]() { return val->makeConverted(emitter, UNKNOWN)->getValue(); };
            _setVRegIfUserVisible(vreg, get_llvm_val, prev, maybe_was_undefined);
        }
    }

    void doStoreName(BST_StoreName* node, const UnwindInfo& unw_info) {
        CompilerVariable* val = evalVReg(node->vreg_value);

        assert(node->id.s() != "None");
        assert(node->id.s() != FRAME_INFO_PTR_NAME);
        assert(val->getType()->isUsable());

        auto vst = node->lookup_type;
        assert(vst != ScopeInfo::VarScopeType::UNKNOWN);
        assert(vst != ScopeInfo::VarScopeType::DEREF);

        auto name = node->id;
        auto vreg = node->vreg;

        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            if (irstate->getSourceInfo()->scoping.areGlobalsFromModule()) {
                auto parent_module = irstate->getGlobals();
                ConcreteCompilerVariable* module = new ConcreteCompilerVariable(MODULE, parent_module);
                module->setattr(emitter, getEmptyOpInfo(unw_info), name.getBox(), val);
            } else {
                auto converted = val->makeConverted(emitter, val->getBoxType());
                auto cs = emitter.createCall3(
                    unw_info, g.funcs.setGlobal, irstate->getGlobals(),
                    emitter.setType(embedRelocatablePtr(name.getBox(), g.llvm_boxedstring_type_ptr), RefType::BORROWED),
                    converted->getValue());
                emitter.refConsumed(converted->getValue(), cs);
            }
        } else if (vst == ScopeInfo::VarScopeType::NAME) {
            // TODO inefficient
            llvm::Value* boxedLocals = irstate->getBoxedLocalsVar();
            llvm::Value* attr = embedRelocatablePtr(name.getBox(), g.llvm_boxedstring_type_ptr);
            emitter.setType(attr, RefType::BORROWED);
            emitter.createCall3(unw_info, g.funcs.boxedLocalsSet, boxedLocals, attr,
                                val->makeConverted(emitter, UNKNOWN)->getValue());
        } else {
            // FAST or CLOSURE

            CompilerVariable* prev = symbol_table[vreg];
            symbol_table[vreg] = val;

            // Clear out the is_defined name since it is now definitely defined:
            assert(!isIsDefinedName(name.s()));
            bool maybe_was_undefined = popDefinedVar(vreg, true);

            if (vst == ScopeInfo::VarScopeType::CLOSURE) {
                size_t offset = node->closure_offset;

                // This is basically `closure->elts[offset] = val;`
                llvm::Value* gep = getClosureElementGep(emitter, irstate->getCreatedClosure(), offset);
                if (prev) {
                    auto load = emitter.getBuilder()->CreateLoad(gep);
                    emitter.setType(load, RefType::OWNED);
                    if (maybe_was_undefined)
                        emitter.setNullable(load, true);
                }
                llvm::Value* v = val->makeConverted(emitter, UNKNOWN)->getValue();
                auto store = emitter.getBuilder()->CreateStore(v, gep);
                emitter.refConsumed(v, store);
            }

            auto&& get_llvm_val = [&]() { return val->makeConverted(emitter, UNKNOWN)->getValue(); };
            _setVRegIfUserVisible(vreg, get_llvm_val, prev, maybe_was_undefined);
        }
    }

    void doStoreAttr(BST_StoreAttr* target, const UnwindInfo& unw_info) {
        CompilerVariable* val = evalVReg(target->vreg_value);
        CompilerVariable* t = evalVReg(target->vreg_target);
        t->setattr(emitter, getEmptyOpInfo(unw_info), target->attr.getBox(), val);
    }

    void _assignSlice(llvm::Value* target, llvm::Value* value, const UnboxedSlice& slice_val,
                      const UnwindInfo& unw_info) {
        llvm::Value* cstart, *cstop;
        cstart = slice_val.start ? slice_val.start->makeConverted(emitter, UNKNOWN)->getValue()
                                 : emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED);
        cstop = slice_val.stop ? slice_val.stop->makeConverted(emitter, UNKNOWN)->getValue()
                               : emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED);

        bool do_patchpoint = ENABLE_ICSETITEMS;
        if (do_patchpoint) {
            auto pp = createSetitemIC();

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(target);
            llvm_args.push_back(cstart);
            llvm_args.push_back(cstop);
            llvm_args.push_back(value);

            emitter.createIC(std::move(pp), (void*)pyston::assignSlice, llvm_args, unw_info);
        } else {
            emitter.createCall(unw_info, g.funcs.assignSlice, { target, cstart, cstop, value });
        }
    }

    void doStoreSub(BST_StoreSub* target, const UnwindInfo& unw_info) {
        CompilerVariable* val = evalVReg(target->vreg_value);
        CompilerVariable* tget = evalVReg(target->vreg_target);

        CompilerVariable* slice = evalVReg(target->vreg_slice);
        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_val = val->makeConverted(emitter, val->getBoxType());

        assert(!(slice->getType() == UNBOXED_SLICE && !extractSlice(slice).step));
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        // TODO add a CompilerVariable::setattr, which can (similar to getitem)
        // statically-resolve the function if possible, and only fall back to
        // patchpoints if it couldn't.
        bool do_patchpoint = ENABLE_ICSETITEMS;
        if (do_patchpoint) {
            auto pp = createSetitemIC();

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());
            llvm_args.push_back(converted_val->getValue());

            emitter.createIC(std::move(pp), (void*)pyston::setitem, llvm_args, unw_info);
        } else {
            emitter.createCall3(unw_info, g.funcs.setitem, converted_target->getValue(), converted_slice->getValue(),
                                converted_val->getValue());
        }
    }

    void doStoreSubSlice(BST_StoreSubSlice* target, const UnwindInfo& unw_info) {
        CompilerVariable* val = evalVReg(target->vreg_value);
        CompilerVariable* tget = evalVReg(target->vreg_target);

        CompilerVariable* lower = target->vreg_lower != VREG_UNDEFINED ? evalVReg(target->vreg_lower) : NULL;
        CompilerVariable* upper = target->vreg_upper != VREG_UNDEFINED ? evalVReg(target->vreg_upper) : NULL;
        CompilerVariable* slice = makeSlice(lower, upper, NULL);
        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_val = val->makeConverted(emitter, val->getBoxType());

        _assignSlice(converted_target->getValue(), converted_val->getValue(), extractSlice(slice), unw_info);
    }

    void doUnpackIntoArray(BST_UnpackIntoArray* target, const UnwindInfo& unw_info) {
        int ntargets = target->num_elts;

        CompilerVariable* val = evalVReg(target->vreg_src);
        std::vector<CompilerVariable*> unpacked = val->unpack(emitter, getOpInfoForNode(target, unw_info), ntargets);

        for (int i = 0; i < ntargets; i++) {
            CompilerVariable* thisval = unpacked[i];
            _doSet(target->vreg_dst[i], thisval, unw_info);
        }
    }

    void _doSet(int vreg, CompilerVariable* val, const UnwindInfo& unw_info) {
        if (vreg == VREG_UNDEFINED) {
            // nothing todo
            return;
        }

        CompilerVariable* prev = symbol_table[vreg];
        symbol_table[vreg] = val;

        // Clear out the is_defined name since it is now definitely defined:
        bool maybe_was_undefined = popDefinedVar(vreg, true);

        auto&& get_llvm_val = [&]() { return val->makeConverted(emitter, UNKNOWN)->getValue(); };
        _setVRegIfUserVisible(vreg, get_llvm_val, prev, maybe_was_undefined);
    }

    void doAssert(BST_Assert* node, const UnwindInfo& unw_info) {
        // cfg translates all asserts into only 'assert 0' on the failing path.
        std::vector<llvm::Value*> llvm_args;

        // We could patchpoint this or try to avoid the overhead, but this should only
        // happen when the assertion is actually thrown so I don't think it will be necessary.
        static BoxedString* AssertionError_str = getStaticString("AssertionError");
        llvm_args.push_back(emitter.setType(
            emitter.createCall2(unw_info, g.funcs.getGlobal, irstate->getGlobals(),
                                emitter.setType(embedRelocatablePtr(AssertionError_str, g.llvm_boxedstring_type_ptr),
                                                RefType::BORROWED)),
            RefType::OWNED));

        ConcreteCompilerVariable* converted_msg = NULL;
        if (node->vreg_msg != VREG_UNDEFINED) {
            CompilerVariable* msg = evalVReg(node->vreg_msg);
            converted_msg = msg->makeConverted(emitter, msg->getBoxType());
            llvm_args.push_back(converted_msg->getValue());
        } else {
            llvm_args.push_back(emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED));
        }
        llvm::CallSite call = emitter.createCall(unw_info, g.funcs.assertFail, llvm_args);
        call.setDoesNotReturn();

        emitter.getBuilder()->CreateUnreachable();
        endBlock(DEAD);
    }

    CompilerVariable* evalAssign(BST_CopyVReg* node, const UnwindInfo& unw_info) {
        return evalVReg(node->vreg_src, false /* don't kill */);
    }

    // invoke delitem in objmodel.cpp, which will invoke the listDelitem of list
    void doDeleteSub(BST_DeleteSub* target, const UnwindInfo& unw_info) {
        CompilerVariable* tget = evalVReg(target->vreg_value);
        CompilerVariable* slice = evalVReg(target->vreg_slice);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());

        assert(!(slice->getType() == UNBOXED_SLICE && !extractSlice(slice).step));
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        bool do_patchpoint = ENABLE_ICDELITEMS;
        if (do_patchpoint) {
            auto pp = createDelitemIC();

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());

            emitter.createIC(std::move(pp), (void*)pyston::delitem, llvm_args, unw_info);
        } else {
            emitter.createCall2(unw_info, g.funcs.delitem, converted_target->getValue(), converted_slice->getValue());
        }
    }
    void doDeleteSubSlice(BST_DeleteSubSlice* target, const UnwindInfo& unw_info) {
        CompilerVariable* tget = evalVReg(target->vreg_value);
        CompilerVariable* lower = target->vreg_lower != VREG_UNDEFINED ? evalVReg(target->vreg_lower) : NULL;
        CompilerVariable* upper = target->vreg_upper != VREG_UNDEFINED ? evalVReg(target->vreg_upper) : NULL;
        CompilerVariable* slice = makeSlice(lower, upper, NULL);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());

        _assignSlice(converted_target->getValue(),
                     emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED), extractSlice(slice),
                     unw_info);
    }

    void _doDelAttr(BST_DeleteAttr* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalVReg(node->vreg_value);
        value->delattr(emitter, getEmptyOpInfo(unw_info), node->attr.getBox());
    }

    void _doDelName(BST_DeleteName* target, const UnwindInfo& unw_info) {
        // Hack: we don't have a bytecode for temporary-kills:
        if (target->id.s()[0] == '#') {
            // The refcounter will automatically delete this object.
            return;
        }

        ScopeInfo::VarScopeType vst = target->lookup_type;
        assert(vst != ScopeInfo::VarScopeType::UNKNOWN);
        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            // Can't use delattr since the errors are different:
            emitter.createCall2(unw_info, g.funcs.delGlobal, irstate->getGlobals(),
                                emitter.setType(embedRelocatablePtr(target->id.getBox(), g.llvm_boxedstring_type_ptr),
                                                RefType::BORROWED));
            return;
        }

        if (vst == ScopeInfo::VarScopeType::NAME) {
            llvm::Value* boxedLocals = irstate->getBoxedLocalsVar();
            llvm::Value* attr = embedRelocatablePtr(target->id.getBox(), g.llvm_boxedstring_type_ptr);
            emitter.setType(attr, RefType::BORROWED);
            emitter.createCall2(unw_info, g.funcs.boxedLocalsDel, boxedLocals, attr);
            return;
        }

        // Can't be in a closure because of this syntax error:
        // SyntaxError: can not delete variable 'x' referenced in nested scope
        assert(vst == ScopeInfo::VarScopeType::FAST);

        CompilerVariable* prev = symbol_table[target->vreg];

        llvm::Value* is_defined_var = getDefinedVar(target->vreg, true);

        _setVRegIfUserVisible(target->vreg, []() { return getNullPtr(g.llvm_value_type_ptr); }, prev,
                              (bool)is_defined_var);

        if (!symbol_table[target->vreg]) {
            llvm::CallSite call = emitter.createCall(
                unw_info, g.funcs.assertNameDefined,
                { getConstantInt(0, g.i1), embedConstantPtr(target->id.c_str(), g.i8_ptr),
                  emitter.setType(embedRelocatablePtr(NameError, g.llvm_class_type_ptr), RefType::BORROWED),
                  getConstantInt(true /*local_error_msg*/, g.i1) });
            call.setDoesNotReturn();
            return;
        }

        if (is_defined_var) {
            emitter.createCall(
                unw_info, g.funcs.assertNameDefined,
                { i1FromLLVMBool(emitter, is_defined_var), embedConstantPtr(target->id.c_str(), g.i8_ptr),
                  emitter.setType(embedRelocatablePtr(NameError, g.llvm_class_type_ptr), RefType::BORROWED),
                  getConstantInt(true /*local_error_msg*/, g.i1) });
            popDefinedVar(target->vreg);
        }

        symbol_table[target->vreg] = NULL;
    }

    void doExec(BST_Exec* node, const UnwindInfo& unw_info) {
        CompilerVariable* body = evalVReg(node->vreg_body);
        llvm::Value* vbody = body->makeConverted(emitter, body->getBoxType())->getValue();

        llvm::Value* vglobals;
        if (node->vreg_globals != VREG_UNDEFINED) {
            CompilerVariable* globals = evalVReg(node->vreg_globals);
            vglobals = globals->makeConverted(emitter, globals->getBoxType())->getValue();
        } else {
            vglobals = getNullPtr(g.llvm_value_type_ptr);
        }

        llvm::Value* vlocals;
        if (node->vreg_locals != VREG_UNDEFINED) {
            CompilerVariable* locals = evalVReg(node->vreg_locals);
            vlocals = locals->makeConverted(emitter, locals->getBoxType())->getValue();
        } else {
            vlocals = getNullPtr(g.llvm_value_type_ptr);
        }

        static_assert(sizeof(FutureFlags) == 4, "");
        emitter.createCall(unw_info, g.funcs.exec,
                           { vbody, vglobals, vlocals, getConstantInt(irstate->getSourceInfo()->future_flags, g.i32) });
    }

    void doPrint(BST_Print* node, const UnwindInfo& unw_info) {
        ConcreteCompilerVariable* dest = NULL;
        if (node->vreg_dest != VREG_UNDEFINED) {
            auto d = evalVReg(node->vreg_dest);
            dest = d->makeConverted(emitter, d->getBoxType());
        } else {
            dest = new ConcreteCompilerVariable(UNKNOWN,
                                                emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED));
        }
        assert(dest);

        ConcreteCompilerVariable* converted;

        if (node->vreg_value != VREG_UNDEFINED) {
            CompilerVariable* var = evalVReg(node->vreg_value);
            converted = var->makeConverted(emitter, var->getBoxType());
        } else {
            converted = new ConcreteCompilerVariable(UNKNOWN, getNullPtr(g.llvm_value_type_ptr));
        }

        emitter.createCall3(unw_info, g.funcs.printHelper, dest->getValue(), converted->getValue(),
                            getConstantInt(node->nl, g.i1));
    }

    void doReturn(BST_Return* node, const UnwindInfo& unw_info) {
        assert(!unw_info.hasHandler());

        CompilerVariable* val;
        if (node->vreg_value == VREG_UNDEFINED) {
            val = emitter.getNone();
        } else {
            val = evalVReg(node->vreg_value);
        }
        assert(val);

        ConcreteCompilerType* opt_rtn_type = irstate->getReturnType();
        if (irstate->getReturnType()->llvmType() == val->getConcreteType()->llvmType())
            opt_rtn_type = val->getConcreteType();

        ConcreteCompilerVariable* rtn = val->makeConverted(emitter, opt_rtn_type);

        emitter.emitSetCurrentStmt(node);

        if (!irstate->getCurFunction()->entry_descriptor)
            emitter.getBuilder()->CreateCall(g.funcs.deinitFrame, irstate->getFrameInfoVar());

        assert(rtn->getValue());
        auto ret_inst = emitter.getBuilder()->CreateRet(rtn->getValue());

        irstate->getRefcounts()->refConsumed(rtn->getValue(), ret_inst);

        symbol_table.clear();

        endBlock(DEAD);
    }

    void doBranch(BST_Branch* node, const UnwindInfo& unw_info) {
        assert(!unw_info.hasHandler());

        assert(node->iftrue->idx > myblock->idx);
        assert(node->iffalse->idx > myblock->idx);

        CompilerVariable* val = evalVReg(node->vreg_test);
        assert(val);

        // We could call nonzero here if there is no try-catch block?
        ASSERT(val->getType() == BOOL, "should have called NONZERO before this; is %s",
               val->getType()->debugName().c_str());
        llvm::Value* v = i1FromBool(emitter, static_cast<ConcreteCompilerVariable*>(val));
        assert(v->getType() == g.i1);

        llvm::BasicBlock* iftrue = entry_blocks[node->iftrue];
        llvm::BasicBlock* iffalse = entry_blocks[node->iffalse];

        endBlock(FINISHED);

        emitter.getBuilder()->CreateCondBr(v, iftrue, iffalse);
    }

    void doOSRExit(llvm::BasicBlock* normal_target, BST_Jump* osr_key) {
        RELEASE_ASSERT(0, "I don't think this can get hit any more and it has not been updated");
#if 0
        llvm::BasicBlock* starting_block = curblock;
        llvm::BasicBlock* onramp = llvm::BasicBlock::Create(g.context, "onramp", irstate->getLLVMFunction());

        // Code to check if we want to do the OSR:
        llvm::GlobalVariable* edgecount_ptr = new llvm::GlobalVariable(
            *g.cur_module, g.i64, false, llvm::GlobalValue::InternalLinkage, getConstantInt(0, g.i64), "edgecount");
        llvm::Value* curcount = emitter.getBuilder()->CreateLoad(edgecount_ptr);
        llvm::Value* newcount = emitter.getBuilder()->CreateAdd(curcount, getConstantInt(1, g.i64));
        emitter.getBuilder()->CreateStore(newcount, edgecount_ptr);

        auto effort = irstate->getEffortLevel();
        int osr_threshold;
        if (effort == EffortLevel::MODERATE)
            osr_threshold = OSR_THRESHOLD_T2;
        else
            RELEASE_ASSERT(0, "Unknown effort: %d", (int)effort);
        llvm::Value* osr_test = emitter.getBuilder()->CreateICmpSGT(newcount, getConstantInt(osr_threshold));

        llvm::Metadata* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), llvm::ConstantAsMetadata::get(getConstantInt(1)),
                llvm::ConstantAsMetadata::get(getConstantInt(1000)) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Metadata*>(md_vals));
        emitter.getBuilder()->CreateCondBr(osr_test, onramp, normal_target, branch_weights);

        // Emitting the actual OSR:
        emitter.getBuilder()->SetInsertPoint(onramp);
        OSREntryDescriptor* entry = OSREntryDescriptor::create(irstate->getCode(), osr_key, irstate->getExceptionStyle());
        OSRExit* exit = new OSRExit(entry);
        llvm::Value* partial_func = emitter.getBuilder()->CreateCall(g.funcs.compilePartialFunc,
                                                                     embedRelocatablePtr(exit, g.i8->getPointerTo()));

        std::vector<llvm::Value*> llvm_args;
        std::vector<llvm::Type*> llvm_arg_types;
        std::vector<ConcreteCompilerVariable*> converted_args;

        SortedSymbolTable sorted_symbol_table(symbol_table.begin(), symbol_table.end());

        sorted_symbol_table[internString(FRAME_INFO_PTR_NAME)]
            = new ConcreteCompilerVariable(FRAME_INFO, irstate->getFrameInfoVar());

        // For OSR calls, we use the same calling convention as in some other places; namely,
        // arg1, arg2, arg3, argarray [nargs is ommitted]
        // It would be nice to directly pass all variables as arguments, instead of packing them into
        // an array, for a couple reasons (eliminate copies, and allow for a tail call).
        // But this doesn't work if the IR is being interpreted, because the interpreter can't
        // do arbitrary-arity function calls (yet?).  One possibility is to pass them as an
        // array for the interpreter and as all arguments for compilation, but I'd rather avoid
        // having two different calling conventions for the same thing.  Plus, this would
        // prevent us from having two OSR exits point to the same OSR entry; not something that
        // we're doing right now but something that would be nice in the future.

        llvm::Value* arg_array = NULL, * malloc_save = NULL;
        if (sorted_symbol_table.size() > 3) {
            // Leave in the ability to use malloc but I guess don't use it.
            // Maybe if there are a ton of live variables it'd be nice to have them be
            // heap-allocated, or if we don't immediately return the result of the OSR?
            bool use_malloc = false;
            if (use_malloc) {
                llvm::Value* n_bytes = getConstantInt((sorted_symbol_table.size() - 3) * sizeof(Box*), g.i64);
                llvm::Value* l_malloc = embedConstantPtr(
                    (void*)malloc, llvm::FunctionType::get(g.i8->getPointerTo(), g.i64, false)->getPointerTo());
                malloc_save = emitter.getBuilder()->CreateCall(l_malloc, n_bytes);
                arg_array = emitter.getBuilder()->CreateBitCast(malloc_save, g.llvm_value_type_ptr->getPointerTo());
            } else {
                llvm::Value* n_varargs = llvm::ConstantInt::get(g.i64, sorted_symbol_table.size() - 3, false);
                // TODO we have a number of allocas with non-overlapping lifetimes, that end up
                // being redundant.
                arg_array = new llvm::AllocaInst(g.llvm_value_type_ptr, n_varargs, "",
                                                 irstate->getLLVMFunction()->getEntryBlock().getFirstInsertionPt());
            }
        }

        int arg_num = -1;
        for (const auto& p : sorted_symbol_table) {
            arg_num++;

            // This line can never get hit right now since we unnecessarily force every variable to be concrete
            // for a loop, since we generate all potential phis:
            ASSERT(p.second->getType() == p.second->getConcreteType(), "trying to pass through %s\n",
                   p.second->getType()->debugName().c_str());

            ConcreteCompilerVariable* var = p.second->makeConverted(emitter, p.second->getConcreteType());
            converted_args.push_back(var);

            assert(var->getType() != BOXED_INT);
            assert(var->getType() != BOXED_FLOAT
                   && "should probably unbox it, but why is it boxed in the first place?");

            llvm::Value* val = var->getValue();

            if (arg_num < 3) {
                llvm_args.push_back(val);
                llvm_arg_types.push_back(val->getType());
            } else {
                llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, arg_num - 3);

                if (var->getType() == INT || var->getType() == BOOL) {
                    val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                } else if (var->getType() == FLOAT) {
                    // val = emitter.getBuilder()->CreateBitCast(val, g.llvm_value_type_ptr);
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                } else if (var->getType() == GENERATOR) {
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.llvm_generator_type_ptr->getPointerTo());
                } else if (var->getType() == UNDEF) {
                    // TODO if there are any undef variables, we're in 'unreachable' territory.
                    // Do we even need to generate any of this code?

                    // Currently we represent 'undef's as 'i16 undef'
                    val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                } else if (var->getType() == CLOSURE) {
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.llvm_closure_type_ptr->getPointerTo());
                } else if (var->getType() == FRAME_INFO) {
                    ptr = emitter.getBuilder()->CreateBitCast(ptr,
                                                              g.llvm_frame_info_type->getPointerTo()->getPointerTo());
                } else {
                    assert(val->getType() == g.llvm_value_type_ptr);
                }

                emitter.getBuilder()->CreateStore(val, ptr);
            }

            ConcreteCompilerType*& t = entry->args[p.first];
            if (t == NULL)
                t = var->getType();
            else
                ASSERT(t == var->getType(), "%s %s\n", t->debugName().c_str(), var->getType()->debugName().c_str());
        }

        if (sorted_symbol_table.size() > 3) {
            llvm_args.push_back(arg_array);
            llvm_arg_types.push_back(arg_array->getType());
        }

        llvm::FunctionType* ft
            = llvm::FunctionType::get(irstate->getReturnType()->llvmType(), llvm_arg_types, false /*vararg*/);
        partial_func = emitter.getBuilder()->CreateBitCast(partial_func, ft->getPointerTo());

        llvm::CallInst* rtn = emitter.getBuilder()->CreateCall(partial_func, llvm_args);

        // If we alloca'd the arg array, we can't make this into a tail call:
        if (arg_array == NULL && malloc_save != NULL) {
            rtn->setTailCall(true);
        }

        if (malloc_save != NULL) {
            llvm::Value* l_free = embedConstantPtr(
                (void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
            emitter.getBuilder()->CreateCall(l_free, malloc_save);
        }

        if (!irstate->getCurFunction()->entry_descriptor)
            emitter.getBuilder()->CreateCall(g.funcs.deinitFrame, irstate->getFrameInfoVar());
        emitter.getBuilder()->CreateRet(rtn);

        emitter.getBuilder()->SetInsertPoint(starting_block);
#endif
    }

    void doJump(BST_Jump* node, const UnwindInfo& unw_info) {
        endBlock(FINISHED);

        llvm::BasicBlock* target = entry_blocks[node->target];

        if (ENABLE_OSR && node->target->idx < myblock->idx && irstate->getEffortLevel() < EffortLevel::MAXIMAL) {
            assert(node->target->predecessors.size() > 1);
            doOSRExit(target, node);
        } else {
            emitter.getBuilder()->CreateBr(target);
        }
    }

    void doRaise(BST_Raise* node, const UnwindInfo& unw_info) {
        // It looks like ommitting the second and third arguments are equivalent to passing None,
        // but ommitting the first argument is *not* the same as passing None.

        ExceptionStyle target_exception_style;

        if (unw_info.hasHandler())
            target_exception_style = CAPI;
        else
            target_exception_style = irstate->getExceptionStyle();

        if (node->vreg_arg0 == VREG_UNDEFINED) {
            assert(node->vreg_arg1 == VREG_UNDEFINED);
            assert(node->vreg_arg2 == VREG_UNDEFINED);

            llvm::Value* exc_info = emitter.getBuilder()->CreateConstInBoundsGEP2_32(irstate->getFrameInfoVar(), 0, 0);
            if (target_exception_style == CAPI) {
                emitter.createCall(unw_info, g.funcs.raise0_capi, exc_info, CAPI, IREmitter::ALWAYS_THROWS);
                emitter.getBuilder()->CreateUnreachable();
            } else {
                emitter.createCall(unw_info, g.funcs.raise0, exc_info);
                emitter.getBuilder()->CreateUnreachable();
            }

            endBlock(DEAD);
            return;
        }

        std::vector<llvm::Value*> args;
        for (auto a : { node->vreg_arg0, node->vreg_arg1, node->vreg_arg2 }) {
            if (a != VREG_UNDEFINED) {
                CompilerVariable* v = evalVReg(a);
                ConcreteCompilerVariable* converted = v->makeConverted(emitter, v->getBoxType());
                args.push_back(converted->getValue());
            } else {
                args.push_back(emitter.getNone()->getValue());
            }
        }

        llvm::Instruction* inst;
        if (target_exception_style == CAPI) {
            inst = emitter.createCall(unw_info, g.funcs.raise3_capi, args, CAPI, IREmitter::ALWAYS_THROWS);
        } else {
            inst = emitter.createCall(unw_info, g.funcs.raise3, args, CXX);
        }

        for (auto a : args) {
            emitter.refConsumed(a, inst);
        }

        emitter.getBuilder()->CreateUnreachable();

        endBlock(DEAD);
    }

    void doStmt(BST_stmt* node, const UnwindInfo& unw_info) {
        // printf("%d stmt: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        switch (node->type) {
            case BST_TYPE::Assert:
                doAssert(bst_cast<BST_Assert>(node), unw_info);
                break;
            case BST_TYPE::DeleteAttr:
                _doDelAttr(bst_cast<BST_DeleteAttr>(node), unw_info);
                break;
            case BST_TYPE::DeleteSub:
                doDeleteSub(bst_cast<BST_DeleteSub>(node), unw_info);
                break;
            case BST_TYPE::DeleteSubSlice:
                doDeleteSubSlice(bst_cast<BST_DeleteSubSlice>(node), unw_info);
                break;
            case BST_TYPE::DeleteName:
                _doDelName(bst_cast<BST_DeleteName>(node), unw_info);
                break;
            case BST_TYPE::Exec:
                doExec(bst_cast<BST_Exec>(node), unw_info);
                break;
            case BST_TYPE::Print:
                doPrint(bst_cast<BST_Print>(node), unw_info);
                break;
            case BST_TYPE::Return:
                assert(!unw_info.hasHandler());
                doReturn(bst_cast<BST_Return>(node), unw_info);
                break;
            case BST_TYPE::StoreName:
                doStoreName(bst_cast<BST_StoreName>(node), unw_info);
                break;
            case BST_TYPE::StoreAttr:
                doStoreAttr(bst_cast<BST_StoreAttr>(node), unw_info);
                break;
            case BST_TYPE::StoreSub:
                doStoreSub(bst_cast<BST_StoreSub>(node), unw_info);
                break;
            case BST_TYPE::StoreSubSlice:
                doStoreSubSlice(bst_cast<BST_StoreSubSlice>(node), unw_info);
                break;
            case BST_TYPE::UnpackIntoArray:
                doUnpackIntoArray(bst_cast<BST_UnpackIntoArray>(node), unw_info);
                break;
            case BST_TYPE::Branch:
                assert(!unw_info.hasHandler());
                doBranch(bst_cast<BST_Branch>(node), unw_info);
                break;
            case BST_TYPE::Jump:
                assert(!unw_info.hasHandler());
                doJump(bst_cast<BST_Jump>(node), unw_info);
                break;
            case BST_TYPE::Invoke: {
                assert(!unw_info.hasHandler());
                BST_Invoke* invoke = bst_cast<BST_Invoke>(node);

                doStmt(invoke->stmt, UnwindInfo(node, entry_blocks[invoke->exc_dest]));

                assert(state == RUNNING || state == DEAD);
                if (state == RUNNING) {
                    emitter.getBuilder()->CreateBr(entry_blocks[invoke->normal_dest]);
                    endBlock(FINISHED);
                }

                break;
            }
            case BST_TYPE::Raise:
                doRaise(bst_cast<BST_Raise>(node), unw_info);
                break;
            case BST_TYPE::SetExcInfo:
                doSetExcInfo((BST_SetExcInfo*)node, unw_info);
                break;
            case BST_TYPE::UncacheExcInfo:
                doUncacheExcInfo((BST_UncacheExcInfo*)node, unw_info);
                break;
            case BST_TYPE::PrintExpr:
                doPrintExpr((BST_PrintExpr*)node, unw_info);
                break;

            // Handle all cases which are derived from BST_stmt_with_dest
            default: {
                CompilerVariable* rtn = NULL;
                switch (node->type) {
                    case BST_TYPE::CopyVReg:
                        rtn = evalAssign(bst_cast<BST_CopyVReg>(node), unw_info);
                        break;
                    case BST_TYPE::BinOp:
                        rtn = evalBinOp(bst_cast<BST_BinOp>(node), unw_info);
                        break;
                    case BST_TYPE::AugBinOp:
                        rtn = evalAugBinOp(bst_cast<BST_AugBinOp>(node), unw_info);
                        break;
                    case BST_TYPE::CallFunc:
                    case BST_TYPE::CallAttr:
                    case BST_TYPE::CallClsAttr:
                        rtn = evalCall((BST_Call*)node, unw_info);
                        break;
                    case BST_TYPE::Compare:
                        rtn = evalCompare(bst_cast<BST_Compare>(node), unw_info);
                        break;
                    case BST_TYPE::Dict:
                        rtn = evalDict(bst_cast<BST_Dict>(node), unw_info);
                        break;
                    case BST_TYPE::List:
                        rtn = evalList(bst_cast<BST_List>(node), unw_info);
                        break;
                    case BST_TYPE::Repr:
                        rtn = evalRepr(bst_cast<BST_Repr>(node), unw_info);
                        break;
                    case BST_TYPE::Set:
                        rtn = evalSet(bst_cast<BST_Set>(node), unw_info);
                        break;
                    case BST_TYPE::Tuple:
                        rtn = evalTuple(bst_cast<BST_Tuple>(node), unw_info);
                        break;
                    case BST_TYPE::UnaryOp:
                        rtn = evalUnaryOp(bst_cast<BST_UnaryOp>(node), unw_info);
                        break;
                    case BST_TYPE::Yield:
                        rtn = evalYield(bst_cast<BST_Yield>(node), unw_info);
                        break;
                    case BST_TYPE::Landingpad:
                        rtn = evalLandingpad((BST_Landingpad*)node, unw_info);
                        break;
                    case BST_TYPE::Locals:
                        rtn = evalLocals((BST_Locals*)node, unw_info);
                        break;
                    case BST_TYPE::LoadName:
                        rtn = evalLoadName((BST_LoadName*)node, unw_info);
                        break;
                    case BST_TYPE::LoadAttr:
                        rtn = evalLoadAttr((BST_LoadAttr*)node, unw_info);
                        break;
                    case BST_TYPE::LoadSub:
                        rtn = evalLoadSub((BST_LoadSub*)node, unw_info);
                        break;
                    case BST_TYPE::LoadSubSlice:
                        rtn = evalLoadSubSlice((BST_LoadSubSlice*)node, unw_info);
                        break;
                    case BST_TYPE::GetIter:
                        rtn = evalGetIter((BST_GetIter*)node, unw_info);
                        break;
                    case BST_TYPE::ImportFrom:
                        rtn = evalImportFrom((BST_ImportFrom*)node, unw_info);
                        break;
                    case BST_TYPE::ImportName:
                        rtn = evalImportName((BST_ImportName*)node, unw_info);
                        break;
                    case BST_TYPE::ImportStar:
                        rtn = evalImportStar((BST_ImportStar*)node, unw_info);
                        break;
                    case BST_TYPE::Nonzero:
                        rtn = evalNonzero((BST_Nonzero*)node, unw_info);
                        break;
                    case BST_TYPE::CheckExcMatch:
                        rtn = evalCheckExcMatch((BST_CheckExcMatch*)node, unw_info);
                        break;
                    case BST_TYPE::HasNext:
                        rtn = evalHasNext((BST_HasNext*)node, unw_info);
                        break;
                    case BST_TYPE::MakeClass:
                        rtn = evalMakeClass(bst_cast<BST_MakeClass>(node), unw_info);
                        break;
                    case BST_TYPE::MakeFunction:
                        rtn = evalMakeFunction(bst_cast<BST_MakeFunction>(node), unw_info);
                        break;
                    case BST_TYPE::MakeSlice:
                        rtn = evalMakeSlice(bst_cast<BST_MakeSlice>(node), unw_info);
                        break;
                    default:
                        printf("Unhandled stmt type at " __FILE__ ":" STRINGIFY(__LINE__) ": %d\n", node->type);
                        exit(1);
                }
                rtn = evalSliceExprPost((BST_stmt_with_dest*)node, unw_info, rtn);
                _doSet(((BST_stmt_with_dest*)node)->vreg_dst, rtn, unw_info);
            }
        }
    }

    void loadArgument(BST_Name* name, ConcreteCompilerType* t, llvm::Value* v, const UnwindInfo& unw_info) {
        CompilerVariable* var = unboxVar(t, v);
        _doSet(name, var, unw_info);
    }

    // bool allowableFakeEndingSymbol(InternedString name) { return isIsDefinedName(name.s()); }

    void endBlock(State new_state) {
        assert(state == RUNNING);

        // cf->func->dump();

        SourceInfo* source = irstate->getSourceInfo();
        auto cfg = source->cfg;
        auto&& scope_info = irstate->getScopeInfo();

        int num_vregs = symbol_table.numVregs();
        for (int vreg = 0; vreg < num_vregs; vreg++) {
            if (!symbol_table[vreg])
                continue;

            auto val = symbol_table[vreg];
            assert(val->getType()->isUsable());

            if (!irstate->getLiveness()->isLiveAtEnd(vreg, myblock)) {
                popDefinedVar(vreg, true);
                symbol_table[vreg] = NULL;
            } else if (irstate->getPhis()->isRequiredAfter(vreg, myblock)) {
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(vreg, myblock);
                assert(phi_type->isUsable());
                // printf("Converting %s from %s to %s\n", p.first.c_str(),
                // p.second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                // printf("have to convert %s from %s to %s\n", p.first.c_str(),
                // p.second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                ConcreteCompilerVariable* v = val->makeConverted(emitter, phi_type);
                symbol_table[vreg] = v;
            } else {
                if (myblock->successors.size()) {
                    // TODO getTypeAtBlockEnd will automatically convert up to the concrete type, which we don't
                    // want
                    // here, but this is just for debugging so I guess let it happen for now:
                    ConcreteCompilerType* ending_type = types->getTypeAtBlockEnd(vreg, myblock);
                    RELEASE_ASSERT(val->canConvertTo(ending_type), "%s is supposed to be %s, but somehow is %s",
                                   cfg->getVRegInfo().getName(vreg).c_str(), ending_type->debugName().c_str(),
                                   val->getType()->debugName().c_str());
                }
            }
        }

        for (int vreg : irstate->getPhis()->getAllRequiredAfter(myblock)) {
            auto name = irstate->getCFG()->getVRegInfo().getName(vreg);
            if (VERBOSITY() >= 3)
                printf("phi will be required for %s\n", name.c_str());
            CompilerVariable*& cur = symbol_table[vreg];

            if (cur != NULL) {
                // printf("defined on this path; ");

                llvm::Value* is_defined = popDefinedVar(vreg, true);

                if (irstate->getPhis()->isPotentiallyUndefinedAfter(vreg, myblock)) {
                    // printf("is potentially undefined later, so marking it defined\n");
                    if (is_defined) {
                        setDefinedVar(vreg, is_defined);
                    } else {
                        setDefinedVar(vreg, makeLLVMBool(1));
                    }
                } else {
                    // printf("is defined in all later paths, so not marking\n");
                    assert(!is_defined);
                }
            } else {
                // printf("no st entry, setting undefined\n");
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(vreg, myblock);
                assert(phi_type->isUsable());

                // Forward an incref'd None instead of a NULL.
                // TODO Change to using NULL to represent not-defined for boxed types, similar
                // to CPython?
                llvm::Value* v;
                if (phi_type == phi_type->getBoxType()) {
                    v = emitter.getNone()->getValue();
                } else {
                    v = llvm::UndefValue::get(phi_type->llvmType());
                }

                cur = new ConcreteCompilerVariable(phi_type, v);
                setDefinedVar(vreg, makeLLVMBool(0));
            }
        }

        state = new_state;
    }

public:
    void addFrameStackmapArgs(PatchpointInfo* pp, std::vector<llvm::Value*>& stackmap_args) override {
        int initial_args = stackmap_args.size();

        auto&& vregs = irstate->getSourceInfo()->cfg->getVRegInfo();

        // For deopts we need to add the compiler created names to the stackmap
        if (ENABLE_FRAME_INTROSPECTION && pp->isDeopt()) {
            auto source = irstate->getSourceInfo();
            if (source->is_generator)
                stackmap_args.push_back(irstate->getPassedGenerator());

            auto&& scoping = source->scoping;
            if (scoping.takesClosure())
                stackmap_args.push_back(irstate->getPassedClosure());

            if (scoping.createsClosure())
                stackmap_args.push_back(irstate->getCreatedClosure());

            typedef std::pair<InternedString, CompilerVariable*> Entry;
            for (auto&& p : symbol_table) {
                int vreg = p.first;

                // We never have to include non compiler generated vars because the user visible variables are stored
                // inside the vregs array.
                if (vregs.isUserVisibleVReg(vreg)) {
                    assert(!vregs.getName(p.first).isCompilerCreatedName());
                    continue;
                }

                CompilerVariable* v = p.second;
                v->serializeToFrame(stackmap_args);
                pp->addFrameVar(p.first, v->getType());
            }

            for (auto&& p : definedness_vars) {
                if (vregs.isUserVisibleVReg(p.first))
                    continue;

                assert(symbol_table[p.first]);

                stackmap_args.push_back(p.second);
                pp->addPotentiallyUndefined(p.first);
            }
        }

        int num_frame_args = stackmap_args.size() - initial_args;
        pp->setNumFrameArgs(num_frame_args);
    }

    EndingState getEndingSymbolTable() override {
        assert(state == FINISHED || state == DEAD);

        SourceInfo* source = irstate->getSourceInfo();

        SymbolTable* st = new SymbolTable(symbol_table);
        ConcreteSymbolTable* phi_st = new ConcreteSymbolTable(symbol_table.numVregs());
        DefinednessTable* def_vars = new DefinednessTable(definedness_vars);

        auto cfg = source->cfg;
        auto&& vreg_info = cfg->getVRegInfo();

        // This should have been consumed:
        assert(incoming_exc_state.empty());

        for (auto&& p : symbol_table) {
            ASSERT(p.second->getType()->isUsable(), "%d", p.first);
        }

        if (myblock->successors.size() == 0) {
            st->clear();
            symbol_table.clear();
            return EndingState(st, phi_st, def_vars, curblock, outgoing_exc_state);
        } else if (myblock->successors.size() > 1) {
            // Since there are no critical edges, all successors come directly from this node,
            // so there won't be any required phis.
            return EndingState(st, phi_st, def_vars, curblock, outgoing_exc_state);
        }

        assert(myblock->successors.size() == 1); // other cases should have been handled

        // In theory this case shouldn't be necessary:
        if (myblock->successors[0]->predecessors.size() == 1) {
            // If the next block has a single predecessor, don't have to
            // emit any phis.
            // Should probably not emit no-op jumps like this though.
            return EndingState(st, phi_st, def_vars, curblock, outgoing_exc_state);
        }

        // We have one successor, but they have more than one predecessor.
        // We're going to sort out which symbols need to go in phi_st and which belong inst.
        for (auto&& p : *st) {
            if (/*allowableFakeEndingSymbol(it->first)
                ||*/ irstate->getPhis()->isRequiredAfter(p.first, myblock)) {
                ConcreteCompilerType* ending_type = types->getTypeAtBlockEnd(p.first, myblock);
                assert(ending_type->isUsable());
                //(*phi_st)[p->first] = p->second->makeConverted(emp, p->second->getConcreteType());
                (*phi_st)[p.first] = p.second->makeConverted(emitter, ending_type);
                (*st)[p.first] = NULL;
            }
        }
        return EndingState(st, phi_st, def_vars, curblock, outgoing_exc_state);
    }

    void giveDefinednessVar(int vreg, llvm::Value* val) override {
        // printf("Giving definedness var %s\n", irstate->getSourceInfo()->cfg->getVRegInfo().getName(vreg).c_str());
        setDefinedVar(vreg, val);
    }

#ifndef NDEBUG
    void giveLocalSymbol(InternedString name, CompilerVariable* var) override {
        assert(name.s() != "None");
        assert(name.s() != FRAME_INFO_PTR_NAME);
        assert(name.s() != CREATED_CLOSURE_NAME);
        assert(name.s() != PASSED_CLOSURE_NAME);
        assert(name.s() != PASSED_GENERATOR_NAME);
        assert(name.s()[0] != '!');

        int vreg = irstate->getSourceInfo()->cfg->getVRegInfo().getVReg(name);
        giveLocalSymbol(vreg, var);
    }
#endif

    void giveLocalSymbol(int vreg, CompilerVariable* var) override {
        assert(var->getType()->isUsable());

        CompilerVariable*& cur = symbol_table[vreg];
        assert(cur == NULL);
        cur = var;
    }

    void copySymbolsFrom(SymbolTable* st) override {
        assert(st);
        DupCache cache;
        for (SymbolTable::iterator it = st->begin(); it != st->end(); ++it) {
            // printf("Copying in %s, a %s\n", it->first.c_str(), it->second->getType()->debugName().c_str());
            symbol_table[it.first()] = it.second()->dup(cache);
            assert(symbol_table[it.first()]->getType()->isUsable());
        }
    }

    void doFunctionEntry(const ParamNames& param_names, const std::vector<ConcreteCompilerType*>& arg_types) override {
        assert(param_names.totalParameters() == arg_types.size());

        auto&& scope_info = irstate->getScopeInfo();

        // TODO: move this to irgen.cpp?

        llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin();
        llvm::Value* globals = NULL;

        llvm::Value* passed_closure_or_null;

        if (scope_info.takesClosure()) {
            passed_closure_or_null = AI;
            irstate->setPassedClosure(passed_closure_or_null);
            ++AI;
            emitter.setType(passed_closure_or_null, RefType::BORROWED);
        } else {
            passed_closure_or_null = getNullPtr(g.llvm_closure_type_ptr);
            emitter.setType(passed_closure_or_null, RefType::BORROWED);
        }

        if (irstate->getSourceInfo()->is_generator) {
            auto passed_generator = AI;
            irstate->setPassedGenerator(passed_generator);
            emitter.setType(passed_generator, RefType::BORROWED);
            ++AI;
        }

        if (!irstate->getSourceInfo()->scoping.areGlobalsFromModule()) {
            globals = AI;
            emitter.setType(globals, RefType::BORROWED);
            ++AI;
        } else {
            BoxedModule* parent_module = irstate->getSourceInfo()->parent_module;
            globals = embedRelocatablePtr(parent_module, g.llvm_value_type_ptr, "cParentModule");
            emitter.setType(globals, RefType::BORROWED);
        }

        irstate->setupFrameInfoVar(passed_closure_or_null, globals);

        if (scope_info.createsClosure()) {
            auto created_closure = emitter.getBuilder()->CreateCall2(
                g.funcs.createClosure, passed_closure_or_null, getConstantInt(scope_info.getClosureSize(), g.i64));
            irstate->setCreatedClosure(created_closure);
            emitter.setType(created_closure, RefType::OWNED);
        }

        std::vector<llvm::Value*> python_parameters;
        for (int i = 0; i < arg_types.size(); i++) {
            assert(AI != irstate->getLLVMFunction()->arg_end());

            if (i == 3) {
                for (int i = 3; i < arg_types.size(); i++) {
                    llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(AI, i - 3);
                    llvm::Value* loaded = emitter.getBuilder()->CreateLoad(ptr);

                    if (arg_types[i]->llvmType() == g.i64)
                        loaded = emitter.getBuilder()->CreatePtrToInt(loaded, arg_types[i]->llvmType());
                    else {
                        assert(arg_types[i]->llvmType() == g.llvm_value_type_ptr);
                        emitter.setType(loaded, RefType::BORROWED);
                    }

                    python_parameters.push_back(loaded);
                }
                ++AI;
                break;
            }

            python_parameters.push_back(AI);
            emitter.setType(AI, RefType::BORROWED);
            ++AI;
        }

        assert(AI == irstate->getLLVMFunction()->arg_end());
        assert(python_parameters.size() == param_names.totalParameters());

        int i = 0;
        for (auto&& arg : param_names.argsAsName()) {
            loadArgument(arg, arg_types[i], python_parameters[i], UnwindInfo::cantUnwind());
            ++i;
        }

        if (param_names.has_vararg_name) {
            loadArgument(param_names.varArgAsName(), arg_types[i], python_parameters[i], UnwindInfo::cantUnwind());
            i++;
        }

        if (param_names.has_kwarg_name) {
            llvm::Value* passed_dict = python_parameters[i];
            emitter.setNullable(passed_dict, true);

            llvm::BasicBlock* starting_block = emitter.currentBasicBlock();
            llvm::BasicBlock* isnull_bb = emitter.createBasicBlock("isnull");
            llvm::BasicBlock* continue_bb = emitter.createBasicBlock("kwargs_join");

            llvm::Value* kwargs_null
                = emitter.getBuilder()->CreateICmpEQ(passed_dict, getNullPtr(g.llvm_value_type_ptr));
            llvm::BranchInst* null_check = emitter.getBuilder()->CreateCondBr(kwargs_null, isnull_bb, continue_bb);

            emitter.setCurrentBasicBlock(isnull_bb);
            llvm::Value* created_dict = emitter.getBuilder()->CreateCall(g.funcs.createDict);
            emitter.setType(created_dict, RefType::OWNED);
            auto isnull_terminator = emitter.getBuilder()->CreateBr(continue_bb);

            emitter.setCurrentBasicBlock(continue_bb);
            llvm::PHINode* phi = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, 2);
            phi->addIncoming(passed_dict, starting_block);
            phi->addIncoming(created_dict, isnull_bb);

            emitter.setType(phi, RefType::OWNED);
            emitter.refConsumed(passed_dict, null_check);
            emitter.refConsumed(created_dict, isnull_terminator);

            loadArgument(param_names.kwArgAsName(), arg_types[i], phi, UnwindInfo::cantUnwind());
            i++;
        }

        assert(i == arg_types.size());
    }

    void run(const CFGBlock* block) override {
        auto&& vregs = block->cfg->getVRegInfo();
        if (VERBOSITY("irgenerator") >= 2) { // print starting symbol table
            printf("  %d init:", block->idx);
            for (auto it = symbol_table.begin(); it != symbol_table.end(); ++it) {
                if (vregs.vregHasName(it.first()))
                    printf(" %s", vregs.getName(it.first()).c_str());
                else
                    printf(" <v%d>", it.first());
            }
            printf("\n");
        }
        for (int i = 0; i < block->body.size(); i++) {
            if (state == DEAD)
                break;
            assert(state != FINISHED);

#if ENABLE_SAMPLING_PROFILER
            auto stmt = block->body[i];
            if (stmt->type != BST_TYPE::Landigpad && stmt->lineno > 0)
                doSafePoint(block->body[i]);
#endif

            doStmt(block->body[i], UnwindInfo(block->body[i], NULL));
        }
        if (VERBOSITY("irgenerator") >= 2) { // print ending symbol table
            printf("  %d fini:", block->idx);
            for (auto it = symbol_table.begin(); it != symbol_table.end(); ++it)
                if (vregs.vregHasName(it.first()))
                    printf(" %s", vregs.getName(it.first()).c_str());
                else
                    printf(" <v%d>", it.first());
            printf("\n");
        }
    }

    void doSafePoint(BST_stmt* next_statement) override {
        // We need to setup frame introspection by updating the current stmt because we can run can run arbitrary code
        // like finalizers inside allowGLReadPreemption.
        emitter.emitSetCurrentStmt(next_statement);
        emitter.getBuilder()->CreateCall(g.funcs.allowGLReadPreemption);
    }

    // Create a (or reuse an existing) block that will catch a CAPI exception, and then forward
    // it to the "final_dest" block.  ie final_dest is a block corresponding to the IR level
    // LANDINGPAD, and this function will create a helper block that fetches the exception.
    // As a special-case, a NULL value for final_dest means that this helper block should
    // instead propagate the exception out of the function.
    llvm::BasicBlock* getCAPIExcDest(llvm::BasicBlock* from_block, llvm::BasicBlock* final_dest, BST_stmt* current_stmt,
                                     bool is_after_deopt) override {
        llvm::BasicBlock*& capi_exc_dest = capi_exc_dests[final_dest];
        llvm::PHINode*& phi_node = capi_phis[final_dest];

        if (!capi_exc_dest) {
            auto orig_block = curblock;

            capi_exc_dest = llvm::BasicBlock::Create(g.context, "capi_exc_dest", irstate->getLLVMFunction());

            emitter.setCurrentBasicBlock(capi_exc_dest);
            assert(!phi_node);
            phi_node = emitter.getBuilder()->CreatePHI(g.llvm_bststmt_type_ptr, 0);

            emitter.emitSetCurrentStmt(current_stmt);
            emitter.getBuilder()->CreateCall(g.funcs.caughtCapiException);

            if (!final_dest) {
                // Propagate the exception out of the function:
                if (irstate->getExceptionStyle() == CXX) {
                    emitter.getBuilder()->CreateCall(g.funcs.reraiseCapiExcAsCxx);
                    emitter.getBuilder()->CreateUnreachable();
                } else {
                    if (!irstate->getCurFunction()->entry_descriptor && !is_after_deopt)
                        emitter.getBuilder()->CreateCall(g.funcs.deinitFrame, irstate->getFrameInfoVar());
                    emitter.getBuilder()->CreateRet(getNullPtr(g.llvm_value_type_ptr));
                }
            } else {
                // Catch the exception and forward to final_dest:
                llvm::Value* exc_type_ptr
                    = new llvm::AllocaInst(g.llvm_value_type_ptr, getConstantInt(1, g.i64), "exc_type",
                                           irstate->getLLVMFunction()->getEntryBlock().getFirstInsertionPt());
                llvm::Value* exc_value_ptr
                    = new llvm::AllocaInst(g.llvm_value_type_ptr, getConstantInt(1, g.i64), "exc_value",
                                           irstate->getLLVMFunction()->getEntryBlock().getFirstInsertionPt());
                llvm::Value* exc_traceback_ptr
                    = new llvm::AllocaInst(g.llvm_value_type_ptr, getConstantInt(1, g.i64), "exc_traceback",
                                           irstate->getLLVMFunction()->getEntryBlock().getFirstInsertionPt());
                emitter.getBuilder()->CreateCall3(g.funcs.PyErr_Fetch, exc_type_ptr, exc_value_ptr, exc_traceback_ptr);
                // TODO: I think we should be doing this on a python raise() or when we enter a python catch:
                emitter.getBuilder()->CreateCall3(g.funcs.PyErr_NormalizeException, exc_type_ptr, exc_value_ptr,
                                                  exc_traceback_ptr);
                llvm::Value* exc_type = emitter.getBuilder()->CreateLoad(exc_type_ptr);
                llvm::Value* exc_value = emitter.getBuilder()->CreateLoad(exc_value_ptr);
                llvm::Value* exc_traceback = emitter.getBuilder()->CreateLoad(exc_traceback_ptr);

                emitter.setType(exc_type, RefType::OWNED);
                emitter.setType(exc_value, RefType::OWNED);
                emitter.setType(exc_traceback, RefType::OWNED);

                addOutgoingExceptionState(
                    IRGenerator::ExceptionState(capi_exc_dest, new ConcreteCompilerVariable(UNKNOWN, exc_type),
                                                new ConcreteCompilerVariable(UNKNOWN, exc_value),
                                                new ConcreteCompilerVariable(UNKNOWN, exc_traceback)));

                emitter.getBuilder()->CreateBr(final_dest);
            }

            emitter.setCurrentBasicBlock(from_block);
        }

        assert(capi_exc_dest);
        assert(phi_node);

        // Break a likely critical edge, for the benefit of the refcounter.
        // We should probably just teach the refcounter to break the edges on-demand though.
        llvm::BasicBlock* critedge_breaker = llvm::BasicBlock::Create(g.context, "", irstate->getLLVMFunction());
        critedge_breaker->moveBefore(capi_exc_dest);
        llvm::BranchInst::Create(capi_exc_dest, critedge_breaker);

        phi_node->addIncoming(embedRelocatablePtr(current_stmt, g.llvm_bststmt_type_ptr), critedge_breaker);

        return critedge_breaker;
    }

    llvm::BasicBlock* getCXXExcDest(const UnwindInfo& unw_info) override {
        llvm::BasicBlock* final_dest;
        if (unw_info.hasHandler()) {
            final_dest = unw_info.exc_dest;
        } else {
            final_dest = NULL;
        }

        llvm::BasicBlock* orig_block = curblock;

        llvm::BasicBlock* cxx_exc_dest = llvm::BasicBlock::Create(g.context, "cxxwrapper", irstate->getLLVMFunction());

        emitter.getBuilder()->SetInsertPoint(cxx_exc_dest);

        llvm::Value* exc_type, *exc_value, *exc_traceback;
        std::tie(exc_type, exc_value, exc_traceback) = createLandingpad(cxx_exc_dest);
        emitter.setType(exc_type, RefType::OWNED);
        emitter.setType(exc_value, RefType::OWNED);
        emitter.setType(exc_traceback, RefType::OWNED);

        // final_dest==NULL => propagate the exception out of the function.
        if (final_dest) {
            // Catch the exception and forward to final_dest:
            addOutgoingExceptionState(ExceptionState(cxx_exc_dest, new ConcreteCompilerVariable(UNKNOWN, exc_type),
                                                     new ConcreteCompilerVariable(UNKNOWN, exc_value),
                                                     new ConcreteCompilerVariable(UNKNOWN, exc_traceback)));

            emitter.getBuilder()->CreateBr(final_dest);
        } else if (irstate->getExceptionStyle() == CAPI) {
            auto call_inst
                = emitter.getBuilder()->CreateCall3(g.funcs.PyErr_Restore, exc_type, exc_value, exc_traceback);
            irstate->getRefcounts()->refConsumed(exc_type, call_inst);
            irstate->getRefcounts()->refConsumed(exc_value, call_inst);
            irstate->getRefcounts()->refConsumed(exc_traceback, call_inst);
            if (!irstate->getCurFunction()->entry_descriptor && !unw_info.is_after_deopt) {
                emitter.getBuilder()->CreateCall(g.funcs.deinitFrame, irstate->getFrameInfoVar());
            }
            emitter.getBuilder()->CreateRet(getNullPtr(g.llvm_value_type_ptr));
        } else {
            // auto call_inst = emitter.createCall3(UnwindInfo(unw_info.current_stmt, NO_CXX_INTERCEPTION),
            // g.funcs.rawReraise, exc_type, exc_value, exc_traceback);
            auto call_inst = emitter.getBuilder()->CreateCall3(g.funcs.rawReraise, exc_type, exc_value, exc_traceback);
            irstate->getRefcounts()->refConsumed(exc_type, call_inst);
            irstate->getRefcounts()->refConsumed(exc_value, call_inst);
            irstate->getRefcounts()->refConsumed(exc_traceback, call_inst);

            emitter.getBuilder()->CreateUnreachable();
        }

        emitter.setCurrentBasicBlock(orig_block);

        return cxx_exc_dest;
    }

    void addOutgoingExceptionState(ExceptionState exception_state) override {
        this->outgoing_exc_state.push_back(exception_state);
    }

    void setIncomingExceptionState(llvm::SmallVector<ExceptionState, 2> exc_state) override {
        assert(this->incoming_exc_state.empty());
        this->incoming_exc_state = std::move(exc_state);
    }
};

std::tuple<llvm::Value*, llvm::Value*, llvm::Value*> createLandingpad(llvm::BasicBlock* bb) {
    assert(bb->begin() == bb->end());

    llvm::IRBuilder<true> builder(bb);

    static llvm::Function* _personality_func = g.stdlib_module->getFunction("__gxx_personality_v0");
    assert(_personality_func);
    llvm::Value* personality_func
        = g.cur_module->getOrInsertFunction(_personality_func->getName(), _personality_func->getFunctionType());
    assert(personality_func);
    llvm::LandingPadInst* landing_pad = builder.CreateLandingPad(
        llvm::StructType::create(std::vector<llvm::Type*>{ g.i8_ptr, g.i64 }), personality_func, 1);
    landing_pad->addClause(getNullPtr(g.i8_ptr));

    llvm::Value* cxaexc_pointer = builder.CreateExtractValue(landing_pad, { 0 });

    static llvm::Function* std_module_catch = g.stdlib_module->getFunction("__cxa_begin_catch");
    auto begin_catch_func
        = g.cur_module->getOrInsertFunction(std_module_catch->getName(), std_module_catch->getFunctionType());
    assert(begin_catch_func);

    llvm::Value* excinfo_pointer = builder.CreateCall(begin_catch_func, cxaexc_pointer);
    llvm::Value* excinfo_pointer_casted = builder.CreateBitCast(excinfo_pointer, g.llvm_excinfo_type->getPointerTo());

    llvm::Value* exc_type = builder.CreateLoad(builder.CreateConstInBoundsGEP2_32(excinfo_pointer_casted, 0, 0));
    llvm::Value* exc_value = builder.CreateLoad(builder.CreateConstInBoundsGEP2_32(excinfo_pointer_casted, 0, 1));
    llvm::Value* exc_traceback = builder.CreateLoad(builder.CreateConstInBoundsGEP2_32(excinfo_pointer_casted, 0, 2));

    return std::make_tuple(exc_type, exc_value, exc_traceback);
}

IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types) {
    return new IRGeneratorImpl(irstate, entry_blocks, myblock, types);
}
}
