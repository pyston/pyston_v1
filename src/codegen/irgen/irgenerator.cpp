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
#include "core/ast.h"
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

IRGenState::IRGenState(FunctionMetadata* md, CompiledFunction* cf, SourceInfo* source_info,
                       std::unique_ptr<PhiAnalysis> phis, ParamNames* param_names, GCBuilder* gc,
                       llvm::MDNode* func_dbg_info, RefcountTracker* refcount_tracker)
    : md(md),
      cf(cf),
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
    assert(cf->func);
    assert(!cf->md); // in this case don't need to pass in sourceinfo
}

IRGenState::~IRGenState() {
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

template <typename Builder> static llvm::Value* getMDGep(Builder& builder, llvm::Value* v) {
    static_assert(offsetof(FrameInfo, md) == 72 + 16, "");
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

        if (getScopeInfo()->usesNameLookup()) {
            // load frame_info.boxedLocals
            this->boxed_locals = builder.CreateLoad(getBoxedLocalsGep(builder, this->frame_info));
            getRefcounts()->setType(this->boxed_locals, RefType::BORROWED);
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

        if (getScopeInfo()->usesNameLookup()) {
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
        builder.CreateStore(embedRelocatablePtr(getMD(), g.llvm_functionmetadata_type_ptr), getMDGep(builder, al));

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
    assert(getScopeInfo()->usesNameLookup());
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

ScopeInfo* IRGenState::getScopeInfo() {
    return getSourceInfo()->getScopeInfo();
}

ScopeInfo* IRGenState::getScopeInfoForNode(AST* node) {
    auto source = getSourceInfo();
    return source->scoping->getScopeInfoForNode(node);
}

llvm::Value* IRGenState::getGlobals() {
    assert(globals);
    return globals;
}

llvm::Value* IRGenState::getGlobalsIfCustom() {
    if (source_info->scoping->areGlobalsFromModule())
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

    llvm::CallSite emitPatchpoint(llvm::Type* return_type, const ICSetupInfo* pp, llvm::Value* func,
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

        PatchpointInfo* info = PatchpointInfo::create(currentFunction(), pp, ic_stackmap_args.size(), func_addr);

        int64_t pp_id = info->getId();
        int pp_size = pp ? pp->totalSize() : CALL_ONLY_SIZE;

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
    void emitSetCurrentStmt(AST_stmt* stmt) {
        if (stmt)
            getBuilder()->CreateStore(stmt ? embedRelocatablePtr(stmt, g.llvm_aststmt_type_ptr)
                                           : getNullPtr(g.llvm_aststmt_type_ptr),
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

    llvm::Instruction* createIC(const ICSetupInfo* pp, void* func_addr, const std::vector<llvm::Value*>& args,
                                const UnwindInfo& unw_info, ExceptionStyle target_exception_style = CXX,
                                llvm::Value* capi_exc_value = NULL) override {
        std::vector<llvm::Value*> stackmap_args;

        llvm::CallSite rtn = emitPatchpoint(pp->hasReturnValue() ? g.i64 : g.void_, pp,
                                            embedConstantPtr(func_addr, g.i8->getPointerTo()), args, stackmap_args,
                                            unw_info, target_exception_style, capi_exc_value);

        rtn.setCallingConv(pp->getCallingConvention());
        return rtn.getInstruction();
    }

    llvm::Value* createDeopt(AST_stmt* current_stmt, AST_expr* node, llvm::Value* node_value) override {
        ICSetupInfo* pp = createDeoptIC();
        llvm::Instruction* v
            = createIC(pp, (void*)pyston::deopt, { embedRelocatablePtr(node, g.llvm_astexpr_type_ptr), node_value },
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
        llvm::Constant* none = embedRelocatablePtr(None, g.llvm_value_type_ptr, "cNone");
        setType(none, RefType::BORROWED);
        return new ConcreteCompilerVariable(typeFromClass(none_cls), none);
    }
};

IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock, IRGenerator* irgenerator) {
    return new IREmitterImpl(irstate, curblock, irgenerator);
}

static std::unordered_map<AST_expr*, std::vector<BoxedString*>*> made_keyword_storage;
std::vector<BoxedString*>* getKeywordNameStorage(AST_Call* node) {
    auto it = made_keyword_storage.find(node);
    if (it != made_keyword_storage.end())
        return it->second;

    auto rtn = new std::vector<BoxedString*>();
    made_keyword_storage.insert(it, std::make_pair(node, rtn));

    // Only add the keywords to the array the first time, since
    // the later times we will hit the cache which will have the
    // keyword names already populated:
    if (!rtn->size()) {
        for (auto kw : node->keywords)
            rtn->push_back(kw->arg.getBox());
    }

    return rtn;
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
          entry_blocks(entry_blocks),
          myblock(myblock),
          types(types),
          state(RUNNING) {}

    virtual CFGBlock* getCFGBlock() override { return myblock; }

private:
    OpInfo getOpInfoForNode(AST* ast, const UnwindInfo& unw_info) {
        assert(ast);

        EffortLevel effort = irstate->getEffortLevel();
        // This is the only place we create type recorders for the llvm tier;
        // if we are ok with never doing that there's a bunch of code that could
        // be removed.
        bool record_types = false;

        TypeRecorder* type_recorder;
        if (record_types) {
            type_recorder = getTypeRecorderForNode(ast);
        } else {
            type_recorder = NULL;
        }

        return OpInfo(irstate->getEffortLevel(), type_recorder, unw_info, ICInfo::getICInfoForNode(ast));
    }

    OpInfo getEmptyOpInfo(const UnwindInfo& unw_info) {
        return OpInfo(irstate->getEffortLevel(), NULL, unw_info, NULL);
    }

    void createExprTypeGuard(llvm::Value* check_val, AST* node, llvm::Value* node_value, AST_stmt* current_statement) {
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
        llvm::Value* v = emitter.createDeopt(current_statement, (AST_expr*)node, node_value);
        llvm::Instruction* ret_inst = emitter.getBuilder()->CreateRet(v);
        irstate->getRefcounts()->refConsumed(v, ret_inst);

        curblock = success_bb;
        emitter.getBuilder()->SetInsertPoint(curblock);
    }

    template <typename T> InternedString internString(T&& s) {
        return irstate->getSourceInfo()->getInternedStrings().get(std::forward<T>(s));
    }

    InternedString getIsDefinedName(InternedString name) {
        return pyston::getIsDefinedName(name, irstate->getSourceInfo()->getInternedStrings());
    }

    CompilerVariable* evalAttribute(AST_Attribute* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);

        CompilerVariable* rtn = value->getattr(emitter, getOpInfoForNode(node, unw_info), node->attr.getBox(), false);
        return rtn;
    }

    CompilerVariable* evalClsAttribute(AST_ClsAttribute* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);
        CompilerVariable* rtn = value->getattr(emitter, getOpInfoForNode(node, unw_info), node->attr.getBox(), true);
        return rtn;
    }

    CompilerVariable* evalLangPrimitive(AST_LangPrimitive* node, const UnwindInfo& unw_info) {
        switch (node->opcode) {
            case AST_LangPrimitive::CHECK_EXC_MATCH: {
                assert(node->args.size() == 2);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);
                CompilerVariable* cls = evalExpr(node->args[1], unw_info);

                ConcreteCompilerVariable* converted_obj = obj->makeConverted(emitter, obj->getBoxType());
                ConcreteCompilerVariable* converted_cls = cls->makeConverted(emitter, cls->getBoxType());

                llvm::Value* v = emitter.createCall(unw_info, g.funcs.exceptionMatches,
                                                    { converted_obj->getValue(), converted_cls->getValue() });
                assert(v->getType() == g.i1);

                return boolFromI1(emitter, v);
            }
            case AST_LangPrimitive::LANDINGPAD: {
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
            case AST_LangPrimitive::LOCALS: {
                return new ConcreteCompilerVariable(UNKNOWN, irstate->getBoxedLocalsVar());
            }
            case AST_LangPrimitive::GET_ITER: {
                assert(node->args.size() == 1);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);
                auto rtn = obj->getPystonIter(emitter, getOpInfoForNode(node, unw_info));
                return rtn;
            }
            case AST_LangPrimitive::IMPORT_FROM: {
                assert(node->args.size() == 2);
                assert(node->args[0]->type == AST_TYPE::Name);
                assert(node->args[1]->type == AST_TYPE::Str);

                CompilerVariable* module = evalExpr(node->args[0], unw_info);
                ConcreteCompilerVariable* converted_module = module->makeConverted(emitter, module->getBoxType());

                auto ast_str = ast_cast<AST_Str>(node->args[1]);
                assert(ast_str->str_type == AST_Str::STR);
                const std::string& name = ast_str->str_data;
                assert(name.size());

                llvm::Value* name_arg
                    = embedRelocatablePtr(irstate->getSourceInfo()->parent_module->getStringConstant(name, true),
                                          g.llvm_boxedstring_type_ptr);
                emitter.setType(name_arg, RefType::BORROWED);
                llvm::Value* r
                    = emitter.createCall2(unw_info, g.funcs.importFrom, converted_module->getValue(), name_arg);
                emitter.setType(r, RefType::OWNED);

                CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r);
                return v;
            }
            case AST_LangPrimitive::IMPORT_STAR: {
                assert(node->args.size() == 1);
                assert(node->args[0]->type == AST_TYPE::Name);

                RELEASE_ASSERT(irstate->getSourceInfo()->ast->type == AST_TYPE::Module,
                               "import * not supported in functions (yet)");

                CompilerVariable* module = evalExpr(node->args[0], unw_info);
                ConcreteCompilerVariable* converted_module = module->makeConverted(emitter, module->getBoxType());

                llvm::Value* r = emitter.createCall2(unw_info, g.funcs.importStar, converted_module->getValue(),
                                                     irstate->getGlobals());
                emitter.setType(r, RefType::OWNED);
                CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r);
                return v;
            }
            case AST_LangPrimitive::IMPORT_NAME: {
                assert(node->args.size() == 3);
                assert(node->args[0]->type == AST_TYPE::Num);
                assert(static_cast<AST_Num*>(node->args[0])->num_type == AST_Num::INT);
                assert(node->args[2]->type == AST_TYPE::Str);

                int level = static_cast<AST_Num*>(node->args[0])->n_int;

                // TODO this could be a constant Box* too
                CompilerVariable* froms = evalExpr(node->args[1], unw_info);
                ConcreteCompilerVariable* converted_froms = froms->makeConverted(emitter, froms->getBoxType());

                auto ast_str = ast_cast<AST_Str>(node->args[2]);
                assert(ast_str->str_type == AST_Str::STR);
                const std::string& module_name = ast_str->str_data;

                llvm::Value* imported = emitter.createCall(
                    unw_info, g.funcs.import,
                    { getConstantInt(level, g.i32), converted_froms->getValue(),
                      emitter.setType(embedRelocatablePtr(module_name.c_str(), g.i8_ptr), RefType::BORROWED),
                      getConstantInt(module_name.size(), g.i64) });
                emitter.setType(imported, RefType::OWNED);
                ConcreteCompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, imported);
                return v;
            }
            case AST_LangPrimitive::NONE: {
                return emitter.getNone();
            }
            case AST_LangPrimitive::NONZERO: {
                assert(node->args.size() == 1);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);

                CompilerVariable* rtn = obj->nonzero(emitter, getOpInfoForNode(node, unw_info));
                return rtn;
            }
            case AST_LangPrimitive::HASNEXT: {
                assert(node->args.size() == 1);
                CompilerVariable* obj = evalExpr(node->args[0], unw_info);

                CompilerVariable* rtn = obj->hasnext(emitter, getOpInfoForNode(node, unw_info));
                return rtn;
            }
            case AST_LangPrimitive::SET_EXC_INFO: {
                assert(node->args.size() == 3);
                CompilerVariable* type = evalExpr(node->args[0], unw_info);
                CompilerVariable* value = evalExpr(node->args[1], unw_info);
                CompilerVariable* traceback = evalExpr(node->args[2], unw_info);

                auto* builder = emitter.getBuilder();

                llvm::Value* frame_info = irstate->getFrameInfoVar();

                ConcreteCompilerVariable* converted_type = type->makeConverted(emitter, UNKNOWN);
                ConcreteCompilerVariable* converted_value = value->makeConverted(emitter, UNKNOWN);
                ConcreteCompilerVariable* converted_traceback = traceback->makeConverted(emitter, UNKNOWN);

                auto inst = emitter.createCall(UnwindInfo::cantUnwind(), g.funcs.setFrameExcInfo,
                                               { frame_info, converted_type->getValue(), converted_value->getValue(),
                                                 converted_traceback->getValue() },
                                               NOEXC);
                emitter.refConsumed(converted_type->getValue(), inst);
                emitter.refConsumed(converted_value->getValue(), inst);
                emitter.refConsumed(converted_traceback->getValue(), inst);

                return emitter.getNone();
            }
            case AST_LangPrimitive::UNCACHE_EXC_INFO: {
                assert(node->args.empty());

                auto* builder = emitter.getBuilder();

                llvm::Value* frame_info = irstate->getFrameInfoVar();
                llvm::Constant* v = getNullPtr(g.llvm_value_type_ptr);
                emitter.setType(v, RefType::BORROWED);

                emitter.createCall(UnwindInfo::cantUnwind(), g.funcs.setFrameExcInfo, { frame_info, v, v, v }, NOEXC);

                return emitter.getNone();
            }
            case AST_LangPrimitive::PRINT_EXPR: {
                assert(node->args.size() == 1);

                CompilerVariable* obj = evalExpr(node->args[0], unw_info);
                ConcreteCompilerVariable* converted = obj->makeConverted(emitter, obj->getBoxType());

                emitter.createCall(unw_info, g.funcs.printExprHelper, converted->getValue());

                return emitter.getNone();
            }
            default:
                RELEASE_ASSERT(0, "%d", node->opcode);
        }
    }

    CompilerVariable* _evalBinExp(AST* node, CompilerVariable* left, CompilerVariable* right, AST_TYPE::AST_TYPE type,
                                  BinExpType exp_type, const UnwindInfo& unw_info) {
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

    CompilerVariable* evalBinOp(AST_BinOp* node, const UnwindInfo& unw_info) {
        CompilerVariable* left = evalExpr(node->left, unw_info);
        CompilerVariable* right = evalExpr(node->right, unw_info);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        CompilerVariable* rtn = this->_evalBinExp(node, left, right, node->op_type, BinOp, unw_info);
        return rtn;
    }

    CompilerVariable* evalAugBinOp(AST_AugBinOp* node, const UnwindInfo& unw_info) {
        CompilerVariable* left = evalExpr(node->left, unw_info);
        CompilerVariable* right = evalExpr(node->right, unw_info);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        CompilerVariable* rtn = this->_evalBinExp(node, left, right, node->op_type, AugBinOp, unw_info);
        return rtn;
    }

    CompilerVariable* evalCompare(AST_Compare* node, const UnwindInfo& unw_info) {
        RELEASE_ASSERT(node->ops.size() == 1, "");

        CompilerVariable* left = evalExpr(node->left, unw_info);
        CompilerVariable* right = evalExpr(node->comparators[0], unw_info);

        assert(left);
        assert(right);

        if (node->ops[0] == AST_TYPE::Is || node->ops[0] == AST_TYPE::IsNot) {
            return doIs(emitter, left, right, node->ops[0] == AST_TYPE::IsNot);
        }

        CompilerVariable* rtn = _evalBinExp(node, left, right, node->ops[0], Compare, unw_info);
        return rtn;
    }

    CompilerVariable* evalCall(AST_Call* node, const UnwindInfo& unw_info) {
        bool is_callattr;
        bool callattr_clsonly = false;
        InternedString attr;
        CompilerVariable* func;
        if (node->func->type == AST_TYPE::Attribute) {
            is_callattr = true;
            callattr_clsonly = false;
            AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
            func = evalExpr(attr_ast->value, unw_info);
            attr = attr_ast->attr;
        } else if (node->func->type == AST_TYPE::ClsAttribute) {
            is_callattr = true;
            callattr_clsonly = true;
            AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
            func = evalExpr(attr_ast->value, unw_info);
            attr = attr_ast->attr;
        } else {
            is_callattr = false;
            func = evalExpr(node->func, unw_info);
        }

        std::vector<CompilerVariable*> args;
        std::vector<BoxedString*>* keyword_names = NULL;
        if (node->keywords.size())
            keyword_names = getKeywordNameStorage(node);

        for (int i = 0; i < node->args.size(); i++) {
            CompilerVariable* a = evalExpr(node->args[i], unw_info);
            args.push_back(a);
        }

        for (int i = 0; i < node->keywords.size(); i++) {
            CompilerVariable* a = evalExpr(node->keywords[i]->value, unw_info);
            args.push_back(a);
        }

        if (node->starargs)
            args.push_back(evalExpr(node->starargs, unw_info));
        if (node->kwargs)
            args.push_back(evalExpr(node->kwargs, unw_info));

        struct ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs != NULL,
                                   node->kwargs != NULL);


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

    CompilerVariable* evalDict(AST_Dict* node, const UnwindInfo& unw_info) {
        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
        emitter.setType(v, RefType::OWNED);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(DICT, v);
        if (node->keys.size()) {
            static BoxedString* setitem_str = getStaticString("__setitem__");
            CompilerVariable* setitem = rtn->getattr(emitter, getEmptyOpInfo(unw_info), setitem_str, true);
            for (int i = 0; i < node->keys.size(); i++) {
                CompilerVariable* key = evalExpr(node->keys[i], unw_info);
                CompilerVariable* value = evalExpr(node->values[i], unw_info);
                assert(key);
                assert(value);

                std::vector<CompilerVariable*> args;
                args.push_back(key);
                args.push_back(value);
                // TODO should use callattr
                CompilerVariable* rtn = setitem->call(emitter, getEmptyOpInfo(unw_info), ArgPassSpec(2), args, NULL);
            }
        }
        return rtn;
    }

    void _addAnnotation(const char* message) {
        llvm::Instruction* inst = emitter.getBuilder()->CreateCall(
            llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::donothing));
        llvm::Metadata* md_vals[] = { llvm::ConstantAsMetadata::get(getConstantInt(0)) };
        llvm::MDNode* mdnode = llvm::MDNode::get(g.context, md_vals);
        inst->setMetadata(message, mdnode);
    }

    CompilerVariable* evalIndex(AST_Index* node, const UnwindInfo& unw_info) { return evalExpr(node->value, unw_info); }

    CompilerVariable* evalLambda(AST_Lambda* node, const UnwindInfo& unw_info) {
        AST_Return* expr = new AST_Return();
        expr->value = node->body;

        std::vector<AST_stmt*> body = { expr };
        CompilerVariable* func = _createFunction(node, unw_info, node->args, body);
        ConcreteCompilerVariable* converted = func->makeConverted(emitter, func->getBoxType());

        return converted;
    }


    CompilerVariable* evalList(AST_List* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], unw_info);
            elts.push_back(value);
        }

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createList);
        emitter.setType(v, RefType::OWNED);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(LIST, v);

        llvm::Value* f = g.funcs.listAppendInternal;
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(
            v, *llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(f->getType())->getElementType())
                    ->param_begin());

        for (int i = 0; i < node->elts.size(); i++) {
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

    ConcreteCompilerVariable* _getGlobal(AST_Name* node, const UnwindInfo& unw_info) {
        if (node->id.s() == "None")
            return emitter.getNone();

        bool do_patchpoint = ENABLE_ICGETGLOBALS;
        if (do_patchpoint) {
            ICSetupInfo* pp = createGetGlobalIC(getOpInfoForNode(node, unw_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(irstate->getGlobals());
            llvm_args.push_back(emitter.setType(embedRelocatablePtr(node->id.getBox(), g.llvm_boxedstring_type_ptr),
                                                RefType::BORROWED));

            llvm::Instruction* uncasted = emitter.createIC(pp, (void*)pyston::getGlobal, llvm_args, unw_info);
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

    CompilerVariable* evalName(AST_Name* node, const UnwindInfo& unw_info) {
        auto scope_info = irstate->getScopeInfo();

        bool is_kill = irstate->getLiveness()->isKill(node, myblock);
        assert(!is_kill || node->id.s()[0] == '#');

        ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(node->id);
        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            assert(!is_kill);
            return _getGlobal(node, unw_info);
        } else if (vst == ScopeInfo::VarScopeType::DEREF) {
            assert(!is_kill);
            assert(scope_info->takesClosure());

            // This is the information on how to look up the variable in the closure object.
            DerefInfo deref_info = scope_info->getDerefInfo(node->id);

            // This code is basically:
            // closure = created_closure;
            // closure = closure->parent;
            // [...]
            // closure = closure->parent;
            // closure->elts[deref_info.offset]
            // Where the parent lookup is done `deref_info.num_parents_from_passed_closure` times
            CompilerVariable* closure = symbol_table[internString(PASSED_CLOSURE_NAME)];
            llvm::Value* closureValue = closure->makeConverted(emitter, CLOSURE)->getValue();
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
            if (symbol_table.find(node->id) == symbol_table.end()) {
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

            InternedString defined_name = getIsDefinedName(node->id);
            ConcreteCompilerVariable* is_defined_var
                = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));

            if (is_defined_var) {
                emitter.createCall(
                    unw_info, g.funcs.assertNameDefined,
                    { i1FromBool(emitter, is_defined_var), embedRelocatablePtr(node->id.c_str(), g.i8_ptr),
                      emitter.setType(embedRelocatablePtr(UnboundLocalError, g.llvm_class_type_ptr), RefType::BORROWED),
                      getConstantInt(true, g.i1) });

                // At this point we know the name must be defined (otherwise the assert would have fired):
                _popFake(defined_name);
            }

            CompilerVariable* rtn = symbol_table[node->id];
            if (is_kill)
                symbol_table.erase(node->id);
            return rtn;
        }
    }

    CompilerVariable* evalNum(AST_Num* node, const UnwindInfo& unw_info) {
        // We can operate on ints and floats unboxed, so don't box those at first;
        // complex and long's have to get boxed so box them immediately.
        if (node->num_type == AST_Num::INT) {
            return makeInt(node->n_int);
        } else if (node->num_type == AST_Num::FLOAT) {
            return makeFloat(node->n_float);
        } else if (node->num_type == AST_Num::COMPLEX) {
            return makePureImaginary(emitter,
                                     irstate->getSourceInfo()->parent_module->getPureImaginaryConstant(node->n_float));
        } else {
            return makeLong(emitter, irstate->getSourceInfo()->parent_module->getLongConstant(node->n_long));
        }
    }

    CompilerVariable* evalRepr(AST_Repr* node, const UnwindInfo& unw_info) {
        CompilerVariable* var = evalExpr(node->value, unw_info);
        ConcreteCompilerVariable* cvar = var->makeConverted(emitter, var->getBoxType());

        std::vector<llvm::Value*> args{ cvar->getValue() };
        llvm::Instruction* uncasted = emitter.createCall(unw_info, g.funcs.repr, args);
        emitter.setType(uncasted, RefType::BORROWED); // Well, really it's owned, and we handoff the ref to the bitcast
        auto rtn = createAfter<llvm::BitCastInst>(uncasted, uncasted, g.llvm_value_type_ptr, "");
        emitter.setType(rtn, RefType::OWNED);

        return new ConcreteCompilerVariable(STR, rtn);
    }

    CompilerVariable* evalSet(AST_Set* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], unw_info);
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

    CompilerVariable* evalSlice(AST_Slice* node, const UnwindInfo& unw_info) {
        CompilerVariable* start, *stop, *step;
        start = node->lower ? evalExpr(node->lower, unw_info) : NULL;
        stop = node->upper ? evalExpr(node->upper, unw_info) : NULL;
        step = node->step ? evalExpr(node->step, unw_info) : NULL;

        return makeSlice(start, stop, step);
    }

    CompilerVariable* evalExtSlice(AST_ExtSlice* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (auto* e : node->dims) {
            elts.push_back(evalSlice(e, unw_info));
        }

        CompilerVariable* rtn = makeTuple(elts);
        return rtn;
    }

    CompilerVariable* evalStr(AST_Str* node, const UnwindInfo& unw_info) {
        if (node->str_type == AST_Str::STR) {
            llvm::Value* rtn
                = embedRelocatablePtr(irstate->getSourceInfo()->parent_module->getStringConstant(node->str_data, true),
                                      g.llvm_value_type_ptr);
            emitter.setType(rtn, RefType::BORROWED);

            return new ConcreteCompilerVariable(STR, rtn);
        } else if (node->str_type == AST_Str::UNICODE) {
            llvm::Value* rtn = embedRelocatablePtr(
                irstate->getSourceInfo()->parent_module->getUnicodeConstant(node->str_data), g.llvm_value_type_ptr);
            emitter.setType(rtn, RefType::BORROWED);

            return new ConcreteCompilerVariable(typeFromClass(unicode_cls), rtn);
        } else {
            RELEASE_ASSERT(0, "%d", node->str_type);
        }
    }

    CompilerVariable* evalSubscript(AST_Subscript* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);
        CompilerVariable* slice = evalSlice(node->slice, unw_info);

        CompilerVariable* rtn = value->getitem(emitter, getOpInfoForNode(node, unw_info), slice);
        return rtn;
    }

    CompilerVariable* evalTuple(AST_Tuple* node, const UnwindInfo& unw_info) {
        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], unw_info);
            elts.push_back(value);
        }

        CompilerVariable* rtn = makeTuple(elts);
        return rtn;
    }

    CompilerVariable* evalUnaryOp(AST_UnaryOp* node, const UnwindInfo& unw_info) {
        CompilerVariable* operand = evalExpr(node->operand, unw_info);

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

    CompilerVariable* evalYield(AST_Yield* node, const UnwindInfo& unw_info) {
        CompilerVariable* generator = symbol_table[internString(PASSED_GENERATOR_NAME)];
        assert(generator);
        ConcreteCompilerVariable* convertedGenerator = generator->makeConverted(emitter, generator->getBoxType());

        CompilerVariable* value = node->value ? evalExpr(node->value, unw_info) : emitter.getNone();
        ConcreteCompilerVariable* convertedValue = value->makeConverted(emitter, value->getBoxType());

        std::vector<llvm::Value*> args;
        args.push_back(convertedGenerator->getValue());
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

    CompilerVariable* evalMakeClass(AST_MakeClass* mkclass, const UnwindInfo& unw_info) {
        assert(mkclass->type == AST_TYPE::MakeClass && mkclass->class_def->type == AST_TYPE::ClassDef);
        AST_ClassDef* node = mkclass->class_def;
        ScopeInfo* scope_info = irstate->getScopeInfoForNode(node);
        assert(scope_info);

        std::vector<CompilerVariable*> bases;
        for (auto b : node->bases) {
            CompilerVariable* base = evalExpr(b, unw_info);
            bases.push_back(base);
        }

        CompilerVariable* _bases_tuple = makeTuple(bases);
        ConcreteCompilerVariable* bases_tuple = _bases_tuple->makeConverted(emitter, _bases_tuple->getBoxType());

        std::vector<CompilerVariable*> decorators;
        for (auto d : node->decorator_list) {
            decorators.push_back(evalExpr(d, unw_info));
        }

        FunctionMetadata* md = wrapFunction(node, nullptr, node->body, irstate->getSourceInfo());

        // TODO duplication with _createFunction:
        CompilerVariable* created_closure = NULL;
        if (scope_info->takesClosure()) {
            if (irstate->getScopeInfo()->createsClosure()) {
                created_closure = symbol_table[internString(CREATED_CLOSURE_NAME)];
            } else {
                assert(irstate->getScopeInfo()->passesThroughClosure());
                created_closure = symbol_table[internString(PASSED_CLOSURE_NAME)];
            }
            assert(created_closure);
        }

        // TODO kind of silly to create the function just to usually-delete it afterwards;
        // one reason to do this is to pass the closure through if necessary,
        // but since the classdef can't create its own closure, shouldn't need to explicitly
        // create that scope to pass the closure through.
        assert(irstate->getSourceInfo()->scoping->areGlobalsFromModule());
        CompilerVariable* func = makeFunction(emitter, md, created_closure, irstate->getGlobalsIfCustom(), {});

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

    CompilerVariable* _createFunction(AST* node, const UnwindInfo& unw_info, AST_arguments* args,
                                      const std::vector<AST_stmt*>& body) {
        FunctionMetadata* md = wrapFunction(node, args, body, irstate->getSourceInfo());

        std::vector<ConcreteCompilerVariable*> defaults;
        for (auto d : args->defaults) {
            CompilerVariable* e = evalExpr(d, unw_info);
            ConcreteCompilerVariable* converted = e->makeConverted(emitter, e->getBoxType());
            defaults.push_back(converted);
        }

        CompilerVariable* created_closure = NULL;

        bool takes_closure;
        // Optimization: when compiling a module, it's nice to not have to run analyses into the
        // entire module's source code.
        // If we call getScopeInfoForNode, that will trigger an analysis of that function tree,
        // but we're only using it here to figure out if that function takes a closure.
        // Top level functions never take a closure, so we can skip the analysis.
        if (irstate->getSourceInfo()->ast->type == AST_TYPE::Module)
            takes_closure = false;
        else {
            takes_closure = irstate->getScopeInfoForNode(node)->takesClosure();
        }

        bool is_generator = md->source->is_generator;

        if (takes_closure) {
            if (irstate->getScopeInfo()->createsClosure()) {
                created_closure = symbol_table[internString(CREATED_CLOSURE_NAME)];
            } else {
                assert(irstate->getScopeInfo()->passesThroughClosure());
                created_closure = symbol_table[internString(PASSED_CLOSURE_NAME)];
            }
            assert(created_closure);
        }

        CompilerVariable* func = makeFunction(emitter, md, created_closure, irstate->getGlobalsIfCustom(), defaults);

        return func;
    }

    CompilerVariable* evalMakeFunction(AST_MakeFunction* mkfn, const UnwindInfo& unw_info) {
        AST_FunctionDef* node = mkfn->function_def;
        std::vector<CompilerVariable*> decorators;
        for (auto d : node->decorator_list) {
            decorators.push_back(evalExpr(d, unw_info));
        }

        CompilerVariable* func = _createFunction(node, unw_info, node->args, node->body);

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
                print_ast(node);
                llvm::outs().flush();
                printf("\n");
            }

#ifndef NDEBUG
            // That's not really a speculation.... could potentially handle this here, but
            // I think it's better to just not generate bad speculations:
            if (rtn->canConvertTo(speculated_type)) {
                auto source = irstate->getSourceInfo();
                printf("On %s:%d, function %s:\n", source->getFn()->c_str(), source->body[0]->lineno,
                       source->getName()->c_str());
                irstate->getSourceInfo()->cfg->print();
            }
            RELEASE_ASSERT(!rtn->canConvertTo(speculated_type), "%s %s", rtn->getType()->debugName().c_str(),
                           speculated_type->debugName().c_str());
#endif

            ConcreteCompilerVariable* old_rtn = rtn->makeConverted(emitter, UNKNOWN);

            llvm::Value* guard_check = old_rtn->makeClassCheck(emitter, speculated_class);
            assert(guard_check->getType() == g.i1);
            createExprTypeGuard(guard_check, node, old_rtn->getValue(), unw_info.current_stmt);

            rtn = unboxVar(speculated_type, old_rtn->getValue());
        }

        assert(rtn);
        assert(rtn->getType()->isUsable());

        return rtn;
    }

    CompilerVariable* evalSlice(AST_slice* node, const UnwindInfo& unw_info) {
        // printf("%d expr: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        CompilerVariable* rtn = NULL;
        switch (node->type) {
            case AST_TYPE::ExtSlice:
                rtn = evalExtSlice(ast_cast<AST_ExtSlice>(node), unw_info);
                break;
            case AST_TYPE::Ellipsis:
                rtn = getEllipsis();
                break;
            case AST_TYPE::Index:
                rtn = evalIndex(ast_cast<AST_Index>(node), unw_info);
                break;
            case AST_TYPE::Slice:
                rtn = evalSlice(ast_cast<AST_Slice>(node), unw_info);
                break;
            default:
                printf("Unhandled slice type: %d (irgenerator.cpp:" STRINGIFY(__LINE__) ")\n", node->type);
                exit(1);
        }
        return evalSliceExprPost(node, unw_info, rtn);
    }

    CompilerVariable* evalExpr(AST_expr* node, const UnwindInfo& unw_info) {
        // printf("%d expr: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        CompilerVariable* rtn = NULL;
        switch (node->type) {
            case AST_TYPE::Attribute:
                rtn = evalAttribute(ast_cast<AST_Attribute>(node), unw_info);
                break;
            case AST_TYPE::AugBinOp:
                rtn = evalAugBinOp(ast_cast<AST_AugBinOp>(node), unw_info);
                break;
            case AST_TYPE::BinOp:
                rtn = evalBinOp(ast_cast<AST_BinOp>(node), unw_info);
                break;
            case AST_TYPE::Call:
                rtn = evalCall(ast_cast<AST_Call>(node), unw_info);
                break;
            case AST_TYPE::Compare:
                rtn = evalCompare(ast_cast<AST_Compare>(node), unw_info);
                break;
            case AST_TYPE::Dict:
                rtn = evalDict(ast_cast<AST_Dict>(node), unw_info);
                break;
            case AST_TYPE::Lambda:
                rtn = evalLambda(ast_cast<AST_Lambda>(node), unw_info);
                break;
            case AST_TYPE::List:
                rtn = evalList(ast_cast<AST_List>(node), unw_info);
                break;
            case AST_TYPE::Name:
                rtn = evalName(ast_cast<AST_Name>(node), unw_info);
                break;
            case AST_TYPE::Num:
                rtn = evalNum(ast_cast<AST_Num>(node), unw_info);
                break;
            case AST_TYPE::Repr:
                rtn = evalRepr(ast_cast<AST_Repr>(node), unw_info);
                break;
            case AST_TYPE::Set:
                rtn = evalSet(ast_cast<AST_Set>(node), unw_info);
                break;
            case AST_TYPE::Str:
                rtn = evalStr(ast_cast<AST_Str>(node), unw_info);
                break;
            case AST_TYPE::Subscript:
                rtn = evalSubscript(ast_cast<AST_Subscript>(node), unw_info);
                break;
            case AST_TYPE::Tuple:
                rtn = evalTuple(ast_cast<AST_Tuple>(node), unw_info);
                break;
            case AST_TYPE::UnaryOp:
                rtn = evalUnaryOp(ast_cast<AST_UnaryOp>(node), unw_info);
                break;
            case AST_TYPE::Yield:
                rtn = evalYield(ast_cast<AST_Yield>(node), unw_info);
                break;

            // pseudo-nodes
            case AST_TYPE::ClsAttribute:
                rtn = evalClsAttribute(ast_cast<AST_ClsAttribute>(node), unw_info);
                break;
            case AST_TYPE::LangPrimitive:
                rtn = evalLangPrimitive(ast_cast<AST_LangPrimitive>(node), unw_info);
                break;
            case AST_TYPE::MakeClass:
                rtn = evalMakeClass(ast_cast<AST_MakeClass>(node), unw_info);
                break;
            case AST_TYPE::MakeFunction:
                rtn = evalMakeFunction(ast_cast<AST_MakeFunction>(node), unw_info);
                break;
            default:
                printf("Unhandled expr type: %d (irgenerator.cpp:" STRINGIFY(__LINE__) ")\n", node->type);
                exit(1);
        }
        return evalSliceExprPost(node, unw_info, rtn);
    }

    void _setFake(InternedString name, CompilerVariable* val) {
        assert(name.s()[0] == '!');
        CompilerVariable*& cur = symbol_table[name];
        assert(cur == NULL);
        cur = val;
    }

    // whether a Python variable FOO might be undefined or not is determined by whether the corresponding is_defined_FOO
    // variable is present in our symbol table. If it is, then it *might* be undefined. If it isn't, then it either is
    // definitely defined, or definitely isn't.
    //
    // to check whether a variable is in our symbol table, call _getFake with allow_missing = true and check whether the
    // result is NULL.
    CompilerVariable* _getFake(InternedString name, bool allow_missing = false) {
        assert(name.s()[0] == '!');
        auto it = symbol_table.find(name);
        if (it == symbol_table.end()) {
            assert(allow_missing);
            return NULL;
        }
        return it->second;
    }

    CompilerVariable* _popFake(InternedString name, bool allow_missing = false) {
        CompilerVariable* rtn = _getFake(name, allow_missing);
        symbol_table.erase(name);
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
    void _doSet(int vreg, InternedString name, ScopeInfo::VarScopeType vst, CompilerVariable* val,
                const UnwindInfo& unw_info) {
        assert(name.s() != "None");
        assert(name.s() != FRAME_INFO_PTR_NAME);
        assert(val->getType()->isUsable());

        assert(vst != ScopeInfo::VarScopeType::DEREF);

        if (vst == ScopeInfo::VarScopeType::GLOBAL) {
            if (irstate->getSourceInfo()->scoping->areGlobalsFromModule()) {
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

            CompilerVariable* prev = symbol_table[name];
            symbol_table[name] = val;

            // Clear out the is_defined name since it is now definitely defined:
            assert(!isIsDefinedName(name.s()));
            InternedString defined_name = getIsDefinedName(name);
            bool maybe_was_undefined = _popFake(defined_name, true);

            if (vst == ScopeInfo::VarScopeType::CLOSURE) {
                size_t offset = irstate->getScopeInfo()->getClosureOffset(name);

                // This is basically `closure->elts[offset] = val;`
                CompilerVariable* closure = symbol_table[internString(CREATED_CLOSURE_NAME)];
                llvm::Value* closureValue = closure->makeConverted(emitter, CLOSURE)->getValue();
                llvm::Value* gep = getClosureElementGep(emitter, closureValue, offset);
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

    void _doSet(AST_Name* name, CompilerVariable* val, const UnwindInfo& unw_info) {
        auto scope_info = irstate->getScopeInfo();
        auto vst = name->lookup_type;
        if (vst == ScopeInfo::VarScopeType::UNKNOWN)
            vst = scope_info->getScopeTypeOfName(name->id);
        assert(vst != ScopeInfo::VarScopeType::DEREF);
        _doSet(name->vreg, name->id, vst, val, unw_info);
    }

    void _doSetattr(AST_Attribute* target, CompilerVariable* val, const UnwindInfo& unw_info) {
        CompilerVariable* t = evalExpr(target->value, unw_info);
        t->setattr(emitter, getEmptyOpInfo(unw_info), target->attr.getBox(), val);
    }

    void _doSetitem(AST_Subscript* target, CompilerVariable* val, const UnwindInfo& unw_info) {
        CompilerVariable* tget = evalExpr(target->value, unw_info);
        CompilerVariable* slice = evalSlice(target->slice, unw_info);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        ConcreteCompilerVariable* converted_val = val->makeConverted(emitter, val->getBoxType());

        // TODO add a CompilerVariable::setattr, which can (similar to getitem)
        // statically-resolve the function if possible, and only fall back to
        // patchpoints if it couldn't.
        bool do_patchpoint = ENABLE_ICSETITEMS;
        if (do_patchpoint) {
            ICSetupInfo* pp = createSetitemIC(getEmptyOpInfo(unw_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());
            llvm_args.push_back(converted_val->getValue());

            emitter.createIC(pp, (void*)pyston::setitem, llvm_args, unw_info);
        } else {
            emitter.createCall3(unw_info, g.funcs.setitem, converted_target->getValue(), converted_slice->getValue(),
                                converted_val->getValue());
        }
    }

    void _doUnpackTuple(AST_Tuple* target, CompilerVariable* val, const UnwindInfo& unw_info) {
        int ntargets = target->elts.size();

        std::vector<CompilerVariable*> unpacked = val->unpack(emitter, getOpInfoForNode(target, unw_info), ntargets);

#ifndef NDEBUG
        for (auto e : target->elts) {
            ASSERT(e->type == AST_TYPE::Name && ast_cast<AST_Name>(e)->id.s()[0] == '#',
                   "should only be unpacking tuples into cfg-generated names!");
        }
#endif

        for (int i = 0; i < ntargets; i++) {
            CompilerVariable* thisval = unpacked[i];
            _doSet(target->elts[i], thisval, unw_info);
        }
    }

    void _doSet(AST* target, CompilerVariable* val, const UnwindInfo& unw_info) {
        switch (target->type) {
            case AST_TYPE::Attribute:
                _doSetattr(ast_cast<AST_Attribute>(target), val, unw_info);
                break;
            case AST_TYPE::Name:
                _doSet(ast_cast<AST_Name>(target), val, unw_info);
                break;
            case AST_TYPE::Subscript:
                _doSetitem(ast_cast<AST_Subscript>(target), val, unw_info);
                break;
            case AST_TYPE::Tuple:
                _doUnpackTuple(ast_cast<AST_Tuple>(target), val, unw_info);
                break;
            default:
                ASSERT(0, "Unknown type for IRGenerator: %d", target->type);
                abort();
        }
    }

    void doAssert(AST_Assert* node, const UnwindInfo& unw_info) {
        // cfg translates all asserts into only 'assert 0' on the failing path.
        AST_expr* test = node->test;
        assert(test->type == AST_TYPE::Num);
        AST_Num* num = ast_cast<AST_Num>(test);
        assert(num->num_type == AST_Num::INT);
        assert(num->n_int == 0);

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
        if (node->msg) {
            CompilerVariable* msg = evalExpr(node->msg, unw_info);
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

    void doAssign(AST_Assign* node, const UnwindInfo& unw_info) {
        CompilerVariable* val = evalExpr(node->value, unw_info);

        for (int i = 0; i < node->targets.size(); i++) {
            _doSet(node->targets[i], val, unw_info);
        }
    }

    void doDelete(AST_Delete* node, const UnwindInfo& unw_info) {
        for (AST_expr* target : node->targets) {
            switch (target->type) {
                case AST_TYPE::Subscript:
                    _doDelitem(static_cast<AST_Subscript*>(target), unw_info);
                    break;
                case AST_TYPE::Attribute:
                    _doDelAttr(static_cast<AST_Attribute*>(target), unw_info);
                    break;
                case AST_TYPE::Name:
                    _doDelName(static_cast<AST_Name*>(target), unw_info);
                    break;
                default:
                    ASSERT(0, "Unsupported del target: %d", target->type);
                    abort();
            }
        }
    }

    // invoke delitem in objmodel.cpp, which will invoke the listDelitem of list
    void _doDelitem(AST_Subscript* target, const UnwindInfo& unw_info) {
        CompilerVariable* tget = evalExpr(target->value, unw_info);
        CompilerVariable* slice = evalSlice(target->slice, unw_info);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        bool do_patchpoint = ENABLE_ICDELITEMS;
        if (do_patchpoint) {
            ICSetupInfo* pp = createDelitemIC(getEmptyOpInfo(unw_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());

            emitter.createIC(pp, (void*)pyston::delitem, llvm_args, unw_info);
        } else {
            emitter.createCall2(unw_info, g.funcs.delitem, converted_target->getValue(), converted_slice->getValue());
        }
    }

    void _doDelAttr(AST_Attribute* node, const UnwindInfo& unw_info) {
        CompilerVariable* value = evalExpr(node->value, unw_info);
        value->delattr(emitter, getEmptyOpInfo(unw_info), node->attr.getBox());
    }

    void _doDelName(AST_Name* target, const UnwindInfo& unw_info) {
        // Hack: we don't have a bytecode for temporary-kills:
        if (target->id.s()[0] == '#') {
            // The refcounter will automatically delete this object.
            return;
        }

        auto scope_info = irstate->getScopeInfo();
        ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(target->id);
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

        CompilerVariable* prev = symbol_table.count(target->id) ? symbol_table[target->id] : NULL;

        InternedString defined_name = getIsDefinedName(target->id);
        ConcreteCompilerVariable* is_defined_var = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));

        _setVRegIfUserVisible(target->vreg, []() { return getNullPtr(g.llvm_value_type_ptr); }, prev,
                              (bool)is_defined_var);

        if (symbol_table.count(target->id) == 0) {
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
                { i1FromBool(emitter, is_defined_var), embedConstantPtr(target->id.c_str(), g.i8_ptr),
                  emitter.setType(embedRelocatablePtr(NameError, g.llvm_class_type_ptr), RefType::BORROWED),
                  getConstantInt(true /*local_error_msg*/, g.i1) });
            _popFake(defined_name);
        }

        symbol_table.erase(target->id);
    }

    void doExec(AST_Exec* node, const UnwindInfo& unw_info) {
        CompilerVariable* body = evalExpr(node->body, unw_info);
        llvm::Value* vbody = body->makeConverted(emitter, body->getBoxType())->getValue();

        llvm::Value* vglobals;
        if (node->globals) {
            CompilerVariable* globals = evalExpr(node->globals, unw_info);
            vglobals = globals->makeConverted(emitter, globals->getBoxType())->getValue();
        } else {
            vglobals = getNullPtr(g.llvm_value_type_ptr);
        }

        llvm::Value* vlocals;
        if (node->locals) {
            CompilerVariable* locals = evalExpr(node->locals, unw_info);
            vlocals = locals->makeConverted(emitter, locals->getBoxType())->getValue();
        } else {
            vlocals = getNullPtr(g.llvm_value_type_ptr);
        }

        static_assert(sizeof(FutureFlags) == 4, "");
        emitter.createCall(unw_info, g.funcs.exec,
                           { vbody, vglobals, vlocals, getConstantInt(irstate->getSourceInfo()->future_flags, g.i32) });
    }

    void doPrint(AST_Print* node, const UnwindInfo& unw_info) {
        ConcreteCompilerVariable* dest = NULL;
        if (node->dest) {
            auto d = evalExpr(node->dest, unw_info);
            dest = d->makeConverted(emitter, d->getBoxType());
        } else {
            dest = new ConcreteCompilerVariable(UNKNOWN,
                                                emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED));
        }
        assert(dest);

        assert(node->values.size() <= 1);
        ConcreteCompilerVariable* converted;

        if (node->values.size() == 1) {
            CompilerVariable* var = evalExpr(node->values[0], unw_info);
            converted = var->makeConverted(emitter, var->getBoxType());
        } else {
            converted = new ConcreteCompilerVariable(UNKNOWN, getNullPtr(g.llvm_value_type_ptr));
        }

        emitter.createCall3(unw_info, g.funcs.printHelper, dest->getValue(), converted->getValue(),
                            getConstantInt(node->nl, g.i1));
    }

    void doReturn(AST_Return* node, const UnwindInfo& unw_info) {
        assert(!unw_info.hasHandler());

        CompilerVariable* val;
        if (node->value == NULL) {
            val = emitter.getNone();
        } else {
            val = evalExpr(node->value, unw_info);
        }
        assert(val);

        ConcreteCompilerType* opt_rtn_type = irstate->getReturnType();
        if (irstate->getReturnType()->llvmType() == val->getConcreteType()->llvmType())
            opt_rtn_type = val->getConcreteType();

        ConcreteCompilerVariable* rtn = val->makeConverted(emitter, opt_rtn_type);

        if (!irstate->getCurFunction()->entry_descriptor)
            emitter.getBuilder()->CreateCall(g.funcs.deinitFrame, irstate->getFrameInfoVar());

        assert(rtn->getValue());
        auto ret_inst = emitter.getBuilder()->CreateRet(rtn->getValue());

        irstate->getRefcounts()->refConsumed(rtn->getValue(), ret_inst);

        symbol_table.clear();

        endBlock(DEAD);
    }

    void doBranch(AST_Branch* node, const UnwindInfo& unw_info) {
        assert(!unw_info.hasHandler());

        assert(node->iftrue->idx > myblock->idx);
        assert(node->iffalse->idx > myblock->idx);

        CompilerVariable* val = evalExpr(node->test, unw_info);
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

    void doExpr(AST_Expr* node, const UnwindInfo& unw_info) { CompilerVariable* var = evalExpr(node->value, unw_info); }

    void doOSRExit(llvm::BasicBlock* normal_target, AST_Jump* osr_key) {
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
        OSREntryDescriptor* entry = OSREntryDescriptor::create(irstate->getMD(), osr_key, irstate->getExceptionStyle());
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
    }

    void doJump(AST_Jump* node, const UnwindInfo& unw_info) {
        endBlock(FINISHED);

        llvm::BasicBlock* target = entry_blocks[node->target];

        if (ENABLE_OSR && node->target->idx < myblock->idx && irstate->getEffortLevel() < EffortLevel::MAXIMAL) {
            assert(node->target->predecessors.size() > 1);
            doOSRExit(target, node);
        } else {
            emitter.getBuilder()->CreateBr(target);
        }
    }

    void doRaise(AST_Raise* node, const UnwindInfo& unw_info) {
        // It looks like ommitting the second and third arguments are equivalent to passing None,
        // but ommitting the first argument is *not* the same as passing None.

        ExceptionStyle target_exception_style;

        if (unw_info.hasHandler())
            target_exception_style = CAPI;
        else
            target_exception_style = irstate->getExceptionStyle();

        if (node->arg0 == NULL) {
            assert(!node->arg1);
            assert(!node->arg2);

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
        for (auto a : { node->arg0, node->arg1, node->arg2 }) {
            if (a) {
                CompilerVariable* v = evalExpr(a, unw_info);
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

    void doStmt(AST_stmt* node, const UnwindInfo& unw_info) {
        // printf("%d stmt: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        switch (node->type) {
            case AST_TYPE::Assert:
                doAssert(ast_cast<AST_Assert>(node), unw_info);
                break;
            case AST_TYPE::Assign:
                doAssign(ast_cast<AST_Assign>(node), unw_info);
                break;
            case AST_TYPE::Delete:
                doDelete(ast_cast<AST_Delete>(node), unw_info);
                break;
            case AST_TYPE::Exec:
                doExec(ast_cast<AST_Exec>(node), unw_info);
                break;
            case AST_TYPE::Expr:
                if ((((AST_Expr*)node)->value)->type != AST_TYPE::Str)
                    doExpr(ast_cast<AST_Expr>(node), unw_info);
                break;
            // case AST_TYPE::If:
            // doIf(ast_cast<AST_If>(node));
            // break;
            // case AST_TYPE::Import:
            //     doImport(ast_cast<AST_Import>(node), unw_info);
            //     break;
            // case AST_TYPE::ImportFrom:
            //     doImportFrom(ast_cast<AST_ImportFrom>(node), unw_info);
            //     break;
            case AST_TYPE::Global:
                // Should have been handled already
                break;
            case AST_TYPE::Pass:
                break;
            case AST_TYPE::Print:
                doPrint(ast_cast<AST_Print>(node), unw_info);
                break;
            case AST_TYPE::Return:
                assert(!unw_info.hasHandler());
                doReturn(ast_cast<AST_Return>(node), unw_info);
                break;
            case AST_TYPE::Branch:
                assert(!unw_info.hasHandler());
                doBranch(ast_cast<AST_Branch>(node), unw_info);
                break;
            case AST_TYPE::Jump:
                assert(!unw_info.hasHandler());
                doJump(ast_cast<AST_Jump>(node), unw_info);
                break;
            case AST_TYPE::Invoke: {
                assert(!unw_info.hasHandler());
                AST_Invoke* invoke = ast_cast<AST_Invoke>(node);

                doStmt(invoke->stmt, UnwindInfo(node, entry_blocks[invoke->exc_dest]));

                assert(state == RUNNING || state == DEAD);
                if (state == RUNNING) {
                    emitter.getBuilder()->CreateBr(entry_blocks[invoke->normal_dest]);
                    endBlock(FINISHED);
                }

                break;
            }
            case AST_TYPE::Raise:
                doRaise(ast_cast<AST_Raise>(node), unw_info);
                break;
            default:
                printf("Unhandled stmt type at " __FILE__ ":" STRINGIFY(__LINE__) ": %d\n", node->type);
                exit(1);
        }
    }

    void loadArgument(InternedString name, ConcreteCompilerType* t, llvm::Value* v, const UnwindInfo& unw_info) {
        assert(name.s() != FRAME_INFO_PTR_NAME);
        CompilerVariable* var = unboxVar(t, v);
        auto cfg = irstate->getSourceInfo()->cfg;
        auto vst = irstate->getScopeInfo()->getScopeTypeOfName(name);
        int vreg = -1;
        if (vst == ScopeInfo::VarScopeType::FAST || vst == ScopeInfo::VarScopeType::CLOSURE) {
            vreg = cfg->getVRegInfo().getVReg(name);
        }
        _doSet(vreg, name, vst, var, unw_info);
    }

    void loadArgument(AST_expr* name, ConcreteCompilerType* t, llvm::Value* v, const UnwindInfo& unw_info) {
        CompilerVariable* var = unboxVar(t, v);
        _doSet(name, var, unw_info);
    }

    bool allowableFakeEndingSymbol(InternedString name) {
        // TODO this would be a great place to be able to use interned versions of the static names...
        return isIsDefinedName(name.s()) || name.s() == PASSED_CLOSURE_NAME || name.s() == CREATED_CLOSURE_NAME
               || name.s() == PASSED_GENERATOR_NAME;
    }

    void endBlock(State new_state) {
        assert(state == RUNNING);

        // cf->func->dump();

        SourceInfo* source = irstate->getSourceInfo();
        ScopeInfo* scope_info = irstate->getScopeInfo();

        // Sort the names here to make the process deterministic:
        std::map<InternedString, CompilerVariable*> sorted_symbol_table(symbol_table.begin(), symbol_table.end());
        for (const auto& p : sorted_symbol_table) {
            assert(p.first.s() != FRAME_INFO_PTR_NAME);
            assert(p.second->getType()->isUsable());
            if (allowableFakeEndingSymbol(p.first))
                continue;

            // ASSERT(p.first[0] != '!' || isIsDefinedName(p.first), "left a fake variable in the real
            // symbol table? '%s'", p.first.c_str());

            if (!irstate->getLiveness()->isLiveAtEnd(p.first, myblock)) {
                symbol_table.erase(getIsDefinedName(p.first));
                symbol_table.erase(p.first);
            } else if (irstate->getPhis()->isRequiredAfter(p.first, myblock)) {
                assert(scope_info->getScopeTypeOfName(p.first) != ScopeInfo::VarScopeType::GLOBAL);
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(p.first, myblock);
                assert(phi_type->isUsable());
                // printf("Converting %s from %s to %s\n", p.first.c_str(),
                // p.second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                // printf("have to convert %s from %s to %s\n", p.first.c_str(),
                // p.second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                ConcreteCompilerVariable* v = p.second->makeConverted(emitter, phi_type);
                symbol_table[p.first] = v;
            } else {
                if (myblock->successors.size()) {
                    // TODO getTypeAtBlockEnd will automatically convert up to the concrete type, which we don't
                    // want
                    // here, but this is just for debugging so I guess let it happen for now:
                    ConcreteCompilerType* ending_type = types->getTypeAtBlockEnd(p.first, myblock);
                    RELEASE_ASSERT(p.second->canConvertTo(ending_type), "%s is supposed to be %s, but somehow is %s",
                                   p.first.c_str(), ending_type->debugName().c_str(),
                                   p.second->getType()->debugName().c_str());
                }
            }
        }

        const PhiAnalysis::RequiredSet& all_phis = irstate->getPhis()->getAllRequiredAfter(myblock);
        for (PhiAnalysis::RequiredSet::const_iterator it = all_phis.begin(), end = all_phis.end(); it != end; ++it) {
            if (VERBOSITY() >= 3)
                printf("phi will be required for %s\n", it->c_str());
            assert(scope_info->getScopeTypeOfName(*it) != ScopeInfo::VarScopeType::GLOBAL);
            CompilerVariable*& cur = symbol_table[*it];

            InternedString defined_name = getIsDefinedName(*it);

            if (cur != NULL) {
                // printf("defined on this path; ");

                ConcreteCompilerVariable* is_defined
                    = static_cast<ConcreteCompilerVariable*>(_popFake(defined_name, true));

                if (irstate->getPhis()->isPotentiallyUndefinedAfter(*it, myblock)) {
                    // printf("is potentially undefined later, so marking it defined\n");
                    if (is_defined) {
                        _setFake(defined_name, is_defined);
                    } else {
                        _setFake(defined_name, makeBool(1));
                    }
                } else {
                    // printf("is defined in all later paths, so not marking\n");
                    assert(!is_defined);
                }
            } else {
                // printf("no st entry, setting undefined\n");
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(*it, myblock);
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
                _setFake(defined_name, makeBool(0));
            }
        }

        state = new_state;
    }

public:
    void addFrameStackmapArgs(PatchpointInfo* pp, std::vector<llvm::Value*>& stackmap_args) override {
        int initial_args = stackmap_args.size();

        // For deopts we need to add the compiler created names to the stackmap
        if (ENABLE_FRAME_INTROSPECTION && pp->isDeopt()) {
            // TODO: don't need to use a sorted symbol table if we're explicitly recording the names!
            // nice for debugging though.
            typedef std::pair<InternedString, CompilerVariable*> Entry;
            std::vector<Entry> sorted_symbol_table(symbol_table.begin(), symbol_table.end());
            std::sort(sorted_symbol_table.begin(), sorted_symbol_table.end(),
                      [](const Entry& lhs, const Entry& rhs) { return lhs.first < rhs.first; });
            for (const auto& p : sorted_symbol_table) {
                // We never have to include non compiler generated vars because the user visible variables are stored
                // inside the vregs array.
                if (!p.first.isCompilerCreatedName())
                    continue;

                CompilerVariable* v = p.second;
                v->serializeToFrame(stackmap_args);
                pp->addFrameVar(p.first.s(), v->getType());
            }
        }

        int num_frame_args = stackmap_args.size() - initial_args;
        pp->setNumFrameArgs(num_frame_args);
    }

    EndingState getEndingSymbolTable() override {
        assert(state == FINISHED || state == DEAD);

        SourceInfo* source = irstate->getSourceInfo();

        SymbolTable* st = new SymbolTable(symbol_table);
        ConcreteSymbolTable* phi_st = new ConcreteSymbolTable();

        // This should have been consumed:
        assert(incoming_exc_state.empty());

        for (auto&& p : symbol_table) {
            ASSERT(p.second->getType()->isUsable(), "%s", p.first.c_str());
        }

        if (myblock->successors.size() == 0) {
            st->clear();
            symbol_table.clear();
            return EndingState(st, phi_st, curblock, outgoing_exc_state);
        } else if (myblock->successors.size() > 1) {
            // Since there are no critical edges, all successors come directly from this node,
            // so there won't be any required phis.
            return EndingState(st, phi_st, curblock, outgoing_exc_state);
        }

        assert(myblock->successors.size() == 1); // other cases should have been handled

        // In theory this case shouldn't be necessary:
        if (myblock->successors[0]->predecessors.size() == 1) {
            // If the next block has a single predecessor, don't have to
            // emit any phis.
            // Should probably not emit no-op jumps like this though.
            return EndingState(st, phi_st, curblock, outgoing_exc_state);
        }

        // We have one successor, but they have more than one predecessor.
        // We're going to sort out which symbols need to go in phi_st and which belong inst.
        for (SymbolTable::iterator it = st->begin(); it != st->end();) {
            if (allowableFakeEndingSymbol(it->first) || irstate->getPhis()->isRequiredAfter(it->first, myblock)) {
                // this conversion should have already happened... should refactor this.
                ConcreteCompilerType* ending_type;
                if (isIsDefinedName(it->first.s())) {
                    assert(it->second->getType() == BOOL);
                    ending_type = BOOL;
                } else if (it->first.s() == PASSED_CLOSURE_NAME) {
                    ending_type = getPassedClosureType();
                } else if (it->first.s() == CREATED_CLOSURE_NAME) {
                    ending_type = getCreatedClosureType();
                } else if (it->first.s() == PASSED_GENERATOR_NAME) {
                    ending_type = GENERATOR;
                } else if (it->first.s() == FRAME_INFO_PTR_NAME) {
                    ending_type = FRAME_INFO;
                } else {
                    ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                }
                assert(ending_type->isUsable());
                //(*phi_st)[it->first] = it->second->makeConverted(emitter, it->second->getConcreteType());
                (*phi_st)[it->first] = it->second->makeConverted(emitter, ending_type);
                it = st->erase(it);
            } else {
                ++it;
            }
        }
        return EndingState(st, phi_st, curblock, outgoing_exc_state);
    }

    void giveLocalSymbol(InternedString name, CompilerVariable* var) override {
        assert(name.s() != "None");
        assert(name.s() != FRAME_INFO_PTR_NAME);
        ASSERT(irstate->getScopeInfo()->getScopeTypeOfName(name) != ScopeInfo::VarScopeType::GLOBAL, "%s",
               name.c_str());

        ASSERT(var->getType()->isUsable(), "%s", name.c_str());

        CompilerVariable*& cur = symbol_table[name];
        assert(cur == NULL);
        cur = var;
    }

    void copySymbolsFrom(SymbolTable* st) override {
        assert(st);
        DupCache cache;
        for (SymbolTable::iterator it = st->begin(); it != st->end(); ++it) {
            // printf("Copying in %s, a %s\n", it->first.c_str(), it->second->getType()->debugName().c_str());
            symbol_table[it->first] = it->second->dup(cache);
            assert(symbol_table[it->first]->getType()->isUsable());
        }
    }

    ConcreteCompilerType* getPassedClosureType() {
        // TODO could know the exact closure shape
        return CLOSURE;
    }

    ConcreteCompilerType* getCreatedClosureType() {
        // TODO could know the exact closure shape
        return CLOSURE;
    }

    void doFunctionEntry(const ParamNames& param_names, const std::vector<ConcreteCompilerType*>& arg_types) override {
        assert(param_names.totalParameters() == arg_types.size());

        auto scope_info = irstate->getScopeInfo();


        llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin();
        llvm::Value* passed_closure = NULL;
        llvm::Value* generator = NULL;
        llvm::Value* globals = NULL;

        if (scope_info->takesClosure()) {
            passed_closure = AI;
            ++AI;
            emitter.setType(passed_closure, RefType::BORROWED);
        } else {
            passed_closure = getNullPtr(g.llvm_closure_type_ptr);
            emitter.setType(passed_closure, RefType::BORROWED);
        }

        if (irstate->getSourceInfo()->is_generator) {
            generator = AI;
            emitter.setType(generator, RefType::BORROWED);
            ++AI;
        }

        if (!irstate->getSourceInfo()->scoping->areGlobalsFromModule()) {
            globals = AI;
            emitter.setType(globals, RefType::BORROWED);
            ++AI;
        } else {
            BoxedModule* parent_module = irstate->getSourceInfo()->parent_module;
            globals = embedRelocatablePtr(parent_module, g.llvm_value_type_ptr, "cParentModule");
            emitter.setType(globals, RefType::BORROWED);
        }

        irstate->setupFrameInfoVar(passed_closure, globals);

        if (scope_info->takesClosure()) {
            symbol_table[internString(PASSED_CLOSURE_NAME)]
                = new ConcreteCompilerVariable(getPassedClosureType(), passed_closure);
        }

        if (scope_info->createsClosure()) {
            llvm::Value* new_closure = emitter.getBuilder()->CreateCall2(
                g.funcs.createClosure, passed_closure, getConstantInt(scope_info->getClosureSize(), g.i64));
            emitter.setType(new_closure, RefType::OWNED);
            symbol_table[internString(CREATED_CLOSURE_NAME)]
                = new ConcreteCompilerVariable(getCreatedClosureType(), new_closure);
        }

        if (irstate->getSourceInfo()->is_generator)
            symbol_table[internString(PASSED_GENERATOR_NAME)] = new ConcreteCompilerVariable(GENERATOR, generator);

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
        for (; i < param_names.args.size(); i++) {
            loadArgument(internString(param_names.args[i]), arg_types[i], python_parameters[i],
                         UnwindInfo::cantUnwind());
        }

        if (param_names.vararg.size()) {
            loadArgument(internString(param_names.vararg), arg_types[i], python_parameters[i],
                         UnwindInfo::cantUnwind());
            i++;
        }

        if (param_names.kwarg.size()) {
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

            loadArgument(internString(param_names.kwarg), arg_types[i], phi, UnwindInfo::cantUnwind());
            i++;
        }

        assert(i == arg_types.size());
    }

    void run(const CFGBlock* block) override {
        if (VERBOSITY("irgenerator") >= 2) { // print starting symbol table
            printf("  %d init:", block->idx);
            for (auto it = symbol_table.begin(); it != symbol_table.end(); ++it)
                printf(" %s", it->first.c_str());
            printf("\n");
        }
        for (int i = 0; i < block->body.size(); i++) {
            if (state == DEAD)
                break;
            assert(state != FINISHED);

#if ENABLE_SAMPLING_PROFILER
            auto stmt = block->body[i];
            if (!(i == 0 && stmt->type == AST_TYPE::Assign) && stmt->lineno > 0) // could be a landingpad
                doSafePoint(block->body[i]);
#endif

            doStmt(block->body[i], UnwindInfo(block->body[i], NULL));
        }
        if (VERBOSITY("irgenerator") >= 2) { // print ending symbol table
            printf("  %d fini:", block->idx);
            for (auto it = symbol_table.begin(); it != symbol_table.end(); ++it)
                printf(" %s", it->first.c_str());
            printf("\n");
        }
    }

    void doSafePoint(AST_stmt* next_statement) override {
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
    llvm::BasicBlock* getCAPIExcDest(llvm::BasicBlock* from_block, llvm::BasicBlock* final_dest, AST_stmt* current_stmt,
                                     bool is_after_deopt) override {
        llvm::BasicBlock*& capi_exc_dest = capi_exc_dests[final_dest];
        llvm::PHINode*& phi_node = capi_phis[final_dest];

        if (!capi_exc_dest) {
            auto orig_block = curblock;

            capi_exc_dest = llvm::BasicBlock::Create(g.context, "capi_exc_dest", irstate->getLLVMFunction());

            emitter.setCurrentBasicBlock(capi_exc_dest);
            assert(!phi_node);
            phi_node = emitter.getBuilder()->CreatePHI(g.llvm_aststmt_type_ptr, 0);

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

        phi_node->addIncoming(embedRelocatablePtr(current_stmt, g.llvm_aststmt_type_ptr), critedge_breaker);

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

FunctionMetadata* wrapFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body, SourceInfo* source) {
    // Different compilations of the parent scope of a functiondef should lead
    // to the same FunctionMetadata* being used:
    static std::unordered_map<AST*, FunctionMetadata*> made;

    FunctionMetadata*& md = made[node];
    if (md == NULL) {
        std::unique_ptr<SourceInfo> si(
            new SourceInfo(source->parent_module, source->scoping, source->future_flags, node, body, source->getFn()));
        if (args)
            md = new FunctionMetadata(args->args.size(), args->vararg.s().size(), args->kwarg.s().size(),
                                      std::move(si));
        else
            md = new FunctionMetadata(0, false, false, std::move(si));
    }
    return md;
}
}
