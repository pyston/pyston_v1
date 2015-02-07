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

#ifndef PYSTON_RUNTIME_TYPES_H
#define PYSTON_RUNTIME_TYPES_H

#include <ucontext.h>

#include "Python.h"
#include "structmember.h"

#include "codegen/irgen/future.h"
#include "core/threading.h"
#include "core/types.h"
#include "gc/gc_alloc.h"

namespace pyston {

extern bool IN_SHUTDOWN;

class BoxedString;
class BoxedList;
class BoxedDict;
class BoxedTuple;
class BoxedFile;
class BoxedClosure;
class BoxedGenerator;

void setupInt();
void teardownInt();
void setupFloat();
void teardownFloat();
void setupComplex();
void teardownComplex();
void setupStr();
void teardownStr();
void setupList();
void teardownList();
void list_dtor(BoxedList* l);
void setupBool();
void teardownBool();
void dict_dtor(BoxedDict* d);
void setupDict();
void teardownDict();
void tuple_dtor(BoxedTuple* d);
void setupTuple();
void teardownTuple();
void file_dtor(BoxedFile* d);
void setupFile();
void teardownFile();
void setupCAPI();
void teardownCAPI();
void setupGenerator();
void setupUnicode();
void setupDescr();
void teardownDescr();

void setupSys();
void setupBuiltins();
void setupPyston();
void setupThread();
void setupSysEnd();

BoxedDict* getSysModulesDict();
BoxedList* getSysPath();
extern "C" Box* getSysStdout();

extern "C" {
extern BoxedClass* object_cls, *type_cls, *bool_cls, *int_cls, *long_cls, *float_cls, *str_cls, *function_cls,
    *none_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls,
    *enumerate_cls, *xrange_cls, *member_cls, *method_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls,
    *unicode_cls, *property_cls, *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *getset_cls,
    *builtin_function_or_method_cls;
}
extern "C" {
extern Box* None, *NotImplemented, *True, *False;
}
extern "C" {
extern Box* repr_obj, *len_obj, *hash_obj, *range_obj, *abs_obj, *min_obj, *max_obj, *open_obj, *id_obj, *chr_obj,
    *ord_obj, *trap_obj;
} // these are only needed for functionRepr, which is hacky
extern "C" {
extern BoxedModule* sys_module, *builtins_module, *math_module, *time_module, *thread_module;
}

extern "C" Box* boxBool(bool);
extern "C" Box* boxInt(i64);
extern "C" i64 unboxInt(Box*);
extern "C" Box* boxFloat(double d);
extern "C" Box* boxInstanceMethod(Box* obj, Box* func);
extern "C" Box* boxUnboundInstanceMethod(Box* func);

extern "C" Box* boxStringPtr(const std::string* s);
Box* boxString(const std::string& s);
Box* boxString(std::string&& s);
extern "C" BoxedString* boxStrConstant(const char* chars);
extern "C" BoxedString* boxStrConstantSize(const char* chars, size_t n);

// creates an uninitialized string of length n; useful for directly constructing into the string and avoiding copies:
BoxedString* createUninitializedString(ssize_t n);
// Gets a writeable pointer to the contents of a string.
// Is only meant to be used with something just created from createUninitializedString(), though
// in theory it might work in more cases.
char* getWriteableStringContents(BoxedString* s);

extern "C" void listAppendInternal(Box* self, Box* v);
extern "C" void listAppendArrayInternal(Box* self, Box** v, int nelts);
extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, bool isGenerator,
                              std::initializer_list<Box*> defaults);
extern "C" CLFunction* unboxCLFunction(Box* b);
extern "C" Box* createUserClass(const std::string* name, Box* base, Box* attr_dict);
extern "C" double unboxFloat(Box* b);
extern "C" Box* createDict();
extern "C" Box* createList();
extern "C" Box* createSlice(Box* start, Box* stop, Box* step);
extern "C" Box* createTuple(int64_t nelts, Box** elts);
extern "C" void printFloat(double d);

Box* objectStr(Box*);
Box* objectRepr(Box*);


template <class T> class StlCompatAllocator {
public:
    typedef size_t size_type;
    typedef T value_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef std::ptrdiff_t difference_type;

    StlCompatAllocator() {}
    template <class U> StlCompatAllocator(const StlCompatAllocator<U>& other) {}

    template <class U> struct rebind { typedef StlCompatAllocator<U> other; };

    pointer allocate(size_t n) {
        size_t to_allocate = n * sizeof(value_type);
        // assert(to_allocate < (1<<16));

        return reinterpret_cast<pointer>(gc_alloc(to_allocate, gc::GCKind::CONSERVATIVE));
    }

    void deallocate(pointer p, size_t n) { gc::gc_free(p); }

    // I would never be able to come up with this on my own:
    // http://en.cppreference.com/w/cpp/memory/allocator/construct
    template <class U, class... Args> void construct(U* p, Args&&... args) {
        ::new ((void*)p) U(std::forward<Args>(args)...);
    }

    template <class U> void destroy(U* p) { p->~U(); }

    bool operator==(const StlCompatAllocator<T>& rhs) const { return true; }
    bool operator!=(const StlCompatAllocator<T>& rhs) const { return false; }
};

template <typename K, typename V, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>>
class conservative_unordered_map
    : public std::unordered_map<K, V, Hash, KeyEqual, StlCompatAllocator<std::pair<const K, V>>> {};

class BoxedClass : public BoxVar {
public:
    typedef void (*gcvisit_func)(GCVisitor*, Box*);

public:
    PyTypeObject_BODY;

    HCAttrs attrs;

    // If the user sets __getattribute__ or __getattr__, we will have to invalidate
    // all getattr IC entries that relied on the fact that those functions didn't exist.
    // Doing this via invalidation means that instance attr lookups don't have
    // to guard on anything about the class.
    ICInvalidator dependent_icgetattrs;

    // TODO: these don't actually get deallocated right now
    std::unique_ptr<CallattrIC> hasnext_ic, next_ic, repr_ic;
    std::unique_ptr<NonzeroIC> nonzero_ic;
    Box* callHasnextIC(Box* obj, bool null_on_nonexistent);
    Box* callNextIC(Box* obj);
    Box* callReprIC(Box* obj);
    bool callNonzeroIC(Box* obj);

    gcvisit_func gc_visit;

    // Offset of the HCAttrs object or 0 if there are no hcattrs.
    // Analogous to tp_dictoffset
    const int attrs_offset;

    bool instancesHaveHCAttrs() { return attrs_offset != 0; }
    bool instancesHaveDictAttrs() { return tp_dictoffset != 0; }

    // Whether this class object is constant or not, ie whether or not class-level
    // attributes can be changed or added.
    // Does not necessarily imply that the instances of this class are constant,
    // though for now (is_constant && !hasattrs) does imply that the instances are constant.
    bool is_constant;

    // Whether this class was defined by the user or is a builtin type.
    // this is used mostly for debugging.
    bool is_user_defined;

    // Whether this is a Pyston-defined class (as opposed to an extension-defined class).
    // We can ensure certain behavior about our Pyston classes (in particular around GC support)
    // that we can't rely on for extension classes.
    bool is_pyston_class;

    // will need to update this once we support tp_getattr-style overriding:
    bool hasGenericGetattr() { return true; }

    void freeze();

    BoxedClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size, bool is_user_defined);

    DEFAULT_CLASS(type_cls);
};

class BoxedHeapClass : public BoxedClass {
public:
    PyNumberMethods as_number;
    PyMappingMethods as_mapping;
    PySequenceMethods as_sequence;
    PyBufferProcs as_buffer;

    BoxedString* ht_name;
    PyObject** ht_slots;

    BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size, bool is_user_defined,
                   const std::string& name);

    BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size, bool is_user_defined,
                   BoxedString* name);

    // This constructor is only used for bootstrapping purposes to be called for types that
    // are initialized before str_cls.
    BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int instance_size, bool is_user_defined);
};

static_assert(sizeof(pyston::Box) == sizeof(struct _object), "");
static_assert(offsetof(pyston::Box, cls) == offsetof(struct _object, ob_type), "");

static_assert(offsetof(pyston::BoxedClass, cls) == offsetof(struct _typeobject, ob_type), "");
static_assert(offsetof(pyston::BoxedClass, tp_name) == offsetof(struct _typeobject, tp_name), "");
static_assert(offsetof(pyston::BoxedClass, attrs) == offsetof(struct _typeobject, _hcls), "");
static_assert(offsetof(pyston::BoxedClass, dependent_icgetattrs) == offsetof(struct _typeobject, _dep_getattrs), "");
static_assert(offsetof(pyston::BoxedClass, gc_visit) == offsetof(struct _typeobject, _gcvisit_func), "");
static_assert(sizeof(pyston::BoxedClass) == sizeof(struct _typeobject), "");

static_assert(offsetof(pyston::BoxedHeapClass, as_number) == offsetof(PyHeapTypeObject, as_number), "");
static_assert(offsetof(pyston::BoxedHeapClass, as_mapping) == offsetof(PyHeapTypeObject, as_mapping), "");
static_assert(offsetof(pyston::BoxedHeapClass, as_sequence) == offsetof(PyHeapTypeObject, as_sequence), "");
static_assert(offsetof(pyston::BoxedHeapClass, as_buffer) == offsetof(PyHeapTypeObject, as_buffer), "");
static_assert(sizeof(pyston::BoxedHeapClass) == sizeof(PyHeapTypeObject), "");


class HiddenClass : public GCAllocated<gc::GCKind::HIDDEN_CLASS> {
private:
    HiddenClass() {}
    HiddenClass(const HiddenClass* parent) : attr_offsets(parent->attr_offsets) {}

public:
    static HiddenClass* makeRoot() {
#ifndef NDEBUG
        static bool made = false;
        assert(!made);
        made = true;
#endif
        return new HiddenClass();
    }

    std::unordered_map<std::string, int> attr_offsets;
    std::unordered_map<std::string, HiddenClass*> children;

    HiddenClass* getOrMakeChild(const std::string& attr);

    int getOffset(const std::string& attr) {
        auto it = attr_offsets.find(attr);
        if (it == attr_offsets.end())
            return -1;
        return it->second;
    }
    HiddenClass* delAttrToMakeHC(const std::string& attr);

    void gc_visit(GCVisitor* visitor) {
        for (const auto& p : children) {
            visitor->visit(p.second);
        }
    }
};

class BoxedInt : public Box {
public:
    int64_t n;

    BoxedInt(int64_t n) __attribute__((visibility("default"))) : n(n) {}

    DEFAULT_CLASS(int_cls);
};

class BoxedFloat : public Box {
public:
    double d;

    BoxedFloat(double d) __attribute__((visibility("default"))) : d(d) {}

    DEFAULT_CLASS(float_cls);
};

class BoxedComplex : public Box {
public:
    double real;
    double imag;

    BoxedComplex(double r, double i) __attribute__((visibility("default"))) : real(r), imag(i) {}

    DEFAULT_CLASS(complex_cls);
};

class BoxedBool : public BoxedInt {
public:
    BoxedBool(bool b) __attribute__((visibility("default"))) : BoxedInt(b ? 1 : 0) {}

    DEFAULT_CLASS(bool_cls);
};

class BoxedString : public Box {
public:
    // const std::basic_string<char, std::char_traits<char>, StlCompatAllocator<char> > s;
    std::string s;

    BoxedString(const char* s, size_t n) __attribute__((visibility("default"))) : s(s, n) {}
    BoxedString(const std::string&& s) __attribute__((visibility("default"))) : s(std::move(s)) {}
    BoxedString(const std::string& s) __attribute__((visibility("default"))) : s(s) {}

    DEFAULT_CLASS(str_cls);
};

class BoxedUnicode : public Box {
    // TODO implementation
};

class BoxedInstanceMethod : public Box {
public:
    // obj is NULL for unbound instancemethod
    Box* obj, *func;

    BoxedInstanceMethod(Box* obj, Box* func) __attribute__((visibility("default"))) : obj(obj), func(func) {}

    DEFAULT_CLASS(instancemethod_cls);
};

class GCdArray {
public:
    Box* elts[0];

    void* operator new(size_t size, int capacity) {
        assert(size == sizeof(GCdArray));
        return gc_alloc(capacity * sizeof(Box*) + size, gc::GCKind::UNTRACKED);
    }

    void operator delete(void* p) { gc::gc_free(p); }

    static GCdArray* realloc(GCdArray* array, int capacity) {
        return (GCdArray*)gc::gc_realloc(array, capacity * sizeof(Box*) + sizeof(GCdArray));
    }
};

class BoxedList : public Box {
public:
    int64_t size, capacity;
    GCdArray* elts;

    DS_DEFINE_MUTEX(lock);

    BoxedList() __attribute__((visibility("default"))) : size(0), capacity(0) {}

    void ensure(int space);
    void shrink();
    static const int INITIAL_CAPACITY;

    DEFAULT_CLASS(list_cls);
};

class BoxedTuple : public Box {
public:
    typedef std::vector<Box*, StlCompatAllocator<Box*>> GCVector;
    GCVector elts;

    BoxedTuple(GCVector& elts) __attribute__((visibility("default"))) : elts(elts) {}
    BoxedTuple(GCVector&& elts) __attribute__((visibility("default"))) : elts(std::move(elts)) {}

    DEFAULT_CLASS(tuple_cls);
};
extern "C" BoxedTuple* EmptyTuple;

struct PyHasher {
    size_t operator()(Box*) const;
};

struct PyEq {
    bool operator()(Box*, Box*) const;
};

struct PyLt {
    bool operator()(Box*, Box*) const;
};

class BoxedDict : public Box {
public:
    typedef std::unordered_map<Box*, Box*, PyHasher, PyEq, StlCompatAllocator<std::pair<Box*, Box*>>> DictMap;

    DictMap d;

    BoxedDict() __attribute__((visibility("default"))) {}

    DEFAULT_CLASS(dict_cls);

    Box* getOrNull(Box* k) {
        const auto& p = d.find(k);
        if (p != d.end())
            return p->second;
        return NULL;
    }
};
static_assert(sizeof(BoxedDict) == sizeof(PyDictObject), "");

class BoxedFunctionBase : public Box {
public:
    HCAttrs attrs;
    CLFunction* f;
    BoxedClosure* closure;

    bool isGenerator;
    int ndefaults;
    GCdArray* defaults;

    // Accessed via member descriptor
    Box* modname; // __module__

    BoxedFunctionBase(CLFunction* f);
    BoxedFunctionBase(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                      bool isGenerator = false);
};

class BoxedFunction : public BoxedFunctionBase {
public:
    BoxedFunction(CLFunction* f) : BoxedFunctionBase(f) {}
    BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                  bool isGenerator = false)
        : BoxedFunctionBase(f, defaults, closure, isGenerator) {}

    DEFAULT_CLASS(function_cls);
};

class BoxedBuiltinFunctionOrMethod : public BoxedFunctionBase {
public:
    BoxedBuiltinFunctionOrMethod(CLFunction* f) : BoxedFunctionBase(f) {}
    BoxedBuiltinFunctionOrMethod(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                                 bool isGenerator = false)
        : BoxedFunctionBase(f, defaults, closure, isGenerator) {}

    DEFAULT_CLASS(builtin_function_or_method_cls);
};

class BoxedModule : public Box {
public:
    HCAttrs attrs;
    std::string fn; // for traceback purposes; not the same as __file__
    FutureFlags future_flags;

    BoxedModule(const std::string& name, const std::string& fn);
    std::string name();

    DEFAULT_CLASS(module_cls);
};

class BoxedSlice : public Box {
public:
    Box* start, *stop, *step;
    BoxedSlice(Box* lower, Box* upper, Box* step) : start(lower), stop(upper), step(step) {}

    DEFAULT_CLASS(slice_cls);
};

class BoxedMemberDescriptor : public Box {
public:
    enum MemberType {
        BOOL = T_BOOL,
        BYTE = T_BYTE,
        INT = T_INT,
        OBJECT = T_OBJECT,
        OBJECT_EX = T_OBJECT_EX,
        FLOAT = T_FLOAT,
        SHORT = T_SHORT,
        LONG = T_LONG,
        DOUBLE = T_DOUBLE,
        STRING = T_STRING,
        STRING_INPLACE = T_STRING_INPLACE,
        CHAR = T_CHAR,
        UBYTE = T_UBYTE,
        USHORT = T_USHORT,
        UINT = T_UINT,
        ULONG = T_ULONG,
        LONGLONG = T_LONGLONG,
        ULONGLONG = T_ULONGLONG,
        PYSSIZET = T_PYSSIZET
    } type;

    int offset;

    BoxedMemberDescriptor(MemberType type, int offset) : type(type), offset(offset) {}
    BoxedMemberDescriptor(PyMemberDef* member) : type((MemberType)member->type), offset(member->offset) {}

    DEFAULT_CLASS(member_cls);
};

class BoxedGetsetDescriptor : public Box {
public:
    Box* (*get)(Box*, void*);
    int (*set)(Box*, Box*, void*);
    void* closure;

    BoxedGetsetDescriptor(Box* (*get)(Box*, void*), int (*set)(Box*, Box*, void*), void* closure)
        : get(get), set(set), closure(closure) {}

    DEFAULT_CLASS(getset_cls);
};

class BoxedProperty : public Box {
public:
    Box* prop_get;
    Box* prop_set;
    Box* prop_del;
    Box* prop_doc;

    BoxedProperty(Box* get, Box* set, Box* del, Box* doc)
        : prop_get(get), prop_set(set), prop_del(del), prop_doc(doc) {}

    DEFAULT_CLASS(property_cls);
};

class BoxedStaticmethod : public Box {
public:
    Box* sm_callable;

    BoxedStaticmethod(Box* callable) : sm_callable(callable){};

    DEFAULT_CLASS(staticmethod_cls);
};

class BoxedClassmethod : public Box {
public:
    Box* cm_callable;

    BoxedClassmethod(Box* callable) : cm_callable(callable){};

    DEFAULT_CLASS(classmethod_cls);
};

// TODO is there any particular reason to make this a Box, ie a python-level object?
class BoxedClosure : public Box {
public:
    HCAttrs attrs;
    BoxedClosure* parent;

    BoxedClosure(BoxedClosure* parent) : parent(parent) {}

    DEFAULT_CLASS(closure_cls);
};

class BoxedGenerator : public Box {
public:
    HCAttrs attrs;
    BoxedFunctionBase* function;
    Box* arg1, *arg2, *arg3;
    GCdArray* args;

    bool entryExited;
    bool running;
    Box* returnValue;
    ExcInfo exception;

    ucontext_t context, returnContext;
    void* stack_begin;

    BoxedGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args);

    DEFAULT_CLASS(generator_cls);
};

extern "C" void boxGCHandler(GCVisitor* v, Box* b);

Box* exceptionNew1(BoxedClass* cls);
Box* exceptionNew2(BoxedClass* cls, Box* message);
Box* exceptionNew(BoxedClass* cls, BoxedTuple* args);

extern "C" BoxedClass* Exception, *AssertionError, *AttributeError, *TypeError, *NameError, *KeyError, *IndexError,
    *IOError, *OSError, *ZeroDivisionError, *ValueError, *UnboundLocalError, *RuntimeError, *ImportError,
    *StopIteration, *GeneratorExit, *SyntaxError;

Box* makeAttrWrapper(Box* b);

// Our default for tp_alloc:
PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept;
}
#endif
