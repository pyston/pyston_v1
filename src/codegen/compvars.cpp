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

#include "codegen/compvars.h"

#include <cstdio>
#include <sstream>

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/raw_ostream.h"

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

std::string ValuedCompilerType<llvm::Value*>::debugName() {
    std::string rtn;
    llvm::raw_string_ostream os(rtn);
    llvmType()->print(os);
    return rtn;
}

struct RawInstanceMethod {
    CompilerVariable* obj, *func;

    RawInstanceMethod(CompilerVariable* obj, CompilerVariable* func) : obj(obj), func(func) {
        obj->incvref();
        func->incvref();
    }
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
    static InstanceMethodType* get(CompilerType* obj_type, CompilerType* function_type) {
        InstanceMethodType* rtn = made[std::make_pair(obj_type, function_type)];
        if (rtn == NULL)
            rtn = new InstanceMethodType(obj_type, function_type);
        return rtn;
    }

    static CompilerVariable* makeIM(CompilerVariable* obj, CompilerVariable* func) {
        CompilerVariable* rtn = new ValuedCompilerVariable<RawInstanceMethod*>(
            InstanceMethodType::get(obj->getType(), func->getType()), new RawInstanceMethod(obj, func), true);
        return rtn;
    }

    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) {
        std::vector<CompilerType*> new_args(arg_types);
        new_args.insert(new_args.begin(), obj_type);

        ArgPassSpec new_argspec(argspec.num_args + 1u, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
        return function_type->callType(new_argspec, new_args, keyword_names);
    }

    std::string debugName() {
        return "instanceMethod(" + obj_type->debugName() + " ; " + function_type->debugName() + ")";
    }

    virtual void drop(IREmitter& emitter, VAR* var) {
        checkVar(var);
        RawInstanceMethod* val = var->getValue();
        val->obj->decvref(emitter);
        val->func->decvref(emitter);
        delete val;
    }

    virtual CompilerVariable* call(IREmitter& emitter, const OpInfo& info,
                                   ValuedCompilerVariable<RawInstanceMethod*>* var, ArgPassSpec argspec,
                                   const std::vector<CompilerVariable*>& args,
                                   const std::vector<const std::string*>* keyword_names) {
        std::vector<CompilerVariable*> new_args;
        new_args.push_back(var->getValue()->obj);
        new_args.insert(new_args.end(), args.begin(), args.end());

        ArgPassSpec new_argspec(argspec.num_args + 1u, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
        return var->getValue()->func->call(emitter, info, new_argspec, new_args, keyword_names);
    }

    virtual bool canConvertTo(ConcreteCompilerType* other_type) { return other_type == UNKNOWN; }
    virtual ConcreteCompilerType* getConcreteType() { return typeFromClass(instancemethod_cls); }
    virtual ConcreteCompilerType* getBoxType() { return getConcreteType(); }
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) {
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
    virtual CompilerVariable* dup(VAR* var, DupCache& cache) {
        checkVar(var);

        CompilerVariable* rtn = cache[var];
        if (rtn == NULL) {
            RawInstanceMethod* im = var->getValue();
            RawInstanceMethod* new_im = new RawInstanceMethod(im->obj->dup(cache), im->func->dup(cache));
            rtn = new VAR(this, new_im, var->isGrabbed());
        }
        return rtn;
    }
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
    llvm::Type* llvmType() { return g.llvm_value_type_ptr; }

    virtual std::string debugName() { return "AnyBox"; }

    virtual void drop(IREmitter& emitter, VAR* var) { emitter.getGC()->dropPointer(emitter, var->getValue()); }
    virtual void grab(IREmitter& emitter, VAR* var) { emitter.getGC()->grabPointer(emitter, var->getValue()); }

    virtual bool isFitBy(BoxedClass* c) { return true; }

    // XXX should get rid of this implementation and have it just do print o.__repr__()
    virtual void print(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        emitter.createCall(info.exc_info, g.funcs.print, var->getValue());
    }

    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                      const std::string* attr, bool cls_only);
    virtual CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                   ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                   const std::vector<const std::string*>* keyword_names);
    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                       const std::string* attr, bool clsonly, ArgPassSpec argspec,
                                       const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names);
    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var);

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, const std::string* attr,
                 CompilerVariable* v) {
        llvm::Constant* ptr = getStringConstantPtr(*attr + '\0');
        ConcreteCompilerVariable* converted = v->makeConverted(emitter, UNKNOWN);
        // g.funcs.setattr->dump();
        // var->getValue()->dump(); llvm::errs() << '\n';
        // ptr->dump(); llvm::errs() << '\n';
        // converted->getValue()->dump(); llvm::errs() << '\n';
        bool do_patchpoint = ENABLE_ICSETATTRS && !info.isInterpreted();
        if (do_patchpoint) {
            PatchpointSetupInfo* pp
                = patchpoints::createSetattrPatchpoint(emitter.currentFunction(), info.getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(ptr);
            llvm_args.push_back(converted->getValue());

            emitter.createPatchpoint(pp, (void*)pyston::setattr, llvm_args, info.exc_info);
        } else {
            emitter.createCall3(info.exc_info, g.funcs.setattr, var->getValue(), ptr, converted->getValue());
        }
        converted->decvref(emitter);
    }

    virtual llvm::Value* makeClassCheck(IREmitter& emitter, ConcreteCompilerVariable* var, BoxedClass* cls) {
        assert(var->getValue()->getType() == g.llvm_value_type_ptr);
        // TODO this is brittle: directly embeds the position of the class object:
        llvm::Value* cls_ptr = emitter.getBuilder()->CreateConstInBoundsGEP2_32(var->getValue(), 0, 1);
        llvm::Value* cls_value = emitter.getBuilder()->CreateLoad(cls_ptr);
        assert(cls_value->getType() == g.llvm_class_type_ptr);
        llvm::Value* rtn = emitter.getBuilder()->CreateICmpEQ(cls_value, embedConstantPtr(cls, g.llvm_class_type_ptr));
        return rtn;
    }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) { return UNKNOWN; }
    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) {
        return UNKNOWN;
    }
    virtual BoxedClass* guaranteedClass() { return NULL; }
    virtual ConcreteCompilerType* getBoxType() { return this; }
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                    ConcreteCompilerType* other_type) {
        if (other_type == this) {
            var->incvref();
            return var;
        }
        fprintf(stderr, "Can't convert unknown to %s...\n", other_type->debugName().c_str());
        abort();
    }

    virtual ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        bool do_patchpoint = ENABLE_ICGENERICS && !info.isInterpreted();
        llvm::Value* rtn;
        if (do_patchpoint) {
            PatchpointSetupInfo* pp
                = patchpoints::createGenericPatchpoint(emitter.currentFunction(), info.getTypeRecorder(), true, 160);

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());

            rtn = emitter.createPatchpoint(pp, (void*)pyston::unboxedLen, llvm_args, info.exc_info).getInstruction();
        } else {
            rtn = emitter.createCall(info.exc_info, g.funcs.unboxedLen, var->getValue()).getInstruction();
        }
        assert(rtn->getType() == g.i64);
        return new ConcreteCompilerVariable(INT, rtn, true);
    }

    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                      CompilerVariable* slice) {
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());

        bool do_patchpoint = ENABLE_ICGETITEMS && !info.isInterpreted();
        llvm::Value* rtn;
        if (do_patchpoint) {
            PatchpointSetupInfo* pp
                = patchpoints::createGetitemPatchpoint(emitter.currentFunction(), info.getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(converted_slice->getValue());

            llvm::Value* uncasted
                = emitter.createPatchpoint(pp, (void*)pyston::getitem, llvm_args, info.exc_info).getInstruction();
            rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
        } else {
            rtn = emitter.createCall2(info.exc_info, g.funcs.getitem, var->getValue(), converted_slice->getValue())
                      .getInstruction();
        }

        converted_slice->decvref(emitter);
        return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
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
            PatchpointSetupInfo* pp
                = patchpoints::createBinexpPatchpoint(emitter.currentFunction(), info.getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(var->getValue());
            llvm_args.push_back(converted_rhs->getValue());
            llvm_args.push_back(getConstantInt(op_type, g.i32));

            llvm::Value* uncasted
                = emitter.createPatchpoint(pp, rt_func_addr, llvm_args, info.exc_info).getInstruction();
            rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
        } else {
            rtn = emitter.createCall3(info.exc_info, rt_func, var->getValue(), converted_rhs->getValue(),
                                      getConstantInt(op_type, g.i32)).getInstruction();
        }

        converted_rhs->decvref(emitter);

        if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn || op_type == AST_TYPE::Is
            || op_type == AST_TYPE::IsNot) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, rtn);
            ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(BOOL, unboxed, true);
            return rtn;
        }

        return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
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
        PatchpointSetupInfo* pp
            = patchpoints::createGetattrPatchpoint(emitter.currentFunction(), info.getTypeRecorder());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());
        llvm_args.push_back(ptr);

        llvm::Value* uncasted = emitter.createPatchpoint(pp, raw_func, llvm_args, info.exc_info).getInstruction();
        rtn_val = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        rtn_val = emitter.createCall2(info.exc_info, llvm_func, var->getValue(), ptr).getInstruction();
    }
    return new ConcreteCompilerVariable(UNKNOWN, rtn_val, true);
}

static ConcreteCompilerVariable* _call(IREmitter& emitter, const OpInfo& info, llvm::Value* func, void* func_addr,
                                       const std::vector<llvm::Value*> other_args, ArgPassSpec argspec,
                                       const std::vector<CompilerVariable*> args,
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

        PatchpointSetupInfo* pp
            = patchpoints::createCallsitePatchpoint(emitter.currentFunction(), info.getTypeRecorder(), args.size());

        llvm::Value* uncasted = emitter.createPatchpoint(pp, func_addr, llvm_args, info.exc_info).getInstruction();

        assert(llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(func->getType())->getElementType())
                   ->getReturnType() == g.llvm_value_type_ptr);
        rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        // func->dump();
        // for (auto a : llvm_args) {
        // a->dump();
        //}
        // printf("%ld %ld\n", llvm_args.size(), args.size());
        rtn = emitter.createCall(info.exc_info, func, llvm_args).getInstruction();
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
        func = g.funcs.runtimeCall;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());

    llvm::Value* llvm_argspec = llvm::ConstantInt::get(g.i32, argspec.asInt(), false);
    other_args.push_back(llvm_argspec);
    return _call(emitter, info, func, (void*)runtimeCall, other_args, argspec, args, keyword_names, UNKNOWN);
}

CompilerVariable* UnknownType::callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                        const std::string* attr, bool clsonly, ArgPassSpec argspec,
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
        func = g.funcs.callattr;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());
    other_args.push_back(embedConstantPtr(attr, g.llvm_str_type_ptr));
    other_args.push_back(getConstantInt(clsonly, g.i1));

    llvm::Value* llvm_argspec = llvm::ConstantInt::get(g.i32, argspec.asInt(), false);
    other_args.push_back(llvm_argspec);
    return _call(emitter, info, func, (void*)pyston::callattr, other_args, argspec, args, keyword_names, UNKNOWN);
}

ConcreteCompilerVariable* UnknownType::nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
    bool do_patchpoint = ENABLE_ICNONZEROS && !info.isInterpreted();
    llvm::Value* rtn_val;
    if (do_patchpoint) {
        PatchpointSetupInfo* pp
            = patchpoints::createNonzeroPatchpoint(emitter.currentFunction(), info.getTypeRecorder());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());

        llvm::Value* uncasted
            = emitter.createPatchpoint(pp, (void*)pyston::nonzero, llvm_args, info.exc_info).getInstruction();
        rtn_val = emitter.getBuilder()->CreateTrunc(uncasted, g.i1);
    } else {
        rtn_val = emitter.createCall(info.exc_info, g.funcs.nonzero, var->getValue()).getInstruction();
    }
    return new ConcreteCompilerVariable(BOOL, rtn_val, true);
}

CompilerVariable* makeFunction(IREmitter& emitter, CLFunction* f, CompilerVariable* closure,
                               const std::vector<ConcreteCompilerVariable*>& defaults) {
    // Unlike the CLFunction*, which can be shared between recompilations, the Box* around it
    // should be created anew every time the functiondef is encountered

    llvm::Value* closure_v;
    ConcreteCompilerVariable* converted = NULL;
    if (closure) {
        converted = closure->makeConverted(emitter, closure->getConcreteType());
        closure_v = converted->getValue();
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

    llvm::Value* boxed = emitter.getBuilder()->CreateCall(
        g.funcs.boxCLFunction, std::vector<llvm::Value*>{ embedConstantPtr(f, g.llvm_clfunction_type_ptr), closure_v,
                                                          scratch, getConstantInt(defaults.size(), g.i64) });

    if (converted)
        converted->decvref(emitter);
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
    virtual std::string debugName() { return "<AbstractFunctionType>"; }

    virtual ConcreteCompilerType* getConcreteType() { return UNKNOWN; }

    virtual ConcreteCompilerType* getBoxType() { return UNKNOWN; }

    virtual bool canConvertTo(ConcreteCompilerType* other_type) { return other_type == UNKNOWN; }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) { return UNDEF; }

    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) {
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

    virtual BoxedClass* guaranteedClass() { return NULL; }

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
};

class IntType : public ConcreteCompilerType {
public:
    IntType() {}

    llvm::Type* llvmType() { return g.i64; }

    virtual bool isFitBy(BoxedClass* c) { return false; }

    virtual void drop(IREmitter& emitter, ConcreteCompilerVariable* var) {
        // pass
    }
    virtual void grab(IREmitter& emitter, ConcreteCompilerVariable* var) {
        // pass
    }

    virtual void print(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        assert(var->getValue()->getType() == g.i64);

        llvm::Constant* int_fmt = getStringConstantPtr("%ld");
        ;
        emitter.getBuilder()->CreateCall2(g.funcs.printf, int_fmt, var->getValue());
    }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) {
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

        return BOXED_INT->getattrType(attr, cls_only);
    }

    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                       const std::string* attr, bool clsonly, ArgPassSpec argspec,
                                       const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, clsonly, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                      const std::string* attr, bool cls_only) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    virtual void setattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                         CompilerVariable* v) {
        llvm::CallSite call = emitter.createCall2(info.exc_info, g.funcs.raiseAttributeErrorStr,
                                                  getStringConstantPtr("int\0"), getStringConstantPtr(*attr + '\0'));
        call.setDoesNotReturn();
    }

    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                    ConcreteCompilerType* other_type) {
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

    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
        converted->decvref(emitter);
        return rtn;
    }

    virtual ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) {
        llvm::CallSite call
            = emitter.createCall(info.exc_info, g.funcs.raiseNotIterableError, getStringConstantPtr("int"));
        call.setDoesNotReturn();
        return new ConcreteCompilerVariable(INT, llvm::UndefValue::get(g.i64), true);
    }

    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        llvm::Value* cmp = emitter.getBuilder()->CreateICmpNE(var->getValue(), llvm::ConstantInt::get(g.i64, 0, false));
        return new ConcreteCompilerVariable(BOOL, cmp, true);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        if (rhs->getType() != INT) {
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
            converted->decvref(emitter);
            return rtn;
        }

        ConcreteCompilerVariable* converted_right = rhs->makeConverted(emitter, INT);
        llvm::Value* v;
        if (op_type == AST_TYPE::Mod) {
            v = emitter.createCall2(info.exc_info, g.funcs.mod_i64_i64, var->getValue(), converted_right->getValue())
                    .getInstruction();
        } else if (op_type == AST_TYPE::Div || op_type == AST_TYPE::FloorDiv) {
            v = emitter.createCall2(info.exc_info, g.funcs.div_i64_i64, var->getValue(), converted_right->getValue())
                    .getInstruction();
        } else if (op_type == AST_TYPE::Pow) {
            v = emitter.createCall2(info.exc_info, g.funcs.pow_i64_i64, var->getValue(), converted_right->getValue())
                    .getInstruction();
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
        } else {
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
        assert(v->getType() == g.i64 || v->getType() == g.i1);
        return new ConcreteCompilerVariable(v->getType() == g.i64 ? INT : BOOL, v, true);
    }

    virtual ConcreteCompilerType* getBoxType() { return BOXED_INT; }
} _INT;
ConcreteCompilerType* INT = &_INT;

ConcreteCompilerVariable* makeInt(int64_t n) {
    return new ConcreteCompilerVariable(INT, llvm::ConstantInt::get(g.i64, n, true), true);
}

class FloatType : public ConcreteCompilerType {
public:
    FloatType() {}

    llvm::Type* llvmType() { return g.double_; }

    virtual bool isFitBy(BoxedClass* c) { return false; }

    virtual void drop(IREmitter& emitter, ConcreteCompilerVariable* var) {
        // pass
    }
    virtual void grab(IREmitter& emitter, ConcreteCompilerVariable* var) {
        // pass
    }

    virtual void print(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        assert(var->getValue()->getType() == g.double_);

        emitter.getBuilder()->CreateCall(g.funcs.printFloat, var->getValue());
    }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) {
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

    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                      const std::string* attr, bool cls_only) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    virtual void setattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                         CompilerVariable* v) {
        llvm::CallSite call = emitter.createCall2(info.exc_info, g.funcs.raiseAttributeErrorStr,
                                                  getStringConstantPtr("float\0"), getStringConstantPtr(*attr + '\0'));
        call.setDoesNotReturn();
    }

    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                    ConcreteCompilerType* other_type) {
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

    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        llvm::Value* cmp = emitter.getBuilder()->CreateFCmpUNE(var->getValue(), llvm::ConstantFP::get(g.double_, 0));
        return new ConcreteCompilerVariable(BOOL, cmp, true);
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
            converted_right = rhs->makeConverted(emitter, INT);
            llvm::Value* conv = emitter.getBuilder()->CreateSIToFP(converted_right->getValue(), g.double_);
            converted_right->decvref(emitter);
            converted_right = new ConcreteCompilerVariable(FLOAT, conv, true);
        }

        llvm::Value* v;
        bool succeeded = true;
        if (op_type == AST_TYPE::Mod) {
            v = emitter.createCall2(info.exc_info, g.funcs.mod_float_float, var->getValue(),
                                    converted_right->getValue()).getInstruction();
        } else if (op_type == AST_TYPE::Div || op_type == AST_TYPE::FloorDiv) {
            v = emitter.createCall2(info.exc_info, g.funcs.div_float_float, var->getValue(),
                                    converted_right->getValue()).getInstruction();
        } else if (op_type == AST_TYPE::Pow) {
            v = emitter.createCall2(info.exc_info, g.funcs.pow_float_float, var->getValue(),
                                    converted_right->getValue()).getInstruction();
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
            assert(v->getType() == g.double_ || v->getType() == g.i1);
            return new ConcreteCompilerVariable(v->getType() == g.double_ ? FLOAT : BOOL, v, true);
        }

        // TODO duplication with top of function, other functions, etc
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_FLOAT);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        converted->decvref(emitter);
        return rtn;
    }

    virtual ConcreteCompilerType* getBoxType() { return BOXED_FLOAT; }
} _FLOAT;
ConcreteCompilerType* FLOAT = &_FLOAT;

ConcreteCompilerVariable* makeFloat(double d) {
    return new ConcreteCompilerVariable(FLOAT, llvm::ConstantFP::get(g.double_, d), true);
}

class KnownClassobjType : public ValuedCompilerType<BoxedClass*> {
private:
    BoxedClass* cls;

    static std::unordered_map<BoxedClass*, KnownClassobjType*> made;

    KnownClassobjType(BoxedClass* cls) : cls(cls) { assert(cls); }

public:
    virtual std::string debugName() { return "class '" + *getNameOfClass(cls) + "'"; }

    static KnownClassobjType* fromClass(BoxedClass* cls) {
        KnownClassobjType*& rtn = made[cls];
        if (rtn == NULL) {
            rtn = new KnownClassobjType(cls);
        }
        return rtn;
    }

    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) {
        RELEASE_ASSERT(!argspec.has_starargs, "");
        RELEASE_ASSERT(!argspec.has_kwargs, "");
        RELEASE_ASSERT(argspec.num_keywords == 0, "");

        bool is_well_defined = (cls == xrange_cls);
        assert(is_well_defined);
        return typeFromClass(cls);
    }
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
    llvm::Type* llvmType() { return g.llvm_value_type_ptr; }
    std::string debugName() {
        assert(cls);
        // TODO add getTypeName

        return "NormalType(" + *getNameOfClass(cls) + ")";
    }
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                    ConcreteCompilerType* other_type) {
        if (other_type == this) {
            var->incvref();
            return var;
        }
        ASSERT(other_type == UNKNOWN, "%s", other_type->debugName().c_str());
        return new ConcreteCompilerVariable(UNKNOWN, var->getValue(), false);
        // return (new ConcreteCompilerVariable(UNKNOWN, var->getValue(), false))->split(emitter);
    }

    virtual void drop(IREmitter& emitter, VAR* var) { emitter.getGC()->dropPointer(emitter, var->getValue()); }
    virtual void grab(IREmitter& emitter, VAR* var) { emitter.getGC()->grabPointer(emitter, var->getValue()); }

    virtual bool isFitBy(BoxedClass* c) { return c == cls; }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) {
        if (cls->is_constant && !cls->instancesHaveAttrs() && cls->hasGenericGetattr()) {
            Box* rtattr = cls->getattr(*attr);
            if (rtattr == NULL)
                return UNDEF;

            RELEASE_ASSERT(rtattr, "%s.%s", debugName().c_str(), attr->c_str());
            if (rtattr->cls == function_cls) {
                return AbstractFunctionType::fromRT(static_cast<BoxedFunction*>(rtattr), true);
                // return typeFromClass(instancemethod_cls);
            } else {
                return typeFromClass(rtattr->cls);
            }
        }

        return UNKNOWN;
    }

    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) {
        return UNKNOWN;
    }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) {
        // printf("%s.getattr %s\n", debugName().c_str(), attr->c_str());
        if (cls->is_constant && !cls->instancesHaveAttrs() && cls->hasGenericGetattr()) {
            Box* rtattr = cls->getattr(*attr);
            if (rtattr == NULL) {
                llvm::CallSite call = emitter.createCall2(info.exc_info, g.funcs.raiseAttributeErrorStr,
                                                          getStringConstantPtr(*getNameOfClass(cls) + "\0"),
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
                 CompilerVariable* v) {
        return UNKNOWN->setattr(emitter, info, var, attr, v);
    }

    virtual void print(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        converted->print(emitter, info);
        converted->decvref(emitter);
    }

    virtual CompilerVariable* call(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                   ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                   const std::vector<const std::string*>* keyword_names) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->call(emitter, info, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    ConcreteCompilerVariable* tryCallattrConstant(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                                  const std::string* attr, bool clsonly, ArgPassSpec argspec,
                                                  const std::vector<CompilerVariable*>& args,
                                                  const std::vector<const std::string*>* keyword_names,
                                                  bool raise_on_missing = true) {
        if (!cls->is_constant || cls->instancesHaveAttrs() || !cls->hasGenericGetattr())
            return NULL;

        Box* rtattr = cls->getattr(*attr);
        if (rtattr == NULL) {
            if (raise_on_missing) {
                llvm::CallSite call = emitter.createCall2(info.exc_info, g.funcs.raiseAttributeErrorStr,
                                                          getStringConstantPtr(*getNameOfClass(cls) + "\0"),
                                                          getStringConstantPtr(*attr + '\0'));
                call.setDoesNotReturn();
                return undefVariable();
            } else {
                return NULL;
            }
        }

        if (rtattr->cls != function_cls)
            return NULL;
        BoxedFunction* rtattr_func = static_cast<BoxedFunction*>(rtattr);

        RELEASE_ASSERT(!argspec.has_starargs, "");
        RELEASE_ASSERT(!argspec.has_kwargs, "");
        RELEASE_ASSERT(argspec.num_keywords == 0, "");

        CLFunction* cl = rtattr_func->f;
        assert(cl);

        if (cl->takes_varargs || cl->takes_kwargs)
            return NULL;

        RELEASE_ASSERT(cl->num_args == cl->numReceivedArgs(), "");
        RELEASE_ASSERT(args.size() + 1 >= cl->num_args - cl->num_defaults && args.size() + 1 <= cl->num_args, "");

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

            arg_types.push_back(g.llvm_value_type_ptr);
            if (i == 3) {
                arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                break;
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
                UNKNOWN, embedConstantPtr(rtattr_func->defaults->elts[i - args.size() - 1], g.llvm_value_type_ptr),
                true));
        }

        std::vector<llvm::Value*> other_args;

        ConcreteCompilerVariable* rtn = _call(emitter, info, linked_function, cf->code, other_args, argspec, new_args,
                                              keyword_names, cf->spec->rtn_type);
        assert(rtn->getType() == cf->spec->rtn_type);

        // We should provide unboxed versions of these rather than boxing then unboxing:
        // TODO is it more efficient to unbox here, or should we leave it boxed?
        if (cf->spec->rtn_type == BOXED_BOOL) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, rtn->getValue());
            return new ConcreteCompilerVariable(BOOL, unboxed, true);
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

    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                                       const std::string* attr, bool clsonly, ArgPassSpec argspec,
                                       const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, attr, clsonly, argspec, args, keyword_names);
        if (called_constant)
            return called_constant;

        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, clsonly, argspec, args, keyword_names);
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
                    || (cls == list_cls && rhs_cls == int_cls))) {

                const std::string& left_side_name = getOpName(op_type);

                ConcreteCompilerVariable* called_constant = tryCallattrConstant(
                    emitter, info, var, &left_side_name, true, ArgPassSpec(1, 0, 0, 0), { converted_rhs }, NULL, false);
                if (called_constant)
                    return called_constant;
            }
        }

        auto rtn = UNKNOWN->binexp(emitter, info, var, converted_rhs, op_type, exp_type);
        converted_rhs->decvref(emitter);
        return rtn;
    }

    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) {
        static const std::string attr("__getitem__");
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, &attr, true, ArgPassSpec(1, 0, 0, 0), { slice }, NULL, false);
        if (called_constant)
            return called_constant;

        return UNKNOWN->getitem(emitter, info, var, slice);
    }

    virtual ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) {
        static const std::string attr("__len__");
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, &attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL);
        if (called_constant)
            return called_constant;

        return UNKNOWN->len(emitter, info, var);
    }

    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        static const std::string attr("__nonzero__");
        ConcreteCompilerVariable* called_constant
            = tryCallattrConstant(emitter, info, var, &attr, true, ArgPassSpec(0, 0, 0, 0), {}, NULL);
        if (called_constant)
            return called_constant;

        if (cls == bool_cls) {
            assert(0 && "should have been caught by above case");
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, var->getValue());
            assert(unboxed->getType() == g.i1);
            return new ConcreteCompilerVariable(BOOL, unboxed, true);
        }

        return UNKNOWN->nonzero(emitter, info, var);
    }

    static NormalObjectType* fromClass(BoxedClass* cls) {
        NormalObjectType*& rtn = made[cls];
        if (rtn == NULL) {
            rtn = new NormalObjectType(cls);
        }
        return rtn;
    }

    virtual BoxedClass* guaranteedClass() { return cls; }

    virtual ConcreteCompilerType* getBoxType() { return this; }
};
std::unordered_map<BoxedClass*, NormalObjectType*> NormalObjectType::made;
ConcreteCompilerType* STR, *BOXED_INT, *BOXED_FLOAT, *BOXED_BOOL, *NONE;

class ClosureType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() { return g.llvm_closure_type_ptr; }
    std::string debugName() { return "closure"; }

    CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var,
                              const std::string* attr, bool cls_only) {
        assert(!cls_only);
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(var->getValue(), g.llvm_value_type_ptr);
        return ConcreteCompilerVariable(UNKNOWN, bitcast, true).getattr(emitter, info, attr, cls_only);
    }

    void setattr(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var, const std::string* attr,
                 CompilerVariable* v) {
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(var->getValue(), g.llvm_value_type_ptr);
        ConcreteCompilerVariable(UNKNOWN, bitcast, true).setattr(emitter, info, attr, v);
    }

    virtual ConcreteCompilerType* getConcreteType() { return this; }
    // Shouldn't call this:
    virtual ConcreteCompilerType* getBoxType() { RELEASE_ASSERT(0, ""); }

    void drop(IREmitter& emitter, VAR* var) override {}
    void grab(IREmitter& emitter, VAR* var) override {}

} _CLOSURE;
ConcreteCompilerType* CLOSURE = &_CLOSURE;

class StrConstantType : public ValuedCompilerType<std::string*> {
public:
    std::string debugName() { return "str_constant"; }

    virtual ConcreteCompilerType* getConcreteType() { return STR; }

    virtual ConcreteCompilerType* getBoxType() { return STR; }

    virtual void drop(IREmitter& emitter, VAR* var) {
        // pass
    }

    virtual void grab(IREmitter& emitter, VAR* var) {
        // pass
    }

    virtual void print(IREmitter& emitter, const OpInfo& info, ValuedCompilerVariable<std::string*>* value) {
        llvm::Constant* ptr = getStringConstantPtr(*(value->getValue()) + '\0');
        llvm::Constant* fmt = getStringConstantPtr("%s\0");
        emitter.getBuilder()->CreateCall2(g.funcs.printf, fmt, ptr);
    }

    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ValuedCompilerVariable<std::string*>* var,
                                                    ConcreteCompilerType* other_type) {
        assert(other_type == STR || other_type == UNKNOWN);
        llvm::Value* boxed = emitter.getBuilder()->CreateCall(g.funcs.boxStringPtr,
                                                              embedConstantPtr(var->getValue(), g.llvm_str_type_ptr));
        return new ConcreteCompilerVariable(other_type, boxed, true);
    }

    virtual bool canConvertTo(ConcreteCompilerType* other) { return (other == STR || other == UNKNOWN); }

    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                                      bool cls_only) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
        converted->decvref(emitter);
        return rtn;
    }

    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                                       bool clsonly, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->callattr(emitter, info, attr, clsonly, argspec, args, keyword_names);
        converted->decvref(emitter);
        return rtn;
    }

    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, STR);
        CompilerVariable* rtn = converted->getitem(emitter, info, slice);
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

    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) override {
        return makeBool(var->getValue()->size() != 0);
    }

    virtual CompilerVariable* dup(VAR* var, DupCache& cache) {
        CompilerVariable*& rtn = cache[var];

        if (rtn == NULL) {
            rtn = new VAR(this, var->getValue(), var->isGrabbed());
            while (rtn->getVrefs() < var->getVrefs())
                rtn->incvref();
        }
        return rtn;
    }
};
ValuedCompilerType<std::string*>* STR_CONSTANT = new StrConstantType();

CompilerVariable* makeStr(std::string* s) {
    return new ValuedCompilerVariable<std::string*>(STR_CONSTANT, s, true);
}

class VoidType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() { return g.void_; }
};
ConcreteCompilerType* VOID = new VoidType();

ConcreteCompilerType* typeFromClass(BoxedClass* c) {
    assert(c);
    return NormalObjectType::fromClass(c);
}

class BoolType : public ConcreteCompilerType {
public:
    llvm::Type* llvmType() { return g.i1; }

    virtual bool isFitBy(BoxedClass* c) { return false; }

    virtual void drop(IREmitter& emitter, VAR* var) {
        // pass
    }
    virtual void grab(IREmitter& emitter, VAR* var) {
        // pass
    }
    virtual void print(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        assert(var->getValue()->getType() == g.i1);

        llvm::Value* true_str = getStringConstantPtr("True");
        llvm::Value* false_str = getStringConstantPtr("False");
        llvm::Value* selected = emitter.getBuilder()->CreateSelect(var->getValue(), true_str, false_str);
        emitter.getBuilder()->CreateCall(g.funcs.printf, selected);
    }

    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, ConcreteCompilerVariable* var) {
        var->incvref();
        return var;
    }

    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                    ConcreteCompilerType* other_type) {
        if (other_type == BOOL) {
            var->incvref();
            return var;
        }

        ASSERT(other_type == UNKNOWN || other_type == BOXED_BOOL, "%s", other_type->debugName().c_str());
        llvm::Value* boxed = emitter.getBuilder()->CreateCall(g.funcs.boxBool, var->getValue());
        return new ConcreteCompilerVariable(other_type, boxed, true);
    }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) {
        return BOXED_BOOL->getattrType(attr, cls_only);
    }

    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                                      bool cls_only) {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_BOOL);
        CompilerVariable* rtn = converted->getattr(emitter, info, attr, cls_only);
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

    virtual ConcreteCompilerType* getBoxType() { return BOXED_BOOL; }
};
ConcreteCompilerType* BOOL = new BoolType();
ConcreteCompilerVariable* makeBool(bool b) {
    return new ConcreteCompilerVariable(BOOL, llvm::ConstantInt::get(g.i1, b, false), true);
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

    std::string debugName() { return name; }

    virtual void drop(IREmitter& emitter, VAR* var) {
        const std::vector<CompilerVariable*>* elts = var->getValue();
        for (int i = 0; i < elts->size(); i++) {
            (*elts)[i]->decvref(emitter);
        }
    }

    virtual void grab(IREmitter& emitter, VAR* var) { RELEASE_ASSERT(0, ""); }

    virtual CompilerVariable* dup(VAR* var, DupCache& cache) {
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

    virtual void print(IREmitter& emitter, const OpInfo& info, VAR* var) {
        llvm::Constant* open_paren = getStringConstantPtr("(");
        llvm::Constant* close_paren = getStringConstantPtr(")");
        llvm::Constant* comma = getStringConstantPtr(",");
        llvm::Constant* comma_space = getStringConstantPtr(", ");

        VEC* v = var->getValue();

        emitter.getBuilder()->CreateCall(g.funcs.printf, open_paren);

        for (int i = 0; i < v->size(); i++) {
            if (i)
                emitter.getBuilder()->CreateCall(g.funcs.printf, comma_space);
            (*v)[i]->print(emitter, info);
        }
        if (v->size() == 1)
            emitter.getBuilder()->CreateCall(g.funcs.printf, comma);

        emitter.getBuilder()->CreateCall(g.funcs.printf, close_paren);
    }

    virtual bool canConvertTo(ConcreteCompilerType* other_type) {
        return (other_type == UNKNOWN || other_type == BOXED_TUPLE);
    }

    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) {
        assert(other_type == UNKNOWN || other_type == BOXED_TUPLE);

        VEC* v = var->getValue();

        std::vector<ConcreteCompilerVariable*> converted_args;

        llvm::Value* nelts = llvm::ConstantInt::get(g.i64, v->size(), false);
        llvm::Value* alloca = emitter.getBuilder()->CreateAlloca(g.llvm_value_type_ptr, nelts);
        for (int i = 0; i < v->size(); i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(alloca, i);
            ConcreteCompilerVariable* converted = (*v)[i]->makeConverted(emitter, (*v)[i]->getBoxType());
            converted_args.push_back(converted);
            emitter.getBuilder()->CreateStore(converted->getValue(), ptr);
        }

        llvm::Value* rtn = emitter.getBuilder()->CreateCall2(g.funcs.createTuple, nelts, alloca);

        for (int i = 0; i < converted_args.size(); i++) {
            converted_args[i]->decvref(emitter);
        }
        return new ConcreteCompilerVariable(other_type, rtn, true);
    }

    virtual ConcreteCompilerType* getBoxType() { return BOXED_TUPLE; }

    virtual ConcreteCompilerType* getConcreteType() { return BOXED_TUPLE; }

    static TupleType* make(const std::vector<CompilerType*>& elt_types) { return new TupleType(elt_types); }

    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* slice) {
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
                    llvm::CallSite call = emitter.createCall2(info.exc_info, g.funcs.raiseAttributeErrorStr,
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

    virtual ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* var) {
        return new ConcreteCompilerVariable(INT, getConstantInt(var->getValue()->size(), g.i64), true);
    }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) {
        return BOXED_TUPLE->getattrType(attr, cls_only);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        ConcreteCompilerVariable* converted = var->makeConverted(emitter, UNKNOWN);
        CompilerVariable* rtn = converted->binexp(emitter, info, rhs, op_type, exp_type);
        converted->decvref(emitter);
        return rtn;
    }

    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                                       bool clsonly, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        return makeConverted(emitter, var, getConcreteType())
            ->callattr(emitter, info, attr, clsonly, argspec, args, keyword_names);
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
    std::string debugName() { return "undefType"; }

    llvm::Type* llvmType() override {
        // Something that no one else uses...
        // TODO should do something more rare like a unique custom struct
        return llvm::Type::getInt16Ty(g.context);
    }

    virtual CompilerVariable* call(IREmitter& emitter, const OpInfo& info, VAR* var, ArgPassSpec argspec,
                                   const std::vector<CompilerVariable*>& args,
                                   const std::vector<const std::string*>* keyword_names) {
        return undefVariable();
    }
    virtual void drop(IREmitter& emitter, VAR* var) {}
    virtual void grab(IREmitter& emitter, VAR* var) {}
    virtual CompilerVariable* dup(VAR* v, DupCache& cache) {
        // TODO copied from UnknownType
        auto& rtn = cache[v];
        if (rtn == NULL) {
            rtn = new VAR(this, v->getValue(), v->isGrabbed());
            while (rtn->getVrefs() < v->getVrefs())
                rtn->incvref();
        }
        return rtn;
    }
    virtual void print(IREmitter& emitter, const OpInfo& info, VAR* var) {}
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* var, ConcreteCompilerType* other_type) {
        llvm::Value* v = llvm::UndefValue::get(other_type->llvmType());
        return new ConcreteCompilerVariable(other_type, v, true);
    }
    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                                      bool cls_only) {
        return undefVariable();
    }

    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* var, const std::string* attr,
                                       bool clsonly, ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        return undefVariable();
    }

    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) {
        return UNDEF;
    }

    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* var) {
        return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(g.i1), true);
    }

    CompilerVariable* binexp(IREmitter& emitter, const OpInfo& info, VAR* var, CompilerVariable* rhs,
                             AST_TYPE::AST_TYPE op_type, BinExpType exp_type) override {
        return undefVariable();
    }

    virtual ConcreteCompilerType* getBoxType() { return UNKNOWN; }

    virtual ConcreteCompilerType* getConcreteType() { return this; }

    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) { return UNDEF; }

    virtual bool canConvertTo(ConcreteCompilerType* other_type) { return true; }

    virtual BoxedClass* guaranteedClass() { return NULL; }
} _UNDEF;
CompilerType* UNDEF = &_UNDEF;

ConcreteCompilerVariable* undefVariable() {
    return new ConcreteCompilerVariable(&_UNDEF, llvm::UndefValue::get(_UNDEF.llvmType()), true);
}


ConcreteCompilerType* LIST, *SLICE, *MODULE, *DICT, *SET;

} // namespace pyston
