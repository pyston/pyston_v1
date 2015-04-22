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
#include "core/stringpool.h"

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

    bool operator!=(ArgPassSpec rhs) { return !(*this == rhs); }

    int totalPassed() { return num_args + num_keywords + (has_starargs ? 1 : 0) + (has_kwargs ? 1 : 0); }

    uint32_t asInt() const { return *reinterpret_cast<const uint32_t*>(this); }

    void dump() {
        printf("(has_starargs=%s, has_kwargs=%s, num_keywords=%d, num_args=%d)\n", has_starargs ? "true" : "false",
               has_kwargs ? "true" : "false", num_keywords, num_args);
    }
};
static_assert(sizeof(ArgPassSpec) <= sizeof(void*), "ArgPassSpec doesn't fit in register! (CC is probably wrong)");
static_assert(sizeof(ArgPassSpec) == sizeof(uint32_t), "ArgPassSpec::asInt needs to be updated");

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

enum class EffortLevel {
    INTERPRETED = 0,
    MINIMAL = 1,
    MODERATE = 2,
    MAXIMAL = 3,
};

class CompilerType;
template <class V> class ValuedCompilerType;
typedef ValuedCompilerType<llvm::Value*> ConcreteCompilerType;
ConcreteCompilerType* typeFromClass(BoxedClass*);

extern ConcreteCompilerType* INT, *BOXED_INT, *LONG, *FLOAT, *BOXED_FLOAT, *VOID, *UNKNOWN, *BOOL, *STR, *NONE, *LIST,
    *SLICE, *MODULE, *DICT, *BOOL, *BOXED_BOOL, *BOXED_TUPLE, *SET, *FROZENSET, *CLOSURE, *GENERATOR, *BOXED_COMPLEX,
    *FRAME_INFO;
extern CompilerType* UNDEF;

class CompilerVariable;
template <class V> class ValuedCompilerVariable;
typedef ValuedCompilerVariable<llvm::Value*> ConcreteCompilerVariable;

class Box;
class BoxedClass;
class BoxedModule;
class BoxedFunctionBase;

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
    bool boxed_return_value;
    bool accepts_all_inputs;

    FunctionSpecialization(ConcreteCompilerType* rtn_type);
    FunctionSpecialization(ConcreteCompilerType* rtn_type, const std::vector<ConcreteCompilerType*>& arg_types);
};

class BoxedClosure;
class BoxedGenerator;
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

    EffortLevel effort;

    int64_t times_called, times_speculation_failed;
    ICInvalidator dependent_callsites;

    LocationMap* location_map; // only meaningful if this is a compiled frame

    std::vector<ICInfo*> ics;

    CompiledFunction(llvm::Function* func, FunctionSpecialization* spec, bool is_interpreted, void* code,
                     EffortLevel effort, const OSREntryDescriptor* entry_descriptor)
        : clfunc(NULL), func(func), spec(spec), entry_descriptor(entry_descriptor), is_interpreted(is_interpreted),
          code(code), effort(effort), times_called(0), times_speculation_failed(0), location_map(nullptr) {
        assert((spec != NULL) + (entry_descriptor != NULL) == 1);
    }

    ConcreteCompilerType* getReturnType();

    // TODO this will need to be implemented eventually; things to delete:
    // - line_table if it exists
    // - location_map if it exists
    // - all entries in ics (after deregistering them)
    ~CompiledFunction();

    // Call this when a speculation inside this version failed
    void speculationFailed();
};

struct ParamNames {
    bool takes_param_names;
    std::vector<llvm::StringRef> args;
    llvm::StringRef vararg, kwarg;

    explicit ParamNames(AST* ast);
    ParamNames(const std::vector<llvm::StringRef>& args, llvm::StringRef vararg, llvm::StringRef kwarg);
    static ParamNames empty() { return ParamNames(); }

    int totalParameters() const {
        return args.size() + (vararg.str().size() == 0 ? 0 : 1) + (kwarg.str().size() == 0 ? 0 : 1);
    }

private:
    ParamNames() : takes_param_names(false) {}
};

class BoxedModule;
class ScopeInfo;
class InternedStringPool;
class SourceInfo {
public:
    BoxedModule* parent_module;
    ScopingAnalysis* scoping;
    AST* ast;
    CFG* cfg;
    LivenessAnalysis* liveness;
    std::unordered_map<const OSREntryDescriptor*, PhiAnalysis*> phis;
    bool is_generator;

    InternedStringPool& getInternedStrings();

    ScopeInfo* getScopeInfo();

    // TODO we're currently copying the body of the AST into here, since lambdas don't really have a statement-based
    // body and we have to create one.  Ideally, we'd be able to avoid the space duplication for non-lambdas.
    const std::vector<AST_stmt*> body;

    const std::string getName();
    InternedString mangleName(InternedString id);

    Box* getDocString();

    SourceInfo(BoxedModule* m, ScopingAnalysis* scoping, AST* ast, const std::vector<AST_stmt*>& body);
};

typedef std::vector<CompiledFunction*> FunctionList;
struct CallRewriteArgs;
class CLFunction {
public:
    int num_args;
    int num_defaults;
    bool takes_varargs, takes_kwargs;

    SourceInfo* source;
    ParamNames param_names;

    FunctionList
        versions; // any compiled versions along with their type parameters; in order from most preferred to least
    CompiledFunction* always_use_version; // if this version is set, always use it (for unboxed cases)
    std::unordered_map<const OSREntryDescriptor*, CompiledFunction*> osr_versions;

    // Functions can provide an "internal" version, which will get called instead
    // of the normal dispatch through the functionlist.
    // This can be used to implement functions which know how to rewrite themselves,
    // such as typeCall.
    typedef Box* (*InternalCallable)(BoxedFunctionBase*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                     const std::vector<const std::string*>*);
    InternalCallable internal_callable = NULL;

    CLFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs, SourceInfo* source)
        : num_args(num_args), num_defaults(num_defaults), takes_varargs(takes_varargs), takes_kwargs(takes_kwargs),
          source(source), param_names(source->ast), always_use_version(NULL) {
        assert(num_args >= num_defaults);
    }
    CLFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs, const ParamNames& param_names)
        : num_args(num_args), num_defaults(num_defaults), takes_varargs(takes_varargs), takes_kwargs(takes_kwargs),
          source(NULL), param_names(param_names), always_use_version(NULL) {
        assert(num_args >= num_defaults);
    }

    int numReceivedArgs() { return num_args + (takes_varargs ? 1 : 0) + (takes_kwargs ? 1 : 0); }

    void addVersion(CompiledFunction* compiled) {
        assert(compiled);
        assert((compiled->spec != NULL) + (compiled->entry_descriptor != NULL) == 1);
        assert(compiled->clfunc == NULL);
        assert(compiled->is_interpreted == (compiled->code == NULL));
        compiled->clfunc = this;

        if (compiled->entry_descriptor == NULL) {
            if (versions.size() == 0 && compiled->effort == EffortLevel::MAXIMAL && compiled->spec->accepts_all_inputs
                && compiled->spec->boxed_return_value)
                always_use_version = compiled;

            assert(compiled->spec->arg_types.size() == num_args + (takes_varargs ? 1 : 0) + (takes_kwargs ? 1 : 0));
            versions.push_back(compiled);
        } else {
            osr_versions[compiled->entry_descriptor] = compiled;
        }
    }
};

CLFunction* createRTFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs,
                             const ParamNames& param_names = ParamNames::empty());
CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs, int num_defaults, bool takes_varargs,
                          bool takes_kwargs, const ParamNames& param_names = ParamNames::empty());
CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs,
                          const ParamNames& param_names = ParamNames::empty());
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types);
CLFunction* unboxRTFunction(Box*);

// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
CompiledFunction* compileFunction(CLFunction* f, FunctionSpecialization* spec, EffortLevel effort,
                                  const OSREntryDescriptor* entry);
EffortLevel initialEffort();

typedef bool i1;
typedef int64_t i64;

const char* getNameOfClass(BoxedClass* cls);
std::string getFullNameOfClass(BoxedClass* cls);

class Rewriter;
class RewriterVar;
class RuntimeIC;
class CallattrIC;
class NonzeroIC;
class BinopIC;

class Box;

namespace gc {

enum class GCKind : uint8_t {
    PYTHON = 1,
    CONSERVATIVE = 2,
    PRECISE = 3,
    UNTRACKED = 4,
    HIDDEN_CLASS = 5,
};

extern "C" void* gc_alloc(size_t nbytes, GCKind kind);
extern "C" void gc_free(void* ptr);
}

template <gc::GCKind gc_kind> class GCAllocated {
public:
    void* operator new(size_t size) __attribute__((visibility("default"))) { return gc::gc_alloc(size, gc_kind); }
    void operator delete(void* ptr) __attribute__((visibility("default"))) { gc::gc_free(ptr); }
};

class BoxIteratorImpl : public GCAllocated<gc::GCKind::CONSERVATIVE> {
public:
    virtual ~BoxIteratorImpl() = default;
    virtual void next() = 0;
    virtual Box* getValue() = 0;
    virtual bool isSame(const BoxIteratorImpl* rhs) = 0;
};

class BoxIterator {
public:
    BoxIteratorImpl* impl;

    BoxIterator(BoxIteratorImpl* impl) : impl(impl) {}
    ~BoxIterator() = default;

    static llvm::iterator_range<BoxIterator> getRange(Box* container);
    bool operator==(BoxIterator const& rhs) const { return impl->isSame(rhs.impl); }
    bool operator!=(BoxIterator const& rhs) const { return !(*this == rhs); }

    BoxIterator& operator++() {
        impl->next();
        return *this;
    }

    Box* operator*() const { return impl->getValue(); }
    Box* operator*() { return impl->getValue(); }

    void gcHandler(GCVisitor* v) { v->visitPotential(impl); }
};

class HiddenClass;
extern HiddenClass* root_hcls;

struct SetattrRewriteArgs;
struct GetattrRewriteArgs;
struct DelattrRewriteArgs;

struct HCAttrs {
public:
    struct AttrList {
        Box* attrs[0];
    };

    HiddenClass* hcls;
    AttrList* attr_list;

    HCAttrs() : hcls(root_hcls), attr_list(nullptr) {}
};

class BoxedDict;
class BoxedString;

class Box {
private:
    BoxedDict** getDictPtr();

public:
    // Add a no-op constructor to make sure that we don't zero-initialize cls
    Box() {}

    void* operator new(size_t size, BoxedClass* cls) __attribute__((visibility("default")));
    void operator delete(void* ptr) __attribute__((visibility("default"))) { abort(); }

    // Note: cls gets initialized in the new() function.
    BoxedClass* cls;

    llvm::iterator_range<BoxIterator> pyElements();

    HCAttrs* getHCAttrsPtr();
    void setDict(BoxedDict* d);
    BoxedDict* getDict();

    void setattr(const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args);
    void giveAttr(const std::string& attr, Box* val) {
        assert(!this->hasattr(attr));
        this->setattr(attr, val, NULL);
    }

    // getattr() does the equivalent of PyDict_GetItem(obj->dict, attr): it looks up the attribute's value on the
    // object's attribute storage. it doesn't look at other objects or do any descriptor logic.
    Box* getattr(const std::string& attr, GetattrRewriteArgs* rewrite_args);
    Box* getattr(const std::string& attr) { return getattr(attr, NULL); }
    bool hasattr(const std::string& attr) { return getattr(attr) != NULL; }
    void delattr(const std::string& attr, DelattrRewriteArgs* rewrite_args);

    Box* reprIC();
    BoxedString* reprICAsString();
    bool nonzeroIC();
    Box* hasnextOrNullIC();
    Box* nextIC();
};
static_assert(offsetof(Box, cls) == offsetof(struct _object, ob_type), "");

// Our default for tp_alloc:
extern "C" PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept;

#define DEFAULT_CLASS(default_cls)                                                                                     \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) {                                           \
        return Box::operator new(size, default_cls);                                                                   \
    }

// The restrictions on when you can use the SIMPLE (ie fast) variant are encoded as
// asserts in the 1-arg operator new function:
#define DEFAULT_CLASS_SIMPLE(default_cls)                                                                              \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) {                                           \
        /* In the simple cases, we can inline the following methods and simplify things a lot:                         \
         * - Box::operator new                                                                                         \
         * - cls->tp_alloc                                                                                             \
         * - PystonType_GenericAlloc                                                                                   \
         * - PyObject_Init                                                                                             \
         */                                                                                                            \
        assert(default_cls->tp_alloc == PystonType_GenericAlloc);                                                      \
        assert(default_cls->tp_itemsize == 0);                                                                         \
        assert(default_cls->tp_basicsize == size);                                                                     \
        assert(default_cls->is_pyston_class);                                                                          \
        assert(default_cls->attrs_offset == 0);                                                                        \
                                                                                                                       \
        /* note: we want to use size instead of tp_basicsize, since size is a compile-time constant */                 \
        void* mem = gc_alloc(size, gc::GCKind::PYTHON);                                                                \
        assert(mem);                                                                                                   \
                                                                                                                       \
        Box* rtn = static_cast<Box*>(mem);                                                                             \
                                                                                                                       \
        rtn->cls = default_cls;                                                                                        \
        return rtn;                                                                                                    \
        /* TODO: there should be a way to not have to do this nested inlining by hand */                               \
    }

// CPython C API compatibility class:
class BoxVar : public Box {
public:
    Py_ssize_t ob_size;

    BoxVar(Py_ssize_t ob_size) : ob_size(ob_size) {}
};
static_assert(offsetof(BoxVar, ob_size) == offsetof(struct _varobject, ob_size), "");

std::string getFullTypeName(Box* o);
const char* getTypeName(Box* b);

class BoxedClass;

// TODO these shouldn't be here
void setupRuntime();
void teardownRuntime();
BoxedModule* createAndRunModule(const std::string& name, const std::string& fn);
BoxedModule* createModule(const std::string& name, const std::string& fn, const char* doc = NULL);

// TODO where to put this
void appendToSysPath(const std::string& path);
void prependToSysPath(const std::string& path);
void addToSysArgv(const char* str);

// Raise a SyntaxError that occurs at a specific location.
// The traceback given to the user will include this,
// even though the execution didn't actually arrive there.
void raiseSyntaxError(const char* msg, int lineno, int col_offset, const std::string& file, const std::string& func);
void raiseSyntaxErrorHelper(const std::string& file, const std::string& func, AST* node_at, const char* msg, ...);

struct LineInfo {
public:
    const int line, column;
    std::string file, func;

    LineInfo(int line, int column, const std::string& file, const std::string& func)
        : line(line), column(column), file(file), func(func) {}
};

struct ExcInfo {
    Box* type, *value, *traceback;

#ifndef NDEBUG
    ExcInfo(Box* type, Box* value, Box* traceback);
#else
    ExcInfo(Box* type, Box* value, Box* traceback) : type(type), value(value), traceback(traceback) {}
#endif
    bool matches(BoxedClass* cls) const;
    void printExcAndTraceback() const;
};

class BoxedFrame;
struct FrameInfo {
    // Note(kmod): we have a number of fields here that all have independent
    // initialization rules.  We could potentially save time on every function-entry
    // by having an "initialized" variable (or condition) that guards all of them.

    // *Not the same semantics as CPython's frame->f_exc*
    // In CPython, f_exc is the saved exc_info from the previous frame.
    // In Pyston, exc is the frame-local value of sys.exc_info.
    // - This makes frame entering+leaving faster at the expense of slower exceptions.
    //
    // exc.type is initialized to NULL at function entry, and exc.value and exc.tb are left
    // uninitialized.  When one wants to access any of the values, you need to check if exc.type
    // is NULL, and if so crawl up the stack looking for the first frame with a non-null exc.type
    // and copy that.
    ExcInfo exc;

    // This field is always initialized:
    Box* boxedLocals;

    BoxedFrame* frame_obj;

    FrameInfo(ExcInfo exc) : exc(exc), boxedLocals(NULL), frame_obj(0) {}
};

struct CallattrFlags {
    bool cls_only : 1;
    bool null_on_nonexistent : 1;

    char asInt() { return (cls_only << 0) + (null_on_nonexistent << 1); }
};
}

namespace std {
template <> std::pair<pyston::Box**, std::ptrdiff_t> get_temporary_buffer<pyston::Box*>(std::ptrdiff_t count) noexcept;
template <> void return_temporary_buffer<pyston::Box*>(pyston::Box** p);
}

#endif
