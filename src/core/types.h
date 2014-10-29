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

#include <memory>
#include <stddef.h>
#include <vector>

#include "llvm/ADT/iterator_range.h"
#include "Python.h"

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

    bool operator==(ArgPassSpec rhs) {
        return has_starargs == rhs.has_starargs && has_kwargs == rhs.has_kwargs && num_keywords == rhs.num_keywords
               && num_args == rhs.num_args;
    }

    int totalPassed() { return num_args + num_keywords + (has_starargs ? 1 : 0) + (has_kwargs ? 1 : 0); }

    uintptr_t asInt() const { return *reinterpret_cast<const uintptr_t*>(this); }

    void dump() {
        printf("(has_starargs=%s, has_kwargs=%s, num_keywords=%d, num_args=%d)\n", has_starargs ? "true" : "false",
               has_kwargs ? "true" : "false", num_keywords, num_args);
    }
};
static_assert(sizeof(ArgPassSpec) <= sizeof(void*), "ArgPassSpec doesn't fit in register!");

namespace gc {

class TraceStack;
class GCVisitor {
private:
    bool isValid(void* p);

public:
    TraceStack* stack;
    GCVisitor(TraceStack* stack) : stack(stack) {}

    // These all work on *user* pointers, ie pointers to the user_data section of GCAllocations
    void visit(void* p);
    void visitRange(void* const* start, void* const* end);
    void visitPotential(void* p);
    void visitPotentialRange(void* const* start, void* const* end);
};

} // namespace gc
using gc::GCVisitor;

namespace EffortLevel {
enum EffortLevel {
    INTERPRETED = 0,
    MINIMAL,
    MODERATE,
    MAXIMAL,
};
}

class CompilerType;
template <class V> class ValuedCompilerType;
typedef ValuedCompilerType<llvm::Value*> ConcreteCompilerType;
ConcreteCompilerType* typeFromClass(BoxedClass*);

extern ConcreteCompilerType* INT, *BOXED_INT, *LONG, *FLOAT, *BOXED_FLOAT, *VOID, *UNKNOWN, *BOOL, *STR, *NONE, *LIST,
    *SLICE, *MODULE, *DICT, *BOOL, *BOXED_BOOL, *BOXED_TUPLE, *SET, *FROZENSET, *CLOSURE, *GENERATOR, *BOXED_COMPLEX;
extern CompilerType* UNDEF;

class CompilerVariable;
template <class V> class ValuedCompilerVariable;
typedef ValuedCompilerVariable<llvm::Value*> ConcreteCompilerVariable;

class Box;
class BoxedClass;
class BoxedModule;
class BoxedFunction;

class ICGetattr;
struct ICSlotInfo;

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
class BoxedGenerator;
class LineTable;
class ICInfo;
class LocationMap;

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
        Box* (*closure_generator_call)(BoxedClosure*, BoxedGenerator*, Box*, Box*, Box*, Box**);
        Box* (*generator_call)(BoxedGenerator*, Box*, Box*, Box*, Box**);
        void* code;
        uintptr_t code_start;
    };
    int code_size;
    llvm::Value* llvm_code; // the llvm callable.

    EffortLevel::EffortLevel effort;

    int64_t times_called;
    ICInvalidator dependent_callsites;

    // Unfortunately, can't make this a std::unique_ptr if we want to forward-declare LineTable:
    LineTable* line_table;

    LocationMap* location_map; // only meaningful if this is a compiled frame

    std::vector<ICInfo*> ics;

    CompiledFunction(llvm::Function* func, FunctionSpecialization* spec, bool is_interpreted, void* code,
                     llvm::Value* llvm_code, EffortLevel::EffortLevel effort,
                     const OSREntryDescriptor* entry_descriptor)
        : clfunc(NULL), func(func), spec(spec), entry_descriptor(entry_descriptor), is_interpreted(is_interpreted),
          code(code), llvm_code(llvm_code), effort(effort), times_called(0), line_table(nullptr),
          location_map(nullptr) {}

    // TODO this will need to be implemented eventually; things to delete:
    // - line_table if it exists
    // - location_map if it exists
    // - all entries in ics (after deregistering them)
    ~CompiledFunction();
};

class BoxedModule;
class ScopeInfo;
class SourceInfo {
public:
    BoxedModule* parent_module;
    ScopingAnalysis* scoping;
    AST* ast;
    CFG* cfg;
    LivenessAnalysis* liveness;
    PhiAnalysis* phis;

    ScopeInfo* getScopeInfo();

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
struct CallRewriteArgs;
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

extern "C" const std::string* getNameOfClass(BoxedClass* cls);
std::string getFullNameOfClass(BoxedClass* cls);

class Rewriter;
class RewriterVar;
class RuntimeIC;
class CallattrIC;
class NonzeroIC;
class BinopIC;

class Box;
class BoxIterator {
private:
    Box* iter;
    Box* value;

public:
    BoxIterator(Box* iter) : iter(iter), value(nullptr) {}

    bool operator==(BoxIterator const& rhs) const { return (iter == rhs.iter && value == rhs.value); }
    bool operator!=(BoxIterator const& rhs) const { return !(*this == rhs); }

    BoxIterator& operator++();

    Box* operator*() const { return value; }
    Box* operator*() { return value; }

    void gcHandler(GCVisitor* v);
};

namespace gc {

enum class GCKind : uint8_t {
    PYTHON = 1,
    CONSERVATIVE = 2,
    UNTRACKED = 3,
};

extern "C" void* gc_alloc(size_t nbytes, GCKind kind);
}

class PythonGCObject {
public:
    void* operator new(size_t size) __attribute__((visibility("default"))) {
        return gc_alloc(size, gc::GCKind::PYTHON);
    }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { abort(); }
};

class ConservativeGCObject {
public:
    void* operator new(size_t size) __attribute__((visibility("default"))) {
        return gc_alloc(size, gc::GCKind::CONSERVATIVE);
    }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { abort(); }
};

class UntrackedGCObject {
public:
    void* operator new(size_t size) __attribute__((visibility("default"))) {
        return gc_alloc(size, gc::GCKind::UNTRACKED);
    }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { abort(); }
};

class HiddenClass;
extern HiddenClass* root_hcls;

struct SetattrRewriteArgs;
struct GetattrRewriteArgs;
struct DelattrRewriteArgs;

struct HCAttrs {
public:
    struct AttrList : ConservativeGCObject {
        Box* attrs[0];
    };

    HiddenClass* hcls;
    AttrList* attr_list;

    HCAttrs() : hcls(root_hcls), attr_list(nullptr) {}
};

class BoxedString;

class Box : public PythonGCObject {
public:
    BoxedClass* cls;

    llvm::iterator_range<BoxIterator> pyElements();

    Box(BoxedClass* cls) : cls(cls) {
        // if (TRACK_ALLOCATIONS) {
        // int id = Stats::getStatId("allocated_" + *getNameOfClass(c));
        // Stats::log(id);
        //}

        // the only way cls should be NULL is if we're creating the type_cls
        // object itself:
        if (cls == NULL) {
            ASSERT(type_cls == NULL, "should pass a non-null cls here");
        } else {
        }
    }

    HCAttrs* getAttrsPtr();

    void setattr(const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args);
    void giveAttr(const std::string& attr, Box* val) {
        assert(this->getattr(attr) == NULL);
        this->setattr(attr, val, NULL);
    }

    Box* getattr(const std::string& attr, GetattrRewriteArgs* rewrite_args);
    Box* getattr(const std::string& attr) { return getattr(attr, NULL); }
    void delattr(const std::string& attr, DelattrRewriteArgs* rewrite_args);

    Box* reprIC();
    BoxedString* reprICAsString();
    bool nonzeroIC();
    Box* hasnextOrNullIC();
    Box* nextIC();
};
static_assert(offsetof(Box, cls) == offsetof(struct _object, ob_type), "");

// CPython C API compatibility class:
class BoxVar : public Box {
public:
    Py_ssize_t ob_size;

    BoxVar(BoxedClass* cls, Py_ssize_t ob_size) : Box(cls), ob_size(ob_size) {}
};
static_assert(offsetof(BoxVar, ob_size) == offsetof(struct _varobject, ob_size), "");

extern "C" const std::string* getTypeName(Box* o);
std::string getFullTypeName(Box* o);



class BoxedClass;

// TODO these shouldn't be here
void setupRuntime();
void teardownRuntime();
BoxedModule* createAndRunModule(const std::string& name, const std::string& fn);
BoxedModule* createModule(const std::string& name, const std::string& fn);

// TODO where to put this
void appendToSysPath(const std::string& path);
void prependToSysPath(const std::string& path);
void addToSysArgv(const char* str);

std::string formatException(Box* e);
void printLastTraceback();

// Raise a SyntaxError that occurs at a specific location.
// The traceback given to the user will include this,
// even though the execution didn't actually arrive there.
void raiseSyntaxError(const char* msg, int lineno, int col_offset, const std::string& file, const std::string& func);

struct LineInfo {
public:
    const int line, column;
    std::string file, func;

    LineInfo(int line, int column, const std::string& file, const std::string& func)
        : line(line), column(column), file(file), func(func) {}
};
}

#endif
