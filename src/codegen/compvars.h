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

#ifndef PYSTON_CODEGEN_COMPVARS_H
#define PYSTON_CODEGEN_COMPVARS_H

#include <stdint.h>
#include <vector>

#include "codegen/codegen.h"
#include "core/types.h"

namespace pyston {

class OpInfo;

class CompilerType;
class IREmitter;

extern ConcreteCompilerType* INT, *BOXED_INT, *FLOAT, *BOXED_FLOAT, *VOID, *UNKNOWN, *BOOL, *STR, *NONE, *LIST, *SLICE,
    *MODULE, *DICT, *BOOL, *BOXED_BOOL, *BOXED_TUPLE, *SET;
extern CompilerType* UNDEF;

class CompilerType {
public:
    virtual ~CompilerType() {}
    virtual std::string debugName() = 0;
    virtual ConcreteCompilerType* getConcreteType() = 0;
    virtual ConcreteCompilerType* getBoxType() = 0;
    virtual bool canConvertTo(ConcreteCompilerType* other_type) = 0;
    virtual CompilerType* getattrType(const std::string* attr, bool cls_only) = 0;
    virtual CompilerType* callType(ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                                   const std::vector<const std::string*>* keyword_names) = 0;
    virtual BoxedClass* guaranteedClass() = 0;
};

typedef std::unordered_map<CompilerVariable*, CompilerVariable*> DupCache;

template <class V> class _ValuedCompilerType : public CompilerType {
public:
    typedef ValuedCompilerVariable<V> VAR;

    virtual CompilerVariable* dup(VAR* v, DupCache& cache) {
        printf("dup not defined for %s\n", debugName().c_str());
        abort();
    }
    ConcreteCompilerType* getConcreteType() override {
        printf("getConcreteType not defined for %s\n", debugName().c_str());
        abort();
    }
    ConcreteCompilerType* getBoxType() override {
        printf("getBoxType not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual void drop(IREmitter& emmitter, VAR* value) {
        printf("drop not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual void grab(IREmitter& emmitter, VAR* value) {
        printf("grab not defined for %s\n", debugName().c_str());
        abort();
    }
    bool canConvertTo(ConcreteCompilerType* other_type) override {
        printf("canConvertTo not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, VAR* value, ConcreteCompilerType* other_type) {
        printf("makeConverted not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info, VAR* value) {
        printf("nonzero not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, VAR* value, const std::string* attr,
                                      bool cls_only) {
        printf("getattr not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual void setattr(IREmitter& emitter, const OpInfo& info, VAR* value, const std::string* attr,
                         CompilerVariable* v) {
        printf("setattr not defined for %s\n", debugName().c_str());
        abort();
    }

    virtual void delattr(IREmitter& emitter, const OpInfo& info, VAR* value, const std::string* attr) {
        printf("delattr not defined for %s\n", debugName().c_str());
        abort();
    }

    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, VAR* value, const std::string* attr,
                                       bool clsonly, struct ArgPassSpec argspec,
                                       const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        printf("callattr not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual CompilerVariable* call(IREmitter& emitter, const OpInfo& info, VAR* value, struct ArgPassSpec argspec,
                                   const std::vector<CompilerVariable*>& args,
                                   const std::vector<const std::string*>* keyword_names) {
        printf("call not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual void print(IREmitter& emitter, const OpInfo& info, VAR* value) {
        printf("print not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info, VAR* value) {
        printf("len not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, VAR* value, CompilerVariable* v) {
        // Can almost do this, except for error messages + types:
        // static const std::string attr("__getitem__");
        // return callattr(emitter, info, value, &attr, true, ArgPassSpec(1, 0, 0, 0), {v}, NULL);
        printf("getitem not defined for %s\n", debugName().c_str());
        abort();
    }
    virtual llvm::Value* makeClassCheck(IREmitter& emitter, VAR* value, BoxedClass* c) {
        printf("makeClassCheck not defined for %s\n", debugName().c_str());
        abort();
    }
    CompilerType* getattrType(const std::string* attr, bool cls_only) override {
        printf("getattrType not defined for %s\n", debugName().c_str());
        abort();
    }
    CompilerType* callType(struct ArgPassSpec argspec, const std::vector<CompilerType*>& arg_types,
                           const std::vector<const std::string*>* keyword_names) override {
        printf("callType not defined for %s\n", debugName().c_str());
        abort();
    }
    BoxedClass* guaranteedClass() override {
        ASSERT((CompilerType*)getConcreteType() != this, "%s", debugName().c_str());
        return getConcreteType()->guaranteedClass();
    }
};

template <class V> class ValuedCompilerType : public _ValuedCompilerType<V> { public: };

template <> class ValuedCompilerType<llvm::Value*> : public _ValuedCompilerType<llvm::Value*> {
public:
    virtual llvm::Type* llvmType() = 0;
    virtual std::string debugName();

    virtual bool isFitBy(BoxedClass*) {
        printf("isFitBy not defined for %s\n", debugName().c_str());
        abort();
    }

    virtual CompilerVariable* dup(ConcreteCompilerVariable* v, DupCache& cache);
    virtual ConcreteCompilerType* getConcreteType() { return this; }
    virtual bool canConvertTo(ConcreteCompilerType* other_type) { return other_type == this || other_type == UNKNOWN; }
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerVariable* var,
                                                    ConcreteCompilerType* other_type);
};

class CompilerVariable {
private:
    int vrefs;
    bool grabbed;

protected:
    virtual void drop(IREmitter& emitter) = 0;
    virtual void grab(IREmitter& emmitter) = 0;

public:
    CompilerVariable(bool grabbed) : vrefs(1), grabbed(grabbed) {}
    virtual ~CompilerVariable() {}

    bool isGrabbed() { return grabbed; }
    void incvref() {
        assert(vrefs);
        vrefs++;
    }
    void decvrefNodrop() {
        assert(vrefs > 0 && vrefs < (1 << 20));
        // It'd be nice to print out the type of the variable, but this is all happening
        // after the object got deleted so it's pretty precarious, and the getType()
        // debugging call will probably segfault:
        // ASSERT(vrefs, "%s", getType()->debugName().c_str());
        vrefs--;
        if (vrefs == 0) {
            delete this;
        }
    }
    void decvref(IREmitter& emitter) {
        assert(vrefs > 0 && vrefs < (1 << 20));
        // ASSERT(vrefs, "%s", getType()->debugName().c_str());
        vrefs--;
        if (vrefs == 0) {
            if (grabbed)
                drop(emitter);
            delete this;
        }
    }
    int getVrefs() { return vrefs; }
    void ensureGrabbed(IREmitter& emitter) {
        if (!grabbed) {
            grab(emitter);
            grabbed = true;
        }
    }
    virtual CompilerVariable* split(IREmitter& emitter) = 0;
    virtual CompilerVariable* dup(DupCache& cache) = 0;

    virtual CompilerType* getType() = 0;
    virtual ConcreteCompilerType* getConcreteType() = 0;
    virtual ConcreteCompilerType* getBoxType() = 0;
    virtual bool canConvertTo(ConcreteCompilerType* other_type) = 0;
    virtual ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerType* other_type) = 0;
    virtual llvm::Value* makeClassCheck(IREmitter& emitter, BoxedClass* cls) = 0;
    virtual BoxedClass* guaranteedClass() = 0;

    virtual ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info) = 0;
    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, const std::string* attr, bool cls_only)
        = 0;
    virtual void setattr(IREmitter& emitter, const OpInfo& info, const std::string* attr, CompilerVariable* v) = 0;
    virtual void delattr(IREmitter& emitter, const OpInfo& info, const std::string* attr) = 0;
    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, const std::string* attr, bool clsonly,
                                       struct ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) = 0;
    virtual CompilerVariable* call(IREmitter& emitter, const OpInfo& info, struct ArgPassSpec argspec,
                                   const std::vector<CompilerVariable*>& args,
                                   const std::vector<const std::string*>* keyword_names) = 0;
    virtual void print(IREmitter& emitter, const OpInfo& info) = 0;
    virtual ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info) = 0;
    virtual CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, CompilerVariable*) = 0;
};

template <class V> class ValuedCompilerVariable : public CompilerVariable {
private:
    typedef ValuedCompilerType<V> T;
    T* type;
    V value;

protected:
    virtual void drop(IREmitter& emitter) { type->drop(emitter, this); }
    virtual void grab(IREmitter& emmitter) { type->grab(emmitter, this); }

public:
    ValuedCompilerVariable(T* type, V value, bool grabbed) : CompilerVariable(grabbed), type(type), value(value) {}
    virtual T* getType() { return type; }
    virtual V getValue() { return value; }

    ConcreteCompilerType* getConcreteType() override { return type->getConcreteType(); }
    ConcreteCompilerType* getBoxType() override { return type->getBoxType(); }

    ValuedCompilerVariable<V>* split(IREmitter& emitter) override {
        ValuedCompilerVariable<V>* rtn;
        if (getVrefs() == 1) {
            rtn = this;
        } else {
            rtn = new ValuedCompilerVariable<V>(type, value, false);
            this->decvref(emitter);
        }
        rtn->ensureGrabbed(emitter);
        return rtn;
    }
    CompilerVariable* dup(DupCache& cache) override {
        CompilerVariable* rtn = type->dup(this, cache);

        ASSERT(rtn->getVrefs() == getVrefs(), "%d %s", rtn->getVrefs(), type->debugName().c_str());
        return rtn;
    }

    bool canConvertTo(ConcreteCompilerType* other_type) override { return type->canConvertTo(other_type); }
    ConcreteCompilerVariable* makeConverted(IREmitter& emitter, ConcreteCompilerType* other_type) override {
        ConcreteCompilerVariable* rtn = type->makeConverted(emitter, this, other_type);
        ASSERT(rtn->getType() == other_type, "%s", type->debugName().c_str());
        return rtn;
    }
    ConcreteCompilerVariable* nonzero(IREmitter& emitter, const OpInfo& info) override {
        return type->nonzero(emitter, info, this);
    }
    virtual CompilerVariable* getattr(IREmitter& emitter, const OpInfo& info, const std::string* attr, bool cls_only) {
        return type->getattr(emitter, info, this, attr, cls_only);
    }
    virtual void setattr(IREmitter& emitter, const OpInfo& info, const std::string* attr, CompilerVariable* v) {
        type->setattr(emitter, info, this, attr, v);
    }
    virtual void delattr(IREmitter& emitter, const OpInfo& info, const std::string* attr) {
        type->delattr(emitter, info, this, attr);
    }
    virtual CompilerVariable* callattr(IREmitter& emitter, const OpInfo& info, const std::string* attr, bool clsonly,
                                       struct ArgPassSpec argspec, const std::vector<CompilerVariable*>& args,
                                       const std::vector<const std::string*>* keyword_names) {
        return type->callattr(emitter, info, this, attr, clsonly, argspec, args, keyword_names);
    }
    CompilerVariable* call(IREmitter& emitter, const OpInfo& info, struct ArgPassSpec argspec,
                           const std::vector<CompilerVariable*>& args,
                           const std::vector<const std::string*>* keyword_names) override {
        return type->call(emitter, info, this, argspec, args, keyword_names);
    }
    void print(IREmitter& emitter, const OpInfo& info) override { type->print(emitter, info, this); }
    ConcreteCompilerVariable* len(IREmitter& emitter, const OpInfo& info) override {
        return type->len(emitter, info, this);
    }
    CompilerVariable* getitem(IREmitter& emitter, const OpInfo& info, CompilerVariable* slice) override {
        return type->getitem(emitter, info, this, slice);
    }
    llvm::Value* makeClassCheck(IREmitter& emitter, BoxedClass* cls) override {
        return type->makeClassCheck(emitter, this, cls);
    }

    BoxedClass* guaranteedClass() override { return type->guaranteedClass(); }
};

// template <>
// inline ConcreteCompilerVariable::ValuedCompilerVariable(ConcreteCompilerType *type, llvm::Value* value, bool grabbed)
// : CompilerVariable(grabbed), type(type), value(value) {
// assert(value->getType() == type->llvmType());
//}

ConcreteCompilerVariable* makeInt(int64_t);
ConcreteCompilerVariable* makeFloat(double);
ConcreteCompilerVariable* makeBool(bool);
CompilerVariable* makeStr(std::string*);
CompilerVariable* makeFunction(IREmitter& emitter, CLFunction*);
ConcreteCompilerVariable* undefVariable();
CompilerVariable* makeTuple(const std::vector<CompilerVariable*>& elts);

ConcreteCompilerType* typeFromClass(BoxedClass*);
CompilerType* typeOfClassobj(BoxedClass*);
CompilerType* makeTupleType(const std::vector<CompilerType*>& elt_types);
CompilerType* makeFuncType(ConcreteCompilerType* rtn_type, const std::vector<ConcreteCompilerType*>& arg_types);

} // namespace pyston

#endif
