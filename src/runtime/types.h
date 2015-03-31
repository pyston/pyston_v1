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

#include <llvm/ADT/StringMap.h>
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
void setupDescr();
void teardownDescr();
void setupCode();

void setupSys();
void setupBuiltins();
void setupPyston();
void setupThread();
void setupImport();
void setupSysEnd();

BoxedDict* getSysModulesDict();
BoxedList* getSysPath();
extern "C" Box* getSysStdout();

extern "C" {
extern BoxedClass* object_cls, *type_cls, *bool_cls, *int_cls, *long_cls, *float_cls, *str_cls, *function_cls,
    *none_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls,
    *enumerate_cls, *xrange_cls, *member_cls, *method_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls,
    *property_cls, *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *pyston_getset_cls, *capi_getset_cls,
    *builtin_function_or_method_cls;
}
#define unicode_cls (&PyUnicode_Type)
#define memoryview_cls (&PyMemoryView_Type)

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
extern "C" Box* decodeUTF8StringPtr(const std::string* s);

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

    // A "simple" destructor -- one that is allowed to be called at any point after the object is dead.
    // In particular, this means that it can't touch any Python objects or other gc-managed memory,
    // since it will be in an undefined state.
    // (Context: in Python destructors are supposed to be called in topological order, due to reference counting.
    // We don't support that yet, but still want some simple ability to run code when an object gets freed.)
    void (*simple_destructor)(Box*);

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

    typedef bool (*pyston_inquiry)(Box*);

    // tpp_descr_get is currently just a cache only for the use of tp_descr_get, and shouldn't
    // be called or examined by clients:
    descrgetfunc tpp_descr_get;

    pyston_inquiry tpp_hasnext;

    bool hasGenericGetattr() { return tp_getattr == NULL; }

    void freeze();

protected:
    // These functions are not meant for external callers and will mostly just be called
    // by BoxedHeapClass::create(), but setupRuntime() also needs to do some manual class
    // creation due to bootstrapping issues.
    void finishInitialization();

    BoxedClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int weaklist_offset, int instance_size,
               bool is_user_defined);

    friend void setupRuntime();
    friend void setupSysEnd();
};

class BoxedHeapClass : public BoxedClass {
public:
    PyNumberMethods as_number;
    PyMappingMethods as_mapping;
    PySequenceMethods as_sequence;
    PyBufferProcs as_buffer;

    BoxedString* ht_name;
    PyObject** ht_slots;

    // These functions are the preferred way to construct new types:
    static BoxedHeapClass* create(BoxedClass* metatype, BoxedClass* base, gcvisit_func gc_visit, int attrs_offset,
                                  int weaklist_offset, int instance_size, bool is_user_defined, BoxedString* name,
                                  BoxedTuple* bases);
    static BoxedHeapClass* create(BoxedClass* metatype, BoxedClass* base, gcvisit_func gc_visit, int attrs_offset,
                                  int weaklist_offset, int instance_size, bool is_user_defined,
                                  const std::string& name);

private:
    // These functions are not meant for external callers and will mostly just be called
    // by BoxedHeapClass::create(), but setupRuntime() also needs to do some manual class
    // creation due to bootstrapping issues.
    BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int weaklist_offset, int instance_size,
                   bool is_user_defined, BoxedString* name);

    friend void setupRuntime();
    friend void setupSys();

    DEFAULT_CLASS(type_cls);
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
public:
    // We have a couple different storage strategies for attributes, which
    // are distinguished by having a different hidden class type.
    enum HCType {
        NORMAL,      // attributes stored in attributes array, name->offset map stored in hidden class
        DICT_BACKED, // first attribute in array is a dict-like object which stores the attributes
    } const type;

    static HiddenClass* dict_backed;

private:
    HiddenClass(HCType type) : type(type) {}
    HiddenClass(HiddenClass* parent) : type(NORMAL), attr_offsets() {
        assert(parent->type == NORMAL);
        for (auto& p : parent->attr_offsets) {
            this->attr_offsets.insert(&p);
        }
    }

    // Only makes sense for NORMAL hidden classes.  Clients should access through getAttrOffsets():
    llvm::StringMap<int> attr_offsets;
    llvm::StringMap<HiddenClass*> children;

public:
    static HiddenClass* makeRoot() {
#ifndef NDEBUG
        static bool made = false;
        assert(!made);
        made = true;
#endif
        return new HiddenClass(NORMAL);
    }
    static HiddenClass* makeDictBacked() {
#ifndef NDEBUG
        static bool made = false;
        assert(!made);
        made = true;
#endif
        return new HiddenClass(DICT_BACKED);
    }

    void gc_visit(GCVisitor* visitor) {
        // Visit children even for the dict-backed case, since children will just be empty
        for (const auto& p : children) {
            visitor->visit(p.second);
        }
    }


    // Only makes sense for NORMAL hidden classes:
    const llvm::StringMap<int>& getAttrOffsets() {
        assert(type == NORMAL);
        return attr_offsets;
    }

    // Only makes sense for NORMAL hidden classes:
    HiddenClass* getOrMakeChild(const std::string& attr);

    // Only makes sense for NORMAL hidden classes:
    int getOffset(const std::string& attr) {
        assert(type == NORMAL);
        auto it = attr_offsets.find(attr);
        if (it == attr_offsets.end())
            return -1;
        return it->second;
    }

    // Only makes sense for NORMAL hidden classes:
    HiddenClass* delAttrToMakeHC(const std::string& attr);
};

class BoxedInt : public Box {
public:
    int64_t n;

    BoxedInt(int64_t n) __attribute__((visibility("default"))) : n(n) {}

    DEFAULT_CLASS_SIMPLE(int_cls);
};

class BoxedFloat : public Box {
public:
    double d;

    BoxedFloat(double d) __attribute__((visibility("default"))) : d(d) {}

    DEFAULT_CLASS_SIMPLE(float_cls);
};

class BoxedComplex : public Box {
public:
    double real;
    double imag;

    BoxedComplex(double r, double i) __attribute__((visibility("default"))) : real(r), imag(i) {}

    DEFAULT_CLASS_SIMPLE(complex_cls);
};

class BoxedBool : public BoxedInt {
public:
    BoxedBool(bool b) __attribute__((visibility("default"))) : BoxedInt(b ? 1 : 0) {}

    DEFAULT_CLASS_SIMPLE(bool_cls);
};

class BoxedString : public Box {
public:
    // const std::basic_string<char, std::char_traits<char>, StlCompatAllocator<char> > s;
    std::string s;

    BoxedString(const char* s, size_t n) __attribute__((visibility("default")));
    BoxedString(std::string&& s) __attribute__((visibility("default")));
    BoxedString(const std::string& s) __attribute__((visibility("default")));

    DEFAULT_CLASS_SIMPLE(str_cls);
};

class BoxedUnicode : public Box {
    // TODO implementation
};

class BoxedInstanceMethod : public Box {
public:
    Box** in_weakreflist;

    // obj is NULL for unbound instancemethod
    Box* obj, *func;

    BoxedInstanceMethod(Box* obj, Box* func) __attribute__((visibility("default")))
    : in_weakreflist(NULL), obj(obj), func(func) {}

    DEFAULT_CLASS_SIMPLE(instancemethod_cls);
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

    DEFAULT_CLASS_SIMPLE(list_cls);
};

class BoxedTuple : public Box {
public:
    typedef std::vector<Box*, StlCompatAllocator<Box*>> GCVector;
    GCVector elts;

    BoxedTuple(GCVector& elts) __attribute__((visibility("default"))) : elts(elts) {}
    BoxedTuple(GCVector&& elts) __attribute__((visibility("default"))) : elts(std::move(elts)) {}

    DEFAULT_CLASS_SIMPLE(tuple_cls);
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

    DEFAULT_CLASS_SIMPLE(dict_cls);

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
    Box** in_weakreflist;

    CLFunction* f;
    BoxedClosure* closure;

    bool isGenerator;
    int ndefaults;
    GCdArray* defaults;

    ICInvalidator dependent_ics;

    // Accessed via member descriptor
    Box* modname;      // __module__
    BoxedString* name; // __name__ (should be here or in one of the derived classes?)
    Box* doc;          // __doc__

    BoxedFunctionBase(CLFunction* f);
    BoxedFunctionBase(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                      bool isGenerator = false);
};

class BoxedFunction : public BoxedFunctionBase {
public:
    HCAttrs attrs;

    BoxedFunction(CLFunction* f);
    BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                  bool isGenerator = false);

    DEFAULT_CLASS(function_cls);
};

class BoxedBuiltinFunctionOrMethod : public BoxedFunctionBase {
public:
    BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name, const char* doc = NULL);
    BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name, std::initializer_list<Box*> defaults,
                                 BoxedClosure* closure = NULL, bool isGenerator = false, const char* doc = NULL);

    DEFAULT_CLASS(builtin_function_or_method_cls);
};

class BoxedModule : public Box {
public:
    HCAttrs attrs;

    // for traceback purposes; not the same as __file__.  This corresponds to co_filename
    std::string fn;
    FutureFlags future_flags;

    BoxedModule(const std::string& name, const std::string& fn, const char* doc = NULL);
    std::string name();

    Box* getStringConstant(const std::string& ast_str);

    llvm::StringMap<int> str_const_index;
    std::vector<Box*> str_constants;

    DEFAULT_CLASS(module_cls);
};

class BoxedSlice : public Box {
public:
    Box* start, *stop, *step;
    BoxedSlice(Box* lower, Box* upper, Box* step) : start(lower), stop(upper), step(step) {}

    DEFAULT_CLASS_SIMPLE(slice_cls);
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
    bool readonly;

    BoxedMemberDescriptor(MemberType type, int offset, bool readonly = true)
        : type(type), offset(offset), readonly(readonly) {}
    BoxedMemberDescriptor(PyMemberDef* member)
        : type((MemberType)member->type), offset(member->offset), readonly(member->flags & READONLY) {}

    DEFAULT_CLASS_SIMPLE(member_cls);
};

class BoxedGetsetDescriptor : public Box {
public:
    Box* (*get)(Box*, void*);
    void (*set)(Box*, Box*, void*);
    void* closure;

    BoxedGetsetDescriptor(Box* (*get)(Box*, void*), void (*set)(Box*, Box*, void*), void* closure)
        : get(get), set(set), closure(closure) {}

    // No DEFAULT_CLASS annotation here -- force callers to explicitly specifiy pyston_getset_cls or capi_getset_cls
};

class BoxedProperty : public Box {
public:
    Box* prop_get;
    Box* prop_set;
    Box* prop_del;
    Box* prop_doc;

    BoxedProperty(Box* get, Box* set, Box* del, Box* doc)
        : prop_get(get), prop_set(set), prop_del(del), prop_doc(doc) {}

    DEFAULT_CLASS_SIMPLE(property_cls);
};

class BoxedStaticmethod : public Box {
public:
    Box* sm_callable;

    BoxedStaticmethod(Box* callable) : sm_callable(callable){};

    DEFAULT_CLASS_SIMPLE(staticmethod_cls);
};

class BoxedClassmethod : public Box {
public:
    Box* cm_callable;

    BoxedClassmethod(Box* callable) : cm_callable(callable){};

    DEFAULT_CLASS_SIMPLE(classmethod_cls);
};

// TODO is there any particular reason to make this a Box, i.e. a python-level object?
class BoxedClosure : public Box {
public:
    BoxedClosure* parent;
    size_t nelts;
    Box* elts[0];

    BoxedClosure(BoxedClosure* parent) : parent(parent) {}

    void* operator new(size_t size, size_t nelts) __attribute__((visibility("default"))) {
        /*
        BoxedClosure* rtn
            = static_cast<BoxedClosure*>(gc_alloc(_PyObject_VAR_SIZE(closure_cls, nelts), gc::GCKind::PYTHON));
            */
        BoxedClosure* rtn
            = static_cast<BoxedClosure*>(gc_alloc(sizeof(BoxedClosure) + nelts * sizeof(Box*), gc::GCKind::PYTHON));
        rtn->nelts = nelts;
        rtn->cls = closure_cls;
        memset((void*)rtn->elts, 0, sizeof(Box*) * nelts);
        return rtn;
    }
};

class BoxedGenerator : public Box {
public:
    Box** weakreflist;

    BoxedFunctionBase* function;
    Box* arg1, *arg2, *arg3;
    GCdArray* args;

    bool entryExited;
    bool running;
    Box* returnValue;
    ExcInfo exception;

    struct Context* context, *returnContext;
    void* stack_begin;

    BoxedGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args);

    DEFAULT_CLASS(generator_cls);
};

extern "C" void boxGCHandler(GCVisitor* v, Box* b);

Box* objectNewNoArgs(BoxedClass* cls);
Box* objectSetattr(Box* obj, Box* attr, Box* value);

Box* makeAttrWrapper(Box* b);

#define SystemError ((BoxedClass*)PyExc_SystemError)
#define StopIteration ((BoxedClass*)PyExc_StopIteration)
#define NameError ((BoxedClass*)PyExc_NameError)
#define UnboundLocalError ((BoxedClass*)PyExc_UnboundLocalError)
#define BaseException ((BoxedClass*)PyExc_BaseException)
#define TypeError ((BoxedClass*)PyExc_TypeError)
#define AssertionError ((BoxedClass*)PyExc_AssertionError)
#define ValueError ((BoxedClass*)PyExc_ValueError)
#define SystemExit ((BoxedClass*)PyExc_SystemExit)
#define SyntaxError ((BoxedClass*)PyExc_SyntaxError)
#define Exception ((BoxedClass*)PyExc_Exception)
#define AttributeError ((BoxedClass*)PyExc_AttributeError)
#define RuntimeError ((BoxedClass*)PyExc_RuntimeError)
#define ZeroDivisionError ((BoxedClass*)PyExc_ZeroDivisionError)
#define ImportError ((BoxedClass*)PyExc_ImportError)
#define IndexError ((BoxedClass*)PyExc_IndexError)
#define GeneratorExit ((BoxedClass*)PyExc_GeneratorExit)
#define IOError ((BoxedClass*)PyExc_IOError)
#define KeyError ((BoxedClass*)PyExc_KeyError)
#define OverflowError ((BoxedClass*)PyExc_OverflowError)

// Our default for tp_alloc:
extern "C" PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept;

// A descriptor that you can add to your class to provide instances with a __dict__ accessor.
// Classes created in Python get this automatically, but builtin types (including extension types)
// are supposed to add one themselves.  type_cls and function_cls do this, for example.
extern Box* dict_descr;

Box* codeForFunction(BoxedFunction*);
}
#endif
