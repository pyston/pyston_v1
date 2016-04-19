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

#ifndef PYSTON_RUNTIME_TYPES_H
#define PYSTON_RUNTIME_TYPES_H

#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/Twine.h>
#include <ucontext.h>

#include "Python.h"
#include "structmember.h"

#include "codegen/irgen/future.h"
#include "core/contiguous_map.h"
#include "core/from_llvm/DenseMap.h"
#include "core/threading.h"
#include "core/types.h"

namespace pyston {

extern bool IN_SHUTDOWN;

class BoxedString;
class BoxedList;
class BoxedDict;
class BoxedTuple;
class BoxedFile;
class BoxedClosure;
class BoxedGenerator;
class BoxedLong;

void setupInt();
void setupFloat();
void setupComplex();
void setupStr();
void setupList();
void list_dtor(BoxedList* l);
void setupBool();
void dict_dtor(BoxedDict* d);
void setupDict();
void tuple_dtor(BoxedTuple* d);
void setupTuple();
void file_dtor(BoxedFile* d);
void setupFile();
void setupCAPI();
void setupGenerator();
void setupDescr();
void setupCode();
void setupFrame();

void setupSys();
void setupBuiltins();
void setupPyston();
void setupThread();
void setupImport();
void setupAST();
void setupSysEnd();

BORROWED(BoxedDict*) getSysModulesDict();
BORROWED(BoxedList*) getSysPath();

extern "C" BoxedTuple* EmptyTuple;
extern "C" BoxedString* EmptyString;

extern "C" {
extern BoxedClass* object_cls, *type_cls, *bool_cls, *int_cls, *long_cls, *float_cls, *str_cls, *function_cls,
    *none_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *enumerate_cls,
    *xrange_cls, *member_descriptor_cls, *method_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls,
    *property_cls, *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *pyston_getset_cls, *capi_getset_cls,
    *builtin_function_or_method_cls, *set_cls, *frozenset_cls, *code_cls, *frame_cls, *capifunc_cls, *wrapperdescr_cls,
    *wrapperobject_cls;
}
#define unicode_cls (&PyUnicode_Type)
#define memoryview_cls (&PyMemoryView_Type)

#define unicode_cls (&PyUnicode_Type)
#define memoryview_cls (&PyMemoryView_Type)

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

// Contains a list classes that have BaseException as a parent. This list is NOT guaranteed to be
// comprehensive - it will not contain user-defined exception types. This is mainly for optimization
// purposes, where it's useful to speed up the garbage collection of some exceptions.
extern std::vector<BoxedClass*> exception_types;

extern "C" {
extern Box* None, *NotImplemented, *True, *False, *Ellipsis;
}
extern "C" {
extern Box* repr_obj, *len_obj, *hash_obj, *range_obj, *abs_obj, *min_obj, *max_obj, *open_obj, *id_obj, *chr_obj,
    *ord_obj, *trap_obj;
} // these are only needed for functionRepr, which is hacky
extern "C" {
extern BoxedModule* sys_module, *builtins_module, *math_module, *time_module, *thread_module;
}

extern "C" inline Box* boxBool(bool b) __attribute__((visibility("default")));
extern "C" inline Box* boxBool(bool b) {
    if (b)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}
extern "C" inline Box* boxBoolNegated(bool b) __attribute__((visibility("default")));
extern "C" inline Box* boxBoolNegated(bool b) {
    return boxBool(!b);
}
extern "C" Box* boxInt(i64) __attribute__((visibility("default")));
extern "C" i64 unboxInt(Box*);
extern "C" Box* boxFloat(double d);
extern "C" Box* boxInstanceMethod(Box* obj, Box* func, Box* type);
extern "C" Box* boxUnboundInstanceMethod(Box* func, Box* type);

// Both llvm::StringRef and llvm::Twine offer implicit conversions from const char*, so
// put the twine version under a different name.
BoxedString* boxString(llvm::StringRef s);
BoxedString* boxStringTwine(const llvm::Twine& s);

extern "C" Box* decodeUTF8StringPtr(llvm::StringRef s);

extern "C" inline void listAppendInternal(Box* self, Box* v) __attribute__((visibility("default")));
extern "C" inline void listAppendInternalStolen(Box* self, Box* v) __attribute__((visibility("default")));
extern "C" void listAppendArrayInternal(Box* self, Box** v, int nelts);
extern "C" Box* createFunctionFromMetadata(FunctionMetadata* f, BoxedClosure* closure, Box* globals,
                                           std::initializer_list<Box*> defaults) noexcept;
extern "C" FunctionMetadata* getFunctionMetadata(Box* b);
extern "C" Box* createUserClass(BoxedString* name, Box* base, Box* attr_dict);
extern "C" double unboxFloat(Box* b);
extern "C" Box* createDict();
extern "C" Box* createList();
extern "C" Box* createSlice(Box* start, Box* stop, Box* step);
extern "C" Box* createTuple(int64_t nelts, Box** elts);
extern "C" void makePendingCalls();

Box* objectStr(Box*);
Box* objectRepr(Box*);

void checkAndThrowCAPIException();
void throwCAPIException() __attribute__((noreturn));
void ensureCAPIExceptionSet();
struct ExcInfo;
void setCAPIException(STOLEN(const ExcInfo&) e);

// Finalizer-related
void dealloc_null(Box* box);
void file_dealloc(Box*) noexcept;

// In Pyston, this is the same type as CPython's PyTypeObject (they are interchangeable, but we
// use BoxedClass in Pyston wherever possible as a convention).
class BoxedClass : public BoxVar {
public:
    PyTypeObject_BODY;

    HCAttrs attrs;

    // TODO: these don't actually get deallocated right now
    std::unique_ptr<CallattrCapiIC> next_ic;
    std::unique_ptr<CallattrIC> hasnext_ic, repr_ic, iter_ic;
    std::unique_ptr<NonzeroIC> nonzero_ic;
    Box* callHasnextIC(Box* obj, bool null_on_nonexistent);
    Box* call_nextIC(Box* obj) noexcept;
    Box* callReprIC(Box* obj);
    Box* callIterIC(Box* obj);
    bool callNonzeroIC(Box* obj);

    // Offset of the HCAttrs object or 0 if there are no hcattrs.
    // Negative offset is from the end of the class (useful for variable-size objects with the attrs at the end)
    // Analogous to tp_dictoffset
    // A class should have at most of one attrs_offset and tp_dictoffset be nonzero.
    // (But having nonzero attrs_offset here would map to having nonzero tp_dictoffset in CPython)
    int attrs_offset;

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

    // Just for debugging: whether instances of this class should always be considered nonzero.
    // This is the default for anything that doesn't define a __nonzero__ or __len__ method, but
    // for builtin types we have the extra check that we opted into this behavior rather than
    // just forgot to add nonzero/len.
    bool instances_are_nonzero;

    bool has___class__; // Has a custom __class__ attribute (ie different from object's __class__ descriptor)
    bool has_instancecheck;
    bool has_subclasscheck;
    bool has_getattribute;

    typedef llvm_compat_bool (*pyston_inquiry)(Box*);

    // tpp_descr_get is currently just a cache only for the use of tp_descr_get, and shouldn't
    // be called or examined by clients:
    descrgetfunc tpp_descr_get;

    pyston_inquiry tpp_hasnext;

    ExceptionSwitchableFunction<Box*, Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                const std::vector<BoxedString*>*> tpp_call;

    bool hasGenericGetattr() {
        if (tp_getattr || tp_getattro != object_cls->tp_getattro)
            return false;

        // instancemethod_cls should have a custom tp_getattr but is currently implemented
        // as a hack within getattrInternalGeneric
        if (this == instancemethod_cls)
            return false;

        return true;
    }

    void freeze();

    // These should only be used for builtin types:
    static BoxedClass* create(BoxedClass* metatype, BoxedClass* base, int attrs_offset, int weaklist_offset,
                              int instance_size, bool is_user_defined, const char* name, bool is_subclassable = true,
                              destructor dealloc = NULL, freefunc free = NULL, bool is_gc = true,
                              traverseproc traverse = NULL, inquiry clear = NULL);

    BoxedClass(BoxedClass* base, int attrs_offset, int weaklist_offset, int instance_size, bool is_user_defined,
               const char* name, bool is_subclassable, destructor dealloc, freefunc free, bool is_gc = true,
               traverseproc traverse = NULL, inquiry clear = NULL);


    DEFAULT_CLASS_VAR(type_cls, sizeof(PyMemberDef));

    static void dealloc(Box* self) noexcept;

protected:
    // These functions are not meant for external callers and will mostly just be called
    // by BoxedHeapClass::create(), but setupRuntime() also needs to do some manual class
    // creation due to bootstrapping issues.
    void finishInitialization();

    friend void setupRuntime();
    friend void setupSysEnd();
    friend void setupThread();
};

// Corresponds to PyHeapTypeObject.  Very similar to BoxedClass, but allocates some extra space for
// structures that otherwise might get allocated statically.  For instance, tp_as_number for builtin
// types will usually point to a `static PyNumberMethods` object, but for a heap-allocated class it
// will point to `this->as_number`.
class BoxedHeapClass : public BoxedClass {
public:
    PyNumberMethods as_number;
    PyMappingMethods as_mapping;
    PySequenceMethods as_sequence;
    PyBufferProcs as_buffer;

    BoxedString* ht_name;
    PyObject* ht_slots;

    size_t nslots() { return this->ob_size; }

    // These functions are the preferred way to construct new types:
    static BoxedHeapClass* create(BoxedClass* metatype, BoxedClass* base, int attrs_offset, int weaklist_offset,
                                  int instance_size, bool is_user_defined, BoxedString* name, BoxedTuple* bases,
                                  size_t nslots);

private:
    // These functions are not meant for external callers and will mostly just be called
    // by BoxedHeapClass::create(), but setupRuntime() also needs to do some manual class
    // creation due to bootstrapping issues.
    BoxedHeapClass(BoxedClass* base, int attrs_offset, int weaklist_offset, int instance_size, bool is_user_defined,
                   BoxedString* name);

    friend void setupRuntime();
    friend void setupSys();
    friend void setupThread();
};

// Assert that our data structures have the same layout as the C API ones with which they need to be interchangeable.
static_assert(sizeof(pyston::Box) == sizeof(struct _object), "");
static_assert(offsetof(pyston::Box, cls) == offsetof(struct _object, ob_type), "");

static_assert(offsetof(pyston::BoxedClass, cls) == offsetof(struct _typeobject, ob_type), "");
static_assert(offsetof(pyston::BoxedClass, tp_name) == offsetof(struct _typeobject, tp_name), "");
static_assert(offsetof(pyston::BoxedClass, attrs) == offsetof(struct _typeobject, _hcls), "");
static_assert(sizeof(pyston::BoxedClass) == sizeof(struct _typeobject), "");

static_assert(offsetof(pyston::BoxedHeapClass, as_number) == offsetof(PyHeapTypeObject, as_number), "");
static_assert(offsetof(pyston::BoxedHeapClass, as_mapping) == offsetof(PyHeapTypeObject, as_mapping), "");
static_assert(offsetof(pyston::BoxedHeapClass, as_sequence) == offsetof(PyHeapTypeObject, as_sequence), "");
static_assert(offsetof(pyston::BoxedHeapClass, as_buffer) == offsetof(PyHeapTypeObject, as_buffer), "");
static_assert(sizeof(pyston::BoxedHeapClass) == sizeof(PyHeapTypeObject), "");

template <typename B, bool Nullable = false> struct DecrefHandle {
private:
    B* b;

public:
    DecrefHandle(B* b) : b(b) {}
    ~DecrefHandle() {
        if (Nullable)
            Py_XDECREF(b);
        else
            Py_DECREF(b);
    }
    operator B*() { return b; }
    B* operator->() { return b; }
    explicit operator intptr_t() { return (intptr_t)b; }
    bool operator==(B* rhs) { return b == rhs; }
    bool operator!=(B* rhs) { return b != rhs; }

    // Hacky, but C API macros like to use direct C casts.  At least this is "explicit"
    template <typename B2> explicit operator B2*() { return (B2*)(b); }

    B* get() { return b; }

    void operator=(B* new_b) {
        B* old_b = b;
        b = new_b;
        if (Nullable)
            Py_XDECREF(old_b);
        else
            Py_DECREF(old_b);
    }
};
template <typename B, bool Nullable = false> DecrefHandle<B, Nullable> autoDecref(B* b) {
    return DecrefHandle<B, Nullable>(b);
}
template <typename B> DecrefHandle<B, true> autoXDecref(B* b) {
    return DecrefHandle<B, true>(b);
}
#define AUTO_DECREF(x) DecrefHandle<Box, false> CAT(_autodecref_, __LINE__)((x))
#define AUTO_XDECREF(x) DecrefHandle<Box, true> CAT(_autodecref_, __LINE__)((x))

#define KEEP_ALIVE(x) AUTO_DECREF(incref(x))
#define XKEEP_ALIVE(x) AUTO_XDECREF(xincref(x))

// A micro-optimized function to decref an entire array.
// Seems to be about 10% faster than the straightforward version.
template <bool Nullable = false> inline void decrefArray(Box** array, int size) {
    if (size) {
        Box* next = array[0];
        for (int i = 0; i < size - 1; i++) {
            Box* cur = next;
            next = array[i + 1];
            _mm_prefetch(next, _MM_HINT_T0);
            if (Nullable)
                Py_XDECREF(cur);
            else
                Py_DECREF(cur);
        }
        if (Nullable)
            Py_XDECREF(next);
        else
            Py_DECREF(next);
    }
}

template <bool Nullable = false> class AutoDecrefArray {
private:
    Box** array;
    int size;

public:
    AutoDecrefArray(Box** array, int size) : array(array), size(size) {}
    ~AutoDecrefArray() {
        for (int i = 0; i < size; i++) {
            if (Nullable)
                Py_XDECREF(array[i]);
            else
                Py_DECREF(array[i]);
        }
    }
};
#define AUTO_DECREF_ARRAY(x, size) AutoDecrefArray<false> CAT(_autodecref_, __LINE__)((x), (size))
#define AUTO_XDECREF_ARRAY(x, size) AutoDecrefArray<true> CAT(_autodecref_, __LINE__)((x), (size))

class AutoDecrefArgs {
private:
    int num_args;
    Box* arg1, *arg2, *arg3;
    Box** args;

public:
    AutoDecrefArgs(int num_args, Box* arg1, Box* arg2, Box* arg3, Box** args)
        : num_args(num_args), arg1(arg1), arg2(arg2), arg3(arg3), args(args) {}
    AutoDecrefArgs(ParamReceiveSpec paramspec, Box* arg1, Box* arg2, Box* arg3, Box** args)
        : num_args(paramspec.totalReceived()), arg1(arg1), arg2(arg2), arg3(arg3), args(args) {}

    ~AutoDecrefArgs() {
        // TODO Minor optimization: only the last arg (kwargs) is allowed to be NULL.
        switch (num_args) {
            default:
                for (int i = 0; i < num_args - 3; i++) {
                    Py_XDECREF(args[i]);
                }
            case 3:
                Py_XDECREF(arg3);
            case 2:
                Py_XDECREF(arg2);
            case 1:
                Py_XDECREF(arg1);
            case 0:
                break;
        }
    }
};
// Note: this captures the first three args by value (like AUTO_DECREF) but the array by reference.
// You can also pass a ParamReceiveSpec instead of an int for num_args
#define AUTO_DECREF_ARGS(num_args, arg1, arg2, arg3, args)                                                             \
    AutoDecrefArgs CAT(_autodecref_, __LINE__)((num_args), (arg1), (arg2), (arg3), (args))

template <typename B> B* incref(B* b) {
    Py_INCREF(b);
    return b;
}
template <typename B> B* xincref(B* b) {
    Py_XINCREF(b);
    return b;
}

//#define DISABLE_INT_FREELIST

extern "C" int PyInt_ClearFreeList() noexcept;
class BoxedInt : public Box {
private:
    static PyIntObject* free_list;
    static PyIntObject* fill_free_list() noexcept;

public:
    int64_t n;

    BoxedInt(int64_t n) __attribute__((visibility("default"))) : n(n) {}

    void* operator new(size_t size, BoxedClass* cls) __attribute__((visibility("default"))) {
        return Box::operator new(size, cls);
    }
    // int uses a customized allocator, so we can't use DEFAULT_CLASS_SIMPLE (which inlines the default allocator)
    void* operator new(size_t size) __attribute__((visibility("default"))) {
#ifdef DISABLE_INT_FREELIST
        return Box::operator new(size, int_cls);
#else
        if (unlikely(free_list == NULL)) {
            free_list = fill_free_list();
            assert(free_list);
        }

        PyIntObject* v = free_list;
        free_list = (PyIntObject*)v->ob_type;
        PyObject_INIT((BoxedInt*)v, &PyInt_Type);
        return v;
#endif
    }

    static void tp_dealloc(Box* b) noexcept;

    friend int PyInt_ClearFreeList() noexcept;
};
static_assert(sizeof(BoxedInt) == sizeof(PyIntObject), "");
static_assert(offsetof(BoxedInt, n) == offsetof(PyIntObject, ob_ival), "");

extern "C" int PyFloat_ClearFreeList() noexcept;
class BoxedFloat : public Box {
private:
    static PyFloatObject* free_list;
    static PyFloatObject* fill_free_list() noexcept;

public:
    double d;

    BoxedFloat(double d) __attribute__((visibility("default"))) : d(d) {}

    void* operator new(size_t size, BoxedClass* cls) __attribute__((visibility("default"))) {
        return Box::operator new(size, cls);
    }
    // float uses a customized allocator, so we can't use DEFAULT_CLASS_SIMPLE (which inlines the default allocator)
    void* operator new(size_t size) __attribute__((visibility("default"))) {
#ifdef DISABLE_INT_FREELIST
        return Box::operator new(size, float_cls);
#else
        if (unlikely(free_list == NULL)) {
            free_list = fill_free_list();
            assert(free_list);
        }

        PyFloatObject* v = free_list;
        free_list = (PyFloatObject*)v->ob_type;
        PyObject_INIT((BoxedFloat*)v, &PyFloat_Type);
        return v;
#endif
    }

    static void tp_dealloc(Box* b) noexcept;

    friend int PyFloat_ClearFreeList() noexcept;
};
static_assert(sizeof(BoxedFloat) == sizeof(PyFloatObject), "");
static_assert(offsetof(BoxedFloat, d) == offsetof(PyFloatObject, ob_fval), "");

class BoxedComplex : public Box {
public:
    double real;
    double imag;

    BoxedComplex(double r, double i) __attribute__((visibility("default"))) : real(r), imag(i) {}

    DEFAULT_CLASS_SIMPLE(complex_cls, false);
};

static_assert(sizeof(BoxedComplex) == sizeof(PyComplexObject), "");
static_assert(offsetof(BoxedComplex, real) == offsetof(PyComplexObject, cval.real), "");
static_assert(offsetof(BoxedComplex, imag) == offsetof(PyComplexObject, cval.imag), "");

class BoxedBool : public BoxedInt {
public:
    BoxedBool(bool b) __attribute__((visibility("default"))) : BoxedInt(b ? 1 : 0) {}

    DEFAULT_CLASS_SIMPLE(bool_cls, false);
};

class BoxedString : public BoxVar {
public:
    // llvm::StringRef is basically just a pointer and a length, so with proper compiler
    // optimizations and inlining, creating a new one each time shouldn't have any cost.
    llvm::StringRef s() const { return llvm::StringRef(s_data, ob_size); };

    long hash; // -1 means not yet computed
    int interned_state;
    char s_data[0];

    char* data() { return s_data; }
    const char* c_str() {
        assert(data()[size()] == '\0');
        return data();
    }
    size_t size() { return this->ob_size; }

    // DEFAULT_CLASS_VAR_SIMPLE doesn't work because of the +1 for the null byte
    void* operator new(size_t size, BoxedClass* cls, size_t nitems) __attribute__((visibility("default"))) {
        ALLOC_STATS_VAR(str_cls)

        assert(cls->tp_itemsize == sizeof(char));
        return BoxVar::operator new(size, cls, nitems);
    }
    void* operator new(size_t size, size_t nitems) __attribute__((visibility("default"))) {
        ALLOC_STATS_VAR(str_cls)

        assert(str_cls->tp_alloc == PyType_GenericAlloc);
        assert(str_cls->tp_itemsize == 1);
        assert(str_cls->tp_basicsize == offsetof(BoxedString, s_data) + 1);
        assert(str_cls->is_pyston_class);
        assert(str_cls->attrs_offset == 0);

        void* mem = PyObject_MALLOC(sizeof(BoxedString) + 1 + nitems);
        assert(mem);

        BoxVar* rtn = static_cast<BoxVar*>(mem);
        rtn->cls = str_cls;
        rtn->ob_size = nitems;
        _Py_NewReference(rtn);
        return rtn;
    }

    // these should be private, but str.cpp needs them
    BoxedString(const char* s, size_t n) __attribute__((visibility("default")));
    explicit BoxedString(size_t n, char c) __attribute__((visibility("default")));
    explicit BoxedString(llvm::StringRef s) __attribute__((visibility("default")));
    explicit BoxedString(llvm::StringRef lhs, llvm::StringRef rhs) __attribute__((visibility("default")));

    // creates an uninitialized string of length n; useful for directly constructing into the string and avoiding
    // copies:
    static BoxedString* createUninitializedString(ssize_t n) { return new (n) BoxedString(n); }
    static BoxedString* createUninitializedString(BoxedClass* cls, ssize_t n) { return new (cls, n) BoxedString(n); }

    // Gets a writeable pointer to the contents of a string.
    // Is only meant to be used with something just created from createUninitializedString(), though
    // in theory it might work in more cases.
    char* getWriteableStringContents() { return s_data; }

private:
    void* operator new(size_t size) = delete;

    BoxedString(size_t n); // non-initializing constructor

    friend void setupRuntime();
};
static_assert(sizeof(BoxedString) == sizeof(PyStringObject), "");
static_assert(offsetof(BoxedString, ob_size) == offsetof(PyStringObject, ob_size), "");
static_assert(offsetof(BoxedString, hash) == offsetof(PyStringObject, ob_shash), "");
static_assert(offsetof(BoxedString, interned_state) == offsetof(PyStringObject, ob_sstate), "");
static_assert(offsetof(BoxedString, s_data) == offsetof(PyStringObject, ob_sval), "");

extern "C" size_t strHashUnboxed(BoxedString* self);
extern "C" int64_t hashUnboxed(Box* obj);

class BoxedInstanceMethod : public Box {
public:
    Box** im_weakreflist;

    // obj is NULL for unbound instancemethod
    Box* obj, *func, *im_class;

    BoxedInstanceMethod(Box* obj, Box* func, Box* im_class) __attribute__((visibility("default")))
    : im_weakreflist(NULL), obj(obj), func(func), im_class(im_class) {
        Py_INCREF(func);
        Py_XINCREF(obj);
        Py_XINCREF(im_class);
    }

    DEFAULT_CLASS_SIMPLE(instancemethod_cls, true);

    static void dealloc(Box* self) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
};

class GCdArray {
public:
    Box* elts[0];

    void* operator new(size_t size, int capacity) {
        assert(size == sizeof(GCdArray));
        return PyMem_MALLOC(capacity * sizeof(Box*) + size);
    }

    void operator delete(void* p) { PyMem_FREE(p); }

    static GCdArray* grow(GCdArray* array, int capacity) {
        return (GCdArray*)PyMem_REALLOC(array, capacity * sizeof(Box*) + sizeof(GCdArray));
    }
};

class BoxedList : public Box {
private:
    void grow(int min_free);

public:
    Py_ssize_t size;
    GCdArray* elts;
    Py_ssize_t capacity;

    BoxedList() __attribute__((visibility("default"))) : size(0), elts(NULL), capacity(0) {}

    void ensure(int min_free);
    void shrink();
    static const int INITIAL_CAPACITY;

    DEFAULT_CLASS_SIMPLE(list_cls, true);

    static void dealloc(Box* self) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* self) noexcept;
};
static_assert(sizeof(BoxedList) <= sizeof(PyListObject), "");
static_assert(sizeof(BoxedList) >= sizeof(PyListObject), "");
static_assert(offsetof(BoxedList, size) == offsetof(PyListObject, ob_size), "");
static_assert(offsetof(BoxedList, elts) == offsetof(PyListObject, ob_item), "");
static_assert(offsetof(GCdArray, elts) == 0, "");
static_assert(offsetof(BoxedList, capacity) == offsetof(PyListObject, allocated), "");

class BoxedTuple : public BoxVar {
public:
    static BoxedTuple* create(int64_t size) {
        if (size == 0) {
            Py_INCREF(EmptyTuple);
            return EmptyTuple;
        }
        BoxedTuple* rtn = new (size) BoxedTuple();
        memset(rtn->elts, 0, size * sizeof(Box*)); // TODO not all callers want this (but some do)
        return rtn;
    }
    static BoxedTuple* create(int64_t nelts, Box** elts) {
        if (nelts == 0) {
            Py_INCREF(EmptyTuple);
            return EmptyTuple;
        }
        BoxedTuple* rtn = new (nelts) BoxedTuple();
        for (int i = 0; i < nelts; i++) {
            Py_INCREF(elts[i]);
        }
        memmove(&rtn->elts[0], elts, sizeof(Box*) * nelts);
        return rtn;
    }
    static BoxedTuple* create1(Box* elt0) {
        BoxedTuple* rtn = new (1) BoxedTuple();
        Py_INCREF(elt0);
        rtn->elts[0] = elt0;
        return rtn;
    }
    static BoxedTuple* create2(Box* elt0, Box* elt1) {
        BoxedTuple* rtn = new (2) BoxedTuple();
        Py_INCREF(elt0);
        Py_INCREF(elt1);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        return rtn;
    }
    static BoxedTuple* create3(Box* elt0, Box* elt1, Box* elt2) {
        BoxedTuple* rtn = new (3) BoxedTuple();
        Py_INCREF(elt0);
        Py_INCREF(elt1);
        Py_INCREF(elt2);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        rtn->elts[2] = elt2;
        return rtn;
    }
    static BoxedTuple* create4(Box* elt0, Box* elt1, Box* elt2, Box* elt3) {
        BoxedTuple* rtn = new (4) BoxedTuple();
        Py_INCREF(elt0);
        Py_INCREF(elt1);
        Py_INCREF(elt2);
        Py_INCREF(elt3);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        rtn->elts[2] = elt2;
        rtn->elts[3] = elt3;
        return rtn;
    }
    static BoxedTuple* create5(Box* elt0, Box* elt1, Box* elt2, Box* elt3, Box* elt4) {
        BoxedTuple* rtn = new (5) BoxedTuple();
        Py_INCREF(elt0);
        Py_INCREF(elt1);
        Py_INCREF(elt2);
        Py_INCREF(elt3);
        Py_INCREF(elt4);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        rtn->elts[2] = elt2;
        rtn->elts[3] = elt3;
        rtn->elts[4] = elt4;
        return rtn;
    }
    static BoxedTuple* create6(Box* elt0, Box* elt1, Box* elt2, Box* elt3, Box* elt4, Box* elt5) {
        BoxedTuple* rtn = new (6) BoxedTuple();
        Py_INCREF(elt0);
        Py_INCREF(elt1);
        Py_INCREF(elt2);
        Py_INCREF(elt3);
        Py_INCREF(elt4);
        Py_INCREF(elt5);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        rtn->elts[2] = elt2;
        rtn->elts[3] = elt3;
        rtn->elts[4] = elt4;
        rtn->elts[5] = elt5;
        return rtn;
    }
    static BoxedTuple* create(std::initializer_list<Box*> members) {
        auto rtn = new (members.size()) BoxedTuple(members);

        return rtn;
    }

    static BoxedTuple* create(int64_t size, BoxedClass* cls) {
        BoxedTuple* rtn;
        if (cls == tuple_cls)
            rtn = new (size) BoxedTuple();
        else
            rtn = new (cls, size) BoxedTuple();
        memset(rtn->elts, 0, size * sizeof(Box*)); // TODO not all callers want this (but some do)
        return rtn;
    }
    static BoxedTuple* create(int64_t nelts, Box** elts, BoxedClass* cls) {
        BoxedTuple* rtn;
        if (cls == tuple_cls)
            rtn = new (nelts) BoxedTuple();
        else
            rtn = new (cls, nelts) BoxedTuple();
        for (int i = 0; i < nelts; i++) {
            Py_INCREF(elts[i]);
        }
        memmove(&rtn->elts[0], elts, sizeof(Box*) * nelts);
        return rtn;
    }
    static BoxedTuple* create(std::initializer_list<Box*> members, BoxedClass* cls) {
        if (cls == tuple_cls)
            return new (members.size()) BoxedTuple(members);
        else
            return new (cls, members.size()) BoxedTuple(members);
    }

    static int Resize(BoxedTuple** pt, size_t newsize) noexcept;

    Box* const* begin() const { return &elts[0]; }
    Box* const* end() const { return &elts[ob_size]; }
    Box*& operator[](size_t index) { return elts[index]; }

    size_t size() const { return ob_size; }

    // DEFAULT_CLASS_VAR_SIMPLE doesn't work because of declaring 1 element in 'elts'
    void* operator new(size_t size, BoxedClass* cls, size_t nitems) __attribute__((visibility("default"))) {
        ALLOC_STATS_VAR(tuple_cls)

        assert(cls->tp_itemsize == sizeof(Box*));
        return BoxVar::operator new(size, cls, nitems);
    }

    void* operator new(size_t size, size_t nitems) __attribute__((visibility("default"))) {
        ALLOC_STATS_VAR(tuple_cls)

        assert(tuple_cls->tp_alloc == PyType_GenericAlloc);
        assert(tuple_cls->tp_itemsize == sizeof(Box*));
        assert(tuple_cls->tp_basicsize == offsetof(BoxedTuple, elts));
        assert(tuple_cls->is_pyston_class);
        assert(tuple_cls->attrs_offset == 0);

        BoxVar* rtn = static_cast<BoxVar*>(PyObject_GC_NewVar(BoxedTuple, &PyTuple_Type, nitems));
        assert(rtn);
        _PyObject_GC_TRACK(rtn);

        return rtn;
    }

private:
    BoxedTuple() {}

    BoxedTuple(std::initializer_list<Box*>& members) {
        // by the time we make it here elts[] is big enough to contain members
        Box** p = &elts[0];
        for (auto b : members) {
            Py_INCREF(b);
            *p++ = b;
        }
    }

public:
    // CPython declares ob_item (their version of elts) to have 1 element.  We want to
    // copy that behavior so that the sizes of the objects match, but we want to also
    // have a zero-length array in there since we have some extra compiler warnings turned
    // on:  _elts[1] will throw an error, but elts[1] will not.
    union {
        Box* elts[0];
        Box* _elts[1];
    };
};
static_assert(sizeof(BoxedTuple) == sizeof(PyTupleObject), "");
static_assert(offsetof(BoxedTuple, ob_size) == offsetof(PyTupleObject, ob_size), "");
static_assert(offsetof(BoxedTuple, elts) == offsetof(PyTupleObject, ob_item), "");

extern BoxedString* characters[UCHAR_MAX + 1];

// C++ functor objects that implement Python semantics.
struct PyHasher {
    size_t operator()(Box* b) const {
        if (b->cls == str_cls) {
            auto s = static_cast<BoxedString*>(b);
            if (s->hash != -1)
                return s->hash;
            return strHashUnboxed(s);
        }
        return hashUnboxed(b);
    }
};

struct PyEq {
    bool operator()(Box* lhs, Box* rhs) const {
        int r = PyObject_RichCompareBool(lhs, rhs, Py_EQ);
        if (r == -1)
            throwCAPIException();
        return (bool)r;
    }
};

struct PyLt {
    bool operator()(Box* lhs, Box* rhs) const {
        int r = PyObject_RichCompareBool(lhs, rhs, Py_LT);
        if (r == -1)
            throwCAPIException();
        return (bool)r;
    }
};

// llvm::DenseMap doesn't store the original hash values, choosing to instead
// check for equality more often.  This is probably a good tradeoff when the keys
// are pointers and comparison is cheap, but when the equality function is user-defined
// it can be much faster to avoid Python function invocations by doing some integer
// comparisons.
// This also has a user-visible behavior difference of how many times the hash function
// and equality functions get called.
struct BoxAndHash {
    Box* value;
    size_t hash;

    BoxAndHash(Box* value) : value(value), hash(PyHasher()(value)) {}
    BoxAndHash(Box* value, size_t hash) : value(value), hash(hash) {}

    struct Comparisons {
        static bool isEqual(BoxAndHash lhs, BoxAndHash rhs) {
            if (lhs.value == rhs.value)
                return true;
            if (rhs.value == (Box*)-1 || rhs.value == (Box*)-2)
                return false;
            if (lhs.hash != rhs.hash)
                return false;
            return PyEq()(lhs.value, rhs.value);
        }
        static BoxAndHash getEmptyKey() { return BoxAndHash((Box*)-1, 0); }
        static BoxAndHash getTombstoneKey() { return BoxAndHash((Box*)-2, 0); }
        static unsigned getHashValue(BoxAndHash val) { return val.hash; }
    };
};
// Similar to the incref(Box*) function:
inline BoxAndHash& incref(BoxAndHash& b) {
    Py_INCREF(b.value);
    return b;
}

class BoxedDict : public Box {
public:
    typedef pyston::DenseMap<BoxAndHash, Box*, BoxAndHash::Comparisons, detail::DenseMapPair<BoxAndHash, Box*>,
                             /* MinSize= */ 8> DictMap;

    DictMap d;

    BoxedDict() __attribute__((visibility("default"))) {}

    DEFAULT_CLASS_SIMPLE(dict_cls, true);

    BORROWED(Box*) getOrNull(Box* k) {
        const auto& p = d.find(BoxAndHash(k));
        if (p != d.end())
            return p->second;
        return NULL;
    }

    class iterator {
    private:
        DictMap::iterator it;

    public:
        iterator(DictMap::iterator it) : it(std::move(it)) {}

        bool operator!=(const iterator& rhs) const { return it != rhs.it; }
        bool operator==(const iterator& rhs) const { return it == rhs.it; }
        iterator& operator++() {
            ++it;
            return *this;
        }
        std::pair<Box*, Box*> operator*() const { return std::make_pair(it->first.value, it->second); }
        Box* first() const { return it->first.value; }
    };

    iterator begin() { return iterator(d.begin()); }
    iterator end() { return iterator(d.end()); }

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* self) noexcept;
};
static_assert(sizeof(BoxedDict) == sizeof(PyDictObject), "");

class BoxedFunctionBase : public Box {
public:
    Box** weakreflist;

    FunctionMetadata* md;

    // TODO these should really go in BoxedFunction but it's annoying because they don't get
    // initializd until after BoxedFunctionBase's constructor is run which means they could have
    // garbage values when the GC is run (BoxedFunctionBase's constructor might call the GC).
    // So ick... needs to be fixed.
    BoxedClosure* closure;
    Box* globals;

    BoxedTuple* defaults;
    bool can_change_defaults;

    ICInvalidator dependent_ics;

    // Accessed via member descriptor
    Box* modname;      // __module__
    BoxedString* name; // __name__ (should be here or in one of the derived classes?)
    Box* doc;          // __doc__

    BoxedFunctionBase(FunctionMetadata* md);
    BoxedFunctionBase(FunctionMetadata* md, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                      Box* globals = NULL, bool can_change_defaults = false);

    ParamReceiveSpec getParamspec() {
        return ParamReceiveSpec(md->num_args, defaults ? defaults->size() : 0, md->takes_varargs, md->takes_kwargs);
    }
};

class BoxedFunction : public BoxedFunctionBase {
public:
    HCAttrs attrs;

    BoxedFunction(FunctionMetadata* md);
    BoxedFunction(FunctionMetadata* md, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                  Box* globals = NULL, bool can_change_defaults = false);

    DEFAULT_CLASS(function_cls);
};

class BoxedBuiltinFunctionOrMethod : public BoxedFunctionBase {
public:
    BoxedBuiltinFunctionOrMethod(FunctionMetadata* f, const char* name, const char* doc = NULL);
    BoxedBuiltinFunctionOrMethod(FunctionMetadata* f, const char* name, std::initializer_list<Box*> defaults,
                                 BoxedClosure* closure = NULL, const char* doc = NULL);

    DEFAULT_CLASS(builtin_function_or_method_cls);
};

extern "C" void _PyModule_Clear(PyObject*) noexcept;
class BoxedModule : public Box {
public:
    HCAttrs attrs;

    FutureFlags future_flags;

    BoxedModule() {} // noop constructor to disable zero-initialization of cls
    std::string name();

    BORROWED(BoxedString*) getStringConstant(llvm::StringRef ast_str, bool intern = false);
    BORROWED(Box*) getUnicodeConstant(llvm::StringRef ast_str);
    BORROWED(BoxedInt*) getIntConstant(int64_t n);
    BORROWED(BoxedFloat*) getFloatConstant(double d);
    BORROWED(Box*) getPureImaginaryConstant(double d);
    BORROWED(Box*) getLongConstant(llvm::StringRef s);

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* b) noexcept;

private:
    ContiguousMap<llvm::StringRef, BoxedString*, llvm::StringMap<int>> str_constants;
    ContiguousMap<llvm::StringRef, Box*, llvm::StringMap<int>> unicode_constants;
    // Note: DenseMap doesn't work here since we don't prevent the tombstone/empty
    // keys from reaching it.
    ContiguousMap<int64_t, BoxedInt*, std::unordered_map<int64_t, int>> int_constants;
    // I'm not sure how well it works to use doubles as hashtable keys; thankfully
    // it's not a big deal if we get misses.
    ContiguousMap<int64_t, BoxedFloat*, std::unordered_map<int64_t, int>> float_constants;
    ContiguousMap<int64_t, Box*, std::unordered_map<int64_t, int>> imaginary_constants;
    ContiguousMap<llvm::StringRef, Box*, llvm::StringMap<int>> long_constants;
    // Other objects that this module needs to keep alive; see getStringConstant.
    llvm::SmallVector<Box*, 8> keep_alive;

public:
    DEFAULT_CLASS(module_cls);

    friend void _PyModule_Clear(PyObject*) noexcept;
};

class BoxedSlice : public Box {
public:
    Box* start, *stop, *step;
    BoxedSlice(Box* lower, Box* upper, Box* step) : start(lower), stop(upper), step(step) {
        Py_INCREF(lower);
        Py_INCREF(upper);
        Py_INCREF(step);
    }

    static void dealloc(Box* b) noexcept;

    DEFAULT_CLASS_SIMPLE(slice_cls, false);
};
static_assert(sizeof(BoxedSlice) == sizeof(PySliceObject), "");
static_assert(offsetof(BoxedSlice, start) == offsetof(PySliceObject, start), "");
static_assert(offsetof(BoxedSlice, stop) == offsetof(PySliceObject, stop), "");
static_assert(offsetof(BoxedSlice, step) == offsetof(PySliceObject, step), "");

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

    DEFAULT_CLASS_SIMPLE(member_descriptor_cls, false);
};

class BoxedGetsetDescriptor : public Box {
public:
    Box* (*get)(Box*, void*);
    union {
        void* set;
        void (*set_pyston)(Box*, Box*, void*);
        int (*set_capi)(Box*, Box*, void*);
    };
    void* closure;
    BoxedString* name;

    BoxedGetsetDescriptor(BoxedString* name, Box* (*get)(Box*, void*), void (*set)(Box*, Box*, void*), void* closure)
        : get(get), set_pyston(set), closure(closure), name(name) {
        assert(this->cls == pyston_getset_cls);
        Py_INCREF(name);
    }

    BoxedGetsetDescriptor(BoxedString* name, Box* (*get)(Box*, void*), int (*set)(Box*, Box*, void*), void* closure)
        : get(get), set_capi(set), closure(closure), name(name) {
        assert(this->cls == capi_getset_cls);
        Py_INCREF(name);
    }

    static void dealloc(Box* b) noexcept;

    // No DEFAULT_CLASS annotation here -- force callers to explicitly specifiy pyston_getset_cls or capi_getset_cls
};

class BoxedProperty : public Box {
public:
    Box* prop_get;
    Box* prop_set;
    Box* prop_del;
    Box* prop_doc;
    bool getter_doc;

    BoxedProperty(Box* get, Box* set, Box* del, Box* doc) : prop_get(get), prop_set(set), prop_del(del), prop_doc(doc) {
        Py_XINCREF(get);
        Py_XINCREF(set);
        Py_XINCREF(del);
        Py_XINCREF(doc);
    }

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;

    DEFAULT_CLASS_SIMPLE(property_cls, true);
};

class BoxedStaticmethod : public Box {
public:
    Box* sm_callable;

    BoxedStaticmethod(Box* callable) : sm_callable(callable) { Py_INCREF(sm_callable); }

    DEFAULT_CLASS_SIMPLE(staticmethod_cls, true);

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* self) noexcept;
};

class BoxedClassmethod : public Box {
public:
    Box* cm_callable;

    BoxedClassmethod(Box* callable) : cm_callable(callable) { Py_INCREF(cm_callable); }

    DEFAULT_CLASS_SIMPLE(classmethod_cls, true);

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* self) noexcept;
};

// TODO is there any particular reason to make this a Box, i.e. a python-level object?
class BoxedClosure : public Box {
public:
    BoxedClosure* parent;
    size_t nelts;
    Box* elts[0];

    BoxedClosure(BoxedClosure* parent) : parent(parent) { Py_XINCREF(parent); }

    // TODO: convert this to a var-object and use DEFAULT_CLASS_VAR_SIMPLE
    void* operator new(size_t size, size_t nelts) __attribute__((visibility("default"))) {
        BoxedClosure* rtn
            = static_cast<BoxedClosure*>(_PyObject_GC_Malloc(sizeof(BoxedClosure) + nelts * sizeof(Box*)));
        rtn->nelts = nelts;
        PyObject_INIT(rtn, closure_cls);
        _PyObject_GC_TRACK(rtn);
        memset((void*)rtn->elts, 0, sizeof(Box*) * nelts);
        return rtn;
    }

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* self) noexcept;
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
    bool iterated_from__hasnext__;
    ExcInfo exception;

    struct Context* context, *returnContext;
    void* stack_begin;
    FrameInfo* top_caller_frame_info; // The FrameInfo that called into this generator.

    // For abandoned-generator collection -- WIP
    FrameInfo* paused_frame_info; // The FrameInfo the generator was on when it called yield (or NULL if the generator
                                  // hasn't started or has exited).

#if STAT_TIMERS
    StatTimer* prev_stack;
    StatTimer my_timer;
#endif

    BoxedGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args);

    DEFAULT_CLASS(generator_cls);
};

struct wrapper_def {
    const llvm::StringRef name;
    int offset;
    void* function;      // "generic" handler that gets put in the tp_* slot which proxies to the python version
    wrapperfunc wrapper; // "wrapper" that ends up getting called by the Python-visible WrapperDescr
    const llvm::StringRef doc;
    int flags;
    BoxedString* name_strobj;
};

class BoxedWrapperDescriptor : public Box {
public:
    const wrapper_def* wrapper;
    BoxedClass* type;
    void* wrapped;
    BoxedWrapperDescriptor(const wrapper_def* wrapper, BoxedClass* type, void* wrapped)
        : wrapper(wrapper), type(type), wrapped(wrapped) {
        Py_INCREF(type);
    }

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* _self, visitproc visit, void* arg) noexcept;

    DEFAULT_CLASS(wrapperdescr_cls);

    static Box* descr_get(Box* self, Box* inst, Box* owner) noexcept;
    static Box* __call__(BoxedWrapperDescriptor* descr, PyObject* self, BoxedTuple* args, Box** _args);
    template <ExceptionStyle S>
    static Box* tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                        Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI);
};

class BoxedWrapperObject : public Box {
public:
    BoxedWrapperDescriptor* descr;
    Box* obj;

    BoxedWrapperObject(BoxedWrapperDescriptor* descr, Box* obj) : descr(descr), obj(obj) {
        Py_INCREF(descr);
        Py_INCREF(obj);
    }

    DEFAULT_CLASS(wrapperobject_cls);

    static Box* __call__(BoxedWrapperObject* self, Box* args, Box* kwds);
    template <ExceptionStyle S>
    static Box* tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                        Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI);

    static void dealloc(Box* self) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
};

class BoxedMethodDescriptor : public Box {
public:
    PyMethodDef* method;
    BoxedClass* type;

    BoxedMethodDescriptor(PyMethodDef* method, BoxedClass* type) : method(method), type(type) { Py_INCREF(type); }

    DEFAULT_CLASS(method_cls);

    static Box* descr_get(BoxedMethodDescriptor* self, Box* inst, Box* owner) noexcept;
    static Box* __call__(BoxedMethodDescriptor* self, Box* obj, BoxedTuple* varargs, Box** _args);
    template <ExceptionStyle S>
    static Box* tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                        Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI);

    static void dealloc(Box* self) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
};

Box* objectSetattr(Box* obj, Box* attr, Box* value);

BORROWED(Box*) unwrapAttrWrapper(Box* b);
void convertAttrwrapperToPrivateDict(Box* b);
Box* attrwrapperKeys(Box* b);
void attrwrapperDel(Box* b, llvm::StringRef attr);
void attrwrapperClear(Box* b);
BoxedDict* attrwrapperToDict(Box* b);
void attrwrapperSet(Box* b, Box* k, Box* v);

Box* boxAst(AST* ast);
AST* unboxAst(Box* b);

#define fatalOrError(exception, message)                                                                               \
    do {                                                                                                               \
        if (CONTINUE_AFTER_FATAL)                                                                                      \
            PyErr_SetString((exception), (message));                                                                   \
        else                                                                                                           \
            Py_FatalError((message));                                                                                  \
    } while (0)

// A descriptor that you can add to your class to provide instances with a __dict__ accessor.
// Classes created in Python get this automatically, but builtin types (including extension types)
// are supposed to add one themselves.  type_cls and function_cls do this, for example.
extern Box* dict_descr;

FunctionMetadata* metadataFromCode(Box* code);

BORROWED(Box*) getFrame(FrameInfo* frame_info);
BORROWED(Box*) getFrame(int depth);
void frameInvalidateBack(BoxedFrame* frame);
extern "C" void deinitFrame(FrameInfo* frame_info);
extern "C" void deinitFrameMaybe(FrameInfo* frame_info);
int frameinfo_traverse(FrameInfo* frame_info, visitproc visit, void* arg) noexcept;
extern "C" void initFrame(FrameInfo* frame_info);
extern "C" void setFrameExcInfo(FrameInfo* frame_info, STOLEN(Box*) type, STOLEN(Box*) value, STOLEN(Box*) tb);

inline BoxedString* boxString(llvm::StringRef s) {
    if (s.size() <= 1) {
        BoxedString* r;
        if (s.size() == 0)
            r = EmptyString;
        else
            r = characters[s.data()[0] & UCHAR_MAX];
        Py_INCREF(r);
        return r;
    }
    return new (s.size()) BoxedString(s);
}

#define MIN_INTERNED_INT -5  // inclusive
#define MAX_INTERNED_INT 256 // inclusive
static_assert(MIN_INTERNED_INT < 0 && MAX_INTERNED_INT > 0, "");
#define NUM_INTERNED_INTS ((-MIN_INTERNED_INT) + MAX_INTERNED_INT + 1)

extern BoxedInt* interned_ints[NUM_INTERNED_INTS];
extern "C" inline Box* boxInt(int64_t n) {
    if (n >= MIN_INTERNED_INT && n <= MAX_INTERNED_INT) {
        return incref(interned_ints[(-MIN_INTERNED_INT) + n]);
    }
    return new BoxedInt(n);
}

// Helper function: fetch an arg from our calling convention
inline Box*& getArg(int idx, Box*& arg1, Box*& arg2, Box*& arg3, Box** args) {
    if (idx == 0)
        return arg1;
    if (idx == 1)
        return arg2;
    if (idx == 2)
        return arg3;
    return args[idx - 3];
}

inline BORROWED(BoxedString*) getStaticString(llvm::StringRef s) {
    BoxedString* r = internStringImmortal(s);
    constants.push_back(r);
    return r;
}

extern "C" volatile int _pendingcalls_to_do;

inline BORROWED(Box*) Box::getattrString(const char* attr) {
    // XXX need to auto-decref
    BoxedString* s = internStringMortal(attr);
    try {
        Box* r = getattr<NOT_REWRITABLE>(s, NULL);
        Py_DECREF(s);
        return r;
    } catch (ExcInfo e) {
        Py_DECREF(s);
        throw e;
    }
}

inline void ExcInfo::clear() {
    Py_DECREF(type);
    Py_DECREF(value);
    Py_XDECREF(traceback);
}
} // namespace pyston

#endif
