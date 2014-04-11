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

#include <vector>
#include <stdint.h>

#include "core/types.h"

#include "codegen/codegen.h"

namespace pyston {

class CompilerType;
class IREmitter;

extern ConcreteCompilerType *INT, *BOXED_INT, *FLOAT, *BOXED_FLOAT, *VOID, *UNKNOWN, *BOOL, *STR, *NONE, *LIST, *SLICE, *MODULE, *DICT, *BOOL, *BOXED_BOOL, *BOXED_TUPLE;
extern CompilerType* UNDEF;

class CompilerType {
    public:
        virtual std::string debugName() = 0;
        virtual ConcreteCompilerType* getConcreteType() = 0;
        virtual ConcreteCompilerType* getBoxType() = 0;
        virtual bool canConvertTo(ConcreteCompilerType* other_type) = 0;
        virtual CompilerType* getattrType(const std::string &attr) = 0;
        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) = 0;
        virtual BoxedClass* guaranteedClass() = 0;
};

typedef std::unordered_map<CompilerVariable*, CompilerVariable*> DupCache;

template <class V>
class _ValuedCompilerType : public CompilerType {
    public:
        typedef ValuedCompilerVariable<V> VAR;

        virtual CompilerVariable* dup(VAR *v, DupCache &cache) {
            printf("dup not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual ConcreteCompilerType* getConcreteType() {
            printf("getConcreteType not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual ConcreteCompilerType* getBoxType() {
            printf("getBoxType not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual void drop(IREmitter &emmitter, VAR* value) {
            printf("drop not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual void grab(IREmitter &emmitter, VAR* value) {
            printf("grab not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual bool canConvertTo(ConcreteCompilerType* other_type) {
            printf("canConvertTo not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, VAR* value, ConcreteCompilerType* other_type) {
            printf("makeConverted not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter, VAR* value) {
            printf("nonzero not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual CompilerVariable* getattr(IREmitter &emitter, VAR* value, const std::string &attr) {
            printf("getattr not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual void setattr(IREmitter &emitter, VAR* value, const std::string &attr, CompilerVariable *v) {
            printf("setattr not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual CompilerVariable* callattr(IREmitter &emitter, VAR* value, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*>& args) {
            printf("callattr not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual CompilerVariable* call(IREmitter &emitter, VAR* value, const std::vector<CompilerVariable*>& args) {
            printf("call not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual void print(IREmitter &emitter, VAR* value) {
            printf("print not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual ConcreteCompilerVariable* len(IREmitter &emitter, VAR* value) {
            printf("len not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual CompilerVariable* getitem(IREmitter &emitter, VAR* value, CompilerVariable *v) {
            printf("getitem not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual llvm::Value* makeClassCheck(IREmitter &emitter, VAR* value, BoxedClass* c) {
            printf("makeClassCheck not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual CompilerType* getattrType(const std::string &attr) {
            printf("getattrType not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual CompilerType* callType(std::vector<CompilerType*> &arg_types) {
            printf("callType not defined for %s\n", debugName().c_str());
            abort();
        }
        virtual BoxedClass* guaranteedClass() {
            ASSERT((CompilerType*)getConcreteType() != this, "%s", debugName().c_str());
            return getConcreteType()->guaranteedClass();
        }
};

template <class V>
class ValuedCompilerType : public _ValuedCompilerType<V> {
    public:
};

template <>
class ValuedCompilerType<llvm::Value*> : public _ValuedCompilerType<llvm::Value*> {
    public:
        virtual llvm::Type* llvmType() = 0;
        virtual std::string debugName();

        virtual bool isFitBy(BoxedClass*) {
            printf("isFitBy not defined for %s\n", debugName().c_str());
            abort();
        }

        virtual CompilerVariable* dup(ConcreteCompilerVariable *v, DupCache &cache);
        virtual ConcreteCompilerType* getConcreteType() {
            return this;
        }
        virtual bool canConvertTo(ConcreteCompilerType* other_type) {
            return other_type == this || other_type == UNKNOWN;
        }
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerVariable* var, ConcreteCompilerType* other_type);
};

class CompilerVariable {
    private:
        int vrefs;
        bool grabbed;
    protected:
        virtual void drop(IREmitter &emitter) = 0;
        virtual void grab(IREmitter &emmitter) = 0;
    public:
        CompilerVariable(bool grabbed) : vrefs(1), grabbed(grabbed) {}
        virtual ~CompilerVariable() {}

        bool isGrabbed() { return grabbed; }
        void incvref() {
            assert(vrefs);
            vrefs++;
        }
        void decvrefNodrop() {
            ASSERT(vrefs, "%s", getType()->debugName().c_str());
            vrefs--;
            if (vrefs == 0) {
                delete this;
            }
        }
        void decvref(IREmitter &emitter) {
            ASSERT(vrefs, "%s", getType()->debugName().c_str());
            vrefs--;
            if (vrefs == 0) {
                if (grabbed)
                    drop(emitter);
                delete this;
            }
        }
        int getVrefs() {
            return vrefs;
        }
        void ensureGrabbed(IREmitter &emitter) {
            if (!grabbed) {
                grab(emitter);
                grabbed = true;
            }
        }
        virtual CompilerVariable* split(IREmitter &emitter) = 0;
        virtual CompilerVariable* dup(DupCache &cache) = 0;

        virtual CompilerType* getType() = 0;
        virtual ConcreteCompilerType* getConcreteType() = 0;
        virtual ConcreteCompilerType* getBoxType() = 0;
        virtual bool canConvertTo(ConcreteCompilerType *other_type) = 0;
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerType *other_type) = 0;
        virtual llvm::Value* makeClassCheck(IREmitter &emitter, BoxedClass* cls) = 0;
        virtual BoxedClass* guaranteedClass() = 0;

        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter) = 0;
        virtual CompilerVariable* getattr(IREmitter &emitter, const std::string& attr) = 0;
        virtual void setattr(IREmitter &emitter, const std::string& attr, CompilerVariable* v) = 0;
        virtual CompilerVariable* callattr(IREmitter &emitter, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*>& args) = 0;
        virtual CompilerVariable* call(IREmitter &emitter, const std::vector<CompilerVariable*>& args) = 0;
        virtual void print(IREmitter &emitter) = 0;
        virtual ConcreteCompilerVariable* len(IREmitter &emitter) = 0;
        virtual CompilerVariable* getitem(IREmitter &emitter, CompilerVariable*) = 0;
};

template <class V>
class ValuedCompilerVariable : public CompilerVariable {
    private:
        typedef ValuedCompilerType<V> T;
        T *type;
        V value;

    protected:
        virtual void drop(IREmitter &emitter) {
            type->drop(emitter, this);
        }
        virtual void grab(IREmitter &emmitter) {
            type->grab(emmitter, this);
        }

    public:
        ValuedCompilerVariable(T *type, V value, bool grabbed) : CompilerVariable(grabbed), type(type), value(value) {}
        virtual T* getType() { return type; }
        virtual V getValue() { return value; }

        virtual ConcreteCompilerType* getConcreteType() {
            return type->getConcreteType();
        }
        virtual ConcreteCompilerType* getBoxType() {
            return type->getBoxType();
        }

        virtual ValuedCompilerVariable<V>* split(IREmitter &emitter) {
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
        virtual CompilerVariable* dup(DupCache &cache) {
            return type->dup(this, cache);
            //return new ValuedCompilerVariable<V>(type, value, isGrabbed());
        }

        virtual bool canConvertTo(ConcreteCompilerType *other_type) {
            return type->canConvertTo(other_type);
        }
        virtual ConcreteCompilerVariable* makeConverted(IREmitter &emitter, ConcreteCompilerType *other_type) {
            ConcreteCompilerVariable* rtn = type->makeConverted(emitter, this, other_type);
            ASSERT(rtn->getType() == other_type, "%s", type->debugName().c_str());
            return rtn;
        }
        virtual ConcreteCompilerVariable* nonzero(IREmitter &emitter) {
            return type->nonzero(emitter, this);
        }
        virtual CompilerVariable* getattr(IREmitter &emitter, const std::string& attr) {
            return type->getattr(emitter, this, attr);
        }
        virtual void setattr(IREmitter &emitter, const std::string& attr, CompilerVariable *v) {
            type->setattr(emitter, this, attr, v);
        }
        virtual CompilerVariable* callattr(IREmitter &emitter, const std::string &attr, bool clsonly, const std::vector<CompilerVariable*>& args) {
            return type->callattr(emitter, this, attr, clsonly, args);
        }
        virtual CompilerVariable* call(IREmitter &emitter, const std::vector<CompilerVariable*>& args) {
            return type->call(emitter, this, args);
        }
        virtual void print(IREmitter &emitter) {
            type->print(emitter, this);
        }
        virtual ConcreteCompilerVariable* len(IREmitter &emitter) {
            return type->len(emitter, this);
        }
        virtual CompilerVariable* getitem(IREmitter &emitter, CompilerVariable *slice) {
            return type->getitem(emitter, this, slice);
        }
        virtual llvm::Value* makeClassCheck(IREmitter &emitter, BoxedClass* cls) {
            return type->makeClassCheck(emitter, this, cls);
        }

        virtual BoxedClass* guaranteedClass() {
            return type->guaranteedClass();
        }
};

//template <>
//inline ConcreteCompilerVariable::ValuedCompilerVariable(ConcreteCompilerType *type, llvm::Value* value, bool grabbed) : CompilerVariable(grabbed), type(type), value(value) {
    //assert(value->getType() == type->llvmType());
//}

CompilerVariable* makeInt(int64_t);
CompilerVariable* makeFloat(double);
CompilerVariable* makeBool(bool);
CompilerVariable* makeStr(std::string*);
CompilerVariable* makeFunction(IREmitter &emitter, CLFunction*);
CompilerVariable* undefVariable();
CompilerVariable* makeTuple(const std::vector<CompilerVariable*> &elts);

ConcreteCompilerType* typeFromClass(BoxedClass*);
CompilerType* typeOfClassobj(BoxedClass*);
CompilerType* makeTupleType(const std::vector<CompilerType*> &elt_types);
CompilerType* makeFuncType(ConcreteCompilerType* rtn_type, const std::vector<ConcreteCompilerType*> &arg_types);

} // namespace pyston

#endif
