// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include <forward_list>
#include <memory>
#include <stddef.h>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/TinyPtrVector.h"
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
// "NOEXC" is a special exception style that says that no exception (of either type) will get thrown.
enum ExceptionStyle {
    CAPI,
    CXX,

    NOEXC,
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

enum class RefType {
    UNKNOWN
#ifndef NDEBUG
    // Set this to non-zero to make it possible for the debugger to
    = 1
#endif
    ,
    OWNED,
    BORROWED,
};

template <typename T> struct ExceptionSwitchable {
public:
    T capi_val;
    T cxx_val;

    ExceptionSwitchable() : capi_val(), cxx_val() {}
    ExceptionSwitchable(T capi_val, T cxx_val) : capi_val(std::move(capi_val)), cxx_val(std::move(cxx_val)) {}

    template <ExceptionStyle S> T& get() {
        if (S == CAPI)
            return capi_val;
        else
            return cxx_val;
    }

    T& get(ExceptionStyle S) {
        if (S == CAPI)
            return capi_val;
        else
            return cxx_val;
    }

    bool empty() const { return !capi_val && !cxx_val; }
    void clear() { *this = ExceptionSwitchable<T>(); }
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
class BoxedCode;

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

    int starargsIndex() const {
        assert(has_starargs);
        return num_args + num_keywords;
    }

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
    // the arguments are either an array of char* or AST_Name* depending on if all_args_contains_names is set or not
    union NameOrStr {
        NameOrStr(const char* str) : str(str) {}
        NameOrStr(AST_Name* name) : name(name) {}

        const char* str;
        AST_Name* name;
    };
    std::vector<NameOrStr> all_args;

    const unsigned char all_args_contains_names : 1;
    const unsigned char takes_param_names : 1;
    unsigned char has_vararg_name : 1;
    unsigned char has_kwarg_name : 1;

    explicit ParamNames(AST* ast, InternedStringPool& pool);
    ParamNames(const std::vector<const char*>& args, const char* vararg, const char* kwarg);
    static ParamNames empty() { return ParamNames(); }

    int numNormalArgs() const { return all_args.size() - has_vararg_name - has_kwarg_name; }
    int totalParameters() const { return all_args.size(); }

    int kwargsIndex() const {
        assert(has_kwarg_name);
        return all_args.size() - 1;
    }

    llvm::ArrayRef<AST_Name*> argsAsName() const {
        assert(all_args_contains_names);
        return llvm::makeArrayRef((AST_Name * const*)all_args.data(), numNormalArgs());
    }

    llvm::ArrayRef<AST_Name*> allArgsAsName() const {
        assert(all_args_contains_names);
        return llvm::makeArrayRef((AST_Name * const*)all_args.data(), all_args.size());
    }

    std::vector<const char*> allArgsAsStr() const;

    AST_Name* varArgAsName() const {
        assert(all_args_contains_names);
        if (has_vararg_name)
            return all_args[all_args.size() - 1 - has_kwarg_name].name;
        return NULL;
    }

    AST_Name* kwArgAsName() const {
        assert(all_args_contains_names);
        if (has_kwarg_name)
            return all_args.back().name;
        return NULL;
    }

private:
    ParamNames() : all_args_contains_names(0), takes_param_names(0), has_vararg_name(0), has_kwarg_name(0) {}
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

    bool operator==(ParamReceiveSpec rhs) const {
        return takes_varargs == rhs.takes_varargs && takes_kwargs == rhs.takes_kwargs
               && num_defaults == rhs.num_defaults && num_args == rhs.num_args;
    }

    bool operator!=(ParamReceiveSpec rhs) const { return !(*this == rhs); }

    int totalReceived() const { return num_args + (takes_varargs ? 1 : 0) + (takes_kwargs ? 1 : 0); }
    int varargsIndex() const { return num_args; }
    int kwargsIndex() const { return num_args + (takes_varargs ? 1 : 0); }
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
    llvm::SmallPtrSet<ICSlotInfo*, 2> dependents;

public:
    ICInvalidator() : cur_version(0) {}
    ~ICInvalidator();

    void addDependent(ICSlotInfo* icentry);
    int64_t version();
    void invalidateAll();
    void remove(ICSlotInfo* icentry) { dependents.erase(icentry); }

    friend class ICInfo;
    friend class ICSlotInfo;
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
class FrameInfo;

extern std::vector<Box*> constants;
extern std::vector<Box*> late_constants; // constants that should be freed after normal constants

// A specific compilation of a FunctionMetadata.  Usually these will be created by the LLVM JIT, which will take a
// FunctionMetadata
// and some compilation settings, and produce a CompiledFunction
// CompiledFunctions can also be created from raw function pointers, using FunctionMetadata::create.
// A single FunctionMetadata can have multiple CompiledFunctions associated with it, if they have different settings.
// Typically, this will happen due to specialization on the argument types (ie we will generate a separate versions
// of a function that are faster but only apply to specific argument types).
struct CompiledFunction {
private:
public:
    BoxedCode* code_obj;

    // Some compilation settings:
    EffortLevel effort;
    ExceptionStyle exception_style;
    FunctionSpecialization* spec;
    // If this compilation was due to an OSR, `entry_descriptor` contains metadata about the OSR.
    // Otherwise this field is NULL.
    const OSREntryDescriptor* entry_descriptor;

    // The function pointer to the generated code.  For convenience, it can be accessed
    // as one of many different types.
    // TODO: we instead make these accessor-functions that make sure that the code actually
    // matches the C signature that we would return.
    union {
        Box* (*call)(Box*, Box*, Box*, Box**);
        Box* (*closure_call)(BoxedClosure*, Box*, Box*, Box*, Box**);
        Box* (*closure_generator_call)(BoxedClosure*, BoxedGenerator*, Box*, Box*, Box*, Box**);
        Box* (*generator_call)(BoxedGenerator*, Box*, Box*, Box*, Box**);
        Box* (*call1)(Box*, Box*, Box*, Box*, Box**);
        Box* (*call2)(Box*, Box*, Box*, Box*, Box*, Box**);
        Box* (*call3)(Box*, Box*, Box*, Box*, Box*, Box*, Box**);
        Box* (*call_osr)(BoxedGenerator*, BoxedClosure*, FrameInfo*, Box**);
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
    std::unique_ptr<LocationMap> location_map;

    // List of metadata objects for ICs inside this compilation
    std::vector<std::unique_ptr<ICInfo>> ics;

    CompiledFunction(BoxedCode* code_obj, FunctionSpecialization* spec, void* code, EffortLevel effort,
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
class InternedStringPool;
class LivenessAnalysis;

// Each closure has an array (fixed-size for that particular scope) of variables
// and a parent pointer to a parent closure. To look up a variable from the passed-in
// closure (i.e., DEREF), you just need to know (i) how many parents up to go and
// (ii) what offset into the array to find the variable. This struct stores that
// information. You can query the ScopeInfo with a name to get this info.
struct DerefInfo {
    size_t num_parents_from_passed_closure;
    size_t offset;
};

class ScopeInfo;
// The results of our scoping analysis.
// A ScopeInfo is a component of the analysis itself and contains a lot of other
// metadata that is necessary during the analysis, after which we can throw it
// away and only keep a ScopingResults object.
struct ScopingResults {
private:
    bool are_locals_from_module : 1;
    bool are_globals_from_module : 1;
    bool creates_closure : 1;
    bool takes_closure : 1;
    bool passes_through_closure : 1;
    bool uses_name_lookup : 1;

    int closure_size;
    std::vector<std::pair<InternedString, DerefInfo>> deref_info;

public:
    ScopingResults(ScopingResults&&) = default;
    // Delete these just to make sure we avoid extra copies
    ScopingResults(const ScopingResults&) = delete;
    void operator=(const ScopingResults&) = delete;

    bool areLocalsFromModule() const { return are_locals_from_module; }
    bool areGlobalsFromModule() const { return are_globals_from_module; }
    bool createsClosure() const { return creates_closure; }
    bool takesClosure() const { return takes_closure; }
    bool passesThroughClosure() const { return passes_through_closure; }
    bool usesNameLookup() const { return uses_name_lookup; }

    int getClosureSize() const {
        assert(createsClosure());
        return closure_size;
    }
    const std::vector<std::pair<InternedString, DerefInfo>>& getAllDerefVarsAndInfo() const { return deref_info; }
    DerefInfo getDerefInfo(AST_Name*) const;
    size_t getClosureOffset(AST_Name*) const;

    ScopingResults(ScopeInfo* scope_info, bool globals_from_module);
};

// Data about a single textual function definition.
class SourceInfo {
private:
    BoxedString* fn; // equivalent of code.co_filename
    std::unique_ptr<LivenessAnalysis> liveness_info;

public:
    BoxedModule* parent_module;
    ScopingResults scoping;
    AST* ast;
    CFG* cfg;
    FutureFlags future_flags;
    bool is_generator;

    LivenessAnalysis* getLiveness();

    // does not throw CXX or CAPI exceptions:
    BORROWED(BoxedString*) getName() noexcept;
    BORROWED(BoxedString*) getFn();

    llvm::ArrayRef<AST_stmt*> getBody() const;

    Box* getDocString();

    SourceInfo(BoxedModule* m, ScopingResults scoping, FutureFlags future_flags, AST* ast, BoxedString* fn);
    ~SourceInfo();
};

typedef llvm::TinyPtrVector<CompiledFunction*> FunctionList;
struct CallRewriteArgs;


// Compiles a new version of the function with the given signature and adds it to the list;
// should only be called after checking to see if the other versions would work.
CompiledFunction* compileFunction(BoxedCode* code, FunctionSpecialization* spec, EffortLevel effort,
                                  const OSREntryDescriptor* entry, bool force_exception_style = false,
                                  ExceptionStyle forced_exception_style = CXX);
EffortLevel initialEffort();

#if BOOLS_AS_I64
typedef int64_t llvm_compat_bool;
#else
typedef bool llvm_compat_bool;
#endif
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

#define Py_TRAVERSE(obj)                                                                                               \
    do {                                                                                                               \
        int vret = (obj).traverse(visit, arg);                                                                         \
        if (vret)                                                                                                      \
            return vret;                                                                                               \
    } while (0)


class BoxIteratorImpl {
public:
    virtual ~BoxIteratorImpl() = default;
    virtual void next() = 0;
    virtual Box* getValue() = 0;
    virtual bool isSame(const BoxIteratorImpl* rhs) = 0;
    virtual int traverse(visitproc visit, void* arg) = 0;
};

class BoxIterator {
public:
    BoxIteratorImpl* impl;

    BoxIterator(BoxIteratorImpl* impl) : impl(impl) {}

    bool operator==(BoxIterator const& rhs) const { return impl->isSame(rhs.impl); }
    bool operator!=(BoxIterator const& rhs) const { return !(*this == rhs); }

    BoxIterator& operator++() {
        impl->next();
        return *this;
    }

    Box* operator*() const { return impl->getValue(); }
    Box* operator*() { return impl->getValue(); }
};

// Similar to std::unique_ptr<>, but allocates its data on the stack.
// This means that it should only be used with types that can be relocated trivially.
// TODO add assertions for that, similar to SmallFunction.
// Also, if you copy the SmallUniquePtr, the address that it represents changes (since you
// copy the data as well).  In debug mode, this class will enforce that once you get the
// pointer value, it does not get copied again.
template <typename T, int N> class SmallUniquePtr {
private:
    char _data[N];
    bool owned;
#ifndef NDEBUG
    bool address_taken = false;
#endif

    template <typename ConcreteType, typename... Args> SmallUniquePtr(ConcreteType* dummy, Args... args) {
        static_assert(sizeof(ConcreteType) <= N, "SmallUniquePtr not large enough to contain this object");
        new (_data) ConcreteType(std::forward<Args>(args)...);
        owned = true;
    }

public:
    template <typename ConcreteType, typename... Args> static SmallUniquePtr emplace(Args... args) {
        return SmallUniquePtr<T, N>((ConcreteType*)nullptr, args...);
    }

    SmallUniquePtr(const SmallUniquePtr&) = delete;
    SmallUniquePtr(SmallUniquePtr&& rhs) { *this = std::move(rhs); }
    void operator=(const SmallUniquePtr&) = delete;
    void operator=(SmallUniquePtr&& rhs) {
        assert(!rhs.address_taken && "Invalid copy after being converted to a pointer");
        std::swap(_data, rhs._data);
        owned = false;
        std::swap(owned, rhs.owned);
    }

    ~SmallUniquePtr() {
        if (owned)
            ((T*)this)->~T();
    }
    operator T*() {
#ifndef NDEBUG
        address_taken = true;
#endif
        return reinterpret_cast<T*>(_data);
    }
};

// A custom "range" container that helps manage lifetimes.  We need to free the underlying Impl object
// when the range loop is done; previously we had the iterator itself handle this, but that started
// to get complicated since they get copied around, and the management of the begin() and end() iterators
// is slightly different.
// So to simplify, have the range object take care of it.
//
// Note: be careful when explicitly calling begin().  The returned iterator points into this BoxIteratorRange
// object, so once you call begin() it is a bug to move/copy this BoxIteratorRange object (the SmallUniquePtr
// should complain).
class BoxIteratorRange {
private:
    typedef SmallUniquePtr<BoxIteratorImpl, 32> UniquePtr;
    UniquePtr begin_impl;
    BoxIteratorImpl* end_impl;

public:
    template <typename ImplType, typename T>
    BoxIteratorRange(BoxIteratorImpl* end, T&& arg, ImplType* dummy)
        : begin_impl(UniquePtr::emplace<ImplType, T>(arg)), end_impl(end) {}

    BoxIterator begin() { return BoxIterator(begin_impl); }
    BoxIterator end() { return BoxIterator(end_impl); }

    int traverse(visitproc visit, void* arg) {
        Py_TRAVERSE(*begin_impl);
        Py_TRAVERSE(*end_impl);
        return 0;
    }
};

class HiddenClass;
class HiddenClassNormal;
extern HiddenClassNormal* root_hcls;

struct SetattrRewriteArgs;
struct GetattrRewriteArgs;
struct DelattrRewriteArgs;
struct UnaryopRewriteArgs;

// Helper function around PyString_InternFromString:
BoxedString* internStringImmortal(llvm::StringRef s) noexcept;

// Callers should use this function if they can accept mortal string objects.
// FIXME For now it just returns immortal strings, but at least we can use it
// to start documenting the places that can take mortal strings.
inline BoxedString* internStringMortal(llvm::StringRef s) noexcept {
    return internStringImmortal(s);
}

// TODO this is an immortal intern for now
// Ref usage: this transfers a ref from the passed-in string to the new one.
// Typical usage:
// Py_INCREF(s); // or otherwise make sure it was owned
// internStringMortalInplace(s);
// AUTO_DECREF(s);
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

    HCAttrs(HiddenClass* hcls = NULL) : hcls(hcls), attr_list(nullptr) {}

    int traverse(visitproc visit, void* arg) noexcept;

    void _clearRaw() noexcept;       // Raw clear -- clears out and decrefs all the attrs.
                                     // Meant for implementing other clear-like functions
    void clearForDealloc() noexcept; // meant for normal object deallocation.  converts the attrwrapper
    void moduleClear() noexcept;     // Meant for _PyModule_Clear.  doesn't clear all attributes.
};
static_assert(sizeof(HCAttrs) == sizeof(struct _hcattrs), "");

extern std::vector<BoxedClass*> classes;

// Debugging helper: pass this as a tp_clear function to say that you have explicitly verified
// that you don't need a tp_clear (as opposed to haven't added one yet)
#define NOCLEAR ((inquiry)-1)

class BoxedDict;
class BoxedString;

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


// These are just dummy objects to help us differentiate operator new() versions from each other, since we can't use
// normal templating or different function names.
struct FastToken {};
extern FastToken FAST;
struct FastGCToken {};
extern FastGCToken FAST_GC;


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
    void appendNewHCAttr(BORROWED(Box*) val, SetattrRewriteArgs* rewrite_args);

protected:
    // newFast(): a fast implementation of operator new() that optimizes for the common case.  It does this
    // by inlining the following methods and skipping most of the dynamic checks:
    // - Box::operator new
    // - cls->tp_alloc
    // - PyType_GenericAlloc
    // - PyObject_Init
    // The restrictions on when you can use the fast variant are encoded as assertions in the implementation
    // (see runtime/types.h)
    template <bool is_gc> static void* newFast(size_t size, BoxedClass* cls);

public:
    // Add a no-op constructor to make sure that we don't zero-initialize cls
    Box() {}

    void* operator new(size_t size, BoxedClass* cls) __attribute__((visibility("default")));

    void* operator new(size_t size, BoxedClass* cls, FastToken _dummy) { return newFast<false>(size, cls); }
    void* operator new(size_t size, BoxedClass* cls, FastGCToken _dummy) { return newFast<true>(size, cls); }

    void operator delete(void* ptr) __attribute__((visibility("default"))) { abort(); }

    _PyObject_HEAD_EXTRA;

    Py_ssize_t ob_refcnt;

    // Note: cls gets initialized in the new() function.
    BoxedClass* cls;

    BoxIteratorRange pyElements();

    // For instances with hc attrs:
    size_t getHCAttrsOffset();
    HCAttrs* getHCAttrsPtr();
    void setDictBacked(STOLEN(Box*) d);
    // For instances with dict attrs:
    BORROWED(BoxedDict*) getDict();


    // Note, setattr does *not* steal a reference, but it probably should
    void setattr(BoxedString* attr, BORROWED(Box*) val, SetattrRewriteArgs* rewrite_args);
    // giveAttr consumes a reference to val and attr
    void giveAttr(const char* attr, STOLEN(Box*) val) { giveAttr(internStringMortal(attr), val); }
    // giveAttrBorrowed consumes a reference only to attr (but it only has the const char* variant
    // which creates the reference).  should probably switch the names to stay consistent; most functions
    // don't steal references.
    void giveAttrBorrowed(const char* attr, Box* val) {
        Py_INCREF(val);
        giveAttr(internStringMortal(attr), val);
    }
    void giveAttr(STOLEN(BoxedString*) attr, STOLEN(Box*) val);

    void clearAttrsForDealloc();

    void giveAttrDescriptor(const char* attr, Box* (*get)(Box*, void*), int (*set)(Box*, Box*, void*));
    void giveAttrMember(const char* attr, int type, ssize_t offset, bool readonly = true);

    // getattr() does the equivalent of PyDict_GetItem(obj->dict, attr): it looks up the attribute's value on the
    // object's attribute storage. it doesn't look at other objects or do any descriptor logic.
    template <Rewritable rewritable = REWRITABLE>
    BORROWED(Box*) getattr(BoxedString* attr, GetattrRewriteArgs* rewrite_args);
    BORROWED(Box*) getattr(BoxedString* attr) { return getattr<NOT_REWRITABLE>(attr, NULL); }
    BORROWED(Box*) getattrString(const char* attr);

    bool hasattr(BoxedString* attr) { return getattr(attr) != NULL; }
    void delattr(BoxedString* attr, DelattrRewriteArgs* rewrite_args);

    // Only valid for hc-backed instances:
    BORROWED(Box*) getAttrWrapper();

    Box* reprIC(); // returns null on nonexistent!
    Box* strIC();  // returns null on nonexistent!
    BoxedString* reprICAsString();
    bool nonzeroIC();
    Box* hasnextOrNullIC();

    friend class AttrWrapper;

#ifdef Py_TRACE_REFS
    static Box createRefchain() {
        Box rtn;
        rtn._ob_next = &rtn;
        rtn._ob_prev = &rtn;
        return rtn;
    }
#endif
};
static_assert(offsetof(Box, cls) == offsetof(struct _object, ob_type), "");

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

// A faster version that can be used for classes that can use "FAST" operator new
#define DEFAULT_CLASS_SIMPLE(default_cls, is_gc)                                                                       \
    void* operator new(size_t size, BoxedClass * cls) __attribute__((visibility("default"))) {                         \
        return Box::operator new(size, cls);                                                                           \
    }                                                                                                                  \
    void* operator new(size_t size) __attribute__((visibility("default"))) { return newFast<is_gc>(size, default_cls); }

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

// TODO: extract out newFastVar like we did with newFast
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
        assert(default_cls->tp_alloc == PyType_GenericAlloc);                                                          \
        assert(default_cls->tp_itemsize == itemsize);                                                                  \
        assert(default_cls->tp_basicsize == size);                                                                     \
        assert(default_cls->is_pyston_class);                                                                          \
        assert(default_cls->attrs_offset == 0);                                                                        \
                                                                                                                       \
        void* mem = PyObject_MALLOC(size + nitems * itemsize);                                                         \
        assert(mem);                                                                                                   \
                                                                                                                       \
        BoxVar* rtn = static_cast<BoxVar*>(mem);                                                                       \
        rtn->cls = default_cls;                                                                                        \
        _Py_NewReference(rtn);                                                                                         \
        rtn->ob_size = nitems;                                                                                         \
        return rtn;                                                                                                    \
    }

class BoxedClass;

// TODO these shouldn't be here
void setupRuntime();
Box* createAndRunModule(BoxedString* name, const std::string& fn);
BORROWED(BoxedModule*) createModule(BoxedString* name, const char* fn = NULL, const char* doc = NULL) noexcept;
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
    void clear();
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
    //
    // Note: Adding / moving any fields here requires updating the "getXxxGEP()" functions in irgenerator.cpp
    ExcInfo exc;

    // This field is always initialized:
    Box* boxedLocals;

    BoxedFrame* frame_obj;
    BORROWED(BoxedClosure*) passed_closure;

    Box** vregs;
    int num_vregs;

    AST_stmt* stmt; // current statement
    // This is either a module or a dict
    BORROWED(Box*) globals;

    FrameInfo* back;
    // TODO does this need to be owned?  how does cpython do it?
    BORROWED(BoxedCode*) code;

    BORROWED(Box*) updateBoxedLocals();

    static FrameInfo* const NO_DEINIT;

    // Calling disableDeinit makes future deinitFrameMaybe() frames not call deinitFrame().
    // For use by deopt(), which takes over deinit responsibility for its caller.
    void disableDeinit(FrameInfo* replacement_frame);
    bool isDisabledFrame() const { return back == NO_DEINIT; }

    FrameInfo(ExcInfo exc)
        : exc(exc),
          boxedLocals(NULL),
          frame_obj(0),
          passed_closure(0),
          vregs(0),
          num_vregs(INT_MAX),
          stmt(0),
          globals(0),
          back(0),
          code(0) {}
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
#endif
