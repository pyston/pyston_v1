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

#include <cstdio>
#include <sstream>

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/raw_ostream.h"

#include "core/options.h"
#include "core/types.h"

#include "codegen/compvars.h"
#include "codegen/gcbuilder.h"
#include "codegen/patchpoints.h"
#include "codegen/irgen.h"
#include "codegen/irgen/util.h"

#include "runtime/objmodel.h"
#include "runtime/int.h"
#include "runtime/float.h"
#include "runtime/types.h"

namespace pyston {

std::string ValuedCompilerType<llvm::Value*>::debugName() {
    std::string rtn;
    llvm::raw_string_ostream os(rtn);
    llvmType()->print(os);
    return rtn;
}

struct RawInstanceMethod {
    CompilerVariable *obj, *func;

    RawInstanceMethod(CompilerVariable *obj, CompilerVariable *func) : obj(obj), func(func) {
        obj->incvref();
        func->incvref();
    }
};

class InstanceMethodType : public ValuedCompilerType<RawInstanceMethod*> {
    private:
        static std::unordered_map<std::pair<CompilerType*, CompilerType*>, InstanceMethodType*> made;

        CompilerType *obj_type, *function_type;
        InstanceMethodType(CompilerType *obj_type, CompilerType *function_type) : obj_type(obj_type), function_type(function_type) {
        }

        void checkVar(VAR *var) {
#ifndef NDEBUG
            RawInstanceMethod *val = var->getValue();
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
            CompilerVariable* rtn = new ValuedCompilerVariable<RawInstanceMethod*>(InstanceMethodType::get(obj->getType(), func->getType()), new RawInstanceMethod(obj, func), true);
            return rtn;
        }

        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
            std::vector<CompilerType*> new_args(arg_types);
            new_args.insert(new_args.begin(), obj_type);
            return function_type->callType(new_args);
        }

        std::string debugName() {
            return "instanceMethod(" + obj_type->debugName() + " ; " + function_type->debugName() + ")";
        }
        virtual void drop(IREmitter &emitter, VAR *var) {
            checkVar(var);
            RawInstanceMethod *val = var->getValue();
            val->obj->decvref(emitter);
            val->func->decvref(emitter);
            delete val;
        }
        virtual CompilerVariable* call(IREmitter &emitter, ValuedCompilerVariable<RawInstanceMethod*> *var, const std::vector<CompilerVariable*>& args) {
            std::vector<CompilerVariable*> new_args;
            new_args.push_back(var->getValue()->obj);
            new_args.insert(new_args.end(), args.begin(), args.end());
            return var->getValue()->func->call(emitter, new_args);
        }
        virtual bool canConvertTo(ConcreteCompilerType* other_type) {
            return other_type == UNKNOWN;
        }
        virtual ConcreteCompilerType* getConcreteType() {
            return typeFromClass(instancemethod_cls);
        }
        virtual ConcreteCompilerType* getBoxType() {
            return getConcreteType();
        }
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, VAR* var, ConcreteCompilerType* other_type) {
            checkVar(var);
            assert(other_type == UNKNOWN || other_type == typeFromClass(instancemethod_cls));

            RawInstanceMethod *im = var->getValue();
            assert(im->obj);
            assert(im->func);
            ConcreteCompilerVariable *obj = im->obj->makeConverted(emitter, UNKNOWN);
            ConcreteCompilerVariable *func = im->func->makeConverted(emitter, UNKNOWN);

            llvm::Value *boxed = emitter.getBuilder()->CreateCall2(g.funcs.boxInstanceMethod, obj->getValue(), func->getValue());

            obj->decvref(emitter);
            func->decvref(emitter);

            return new ConcreteCompilerVariable(other_type, boxed, true);
        }
        virtual CompilerVariable* dup(VAR *var, DupCache &cache) {
            checkVar(var);

            CompilerVariable *rtn = cache[var];
            if (rtn == NULL) {
                RawInstanceMethod *im = var->getValue();
                RawInstanceMethod *new_im = new RawInstanceMethod(im->obj->dup(cache), im->func->dup(cache));
                rtn = new VAR(this, new_im, var->isGrabbed());
            }
            return rtn;
        }
};
std::unordered_map<std::pair<CompilerType*, CompilerType*>, InstanceMethodType*> InstanceMethodType::made;

ConcreteCompilerVariable* ConcreteCompilerType::makeConverted(IREmitter &emitter, ConcreteCompilerVariable *var, ConcreteCompilerType* other_type) {
    if (other_type == this) {
        var->incvref();
        return var;
    }
    printf("makeConverted not defined for %s\n", debugName().c_str());
    abort();
}
CompilerVariable* ConcreteCompilerType::dup(ConcreteCompilerVariable *v, DupCache &cache) {
    auto &rtn = cache[v];
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

        virtual std::string debugName() {
            return "AnyBox";
        }

        virtual void drop(IREmitter &emitter, VAR *var) {
            emitter.getGC()->dropPointer(emitter, var->getValue());
        }
        virtual void grab(IREmitter &emitter, VAR *var) {
            emitter.getGC()->grabPointer(emitter, var->getValue());
        }

        virtual bool isFitBy(BoxedClass *c) {
            return true;
        }

        // XXX should get rid of this implementation and have it just do print o.__repr__()
        virtual void print(IREmitter &emitter, ConcreteCompilerVariable *var) {
            emitter.getBuilder()->CreateCall(g.funcs.print, var->getValue());
        }

        virtual CompilerVariable* getattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr);
        virtual CompilerVariable* call(IREmitter &emitter, ConcreteCompilerVariable *var, const std::vector<CompilerVariable*> &args);
        virtual CompilerVariable* callattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*> &args);
        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, ConcreteCompilerVariable *var);

        void setattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr, CompilerVariable *v) {
            llvm::Constant* ptr = getStringConstantPtr(attr + '\0');
            ConcreteCompilerVariable *converted = v->makeConverted(emitter, UNKNOWN);
            //g.funcs.setattr->dump();
            //var->getValue()->dump(); llvm::errs() << '\n';
            //ptr->dump(); llvm::errs() << '\n';
            //converted->getValue()->dump(); llvm::errs() << '\n';
            bool do_patchpoint = ENABLE_ICSETATTRS && emitter.getTarget() != IREmitter::INTERPRETER;
            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createSetattrPatchpoint(emitter.currentFunction());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(var->getValue());
                llvm_args.push_back(ptr);
                llvm_args.push_back(converted->getValue());

                emitter.createPatchpoint(pp, (void*)pyston::setattr, llvm_args);
            } else {
                emitter.getBuilder()->CreateCall3(g.funcs.setattr, var->getValue(), ptr, converted->getValue());
            }
            converted->decvref(emitter);
        }

        virtual llvm::Value* makeClassCheck(IREmitter &emitter, ConcreteCompilerVariable *var, BoxedClass *cls) {
            assert(var->getValue()->getType() == g.llvm_value_type_ptr);
            // TODO this is brittle: directly embeds the position of the class object:
            llvm::Value* cls_ptr = emitter.getBuilder()->CreateConstInBoundsGEP2_32(var->getValue(), 0, 1);
            llvm::Value* cls_value = emitter.getBuilder()->CreateLoad(cls_ptr);
            assert(cls_value->getType() == g.llvm_class_type_ptr);
            llvm::Value* rtn = emitter.getBuilder()->CreateICmpEQ(cls_value, embedConstantPtr(cls, g.llvm_class_type_ptr));
            return rtn;
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            return UNKNOWN;
        }
        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
            return UNKNOWN;
        }
        virtual BoxedClass* guaranteedClass() {
            return NULL;
        }
        virtual ConcreteCompilerType* getBoxType() {
            return this;
        }
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerVariable *var, ConcreteCompilerType* other_type) {
            if (other_type == this) {
                var->incvref();
                return var;
            }
            fprintf(stderr, "Can't convert unknown to %s...\n", other_type->debugName().c_str());
            abort();
        }

        virtual ConcreteCompilerVariable* len(IREmitter &emitter, ConcreteCompilerVariable *var) {
            bool do_patchpoint = ENABLE_ICGENERICS && emitter.getTarget() != IREmitter::INTERPRETER;
            llvm::Value* rtn;
            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createGenericPatchpoint(emitter.currentFunction(), true, 160);

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(var->getValue());

                rtn = emitter.createPatchpoint(pp, (void*)pyston::unboxedLen, llvm_args);
            } else {
                rtn = emitter.getBuilder()->CreateCall(g.funcs.unboxedLen, var->getValue());
            }
            assert(rtn->getType() == g.i64);
            return new ConcreteCompilerVariable(INT, rtn, true);
        }

        virtual CompilerVariable* getitem(IREmitter &emitter, ConcreteCompilerVariable *var, CompilerVariable *slice) {
            ConcreteCompilerVariable *converted_slice = slice->makeConverted(emitter, slice->getBoxType());

            bool do_patchpoint = ENABLE_ICGETITEMS && emitter.getTarget() != IREmitter::INTERPRETER;
            llvm::Value *rtn;
            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createGetitemPatchpoint(emitter.currentFunction());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(var->getValue());
                llvm_args.push_back(converted_slice->getValue());

                llvm::Value* uncasted = emitter.createPatchpoint(pp, (void*)pyston::getitem, llvm_args);
                rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            } else {
                rtn = emitter.getBuilder()->CreateCall2(g.funcs.getitem,
                        var->getValue(), converted_slice->getValue());
            }

            converted_slice->decvref(emitter);
            return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
        }
};

ConcreteCompilerType *UNKNOWN = new UnknownType();

CompilerVariable* UnknownType::getattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr) {
    llvm::Constant* ptr = getStringConstantPtr(attr + '\0');

    llvm::Value* rtn_val = NULL;

    bool do_patchpoint = ENABLE_ICGETATTRS && emitter.getTarget() != IREmitter::INTERPRETER;
    if (do_patchpoint) {
        PatchpointSetupInfo *pp = patchpoints::createGetattrPatchpoint(emitter.currentFunction());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());
        llvm_args.push_back(ptr);

        llvm::Value* uncasted = emitter.createPatchpoint(pp, (void*)pyston::getattr, llvm_args);
        rtn_val = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        rtn_val = emitter.getBuilder()->CreateCall2(g.funcs.getattr, var->getValue(), ptr);
    }
    return new ConcreteCompilerVariable(UNKNOWN, rtn_val, true);
}

static ConcreteCompilerVariable* _call(IREmitter &emitter, llvm::Value* func, void* func_addr, const std::vector<llvm::Value*> other_args, const std::vector<CompilerVariable*> args, ConcreteCompilerType *rtn_type) {
    std::vector<BoxedClass*> guaranteed_classes;
    std::vector<ConcreteCompilerVariable*> converted_args;
    for (int i = 0; i < args.size(); i++) {
        converted_args.push_back(args[i]->makeConverted(emitter, args[i]->getBoxType()));
        guaranteed_classes.push_back(converted_args.back()->guaranteedClass());
    }

    std::vector<llvm::Value*> llvm_args;
    llvm_args.insert(llvm_args.end(), other_args.begin(), other_args.end());

    if (args.size() >= 1) {
        llvm_args.push_back(converted_args[0]->getValue());
    }
    if (args.size() >= 2) {
        llvm_args.push_back(converted_args[1]->getValue());
    }
    if (args.size() >= 3) {
        llvm_args.push_back(converted_args[2]->getValue());
    }

    llvm::Value *mallocsave = NULL;
    if (args.size() >= 4) {
        llvm::Value *arg_array;

        llvm::Value *n_bytes = getConstantInt((args.size() - 3) * sizeof(Box*), g.i64);
        mallocsave = emitter.getBuilder()->CreateCall(g.funcs.malloc, n_bytes);
        arg_array = emitter.getBuilder()->CreateBitCast(mallocsave, g.llvm_value_type_ptr->getPointerTo());

        for (int i = 3; i < args.size(); i++) {
            llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, i - 3);
            emitter.getBuilder()->CreateStore(converted_args[i]->getValue(), ptr);
        }
        llvm_args.push_back(arg_array);
    }

    //f->dump();
    //for (int i = 0; i < llvm_args.size(); i++) {
        //llvm_args[i]->dump();
        //llvm::errs() << '\n';
    //}

    llvm::Value* rtn;

    bool do_patchpoint = ENABLE_ICCALLSITES && emitter.getTarget() != IREmitter::INTERPRETER && (func_addr == runtimeCall || func_addr == pyston::callattr);
    if (do_patchpoint) {
        assert(func_addr);

        PatchpointSetupInfo *pp = patchpoints::createCallsitePatchpoint(emitter.currentFunction(), args.size());

        llvm::Value* uncasted = emitter.createPatchpoint(pp, func_addr, llvm_args);

        assert(llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(func->getType())->getElementType())->getReturnType() == g.llvm_value_type_ptr);
        rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
    } else {
        //func->dump();
        //for (auto a : llvm_args) {
            //a->dump();
        //}
        //printf("%ld %ld\n", llvm_args.size(), args.size());
        rtn = emitter.getBuilder()->CreateCall(func, llvm_args);
    }

    if (mallocsave) {
        llvm::Value *l_free = embedConstantPtr((void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
        emitter.getBuilder()->CreateCall(l_free, mallocsave);
    }

    for (int i = 0; i < args.size(); i++) {
        converted_args[i]->decvref(emitter);
    }

    assert(rtn->getType() == rtn_type->llvmType());
    return new ConcreteCompilerVariable(rtn_type, rtn, true);
}

CompilerVariable* UnknownType::call(IREmitter &emitter, ConcreteCompilerVariable *var, const std::vector<CompilerVariable*> &args) {
    llvm::Value* func;
    if (args.size() == 0)
        func = g.funcs.runtimeCall0;
    else if (args.size() == 1)
        func = g.funcs.runtimeCall1;
    else if (args.size() == 2)
        func = g.funcs.runtimeCall2;
    else if (args.size() == 3)
        func = g.funcs.runtimeCall3;
    else
        func = g.funcs.runtimeCall;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());

    llvm::Value *nargs = llvm::ConstantInt::get(g.i64, args.size(), false);
    other_args.push_back(nargs);
    return _call(emitter, func, (void*)runtimeCall, other_args, args, UNKNOWN);
}

CompilerVariable* UnknownType::callattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*> &args) {
    llvm::Value* func;
    if (args.size() == 0)
        func = g.funcs.callattr0;
    else if (args.size() == 1)
        func = g.funcs.callattr1;
    else if (args.size() == 2)
        func = g.funcs.callattr2;
    else if (args.size() == 3)
        func = g.funcs.callattr3;
    else
        func = g.funcs.callattr;

    std::vector<llvm::Value*> other_args;
    other_args.push_back(var->getValue());
    other_args.push_back(embedConstantPtr(&attr, g.llvm_str_type_ptr));
    other_args.push_back(getConstantInt(clsonly, g.i1));

    llvm::Value *nargs = llvm::ConstantInt::get(g.i64, args.size(), false);
    other_args.push_back(nargs);
    return _call(emitter, func, (void*)pyston::callattr, other_args, args, UNKNOWN);
}

ConcreteCompilerVariable* UnknownType::nonzero(IREmitter &emitter, ConcreteCompilerVariable *var) {
    bool do_patchpoint = ENABLE_ICNONZEROS && emitter.getTarget() != IREmitter::INTERPRETER;
    llvm::Value* rtn_val;
    if (do_patchpoint) {
        PatchpointSetupInfo *pp = patchpoints::createNonzeroPatchpoint(emitter.currentFunction());

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(var->getValue());

        llvm::Value* uncasted = emitter.createPatchpoint(pp, (void*)pyston::nonzero, llvm_args);
        rtn_val = emitter.getBuilder()->CreateTrunc(uncasted, g.i1);
    } else {
        rtn_val = emitter.getBuilder()->CreateCall(g.funcs.nonzero, var->getValue());
    }
    return new ConcreteCompilerVariable(BOOL, rtn_val, true);
}

CompilerVariable* makeFunction(IREmitter &emitter, CLFunction *f) {
    // Unlike the CLFunction*, which can be shared between recompilations, the Box* around it
    // should be created anew every time the functiondef is encountered
    llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxCLFunction, embedConstantPtr(f, g.llvm_clfunction_type_ptr));
    return new ConcreteCompilerVariable(typeFromClass(function_cls), boxed, true);
}

class AbstractFunctionType : public CompilerType {
    public:
        struct Sig {
            std::vector<ConcreteCompilerType*> arg_types;
            CompilerType* rtn_type;
        };

    private:
        std::vector<Sig*> sigs;
        AbstractFunctionType(const std::vector<Sig*> &sigs) : sigs(sigs) {
        }
    public:
        virtual std::string debugName() {
            return "<AbstractFunctionType>";
        }

        virtual ConcreteCompilerType* getConcreteType() {
            return UNKNOWN;
        }

        virtual ConcreteCompilerType* getBoxType() {
            return UNKNOWN;
        }

        virtual bool canConvertTo(ConcreteCompilerType* other_type) {
            return other_type == UNKNOWN;
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            return UNDEF;
        }

        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
            for (int i = 0; i < sigs.size(); i++) {
                Sig* sig = sigs[i];
                if (sig->arg_types.size() != arg_types.size())
                    continue;

                bool works = true;
                for (int j = 0; j < sig->arg_types.size(); j++) {
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

        virtual BoxedClass* guaranteedClass() {
            return NULL;
        }

        static CompilerType* fromRT(BoxedFunction* rtfunc, bool stripfirst) {
            std::vector<Sig*> sigs;
            CLFunction *clf = rtfunc->f;

            for (int i = 0; i < clf->versions.size(); i++) {
                CompiledFunction *cf = clf->versions[i];

                FunctionSignature *fsig = cf->sig;

                Sig* type_sig = new Sig();
                type_sig->rtn_type = fsig->rtn_type;

                if (stripfirst) {
                    assert(fsig->arg_types.size() >= 1);
                    type_sig->arg_types.insert(type_sig->arg_types.end(), fsig->arg_types.begin()+1, fsig->arg_types.end());
                } else {
                    type_sig->arg_types.insert(type_sig->arg_types.end(), fsig->arg_types.begin(), fsig->arg_types.end());
                }
                sigs.push_back(type_sig);
            }
            return get(sigs);
        }

        static CompilerType* get(const std::vector<Sig*> &sigs) {
            return new AbstractFunctionType(sigs);
        }
};

class IntType : public ConcreteCompilerType {
    public:
        IntType() {}

        llvm::Type* llvmType() { return g.i64; }

        virtual bool isFitBy(BoxedClass *c) {
            return false;
        }

        virtual void drop(IREmitter &emitter, ConcreteCompilerVariable *var) {
            // pass
        }
        virtual void grab(IREmitter &emitter, ConcreteCompilerVariable *var) {
            // pass
        }

        virtual void print(IREmitter &emitter, ConcreteCompilerVariable *var) {
            assert(var->getValue()->getType() == g.i64);

            llvm::Constant* int_fmt = getStringConstantPtr("%ld");;
            emitter.getBuilder()->CreateCall2(g.funcs.printf, int_fmt, var->getValue());
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            static std::vector<AbstractFunctionType::Sig*> sigs;
            if (sigs.size() == 0) {
                AbstractFunctionType::Sig *int_sig = new AbstractFunctionType::Sig();
                int_sig->rtn_type = INT;
                int_sig->arg_types.push_back(INT);
                sigs.push_back(int_sig);

                AbstractFunctionType::Sig *unknown_sig = new AbstractFunctionType::Sig();
                unknown_sig->rtn_type = UNKNOWN;
                unknown_sig->arg_types.push_back(UNKNOWN);
                sigs.push_back(unknown_sig);
            }

            if (attr == "__add__" || attr == "__sub__" || attr == "__mod__" || attr == "__mul__" || attr == "__lshift__" || attr == "__rshift__" || attr == "__div__" || attr == "__pow__" || attr == "__floordiv__" || attr == "__and__" || attr == "__or__" || attr == "__xor__") {
                return AbstractFunctionType::get(sigs);
            }

            return BOXED_INT->getattrType(attr);
        }

        virtual CompilerVariable* callattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*>& args) {
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable* rtn = converted->callattr(emitter, attr, clsonly, args);
            converted->decvref(emitter);
            return rtn;
        }

        virtual CompilerVariable* getattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr) {
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable* rtn = converted->getattr(emitter, attr);
            converted->decvref(emitter);
            return rtn;
        }

        virtual void setattr(IREmitter &emitter, VAR* var, const std::string &attr, CompilerVariable *v) {
            llvm::CallInst *call = emitter.getBuilder()->CreateCall2(g.funcs.raiseAttributeErrorStr, getStringConstantPtr("int\0"), getStringConstantPtr(attr + '\0'));
            call->setDoesNotReturn();
        }

        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerVariable *var, ConcreteCompilerType* other_type) {
            if (other_type == this) {
                var->incvref();
                return var;
            } else if (other_type == UNKNOWN || other_type == BOXED_INT) {
                llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxInt, var->getValue());
                return new ConcreteCompilerVariable(other_type, boxed, true);
            } else {
                printf("Don't know how to convert i64 to %s\n", other_type->debugName().c_str());
                abort();
            }
        }

        virtual CompilerVariable *getitem(IREmitter &emitter, VAR *var, CompilerVariable *slice) {
            ConcreteCompilerVariable *converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable *rtn = converted->getitem(emitter, slice);
            converted->decvref(emitter);
            return rtn;
        }

        virtual ConcreteCompilerVariable* len(IREmitter &emitter, VAR *var) {
            llvm::CallInst* call = emitter.getBuilder()->CreateCall(g.funcs.raiseNotIterableError, getStringConstantPtr("int"));
            call->setDoesNotReturn();
            return new ConcreteCompilerVariable(INT, llvm::UndefValue::get(g.i64), true);
        }

        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, ConcreteCompilerVariable *var) {
            llvm::Value *cmp = emitter.getBuilder()->CreateICmpNE(var->getValue(), llvm::ConstantInt::get(g.i64, 0, false));
            return new ConcreteCompilerVariable(BOOL, cmp, true);
        }

        virtual ConcreteCompilerType* getBoxType() {
            return BOXED_INT;
        }
} _INT;
ConcreteCompilerType *INT = &_INT;

CompilerVariable* makeInt(int64_t n) {
    return new ConcreteCompilerVariable(INT, llvm::ConstantInt::get(g.i64, n, true), true);
}

class FloatType : public ConcreteCompilerType {
    public:
        FloatType() {}

        llvm::Type* llvmType() { return g.double_; }

        virtual bool isFitBy(BoxedClass *c) {
            return false;
        }

        virtual void drop(IREmitter &emitter, ConcreteCompilerVariable *var) {
            // pass
        }
        virtual void grab(IREmitter &emitter, ConcreteCompilerVariable *var) {
            // pass
        }

        virtual void print(IREmitter &emitter, ConcreteCompilerVariable *var) {
            assert(var->getValue()->getType() == g.double_);

            emitter.getBuilder()->CreateCall(g.funcs.printFloat, var->getValue());
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            static std::vector<AbstractFunctionType::Sig*> sigs;
            if (sigs.size() == 0) {
                AbstractFunctionType::Sig *float_sig = new AbstractFunctionType::Sig();
                float_sig->rtn_type = FLOAT;
                float_sig->arg_types.push_back(FLOAT);
                sigs.push_back(float_sig);

                AbstractFunctionType::Sig *int_sig = new AbstractFunctionType::Sig();
                int_sig->rtn_type = FLOAT;
                int_sig->arg_types.push_back(INT);
                sigs.push_back(int_sig);

                AbstractFunctionType::Sig *unknown_sig = new AbstractFunctionType::Sig();
                unknown_sig->rtn_type = UNKNOWN;
                unknown_sig->arg_types.push_back(UNKNOWN);
                sigs.push_back(unknown_sig);
            }

            if (attr == "__add__" || attr == "__sub__" || attr == "__mul__" || attr == "__div__" || attr == "__pow__" || attr == "__floordiv__" || attr == "__mod__" || attr == "__pow__") {
                return AbstractFunctionType::get(sigs);
            }

            return BOXED_FLOAT->getattrType(attr);
        }

        virtual CompilerVariable* getattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr) {
            ConcreteCompilerVariable* converted = var->makeConverted(emitter, BOXED_INT);
            CompilerVariable* rtn = converted->getattr(emitter, attr);
            converted->decvref(emitter);
            return rtn;
        }

        virtual void setattr(IREmitter &emitter, VAR* var, const std::string &attr, CompilerVariable *v) {
            llvm::CallInst* call = emitter.getBuilder()->CreateCall2(g.funcs.raiseAttributeErrorStr, getStringConstantPtr("float\0"), getStringConstantPtr(attr + '\0'));
            call->setDoesNotReturn();
        }

        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerVariable *var, ConcreteCompilerType* other_type) {
            if (other_type == this) {
                var->incvref();
                return var;
            } else if (other_type == UNKNOWN || other_type == BOXED_FLOAT) {
                llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxFloat, var->getValue());
                return new ConcreteCompilerVariable(other_type, boxed, true);
            } else {
                printf("Don't know how to convert float to %s\n", other_type->debugName().c_str());
                abort();
            }
        }

        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, ConcreteCompilerVariable *var) {
            llvm::Value *cmp = emitter.getBuilder()->CreateFCmpUNE(var->getValue(), llvm::ConstantFP::get(g.double_, 0));
            return new ConcreteCompilerVariable(BOOL, cmp, true);
        }
        virtual ConcreteCompilerType* getBoxType() {
            return BOXED_FLOAT;
        }
} _FLOAT;
ConcreteCompilerType *FLOAT = &_FLOAT;

CompilerVariable* makeFloat(double d) {
    return new ConcreteCompilerVariable(FLOAT, llvm::ConstantFP::get(g.double_, d), true);
}

class KnownClassobjType : public ValuedCompilerType<BoxedClass*> {
    private:
        BoxedClass *cls;

        static std::unordered_map<BoxedClass*, KnownClassobjType*> made;

        KnownClassobjType(BoxedClass *cls) : cls(cls) {
            assert(cls);
        }

    public:
        virtual std::string debugName() {
            return "class '" + *getNameOfClass(cls) + "'";
        }

        static KnownClassobjType* fromClass(BoxedClass* cls) {
            KnownClassobjType* &rtn = made[cls];
            if (rtn == NULL) {
                rtn = new KnownClassobjType(cls);
            }
            return rtn;
        }

        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
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
        BoxedClass *cls;

        static std::unordered_map<BoxedClass*, NormalObjectType*> made;

        NormalObjectType(BoxedClass *cls) : cls(cls) {
            //ASSERT(!isUserDefined(cls) && "instances of user-defined classes can change their __class__, plus even if they couldn't we couldn't statically resolve their attributes", "%s", getNameOfClass(cls)->c_str());

            assert(cls);
        }

    public:
        llvm::Type* llvmType() {
            return g.llvm_value_type_ptr;
        }
        std::string debugName() {
            assert(cls);
            // TODO add getTypeName

            return "NormalType(" + *getNameOfClass(cls) + ")";
        }
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerVariable *var, ConcreteCompilerType* other_type) {
            if (other_type == this) {
                var->incvref();
                return var;
            }
            assert(other_type == UNKNOWN);
            return new ConcreteCompilerVariable(UNKNOWN, var->getValue(), false);
            //return (new ConcreteCompilerVariable(UNKNOWN, var->getValue(), false))->split(emitter);
        }

        virtual void drop(IREmitter &emitter, VAR *var) {
            emitter.getGC()->dropPointer(emitter, var->getValue());
        }
        virtual void grab(IREmitter &emitter, VAR *var) {
            emitter.getGC()->grabPointer(emitter, var->getValue());
        }

        virtual bool isFitBy(BoxedClass *c) {
            return c == cls;
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            if (cls->is_constant && !cls->hasattrs) {
                Box* rtattr = cls->peekattr(attr);
                if (rtattr == NULL)
                    return UNDEF;

                RELEASE_ASSERT(rtattr, "%s.%s", debugName().c_str(), attr.c_str());
                if (rtattr->cls == function_cls) {
                    return AbstractFunctionType::fromRT(static_cast<BoxedFunction*>(rtattr), true);
                    //return typeFromClass(instancemethod_cls);
                } else {
                    return typeFromClass(rtattr->cls);
                }
            }

            return UNKNOWN;
        }
        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
            return UNKNOWN;
        }

        CompilerVariable* getattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr) {
            //printf("%s.getattr %s\n", debugName().c_str(), attr.c_str());
            if (cls->is_constant && !cls->hasattrs) {
                Box* rtattr = cls->peekattr(attr);
                if (rtattr == NULL) {
                    llvm::CallInst *call = emitter.getBuilder()->CreateCall2(g.funcs.raiseAttributeErrorStr, getStringConstantPtr(*getNameOfClass(cls) + "\0"), getStringConstantPtr(attr + '\0'));
                    call->setDoesNotReturn();
                    return undefVariable();
                }

                ASSERT(rtattr, "%s.%s", debugName().c_str(), attr.c_str());
                if (rtattr->cls == function_cls) {
                    CompilerVariable* clattr = new ConcreteCompilerVariable(typeFromClass(function_cls), embedConstantPtr(rtattr, g.llvm_value_type_ptr), false);
                    return InstanceMethodType::makeIM(var, clattr);
                }
            }

            // TODO could specialize more since we know the class already
            return UNKNOWN->getattr(emitter, var, attr);
        }

        void setattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr, CompilerVariable *v) {
            return UNKNOWN->setattr(emitter, var, attr, v);
        }

        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, ConcreteCompilerVariable *var) {
            if (cls == bool_cls) {
                llvm::Value *unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, var->getValue());
                assert(unboxed->getType() == g.i1);
                return new ConcreteCompilerVariable(BOOL, unboxed, true);
            }

            ConcreteCompilerVariable *converted = var->makeConverted(emitter, UNKNOWN);
            ConcreteCompilerVariable *rtn = converted->nonzero(emitter);
            converted->decvref(emitter);
            return rtn;
        }

        virtual void print(IREmitter &emitter, ConcreteCompilerVariable *var) {
            ConcreteCompilerVariable *converted = var->makeConverted(emitter, UNKNOWN);
            converted->print(emitter);
            converted->decvref(emitter);
        }

        virtual CompilerVariable* getitem(IREmitter &emitter, ConcreteCompilerVariable *var, CompilerVariable *slice) {
            return UNKNOWN->getitem(emitter, var, slice);
        }

        virtual ConcreteCompilerVariable* len(IREmitter &emitter, ConcreteCompilerVariable *var) {
            return UNKNOWN->len(emitter, var);
        }

        virtual CompilerVariable* call(IREmitter &emitter, ConcreteCompilerVariable *var, const std::vector<CompilerVariable*>& args) {
            ConcreteCompilerVariable *converted = var->makeConverted(emitter, UNKNOWN);
            CompilerVariable *rtn = converted->call(emitter, args);
            converted->decvref(emitter);
            return rtn;
        }

        virtual CompilerVariable* callattr(IREmitter &emitter, ConcreteCompilerVariable *var, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*>& args) {
            if (cls->is_constant && !cls->hasattrs) {
                Box* rtattr = cls->peekattr(attr);
                if (rtattr == NULL) {
                    llvm::CallInst *call = emitter.getBuilder()->CreateCall2(g.funcs.raiseAttributeErrorStr, getStringConstantPtr(debugName() + '\0'), getStringConstantPtr(attr + '\0'));
                    call->setDoesNotReturn();
                    return undefVariable();
                }

                if (rtattr->cls == function_cls) {
                    CLFunction *cl = unboxRTFunction(rtattr);
                    assert(cl);

                    CompiledFunction *cf = NULL;
                    int nsig_args = 0;
                    bool found = false;
                    // TODO have to find the right version.. similar to resolveclfunc?
                    for (int i = 0; i < cl->versions.size(); i++) {
                        cf = cl->versions[i];
                        nsig_args = cf->sig->arg_types.size();
                        if (nsig_args != args.size() + 1) {
                            continue;
                        }

                        bool fits = true;
                        for (int j = 1; j < nsig_args; j++) {
                            //if (cf->sig->arg_types[j] != UNKNOWN) {
                            //if (cf->sig->arg_types[j]->isFitBy(args[j-1]->guaranteedClass())) {
                            if (!args[j-1]->canConvertTo(cf->sig->arg_types[j])) {
                                printf("Can't use version %d since arg %d (%s) doesn't fit into sig arg of %s\n", i, j, args[j-1]->getType()->debugName().c_str(), cf->sig->arg_types[j]->debugName().c_str());
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
                    assert(nsig_args == args.size() + 1);
                    assert(!cf->is_interpreted);
                    assert(cf->code);

                    std::vector<llvm::Type*> arg_types;
                    for (int i = 0; i < nsig_args; i++) {
                        // TODO support passing unboxed values as arguments
                        assert(cf->sig->arg_types[i]->llvmType() == g.llvm_value_type_ptr);

                        arg_types.push_back(g.llvm_value_type_ptr);
                        if (i == 3) {
                            arg_types.push_back(g.llvm_value_type_ptr->getPointerTo());
                            break;
                        }
                    }
                    llvm::FunctionType *ft = llvm::FunctionType::get(cf->sig->rtn_type->llvmType(), arg_types, false);

                    llvm::Value *linked_function = embedConstantPtr(cf->code, ft->getPointerTo());

                    std::vector<CompilerVariable*> new_args;
                    new_args.push_back(var);
                    new_args.insert(new_args.end(), args.begin(), args.end());

                    std::vector<llvm::Value*> other_args;

                    ConcreteCompilerVariable* rtn = _call(emitter, linked_function, cf->code, other_args, new_args, cf->sig->rtn_type);
                    assert(rtn->getType() == cf->sig->rtn_type);

                    assert(cf->sig->rtn_type != BOXED_INT);
                    ASSERT(cf->sig->rtn_type != BOXED_BOOL, "%p", cf->code);
                    assert(cf->sig->rtn_type != BOXED_FLOAT);

                    return rtn;
                }
            }

            ConcreteCompilerVariable *converted = var->makeConverted(emitter, UNKNOWN);
            CompilerVariable *rtn = converted->callattr(emitter, attr, clsonly, args);
            converted->decvref(emitter);
            return rtn;
        }

        static NormalObjectType* fromClass(BoxedClass* cls) {
            NormalObjectType* &rtn = made[cls];
            if (rtn == NULL) {
                rtn = new NormalObjectType(cls);
            }
            return rtn;
        }

        virtual BoxedClass* guaranteedClass() {
            return cls;
        }

        virtual ConcreteCompilerType* getBoxType() {
            return this;
        }
};
std::unordered_map<BoxedClass*, NormalObjectType*> NormalObjectType::made;
ConcreteCompilerType *STR, *BOXED_INT, *BOXED_FLOAT, *BOXED_BOOL, *NONE;

class StrConstantType : public ValuedCompilerType<std::string*> {
    public:
        std::string debugName() {
            return "str_constant";
        }
        virtual ConcreteCompilerType* getConcreteType() {
            return STR;
        }
        virtual ConcreteCompilerType* getBoxType() {
            return STR;
        }
        virtual void drop(IREmitter &emitter, VAR *var) {
            // pass
        }
        virtual void grab(IREmitter &emitter, VAR *var) {
            // pass
        }
        virtual void print(IREmitter &emitter, ValuedCompilerVariable<std::string*> *value) {
            llvm::Constant* ptr = getStringConstantPtr(*(value->getValue()) + '\0');
            llvm::Constant* fmt = getStringConstantPtr("%s\0");
            emitter.getBuilder()->CreateCall2(g.funcs.printf, fmt, ptr);
        }

        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ValuedCompilerVariable<std::string*> *var, ConcreteCompilerType* other_type) {
            assert(other_type == STR || other_type == UNKNOWN);
            llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxStringPtr, embedConstantPtr(var->getValue(), g.llvm_str_type_ptr));
            return new ConcreteCompilerVariable(other_type, boxed, true);
        }

        virtual bool canConvertTo(ConcreteCompilerType *other) {
            return (other == STR || other == UNKNOWN);
        }

        virtual CompilerVariable *getattr(IREmitter &emitter, VAR *var, const std::string &attr) {
            ConcreteCompilerVariable *converted = var->makeConverted(emitter, STR);
            CompilerVariable *rtn = converted->getattr(emitter, attr);
            converted->decvref(emitter);
            return rtn;
        }

        virtual CompilerVariable* callattr(IREmitter &emitter, VAR *var, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*> &args) {
            ConcreteCompilerVariable *converted = var->makeConverted(emitter, STR);
            CompilerVariable *rtn = converted->callattr(emitter, attr, clsonly, args);
            converted->decvref(emitter);
            return rtn;
        }

        virtual CompilerVariable *getitem(IREmitter &emitter, VAR *var, CompilerVariable *slice) {
            ConcreteCompilerVariable *converted = var->makeConverted(emitter, STR);
            CompilerVariable *rtn = converted->getitem(emitter, slice);
            converted->decvref(emitter);
            return rtn;
        }

};
ValuedCompilerType<std::string*> *STR_CONSTANT = new StrConstantType();

CompilerVariable* makeStr(std::string *s) {
    return new ValuedCompilerVariable<std::string*>(STR_CONSTANT, s, true);
}

class VoidType : public ConcreteCompilerType {
    public:
        llvm::Type* llvmType() { return g.void_; }
};
ConcreteCompilerType *VOID = new VoidType();

ConcreteCompilerType* typeFromClass(BoxedClass* c) {
    assert(c);
    return NormalObjectType::fromClass(c);
}

class BoolType : public ConcreteCompilerType {
    public:
        llvm::Type* llvmType() { return g.i1; }

        virtual bool isFitBy(BoxedClass *c) {
            return false;
        }

        virtual void drop(IREmitter &emitter, VAR *var) {
            // pass
        }
        virtual void grab(IREmitter &emitter, VAR *var) {
            // pass
        }
        virtual void print(IREmitter &emitter, ConcreteCompilerVariable *var) {
            assert(var->getValue()->getType() == g.i1);

            llvm::Value* true_str = getStringConstantPtr("True");
            llvm::Value* false_str = getStringConstantPtr("False");
            llvm::Value* selected = emitter.getBuilder()->CreateSelect(var->getValue(), true_str, false_str);
            emitter.getBuilder()->CreateCall(g.funcs.printf, selected);
        }

        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, ConcreteCompilerVariable *var) {
            var->incvref();
            return var;
        }

        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerVariable *var, ConcreteCompilerType *other_type) {
            if (other_type == BOOL) {
                var->incvref();
                return var;
            }

            ASSERT(other_type == UNKNOWN || other_type == BOXED_BOOL, "%s", other_type->debugName().c_str());
            llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxBool, var->getValue());
            return new ConcreteCompilerVariable(other_type, boxed, true);
        }

        virtual ConcreteCompilerType* getBoxType() {
            return BOXED_BOOL;
        }
};
ConcreteCompilerType *BOOL = new BoolType();
CompilerVariable* makeBool(bool b) {
    return new ConcreteCompilerVariable(BOOL, llvm::ConstantInt::get(g.i1, b, false), true);
}

ConcreteCompilerType *BOXED_TUPLE;
class TupleType : public ValuedCompilerType<const std::vector<CompilerVariable*>*> {
    private:
        std::string name;
        const std::vector<CompilerType*> elt_types;

        TupleType(const std::vector<CompilerType*> &elt_types) : elt_types(elt_types) {
            std::ostringstream os("");
            os << "tuple(";
            for (int i = 0; i < elt_types.size(); i++) {
                if (i) os << ", ";
                os << elt_types[i]->debugName();
            }
            os << ")";
            name = os.str();
        }
    public:
        typedef const std::vector<CompilerVariable*> VEC;

        std::string debugName() {
            return name;
        }

        virtual void drop(IREmitter &emitter, VAR *var) {
            const std::vector<CompilerVariable*> *elts = var->getValue();
            for (int i = 0; i < elts->size(); i++) {
                (*elts)[i]->decvref(emitter);
            }
        }

        virtual void grab(IREmitter &emitter, VAR *var) {
            RELEASE_ASSERT(0, "");
        }

        virtual CompilerVariable* dup(VAR *var, DupCache &cache) {
            CompilerVariable* &rtn = cache[var];

            if (rtn == NULL) {
                std::vector<CompilerVariable*> *elts = new std::vector<CompilerVariable*>();
                const std::vector<CompilerVariable*> *orig_elts = var->getValue();

                for (int i = 0; i < orig_elts->size(); i++) {
                    elts->push_back((*orig_elts)[i]->dup(cache));
                }
                rtn = new VAR(this, elts, var->isGrabbed());
            }
            return rtn;
        }

        virtual void print(IREmitter &emitter, VAR *var) {
            llvm::Constant* open_paren = getStringConstantPtr("(");
            llvm::Constant* close_paren = getStringConstantPtr(")");
            llvm::Constant* comma = getStringConstantPtr(",");
            llvm::Constant* comma_space = getStringConstantPtr(", ");

            VEC* v = var->getValue();

            emitter.getBuilder()->CreateCall(g.funcs.printf, open_paren);

            for (int i = 0; i < v->size(); i++) {
                if (i) emitter.getBuilder()->CreateCall(g.funcs.printf, comma_space);
                (*v)[i]->print(emitter);
            }
            if (v->size() == 1)
                emitter.getBuilder()->CreateCall(g.funcs.printf, comma);

            emitter.getBuilder()->CreateCall(g.funcs.printf, close_paren);
        }

        virtual bool canConvertTo(ConcreteCompilerType* other_type) {
            return (other_type == UNKNOWN || other_type == BOXED_TUPLE);
        }

        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, VAR* var, ConcreteCompilerType* other_type) {
            assert(other_type == UNKNOWN || other_type == BOXED_TUPLE);

            VEC* v = var->getValue();

            std::vector<ConcreteCompilerVariable*> converted_args;

            llvm::Value *nelts = llvm::ConstantInt::get(g.i64, v->size(), false);
            llvm::Value *alloca = emitter.getBuilder()->CreateAlloca(g.llvm_value_type_ptr, nelts);
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

        virtual ConcreteCompilerType* getBoxType() {
            return BOXED_TUPLE;
        }

        virtual ConcreteCompilerType* getConcreteType() {
            return BOXED_TUPLE;
        }

        static TupleType* make(const std::vector<CompilerType*> &elt_types) {
            return new TupleType(elt_types);
        }

        virtual CompilerVariable* getitem(IREmitter &emitter, VAR *var, CompilerVariable *slice) {
            if (slice->getType() == INT) {
                llvm::Value* v = static_cast<ConcreteCompilerVariable*>(slice)->getValue();
                assert(v->getType() == g.i64);
                if (llvm::ConstantInt *ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
                    int64_t i = ci->getSExtValue();
                    if (i >= 0 && i < var->getValue()->size()) {
                        CompilerVariable *rtn = (*var->getValue())[i];
                        rtn->incvref();
                        return rtn;
                    } else {
                        llvm::CallInst *call = emitter.getBuilder()->CreateCall2(g.funcs.raiseAttributeErrorStr, getStringConstantPtr(debugName() + '\0'), getStringConstantPtr("__getitem__\0"));
                        call->setDoesNotReturn();
                        return undefVariable();
                    }
                }
            }
            RELEASE_ASSERT(0, "");
            //return getConstantInt(var->getValue()->size(), g.i64);
        }

        virtual ConcreteCompilerVariable* len(IREmitter &emitter, VAR *var) {
            return new ConcreteCompilerVariable(INT, getConstantInt(var->getValue()->size(), g.i64), true);
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            return BOXED_TUPLE->getattrType(attr);
        }
};

CompilerType* makeTupleType(const std::vector<CompilerType*> &elt_types) {
    return TupleType::make(elt_types);
}

CompilerVariable* makeTuple(const std::vector<CompilerVariable*> &elts) {
    std::vector<CompilerType*> elt_types;
    for (int i = 0; i < elts.size(); i++) {
        elts[i]->incvref();
        elt_types.push_back(elts[i]->getType());
    }
    TupleType *type = TupleType::make(elt_types);

    const std::vector<CompilerVariable*> *alloc_elts = new std::vector<CompilerVariable*>(elts);
    return new TupleType::VAR(type, alloc_elts, true);
}

class UndefType : public ConcreteCompilerType {
    public:
        std::string debugName() {
            return "undefType";
        }

        llvm::Type* llvmType() override {
            // Something that no one else uses...
            // TODO should do something more rare like a unique custom struct
            return llvm::Type::getInt16Ty(g.context);
        }

        virtual CompilerVariable* call(IREmitter &emitter, VAR *var, const std::vector<CompilerVariable*>& args) {
            return undefVariable();
        }
        virtual void drop(IREmitter &emitter, VAR *var) {}
        virtual void grab(IREmitter &emitter, VAR *var) {}
        virtual CompilerVariable* dup(VAR *v, DupCache &cache) {
            // TODO copied from UnknownType
            auto &rtn = cache[v];
            if (rtn == NULL) {
                rtn = new VAR(this, v->getValue(), v->isGrabbed());
                while (rtn->getVrefs() < v->getVrefs())
                    rtn->incvref();
            }
            return rtn;
        }
        virtual void print(IREmitter &emitter, VAR *var) {}
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, VAR* var, ConcreteCompilerType* other_type) {
            llvm::Value *v = llvm::UndefValue::get(other_type->llvmType());
            return new ConcreteCompilerVariable(other_type, v, true);
        }
        virtual CompilerVariable* getattr(IREmitter &emitter, VAR* var, const std::string &attr) {
            return undefVariable();
        }

        virtual CompilerVariable* callattr(IREmitter &emitter, VAR *var, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*>& args) {
            return undefVariable();
        }

        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
            return UNDEF;
        }

        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, VAR *var) {
            return new ConcreteCompilerVariable(BOOL, llvm::UndefValue::get(g.i1), true);
        }

        virtual ConcreteCompilerType* getBoxType() {
            return UNKNOWN;
        }

        virtual ConcreteCompilerType* getConcreteType() {
            return this;
        }

        virtual CompilerType* getattrType(const std::string &attr) {
            return UNDEF;
        }

        virtual bool canConvertTo(ConcreteCompilerType* other_type) {
            return true;
        }

        virtual BoxedClass* guaranteedClass() {
            return NULL;
        }
} _UNDEF;
CompilerType *UNDEF = &_UNDEF;

CompilerVariable* undefVariable() {
    return new ConcreteCompilerVariable(&_UNDEF, llvm::UndefValue::get(_UNDEF.llvmType()), true);
}


ConcreteCompilerType *LIST, *SLICE, *MODULE, *DICT;

} // namespace pyston
