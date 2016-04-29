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

#include "codegen/compvars.h"

#include <cstdio>
#include <sstream>

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "codegen/codegen.h"
#include "codegen/gcbuilder.h"
#include "codegen/irgen.h"
#include "codegen/irgen/util.h"
#include "codegen/patchpoints.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

CompilerType* CompilerType::getPystonIterType() {
    static BoxedString* iter_str = getStaticString("__iter__");
    static BoxedString* hasnext_str = getStaticString("__hasnext__");
    if (hasattr(iter_str) == Yes) {
        CompilerType* iter_type = getattrType(iter_str, true)->callType(ArgPassSpec(0), {}, NULL);
        if (iter_type->hasattr(hasnext_str) == Yes)
            return iter_type;
        // if iter_type->hasattr(hasnext_str) == No we know this is going to be a BoxedIterWrapper
        // we could optimize this case but it looks like this is very uncommon
    }
    return UNKNOWN;
}

CompilerType::Result CompilerType::hasattr(BoxedString* attr) {
    CompilerType* type = getattrType(attr, true);
    if (type == UNKNOWN)
        return Result::Maybe;
    else if (type == UNDEF)
        return Result::No;
    return Result::Yes;
}

std::vector<CompilerType*> CompilerType::unpackTypes(int num_into) {
    assert((CompilerType*)this != UNKNOWN);

    return UNKNOWN->unpackTypes(num_into);
}

void ConcreteCompilerType::serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) {
#ifndef NDEBUG
    if (llvmType() == g.i1) {
        var->getValue()->dump();
        ASSERT(llvmType() != g.i1, "due to an llvm limitation cannot add these to stackmaps yet");
    }
#endif
    stackmap_args.push_back(var->getValue());
}

std::string ValuedCompilerType<llvm::Value*>::debugName() {
    std::string rtn;
    llvm::raw_string_ostream os(rtn);
    llvmType()->print(os);
    return rtn;
}

struct RawInstanceMethod {
    CompilerVariable* obj, *func, *im_class;

    RawInstanceMethod(CompilerVariable* obj, CompilerVariable* func, CompilerVariable* im_class)
        : obj(obj), func(func), im_class(im_class) {}
};

class InstanceMethodType : public ValuedCompilerType<RawInstanceMethod*> {
private:
    static std::unordered_map<std::pair<CompilerType*, CompilerType*>, InstanceMethodType*> made;

    CompilerType* obj_type, *function_type;
    InstanceMethodType(CompilerType* obj_type, CompilerType* function_type)
        : obj_type(obj_type), function_type(function_type) {}

    void checkVar(VAR* var) {
#ifndef NDEBUG
        RawInstanceMethod* val = var->getValue();
        assert(val->obj->getType() == obj_type);
        assert(val->func->getType() == function_type);
#endif
    }

public:
    void assertMatches(RawInstanceMethod* im) override {
        assert(obj_type == im->obj->getType() && function_type == im->func->getType());
    }

    static InstanceMethodType* get(CompilerType* obj_type, CompilerType* function_type) {
        InstanceMethodType*& rtn = made[std::make_pair(obj_type, function_type)];
        if (rtn == NULL)
            rtn = new InstanceMethodType(obj_type, function_type);
        return rtn;
    }

    static CompilerVariable* makeIM(CompilerVariable* obj, CompilerVariable* func, CompilerVariable* im_class) {
        CompilerVariable* rtn = new ValuedCompilerVariable<RawInstanceMethod*>(
            InstanceMethodType::get(obj->getType(), func->getType()), new RawInstanceMethod(obj, func, im_class));
        return rtn;
    }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<llvm::StringRef>* keyword_names) override {
        std::vector<CompilerType*> new_args(arg_types);
        new_args.insert(new_args.begin(), obj_type);

        ArgPassSpec new_argspec(argspec.num_args + 1u, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
        return function_type->callType(new_argspec, new_args, keyword_names);
    }

    std::string debugName() override {
        return "instanceMethod(" + obj_type->debugName() + " ; " + function_type->debugName() + ")";
    }

    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ValuedCompilerVariable<RawInstanceMethod*>* var,
                           ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                           const std::vector<BoxedString*>* keyword_names) override {
        std::vector<CompilerVariable*> new_args;
        new_args.push_back(var->getValue()->obj);
        new_args.insert(new_args.end(), args.begin(), args.end());

        ArgPassSpec new_argspec(argspec.num_args + 1u, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
        return var->getValue()->func->call(emitter, info, new_argspec, new_args, keyword_names);
    }

    bool canConvertTo(CompilerType* other_type) override { return other_type == UNKNOWN; }
    ConcreteCompilerType* getConcreteType() override { return typeFromClass(instancemethod_cls); }
    ConcreteCompilerType* getBoxType() override { return getConcreteType(); }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        checkVar(var);
        assert(other_type == UNKNOWN || other_type == typeFromClass(instancemethod_cls));

        RawInstanceMethod* im = var->getValue();
        assert(im->obj);
        assert(im->func);
        ConcreteCompilerVariable* obj = im->obj->makeConverted(emitter, UNKNOWN);
        ConcreteCompilerVariable* func = im->func->makeConverted(emitter, UNKNOWN);
        ConcreteCompilerVariable* im_class = im->im_class->makeConverted(emitter, UNKNOWN);

        llvm::Value* boxed = emitter.getBuilder()->CreateCall3(g.funcs.boxInstanceMethod, obj->getValue(),
                                                               func->getValue(), im_class->getValue());
        emitter.setType(boxed, RefType::OWNED);

        return new ConcreteCompilerVariable(other_type, boxed);
    }
    CompilerVariable* dup(VAR* var, DupCache& cache) override {
        checkVar(var);

        CompilerVariable* rtn = cache[var];
        if (rtn == NULL) {
            RawInstanceMethod* im = var->getValue();
            RawInstanceMethod* new_im
                = new RawInstanceMethod(im->obj->dup(cache), im->func->dup(cache), im->im_class->dup(cache));
            rtn = new VAR(this, new_im);
        }
        return rtn;
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override {
        var->getValue()->obj->serializeToFrame(stackmap_args);
        var->getValue()->func->serializeToFrame(stackmap_args);
        var->getValue()->im_class->serializeToFrame(stackmap_args);
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs() && vals.size() == 3);
        Box* obj = reinterpret_cast<Box*>(vals[0]);
        Box* func = reinterpret_cast<Box*>(vals[1]);
        Box* im_class = reinterpret_cast<Box*>(vals[2]);
        return boxInstanceMethod(obj, func, im_class);
    }

    int numFrameArgs() override { return obj_type->numFrameArgs() + function_type->numFrameArgs() + 1 /* im_class */; }
};
std::unordered_map<std::pair<CompilerType*, CompilerType*>, InstanceMethodType*> InstanceMethodType::made;

ConcreteCompilerVariable* ConcreteCompilerType::makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                              ConcreteCompilerType* other_type) {
    if (other_type == this) {
        return var;
    }
    printf("makeConverted not defined for %s\n", debugName().c_str());
    abort();
}
CompilerVariable* ConcreteCompilerType::dup(ConcreteCompilerVariable* v, DupCache& cache) {
    auto& rtn = cache[v];
    if (rtn == NULL) {
        rtn = new ConcreteCompilerVariable(this, v->getValue());
    }
    return rtn;
}

class UnknownType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_value_type_ptr; }

    std::string debugName() override { return "AnyBox"; }

    bool isFitBy(BoxedClass* c) override { return true; }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                              bool cls_only) override;
    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, ArgPassSpec argspec,
                           const std::vector<CompilerVariable*>& args,
                           const std::vector<BoxedString*>* keyword_names) override;
    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                               CallattrFlags flags, const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override;
    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override;
    ConcreteCompilerVariable* unaryop(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                      AST_TYPE::AST_TYPE op_type) override;
    ConcreteCompilerVariable* hasnext(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override;

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                 CompilerVariable* v) override {
        llvm::Constant* ptr = embedRelocatablePtr(attr, g.llvm_boxedstring_type_ptr);
        emitter.setType(ptr, RefType::BORROWED);
        ConcreteCompilerVariable* converted = v->makeConverted(emitter, UNKNOWN);
        // g.funcs.setattr->dump();
        // var->getValue()->dump(); llvm::errs() << '\n';
        // ptr->dump(); llvm::errs() << '\n';
        // converted->getValue()->dump(); llvm::errs() << '\n';
        bool do_patchpoint = ENABLE_ICSETATTRS;
        llvm::Instruction* inst;
        if (do_patchpoint) {
            ICSetupInfo* pp = createSetattrIC(info.getTypeRecorder(), info.getBJitICInfo());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(ptr);
            llvm_args.push_back(converted->getValue());

            inst = emitter.createIC(pp, (void*)pyston::setattr, llvm_args, info.unw_info);
        } else {
            inst = emitter.createCall3(info.unw_info, g.funcs.setattr, var->getValue(), ptr, converted->getValue());
        }
        emitter.refConsumed(converted->getValue(), inst);
    }

    void delattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr) override {
        llvm::Constant* ptr = embedRelocatablePtr(attr, g.llvm_boxedstring_type_ptr);
        emitter.setType(ptr, RefType::BORROWED);

        // TODO
        // bool do_patchpoint = ENABLE_ICDELATTRS;
        bool do_patchpoint = false;

        if (do_patchpoint) {
            ICSetupInfo* pp = createDelattrIC(info.getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(ptr);

            emitter.createIC(pp, (void*)pyston::delattr, llvm_args, info.unw_info);
        } else {
            emitter.createCall2(info.unw_info, g.funcs.delattr, var->getValue(), ptr);
        }
    }

    llvm::Value* makeClassCheck(IREmitter& emitter, ConcreteCompilerVariable* var, BoxedClass* cls) override {
        assert(var->getValue()->getType() == g.llvm_value_type_ptr);

        static_assert(offsetof(Box, cls) % sizeof(void*) == 0, "");
        llvm::Value* cls_ptr
            = emitter.getBuilder()->CreateConstInBoundsGEP2_32(var->getValue(), 0, offsetof(Box, cls) / sizeof(void*));

        llvm::Value* cls_value = emitter.getBuilder()->CreateLoad(cls_ptr);
        emitter.setType(cls_value, RefType::BORROWED);
        assert(cls_value->getType() == g.llvm_class_type_ptr);
        llvm::Value* rtn = emitter.getBuilder()->CreateICmpEQ(
            cls_value, emitter.setType(embedRelocatablePtr(cls, g.llvm_class_type_ptr), RefType::BORROWED));
        return rtn;
    }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override { return UNKNOWN; }
    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<llvm::StringRef>* keyword_names) override {
        return UNKNOWN;
    }
    BoxedClass* guaranteedClass() override { return NULL; }
    ConcreteCompilerType* getBoxType() override { return this; }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == this) {
            return var;
        }
        fprintf(stderr, "Can't convert unknown to %s...\n", other_type->debugName().c_str());
        abort();
    }

    CompilerVariable* len(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        bool do_patchpoint = ENABLE_ICGENERICS;
        llvm::Value* rtn;
        if (do_patchpoint) {
            ICSetupInfo* pp = createGenericIC(info.getTypeRecorder(), true, 256);

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());

            rtn = emitter.createIC(pp, (void*)pyston::unboxedLen, llvm_args, info.unw_info);
        } else {
            rtn = emitter.createCall(info.unw_info, g.funcs.unboxedLen, var->getValue());
        }
        assert(rtn->getType() == g.i64);
        return makeInt(rtn);
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              CompilerVariable* slice) override {
        if (slice->getType() == UNBOXED_SLICE) {
            UnboxedSlice slice_val = extractSlice(slice);

            if (slice_val.step == NULL) {
                static BoxedString* attr = getStaticString("__getitem__");
                CompilerType* return_type
                    = var->getType()->getattrType(attr, true)->callType(ArgPassSpec(1), { SLICE }, NULL);
                assert(return_type->getConcreteType() == return_type);

                if (return_type != UNDEF) {
                    llvm::Value* cstart, *cstop;
                    cstart = slice_val.start ? slice_val.start->makeConverted(emitter, UNKNOWN)->getValue()
                                             : emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED);
                    cstop = slice_val.stop ? slice_val.stop->makeConverted(emitter, UNKNOWN)->getValue()
                                           : emitter.setType(getNullPtr(g.llvm_value_type_ptr), RefType::BORROWED);

                    llvm::Value* r = emitter.createCall3(info.unw_info, g.funcs.apply_slice, var->getValue(), cstart,
                                                         cstop, CAPI, getNullPtr(g.llvm_value_type_ptr));
                    emitter.setType(r, RefType::OWNED);
                    emitter.setNullable(r, true);

                    return new ConcreteCompilerVariable(static_cast<ConcreteCompilerType*>(return_type), r);
                } else {
                    // TODO: we could directly emit an exception if we know getitem is undefined but for now let it just
                    // call the normal getitem which will raise the exception
                }
            }
        }

        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        ExceptionStyle target_exception_style = info.preferredExceptionStyle();

        bool do_patchpoint = ENABLE_ICGETITEMS;
        llvm::Value* rtn;
        if (do_patchpoint) {
            ICSetupInfo* pp = createGetitemIC(info.getTypeRecorder(), info.getBJitICInfo());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(converted_slice->getValue());

            llvm::Value* uncasted
                = emitter.createIC(pp, (void*)(target_exception_style == CAPI ? pyston::getitem_capi : pyston::getitem),
                                   llvm_args, info.unw_info, target_exception_style, getNullPtr(g.llvm_value_type_ptr));
            rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            emitter.setType(rtn, RefType::OWNED);
        } else {
            rtn = emitter.createCall2(
                info.unw_info, target_exception_style == CAPI ? g.funcs.getitem_capi : g.funcs.getitem, var->getValue(),
                converted_slice->getValue(), target_exception_style, getNullPtr(g.llvm_value_type_ptr));
            emitter.setType(rtn, RefType::OWNED);
        }

        if (target_exception_style == CAPI)
            emitter.setNullable(rtn, true);

        return new ConcreteCompilerVariable(UNKNOWN, rtn);
    }

    CompilerVariable* getPystonIter(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        static BoxedString* iter_box = getStaticString("__iter__");

        CallattrFlags flags = {.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
        CompilerVariable* iter_call = var->callattr(emitter, info, iter_box, flags, {}, 0);
        ConcreteCompilerVariable* converted_iter_call = iter_call->makeConverted(emitter, iter_call->getBoxType());

        // If the type analysis could determine the iter type is a valid pyston iter (has 'hasnext') we are finished.
        CompilerType* iter_type = var->getType()->getPystonIterType();
        if (iter_type != UNKNOWN) {
            return converted_iter_call;
        }

        // We don't know the type so we have to check at runtime if __iter__ is implemented
        llvm::Value* null_value = getNullPtr(g.llvm_value_type_ptr);
        emitter.setType(null_value, RefType::BORROWED);
        llvm::Value* cmp = emitter.getBuilder()->CreateICmpNE(converted_iter_call->getValue(), null_value);

        llvm::BasicBlock* bb_has_iter = emitter.createBasicBlock("has_iter");
        bb_has_iter->moveAfter(emitter.currentBasicBlock());
        llvm::BasicBlock* bb_no_iter = emitter.createBasicBlock("no_iter");
        bb_no_iter->moveAfter(bb_has_iter);
        llvm::BasicBlock* bb_join = emitter.createBasicBlock("join_after_getiter");
        emitter.getBuilder()->CreateCondBr(cmp, bb_has_iter, bb_no_iter);

        // var has __iter__()
        emitter.setCurrentBasicBlock(bb_has_iter);
        ICSetupInfo* pp = createGenericIC(info.getTypeRecorder(), true, 128);
        llvm::Value* uncasted = emitter.createIC(pp, (void*)pyston::createBoxedIterWrapperIfNeeded,
                                                 { converted_iter_call->getValue() }, info.unw_info);
        llvm::Value* value_has_iter = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
        emitter.setType(value_has_iter, RefType::OWNED);
        llvm::BasicBlock* value_has_iter_bb = emitter.currentBasicBlock();
        auto has_iter_terminator = emitter.getBuilder()->CreateBr(bb_join);

        // var has no __iter__()
        // TODO: we could create a patchpoint if this turns out to be hot
        emitter.setCurrentBasicBlock(bb_no_iter);
        llvm::Value* value_no_iter = emitter.createCall(info.unw_info, g.funcs.getiterHelper, var->getValue());
        emitter.setType(value_no_iter, RefType::OWNED);
        llvm::BasicBlock* value_no_iter_bb = emitter.currentBasicBlock();
        auto no_iter_terminator = emitter.getBuilder()->CreateBr(bb_join);

        // join
        emitter.setCurrentBasicBlock(bb_join);
        auto phi = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, 2, "iter");
        phi->addIncoming(value_has_iter, value_has_iter_bb);
        phi->addIncoming(value_no_iter, value_no_iter_bb);

        emitter.refConsumed(value_has_iter, has_iter_terminator);
        emitter.refConsumed(value_no_iter, no_iter_terminator);
        emitter.setType(phi, RefType::OWNED);

        return new ConcreteCompilerVariable(UNKNOWN, phi);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted_rhs = rhs->makeConverted(emitter, rhs->getBoxType());

        llvm::Value* rtn;
        bool do_patchpoint = ENABLE_ICBINEXPS;

        llvm::Value* rt_func;
        void* rt_func_addr;
        if (exp_type == BinOp) {
            rt_func = g.funcs.binop;
            rt_func_addr = (void*)binop;
        } else if (exp_type == AugBinOp) {
            rt_func = g.funcs.augbinop;
            rt_func_addr = (void*)augbinop;
        } else {
            rt_func = g.funcs.compare;
            rt_func_addr = (void*)compare;
        }

        if (do_patchpoint) {
            ICSetupInfo* pp = createBinexpIC(info.getTypeRecorder(), info.getBJitICInfo());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(converted_rhs->getValue());
            llvm_args.push_back(getConstantInt(op_type, g.i32));

            llvm::Value* uncasted = emitter.createIC(pp, rt_func_addr, llvm_args, info.unw_info);
            rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
        } else {
            rtn = emitter.createCall3(info.unw_info, rt_func, var->getValue(), converted_rhs->getValue(),
                                      getConstantInt(op_type, g.i32));
        }
        emitter.setType(rtn, RefType::OWNED);

        if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn || op_type == AST_TYPE::Is
            || op_type == AST_TYPE::IsNot) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, rtn);
            return boolFromI1(emitter, unboxed);
        }

        return new ConcreteCompilerVariable(UNKNOWN, rtn);
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        return lhs->binexp(emitter, info, var, AST_TYPE::In, Compare);
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        return incref(reinterpret_cast<Box*>(vals[0]));
    }

    std::vector<CompilerVariable*> unpack(IREmitter& emitter, const OpInfo& info, VAR* var, int num_into) override {
        llvm::Value* scratch
            = emitter.getBuilder()->CreateBitCast(emitter.getScratch(sizeof(Box*)), g.llvm_value_type_ptr_ptr);
        llvm::Value* unpacked = emitter.createCall3(info.unw_info, g.funcs.unpackIntoArray, var->getValue(),
                                                    getConstantInt(num_into, g.i64), scratch);
        assert(unpacked->getType() == g.llvm_value_type_ptr->getPointerTo());

        std::vector<CompilerVariable*> rtn;
        for (int i = 0; i < num_into; i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(unpacked, i);
            llvm::Value* val = emitter.getBuilder()->CreateLoad(ptr);
            assert(val->getType() == g.llvm_value_type_ptr);
            emitter.setType(val, RefType::OWNED);

            rtn.push_back(new ConcreteCompilerVariable(UNKNOWN, val));
        }

        emitter.setType(emitter.getBuilder()->CreateLoad(scratch), RefType::OWNED);
        return rtn;
    }

    std::vector<CompilerType*> unpackTypes(int num_into) override {
        return std::vector<CompilerType*>(num_into, UNKNOWN);
    }
};

ConcreteCompilerType* UNKNOWN = new UnknownType();

CompilerVariable* UnknownType::getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                       BoxedString* attr, bool cls_only) {
    llvm::Constant* ptr = embedRelocatablePtr(attr, g.llvm_boxedstring_type_ptr);
    emitter.setType(ptr, RefType::BORROWED);

    llvm::Value* rtn_val = NULL;

    ExceptionStyle target_exception_style = cls_only ? CXX : info.preferredExceptionStyle();

    llvm::Value* llvm_func;
    void* raw_func;
    if (cls_only) {
        assert(target_exception_style == CXX);
        llvm_func = g.funcs.getclsattr;
        raw_func = (void*)pyston::getclsattr;
    } else {
        if (target_exception_style == CXX) {
            llvm_func = g.funcs.getattr;
            raw_func = (void*)pyston::getattr;
        } else {
            llvm_func = g.funcs.getattr_capi;
            raw_func = (void*)pyston::getattr_capi;
        }
    }

    bool do_patchpoint = ENABLE_ICGETATTRS;
    if (do_patchpoint) {
        ICSetupInfo* pp = createGetattrIC(info.getTypeRecorder(), info.getBJitICInfo());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());
        llvm_args.push_back(ptr);

        llvm::Value* uncasted = emitter.createIC(pp, raw_func, llvm_args, info.unw_info, target_exception_style,
                                                 getNullPtr(g.llvm_value_type_ptr));
        rtn_val = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        rtn_val = emitter.createCall2(info.unw_info, llvm_func, var->getValue(), ptr, target_exception_style,
                                      getNullPtr(g.llvm_value_type_ptr));
    }

    emitter.setType(rtn_val, RefType::OWNED);
    if (target_exception_style == CAPI)
        emitter.setNullable(rtn_val, true);

    return new ConcreteCompilerVariable(UNKNOWN, rtn_val);
}

static ConcreteCompilerVariable*
_call(IREmitter& emitter, const OpInfo& info, llvm::Value* func, ExceptionStyle target_exception_style, void* func_addr,
      const std::vector<llvm::Value*>& other_args, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
      const std::vector<BoxedString*>* keyword_names, ConcreteCompilerType* rtn_type, bool nullable_return = false) {
    bool pass_keyword_names = (keyword_names != nullptr);
    assert(pass_keyword_names == (argspec.num_keywords > 0));

    std::vector<BoxedClass*> guaranteed_classes;
    std::vector<ConcreteCompilerVariable*> converted_args;
    for (int i = 0; i < args.size(); i++) {
        assert(args[i]);
        converted_args.push_back(args[i]->makeConverted(emitter, args[i]->getBoxType()));
        guaranteed_classes.push_back(converted_args.back()->guaranteedClass());
    }

    std::vector<llvm::Value*> llvm_args;
    llvm_args.insert(llvm_args.end(), other_args.begin(), other_args.end());

    if (args.size() >= 1) {
        llvm_args.push_back(converted_args[0]->getValue());
    } else if (pass_keyword_names) {
        llvm_args.push_back(getNullPtr(g.llvm_value_type_ptr));
    }

    if (args.size() >= 2) {
        llvm_args.push_back(converted_args[1]->getValue());
    } else if (pass_keyword_names) {
        llvm_args.push_back(getNullPtr(g.llvm_value_type_ptr));
    }

    if (args.size() >= 3) {
        llvm_args.push_back(converted_args[2]->getValue());
    } else if (pass_keyword_names) {
        llvm_args.push_back(getNullPtr(g.llvm_value_type_ptr));
    }

    llvm::SmallVector<llvm::Value*, 4> array_passed_args;

    if (args.size() >= 4) {
        llvm::Value* arg_array;

        llvm::Value* n_varargs = getConstantInt(args.size() - 3, g.i64);

        // Don't use the IRBuilder since we want to specifically put this in the entry block so it only gets called
        // once.
        // TODO we could take this further and use the same alloca for all function calls?
        llvm::Instruction* insertion_point = emitter.currentFunction()->func->getEntryBlock().getFirstInsertionPt();
        arg_array = new llvm::AllocaInst(g.llvm_value_type_ptr, n_varargs, "arg_scratch", insertion_point);

        for (int i = 3; i < args.size(); i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, i - 3);
            emitter.getBuilder()->CreateStore(converted_args[i]->getValue(), ptr);
            array_passed_args.push_back(converted_args[i]->getValue());
        }
        llvm_args.push_back(arg_array);

        if (pass_keyword_names)
            llvm_args.push_back(embedRelocatablePtr(keyword_names, g.vector_ptr));
    } else if (pass_keyword_names) {
        llvm_args.push_back(getNullPtr(g.llvm_value_type_ptr->getPointerTo()));
        llvm_args.push_back(embedRelocatablePtr(keyword_names, g.vector_ptr));
    }

    // f->dump();
    // for (int i = 0; i < llvm_args.size(); i++) {
    // llvm_args[i]->dump();
    // llvm::errs() << '\n';
    //}

    llvm::Value* rtn;
    llvm::Instruction* inst;

    // func->dump();
    // for (auto a : llvm_args)
    // a->dump();

    bool do_patchpoint = ENABLE_ICCALLSITES && (func_addr == runtimeCall || func_addr == runtimeCallCapi
                                                || func_addr == pyston::callattr || func_addr == callattrCapi);
    if (do_patchpoint) {
        assert(func_addr);

        ICSetupInfo* pp = createCallsiteIC(info.getTypeRecorder(), args.size(), info.getBJitICInfo());

        llvm::Instruction* uncasted = emitter.createIC(pp, func_addr, llvm_args, info.unw_info, target_exception_style,
                                                       getNullPtr(g.llvm_value_type_ptr));
        inst = uncasted;

        assert(llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(func->getType())->getElementType())
                   ->getReturnType() == g.llvm_value_type_ptr);
        rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        // printf("\n");
        // func->dump();
        // printf("\n");
        // for (auto a : llvm_args) {
        // a->dump();
        //}
        // printf("%ld %ld\n", llvm_args.size(), args.size());
        // printf("\n");
        inst = emitter.createCall(info.unw_info, func, llvm_args, target_exception_style,
                                  getNullPtr(g.llvm_value_type_ptr));
        rtn = inst;
    }

    if (rtn_type->getBoxType() == rtn_type) {
        emitter.setType(rtn, RefType::OWNED);
        if (nullable_return || target_exception_style == CAPI)
            emitter.setNullable(rtn, true);
    }
    assert(rtn->getType() == rtn_type->llvmType());

    for (auto v : array_passed_args)
        emitter.refUsed(v, inst);

    return new ConcreteCompilerVariable(rtn_type, rtn);
}

CompilerVariable* UnknownType::call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                    ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                    const std::vector<BoxedString*>* keyword_names) {
    bool pass_keywords = (argspec.num_keywords != 0);
    int npassed_args = argspec.totalPassed();

    ExceptionStyle exception_style = info.preferredExceptionStyle();

    llvm::Value* func;
    if (pass_keywords)
        func = g.funcs.runtimeCall.get(exception_style);
    else if (npassed_args == 0)
        func = g.funcs.runtimeCall0.get(exception_style);
    else if (npassed_args == 1)
        func = g.funcs.runtimeCall1.get(exception_style);
    else if (npassed_args == 2)
        func = g.funcs.runtimeCall2.get(exception_style);
    else if (npassed_args == 3)
        func = g.funcs.runtimeCall3.get(exception_style);
    else
        func = g.funcs.runtimeCallN.get(exception_style);

    void* func_ptr = (exception_style == ExceptionStyle::CXX) ? (void*)runtimeCall : (void*)runtimeCallCapi;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());

    llvm::Value* llvm_argspec = llvm::ConstantInt::get(g.i32, argspec.asInt(), false);
    other_args.push_back(llvm_argspec);
    return _call(emitter, info, func, exception_style, func_ptr, other_args, argspec, args, keyword_names, UNKNOWN);
}

CompilerVariable* UnknownType::callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                        BoxedString* attr, CallattrFlags flags,
                                        const std::vector<CompilerVariable*>& args,
                                        const std::vector<BoxedString*>* keyword_names) {
    bool pass_keywords = (flags.argspec.num_keywords != 0);
    int npassed_args = flags.argspec.totalPassed();

    ExceptionStyle exception_style = flags.null_on_nonexistent ? CXX : info.preferredExceptionStyle();

    if (exception_style == CAPI)
        assert(!flags.null_on_nonexistent); // Will conflict with CAPI's null-on-exception

    llvm::Value* func;
    if (pass_keywords)
        func = g.funcs.callattr.get(exception_style);
    else if (npassed_args == 0)
        func = g.funcs.callattr0.get(exception_style);
    else if (npassed_args == 1)
        func = g.funcs.callattr1.get(exception_style);
    else if (npassed_args == 2)
        func = g.funcs.callattr2.get(exception_style);
    else if (npassed_args == 3)
        func = g.funcs.callattr3.get(exception_style);
    else
        func = g.funcs.callattrN.get(exception_style);

    void* func_ptr = (exception_style == ExceptionStyle::CXX) ? (void*)pyston::callattr : (void*)callattrCapi;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());
    other_args.push_back(emitter.setType(embedRelocatablePtr(attr, g.llvm_boxedstring_type_ptr), RefType::BORROWED));
    other_args.push_back(getConstantInt(flags.asInt(), g.i64));
    return _call(emitter, info, func, exception_style, func_ptr, other_args, flags.argspec, args, keyword_names,
                 UNKNOWN, /* nullable_return = */ flags.null_on_nonexistent);
}

ConcreteCompilerVariable* UnknownType::nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
    bool do_patchpoint = ENABLE_ICNONZEROS;
    llvm::Value* rtn_val;
    if (do_patchpoint) {
        ICSetupInfo* pp = createNonzeroIC(info.getTypeRecorder());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());

        llvm::Value* uncasted = emitter.createIC(pp, (void*)pyston::nonzero, llvm_args, info.unw_info);
        rtn_val = emitter.getBuilder()->CreateTrunc(uncasted, g.i1);
    } else {
        rtn_val = emitter.createCall(info.unw_info, g.funcs.nonzero, var->getValue());
    }
    return boolFromI1(emitter, rtn_val);
}

ConcreteCompilerVariable* UnknownType::unaryop(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                               AST_TYPE::AST_TYPE op_type) {
    ConcreteCompilerVariable* converted = var->makeConverted(emitter, var->getBoxType());

    llvm::Value* rtn = NULL;
    bool do_patchpoint = ENABLE_ICGENERICS;
    if (do_patchpoint) {
        ICSetupInfo* pp = createGenericIC(info.getTypeRecorder(), true, 256);

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(converted->getValue());
        llvm_args.push_back(getConstantInt(op_type, g.i32));

        llvm::Value* uncasted = emitter.createIC(pp, (void*)pyston::unaryop, llvm_args, info.unw_info);
        rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        rtn = emitter.createCall2(info.unw_info, g.funcs.unaryop, converted->getValue(),
                                  getConstantInt(op_type, g.i32));
    }
    emitter.setType(rtn, RefType::OWNED);

    return new ConcreteCompilerVariable(UNKNOWN, rtn);
}

ConcreteCompilerVariable* UnknownType::hasnext(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
    bool do_patchpoint = ENABLE_ICS;
    do_patchpoint = false; // we are currently using runtime ics for this
    llvm::Value* rtn_val;
    if (do_patchpoint) {
        ICSetupInfo* pp = createHasnextIC(info.getTypeRecorder());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());

        llvm::Value* uncasted = emitter.createIC(pp, (void*)pyston::hasnext, llvm_args, info.unw_info);
        rtn_val = emitter.getBuilder()->CreateTrunc(uncasted, g.i1);
    } else {
        rtn_val = emitter.createCall(info.unw_info, g.funcs.hasnext, var->getValue());
    }
    return boolFromI1(emitter, rtn_val);
}

CompilerVariable* makeFunction(IREmitter& emitter, FunctionMetadata* f, CompilerVariable* closure, llvm::Value* globals,
                               const std::vector<ConcreteCompilerVariable*>& defaults) {
    // Unlike the FunctionMetadata*, which can be shared between recompilations, the Box* around it
    // should be created anew every time the functiondef is encountered

    llvm::Value* closure_v;
    ConcreteCompilerVariable* convertedClosure = NULL;
    if (closure) {
        convertedClosure = closure->makeConverted(emitter, closure->getConcreteType());
        closure_v = convertedClosure->getValue();
    } else {
        closure_v = getNullPtr(g.llvm_closure_type_ptr);
        emitter.setType(closure_v, RefType::BORROWED);
    }

    llvm::SmallVector<llvm::Value*, 4> array_passed_args;

    llvm::Value* scratch;
    if (defaults.size()) {
        scratch = emitter.getScratch(defaults.size() * sizeof(Box*));
        scratch = emitter.getBuilder()->CreateBitCast(scratch, g.llvm_value_type_ptr_ptr);
        int i = 0;
        for (auto d : defaults) {
            llvm::Value* v = d->getValue();
            array_passed_args.push_back(v);
            llvm::Value* p = emitter.getBuilder()->CreateConstGEP1_32(scratch, i);
            emitter.getBuilder()->CreateStore(v, p);
            i++;
        }
    } else {
        scratch = getNullPtr(g.llvm_value_type_ptr_ptr);
    }

    assert(globals);

    // We know this function call can't throw, so it's safe to use emitter.getBuilder()->CreateCall() rather than
    // emitter.createCall().
    llvm::Instruction* boxed = emitter.getBuilder()->CreateCall(
        g.funcs.createFunctionFromMetadata,
        std::vector<llvm::Value*>{ embedRelocatablePtr(f, g.llvm_functionmetadata_type_ptr), closure_v, globals,
                                   scratch, getConstantInt(defaults.size(), g.i64) });
    emitter.setType(boxed, RefType::OWNED);

    // The refcounter needs to know that this call "uses" the arguments that got passed via scratch.
    for (auto v : array_passed_args) {
        emitter.refUsed(v, boxed);
    }

    return new ConcreteCompilerVariable(typeFromClass(function_cls), boxed);
}

class AbstractFunctionType : public CompilerType {
public:
    struct Sig {
        std::vector<ConcreteCompilerType*> arg_types;
        CompilerType* rtn_type;
        int ndefaults = 0;
        bool takes_varargs = false;
        bool takes_kwargs = false;
    };

private:
    std::vector<Sig*> sigs;
    AbstractFunctionType(const std::vector<Sig*>& sigs) : sigs(sigs) {}

public:
    std::string debugName() override { return "<AbstractFunctionType>"; }

    ConcreteCompilerType* getConcreteType() override { return UNKNOWN; }

    ConcreteCompilerType* getBoxType() override { return UNKNOWN; }

    bool canConvertTo(CompilerType* other_type) override { return other_type == UNKNOWN; }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override { return UNDEF; }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<llvm::StringRef>* keyword_names) override {
        RELEASE_ASSERT(!argspec.has_starargs, "");
        RELEASE_ASSERT(!argspec.has_kwargs, "");
        RELEASE_ASSERT(argspec.num_keywords == 0, "");
        RELEASE_ASSERT(!keyword_names || keyword_names->empty() == 0, "");

        for (int i = 0; i < sigs.size(); i++) {
            Sig* sig = sigs[i];
            int num_normal_args = sig->arg_types.size() - ((sig->takes_varargs ? 1 : 0) + (sig->takes_kwargs ? 1 : 0));
            if (arg_types.size() < num_normal_args - sig->ndefaults)
                continue;
            if (!sig->takes_varargs && arg_types.size() > sig->arg_types.size())
                continue;

            bool works = true;
            for (int j = 0; j < arg_types.size(); j++) {
                if (j == num_normal_args)
                    break;
                if (!arg_types[j]->canConvertTo(sig->arg_types[j])) {
                    works = false;
                    break;
                }
            }

            if (!works)
                continue;

            return sig->rtn_type;
        }
        return UNDEF;
    }

    BoxedClass* guaranteedClass() override { return NULL; }

    static CompilerType* fromRT(BoxedFunction* rtfunc, bool stripfirst) {
        std::vector<Sig*> sigs;
        FunctionMetadata* md = rtfunc->md;

        assert(!rtfunc->can_change_defaults);

        for (int i = 0; i < md->versions.size(); i++) {
            CompiledFunction* cf = md->versions[i];

            FunctionSpecialization* fspec = cf->spec;

            Sig* type_sig = new Sig();
            auto paramspec = rtfunc->getParamspec();
            type_sig->rtn_type = fspec->rtn_type->getUsableType();
            type_sig->ndefaults = paramspec.num_defaults;
            type_sig->takes_varargs = paramspec.takes_varargs;
            type_sig->takes_kwargs = paramspec.takes_kwargs;

            if (stripfirst) {
                assert(fspec->arg_types.size() >= 1);
                type_sig->arg_types.insert(type_sig->arg_types.end(), fspec->arg_types.begin() + 1,
                                           fspec->arg_types.end());
            } else {
                type_sig->arg_types.insert(type_sig->arg_types.end(), fspec->arg_types.begin(), fspec->arg_types.end());
            }
            sigs.push_back(type_sig);
        }
        return get(sigs);
    }

    static CompilerType* get(const std::vector<Sig*>& sigs) { return new AbstractFunctionType(sigs); }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());
        abort();
    }

    int numFrameArgs() override { abort(); }
};

template <typename T> struct UnboxedVal {
    T val;
    ConcreteCompilerVariable* boxed;

    UnboxedVal(T val, ConcreteCompilerVariable* boxed) : val(std::move(val)), boxed(boxed) {}
};

template <typename T, typename D> class UnboxedType : public ValuedCompilerType<std::shared_ptr<UnboxedVal<T>>> {
public:
    // Subclasses need to implement:
    //   _makeConverted
    //   _dup
    //   _numFrameArgs
    //   _serializeToFrame
    //   _deserializeFromFrame
    typedef UnboxedVal<T> Unboxed;
    typedef typename ValuedCompilerType<std::shared_ptr<UnboxedVal<T>>>::VAR VAR;

    void assertMatches(std::shared_ptr<Unboxed> val) override final {
        static_cast<D*>(this)->_assertMatches(val->val);
        assert(!val->boxed || val->boxed->getType() == static_cast<D*>(this)->getBoxType());
    }

    CompilerVariable* dup(VAR* var, DupCache& cache) override final {
        CompilerVariable*& rtn = cache[var];

        if (rtn == NULL) {
            auto orig_v = var->getValue();

            T val_duped = static_cast<D*>(this)->_dup(orig_v->val, cache);

            CompilerVariable* box_duped = orig_v->boxed ? orig_v->boxed->dup(cache) : NULL;
            assert(!box_duped || box_duped->getType() == box_duped->getType()->getBoxType());

            auto val
                = std::make_shared<Unboxed>(std::move(val_duped), static_cast<ConcreteCompilerVariable*>(box_duped));
            rtn = new VAR(this, val);
        }
        return rtn;
    }

    ConcreteCompilerType* getConcreteType() override final { return this->getBoxType(); }

    bool canConvertTo(CompilerType* other_type) override final {
        return (other_type == this || other_type == UNKNOWN || other_type == this->getBoxType());
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var,
                                            ConcreteCompilerType* other_type) override final {
        assert(canConvertTo(other_type));

        auto val = var->getValue();
        ConcreteCompilerVariable* boxed = val->boxed;

        if (!boxed) {
            boxed = static_cast<D*>(this)->_makeConverted(emitter, val->val, this->getBoxType());
            ASSERT(boxed->getType() == this->getBoxType(), "%s %s", boxed->getType()->debugName().c_str(),
                   this->getBoxType()->debugName().c_str());

            val->boxed = boxed;
        }

        if (boxed->getType() != other_type) {
            assert(other_type == UNKNOWN);
            return boxed->makeConverted(emitter, other_type);
        }

        return boxed;
    }

    // Serialization strategy is a bit silly for now: we will emit a bool saying whether we emitted the
    // boxed or unboxed value.  There's no reason that has to be in the serialization though (it could
    // be in the metadata), and we shouldn't have to pad the smaller version to the size of the larger one.
    int numFrameArgs() override final {
        return 1 + std::max(static_cast<D*>(this)->_numFrameArgs(), this->getBoxType()->numFrameArgs());
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override final {
        auto v = var->getValue();

        int total_args = numFrameArgs();
        int needed_args = stackmap_args.size() + total_args;

        if (v->boxed) {
            stackmap_args.push_back(getConstantInt(1, g.i64));
            v->boxed->serializeToFrame(stackmap_args);
        } else {
            stackmap_args.push_back(getConstantInt(0, g.i64));
            static_cast<D*>(this)->_serializeToFrame(v->val, stackmap_args);
        }

        while (stackmap_args.size() < needed_args)
            stackmap_args.push_back(getConstantInt(0, g.i64));
    }

    Box* deserializeFromFrame(const FrameVals& vals) override final {
        assert(vals.size() == numFrameArgs());


        bool is_boxed = vals[0];

        if (is_boxed) {
            // TODO: inefficient
            FrameVals sub_vals(vals.begin() + 1, vals.begin() + 1 + this->getBoxType()->numFrameArgs());
            return this->getBoxType()->deserializeFromFrame(sub_vals);
        } else {
            FrameVals sub_vals(vals.begin() + 1, vals.begin() + 1 + static_cast<D*>(this)->_numFrameArgs());
            return static_cast<D*>(this)->_deserializeFromFrame(sub_vals);
        }
    }
};

class IntType : public UnboxedType<llvm::Value*, IntType> {
public:
    IntType() {}

    llvm::Value* _dup(llvm::Value* v, DupCache& cache) { return v; }

    void _assertMatches(llvm::Value* v) { assert(v->getType() == g.i64); }

    std::string debugName() override { return "int"; }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override {
        /*
        static std::vector<AbstractFunctionType::Sig*> sigs;
        if (sigs.size() == 0) {
            AbstractFunctionType::Sig* int_sig = new AbstractFunctionType::Sig();
            int_sig->rtn_type = INT;
            int_sig->arg_types.push_back(INT);
            sigs.push_back(int_sig);

            AbstractFunctionType::Sig* unknown_sig = new AbstractFunctionType::Sig();
            unknown_sig->rtn_type = UNKNOWN;
            unknown_sig->arg_types.push_back(UNKNOWN);
            sigs.push_back(unknown_sig);
        }

        if (*attr == "__add__" || *attr == "__sub__" || *attr == "__mod__" || *attr == "__mul__"
            || *attr == "__lshift__" || *attr == "__rshift__" || *attr == "__div__" || *attr == "__pow__"
            || *attr == "__floordiv__" || *attr == "__and__" || *attr == "__or__" || *attr == "__xor__") {
            return AbstractFunctionType::get(sigs);
        }

        if (*attr == "__iadd__" || *attr == "__isub__" || *attr == "__imod__" || *attr == "__imul__"
            || *attr == "__ilshift__" || *attr == "__irshift__" || *attr == "__idiv__" || *attr == "__ipow__"
            || *attr == "__ifloordiv__" || *attr == "__iand__" || *attr == "__ior__" || *attr == "__ixor__") {
            return AbstractFunctionType::get(sigs);
        }
        */

        static std::vector<AbstractFunctionType::Sig*> sigs;
        if (sigs.size() == 0) {
            AbstractFunctionType::Sig* int__float_sig = new AbstractFunctionType::Sig();
            int__float_sig->rtn_type = UNBOXED_FLOAT;
            int__float_sig->arg_types.push_back(UNBOXED_FLOAT);
            sigs.push_back(int__float_sig);

            AbstractFunctionType::Sig* unknown_sig = new AbstractFunctionType::Sig();
            unknown_sig->rtn_type = UNKNOWN;
            unknown_sig->arg_types.push_back(UNKNOWN);
            sigs.push_back(unknown_sig);
        }

        // we can handle those operations when the rhs is a float
        if (attr->s() == "__add__" || attr->s() == "__sub__" || attr->s() == "__mul__" || attr->s() == "__div__"
            || attr->s() == "__pow__" || attr->s() == "__floordiv__" || attr->s() == "__mod__"
            || attr->s() == "__pow__") {
            return AbstractFunctionType::get(sigs);
        }
        return BOXED_INT->getattrType(attr, cls_only);
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CallattrFlags flags,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, args, keyword_names);
        return rtn;
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr,
                              bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        return rtn;
    }

    void setattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CompilerVariable* v) override {
        llvm::CallSite call
            = emitter.createCall3(info.unw_info, g.funcs.raiseAttributeErrorStr, embedConstantPtr("int", g.i8_ptr),
                                  embedConstantPtr(attr->data(), g.i8_ptr), getConstantInt(attr->size(), g.i64));
        call.setDoesNotReturn();
    }

    void delattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr) override {
        llvm::CallSite call
            = emitter.createCall3(info.unw_info, g.funcs.raiseAttributeErrorStr, embedConstantPtr("int", g.i8_ptr),
                                  embedConstantPtr(attr->data(), g.i8_ptr), getConstantInt(attr->size(), g.i64));
        call.setDoesNotReturn();
    }

    ConcreteCompilerVariable* _makeConverted(IREmitter& emitter, llvm::Value* unboxed,
                                             ConcreteCompilerType* other_type) {
        assert(other_type == BOXED_INT);
        llvm::Value* boxed;
        if (llvm::ConstantInt* llvm_val = llvm::dyn_cast<llvm::ConstantInt>(unboxed)) {
            boxed = embedRelocatablePtr(emitter.getIntConstant(llvm_val->getSExtValue()), g.llvm_value_type_ptr);
            emitter.setType(boxed, RefType::BORROWED);
        } else {
            boxed = emitter.getBuilder()->CreateCall(g.funcs.boxInt, unboxed);
            emitter.setType(boxed, RefType::OWNED);
        }
        return new ConcreteCompilerVariable(other_type, boxed);
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        return rtn;
    }

    CompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        llvm::CallSite call
            = emitter.createCall(info.unw_info, g.funcs.raiseNotIterableError, embedConstantPtr("int", g.i8_ptr));
        call.setDoesNotReturn();
        return makeInt(llvm::UndefValue::get(g.i64));
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        llvm::Value* cmp
            = emitter.getBuilder()->CreateICmpNE(var->getValue()->val, llvm::ConstantInt::get(g.i64, 0, false));
        return boolFromI1(emitter, cmp);
    }

    CompilerVariable* unaryop(IREmitter& emitter, const OpInfo& info, VAR* var, AST_TYPE::AST_TYPE op_type) override {
        llvm::Value* unboxed = var->getValue()->val;
        if (op_type == AST_TYPE::USub) {
            if (llvm::ConstantInt* llvm_val = llvm::dyn_cast<llvm::ConstantInt>(unboxed)) {
                int64_t val = llvm_val->getSExtValue();
                if (val != PYSTON_INT_MIN) {
                    return makeInt(-val);
                }
            }
            // Not safe to emit a simple negation in the general case since val could be INT_MIN.
            // Could emit a check though.
        }

        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        auto rtn = converted->unaryop(emitter, info, op_type);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        bool can_lower = (rhs->getType() == INT && exp_type == Compare);
        if (!can_lower) {
            // if the rhs is a float convert the lhs to a float and do the operation on it.
            if (rhs->getType() == FLOAT) {
                if (op_type == AST_TYPE::IsNot || op_type == AST_TYPE::Is)
                    return makeBool(op_type == AST_TYPE::IsNot);

                llvm::Value* conv = emitter.getBuilder()->CreateSIToFP(var->getValue()->val, g.double_);
                auto converted_left = makeFloat(conv);
                return converted_left->binexp(emitter, info, rhs, op_type, exp_type);
            }

            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
            return rtn;
        }

        assert(rhs->getType() == INT);
        llvm::Value* right_val = static_cast<VAR*>(rhs)->getValue()->val;
        llvm::Value* v;
        /*if (op_type == AST_TYPE::Mod) {
            v = emitter.createCall2(info.unw_info, g.funcs.mod_i64_i64, var->getValue(), converted_right->getValue())
                    ;
        } else if (op_type == AST_TYPE::Div || op_type == AST_TYPE::FloorDiv) {
            v = emitter.createCall2(info.unw_info, g.funcs.div_i64_i64, var->getValue(), converted_right->getValue())
                    ;
        } else if (op_type == AST_TYPE::Pow) {
            v = emitter.createCall2(info.unw_info, g.funcs.pow_i64_i64, var->getValue(), converted_right->getValue())
                    ;
        } else if (exp_type == BinOp || exp_type == AugBinOp) {
            llvm::Instruction::BinaryOps binopcode;
            switch (op_type) {
                case AST_TYPE::Add:
                    binopcode = llvm::Instruction::Add;
                    break;
                case AST_TYPE::BitAnd:
                    binopcode = llvm::Instruction::And;
                    break;
                case AST_TYPE::BitOr:
                    binopcode = llvm::Instruction::Or;
                    break;
                case AST_TYPE::BitXor:
                    binopcode = llvm::Instruction::Xor;
                    break;
                case AST_TYPE::LShift:
                    binopcode = llvm::Instruction::Shl;
                    break;
                case AST_TYPE::RShift:
                    binopcode = llvm::Instruction::AShr;
                    break;
                case AST_TYPE::Mult:
                    binopcode = llvm::Instruction::Mul;
                    break;
                case AST_TYPE::Sub:
                    binopcode = llvm::Instruction::Sub;
                    break;
                default:
                    ASSERT(0, "%s", getOpName(op_type).c_str());
                    abort();
                    break;
            }
            v = emitter.getBuilder()->CreateBinOp(binopcode, var->getValue(), converted_right->getValue());
        } else */ {
            assert(exp_type == Compare);
            llvm::CmpInst::Predicate cmp_pred;
            switch (op_type) {
                case AST_TYPE::Eq:
                case AST_TYPE::Is:
                    cmp_pred = llvm::CmpInst::ICMP_EQ;
                    break;
                case AST_TYPE::Lt:
                    cmp_pred = llvm::CmpInst::ICMP_SLT;
                    break;
                case AST_TYPE::LtE:
                    cmp_pred = llvm::CmpInst::ICMP_SLE;
                    break;
                case AST_TYPE::Gt:
                    cmp_pred = llvm::CmpInst::ICMP_SGT;
                    break;
                case AST_TYPE::GtE:
                    cmp_pred = llvm::CmpInst::ICMP_SGE;
                    break;
                case AST_TYPE::NotEq:
                case AST_TYPE::IsNot:
                    cmp_pred = llvm::CmpInst::ICMP_NE;
                    break;
                default:
                    ASSERT(0, "%s", getOpName(op_type)->c_str());
                    abort();
                    break;
            }
            v = emitter.getBuilder()->CreateICmp(cmp_pred, var->getValue()->val, right_val);
        }
        assert(v->getType() == g.i1);
        return boolFromI1(emitter, v);
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        llvm::CallSite call
            = emitter.createCall(info.unw_info, g.funcs.raiseNotIterableError, embedConstantPtr("int", g.i8_ptr));
        call.setDoesNotReturn();
        return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(BOOL->llvmType()));
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_INT; }

    int _numFrameArgs() { return 1; }

    Box* _deserializeFromFrame(const FrameVals& vals) {
        assert(vals.size() == 1);

        return boxInt(vals[0]);
    }

    void _serializeToFrame(llvm::Value* val, std::vector<llvm::Value*>& stackmap_args) { stackmap_args.push_back(val); }

    static llvm::Value* extractInt(CompilerVariable* v) {
        assert(v->getType() == INT);
        return static_cast<VAR*>(v)->getValue()->val;
    }
} _INT;
CompilerType* INT = &_INT;

CompilerVariable* makeInt(llvm::Value* n) {
    assert(n->getType() == g.i64);
    return new IntType::VAR(&_INT, std::make_shared<IntType::Unboxed>(n, nullptr));
}

CompilerVariable* makeInt(int64_t n) {
    return makeInt(llvm::ConstantInt::get(g.i64, n, true));
}

CompilerVariable* makeUnboxedInt(IREmitter& emitter, ConcreteCompilerVariable* v) {
    assert(v->getType() == BOXED_INT);
    llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxInt, v->getValue());
    return new IntType::VAR(&_INT, std::make_shared<IntType::Unboxed>(unboxed, v));
}

CompilerVariable* makeUnboxedInt(IREmitter& emitter, llvm::Value* v) {
    assert(v->getType() == g.llvm_value_type_ptr);
    return makeUnboxedInt(emitter, new ConcreteCompilerVariable(BOXED_INT, v));
}

class FloatType : public UnboxedType<llvm::Value*, FloatType> {
public:
    FloatType() {}

    std::string debugName() override { return "float"; }

    void _assertMatches(llvm::Value* v) { assert(v->getType() == g.double_); }

    llvm::Value* _dup(llvm::Value* v, DupCache& cache) { return v; }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override {
        static std::vector<AbstractFunctionType::Sig*> sigs;
        if (sigs.size() == 0) {
            AbstractFunctionType::Sig* float_sig = new AbstractFunctionType::Sig();
            float_sig->rtn_type = UNBOXED_FLOAT;
            float_sig->arg_types.push_back(UNBOXED_FLOAT);
            sigs.push_back(float_sig);

            AbstractFunctionType::Sig* int_sig = new AbstractFunctionType::Sig();
            int_sig->rtn_type = UNBOXED_FLOAT;
            int_sig->arg_types.push_back(UNBOXED_INT);
            sigs.push_back(int_sig);

            AbstractFunctionType::Sig* unknown_sig = new AbstractFunctionType::Sig();
            unknown_sig->rtn_type = UNKNOWN;
            unknown_sig->arg_types.push_back(UNKNOWN);
            sigs.push_back(unknown_sig);
        }

        if (attr->s() == "__add__" || attr->s() == "__sub__" || attr->s() == "__mul__" || attr->s() == "__div__"
            || attr->s() == "__pow__" || attr->s() == "__floordiv__" || attr->s() == "__mod__"
            || attr->s() == "__pow__") {
            return AbstractFunctionType::get(sigs);
        }

        if (attr->s() == "__iadd__" || attr->s() == "__isub__" || attr->s() == "__imul__" || attr->s() == "__idiv__"
            || attr->s() == "__ipow__" || attr->s() == "__ifloordiv__" || attr->s() == "__imod__"
            || attr->s() == "__ipow__") {
            return AbstractFunctionType::get(sigs);
        }

        return BOXED_FLOAT->getattrType(attr, cls_only);
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr,
                              bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CallattrFlags flags,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, args, keyword_names);
        return rtn;
    }

    void setattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CompilerVariable* v) override {
        llvm::CallSite call
            = emitter.createCall3(info.unw_info, g.funcs.raiseAttributeErrorStr, embedConstantPtr("float", g.i8_ptr),
                                  embedConstantPtr(attr->data(), g.i8_ptr), getConstantInt(attr->size(), g.i64));
        call.setDoesNotReturn();
    }

    void delattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr) override {
        llvm::CallSite call
            = emitter.createCall3(info.unw_info, g.funcs.raiseAttributeErrorStr, embedConstantPtr("float", g.i8_ptr),
                                  embedConstantPtr(attr->data(), g.i8_ptr), getConstantInt(attr->size(), g.i64));
        call.setDoesNotReturn();
    }

    ConcreteCompilerVariable* _makeConverted(IREmitter& emitter, llvm::Value* unboxed,
                                             ConcreteCompilerType* other_type) {
        assert(other_type == BOXED_FLOAT);
        llvm::Value* boxed;
        if (llvm::ConstantFP* llvm_val = llvm::dyn_cast<llvm::ConstantFP>(unboxed)) {
            // Will this ever hit the cache?
            boxed = embedRelocatablePtr(emitter.getFloatConstant(llvm_val->getValueAPF().convertToDouble()),
                                        g.llvm_value_type_ptr);
            emitter.setType(boxed, RefType::BORROWED);
        } else {
            boxed = emitter.getBuilder()->CreateCall(g.funcs.boxFloat, unboxed);
            emitter.setType(boxed, RefType::OWNED);
        }
        return new ConcreteCompilerVariable(other_type, boxed);
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        llvm::Value* cmp
            = emitter.getBuilder()->CreateFCmpUNE(var->getValue()->val, llvm::ConstantFP::get(g.double_, 0));
        return boolFromI1(emitter, cmp);
    }

    CompilerVariable* unaryop(IREmitter& emitter, const OpInfo& info, VAR* var, AST_TYPE::AST_TYPE op_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        auto rtn = converted->unaryop(emitter, info, op_type);
        return rtn;
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        assert(rhs->getType() != UNBOXED_FLOAT); // we could handle this here but it shouldn't happen

        if (rhs->getType() != INT && rhs->getType() != FLOAT) {
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
            CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
            return rtn;
        }

        llvm::Value* rhs_val;
        if (rhs->getType() == FLOAT) {
            rhs_val = static_cast<FloatType::VAR*>(rhs)->getValue()->val;
        } else {
            if (op_type == AST_TYPE::IsNot || op_type == AST_TYPE::Is)
                return makeBool(op_type == AST_TYPE::IsNot);

            assert(rhs->getType() == INT);
            llvm::Value* right_val = IntType::extractInt(rhs);
            rhs_val = emitter.getBuilder()->CreateSIToFP(right_val, g.double_);
        }

        llvm::Value* v;
        bool succeeded = true;
        if (op_type == AST_TYPE::Mod) {
            v = emitter.createCall2(info.unw_info, g.funcs.mod_float_float, var->getValue()->val, rhs_val);
        } else if (op_type == AST_TYPE::Div || op_type == AST_TYPE::TrueDiv) {
            v = emitter.createCall2(info.unw_info, g.funcs.div_float_float, var->getValue()->val, rhs_val);
        } else if (op_type == AST_TYPE::FloorDiv) {
            v = emitter.createCall2(info.unw_info, g.funcs.floordiv_float_float, var->getValue()->val, rhs_val);
        } else if (op_type == AST_TYPE::Pow) {
            v = emitter.createCall2(info.unw_info, g.funcs.pow_float_float, var->getValue()->val, rhs_val);
        } else if (exp_type == BinOp || exp_type == AugBinOp) {
            llvm::Instruction::BinaryOps binopcode;
            switch (op_type) {
                case AST_TYPE::Add:
                    binopcode = llvm::Instruction::FAdd;
                    break;
                case AST_TYPE::Mult:
                    binopcode = llvm::Instruction::FMul;
                    break;
                case AST_TYPE::Sub:
                    binopcode = llvm::Instruction::FSub;
                    break;
                case AST_TYPE::BitAnd:
                case AST_TYPE::BitOr:
                case AST_TYPE::BitXor:
                case AST_TYPE::LShift:
                case AST_TYPE::RShift:
                    succeeded = false;
                    break;
                default:
                    ASSERT(0, "%s", getOpName(op_type)->c_str());
                    abort();
                    break;
            }

            if (succeeded) {
                v = emitter.getBuilder()->CreateBinOp(binopcode, var->getValue()->val, rhs_val);
            }
        } else {
            assert(exp_type == Compare);
            llvm::CmpInst::Predicate cmp_pred;
            switch (op_type) {
                case AST_TYPE::Eq:
                case AST_TYPE::Is:
                    cmp_pred = llvm::CmpInst::FCMP_OEQ;
                    break;
                case AST_TYPE::Lt:
                    cmp_pred = llvm::CmpInst::FCMP_OLT;
                    break;
                case AST_TYPE::LtE:
                    cmp_pred = llvm::CmpInst::FCMP_OLE;
                    break;
                case AST_TYPE::Gt:
                    cmp_pred = llvm::CmpInst::FCMP_OGT;
                    break;
                case AST_TYPE::GtE:
                    cmp_pred = llvm::CmpInst::FCMP_OGE;
                    break;
                case AST_TYPE::NotEq:
                case AST_TYPE::IsNot:
                    cmp_pred = llvm::CmpInst::FCMP_UNE;
                    break;
                default:
                    ASSERT(0, "%s", getOpName(op_type)->c_str());
                    abort();
                    break;
            }
            v = emitter.getBuilder()->CreateFCmp(cmp_pred, var->getValue()->val, rhs_val);
        }

        if (succeeded) {
            if (v->getType() == g.double_) {
                return makeFloat(v);
            } else {
                return boolFromI1(emitter, v);
            }
        }

        // TODO duplication with top of function, other functions, etc
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        return rtn;
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        llvm::CallSite call
            = emitter.createCall(info.unw_info, g.funcs.raiseNotIterableError, embedConstantPtr("float", g.i8_ptr));
        call.setDoesNotReturn();
        return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(BOOL->llvmType()));
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_FLOAT; }

    int _numFrameArgs() { return 1; }

    void _serializeToFrame(llvm::Value* v, std::vector<llvm::Value*>& stackmap_args) { stackmap_args.push_back(v); }

    Box* _deserializeFromFrame(const FrameVals& vals) {
        assert(vals.size() == 1);

        double d = *reinterpret_cast<const double*>(&vals[0]);
        return boxFloat(d);
    }
} _FLOAT;
CompilerType* FLOAT = &_FLOAT;

class PhonyUnboxedType : public ConcreteCompilerType {
private:
    llvm::Type* t;
    CompilerType* usable_type;

public:
    PhonyUnboxedType(llvm::Type* t, CompilerType* usable_type) : t(t), usable_type(usable_type) {}

    std::string debugName() override { return "phony(" + ConcreteCompilerType::debugName() + ")"; }

    CompilerType* getUsableType() override { return usable_type; }
    ConcreteCompilerType* getBoxType() override { return getUsableType()->getBoxType(); }

    llvm::Type* llvmType() override { return t; }

    Box* deserializeFromFrame(const FrameVals& vals) override { RELEASE_ASSERT(0, "unavailable for phony types"); }
};

ConcreteCompilerType* UNBOXED_INT = new PhonyUnboxedType(llvm::Type::getInt64Ty(llvm::getGlobalContext()), INT);
ConcreteCompilerType* UNBOXED_FLOAT = new PhonyUnboxedType(llvm::Type::getDoubleTy(llvm::getGlobalContext()), FLOAT);

CompilerVariable* makeFloat(llvm::Value* n) {
    assert(n->getType() == g.double_);
    return new FloatType::VAR(&_FLOAT, std::make_shared<FloatType::Unboxed>(n, nullptr));
}

CompilerVariable* makeFloat(double n) {
    return makeFloat(llvm::ConstantFP::get(g.double_, n));
}

CompilerVariable* makeUnboxedFloat(IREmitter& emitter, ConcreteCompilerVariable* v) {
    assert(v->getType() == BOXED_FLOAT);
    llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxFloat, v->getValue());
    return new FloatType::VAR(&_FLOAT, std::make_shared<FloatType::Unboxed>(unboxed, v));
}

CompilerVariable* makeUnboxedFloat(IREmitter& emitter, llvm::Value* v) {
    assert(v->getType() == g.llvm_value_type_ptr);
    return makeUnboxedFloat(emitter, new ConcreteCompilerVariable(BOXED_FLOAT, v));
}


ConcreteCompilerVariable* makeLong(IREmitter& emitter, Box* v) {
    return new ConcreteCompilerVariable(
        LONG, emitter.setType(embedRelocatablePtr(v, g.llvm_value_type_ptr), RefType::BORROWED));
}

ConcreteCompilerVariable* makePureImaginary(IREmitter& emitter, Box* v) {
    return new ConcreteCompilerVariable(
        BOXED_COMPLEX, emitter.setType(embedRelocatablePtr(v, g.llvm_value_type_ptr), RefType::BORROWED));
}

class KnownClassobjType : public ValuedCompilerType<BoxedClass*> {
private:
    BoxedClass* cls;

    static std::unordered_map<BoxedClass*, KnownClassobjType*> made;

    KnownClassobjType(BoxedClass* cls) : cls(cls) { assert(cls); }

public:
    std::string debugName() override { return "class '" + std::string(getNameOfClass(cls)) + "'"; }

    void assertMatches(BoxedClass* cls) override { assert(cls == this->cls); }

    static KnownClassobjType* fromClass(BoxedClass* cls) {
        KnownClassobjType*& rtn = made[cls];
        if (rtn == NULL) {
            rtn = new KnownClassobjType(cls);
        }
        return rtn;
    }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<llvm::StringRef>* keyword_names) override {
        RELEASE_ASSERT(!argspec.has_starargs, "");
        RELEASE_ASSERT(!argspec.has_kwargs, "");
        RELEASE_ASSERT(argspec.num_keywords == 0, "");

        bool is_well_defined = (cls == xrange_cls);
        assert(is_well_defined);
        return typeFromClass(cls);
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override { abort(); }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());
        abort();
    }

    int numFrameArgs() override { return 0; }
};
std::unordered_map<BoxedClass*, KnownClassobjType*> KnownClassobjType::made;

CompilerType* typeOfClassobj(BoxedClass* cls) {
    return KnownClassobjType::fromClass(cls);
}

class NormalObjectType : public ConcreteCompilerType {
private:
    BoxedClass* cls;

    static std::unordered_map<BoxedClass*, NormalObjectType*> made;

    NormalObjectType(BoxedClass* cls) : cls(cls) { assert(cls); }

public:
    llvm::Type* llvmType() override { return g.llvm_value_type_ptr; }
    std::string debugName() override {
        assert(cls);
        // TODO add getTypeName

        return "NormalType(" + std::string(getNameOfClass(cls)) + ")";
    }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == this) {
            return var;
        }
        ASSERT(other_type == UNKNOWN, "%s", other_type->debugName().c_str());
        return new ConcreteCompilerVariable(UNKNOWN, var->getValue());
    }

    bool isFitBy(BoxedClass* c) override {
        // I don't think it's ok to accept subclasses
        return c == cls;
    }

    bool canStaticallyResolveGetattrs() {
        return (cls->is_constant && !cls->instancesHaveHCAttrs() && !cls->instancesHaveDictAttrs()
                && cls->hasGenericGetattr());
    }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override {
        // Any changes here need to be mirrored in getattr()
        if (canStaticallyResolveGetattrs()) {
            Box* rtattr = typeLookup(cls, attr);
            if (rtattr == NULL)
                return UNDEF;

            if (rtattr->cls == function_cls) {
                return AbstractFunctionType::fromRT(static_cast<BoxedFunction*>(rtattr), true);
                // return typeFromClass(instancemethod_cls);
            } else {
                // Have to follow the descriptor protocol here
                return UNKNOWN;
            }
        }

        return UNKNOWN;
    }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<llvm::StringRef>* keyword_names) override {
        return UNKNOWN;
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                              bool cls_only) override {
        // Any changes here need to be mirrored in getattrType()
        if (canStaticallyResolveGetattrs()) {
            Box* rtattr = typeLookup(cls, attr);
            if (rtattr == NULL) {
                ExceptionStyle exception_style = info.preferredExceptionStyle();
                llvm::Value* raise_func = exception_style == CXX ? g.funcs.raiseAttributeErrorStr
                                                                 : g.funcs.raiseAttributeErrorStrCapi;
                llvm::CallSite call = emitter.createCall3(
                    info.unw_info, raise_func, embedRelocatablePtr(cls->tp_name, g.i8_ptr),
                    embedRelocatablePtr(attr->data(), g.i8_ptr), getConstantInt(attr->size(), g.i64), exception_style,
                    IREmitter::ALWAYS_THROWS);

                if (exception_style == CXX)
                    call.setDoesNotReturn();
                return undefVariable();
            }

            ASSERT(rtattr, "%s.%s", debugName().c_str(), attr->data());
            if (rtattr->cls == function_cls) {
                CompilerVariable* clattr = new ConcreteCompilerVariable(
                    typeFromClass(function_cls),
                    emitter.setType(embedRelocatablePtr(rtattr, g.llvm_value_type_ptr), RefType::BORROWED));

                return InstanceMethodType::makeIM(
                    var, clattr,
                    new ConcreteCompilerVariable(
                        UNKNOWN, emitter.setType(embedRelocatablePtr(cls, g.llvm_value_type_ptr), RefType::BORROWED)));
            }
        }

        // TODO could specialize more since we know the class already
        return UNKNOWN->getattr(emitter, info, var, attr, cls_only);
    }

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                 CompilerVariable* v) override {
        return UNKNOWN->setattr(emitter, info, var, attr, v);
    }

    void delattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr) override {
        return UNKNOWN->delattr(emitter, info, var, attr);
    }

    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, ArgPassSpec argspec,
                           const std::vector<CompilerVariable*>& args,
                           const std::vector<BoxedString*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->call(emitter, info, argspec, args, keyword_names);
        return rtn;
    }

    CompilerVariable* tryCallattrConstant(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                          BoxedString* attr, bool clsonly, ArgPassSpec argspec,
                                          const std::vector<CompilerVariable*>& args,
                                          const std::vector<BoxedString*>* keyword_names, bool* no_attribute = NULL,
                                          ExceptionStyle exception_style = CXX) {
        if (!canStaticallyResolveGetattrs())
            return NULL;

        Box* rtattr = typeLookup(cls, attr);
        if (rtattr == NULL) {
            if (no_attribute) {
                *no_attribute = true;
            } else {
                llvm::Value* raise_func = exception_style == CXX ? g.funcs.raiseAttributeErrorStr
                                                                 : g.funcs.raiseAttributeErrorStrCapi;

                llvm::CallSite call = emitter.createCall3(
                    info.unw_info, raise_func, embedRelocatablePtr(cls->tp_name, g.i8_ptr),
                    emitter.setType(embedRelocatablePtr(attr->data(), g.i8_ptr), RefType::BORROWED),
                    getConstantInt(attr->size(), g.i64), exception_style, IREmitter::ALWAYS_THROWS);

                if (exception_style == CXX)
                    call.setDoesNotReturn();
            }
            return undefVariable();
        }

        if (rtattr->cls != function_cls)
            return NULL;
        BoxedFunction* rtattr_func = static_cast<BoxedFunction*>(rtattr);

        if (argspec.num_keywords || argspec.has_starargs || argspec.has_kwargs)
            return NULL;

        // We can handle can_change_defaults=true functions by just returning NULL here,
        // but I don't think we should be running into that case.
        RELEASE_ASSERT(!rtattr_func->can_change_defaults, "could handle this but unexpected");

        FunctionMetadata* md = rtattr_func->md;
        assert(md);

        ParamReceiveSpec paramspec = rtattr_func->getParamspec();
        if (md->takes_varargs || paramspec.takes_kwargs)
            return NULL;

        RELEASE_ASSERT(paramspec.num_args == md->numReceivedArgs(), "");
        RELEASE_ASSERT(args.size() + 1 >= paramspec.num_args - paramspec.num_defaults
                           && args.size() + 1 <= paramspec.num_args,
                       "%d", info.unw_info.current_stmt->lineno);

        CompiledFunction* cf = NULL;
        CompiledFunction* best_exception_mismatch = NULL;
        bool found = false;
        // TODO have to find the right version.. similar to resolveclfunc?
        for (int i = 0; i < md->versions.size(); i++) {
            cf = md->versions[i];
            assert(cf->spec->arg_types.size() == md->numReceivedArgs());

            bool fits = true;
            for (int j = 0; j < args.size(); j++) {
                if (!args[j]->canConvertTo(cf->spec->arg_types[j + 1])) {
                    fits = false;
                    break;
                }
            }
            if (!fits)
                continue;

            if (cf->exception_style != exception_style) {
                if (!best_exception_mismatch)
                    best_exception_mismatch = cf;
                continue;
            }

            found = true;
            break;
        }

        if (!found) {
            assert(best_exception_mismatch);
            cf = best_exception_mismatch;
            found = true;
        }

        RELEASE_ASSERT(found, "");
        RELEASE_ASSERT(cf->code, "");

        std::vector<llvm::Type*> arg_types;
        RELEASE_ASSERT(paramspec.num_args == md->numReceivedArgs(), "");
        for (int i = 0; i < paramspec.num_args; i++) {
            // TODO support passing unboxed values as arguments
            assert(cf->spec->arg_types[i]->llvmType() == g.llvm_value_type_ptr);

            if (i == 3) {
                arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                break;
            } else {
                arg_types.push_back(g.llvm_value_type_ptr);
            }
        }
        llvm::FunctionType* ft = llvm::FunctionType::get(cf->spec->rtn_type->llvmType(), arg_types, false);

        llvm::Value* linked_function;
        if (cf->func) // for JITed functions we need to make the desination address relocatable.
            linked_function = embedRelocatablePtr(cf->code, ft->getPointerTo());
        else
            linked_function = embedConstantPtr(cf->code, ft->getPointerTo());

        std::vector<CompilerVariable*> new_args;
        new_args.push_back(var);
        new_args.insert(new_args.end(), args.begin(), args.end());

        RELEASE_ASSERT(!rtattr_func->can_change_defaults, "");
        for (int i = args.size() + 1; i < paramspec.num_args; i++) {
            // TODO should _call() be able to take llvm::Value's directly?
            auto value = rtattr_func->defaults->elts[i - paramspec.num_args + paramspec.num_defaults];
            llvm::Value* llvm_value;
            if (value)
                llvm_value = embedRelocatablePtr(value, g.llvm_value_type_ptr);
            else
                llvm_value = getNullPtr(g.llvm_value_type_ptr);
            emitter.setType(llvm_value, RefType::BORROWED);

            new_args.push_back(new ConcreteCompilerVariable(UNKNOWN, llvm_value));
        }

        std::vector<llvm::Value*> other_args;

        ConcreteCompilerVariable* rtn = _call(emitter, info, linked_function, cf->exception_style, cf->code, other_args,
                                              argspec, new_args, keyword_names, cf->spec->rtn_type);
        assert(rtn->getType() == cf->spec->rtn_type);
        ConcreteCompilerType* rtn_type = rtn->getType();

        assert(rtn_type != UNDEF);

        // We should provide unboxed versions of these rather than boxing then unboxing:
        // TODO is it more efficient to unbox here, or should we leave it boxed?
        if (rtn_type == BOXED_BOOL) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, rtn->getValue());
            return boolFromI1(emitter, unboxed);
        }
        if (rtn_type == BOXED_INT) {
            return makeUnboxedInt(emitter, rtn);
        }
        if (rtn_type == UNBOXED_INT) {
            return makeInt(rtn->getValue());
        }
        if (rtn_type == BOXED_FLOAT) {
            return makeUnboxedFloat(emitter, rtn);
        }

        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                               CallattrFlags flags, const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        ExceptionStyle exception_style = info.preferredExceptionStyle();

        bool no_attribute = false;
        bool* no_attribute_ptr = NULL;
        if (flags.null_on_nonexistent)
            no_attribute_ptr = &no_attribute;

        CompilerVariable* called_constant = tryCallattrConstant(emitter, info, var, attr, flags.cls_only, flags.argspec,
                                                                args, keyword_names, no_attribute_ptr, exception_style);

        if (no_attribute)
            return new ConcreteCompilerVariable(UNKNOWN, getNullPtr(g.llvm_value_type_ptr));

        if (called_constant)
            return called_constant;

        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, args, keyword_names);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted_rhs = rhs->makeConverted(emitter, rhs->getBoxType());

        BoxedClass* rhs_cls = converted_rhs->guaranteedClass();
        if (rhs_cls && rhs_cls->is_constant && !rhs_cls->is_user_defined) {
            // Ugly, but for now special-case the set of type-pairs that we know will always work
            if (exp_type == BinOp
                && ((cls == int_cls && rhs_cls == int_cls) || (cls == float_cls && rhs_cls == float_cls)
                    || (cls == list_cls && rhs_cls == int_cls) || (cls == str_cls))) {

                BoxedString* left_side_name = getOpName(op_type);

                bool no_attribute = false;

                CompilerVariable* called_constant
                    = tryCallattrConstant(emitter, info, var, left_side_name, true, ArgPassSpec(1, 0, 0, 0),
                                          { converted_rhs }, NULL, &no_attribute);

                if (no_attribute) {
                    assert(called_constant->getType() == UNDEF);

                    // Kind of hacky, but just call into getitem like normal.  except...
                    auto r = UNKNOWN->binexp(emitter, info, var, converted_rhs, op_type, exp_type);
                    // ... return the undef value, since that matches what the type analyzer thought we would do.
                    return called_constant;
                }

                if (called_constant) {
                    return called_constant;
                }
            }
        }

        auto rtn = UNKNOWN->binexp(emitter, info, var, converted_rhs, op_type, exp_type);
        return rtn;
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        return UNKNOWN->contains(emitter, info, var, lhs);
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        static BoxedString* attr = getStaticString("__getitem__");
        bool no_attribute = false;

        if (slice->getType() == UNBOXED_SLICE) {
            UnboxedSlice slice_val = extractSlice(slice);

            // This corresponds to the case in apply_slice that calls into PySequence_GetSlice.
            // Other cases will get handled by UNKNOWN.getitem
            if (!slice_val.step && canStaticallyResolveGetattrs() && cls->tp_as_sequence
                && cls->tp_as_sequence->sq_slice) {
                if ((!slice_val.start || slice_val.start->getType() == INT || slice_val.start->getType() == BOXED_INT)
                    && (!slice_val.stop || slice_val.stop->getType() == INT
                        || slice_val.stop->getType() == BOXED_INT)) {

                    CompilerType* return_type = getattrType(attr, true)->callType(ArgPassSpec(1), { SLICE }, NULL);
                    assert(return_type->getConcreteType() == return_type);

                    llvm::Value* start = NULL;
                    if (!slice_val.start)
                        start = getConstantInt(0, g.i64);
                    else {
                        if (slice_val.start->getType() == BOXED_INT)
                            slice_val.start
                                = makeUnboxedInt(emitter, static_cast<ConcreteCompilerVariable*>(slice_val.start));
                        start = IntType::extractInt(slice_val.start);
                    }

                    llvm::Value* stop = NULL;
                    if (!slice_val.stop)
                        stop = getConstantInt(PY_SSIZE_T_MAX, g.i64);
                    else {
                        if (slice_val.stop->getType() == BOXED_INT)
                            slice_val.stop
                                = makeUnboxedInt(emitter, static_cast<ConcreteCompilerVariable*>(slice_val.stop));
                        stop = IntType::extractInt(slice_val.stop);
                    }

                    static llvm::FunctionType* ft = llvm::FunctionType::get(
                        g.llvm_value_type_ptr, { g.llvm_value_type_ptr, g.i64, g.i64 }, false);
                    llvm::Value* r = emitter.createCall3(
                        info.unw_info, embedConstantPtr((void*)PySequence_GetSlice, ft->getPointerTo()),
                        var->getValue(), start, stop, CAPI, getNullPtr(g.llvm_value_type_ptr));
                    emitter.setType(r, RefType::OWNED);
                    emitter.setNullable(r, true);

                    return new ConcreteCompilerVariable(static_cast<ConcreteCompilerType*>(return_type), r);
                }
            }
        }

        // Only try calling getitem if it's not a slice.  For the slice case, defer to UNKNOWN->getitem, which will
        // call into apply_slice
        if (slice->getType() != UNBOXED_SLICE || extractSlice(slice).step != NULL) {
            ExceptionStyle exception_style = info.preferredExceptionStyle();

            CompilerVariable* called_constant
                = tryCallattrConstant(emitter, info, var, attr, true, ArgPassSpec(1, 0, 0, 0), { slice }, NULL,
                                      &no_attribute, exception_style);

            if (no_attribute) {
                assert(called_constant->getType() == UNDEF);

                // Kind of hacky, but just call into getitem like normal.  except...
                auto r = UNKNOWN->getitem(emitter, info, var, slice);
                // ... return the undef value, since that matches what the type analyzer thought we would do.
                return called_constant;
            }

            if (called_constant)
                return called_constant;
        }

        return UNKNOWN->getitem(emitter, info, var, slice);
    }

    CompilerVariable* getPystonIter(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return UNKNOWN->getPystonIter(emitter, info, var);
    }

    CompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        static BoxedString* attr = getStaticString("__len__");
        CompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL);
        if (called_constant)
            return called_constant;

        return UNKNOWN->len(emitter, info, var);
    }

    CompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        if (cls == None->cls)
            return makeBool(false);

        static BoxedString* attr = getStaticString("__nonzero__");
        bool no_attribute = false;
        CompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL, &no_attribute);

        // TODO: if no_attribute, we could optimize by continuing the dispatch process and trying
        // to call __len__ (and if that doesn't exist, returning a static true).
        // For now, I'd rather not duplicate the dispatch behavior between here and objmodel.cpp::nonzero.

        if (called_constant && !no_attribute)
            return called_constant;

        if (cls == bool_cls) {
            assert(0 && "should have been caught by above case");
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, var->getValue());
            assert(unboxed->getType() == g.i1);
            return boolFromI1(emitter, unboxed);
        }

        return UNKNOWN->nonzero(emitter, info, var);
    }

    CompilerVariable* unaryop(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              AST_TYPE::AST_TYPE op_type) override {
        BoxedString* attr = getOpName(op_type);

        bool no_attribute = false;
        CompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL, &no_attribute);

        if (called_constant && !no_attribute)
            return called_constant;

        return UNKNOWN->unaryop(emitter, info, var, op_type);
    }

    CompilerVariable* hasnext(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        static BoxedString* attr = getStaticString("__hasnext__");

        CompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL, NULL);

        if (called_constant)
            return called_constant;

        return UNKNOWN->hasnext(emitter, info, var);
    }

    static NormalObjectType* fromClass(BoxedClass* cls) {
        NormalObjectType*& rtn = made[cls];
        if (rtn == NULL) {
            rtn = new NormalObjectType(cls);
        }
        return rtn;
    }

    BoxedClass* guaranteedClass() override { return cls; }

    ConcreteCompilerType* getBoxType() override { return this; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        return incref(reinterpret_cast<Box*>(vals[0]));
    }
};
std::unordered_map<BoxedClass*, NormalObjectType*> NormalObjectType::made;
ConcreteCompilerType* STR, *BOXED_INT, *BOXED_FLOAT, *BOXED_BOOL, *NONE;

class ClosureType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_closure_type_ptr; }
    std::string debugName() override { return "closure"; }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                              bool cls_only) override {
        RELEASE_ASSERT(0, "should not be called\n");
        /*
        assert(!cls_only);
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(var->getValue(), g.llvm_value_type_ptr);
        return ConcreteCompilerVariable(UNKNOWN, bitcast, true).getattr(emitter, info, attr, cls_only);
        */
    }

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                 CompilerVariable* v) override {
        RELEASE_ASSERT(0, "should not be called\n");
    }

    ConcreteCompilerType* getConcreteType() override { return this; }
    ConcreteCompilerType* getBoxType() override { return this; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        return incref(reinterpret_cast<Box*>(vals[0]));
    }
} _CLOSURE;
ConcreteCompilerType* CLOSURE = &_CLOSURE;

class GeneratorType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_generator_type_ptr; }
    std::string debugName() override { return "generator"; }

    ConcreteCompilerType* getConcreteType() override { return this; }
    ConcreteCompilerType* getBoxType() override { return GENERATOR; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());
        return incref(reinterpret_cast<Box*>(vals[0]));
    }
} _GENERATOR;
ConcreteCompilerType* GENERATOR = &_GENERATOR;

class FrameInfoType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_frame_info_type->getPointerTo(); }
    std::string debugName() override { return "FrameInfo"; }

    ConcreteCompilerType* getConcreteType() override { return this; }
    ConcreteCompilerType* getBoxType() override { return FRAME_INFO; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        RELEASE_ASSERT(false, "should not be called"); // This function shouldn't be called.
    }
} _FRAME_INFO;
ConcreteCompilerType* FRAME_INFO = &_FRAME_INFO;

class StrConstantType : public ValuedCompilerType<BoxedString*> {
public:
    std::string debugName() override { return "str_constant"; }

    void assertMatches(BoxedString* v) override {}

    ConcreteCompilerType* getConcreteType() override { return STR; }

    ConcreteCompilerType* getBoxType() override { return STR; }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        assert(other_type == STR || other_type == UNKNOWN);
        llvm::Value* boxed = embedRelocatablePtr(var->getValue(), g.llvm_value_type_ptr);
        emitter.setType(boxed, RefType::BORROWED);
        return new ConcreteCompilerVariable(other_type, boxed);
    }

    bool canConvertTo(CompilerType* other) override { return (other == STR || other == UNKNOWN); }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr,
                              bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CallattrFlags flags,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, args, keyword_names);
        return rtn;
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        return rtn;
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->contains(emitter, info, lhs);
        return rtn;
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return makeBool(var->getValue()->size() != 0);
    }

    CompilerVariable* dup(VAR* var, DupCache& cache) override {
        CompilerVariable*& rtn = cache[var];

        if (rtn == NULL) {
            rtn = new VAR(this, var->getValue());
        }
        return rtn;
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override {
        RELEASE_ASSERT(0, "shouldn't serialize/deserialize non-concrete types?");
        /*
        stackmap_args.push_back(embedRelocatablePtr(var->getValue().data(), g.i8_ptr));
        stackmap_args.push_back(embedRelocatablePtr(var->getValue().size(), g.i64));
        */
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        RELEASE_ASSERT(0, "shouldn't serialize/deserialize non-concrete types?");
        /*
        assert(vals.size() == numFrameArgs());

        return boxStringPtr(reinterpret_cast<std::string*>(vals[0]));
        */
    }

    int numFrameArgs() override {
        RELEASE_ASSERT(0, "shouldn't serialize/deserialize non-concrete types?");
        return 2;
    }
};
static ValuedCompilerType<BoxedString*>* STR_CONSTANT = new StrConstantType();

CompilerVariable* makeStr(BoxedString* s) {
    return new ValuedCompilerVariable<BoxedString*>(STR_CONSTANT, s);
}

ConcreteCompilerType* typeFromClass(BoxedClass* c) {
    assert(c);
    return NormalObjectType::fromClass(c);
}

class BoolType : public ConcreteCompilerType {
public:
    std::string debugName() override { return "bool"; }
    llvm::Type* llvmType() override {
        if (BOOLS_AS_I64)
            return g.i64;
        else
            return g.i1;
    }

    bool isFitBy(BoxedClass* c) override { return false; }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        return var;
    }

    bool canConvertTo(CompilerType* other_type) override {
        return (other_type == UNKNOWN || other_type == BOXED_BOOL || other_type == BOOL);
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == BOOL) {
            return var;
        }

        ASSERT(other_type == UNKNOWN || other_type == BOXED_BOOL, "%s", other_type->debugName().c_str());
        llvm::Value* boxed = emitter.getBuilder()->CreateSelect(
            i1FromBool(emitter, var),
            emitter.setType(embedRelocatablePtr(True, g.llvm_value_type_ptr), RefType::BORROWED),
            emitter.setType(embedRelocatablePtr(False, g.llvm_value_type_ptr), RefType::BORROWED));
        emitter.setType(boxed, RefType::BORROWED);
        return new ConcreteCompilerVariable(other_type, boxed);
    }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override {
        return BOXED_BOOL->getattrType(attr, cls_only);
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr,
                              bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_BOOL);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, BoxedString* attr,
                               CallattrFlags flags, const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_BOOL);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, args, keyword_names);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        return rtn;
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        llvm::CallSite call
            = emitter.createCall(info.unw_info, g.funcs.raiseNotIterableError, embedConstantPtr("bool", g.i8_ptr));
        call.setDoesNotReturn();
        return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(BOOL->llvmType()));
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_BOOL; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        assert(llvmType() == g.i64);
        bool b = (bool)vals[0];
        return boxBool(b);
    }
};
ConcreteCompilerType* BOOL = new BoolType();
ConcreteCompilerVariable* makeBool(bool b) {
    return new ConcreteCompilerVariable(BOOL, llvm::ConstantInt::get(BOOL->llvmType(), b, false));
}

ConcreteCompilerVariable* doIs(IREmitter& emitter, CompilerVariable* lhs, CompilerVariable* rhs, bool negate) {
    // TODO: I think we can do better here and not force the types to box themselves

    ConcreteCompilerVariable* converted_left = lhs->makeConverted(emitter, UNKNOWN);
    ConcreteCompilerVariable* converted_right = rhs->makeConverted(emitter, UNKNOWN);
    llvm::Value* cmp;
    if (!negate)
        cmp = emitter.getBuilder()->CreateICmpEQ(converted_left->getValue(), converted_right->getValue());
    else
        cmp = emitter.getBuilder()->CreateICmpNE(converted_left->getValue(), converted_right->getValue());

    return boolFromI1(emitter, cmp);
}

ConcreteCompilerType* BOXED_TUPLE;
class TupleType : public UnboxedType<const std::vector<CompilerVariable*>, TupleType> {
private:
    std::string name;
    const std::vector<CompilerType*> elt_types;

    TupleType(const std::vector<CompilerType*>& elt_types) : elt_types(elt_types) {
        std::ostringstream os("");
        os << "tuple(";
        for (int i = 0; i < elt_types.size(); i++) {
            if (i)
                os << ", ";
            os << elt_types[i]->debugName();
        }
        os << ")";
        name = os.str();
    }

public:
    typedef const std::vector<CompilerVariable*> VEC;

    void _assertMatches(const VEC& v) {
        assert(v.size() == elt_types.size());

        for (int i = 0; i < v.size(); i++) {
            assert(v[i]->getType() == elt_types[i]);
        }
    }

    std::string debugName() override { return name; }

    VEC _dup(const VEC& orig_elts, DupCache& cache) {
        std::vector<CompilerVariable*> elts;

        for (int i = 0; i < orig_elts.size(); i++) {
            elts.push_back(orig_elts[i]->dup(cache));
        }
        return elts;
    }

    ConcreteCompilerVariable* _makeConverted(IREmitter& emitter, const VEC& v, ConcreteCompilerType* other_type) {
        assert(other_type == UNKNOWN || other_type == BOXED_TUPLE);

        std::vector<ConcreteCompilerVariable*> converted_args;

        llvm::Value* nelts = llvm::ConstantInt::get(g.i64, v.size(), false);

        llvm::Value* _scratch = emitter.getScratch(v.size() * sizeof(void*));
        auto scratch = emitter.getBuilder()->CreateBitCast(_scratch, g.llvm_value_type_ptr->getPointerTo());

        llvm::SmallVector<llvm::Value*, 4> array_passed_args;

        // First, convert all the args, before putting any in the scratch.
        // Do it this way in case any of the conversions themselves need scratch space
        // (ie nested tuples).
        // TODO could probably do this better: create a scratch reservation that gets released
        // at some point, so that we know which scratch space is still in use, so that we can handle
        // multiple concurrent scratch users.
        for (int i = 0; i < v.size(); i++) {
            ConcreteCompilerVariable* converted = v[i]->makeConverted(emitter, v[i]->getBoxType());
            converted_args.push_back(converted);
        }

        for (int i = 0; i < v.size(); i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(scratch, i);
            emitter.getBuilder()->CreateStore(converted_args[i]->getValue(), ptr);
            array_passed_args.push_back(converted_args[i]->getValue());
        }

        llvm::Instruction* rtn = emitter.getBuilder()->CreateCall2(g.funcs.createTuple, nelts, scratch);
        emitter.setType(rtn, RefType::OWNED);

        for (auto v : array_passed_args)
            emitter.refUsed(v, rtn);

        return new ConcreteCompilerVariable(other_type, rtn);
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_TUPLE; }

    static TupleType* make(const std::vector<CompilerType*>& elt_types) { return new TupleType(elt_types); }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        assert(slice->getType() != UNBOXED_INT);
        if (slice->getType() == INT) {
            llvm::Value* v = IntType::extractInt(slice);
            assert(v->getType() == g.i64);
            if (llvm::ConstantInt* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
                int64_t i = ci->getSExtValue();
                auto v = var->getValue();
                const VEC* elts = &v->val;
                if (i >= 0 && i < elts->size()) {
                    CompilerVariable* rtn = (*elts)[i];
                    return rtn;
                } else if (i < 0 && -i <= elts->size()) {
                    CompilerVariable* rtn = (*elts)[elts->size() + i];
                    return rtn;
                } else {
                    ExceptionStyle target_exception_style = info.preferredExceptionStyle();

                    if (target_exception_style == CAPI) {
                        llvm::CallSite call
                            = emitter.createCall(info.unw_info, g.funcs.raiseIndexErrorStrCapi,
                                                 embedConstantPtr("tuple", g.i8_ptr), CAPI, IREmitter::ALWAYS_THROWS);
                    } else {
                        llvm::CallSite call = emitter.createCall(info.unw_info, g.funcs.raiseIndexErrorStr,
                                                                 embedConstantPtr("tuple", g.i8_ptr), CXX);
                        call.setDoesNotReturn();
                    }
                    return undefVariable();
                }
            }
        }

        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_TUPLE);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        return rtn;
    }

    CompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return makeInt(var->getValue()->val.size());
    }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override {
        return BOXED_TUPLE->getattrType(attr, cls_only);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        return rtn;
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        llvm::SmallVector<std::pair<llvm::BasicBlock*, llvm::Value*>, 4> phi_incoming;

        llvm::BasicBlock* end = emitter.createBasicBlock();

        ConcreteCompilerVariable* converted_lhs = lhs->makeConverted(emitter, lhs->getConcreteType());

        for (CompilerVariable* e : var->getValue()->val) {
            // TODO: we could potentially avoid the identity tests if we know that either type has
            // an __eq__ that is reflexive (returns True for the same object).
            {
                ConcreteCompilerVariable* is_same = doIs(emitter, converted_lhs, e, false);
                llvm::Value* raw = i1FromBool(emitter, is_same);

                phi_incoming.push_back(std::make_pair(emitter.currentBasicBlock(), getConstantInt(1, g.i1)));
                llvm::BasicBlock* new_bb = emitter.createBasicBlock();
                new_bb->moveAfter(emitter.currentBasicBlock());
                emitter.getBuilder()->CreateCondBr(raw, end, new_bb);
                emitter.setCurrentBasicBlock(new_bb);
            }

            {
                CompilerVariable* eq = converted_lhs->binexp(emitter, info, e, AST_TYPE::Eq, Compare);
                CompilerVariable* eq_nonzero = eq->nonzero(emitter, info);
                assert(eq_nonzero->getType() == BOOL);
                llvm::Value* raw = i1FromBool(emitter, static_cast<ConcreteCompilerVariable*>(eq_nonzero));

                phi_incoming.push_back(std::make_pair(emitter.currentBasicBlock(), getConstantInt(1, g.i1)));
                llvm::BasicBlock* new_bb = emitter.createBasicBlock();
                new_bb->moveAfter(emitter.currentBasicBlock());
                emitter.getBuilder()->CreateCondBr(raw, end, new_bb);
                emitter.setCurrentBasicBlock(new_bb);
            }
        }

        // TODO This last block is unnecessary:
        phi_incoming.push_back(std::make_pair(emitter.currentBasicBlock(), getConstantInt(0, g.i1)));
        emitter.getBuilder()->CreateBr(end);

        end->moveAfter(emitter.currentBasicBlock());
        emitter.setCurrentBasicBlock(end);

        auto phi = emitter.getBuilder()->CreatePHI(g.i1, phi_incoming.size());
        for (auto p : phi_incoming) {
            phi->addIncoming(p.second, p.first);
        }

        return boolFromI1(emitter, phi);
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CallattrFlags flags,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        return makeConverted(emitter, var, getConcreteType())
            ->callattr(emitter, info, attr, flags, args, keyword_names);
    }

    void _serializeToFrame(const VEC& val, std::vector<llvm::Value*>& stackmap_args) {
        for (auto elt : val) {
            elt->serializeToFrame(stackmap_args);
        }
    }

    Box* _deserializeFromFrame(const FrameVals& vals) {
        assert(vals.size() == _numFrameArgs());

        BoxedTuple* rtn = BoxedTuple::create(elt_types.size());
        int rtn_idx = 0;
        int cur_idx = 0;
        for (auto e : elt_types) {
            int num_args = e->numFrameArgs();
            // TODO: inefficient to make these copies
            FrameVals sub_vals(vals.begin() + cur_idx, vals.begin() + cur_idx + num_args);

            rtn->elts[rtn_idx++] = e->deserializeFromFrame(sub_vals);

            cur_idx += num_args;
        }
        assert(cur_idx == vals.size());
        return rtn;
    }

    int _numFrameArgs() {
        int rtn = 0;
        for (auto e : elt_types)
            rtn += e->numFrameArgs();
        return rtn;
    }

    std::vector<CompilerVariable*> unpack(IREmitter& emitter, const OpInfo& info, VAR* var, int num_into) override {
        if (num_into != elt_types.size()) {
            return ValuedCompilerType::unpack(emitter, info, var, num_into);
        }

        return var->getValue()->val;
    }

    std::vector<CompilerType*> unpackTypes(int num_into) override {
        if (num_into != elt_types.size()) {
            return ValuedCompilerType::unpackTypes(num_into);
        }

        return elt_types;
    }
};

CompilerType* makeTupleType(const std::vector<CompilerType*>& elt_types) {
    return TupleType::make(elt_types);
}

CompilerVariable* makeTuple(const std::vector<CompilerVariable*>& elts) {
    std::vector<CompilerType*> elt_types;
    for (int i = 0; i < elts.size(); i++) {
        elt_types.push_back(elts[i]->getType());
    }
    TupleType* type = TupleType::make(elt_types);

    auto alloc_var = std::make_shared<TupleType::Unboxed>(elts, nullptr);
    return new TupleType::VAR(type, alloc_var);
}

class UnboxedSliceType : public ValuedCompilerType<UnboxedSlice> {
public:
    std::string debugName() override { return "slice"; }

    void assertMatches(UnboxedSlice slice) override {}

    int numFrameArgs() override { RELEASE_ASSERT(0, "unboxed slice should never get serialized"); }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        RELEASE_ASSERT(0, "unboxed slice should never get serialized");
    }

    void serializeToFrame(VAR* v, std::vector<llvm::Value*>& stackmap_args) override {
        RELEASE_ASSERT(0, "unboxed slice should never get serialized");
    }

    ConcreteCompilerType* getConcreteType() override { return SLICE; }
    ConcreteCompilerType* getBoxType() override { return SLICE; }

    bool canConvertTo(CompilerType* other) override { return other == this || other == SLICE || other == UNKNOWN; }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        assert(other_type == SLICE || other_type == UNKNOWN);

        auto slice = var->getValue();

        ConcreteCompilerVariable* cstart, *cstop, *cstep;
        cstart = slice.start ? slice.start->makeConverted(emitter, slice.start->getBoxType()) : emitter.getNone();
        cstop = slice.stop ? slice.stop->makeConverted(emitter, slice.stop->getBoxType()) : emitter.getNone();
        cstep = slice.step ? slice.step->makeConverted(emitter, slice.step->getBoxType()) : emitter.getNone();

        std::vector<llvm::Value*> args;
        args.push_back(cstart->getValue());
        args.push_back(cstop->getValue());
        args.push_back(cstep->getValue());
        llvm::Value* rtn = emitter.getBuilder()->CreateCall(g.funcs.createSlice, args);
        emitter.setType(rtn, RefType::OWNED);

        return new ConcreteCompilerVariable(SLICE, rtn);
    }
} _UNBOXED_SLICE;
CompilerType* UNBOXED_SLICE = &_UNBOXED_SLICE;

CompilerVariable* makeSlice(CompilerVariable* start, CompilerVariable* stop, CompilerVariable* step) {
    return new UnboxedSliceType::VAR(&_UNBOXED_SLICE, UnboxedSlice{ start, stop, step });
}

UnboxedSlice extractSlice(CompilerVariable* slice) {
    assert(slice->getType() == UNBOXED_SLICE);
    return static_cast<UnboxedSliceType::VAR*>(slice)->getValue();
}

class UndefType : public ConcreteCompilerType {
public:
    std::string debugName() override { return "undefType"; }

    llvm::Type* llvmType() override {
        // Something that no one else uses...
        // TODO should do something more rare like a unique custom struct
        return llvm::Type::getInt16Ty(g.context);
    }

    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, VAR* var, ArgPassSpec argspec,
                           const std::vector<CompilerVariable*>& args,
                           const std::vector<BoxedString*>* keyword_names) override {
        return undefVariable();
    }
    CompilerVariable* dup(VAR* v, DupCache& cache) override {
        // TODO copied from UnknownType
        auto& rtn = cache[v];
        if (rtn == NULL) {
            rtn = new VAR(this, v->getValue());
        }
        return rtn;
    }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        if (other_type == other_type->getBoxType()) {
            assert(other_type == UNKNOWN);
            return emitter.getNone()->makeConverted(emitter, other_type);
        }
        llvm::Value* v = llvm::UndefValue::get(other_type->llvmType());
        return new ConcreteCompilerVariable(other_type, v);
    }
    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr,
                              bool cls_only) override {
        return undefVariable();
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, BoxedString* attr, CallattrFlags flags,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<BoxedString*>* keyword_names) override {
        return undefVariable();
    }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<llvm::StringRef>* keyword_names) override {
        return UNDEF;
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(BOOL->llvmType()));
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        return undefVariable();
    }

    CompilerVariable* contains(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* lhs) override {
        return boolFromI1(emitter, llvm::UndefValue::get(g.i1));
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              CompilerVariable* slice) override {
        return undefVariable();
    }

    ConcreteCompilerType* getBoxType() override { return UNKNOWN; }

    ConcreteCompilerType* getConcreteType() override { return this; }

    CompilerType* getattrType(BoxedString* attr, bool cls_only) override { return UNDEF; }

    bool canConvertTo(CompilerType* other_type) override { return true; }

    BoxedClass* guaranteedClass() override { return NULL; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        abort();
    }
} _UNDEF;
CompilerType* UNDEF = &_UNDEF;

ConcreteCompilerVariable* undefVariable() {
    return new ConcreteCompilerVariable(&_UNDEF, llvm::UndefValue::get(_UNDEF.llvmType()));
}

ConcreteCompilerVariable* boolFromI1(IREmitter& emitter, llvm::Value* v) {
    if (BOOLS_AS_I64) {
        assert(v->getType() == g.i1);
        assert(BOOL->llvmType() == g.i64);
        llvm::Value* v2 = emitter.getBuilder()->CreateZExt(v, BOOL->llvmType());
        return new ConcreteCompilerVariable(BOOL, v2);
    } else {
        return new ConcreteCompilerVariable(BOOL, v);
    }
}

llvm::Value* i1FromBool(IREmitter& emitter, ConcreteCompilerVariable* v) {
    if (BOOLS_AS_I64) {
        assert(v->getType() == BOOL);
        assert(BOOL->llvmType() == g.i64);
        llvm::Value* v2 = emitter.getBuilder()->CreateTrunc(v->getValue(), g.i1);
        return v2;
    } else {
        return v->getValue();
    }
}


ConcreteCompilerType* LIST, *SLICE, *MODULE, *DICT, *SET, *FROZENSET, *LONG, *BOXED_COMPLEX;

} // namespace pyston
