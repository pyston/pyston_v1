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

#ifndef PYSTON_CORE_TYPES_H
#define PYSTON_CORE_TYPES_H

// TODO while having all these defs in a single header file is an improvement
// over having them spread randomly in different files, this should probably be split again
// but in a way that makes more sense.

#include <llvm/ADT/iterator_range.h>

#include "core/common.h"
#include "core/stats.h"

namespace llvm {
class Function;
class Type;
class Value;
}

namespace pyston {

class GCVisitor {
public:
    virtual ~GCVisitor() {}
    virtual void visit(void* p) = 0;
    virtual void visitRange(void* const* start, void* const* end) = 0;
    virtual void visitPotential(void* p) = 0;
    virtual void visitPotentialRange(void* const* start, void* const* end) = 0;
};

typedef int kindid_t;
class AllocationKind;
extern "C" kindid_t registerKind(const AllocationKind*);
class AllocationKind {
public:
#ifndef NDEBUG
    static const int64_t COOKIE = 0x1234abcd0c00c1e;
    const int64_t _cookie = COOKIE;
#endif

    typedef void (*GCHandler)(GCVisitor*, void*);
    GCHandler gc_handler;

    typedef void (*FinalizationFunc)(void*);
    FinalizationFunc finalizer;

    const kindid_t kind_id;

public:
    AllocationKind(GCHandler gc_handler, FinalizationFunc finalizer) __attribute__((visibility("default")))
    : gc_handler(gc_handler), finalizer(finalizer), kind_id(registerKind(this)) {}
};
extern "C" const AllocationKind untracked_kind, conservative_kind;

class ObjectFlavor;
class ObjectFlavor : public AllocationKind {
public:
    ObjectFlavor(GCHandler gc_handler, FinalizationFunc finalizer) __attribute__((visibility("default")))
    : AllocationKind(gc_handler, finalizer) {}
};



namespace EffortLevel {
enum EffortLevel {
    INTERPRETED = 0,
    MINIMAL,
    MODERATE,
    MAXIMAL,
};
}

template <class V> class ValuedCompilerType;
typedef ValuedCompilerType<llvm::Value*> ConcreteCompilerType;

class CompilerVariable;
template <class V> class ValuedCompilerVariable;
typedef ValuedCompilerVariable<llvm::Value*> ConcreteCompilerVariable;

class Box;
class BoxedClass;
class BoxedModule;
class BoxedFunction;

class ICGetattr;
class ICSlotInfo;

class CFG;
class AST;
class AST_FunctionDef;
class AST_arguments;
class AST_expr;
class AST_stmt;

class PhiAnalysis;
class LivenessAnalysis;
class ScopingAnalysis;

class CLFunction;
class OSREntryDescriptor;

class ICInvalidator {
private:
    int64_t cur_version;
    std::unordered_set<ICSlotInfo*> dependents;

public:
    ICInvalidator() : cur_version(0) {}

    void addDependent(ICSlotInfo* icentry);
    int64_t version();
    void invalidateAll();
};

// Codegen types:

struct FunctionSignature {
    ConcreteCompilerType* rtn_type;
    std::vector<ConcreteCompilerType*> arg_types;
    bool is_vararg;

    FunctionSignature(ConcreteCompilerType* rtn_type, bool is_vararg) : rtn_type(rtn_type), is_vararg(is_vararg) {}

    FunctionSignature(ConcreteCompilerType* rtn_type, ConcreteCompilerType* arg1, ConcreteCompilerType* arg2,
                      bool is_vararg)
        : rtn_type(rtn_type), is_vararg(is_vararg) {
        arg_types.push_back(arg1);
        arg_types.push_back(arg2);
    }

    FunctionSignature(ConcreteCompilerType* rtn_type, std::vector<ConcreteCompilerType*>& arg_types, bool is_vararg)
        : rtn_type(rtn_type), arg_types(arg_types), is_vararg(is_vararg) {}
};

struct CompiledFunction {
private:
public:
    CLFunction* clfunc;
    llvm::Function* func; // the llvm IR object
    FunctionSignature* sig;
    const OSREntryDescriptor* entry_descriptor;
    bool is_interpreted;

    union {
        Box* (*call)(Box*, Box*, Box*, Box**);
        void* code;
    };
    llvm::Value* llvm_code; // the llvm callable.

    EffortLevel::EffortLevel effort;

    int64_t times_called;
    ICInvalidator dependent_callsites;

    CompiledFunction(llvm::Function* func, FunctionSignature* sig, bool is_interpreted, void* code,
                     llvm::Value* llvm_code, EffortLevel::EffortLevel effort,
                     const OSREntryDescriptor* entry_descriptor)
        : clfunc(NULL), func(func), sig(sig), entry_descriptor(entry_descriptor), is_interpreted(is_interpreted),
          code(code), llvm_code(llvm_code), effort(effort), times_called(0) {}
};

class BoxedModule;
class SourceInfo {
public:
    BoxedModule* parent_module;
    ScopingAnalysis* scoping;
    AST* ast;
    CFG* cfg;
    LivenessAnalysis* liveness;
    PhiAnalysis* phis;

    const std::string getName();
    AST_arguments* getArgsAST();
    const std::vector<AST_expr*>& getArgNames();
    const std::vector<AST_stmt*>& getBody();

    SourceInfo(BoxedModule* m, ScopingAnalysis* scoping)
        : parent_module(m), scoping(scoping), ast(NULL), cfg(NULL), liveness(NULL), phis(NULL) {}
};
typedef std::vector<CompiledFunction*> FunctionList;
struct CLFunction {
    SourceInfo* source;
    FunctionList
    versions; // any compiled versions along with their type parameters; in order from most preferred to least
    std::unordered_map<const OSREntryDescriptor*, CompiledFunction*> osr_versions;

    CLFunction(SourceInfo* source) : source(source) {}

    void addVersion(CompiledFunction* compiled) {
        assert(compiled);
        assert((source == NULL) == (compiled->func == NULL));
        assert(compiled->sig);
        assert(compiled->clfunc == NULL);
        assert(compiled->is_interpreted == (compiled->code == NULL));
        assert(compiled->is_interpreted == (compiled->llvm_code == NULL));
        compiled->clfunc = this;
        if (compiled->entry_descriptor == NULL)
            versions.push_back(compiled);
        else
            osr_versions[compiled->entry_descriptor] = compiled;
    }
};

extern "C" CLFunction* createRTFunction();
extern "C" CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs, bool is_vararg);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type, int nargs, bool is_vararg);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types, bool is_vararg);
CLFunction* unboxRTFunction(Box*);
// extern "C" CLFunction* boxRTFunctionVariadic(const char* name, int nargs_min, int nargs_max, void* f);
extern "C" CompiledFunction* resolveCLFunc(CLFunction* f, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box** args);
extern "C" Box* callCompiledFunc(CompiledFunction* cf, int64_t nargs, Box* arg1, Box* arg2, Box* arg3, Box** args);

typedef bool i1;
typedef int64_t i64;

extern "C" void* rt_alloc(size_t);
extern "C" void rt_free(void*);

extern "C" const std::string* getNameOfClass(BoxedClass* cls);

class Rewriter;
class RewriterVar;

struct GCObjectHeader {
    kindid_t kind_id;
    uint16_t kind_data; // this part of the header is free for the kind to set as it wishes.
    uint8_t gc_flags;

    constexpr GCObjectHeader(const AllocationKind* kind) : kind_id(kind->kind_id), kind_data(0), gc_flags(0) {}
};
static_assert(sizeof(GCObjectHeader) <= sizeof(void*), "");

class GCObject {
public:
    GCObjectHeader gc_header;

    constexpr GCObject(const AllocationKind* kind) : gc_header(kind) {}

    void* operator new(size_t size) __attribute__((visibility("default"))) { return rt_alloc(size); }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { rt_free(ptr); }
};

extern "C" const AllocationKind hc_kind;
class HiddenClass : public GCObject {
private:
    HiddenClass() : GCObject(&hc_kind) {}
    HiddenClass(const HiddenClass* parent) : GCObject(&hc_kind), attr_offsets(parent->attr_offsets) {}

public:
    static HiddenClass* getRoot();
    std::unordered_map<std::string, int> attr_offsets;
    std::unordered_map<std::string, HiddenClass*> children;

    HiddenClass* getOrMakeChild(const std::string& attr);

    int getOffset(const std::string& attr) {
        std::unordered_map<std::string, int>::iterator it = attr_offsets.find(attr);
        if (it == attr_offsets.end())
            return -1;
        return it->second;
    }
};

class Box;
class BoxIterator {
public:
    BoxIterator(Box* iter) : iter(iter), value(nullptr) {}

    bool operator==(BoxIterator const& rhs) const { return (iter == rhs.iter && value == rhs.value); }
    bool operator!=(BoxIterator const& rhs) const { return !(*this == rhs); }

    BoxIterator& operator++();
    BoxIterator operator++(int) {
        BoxIterator tmp(*this);
        operator++();
        return tmp;
    }

    Box* operator*() const { return value; }
    Box* operator*() { return value; }

private:
    Box* iter;
    Box* value;
};


class SetattrRewriteArgs2;
class GetattrRewriteArgs;
class GetattrRewriteArgs2;

struct HCAttrs {
public:
    struct AttrList : GCObject {
        Box* attrs[0];
    };

    HiddenClass* hcls;
    AttrList* attr_list;

    HCAttrs() : hcls(HiddenClass::getRoot()), attr_list(nullptr) {}
};

class Box : public GCObject {
public:
    BoxedClass* cls;

    llvm::iterator_range<BoxIterator> pyElements();

    Box(const ObjectFlavor* flavor, BoxedClass* cls);

    HCAttrs* getAttrs();

    void setattr(const std::string& attr, Box* val, SetattrRewriteArgs2* rewrite_args2);
    void giveAttr(const std::string& attr, Box* val) {
        assert(this->getattr(attr) == NULL);
        this->setattr(attr, val, NULL);
    }

    Box* getattr(const std::string& attr, GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2);
    Box* getattr(const std::string& attr) { return getattr(attr, NULL, NULL); }
};



class BoxedClass : public Box {
public:
    HCAttrs attrs;

    // If the user sets __getattribute__ or __getattr__, we will have to invalidate
    // all getattr IC entries that relied on the fact that those functions didn't exist.
    // Doing this via invalidation means that instance attr lookups don't have
    // to guard on anything about the class.
    ICInvalidator dependent_icgetattrs;

    // Only a single base supported for now.
    // Is NULL iff this is object_cls
    BoxedClass* const base;

    // Offset of the HCAttrs object or 0 if there are no hcattrs.
    // Analogous to tp_dictoffset
    const int attrs_offset;
    // Analogous to tp_basicsize
    const int instance_size;

    bool instancesHaveAttrs() { return attrs_offset != 0; }

    // Whether this class object is constant or not, ie whether or not class-level
    // attributes can be changed or added.
    // Does not necessarily imply that the instances of this class are constant,
    // though for now (is_constant && !hasattrs) does imply that the instances are constant.
    bool is_constant;

    // Whether this class was defined by the user or is a builtin type.
    // this is used mostly for debugging.
    const bool is_user_defined;

    // will need to update this once we support tp_getattr-style overriding:
    bool hasGenericGetattr() { return true; }

    BoxedClass(BoxedClass* base, int attrs_offset, int instance_size, bool is_user_defined);
    void freeze() {
        assert(!is_constant);
        is_constant = true;
    }
};

// TODO these shouldn't be here
void setupRuntime();
void teardownRuntime();
BoxedModule* createModule(const std::string& name, const std::string& fn);

std::string getPythonFuncAt(void* ip, void* sp);

// TODO where to put this
void addToSysPath(const std::string& path);
void addToSysArgv(const char* str);

std::string formatException(Box* e);
void printTraceback();
}

#endif
