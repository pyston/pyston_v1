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

#include "codegen/compvars.h"

#include <cstdio>
#include <sstream>

#include "llvm/IR/IntrinsicInst.h"
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

namespace pyston {

static const std::string& iter_str = "__iter__";
static const std::string& hasnext_str = "__hasnext__";

CompilerType* CompilerType::getPystonIterType() {
    if (hasattr(&iter_str) == Yes) {
        CompilerType* iter_type = getattrType(&iter_str, true)->callType(ArgPassSpec(0), {}, NULL);
        if (iter_type->hasattr(&hasnext_str) == Yes)
            return iter_type;
        // if iter_type->hasattr(&hasnext_str) == No we know this is going to be a BoxedIterWrapper
        // we could optimize this case but it looks like this is very uncommon
    }
    return UNKNOWN;
}

CompilerType::Result CompilerType::hasattr(const std::string* attr) {
    CompilerType* type = getattrType(attr, true);
    if (type == UNKNOWN)
        return Result::Maybe;
    else if (type == UNDEF)
        return Result::No;
    return Result::Yes;
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
    CompilerVariable* obj, *func;

    RawInstanceMethod(CompilerVariable* obj, CompilerVariable* func) : obj(obj), func(func) {}
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
        InstanceMethodType* rtn = made[std::make_pair(obj_type, function_type)];
        if (rtn == NULL)
            rtn = new InstanceMethodType(obj_type, function_type);
        return rtn;
    }

    static CompilerVariable* makeIM(CompilerVariable* obj, CompilerVariable* func) {
        CompilerVariable* rtn = new ValuedCompilerVariable<RawInstanceMethod*>(
            InstanceMethodType::get(obj->getType(), func->getType()), new RawInstanceMethod(obj, func), true);
        obj->incvref();
        func->incvref();
        return rtn;
    }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<const std::string*>* keyword_names) override {
        std::vector<CompilerType*> new_args(arg_types);
        new_args.insert(new_args.begin(), obj_type);

        ArgPassSpec new_argspec(argspec.num_args + 1u, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
        return function_type->callType(new_argspec, new_args, keyword_names);
    }

    std::string debugName() override {
        return "instanceMethod(" + obj_type->debugName() + " ; " + function_type->debugName() + ")";
    }

    void drop(IREmitter& emitter, VAR* var) override {
        checkVar(var);
        RawInstanceMethod* val = var->getValue();
        val->obj->decvref(emitter);
        val->func->decvref(emitter);
        delete val;
    }

    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ValuedCompilerVariable<RawInstanceMethod*>* var,
                           ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                           const std::vector<const std::string*>* keyword_names) override {
        std::vector<CompilerVariable*> new_args;
        new_args.push_back(var->getValue()->obj);
        new_args.insert(new_args.end(), args.begin(), args.end());

        ArgPassSpec new_argspec(argspec.num_args + 1u, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
        return var->getValue()->func->call(emitter, info, new_argspec, new_args, keyword_names);
    }

    bool canConvertTo(ConcreteCompilerType* other_type) override { return other_type == UNKNOWN; }
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

        llvm::Value* boxed
            = emitter.getBuilder()->CreateCall2(g.funcs.boxInstanceMethod, obj->getValue(), func->getValue());

        obj->decvref(emitter);
        func->decvref(emitter);

        return new ConcreteCompilerVariable(other_type, boxed, true);
    }
    CompilerVariable* dup(VAR* var, DupCache& cache) override {
        checkVar(var);

        CompilerVariable* rtn = cache[var];
        if (rtn == NULL) {
            RawInstanceMethod* im = var->getValue();
            RawInstanceMethod* new_im = new RawInstanceMethod(im->obj->dup(cache), im->func->dup(cache));
            rtn = new VAR(this, new_im, var->isGrabbed());
            while (rtn->getVrefs() < var->getVrefs())
                rtn->incvref();
        }
        return rtn;
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override {
        var->getValue()->obj->serializeToFrame(stackmap_args);
        var->getValue()->func->serializeToFrame(stackmap_args);
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());
        abort();
    }

    int numFrameArgs() override { return obj_type->numFrameArgs() + function_type->numFrameArgs(); }
};
std::unordered_map<std::pair<CompilerType*, CompilerType*>, InstanceMethodType*> InstanceMethodType::made;

ConcreteCompilerVariable* ConcreteCompilerType::makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                              ConcreteCompilerType* other_type) {
    if (other_type == this) {
        var->incvref();
        return var;
    }
    printf("makeConverted not defined for %s\n", debugName().c_str());
    abort();
}
CompilerVariable* ConcreteCompilerType::dup(ConcreteCompilerVariable* v, DupCache& cache) {
    auto& rtn = cache[v];
    if (rtn == NULL) {
        rtn = new ConcreteCompilerVariable(this, v->getValue(), v->isGrabbed());
        while (rtn->getVrefs() < v->getVrefs())
            rtn->incvref();
    }
    return rtn;
}

class UnknownType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_value_type_ptr; }

    std::string debugName() override { return "AnyBox"; }

    void drop(IREmitter& emitter, VAR* var) override { emitter.getGC()->dropPointer(emitter, var->getValue()); }
    void grab(IREmitter& emitter, VAR* var) override { emitter.getGC()->grabPointer(emitter, var->getValue()); }

    bool isFitBy(BoxedClass* c) override { return true; }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) override;
    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, ArgPassSpec argspec,
                           const std::vector<CompilerVariable*>& args,
                           const std::vector<const std::string*>* keyword_names) override;
    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                               const std::string* attr, CallattrFlags flags, ArgPassSpec argspec,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override;
    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override;
    ConcreteCompilerVariable* hasnext(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override;

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, const std::string* attr,
                 CompilerVariable* v) override {
        llvm::Constant* ptr = getStringConstantPtr(*attr + '\0');
        ConcreteCompilerVariable* converted = v->makeConverted(emitter, UNKNOWN);
        // g.funcs.setattr->dump();
        // var->getValue()->dump(); llvm::errs() << '\n';
        // ptr->dump(); llvm::errs() << '\n';
        // converted->getValue()->dump(); llvm::errs() << '\n';
        bool do_patchpoint = ENABLE_ICSETATTRS && !info.isInterpreted();
        if (do_patchpoint) {
            ICSetupInfo* pp = createSetattrIC(info.getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(ptr);
            llvm_args.push_back(converted->getValue());

            emitter.createIC(pp, (void*)pyston::setattr, llvm_args, info.unw_info);
        } else {
            emitter.createCall3(info.unw_info, g.funcs.setattr, var->getValue(), ptr, converted->getValue());
        }
        converted->decvref(emitter);
    }

    void delattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                 const std::string* attr) override {
        llvm::Constant* ptr = getStringConstantPtr(*attr + '\0');

        // TODO
        // bool do_patchpoint = ENABLE_ICDELATTRS && !info.isInterpreted();
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
        assert(cls_value->getType() == g.llvm_class_type_ptr);
        llvm::Value* rtn = emitter.getBuilder()->CreateICmpEQ(cls_value, embedConstantPtr(cls, g.llvm_class_type_ptr));
        return rtn;
    }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override { return UNKNOWN; }
    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<const std::string*>* keyword_names) override {
        return UNKNOWN;
    }
    BoxedClass* guaranteedClass() override { return NULL; }
    ConcreteCompilerType* getBoxType() override { return this; }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == this) {
            var->incvref();
            return var;
        }
        fprintf(stderr, "Can't convert unknown to %s...\n", other_type->debugName().c_str());
        abort();
    }

    ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        bool do_patchpoint = ENABLE_ICGENERICS && !info.isInterpreted();
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
        return new ConcreteCompilerVariable(INT, rtn, true);
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        bool do_patchpoint = ENABLE_ICGETITEMS && !info.isInterpreted();
        llvm::Value* rtn;
        if (do_patchpoint) {
            ICSetupInfo* pp = createGetitemIC(info.getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(converted_slice->getValue());

            llvm::Value* uncasted = emitter.createIC(pp, (void*)pyston::getitem, llvm_args, info.unw_info);
            rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
        } else {
            rtn = emitter.createCall2(info.unw_info, g.funcs.getitem, var->getValue(), converted_slice->getValue());
        }

        converted_slice->decvref(emitter);
        return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
    }

    CompilerVariable* getPystonIter(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        CallattrFlags flags = {.cls_only = true, .null_on_nonexistent = true };
        CompilerVariable* iter_call = var->callattr(emitter, info, &iter_str, flags, ArgPassSpec(0), {}, 0);
        ConcreteCompilerVariable* converted_iter_call = iter_call->makeConverted(emitter, iter_call->getBoxType());

        // If the type analysis could determine the iter type is a valid pyston iter (has 'hasnext') we are finished.
        CompilerType* iter_type = var->getType()->getPystonIterType();
        if (iter_type != UNKNOWN) {
            iter_call->decvref(emitter);
            return converted_iter_call;
        }

        // We don't know the type so we have to check at runtime if __iter__ is implemented
        llvm::Value* cmp = emitter.getBuilder()->CreateICmpNE(converted_iter_call->getValue(),
                                                              embedConstantPtr(0, g.llvm_value_type_ptr));

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
        llvm::BasicBlock* value_has_iter_bb = emitter.currentBasicBlock();
        emitter.getBuilder()->CreateBr(bb_join);

        // var has no __iter__()
        // TODO: we could create a patchpoint if this turns out to be hot
        emitter.setCurrentBasicBlock(bb_no_iter);
        llvm::Value* value_no_iter = emitter.createCall(info.unw_info, g.funcs.getiterHelper, var->getValue());
        llvm::BasicBlock* value_no_iter_bb = emitter.currentBasicBlock();
        emitter.getBuilder()->CreateBr(bb_join);

        // join
        emitter.setCurrentBasicBlock(bb_join);
        auto phi = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, 2, "iter");
        phi->addIncoming(value_has_iter, value_has_iter_bb);
        phi->addIncoming(value_no_iter, value_no_iter_bb);

        converted_iter_call->decvref(emitter);
        iter_call->decvref(emitter);

        return new ConcreteCompilerVariable(UNKNOWN, phi, true);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted_rhs = rhs->makeConverted(emitter, rhs->getBoxType());

        llvm::Value* rtn;
        bool do_patchpoint = ENABLE_ICBINEXPS && !info.isInterpreted();

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
            ICSetupInfo* pp = createBinexpIC(info.getTypeRecorder());

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

        converted_rhs->decvref(emitter);

        if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn || op_type == AST_TYPE::Is
            || op_type == AST_TYPE::IsNot) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, rtn);
            return boolFromI1(emitter, unboxed);
        }

        return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        return reinterpret_cast<Box*>(vals[0]);
    }

    std::vector<CompilerVariable*> unpack(IREmitter& emitter, const OpInfo& info, VAR* var, int num_into) override {
        llvm::Value* unpacked = emitter.createCall2(info.unw_info, g.funcs.unpackIntoArray, var->getValue(),
                                                    getConstantInt(num_into, g.i64));
        assert(unpacked->getType() == g.llvm_value_type_ptr->getPointerTo());

        std::vector<CompilerVariable*> rtn;
        for (int i = 0; i < num_into; i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(unpacked, i);
            llvm::Value* val = emitter.getBuilder()->CreateLoad(ptr);
            assert(val->getType() == g.llvm_value_type_ptr);

            rtn.push_back(new ConcreteCompilerVariable(UNKNOWN, val, true));
        }
        return rtn;
    }
};

ConcreteCompilerType* UNKNOWN = new UnknownType();

CompilerVariable* UnknownType::getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                       const std::string* attr, bool cls_only) {
    llvm::Constant* ptr = getStringConstantPtr(*attr + '\0');

    llvm::Value* rtn_val = NULL;

    llvm::Value* llvm_func;
    void* raw_func;
    if (cls_only) {
        llvm_func = g.funcs.getclsattr;
        raw_func = (void*)pyston::getclsattr;
    } else {
        llvm_func = g.funcs.getattr;
        raw_func = (void*)pyston::getattr;
    }

    bool do_patchpoint = ENABLE_ICGETATTRS && !info.isInterpreted();
    if (do_patchpoint) {
        ICSetupInfo* pp = createGetattrIC(info.getTypeRecorder());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());
        llvm_args.push_back(ptr);

        llvm::Value* uncasted = emitter.createIC(pp, raw_func, llvm_args, info.unw_info);
        rtn_val = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        rtn_val = emitter.createCall2(info.unw_info, llvm_func, var->getValue(), ptr);
    }
    return new ConcreteCompilerVariable(UNKNOWN, rtn_val, true);
}

static ConcreteCompilerVariable* _call(IREmitter& emitter, const OpInfo& info, llvm::Value* func, void* func_addr,
                                       const std::vector<llvm::Value*>& other_args, ArgPassSpec argspec,
                                       const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names,
                                       ConcreteCompilerType* rtn_type) {
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
        llvm_args.push_back(embedConstantPtr(NULL, g.llvm_value_type_ptr));
    }

    if (args.size() >= 2) {
        llvm_args.push_back(converted_args[1]->getValue());
    } else if (pass_keyword_names) {
        llvm_args.push_back(embedConstantPtr(NULL, g.llvm_value_type_ptr));
    }

    if (args.size() >= 3) {
        llvm_args.push_back(converted_args[2]->getValue());
    } else if (pass_keyword_names) {
        llvm_args.push_back(embedConstantPtr(NULL, g.llvm_value_type_ptr));
    }

    llvm::Value* mallocsave = NULL;
    if (args.size() >= 4) {
        llvm::Value* arg_array;

        if (info.isInterpreted()) {
            llvm::Value* n_bytes = getConstantInt((args.size() - 3) * sizeof(Box*), g.i64);
            mallocsave = emitter.getBuilder()->CreateCall(g.funcs.malloc, n_bytes);
            arg_array = emitter.getBuilder()->CreateBitCast(mallocsave, g.llvm_value_type_ptr->getPointerTo());
        } else {
            llvm::Value* n_varargs = getConstantInt(args.size() - 3, g.i64);

            // Don't use the IRBuilder since we want to specifically put this in the entry block so it only gets called
            // once.
            // TODO we could take this further and use the same alloca for all function calls?
            llvm::Instruction* insertion_point = emitter.currentFunction()->func->getEntryBlock().getFirstInsertionPt();
            arg_array = new llvm::AllocaInst(g.llvm_value_type_ptr, n_varargs, "arg_scratch", insertion_point);
        }

        for (int i = 3; i < args.size(); i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, i - 3);
            emitter.getBuilder()->CreateStore(converted_args[i]->getValue(), ptr);
        }
        llvm_args.push_back(arg_array);

        if (pass_keyword_names)
            llvm_args.push_back(embedConstantPtr(keyword_names, g.vector_ptr));
    } else if (pass_keyword_names) {
        llvm_args.push_back(embedConstantPtr(NULL, g.llvm_value_type_ptr->getPointerTo()));
        llvm_args.push_back(embedConstantPtr(keyword_names, g.vector_ptr));
    }

    // f->dump();
    // for (int i = 0; i < llvm_args.size(); i++) {
    // llvm_args[i]->dump();
    // llvm::errs() << '\n';
    //}

    llvm::Value* rtn;

    // func->dump();
    // for (auto a : llvm_args)
    // a->dump();

    bool do_patchpoint = ENABLE_ICCALLSITES && !info.isInterpreted()
                         && (func_addr == runtimeCall || func_addr == pyston::callattr);
    if (do_patchpoint) {
        assert(func_addr);

        ICSetupInfo* pp = createCallsiteIC(info.getTypeRecorder(), args.size());

        llvm::Value* uncasted = emitter.createIC(pp, func_addr, llvm_args, info.unw_info);

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
        rtn = emitter.createCall(info.unw_info, func, llvm_args);
    }

    if (mallocsave) {
        llvm::Value* l_free = embedConstantPtr(
            (void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
        emitter.getBuilder()->CreateCall(l_free, mallocsave);
    }

    for (int i = 0; i < args.size(); i++) {
        converted_args[i]->decvref(emitter);
    }

    assert(rtn->getType() == rtn_type->llvmType());
    return new ConcreteCompilerVariable(rtn_type, rtn, true);
}

CompilerVariable* UnknownType::call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                    ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                    const std::vector<const std::string*>* keyword_names) {
    bool pass_keywords = (argspec.num_keywords != 0);
    int npassed_args = argspec.totalPassed();

    llvm::Value* func;
    if (pass_keywords)
        func = g.funcs.runtimeCall;
    else if (npassed_args == 0)
        func = g.funcs.runtimeCall0;
    else if (npassed_args == 1)
        func = g.funcs.runtimeCall1;
    else if (npassed_args == 2)
        func = g.funcs.runtimeCall2;
    else if (npassed_args == 3)
        func = g.funcs.runtimeCall3;
    else
        func = g.funcs.runtimeCallN;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());

    llvm::Value* llvm_argspec = llvm::ConstantInt::get(g.i32, argspec.asInt(), false);
    other_args.push_back(llvm_argspec);
    return _call(emitter, info, func, (void*)runtimeCall, other_args, argspec, args, keyword_names, UNKNOWN);
}

CompilerVariable* UnknownType::callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                        const std::string* attr, CallattrFlags flags, ArgPassSpec argspec,
                                        const std::vector<CompilerVariable*>& args,
                                        const std::vector<const std::string*>* keyword_names) {
    bool pass_keywords = (argspec.num_keywords != 0);
    int npassed_args = argspec.totalPassed();

    llvm::Value* func;
    if (pass_keywords)
        func = g.funcs.callattr;
    else if (npassed_args == 0)
        func = g.funcs.callattr0;
    else if (npassed_args == 1)
        func = g.funcs.callattr1;
    else if (npassed_args == 2)
        func = g.funcs.callattr2;
    else if (npassed_args == 3)
        func = g.funcs.callattr3;
    else
        func = g.funcs.callattrN;

    union {
        CallattrFlags flags;
        char value;
    } flags_to_int;
    static_assert(sizeof(CallattrFlags) == sizeof(char), "");
    flags_to_int.flags = flags;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());
    other_args.push_back(embedConstantPtr(attr, g.llvm_str_type_ptr));
    other_args.push_back(getConstantInt(flags_to_int.value, g.i8));

    llvm::Value* llvm_argspec = llvm::ConstantInt::get(g.i32, argspec.asInt(), false);
    other_args.push_back(llvm_argspec);
    return _call(emitter, info, func, (void*)pyston::callattr, other_args, argspec, args, keyword_names, UNKNOWN);
}

ConcreteCompilerVariable* UnknownType::nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
    bool do_patchpoint = ENABLE_ICNONZEROS && !info.isInterpreted();
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

ConcreteCompilerVariable* UnknownType::hasnext(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
    bool do_patchpoint = ENABLE_ICS && !info.isInterpreted();
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

CompilerVariable* makeFunction(IREmitter& emitter, CLFunction* f, CompilerVariable* closure, bool isGenerator,
                               const std::vector<ConcreteCompilerVariable*>& defaults) {
    // Unlike the CLFunction*, which can be shared between recompilations, the Box* around it
    // should be created anew every time the functiondef is encountered

    llvm::Value* closure_v;
    ConcreteCompilerVariable* convertedClosure = NULL;
    if (closure) {
        convertedClosure = closure->makeConverted(emitter, closure->getConcreteType());
        closure_v = convertedClosure->getValue();
    } else {
        closure_v = embedConstantPtr(nullptr, g.llvm_closure_type_ptr);
    }

    llvm::Value* scratch;
    if (defaults.size()) {
        scratch = emitter.getScratch(defaults.size() * sizeof(Box*));
        scratch = emitter.getBuilder()->CreateBitCast(scratch, g.llvm_value_type_ptr_ptr);
        int i = 0;
        for (auto d : defaults) {
            llvm::Value* v = d->getValue();
            llvm::Value* p = emitter.getBuilder()->CreateConstGEP1_32(scratch, i);
            emitter.getBuilder()->CreateStore(v, p);
            i++;
        }
    } else {
        scratch = embedConstantPtr(nullptr, g.llvm_value_type_ptr_ptr);
    }

    llvm::Value* isGenerator_v = llvm::ConstantInt::get(g.i1, isGenerator, false);

    // We know this function call can't throw, so it's safe to use emitter.getBuilder()->CreateCall() rather than
    // emitter.createCall().
    llvm::Value* boxed = emitter.getBuilder()->CreateCall(
        g.funcs.boxCLFunction,
        std::vector<llvm::Value*>{ embedConstantPtr(f, g.llvm_clfunction_type_ptr), closure_v, isGenerator_v, scratch,
                                   getConstantInt(defaults.size(), g.i64) });

    if (convertedClosure)
        convertedClosure->decvref(emitter);
    return new ConcreteCompilerVariable(typeFromClass(function_cls), boxed, true);
}

class AbstractFunctionType : public CompilerType {
public:
    struct Sig {
        std::vector<ConcreteCompilerType*> arg_types;
        CompilerType* rtn_type;
        int ndefaults;
    };

private:
    std::vector<Sig*> sigs;
    AbstractFunctionType(const std::vector<Sig*>& sigs) : sigs(sigs) {}

public:
    std::string debugName() override { return "<AbstractFunctionType>"; }

    ConcreteCompilerType* getConcreteType() override { return UNKNOWN; }

    ConcreteCompilerType* getBoxType() override { return UNKNOWN; }

    bool canConvertTo(ConcreteCompilerType* other_type) override { return other_type == UNKNOWN; }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override { return UNDEF; }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<const std::string*>* keyword_names) override {
        RELEASE_ASSERT(!argspec.has_starargs, "");
        RELEASE_ASSERT(!argspec.has_kwargs, "");
        RELEASE_ASSERT(argspec.num_keywords == 0, "");

        for (int i = 0; i < sigs.size(); i++) {
            Sig* sig = sigs[i];
            if (arg_types.size() < sig->arg_types.size() - sig->ndefaults || arg_types.size() > sig->arg_types.size())
                continue;

            bool works = true;
            for (int j = 0; j < arg_types.size(); j++) {
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
        CLFunction* clf = rtfunc->f;

        for (int i = 0; i < clf->versions.size(); i++) {
            CompiledFunction* cf = clf->versions[i];

            FunctionSpecialization* fspec = cf->spec;

            Sig* type_sig = new Sig();
            type_sig->rtn_type = fspec->rtn_type;
            type_sig->ndefaults = clf->num_defaults;

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

class IntType : public ConcreteCompilerType {
public:
    IntType() {}

    llvm::Type* llvmType() override { return g.i64; }

    bool isFitBy(BoxedClass* c) override { return false; }

    void drop(IREmitter& emitter, ConcreteCompilerVariable* var) override {
        // pass
    }
    void grab(IREmitter& emitter, ConcreteCompilerVariable* var) override {
        // pass
    }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override {
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
            int__float_sig->rtn_type = FLOAT;
            int__float_sig->arg_types.push_back(FLOAT);
            sigs.push_back(int__float_sig);

            AbstractFunctionType::Sig* unknown_sig = new AbstractFunctionType::Sig();
            unknown_sig->rtn_type = UNKNOWN;
            unknown_sig->arg_types.push_back(UNKNOWN);
            sigs.push_back(unknown_sig);
        }

        // we can handle those operations when the rhs is a float
        if (*attr == "__add__" || *attr == "__sub__" || *attr == "__mul__" || *attr == "__div__" || *attr == "__pow__"
            || *attr == "__floordiv__" || *attr == "__mod__" || *attr == "__pow__") {
            return AbstractFunctionType::get(sigs);
        }
        return BOXED_INT->getattrType(attr, cls_only);
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                               const std::string* attr, CallattrFlags flags, ArgPassSpec argspec,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    void setattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                 CompilerVariable* v) override {
        llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                  getStringConstantPtr("int\0"), getStringConstantPtr(*attr + '\0'));
        call.setDoesNotReturn();
    }

    void delattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr) override {
        llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                  getStringConstantPtr("int\0"), getStringConstantPtr(*attr + '\0'));
        call.setDoesNotReturn();
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == this) {
            var->incvref();
            return var;
        } else if (other_type == UNKNOWN || other_type == BOXED_INT) {
            llvm::Value* boxed = emitter.getBuilder()->CreateCall(g.funcs.boxInt, var->getValue());
            return new ConcreteCompilerVariable(other_type, boxed, true);
        } else {
            printf("Don't know how to convert i64 to %s\n", other_type->debugName().c_str());
            abort();
        }
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        converted->decvref(emitter);
        return rtn;
    }

    ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        llvm::CallSite call
            = emitter.createCall(info.unw_info, g.funcs.raiseNotIterableError, getStringConstantPtr("int"));
        call.setDoesNotReturn();
        return new ConcreteCompilerVariable(INT, llvm::UndefValue::get(g.i64), true);
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        llvm::Value* cmp = emitter.getBuilder()->CreateICmpNE(var->getValue(), llvm::ConstantInt::get(g.i64, 0, false));
        return boolFromI1(emitter, cmp);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        bool can_lower = (rhs->getType() == INT && exp_type == Compare);
        if (!can_lower) {
            // if the rhs is a float convert the lhs to a float and do the operation on it.
            if (rhs->getType() == FLOAT) {
                if (op_type == AST_TYPE::IsNot || op_type == AST_TYPE::Is)
                    return makeBool(op_type == AST_TYPE::IsNot);

                ConcreteCompilerVariable* converted_left = var->makeConverted(emitter, INT);
                llvm::Value* conv = emitter.getBuilder()->CreateSIToFP(converted_left->getValue(), g.double_);
                converted_left->decvref(emitter);
                converted_left = new ConcreteCompilerVariable(FLOAT, conv, true);
                return converted_left->binexp(emitter, info, rhs, op_type, exp_type);
            }

            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
            converted->decvref(emitter);
            return rtn;
        }

        ConcreteCompilerVariable* converted_right = rhs->makeConverted(emitter, INT);
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
                    ASSERT(0, "%s", getOpName(op_type).c_str());
                    abort();
                    break;
            }
            v = emitter.getBuilder()->CreateICmp(cmp_pred, var->getValue(), converted_right->getValue());
        }
        converted_right->decvref(emitter);
        if (v->getType() == g.i64) {
            return new ConcreteCompilerVariable(INT, v, true);
        } else {
            return boolFromI1(emitter, v);
        }
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_INT; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);

        return boxInt(vals[0]);
    }
} _INT;
ConcreteCompilerType* INT = &_INT;

ConcreteCompilerVariable* makeInt(int64_t n) {
    return new ConcreteCompilerVariable(INT, llvm::ConstantInt::get(g.i64, n, true), true);
}

class FloatType : public ConcreteCompilerType {
public:
    FloatType() {}

    llvm::Type* llvmType() override { return g.double_; }

    bool isFitBy(BoxedClass* c) override { return false; }

    void drop(IREmitter& emitter, ConcreteCompilerVariable* var) override {
        // pass
    }
    void grab(IREmitter& emitter, ConcreteCompilerVariable* var) override {
        // pass
    }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override {
        static std::vector<AbstractFunctionType::Sig*> sigs;
        if (sigs.size() == 0) {
            AbstractFunctionType::Sig* float_sig = new AbstractFunctionType::Sig();
            float_sig->rtn_type = FLOAT;
            float_sig->arg_types.push_back(FLOAT);
            sigs.push_back(float_sig);

            AbstractFunctionType::Sig* int_sig = new AbstractFunctionType::Sig();
            int_sig->rtn_type = FLOAT;
            int_sig->arg_types.push_back(INT);
            sigs.push_back(int_sig);

            AbstractFunctionType::Sig* unknown_sig = new AbstractFunctionType::Sig();
            unknown_sig->rtn_type = UNKNOWN;
            unknown_sig->arg_types.push_back(UNKNOWN);
            sigs.push_back(unknown_sig);
        }

        if (*attr == "__add__" || *attr == "__sub__" || *attr == "__mul__" || *attr == "__div__" || *attr == "__pow__"
            || *attr == "__floordiv__" || *attr == "__mod__" || *attr == "__pow__") {
            return AbstractFunctionType::get(sigs);
        }

        if (*attr == "__iadd__" || *attr == "__isub__" || *attr == "__imul__" || *attr == "__idiv__"
            || *attr == "__ipow__" || *attr == "__ifloordiv__" || *attr == "__imod__" || *attr == "__ipow__") {
            return AbstractFunctionType::get(sigs);
        }

        return BOXED_FLOAT->getattrType(attr, cls_only);
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                               const std::string* attr, CallattrFlags flags, ArgPassSpec argspec,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    void setattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                 CompilerVariable* v) override {
        llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                  getStringConstantPtr("float\0"), getStringConstantPtr(*attr + '\0'));
        call.setDoesNotReturn();
    }

    void delattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr) override {
        llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                  getStringConstantPtr("float\0"), getStringConstantPtr(*attr + '\0'));
        call.setDoesNotReturn();
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == this) {
            var->incvref();
            return var;
        } else if (other_type == UNKNOWN || other_type == BOXED_FLOAT) {
            llvm::Value* boxed = emitter.getBuilder()->CreateCall(g.funcs.boxFloat, var->getValue());
            return new ConcreteCompilerVariable(other_type, boxed, true);
        } else {
            printf("Don't know how to convert float to %s\n", other_type->debugName().c_str());
            abort();
        }
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        llvm::Value* cmp = emitter.getBuilder()->CreateFCmpUNE(var->getValue(), llvm::ConstantFP::get(g.double_, 0));
        return boolFromI1(emitter, cmp);
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        if (rhs->getType() != INT && rhs->getType() != FLOAT) {
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
            CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
            converted->decvref(emitter);
            return rtn;
        }

        ConcreteCompilerVariable* converted_right;
        if (rhs->getType() == FLOAT) {
            converted_right = rhs->makeConverted(emitter, FLOAT);
        } else {
            if (op_type == AST_TYPE::IsNot || op_type == AST_TYPE::Is)
                return makeBool(op_type == AST_TYPE::IsNot);

            converted_right = rhs->makeConverted(emitter, INT);
            llvm::Value* conv = emitter.getBuilder()->CreateSIToFP(converted_right->getValue(), g.double_);
            converted_right->decvref(emitter);
            converted_right = new ConcreteCompilerVariable(FLOAT, conv, true);
        }

        llvm::Value* v;
        bool succeeded = true;
        if (op_type == AST_TYPE::Mod) {
            v = emitter.createCall2(info.unw_info, g.funcs.mod_float_float, var->getValue(),
                                    converted_right->getValue());
        } else if (op_type == AST_TYPE::Div || op_type == AST_TYPE::TrueDiv) {
            v = emitter.createCall2(info.unw_info, g.funcs.div_float_float, var->getValue(),
                                    converted_right->getValue());
        } else if (op_type == AST_TYPE::FloorDiv) {
            v = emitter.createCall2(info.unw_info, g.funcs.floordiv_float_float, var->getValue(),
                                    converted_right->getValue());
        } else if (op_type == AST_TYPE::Pow) {
            v = emitter.createCall2(info.unw_info, g.funcs.pow_float_float, var->getValue(),
                                    converted_right->getValue());
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
                    ASSERT(0, "%s", getOpName(op_type).c_str());
                    abort();
                    break;
            }

            if (succeeded) {
                v = emitter.getBuilder()->CreateBinOp(binopcode, var->getValue(), converted_right->getValue());
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
                    ASSERT(0, "%s", getOpName(op_type).c_str());
                    abort();
                    break;
            }
            v = emitter.getBuilder()->CreateFCmp(cmp_pred, var->getValue(), converted_right->getValue());
        }
        converted_right->decvref(emitter);

        if (succeeded) {
            if (v->getType() == g.double_) {
                return new ConcreteCompilerVariable(FLOAT, v, true);
            } else {
                return boolFromI1(emitter, v);
            }
        }

        // TODO duplication with top of function, other functions, etc
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        converted->decvref(emitter);
        return rtn;
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_FLOAT; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);

        double d = *reinterpret_cast<const double*>(&vals[0]);
        return boxFloat(d);
    }
} _FLOAT;
ConcreteCompilerType* FLOAT = &_FLOAT;

ConcreteCompilerVariable* makeFloat(double d) {
    return new ConcreteCompilerVariable(FLOAT, llvm::ConstantFP::get(g.double_, d), true);
}

ConcreteCompilerVariable* makeLong(IREmitter& emitter, std::string& n_long) {
    llvm::Value* v
        = emitter.getBuilder()->CreateCall(g.funcs.createLong, embedConstantPtr(&n_long, g.llvm_str_type_ptr));
    return new ConcreteCompilerVariable(LONG, v, true);
}

ConcreteCompilerVariable* makePureImaginary(IREmitter& emitter, double imag) {
    llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createPureImaginary, getConstantDouble(imag));
    return new ConcreteCompilerVariable(BOXED_COMPLEX, v, true);
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
                           const std::vector<const std::string*>* keyword_names) override {
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

    NormalObjectType(BoxedClass* cls) : cls(cls) {
        // ASSERT(!isUserDefined(cls) && "instances of user-defined classes can change their __class__, plus even if
        // they couldn't we couldn't statically resolve their attributes", "%s", getNameOfClass(cls)->c_str());

        assert(cls);
    }

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
            var->incvref();
            return var;
        }
        ASSERT(other_type == UNKNOWN, "%s", other_type->debugName().c_str());
        return new ConcreteCompilerVariable(UNKNOWN, var->getValue(), false);
        // return (new ConcreteCompilerVariable(UNKNOWN, var->getValue(), false))->split(emitter);
    }

    void drop(IREmitter& emitter, VAR* var) override { emitter.getGC()->dropPointer(emitter, var->getValue()); }
    void grab(IREmitter& emitter, VAR* var) override { emitter.getGC()->grabPointer(emitter, var->getValue()); }

    bool isFitBy(BoxedClass* c) override {
        // I don't think it's ok to accept subclasses
        return c == cls;
    }

    bool canStaticallyResolveGetattrs() {
        return (cls->is_constant && !cls->instancesHaveHCAttrs() && !cls->instancesHaveDictAttrs()
                && cls->hasGenericGetattr());
    }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override {
        // Any changes here need to be mirrored in getattr()
        if (canStaticallyResolveGetattrs()) {
            Box* rtattr = cls->getattr(*attr);
            if (rtattr == NULL)
                return UNDEF;

            RELEASE_ASSERT(rtattr, "%s.%s", debugName().c_str(), attr->c_str());
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
                           const std::vector<const std::string*>* keyword_names) override {
        return UNKNOWN;
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) override {
        // Any changes here need to be mirrored in getattrType()
        if (canStaticallyResolveGetattrs()) {
            Box* rtattr = cls->getattr(*attr);
            if (rtattr == NULL) {
                llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                          getStringConstantPtr(std::string(getNameOfClass(cls)) + "\0"),
                                                          getStringConstantPtr(*attr + '\0'));
                call.setDoesNotReturn();
                return undefVariable();
            }

            ASSERT(rtattr, "%s.%s", debugName().c_str(), attr->c_str());
            if (rtattr->cls == function_cls) {
                CompilerVariable* clattr = new ConcreteCompilerVariable(
                    typeFromClass(function_cls), embedConstantPtr(rtattr, g.llvm_value_type_ptr), false);
                return InstanceMethodType::makeIM(var, clattr);
            }
        }

        // TODO could specialize more since we know the class already
        return UNKNOWN->getattr(emitter, info, var, attr, cls_only);
    }

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, const std::string* attr,
                 CompilerVariable* v) override {
        return UNKNOWN->setattr(emitter, info, var, attr, v);
    }

    void delattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                 const std::string* attr) override {
        return UNKNOWN->delattr(emitter, info, var, attr);
    }

    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, ArgPassSpec argspec,
                           const std::vector<CompilerVariable*>& args,
                           const std::vector<const std::string*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->call(emitter, info, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    ConcreteCompilerVariable* tryCallattrConstant(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                                  const std::string* attr, bool clsonly, ArgPassSpec argspec,
                                                  const std::vector<CompilerVariable*>& args,
                                                  const std::vector<const std::string*>* keyword_names,
                                                  bool* no_attribute = NULL) {
        if (!canStaticallyResolveGetattrs())
            return NULL;

        Box* rtattr = cls->getattr(*attr);
        if (rtattr == NULL) {
            if (no_attribute) {
                *no_attribute = true;
            } else {
                llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                          getStringConstantPtr(std::string(getNameOfClass(cls)) + "\0"),
                                                          getStringConstantPtr(*attr + '\0'));
                call.setDoesNotReturn();
            }
            return undefVariable();
        }

        if (rtattr->cls != function_cls)
            return NULL;
        BoxedFunction* rtattr_func = static_cast<BoxedFunction*>(rtattr);

        if (argspec.num_keywords || argspec.has_starargs || argspec.has_kwargs)
            return NULL;

        CLFunction* cl = rtattr_func->f;
        assert(cl);

        if (cl->takes_varargs || cl->takes_kwargs)
            return NULL;

        RELEASE_ASSERT(cl->num_args == cl->numReceivedArgs(), "");
        RELEASE_ASSERT(args.size() + 1 >= cl->num_args - cl->num_defaults && args.size() + 1 <= cl->num_args, "%d",
                       info.unw_info.current_stmt->lineno);

        CompiledFunction* cf = NULL;
        bool found = false;
        // TODO have to find the right version.. similar to resolveclfunc?
        for (int i = 0; i < cl->versions.size(); i++) {
            cf = cl->versions[i];
            assert(cf->spec->arg_types.size() == cl->numReceivedArgs());

            bool fits = true;
            for (int j = 0; j < args.size(); j++) {
                if (!args[j]->canConvertTo(cf->spec->arg_types[j + 1])) {
                    fits = false;
                    break;
                }
            }
            if (!fits)
                continue;

            found = true;
            break;
        }

        assert(found);
        assert(!cf->is_interpreted);
        assert(cf->code);

        std::vector<llvm::Type*> arg_types;
        RELEASE_ASSERT(cl->num_args == cl->numReceivedArgs(), "");
        for (int i = 0; i < cl->num_args; i++) {
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

        llvm::Value* linked_function = embedConstantPtr(cf->code, ft->getPointerTo());

        std::vector<CompilerVariable*> new_args;
        new_args.push_back(var);
        new_args.insert(new_args.end(), args.begin(), args.end());

        for (int i = args.size() + 1; i < cl->num_args; i++) {
            // TODO should _call() be able to take llvm::Value's directly?
            new_args.push_back(new ConcreteCompilerVariable(
                UNKNOWN, embedConstantPtr(rtattr_func->defaults->elts[i - cl->num_args + cl->num_defaults],
                                          g.llvm_value_type_ptr),
                true));
        }

        std::vector<llvm::Value*> other_args;

        ConcreteCompilerVariable* rtn = _call(emitter, info, linked_function, cf->code, other_args, argspec, new_args,
                                              keyword_names, cf->spec->rtn_type);
        assert(rtn->getType() == cf->spec->rtn_type);
        assert(rtn->getType() != UNDEF);

        // We should provide unboxed versions of these rather than boxing then unboxing:
        // TODO is it more efficient to unbox here, or should we leave it boxed?
        if (cf->spec->rtn_type == BOXED_BOOL) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, rtn->getValue());
            return boolFromI1(emitter, unboxed);
        }
        if (cf->spec->rtn_type == BOXED_INT) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxInt, rtn->getValue());
            return new ConcreteCompilerVariable(INT, unboxed, true);
        }
        if (cf->spec->rtn_type == BOXED_FLOAT) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxFloat, rtn->getValue());
            return new ConcreteCompilerVariable(FLOAT, unboxed, true);
        }
        assert(cf->spec->rtn_type != BOXED_INT);
        ASSERT(cf->spec->rtn_type != BOXED_BOOL, "%p", cf->code);
        assert(cf->spec->rtn_type != BOXED_FLOAT);

        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                               const std::string* attr, CallattrFlags flags, ArgPassSpec argspec,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, attr, flags.cls_only, argspec, args, keyword_names);
        if (called_constant)
            return called_constant;

        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, argspec, args, keyword_names);
        converted->decvref(emitter);
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

                const std::string& left_side_name = getOpName(op_type);

                bool no_attribute = false;

                ConcreteCompilerVariable* called_constant
                    = tryCallattrConstant(emitter, info, var, &left_side_name, true, ArgPassSpec(1, 0, 0, 0),
                                          { converted_rhs }, NULL, &no_attribute);

                if (no_attribute) {
                    assert(called_constant->getType() == UNDEF);

                    // Kind of hacky, but just call into getitem like normal.  except...
                    auto r = UNKNOWN->binexp(emitter, info, var, converted_rhs, op_type, exp_type);
                    r->decvref(emitter);
                    // ... return the undef value, since that matches what the type analyzer thought we would do.
                    return called_constant;
                }

                if (called_constant) {
                    converted_rhs->decvref(emitter);
                    return called_constant;
                }
            }
        }

        auto rtn = UNKNOWN->binexp(emitter, info, var, converted_rhs, op_type, exp_type);
        converted_rhs->decvref(emitter);
        return rtn;
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        static const std::string attr("__getitem__");
        bool no_attribute = false;
        ConcreteCompilerVariable* called_constant = tryCallattrConstant(
            emitter, info, var, &attr, true, ArgPassSpec(1, 0, 0, 0), { slice }, NULL, &no_attribute);

        if (no_attribute) {
            assert(called_constant->getType() == UNDEF);

            // Kind of hacky, but just call into getitem like normal.  except...
            auto r = UNKNOWN->getitem(emitter, info, var, slice);
            r->decvref(emitter);
            // ... return the undef value, since that matches what the type analyzer thought we would do.
            return called_constant;
        }

        if (called_constant)
            return called_constant;

        return UNKNOWN->getitem(emitter, info, var, slice);
    }

    CompilerVariable* getPystonIter(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return UNKNOWN->getPystonIter(emitter, info, var);
    }

    ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        static const std::string attr("__len__");
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, &attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL);
        if (called_constant)
            return called_constant;

        return UNKNOWN->len(emitter, info, var);
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        static const std::string attr("__nonzero__");

        bool no_attribute = false;
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, &attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL, &no_attribute);

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

    ConcreteCompilerVariable* hasnext(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        static const std::string attr("__hasnext__");

        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, &attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL, NULL);

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
        return reinterpret_cast<Box*>(vals[0]);
    }
};
std::unordered_map<BoxedClass*, NormalObjectType*> NormalObjectType::made;
ConcreteCompilerType* STR, *BOXED_INT, *BOXED_FLOAT, *BOXED_BOOL, *NONE;

class ClosureType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_closure_type_ptr; }
    std::string debugName() override { return "closure"; }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) override {
        RELEASE_ASSERT(0, "should not be called\n");
        /*
        assert(!cls_only);
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(var->getValue(), g.llvm_value_type_ptr);
        return ConcreteCompilerVariable(UNKNOWN, bitcast, true).getattr(emitter, info, attr, cls_only);
        */
    }

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, const std::string* attr,
                 CompilerVariable* v) override {
        RELEASE_ASSERT(0, "should not be called\n");
    }

    ConcreteCompilerType* getConcreteType() override { return this; }
    ConcreteCompilerType* getBoxType() override { return this; }

    void drop(IREmitter& emitter, VAR* var) override {}
    void grab(IREmitter& emitter, VAR* var) override {}

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        return reinterpret_cast<Box*>(vals[0]);
    }
} _CLOSURE;
ConcreteCompilerType* CLOSURE = &_CLOSURE;

class GeneratorType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_generator_type_ptr; }
    std::string debugName() override { return "generator"; }

    ConcreteCompilerType* getConcreteType() override { return this; }
    ConcreteCompilerType* getBoxType() override { return GENERATOR; }

    void drop(IREmitter& emitter, VAR* var) override {
        // pass
    }
    void grab(IREmitter& emitter, VAR* var) override {
        // pass
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());
        return reinterpret_cast<Box*>(vals[0]);
    }
} _GENERATOR;
ConcreteCompilerType* GENERATOR = &_GENERATOR;

class FrameInfoType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.llvm_frame_info_type->getPointerTo(); }
    std::string debugName() override { return "FrameInfo"; }

    ConcreteCompilerType* getConcreteType() override { return this; }
    ConcreteCompilerType* getBoxType() override { return FRAME_INFO; }

    void drop(IREmitter& emitter, VAR* var) override {
        // pass
    }
    void grab(IREmitter& emitter, VAR* var) override {
        // pass
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        RELEASE_ASSERT(false, "should not be called"); // This function shouldn't be called.
    }
} _FRAME_INFO;
ConcreteCompilerType* FRAME_INFO = &_FRAME_INFO;

class StrConstantType : public ValuedCompilerType<const std::string*> {
public:
    std::string debugName() override { return "str_constant"; }

    void assertMatches(const std::string* v) override {}

    ConcreteCompilerType* getConcreteType() override { return STR; }

    ConcreteCompilerType* getBoxType() override { return STR; }

    void drop(IREmitter& emitter, VAR* var) override {
        // pass
    }

    void grab(IREmitter& emitter, VAR* var) override {
        // pass
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        assert(other_type == STR || other_type == UNKNOWN);
        llvm::Value* boxed = emitter.getBuilder()->CreateCall(g.funcs.boxStringPtr,
                                                              embedConstantPtr(var->getValue(), g.llvm_str_type_ptr));
        return new ConcreteCompilerVariable(other_type, boxed, true);
    }

    bool canConvertTo(ConcreteCompilerType* other) override { return (other == STR || other == UNKNOWN); }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                              bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                               CallattrFlags flags, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        converted->decvref(emitter);
        return rtn;
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return makeBool(var->getValue()->size() != 0);
    }

    CompilerVariable* dup(VAR* var, DupCache& cache) override {
        CompilerVariable*& rtn = cache[var];

        if (rtn == NULL) {
            rtn = new VAR(this, var->getValue(), var->isGrabbed());
            while (rtn->getVrefs() < var->getVrefs())
                rtn->incvref();
        }
        return rtn;
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override {
        stackmap_args.push_back(embedConstantPtr(var->getValue(), g.i8_ptr));
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());

        return boxStringPtr(reinterpret_cast<std::string*>(vals[0]));
    }

    int numFrameArgs() override { return 1; }
};
static ValuedCompilerType<const std::string*>* STR_CONSTANT = new StrConstantType();

CompilerVariable* makeStr(const std::string* s) {
    return new ValuedCompilerVariable<const std::string*>(STR_CONSTANT, s, true);
}

CompilerVariable* makeUnicode(IREmitter& emitter, const std::string* s) {
    llvm::Value* boxed
        = emitter.getBuilder()->CreateCall(g.funcs.decodeUTF8StringPtr, embedConstantPtr(s, g.llvm_str_type_ptr));
    return new ConcreteCompilerVariable(typeFromClass(unicode_cls), boxed, true);
}

class VoidType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() override { return g.void_; }

    Box* deserializeFromFrame(const FrameVals& vals) override { abort(); }
};
ConcreteCompilerType* VOID = new VoidType();

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

    void drop(IREmitter& emitter, VAR* var) override {
        // pass
    }
    void grab(IREmitter& emitter, VAR* var) override {
        // pass
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) override {
        var->incvref();
        return var;
    }

    bool canConvertTo(ConcreteCompilerType* other_type) override {
        return (other_type == UNKNOWN || other_type == BOXED_BOOL || other_type == BOOL);
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                            ConcreteCompilerType* other_type) override {
        if (other_type == BOOL) {
            var->incvref();
            return var;
        }

        ASSERT(other_type == UNKNOWN || other_type == BOXED_BOOL, "%s", other_type->debugName().c_str());
        llvm::Value* boxed = emitter.getBuilder()->CreateCall(g.funcs.boxBool, i1FromBool(emitter, var));
        return new ConcreteCompilerVariable(other_type, boxed, true);
    }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override {
        return BOXED_BOOL->getattrType(attr, cls_only);
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                              bool cls_only) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_BOOL);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                               const std::string* attr, CallattrFlags flags, ArgPassSpec argspec,
                               const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_BOOL);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, flags, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        converted->decvref(emitter);
        return rtn;
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
    return new ConcreteCompilerVariable(BOOL, llvm::ConstantInt::get(BOOL->llvmType(), b, false), true);
}

ConcreteCompilerType* BOXED_TUPLE;
class TupleType : public ValuedCompilerType<const std::vector<CompilerVariable*>*> {
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

    void assertMatches(const std::vector<CompilerVariable*>* v) override {
        assert(v->size() == elt_types.size());

        for (int i = 0; i < v->size(); i++) {
            assert((*v)[i]->getType() == elt_types[i]);
        }
    }

    std::string debugName() override { return name; }

    void drop(IREmitter& emitter, VAR* var) override {
        const std::vector<CompilerVariable*>* elts = var->getValue();
        for (int i = 0; i < elts->size(); i++) {
            (*elts)[i]->decvref(emitter);
        }
    }

    void grab(IREmitter& emitter, VAR* var) override { RELEASE_ASSERT(0, ""); }

    CompilerVariable* dup(VAR* var, DupCache& cache) override {
        CompilerVariable*& rtn = cache[var];

        if (rtn == NULL) {
            std::vector<CompilerVariable*>* elts = new std::vector<CompilerVariable*>();
            const std::vector<CompilerVariable*>* orig_elts = var->getValue();

            for (int i = 0; i < orig_elts->size(); i++) {
                elts->push_back((*orig_elts)[i]->dup(cache));
            }
            rtn = new VAR(this, elts, var->isGrabbed());
            while (rtn->getVrefs() < var->getVrefs())
                rtn->incvref();
        }
        return rtn;
    }

    bool canConvertTo(ConcreteCompilerType* other_type) override {
        return (other_type == UNKNOWN || other_type == BOXED_TUPLE);
    }

    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        assert(other_type == UNKNOWN || other_type == BOXED_TUPLE);

        VEC* v = var->getValue();

        std::vector<ConcreteCompilerVariable*> converted_args;

        llvm::Value* nelts = llvm::ConstantInt::get(g.i64, v->size(), false);

        llvm::Value* _scratch = emitter.getScratch(v->size() * sizeof(void*));
        auto scratch = emitter.getBuilder()->CreateBitCast(_scratch, g.llvm_value_type_ptr->getPointerTo());

        // First, convert all the args, before putting any in the scratch.
        // Do it this way in case any of the conversions themselves need scratch space
        // (ie nested tuples).
        // TODO could probably do this better: create a scratch reservation that gets released
        // at some point, so that we know which scratch space is still in use, so that we can handle
        // multiple concurrent scratch users.
        for (int i = 0; i < v->size(); i++) {
            ConcreteCompilerVariable* converted = (*v)[i]->makeConverted(emitter, (*v)[i]->getBoxType());
            converted_args.push_back(converted);
        }

        for (int i = 0; i < v->size(); i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(scratch, i);
            emitter.getBuilder()->CreateStore(converted_args[i]->getValue(), ptr);
        }

        llvm::Value* rtn = emitter.getBuilder()->CreateCall2(g.funcs.createTuple, nelts, scratch);

        for (int i = 0; i < converted_args.size(); i++) {
            converted_args[i]->decvref(emitter);
        }
        return new ConcreteCompilerVariable(other_type, rtn, true);
    }

    ConcreteCompilerType* getBoxType() override { return BOXED_TUPLE; }

    ConcreteCompilerType* getConcreteType() override { return BOXED_TUPLE; }

    static TupleType* make(const std::vector<CompilerType*>& elt_types) { return new TupleType(elt_types); }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) override {
        if (slice->getType() == INT) {
            llvm::Value* v = static_cast<ConcreteCompilerVariable*>(slice)->getValue();
            assert(v->getType() == g.i64);
            if (llvm::ConstantInt* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
                int64_t i = ci->getSExtValue();
                if (i >= 0 && i < var->getValue()->size()) {
                    CompilerVariable* rtn = (*var->getValue())[i];
                    rtn->incvref();
                    return rtn;
                } else {
                    llvm::CallSite call = emitter.createCall2(info.unw_info, g.funcs.raiseAttributeErrorStr,
                                                              getStringConstantPtr(debugName() + '\0'),
                                                              getStringConstantPtr("__getitem__\0"));
                    call.setDoesNotReturn();
                    return undefVariable();
                }
            }
        }
        RELEASE_ASSERT(0, "");
        // return getConstantInt(var->getValue()->size(), g.i64);
    }

    ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return new ConcreteCompilerVariable(INT, getConstantInt(var->getValue()->size(), g.i64), true);
    }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override {
        return BOXED_TUPLE->getattrType(attr, cls_only);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        converted->decvref(emitter);
        return rtn;
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                               CallattrFlags flags, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        return makeConverted(emitter, var, getConcreteType())
            ->callattr(emitter, info, attr, flags, argspec, args, keyword_names);
    }

    void serializeToFrame(VAR* var, std::vector<llvm::Value*>& stackmap_args) override {
        for (auto v : *var->getValue()) {
            v->serializeToFrame(stackmap_args);
        }
    }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == numFrameArgs());

        BoxedTuple::GCVector elts;
        int cur_idx = 0;
        for (auto e : elt_types) {
            int num_args = e->numFrameArgs();
            // TODO: inefficient to make these copies
            FrameVals sub_vals(vals.begin() + cur_idx, vals.begin() + cur_idx + num_args);

            elts.push_back(e->deserializeFromFrame(sub_vals));

            cur_idx += num_args;
        }
        assert(cur_idx == vals.size());

        return new BoxedTuple(std::move(elts));
    }

    int numFrameArgs() override {
        int rtn = 0;
        for (auto e : elt_types)
            rtn += e->numFrameArgs();
        return rtn;
    }

    std::vector<CompilerVariable*> unpack(IREmitter& emitter, const OpInfo& info, VAR* var, int num_into) override {
        if (num_into != elt_types.size()) {
            return ValuedCompilerType::unpack(emitter, info, var, num_into);
        }

        // Not sure if this is right:
        for (auto e : *var->getValue())
            e->incvref();

        return *var->getValue();
    }
};

CompilerType* makeTupleType(const std::vector<CompilerType*>& elt_types) {
    return TupleType::make(elt_types);
}

CompilerVariable* makeTuple(const std::vector<CompilerVariable*>& elts) {
    std::vector<CompilerType*> elt_types;
    for (int i = 0; i < elts.size(); i++) {
        elts[i]->incvref();
        elt_types.push_back(elts[i]->getType());
    }
    TupleType* type = TupleType::make(elt_types);

    const std::vector<CompilerVariable*>* alloc_elts = new std::vector<CompilerVariable*>(elts);
    return new TupleType::VAR(type, alloc_elts, true);
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
                           const std::vector<const std::string*>* keyword_names) override {
        return undefVariable();
    }
    void drop(IREmitter& emitter, VAR* var) override {}
    void grab(IREmitter& emitter, VAR* var) override {}
    CompilerVariable* dup(VAR* v, DupCache& cache) override {
        // TODO copied from UnknownType
        auto& rtn = cache[v];
        if (rtn == NULL) {
            rtn = new VAR(this, v->getValue(), v->isGrabbed());
            while (rtn->getVrefs() < v->getVrefs())
                rtn->incvref();
        }
        return rtn;
    }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) override {
        llvm::Value* v = llvm::UndefValue::get(other_type->llvmType());
        return new ConcreteCompilerVariable(other_type, v, true);
    }
    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                              bool cls_only) override {
        return undefVariable();
    }

    CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                               CallattrFlags flags, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                               const std::vector<const std::string*>* keyword_names) override {
        return undefVariable();
    }

    CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<const std::string*>* keyword_names) override {
        return UNDEF;
    }

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(BOOL->llvmType()), true);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        return undefVariable();
    }

    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              CompilerVariable* slice) override {
        return undefVariable();
    }

    ConcreteCompilerType* getBoxType() override { return UNKNOWN; }

    ConcreteCompilerType* getConcreteType() override { return this; }

    CompilerType* getattrType(const std::string* attr, bool cls_only) override { return UNDEF; }

    bool canConvertTo(ConcreteCompilerType* other_type) override { return true; }

    BoxedClass* guaranteedClass() override { return NULL; }

    Box* deserializeFromFrame(const FrameVals& vals) override {
        assert(vals.size() == 1);
        abort();
    }
} _UNDEF;
CompilerType* UNDEF = &_UNDEF;

ConcreteCompilerVariable* undefVariable() {
    return new ConcreteCompilerVariable(&_UNDEF, llvm::UndefValue::get(_UNDEF.llvmType()), true);
}

ConcreteCompilerVariable* boolFromI1(IREmitter& emitter, llvm::Value* v) {
    if (BOOLS_AS_I64) {
        assert(v->getType() == g.i1);
        assert(BOOL->llvmType() == g.i64);
        llvm::Value* v2 = emitter.getBuilder()->CreateZExt(v, BOOL->llvmType());
        return new ConcreteCompilerVariable(BOOL, v2, true);
    } else {
        return new ConcreteCompilerVariable(BOOL, v, true);
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
