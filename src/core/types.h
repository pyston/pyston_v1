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

namespace gc {

class TraceStack;
class GCVisitor {
private:
    bool isValid(void* p);

public:
    TraceStack* stack;
    GCVisitor(TraceStack* stack) : stack(stack) {}

    // These all work on *user* pointers, ie pointers to the user_data section of GCAllocations
    void visitIf(void* p) {
        if (p)
            visit(p);
    }
    void visit(void* p);
    void visitRange(void* const* start, void* const* end);
    void visitPotential(void* p);
    void visitPotentialRange(void* const* start, void* const* end);
};

} // namespace gc
using gc::GCVisitor;

enum class EffortLevel {
    MODERATE = 2,
    MAXIMAL = 3,
};

enum ExceptionStyle {
    CAPI,
    CXX,
};

template <typename T> struct ExceptionSwitchable {
public:
    T capi_val;
    T cxx_val;

    ExceptionSwitchable() : capi_val(), cxx_val() {}
    ExceptionSwitchable(T capi_val, T cxx_val) : capi_val(std::move(capi_val)), cxx_val(std::move(cxx_val)) {}

    template <ExceptionStyle S> T get() {
        if (S == CAPI)
            return capi_val;
        else
            return cxx_val;
    }

    T get(ExceptionStyle S) {
        if (S == CAPI)
            return capi_val;
        else
            return cxx_val;
    }
};

template <typename R, typename... Args>
struct ExceptionSwitchableFunction : public ExceptionSwitchable<R (*)(Args...)> {
public:
    typedef R (*FTy)(Args...);
    ExceptionSwitchableFunction(FTy capi_ptr, FTy cxx_ptr) : ExceptionSwitchable<FTy>(capi_ptr, cxx_ptr) {}

    template <ExceptionStyle S> R call(Args... args) noexcept(S == CAPI) { return this->template get<S>()(args...); }
};

class CompilerType;
template <class V> class ValuedCompilerType;
typedef ValuedCompilerType<llvm::Value*> ConcreteCompilerType;
ConcreteCompilerType* typeFromClass(BoxedClass*);

extern ConcreteCompilerType* INT, *BOXED_INT, *LONG, *FLOAT, *BOXED_FLOAT, *UNKNOWN, *BOOL, *STR, *NONE, *LIST, *SLICE,
    *MODULE, *DICT, *BOOL, *BOXED_BOOL, *BOXED_TUPLE, *SET, *FROZENSET, *CLOSURE, *GENERATOR, *BOXED_COMPLEX,
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
class AST_Name;
class AST_stmt;

class PhiAnalysis;
class LivenessAnalysis;
class ScopingAnalysis;

class CLFunction;
class OSREntryDescriptor;

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

    int kwargsIndex() const {
        assert(has_kwargs);
        return num_args + num_keywords + (has_starargs ? 1 : 0);
    }

    uint32_t asInt() const { return *reinterpret_cast<const uint32_t*>(this); }

    void dump() {
        printf("(has_starargs=%s, has_kwargs=%s, num_keywords=%d, num_args=%d)\n", has_starargs ? "true" : "false",
               has_kwargs ? "true" : "false", num_keywords, num_args);
    }
};
static_assert(sizeof(ArgPassSpec) <= sizeof(void*), "ArgPassSpec doesn't fit in register! (CC is probably wrong)");
static_assert(sizeof(ArgPassSpec) == sizeof(uint32_t), "ArgPassSpec::asInt needs to be updated");

struct ParamNames {
    bool takes_param_names;
    std::vector<llvm::StringRef> args;
    llvm::StringRef vararg, kwarg;

    // This members are only set if the InternedStringPool& constructor is used (aka. source is available)!
    // They are used as an optimization while interpreting because the AST_Names nodes cache important stuff
    // (InternedString, lookup_type) which would otherwise have to get recomputed all the time.
    std::vector<AST_Name*> arg_names;
    AST_Name* vararg_name, *kwarg_name;

    explicit ParamNames(AST* ast, InternedStringPool& pool);
    ParamNames(const std::vector<llvm::StringRef>& args, llvm::StringRef vararg, llvm::StringRef kwarg);
    static ParamNames empty() { return ParamNames(); }

    int totalParameters() const {
        return args.size() + (vararg.str().size() == 0 ? 0 : 1) + (kwarg.str().size() == 0 ? 0 : 1);
    }

    int kwargsIndex() const {
        assert(kwarg.str().size());
        return args.size() + (vararg.str().size() == 0 ? 0 : 1);
    }

private:
    ParamNames() : takes_param_names(false), vararg_name(NULL), kwarg_name(NULL) {}
};

// Probably overkill to copy this from ArgPassSpec
struct ParamReceiveSpec {
    bool takes_varargs : 1;
    bool takes_kwargs : 1;
    unsigned int num_defaults : 14;
    unsigned int num_args : 16;

    static const int MAX_ARGS = (1 << 16) - 1;
    static const int MAX_DEFAULTS = (1 << 14) - 1;

    explicit ParamReceiveSpec(int num_args)
        : takes_varargs(false), takes_kwargs(false), num_defaults(0), num_args(num_args) {
        assert(num_args <= MAX_ARGS);
        assert(num_defaults <= MAX_DEFAULTS);
    }
    explicit ParamReceiveSpec(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs)
        : takes_varargs(takes_varargs), takes_kwargs(takes_kwargs), num_defaults(num_defaults), num_args(num_args) {
        assert(num_args <= MAX_ARGS);
        assert(num_defaults <= MAX_DEFAULTS);
    }

    bool operator==(ParamReceiveSpec rhs) {
        return takes_varargs == rhs.takes_varargs && takes_kwargs == rhs.takes_kwargs
               && num_defaults == rhs.num_defaults && num_args == rhs.num_args;
    }

    bool operator!=(ParamReceiveSpec rhs) { return !(*this == rhs); }

    int totalReceived() { return num_args + (takes_varargs ? 1 : 0) + (takes_kwargs ? 1 : 0); }
    int kwargsIndex() { return num_args + (takes_varargs ? 1 : 0); }
};

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
class JitCodeBlock;

struct CompiledFunction {
private:
public:
    CLFunction* clfunc;
    llvm::Function* func; // the llvm IR object
    FunctionSpecialization* spec;
    const OSREntryDescriptor* entry_descriptor;

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
    ExceptionStyle exception_style;

    int64_t times_called, times_speculation_failed;
    ICInvalidator dependent_callsites;

    LocationMap* location_map;

    std::vector<ICInfo*> ics;

    CompiledFunction(llvm::Function* func, FunctionSpecialization* spec, void* code, EffortLevel effort,
                     ExceptionStyle exception_style, const OSREntryDescriptor* entry_descriptor);

    ConcreteCompilerType* getReturnType();

    // TODO this will need to be implemented eventually; things to delete:
    // - line_table if it exists
    // - location_map if it exists
    // - all entries in ics (after deregistering them)
    ~CompiledFunction();

    // Call this when a speculation inside this version failed
    void speculationFailed();
};

typedef int FutureFlags;

class BoxedModule;
class ScopeInfo;
class InternedStringPool;
class LivenessAnalysis;
class SourceInfo {
public:
    BoxedModule* parent_module;
    ScopingAnalysis* scoping;
    ScopeInfo* scope_info;
    FutureFlags future_flags;
    AST* ast;
    CFG* cfg;
    bool is_generator;
    std::string fn; // equivalent of code.co_filename

    InternedStringPool& getInternedStrings();

    ScopeInfo* getScopeInfo();
    LivenessAnalysis* getLiveness();

    // TODO we're currently copying the body of the AST into here, since lambdas don't really have a statement-based
    // body and we have to create one.  Ideally, we'd be able to avoid the space duplication for non-lambdas.
    const std::vector<AST_stmt*> body;

    llvm::StringRef getName();
    InternedString mangleName(InternedString id);

    Box* getDocString();

    SourceInfo(BoxedModule* m, ScopingAnalysis* scoping, FutureFlags future_flags, AST* ast,
               std::vector<AST_stmt*> body, std::string fn);
    ~SourceInfo();

private:
    std::unique_ptr<LivenessAnalysis> liveness_info;
};

typedef std::vector<CompiledFunction*> FunctionList;
struct CallRewriteArgs;
class BoxedCode;
class CLFunction {
public:
    ParamReceiveSpec paramspec;

    std::unique_ptr<SourceInfo> source;
    ParamNames param_names;

    FunctionList
        versions; // any compiled versions along with their type parameters; in order from most preferred to least
    CompiledFunction* always_use_version; // if this version is set, always use it (for unboxed cases)
    std::unordered_map<const OSREntryDescriptor*, CompiledFunction*> osr_versions;

    // Please use codeForFunction() to access this:
    BoxedCode* code_obj;

    // For use by the interpreter/baseline jit:
    int times_interpreted;
    std::vector<std::unique_ptr<JitCodeBlock>> code_blocks;
    ICInvalidator dependent_interp_callsites;

    // Functions can provide an "internal" version, which will get called instead
    // of the normal dispatch through the functionlist.
    // This can be used to implement functions which know how to rewrite themselves,
    // such as typeCall.
    typedef ExceptionSwitchableFunction<Box*, BoxedFunctionBase*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*,
                                        Box**, const std::vector<BoxedString*>*> InternalCallable;
    InternalCallable internal_callable;

    CLFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs,
               std::unique_ptr<SourceInfo> source);
    CLFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs, const ParamNames& param_names);
    ~CLFunction();

    int numReceivedArgs() { return paramspec.totalReceived(); }

    void addVersion(CompiledFunction* compiled) {
        assert(compiled);
        assert((compiled->spec != NULL) + (compiled->entry_descriptor != NULL) == 1);
        assert(compiled->clfunc == NULL);
        assert(compiled->code);
        compiled->clfunc = this;

        if (compiled->entry_descriptor == NULL) {
            bool could_have_speculations = (source.get() != NULL);
            if (!could_have_speculations && versions.size() == 0 && compiled->effort == EffortLevel::MAXIMAL
                && compiled->spec->accepts_all_inputs && compiled->spec->boxed_return_value)
                always_use_version = compiled;

            assert(compiled->spec->arg_types.size() == paramspec.totalReceived());
            versions.push_back(compiled);
        } else {
            osr_versions[compiled->entry_descriptor] = compiled;
        }
    }

    bool isGenerator() const {
        if (source)
            return source->is_generator;
        return false;
    }
};

CLFunction* createRTFunction(int num_args, int num_defaults, bool takes_varargs, bool takes_kwargs,
                             const ParamNames& param_names = ParamNames::empty());
CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs, int num_defaults, bool takes_varargs,
                          bool takes_kwargs, const ParamNames& param_names = ParamNames::empty(),
                          ExceptionStyle exception_style = CXX);
CLFunction* boxRTFunction(void* f, ConcreteCompilerType* rtn_type, int nargs,
                          const ParamNames& param_names = ParamNames::empty(), ExceptionStyle exception_style = CXX);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type, ExceptionStyle exception_style = CXX);
void addRTFunction(CLFunction* cf, void* f, ConcreteCompilerType* rtn_type,
                   const std::vector<ConcreteCompilerType*>& arg_types, ExceptionStyle exception_style = CXX);
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
    CONSERVATIVE_PYTHON = 6,
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

// Helper function around PyString_InternFromString:
BoxedString* internStringImmortal(llvm::StringRef s);

// Callers should use this function if they can accept mortal string objects.
// FIXME For now it just returns immortal strings, but at least we can use it
// to start documenting the places that can take mortal strings.
inline BoxedString* internStringMortal(const char* s) {
    return internStringImmortal(s);
}

inline BoxedString* internStringMortal(llvm::StringRef s) {
    assert(s.data()[s.size()] == '\0');
    return internStringMortal(s.data());
}

// TODO this is an immortal intern for now
inline void internStringMortalInplace(BoxedString*& s) {
    PyString_InternInPlace((PyObject**)&s);
}

struct HCAttrs {
public:
    struct AttrList {
        Box* attrs[0];
    };

    HiddenClass* hcls;
    AttrList* attr_list;

    HCAttrs(HiddenClass* hcls = root_hcls) : hcls(hcls), attr_list(nullptr) {}
};

class BoxedDict;
class BoxedString;

// In Pyston, this is the same type as CPython's PyObject (they are interchangeable, but we
// use Box in Pyston wherever possible as a convention).
//
// Other types on Pyston inherit from Box (e.g. BoxedString is a Box). Why is this class not
// polymorphic? Because of C extension support -- having virtual methods would change the layout
// of the object.
class Box {
private:
    BoxedDict** getDictPtr();

    // Appends a new value to the hcattrs array.
    void appendNewHCAttr(Box* val, SetattrRewriteArgs* rewrite_args);

public:
    // Add a no-op constructor to make sure that we don't zero-initialize cls
    Box() {}

    void* operator new(size_t size, BoxedClass* cls) __attribute__((visibility("default")));
    void operator delete(void* ptr) __attribute__((visibility("default"))) { abort(); }

    // Note: cls gets initialized in the new() function.
    BoxedClass* cls;

    llvm::iterator_range<BoxIterator> pyElements();

    size_t getHCAttrsOffset();
    HCAttrs* getHCAttrsPtr();
    void setDict(BoxedDict* d);
    BoxedDict* getDict();


    void setattr(BoxedString* attr, Box* val, SetattrRewriteArgs* rewrite_args);
    void giveAttr(const char* attr, Box* val) { giveAttr(internStringMortal(attr), val); }
    void giveAttr(BoxedString* attr, Box* val) {
        assert(!this->hasattr(attr));
        this->setattr(attr, val, NULL);
    }

    // getattr() does the equivalent of PyDict_GetItem(obj->dict, attr): it looks up the attribute's value on the
    // object's attribute storage. it doesn't look at other objects or do any descriptor logic.
    Box* getattr(BoxedString* attr, GetattrRewriteArgs* rewrite_args);
    Box* getattr(BoxedString* attr) { return getattr(attr, NULL); }
    bool hasattr(BoxedString* attr) { return getattr(attr) != NULL; }
    void delattr(BoxedString* attr, DelattrRewriteArgs* rewrite_args);

    // Only valid for hc-backed instances:
    Box* getAttrWrapper();

    Box* reprIC();
    BoxedString* reprICAsString();
    bool nonzeroIC();
    Box* hasnextOrNullIC();

    friend class AttrWrapper;
};
static_assert(offsetof(Box, cls) == offsetof(struct _object, ob_type), "");

// Our default for tp_alloc:
extern "C" PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept;

#define DEFAULT_CLASS(default_cls)                                                                                     \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        assert(cls->tp_itemsize == 0);                                                                                 \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) {                                           \
        assert(default_cls->tp_itemsize == 0);                                                                         \
        return Box::operator new(size, default_cls);                                                                   \
    }

#if STAT_ALLOCATIONS
#define ALLOC_STATS(cls)                                                                                               \
    if (cls->tp_name) {                                                                                                \
        std::string per_name_alloc_name = "alloc." + std::string(cls->tp_name);                                        \
        std::string per_name_allocsize_name = "allocsize." + std::string(cls->tp_name);                                \
        Stats::log(Stats::getStatCounter(per_name_alloc_name));                                                        \
        Stats::log(Stats::getStatCounter(per_name_allocsize_name), size);                                              \
    }
#define ALLOC_STATS_VAR(cls)                                                                                           \
    if (cls->tp_name) {                                                                                                \
        std::string per_name_alloc_name = "alloc." + std::string(cls->tp_name);                                        \
        std::string per_name_alloc_name0 = "alloc." + std::string(cls->tp_name) + "(0)";                               \
        std::string per_name_allocsize_name = "allocsize." + std::string(cls->tp_name);                                \
        std::string per_name_allocsize_name0 = "allocsize." + std::string(cls->tp_name) + "(0)";                       \
        static StatCounter alloc_name(per_name_alloc_name);                                                            \
        static StatCounter alloc_name0(per_name_alloc_name0);                                                          \
        static StatCounter allocsize_name(per_name_allocsize_name);                                                    \
        static StatCounter allocsize_name0(per_name_allocsize_name0);                                                  \
        if (nitems == 0) {                                                                                             \
            alloc_name0.log();                                                                                         \
            allocsize_name0.log(_PyObject_VAR_SIZE(cls, nitems));                                                      \
        } else {                                                                                                       \
            alloc_name.log();                                                                                          \
            allocsize_name.log(_PyObject_VAR_SIZE(cls, nitems));                                                       \
        }                                                                                                              \
    }
#else
#define ALLOC_STATS(cls)
#define ALLOC_STATS_VAR(cls)
#endif


// The restrictions on when you can use the SIMPLE (ie fast) variant are encoded as
// asserts in the 1-arg operator new function:
#define DEFAULT_CLASS_SIMPLE(default_cls)                                                                              \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) {                                           \
        ALLOC_STATS(default_cls);                                                                                      \
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
        /* Don't allocate classes through this -- we need to keep track of all class objects. */                       \
        assert(default_cls != type_cls);                                                                               \
        assert(!gc::hasOrderedFinalizer(default_cls));                                                                 \
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

#define DEFAULT_CLASS_VAR(default_cls, itemsize)                                                                       \
    static_assert(itemsize > 0, "");                                                                                   \
    /* asserts that the class in question is a subclass of BoxVar */                                                   \
    inline void _base_check() {                                                                                        \
        static_assert(std::is_base_of<BoxVar, std::remove_pointer<decltype(this)>::type>::value, "");                  \
    }                                                                                                                  \
                                                                                                                       \
    void* operator new(size_t size, BoxedClass * cls, size_t nitems) __attribute__((visibility("default"))) {          \
        assert(cls->tp_itemsize == itemsize);                                                                          \
        return BoxVar::operator new(size, cls, nitems);                                                                \
    }                                                                                                                  \
    void* operator new(size_t size, size_t nitems) __attribute__((visibility("default"))) {                            \
        assert(default_cls->tp_itemsize == itemsize);                                                                  \
        return BoxVar::operator new(size, default_cls, nitems);                                                        \
    }

#define DEFAULT_CLASS_VAR_SIMPLE(default_cls, itemsize)                                                                \
    static_assert(itemsize > 0, "");                                                                                   \
    inline void _base_check() {                                                                                        \
        static_assert(std::is_base_of<BoxVar, std::remove_pointer<decltype(this)>::type>::value, "");                  \
    }                                                                                                                  \
                                                                                                                       \
    void* operator new(size_t size, BoxedClass * cls, size_t nitems) __attribute__((visibility("default"))) {          \
        assert(cls->tp_itemsize == itemsize);                                                                          \
        return BoxVar::operator new(size, cls, nitems);                                                                \
    }                                                                                                                  \
    void* operator new(size_t size, size_t nitems) __attribute__((visibility("default"))) {                            \
        ALLOC_STATS_VAR(default_cls)                                                                                   \
        assert(default_cls->tp_alloc == PystonType_GenericAlloc);                                                      \
        assert(default_cls->tp_itemsize == itemsize);                                                                  \
        assert(default_cls->tp_basicsize == size);                                                                     \
        assert(default_cls->is_pyston_class);                                                                          \
        assert(default_cls->attrs_offset == 0);                                                                        \
                                                                                                                       \
        void* mem = gc_alloc(size + nitems * itemsize, gc::GCKind::PYTHON);                                            \
        assert(mem);                                                                                                   \
                                                                                                                       \
        BoxVar* rtn = static_cast<BoxVar*>(mem);                                                                       \
        rtn->cls = default_cls;                                                                                        \
        rtn->ob_size = nitems;                                                                                         \
        return rtn;                                                                                                    \
    }

// CPython C API compatibility class:
class BoxVar : public Box {
public:
    // This field gets initialized in operator new.
    Py_ssize_t ob_size;

    BoxVar() {}

    void* operator new(size_t size, BoxedClass* cls, size_t nitems) __attribute__((visibility("default")));
};
static_assert(offsetof(BoxVar, ob_size) == offsetof(struct _varobject, ob_size), "");

std::string getFullTypeName(Box* o);
const char* getTypeName(Box* b);

class BoxedClass;

// TODO these shouldn't be here
void setupRuntime();
void teardownRuntime();
Box* createAndRunModule(const std::string& name, const std::string& fn);
BoxedModule* createModule(const std::string& name, const char* fn = NULL, const char* doc = NULL);
Box* moduleInit(BoxedModule* self, Box* name, Box* doc = NULL);

// TODO where to put this
void appendToSysPath(llvm::StringRef path);
void prependToSysPath(llvm::StringRef path);
void addToSysArgv(const char* str);

// Raise a SyntaxError that occurs at a specific location.
// The traceback given to the user will include this,
// even though the execution didn't actually arrive there.
void raiseSyntaxError(const char* msg, int lineno, int col_offset, llvm::StringRef file, llvm::StringRef func);
void raiseSyntaxErrorHelper(llvm::StringRef file, llvm::StringRef func, AST* node_at, const char* msg, ...);

struct LineInfo {
public:
    int line, column;
    std::string file, func;

    LineInfo(int line, int column, llvm::StringRef file, llvm::StringRef func)
        : line(line), column(column), file(file), func(func) {}
};

struct ExcInfo {
    Box* type, *value, *traceback;
    bool reraise;

#ifndef NDEBUG
    ExcInfo(Box* type, Box* value, Box* traceback);
#else
    ExcInfo(Box* type, Box* value, Box* traceback) : type(type), value(value), traceback(traceback), reraise(false) {}
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
    // TODO: do we want exceptions to be slower? benchmark this!
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

    void gcVisit(GCVisitor* visitor);
};

struct CallattrFlags {
    bool cls_only : 1;
    bool null_on_nonexistent : 1;
    ArgPassSpec argspec;

    uint64_t asInt() { return (uint64_t(argspec.asInt()) << 32) | (cls_only << 0) | (null_on_nonexistent << 1); }
};
static_assert(sizeof(CallattrFlags) == sizeof(uint64_t), "");

// similar to Java's Array.binarySearch:
// return values are either:
//   >= 0 : the index where a given item was found
//   < 0  : a negative number that can be transformed (using "-num-1") into the insertion point
//
template <typename T, typename RandomAccessIterator, typename Cmp>
int binarySearch(T needle, RandomAccessIterator start, RandomAccessIterator end, Cmp cmp) {
    int l = 0;
    int r = end - start - 1;
    while (l <= r) {
        int mid = l + (r - l) / 2;
        auto mid_item = *(start + mid);
        int c = cmp(needle, mid_item);
        if (c < 0)
            r = mid - 1;
        else if (c > 0)
            l = mid + 1;
        else
            return mid;
    }
    return -(l + 1);
}
}

namespace std {
template <> std::pair<pyston::Box**, std::ptrdiff_t> get_temporary_buffer<pyston::Box*>(std::ptrdiff_t count) noexcept;
template <> void return_temporary_buffer<pyston::Box*>(pyston::Box** p);
}

#endif
