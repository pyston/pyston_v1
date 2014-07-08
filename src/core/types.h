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

struct ArgPassSpec {
    bool has_starargs : 1;
    bool has_kwargs : 1;
    unsigned int num_keywords : 14;
    unsigned int num_args : 16;

    static const int MAX_ARGS = (1 << 16) - 1;
    static const int MAX_KEYWORDS = (1 << 14) - 1;

    explicit ArgPassSpec(int num_args) : has_starargs(false), has_kwargs(false), num_keywords(0), num_args(num_args) {
        assert(num_args <= MAX_ARGS);
        assert(num_keywords <= MAX_KEYWORDS);
    }
    explicit ArgPassSpec(int num_args, int num_keywords, bool has_starargs, bool has_kwargs)
        : has_starargs(has_starargs), has_kwargs(has_kwargs), num_keywords(num_keywords), num_args(num_args) {
        assert(num_args <= MAX_ARGS);
        assert(num_keywords <= MAX_KEYWORDS);
    }

    int totalPassed() { return num_args + num_keywords + (has_starargs ? 1 : 0) + (has_kwargs ? 1 : 0); }

    uintptr_t asInt() const { return *reinterpret_cast<const uintptr_t*>(this); }
};
static_assert(sizeof(ArgPassSpec) <= sizeof(void*), "ArgPassSpec doesn't fit in register!");

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

struct FunctionSpecialization {
    ConcreteCompilerType* rtn_type;
    std::vector<ConcreteCompilerType*> arg_types;

    FunctionSpecialization(ConcreteCompilerType* rtn_type) : rtn_type(rtn_type) {}

    FunctionSpecialization(ConcreteCompilerType* rtn_type, ConcreteCompilerType* arg1, ConcreteCompilerType* arg2)
        : rtn_type(rtn_type), arg_types({ arg1, arg2 }) {}

    FunctionSpecialization(ConcreteCompilerType* rtn_type, const std::vector<ConcreteCompilerType*>& arg_types)
        : rtn_type(rtn_type), arg_types(arg_types) {}
};

class BoxedClosure;
struct CompiledFunction {
private:
public:
    CLFunction* clfunc;
    llvm::Function* func; // the llvm IR object
    FunctionSpecialization* spec;
    const OSREntryDescriptor* entry_descriptor;
    bool is_interpreted;

    union {
        Box* (*call)(Box*, Box*, Box*, Box**);
        Box* (*closure_call)(BoxedClosure*, Box*, Box*, Box*, Box**);
        void* code;
    };
    llvm::Value* llvm_code; // the llvm callable.

    EffortLevel::EffortLevel effort;

    int64_t times_called;
    ICInvalidator dependent_callsites;

    CompiledFunction(llvm::Function* func, FunctionSpecialization* spec, bool is_interpreted, void* code,
                     llvm::Value* llvm_code, EffortLevel::EffortLevel effort,
                     const OSREntryDescriptor* entry_descriptor)
        : clfunc(NULL), func(func), spec(spec), entry_descriptor(entry_descriptor), is_interpreted(is_interpreted),
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

    struct ArgNames {
        const std::vector<AST_expr*>* args;
        const std::string* vararg, *kwarg;

        explicit ArgNames(AST* ast);

        int totalParameters() const {
            if (!args)
                return 0;
            return args->size() + (vararg->size() == 0 ? 0 : 1) + (kwarg->size() == 0 ? 0 : 1);
        }
    };

    ArgNames arg_names;
    // TODO we're currently copying the body of the AST into here, since lambdas don't really have a statement-based
    // body and we have to create one.  Ideally, we'd be able to avoid the space duplication for non-lambdas.
    const std::vector<AST_stmt*> body;

    const std::string getName();

    SourceInfo(BoxedModule* m, ScopingAnalysis* scoping, AST* ast, const std::vector<AST_stmt*>& body)
        : parent_module(m), scoping(scoping), ast(ast), cfg(NULL), liveness(NULL), phis(NULL), arg_names(ast),
          body(body) {}
};

typedef std::vector<CompiledFunction*> FunctionList;
class CallRewriteArgs;
class CLFunction {
public:
    int num_args;
    int num_defaults;
    bool takes_varargs, takes_kwargs;

    SourceInfo* source;
    FunctionList
    versions; // any compiled versions along with their type parameters; in order from most preferred to least
    std::unordered_map<const OSREntryDescriptor*, CompiledFunction*> osr_versions;

    // Functions can provide an "internal" version, which will get called instead
    // of the normal dispatch through the functionlist.
    // This can be used to implement functions which know how to rewrite themselves,
    // such as typeCall.
    typedef Box* (*InternalCallable)(BoxedFunction*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                     const std::vector<const std::string*>*);
    InternalCallable internal_callable = NULL;

    CLFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs, SourceInfo* source)
        : num_args(num_args), num_defaults(num_defaults), takes_varargs(takes_varargs), takes_kwargs(takes_kwargs),
          source(source) {
        assert(num_args >= num_defaults);
    }

    int numReceivedArgs() { return num_args + (takes_varargs ? 1 : 0) + (takes_kwargs ? 1 : 0); }

    // const std::vector<AST_expr*>* getArgNames();

    void addVersion(CompiledFunction* compiled) {
        assert(compiled);
        assert((source == NULL) == (compiled->func == NULL));
        assert(compiled->spec);
        assert(compiled->spec->arg_types.size() == num_args + (takes_varargs ? 1 : 0) + (takes_kwargs ? 1 : 0));
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

CLFunction* createRTFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs);
CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs, int num_defaults, bool takes_varargs,
                          bool takes_kwargs);
CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types);
CLFunction* unboxRTFunction(Box*);

// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
CompiledFunction* compileFunction(CLFunction* f, FunctionSpecialization* spec, EffortLevel::EffortLevel effort,
                                  const OSREntryDescriptor* entry);
EffortLevel::EffortLevel initialEffort();

typedef bool i1;
typedef int64_t i64;

extern "C" void* rt_alloc(size_t);
extern "C" void rt_free(void*);
extern "C" void* rt_realloc(void* ptr, size_t new_size);

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

    HCAttrs* getAttrsPtr();

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
void appendToSysPath(const std::string& path);
void prependToSysPath(const std::string& path);
void addToSysArgv(const char* str);

std::string formatException(Box* e);
void printLastTraceback();

struct LineInfo {
public:
    const int line, column;
    std::string file, func;

    LineInfo(int line, int column, const std::string& file, const std::string& func)
        : line(line), column(column), file(file), func(func) {}
};
}

#endif
