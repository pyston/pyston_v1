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
#include "gc/gc.h"

namespace llvm {
class Function;
class Type;
class Value;
}

namespace pyston {

using gc::GCVisitor;

// The "effort" which we will put into compiling a Python function.  This applies to the LLVM tier,
// where it can affect things such as the amount of type analysis we do, whether or not to run expensive
// LLVM optimization passes, etc.
// Currently (Nov '15) these are mostly unused, and we only use MAXIMAL.  There used to be two other levels
// as well but we stopped using them too.
enum class EffortLevel {
    MODERATE = 2,
    MAXIMAL = 3,
};

// Pyston supports two ways of implementing Python exceptions: by using return-code-based exceptions ("CAPI"
// style since this is what the CPython C API uses), or our custom C++-based exceptions ("CXX" style).  CAPI
// is faster when an exception is thrown, and CXX is faster when an exception is not thrown, so depending on
// the situation it can be beneficial to use one or the other.  The JIT will use some light profiling to
// determine when to emit code in one style or the other.
// Many runtime functions support being called in either style, and can get passed one of these enum values
// as a template parameter to switch between them.
enum ExceptionStyle {
    CAPI,
    CXX,
};

// Much of our runtime supports "rewriting" aka tracing itself.  Our tracing JIT requires support from the
// functions to be traced, so our runtime functions will contain checks to see if the tracer is currently
// activated, and then will do additional work.
// When the tracer isn't active, these extra "is the tracer active" checks can become expensive.  So when
// the caller knows that the tracer is not active, they can call a special version of the function where
// all of the "is the tracer active" checks are hardcoded to false.  This is possible by passing "NOT_REWRITEABLE"
// as a template argument to the called function.
enum Rewritable {
    NOT_REWRITABLE,
    REWRITABLE,
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

// CompilerType (and a specific kind of CompilerType, the ConcreteCompilerType) are the way that the LLVM JIT represents
// type information.  See src/codegen/compvars.h for more information.
class CompilerType;
template <class V> class ValuedCompilerType;
typedef ValuedCompilerType<llvm::Value*> ConcreteCompilerType;
ConcreteCompilerType* typeFromClass(BoxedClass*);

extern ConcreteCompilerType* UNBOXED_INT, *BOXED_INT, *LONG, *UNBOXED_FLOAT, *BOXED_FLOAT, *UNKNOWN, *BOOL, *STR, *NONE,
    *LIST, *SLICE, *MODULE, *DICT, *BOOL, *BOXED_BOOL, *BOXED_TUPLE, *SET, *FROZENSET, *CLOSURE, *GENERATOR,
    *BOXED_COMPLEX, *FRAME_INFO;
extern CompilerType* UNDEF, *INT, *FLOAT, *UNBOXED_SLICE;

// CompilerVariables are the way that the LLVM JIT tracks variables, which are a CompilerType combined with some sort
// of value (the type of value depends on the type of CompilerType).
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

class FunctionMetadata;
class OSREntryDescriptor;

// Pyston's internal calling convention is to pass around arguments in as unprocessed a form as possible,
// which lets the callee decide how they would like to receive their arguments.  In addition to the actual
// argument parameters, functions will often receive an ArgPassSpec struct which specifies the meaning of
// the raw pointer values, such as whether they were positional arguments or keyword arguments, etc.
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

// Similar to ArgPassSpec, this struct is how functions specify what their parameter signature is.
// (Probably overkill to copy this from ArgPassSpec)
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

// Inline-caches contain fastpath code, and need to know that their fastpath is valid for a particular set
// of arguments.  This is usually done with guards: conditional checks that will avoid the fastpath if the
// assumptions failed.  This can also be done using invalidation: no checks will be emitted into the generated
// assembly, but instead if the assumption is invalidated, the IC will get erased.
// This is useful for cases where we expect the assumption to overwhelmingly be true, or cases where it
// is not possible to use guards.  It is more difficult to use invalidation because it is much easier to
// get it wrong by forgetting to invalidate in all places that are necessary (whereas it is easier to be
// conservative about guarding).
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

// A specific compilation of a FunctionMetadata.  Usually these will be created by the LLVM JIT, which will take a FunctionMetadata
// and some compilation settings, and produce a CompiledFunction
// CompiledFunctions can also be created from raw function pointers, using FunctionMetadata::create.
// A single FunctionMetadata can have multiple CompiledFunctions associated with it, if they have different settings.
// Typically, this will happen due to specialization on the argument types (ie we will generate a separate versions
// of a function that are faster but only apply to specific argument types).
struct CompiledFunction {
private:
public:
    FunctionMetadata* md;
    llvm::Function* func; // the llvm IR object

    // Some compilation settings:
    EffortLevel effort;
    ExceptionStyle exception_style;
    FunctionSpecialization* spec;
    // If this compilation was due to an OSR, `entry_descriptor` contains metadata about the OSR.
    // Otherwise this field is NULL.
    const OSREntryDescriptor* entry_descriptor;

    // Pointers that were written directly into the code, which the GC should be aware of.
    std::vector<const void*> pointers_in_code;

    // The function pointer to the generated code.  For convenience, it can be accessed
    // as one of many different types.
    union {
        Box* (*call)(Box*, Box*, Box*, Box**);
        Box* (*closure_call)(BoxedClosure*, Box*, Box*, Box*, Box**);
        Box* (*closure_generator_call)(BoxedClosure*, BoxedGenerator*, Box*, Box*, Box*, Box**);
        Box* (*generator_call)(BoxedGenerator*, Box*, Box*, Box*, Box**);
        Box* (*call1)(Box*, Box*, Box*, Box*, Box**);
        Box* (*call2)(Box*, Box*, Box*, Box*, Box*, Box**);
        Box* (*call3)(Box*, Box*, Box*, Box*, Box*, Box*, Box**);
        void* code;
        uintptr_t code_start;
    };
    int code_size;

    // Some simple profiling stats:
    int64_t times_called, times_speculation_failed;

    // A list of ICs that depend on various properties of this CompiledFunction.
    // These will get invalidated in situations such as: we compiled a higher-effort version of
    // this function so we want to get old callsites to use the newer and better version, or
    // we noticed that we compiled the function with speculations that kept on failing and
    // we want to generate a more conservative version.
    ICInvalidator dependent_callsites;

    // Metadata that lets us find local variables from the C stack fram.
    LocationMap* location_map;

    // List of metadata objects for ICs inside this compilation
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

    static void visitAllCompiledFunctions(GCVisitor* visitor);
};

typedef int FutureFlags;

class BoxedModule;
class ScopeInfo;
class InternedStringPool;
class LivenessAnalysis;

// Data about a single textual function definition.
class SourceInfo {
private:
    BoxedString* fn; // equivalent of code.co_filename

public:
    BoxedModule* parent_module;
    ScopingAnalysis* scoping;
    ScopeInfo* scope_info;
    FutureFlags future_flags;
    AST* ast;
    CFG* cfg;
    bool is_generator;

    InternedStringPool& getInternedStrings();

    ScopeInfo* getScopeInfo();
    LivenessAnalysis* getLiveness();

    // TODO we're currently copying the body of the AST into here, since lambdas don't really have a statement-based
    // body and we have to create one.  Ideally, we'd be able to avoid the space duplication for non-lambdas.
    const std::vector<AST_stmt*> body;

    BoxedString* getName();
    BoxedString* getFn() { return fn; }

    InternedString mangleName(InternedString id);

    Box* getDocString();

    SourceInfo(BoxedModule* m, ScopingAnalysis* scoping, FutureFlags future_flags, AST* ast,
               std::vector<AST_stmt*> body, BoxedString* fn);
    ~SourceInfo();

private:
    std::unique_ptr<LivenessAnalysis> liveness_info;
};

typedef std::vector<CompiledFunction*> FunctionList;
struct CallRewriteArgs;

// A BoxedCode is our implementation of the Python "code" object (such as function.func_code).
// It is implemented as a wrapper around a FunctionMetadata.
class BoxedCode;

// FunctionMetadata corresponds to metadata about a function definition.  If the same 'def foo():' block gets
// executed multiple times, there will only be a single FunctionMetadata, even though multiple function objects
// will get created from it.
// FunctionMetadata objects can also be created to correspond to C/C++ runtime functions, via FunctionMetadata::create.
//
// FunctionMetadata objects also keep track of any machine code that we have available for this function.
class FunctionMetadata {
private:
    // The Python-level "code" object corresponding to this FunctionMetadata.  We store it in the FunctionMetadata
    // so that multiple attempts to translate from FunctionMetadata->BoxedCode will always return the same
    // BoxedCode object.
    // Callers should use getCode()
    BoxedCode* code_obj;

public:
    int num_args;
    bool takes_varargs, takes_kwargs;

    std::unique_ptr<SourceInfo> source; // source can be NULL for functions defined in the C/C++ runtime
    ParamNames param_names;

    FunctionList
        versions; // any compiled versions along with their type parameters; in order from most preferred to least
    CompiledFunction* always_use_version; // if this version is set, always use it (for unboxed cases)
    std::unordered_map<const OSREntryDescriptor*, CompiledFunction*> osr_versions;

    // Profiling counter:
    int propagated_cxx_exceptions = 0;

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

    FunctionMetadata(int num_args, bool takes_varargs, bool takes_kwargs, std::unique_ptr<SourceInfo> source);
    FunctionMetadata(int num_args, bool takes_varargs, bool takes_kwargs,
                     const ParamNames& param_names = ParamNames::empty());
    ~FunctionMetadata();

    int numReceivedArgs() { return num_args + takes_varargs + takes_kwargs; }

    BoxedCode* getCode();

    bool isGenerator() const {
        if (source)
            return source->is_generator;
        return false;
    }

    // These functions add new compiled "versions" (or, instantiations) of this FunctionMetadata.  The first
    // form takes a CompiledFunction* directly, and the second forms (meant for use by the C++ runtime) take
    // some raw parameters and will create the CompiledFunction behind the scenes.
    void addVersion(CompiledFunction* compiled);
    void addVersion(void* f, ConcreteCompilerType* rtn_type, ExceptionStyle exception_style = CXX);
    void addVersion(void* f, ConcreteCompilerType* rtn_type, const std::vector<ConcreteCompilerType*>& arg_types,
                    ExceptionStyle exception_style = CXX);

    int calculateNumVRegs();

    // Helper function, meant for the C++ runtime, which allocates a FunctionMetadata object and calls addVersion
    // once to it.
    static FunctionMetadata* create(void* f, ConcreteCompilerType* rtn_type, int nargs, bool takes_varargs,
                                    bool takes_kwargs, const ParamNames& param_names = ParamNames::empty(),
                                    ExceptionStyle exception_style = CXX) {
        assert(!param_names.takes_param_names || nargs == param_names.args.size());
        assert(takes_varargs || param_names.vararg.str() == "");
        assert(takes_kwargs || param_names.kwarg.str() == "");

        FunctionMetadata* fmd = new FunctionMetadata(nargs, takes_varargs, takes_kwargs, param_names);
        fmd->addVersion(f, rtn_type, exception_style);
        return fmd;
    }

    static FunctionMetadata* create(void* f, ConcreteCompilerType* rtn_type, int nargs,
                                    const ParamNames& param_names = ParamNames::empty(),
                                    ExceptionStyle exception_style = CXX) {
        return create(f, rtn_type, nargs, false, false, param_names, exception_style);
    }
};


// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
CompiledFunction* compileFunction(FunctionMetadata* f, FunctionSpecialization* spec, EffortLevel effort,
                                  const OSREntryDescriptor* entry, bool force_exception_style = false,
                                  ExceptionStyle forced_exception_style = CXX);
EffortLevel initialEffort();

typedef bool i1;
typedef int64_t i64;

const char* getNameOfClass(BoxedClass* cls);
std::string getFullNameOfClass(BoxedClass* cls);
std::string getFullTypeName(Box* o);
const char* getTypeName(Box* b);


class Rewriter;
class RewriterVar;
class RuntimeIC;
class CallattrIC;
class CallattrCapiIC;
class NonzeroIC;
class BinopIC;

class Box;

class BoxIteratorImpl : public gc::GCAllocatedRuntime {
public:
    virtual ~BoxIteratorImpl() = default;
    virtual void next() = 0;
    virtual Box* getValue() = 0;
    virtual bool isSame(const BoxIteratorImpl* rhs) = 0;

    virtual void gc_visit(GCVisitor* v) = 0;
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
inline BoxedString* internStringMortal(llvm::StringRef s) {
    return internStringImmortal(s);
}

// TODO this is an immortal intern for now
inline void internStringMortalInplace(BoxedString*& s) noexcept {
    PyString_InternInPlace((PyObject**)&s);
}

// The data structure definition for hidden-class-based attributes.  Consists of a
// pointer to the hidden class object, and a pointer to a variable-size attributes array.
struct HCAttrs {
public:
    struct AttrList {
        Box* attrs[0];
    };

    HiddenClass* hcls;
    AttrList* attr_list;

    HCAttrs(HiddenClass* hcls = root_hcls) : hcls(hcls), attr_list(nullptr) {}
};
static_assert(sizeof(HCAttrs) == sizeof(struct _hcattrs), "");

class BoxedDict;
class BoxedString;

// "Box" is the base class of any C++ type that implements a Python type.  For example,
// BoxedString is the data structure that implements Python's str type, and BoxedString
// inherits from Box.
//
// This is the same as CPython's PyObject (and they are interchangeable), with the difference
// since we are in C++ (whereas CPython is in C) we can use C++ inheritance to implement
// Python inheritance, and avoid the raw pointer casts that CPython needs everywhere.
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

    // For instances with hc attrs:
    size_t getHCAttrsOffset();
    HCAttrs* getHCAttrsPtr();
    void setDictBacked(Box* d);
    // For instances with dict attrs:
    BoxedDict* getDict();
    void setDict(BoxedDict* d);


    void setattr(BoxedString* attr, Box* val, SetattrRewriteArgs* rewrite_args);
    void giveAttr(const char* attr, Box* val) { giveAttr(internStringMortal(attr), val); }
    void giveAttr(BoxedString* attr, Box* val) {
        assert(!this->hasattr(attr));
        this->setattr(attr, val, NULL);
    }

    // getattr() does the equivalent of PyDict_GetItem(obj->dict, attr): it looks up the attribute's value on the
    // object's attribute storage. it doesn't look at other objects or do any descriptor logic.
    template <Rewritable rewritable = REWRITABLE>
    Box* getattr(BoxedString* attr, GetattrRewriteArgs* rewrite_args);
    Box* getattr(BoxedString* attr) { return getattr<NOT_REWRITABLE>(attr, NULL); }
    bool hasattr(BoxedString* attr) { return getattr(attr) != NULL; }
    void delattr(BoxedString* attr, DelattrRewriteArgs* rewrite_args);

    // Only valid for hc-backed instances:
    Box* getAttrWrapper();

    Box* reprIC();
    BoxedString* reprICAsString();
    bool nonzeroIC();
    Box* hasnextOrNullIC();

    friend class AttrWrapper;

    static void gcHandler(GCVisitor* v, Box* b);
};
static_assert(offsetof(Box, cls) == offsetof(struct _object, ob_type), "");

// Our default for tp_alloc:
extern "C" PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept;

// These are some macros for tying the C++ type hiercharchy to the Pyston type hiercharchy.
// Classes that inherit from Box have a special operator new() that takes a class object (as
// a BoxedClass*) since the class is necessary for object allocation.
// To enable expressions such as `new BoxedString()` instead of having to type
// `new (str_cls) BoxedString()` everywhere, we need to tell C++ what the default class is.
// We can do this by putting `DEFAULT_CLASS(str_cls);` anywhere in the definition of BoxedString.
#define DEFAULT_CLASS(default_cls)                                                                                     \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        assert(cls->tp_itemsize == 0);                                                                                 \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) {                                           \
        assert(default_cls->tp_itemsize == 0);                                                                         \
        return Box::operator new(size, default_cls);                                                                   \
    }

#if STAT_ALLOCATION_TYPES
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


// In the simple cases, we can inline the fast paths of the following methods and improve allocation speed quite a bit:
// - Box::operator new
// - cls->tp_alloc
// - PystonType_GenericAlloc
// - PyObject_Init
// The restrictions on when you can use the SIMPLE (ie fast) variant are encoded as
// asserts in the 1-arg operator new function:
#define DEFAULT_CLASS_SIMPLE(default_cls)                                                                              \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) {                                           \
        ALLOC_STATS(default_cls);                                                                                      \
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

// This corresponds to CPython's PyVarObject, for objects with a variable number of "items" that are stored inline.
// For example, strings and tuples store their data in line in the main object allocation, so are BoxVars.  Lists,
// since they have a changeable size, store their elements in a separate array, and their main object is a fixed
// size and so aren't BoxVar.
class BoxVar : public Box {
public:
    // This field gets initialized in operator new.
    Py_ssize_t ob_size;

    BoxVar() {}

    void* operator new(size_t size, BoxedClass* cls, size_t nitems) __attribute__((visibility("default")));
};
static_assert(offsetof(BoxVar, ob_size) == offsetof(struct _varobject, ob_size), "");

// This is the variant of DEFAULT_CLASS that applies to BoxVar objects.
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

class BoxedClass;

// TODO these shouldn't be here
void setupRuntime();
void teardownRuntime();
Box* createAndRunModule(BoxedString* name, const std::string& fn);
BoxedModule* createModule(BoxedString* name, const char* fn = NULL, const char* doc = NULL);
Box* moduleInit(BoxedModule* self, Box* name, Box* doc = NULL);

// TODO where to put this
void appendToSysPath(llvm::StringRef path);
void prependToSysPath(llvm::StringRef path);
void addToSysArgv(const char* str);

// Raise a SyntaxError that occurs at a specific location.
// The traceback given to the user will include this,
// even though the execution didn't actually arrive there.
// CPython has slightly different behavior depending on where in the pipeline (parser vs compiler)
// the SyntaxError was thrown; setting compiler_error=True is for the case that it was thrown in
// the compiler portion (which calls a function called compiler_error()).
void raiseSyntaxError(const char* msg, int lineno, int col_offset, llvm::StringRef file, llvm::StringRef func,
                      bool compiler_error = false);
void raiseSyntaxErrorHelper(llvm::StringRef file, llvm::StringRef func, AST* node_at, const char* msg, ...)
    __attribute__((format(printf, 4, 5)));

// A data structure used for storing information for tracebacks.
struct LineInfo {
public:
    int line, column;
    BoxedString* file, *func;

    LineInfo(int line, int column, BoxedString* file, BoxedString* func)
        : line(line), column(column), file(file), func(func) {}
};

// A data structure to simplify passing around all the data about a thrown exception.
struct ExcInfo {
    Box* type, *value, *traceback;

    constexpr ExcInfo(Box* type, Box* value, Box* traceback) : type(type), value(value), traceback(traceback) {}
    bool matches(BoxedClass* cls) const;
    void printExcAndTraceback() const;
};

// Our object that implements Python's "frame" object:
class BoxedFrame;

// Our internal data structure for storing certain information about a stack frame.
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
    BoxedClosure* passed_closure;

    Box** vregs;
    // Current statement
    // Caution the llvm tier only updates this information on direct external calls but not for patchpoints.
    // This means if a patchpoint "current_stmt" info is available it must be used instead of this field.
    AST_stmt* stmt;
    // This is either a module or a dict
    Box* globals;

    FrameInfo(ExcInfo exc) : exc(exc), boxedLocals(NULL), frame_obj(0), passed_closure(0), vregs(0), stmt(0), globals(0) {}

    void gcVisit(GCVisitor* visitor);
};

// callattr() takes a number of flags and arguments, and for performance we pack them into a single register:
struct CallattrFlags {
    bool cls_only : 1;
    bool null_on_nonexistent : 1;
    ArgPassSpec argspec;

    uint64_t asInt() { return (uint64_t(argspec.asInt()) << 32) | (cls_only << 0) | (null_on_nonexistent << 1); }
};
static_assert(sizeof(CallattrFlags) == sizeof(uint64_t), "");

// A C++-style RAII way of handling a PyArena*
class ArenaWrapper {
private:
    PyArena* arena;

public:
    ArenaWrapper() { arena = PyArena_New(); }
    ~ArenaWrapper() {
        if (arena)
            PyArena_Free(arena);
    }

    operator PyArena*() const { return arena; }
};

// A C++-style RAII way of handling a FILE*
class FileHandle {
private:
    FILE* file;

public:
    FileHandle(const char* fn, const char* mode) : file(fopen(fn, mode)) {}
    ~FileHandle() {
        if (file)
            fclose(file);
    }

    operator FILE*() const { return file; }
};

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

// We need to override these functions so that our GC can know about them.
namespace std {
template <> std::pair<pyston::Box**, std::ptrdiff_t> get_temporary_buffer<pyston::Box*>(std::ptrdiff_t count) noexcept;
template <> void return_temporary_buffer<pyston::Box*>(pyston::Box** p);
}

#endif
