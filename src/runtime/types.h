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
#include <llvm/ADT/Twine.h>
#include <ucontext.h>

#include "Python.h"
#include "structmember.h"

#include "codegen/irgen/future.h"
#include "core/contiguous_map.h"
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
void setupFrame();

void setupSys();
void setupBuiltins();
void setupPyston();
void setupThread();
void setupImport();
void setupAST();
void setupSysEnd();

BoxedDict* getSysModulesDict();
BoxedList* getSysPath();
extern "C" Box* getSysStdout();

extern "C" {
extern BoxedClass* object_cls, *type_cls, *bool_cls, *int_cls, *long_cls, *float_cls, *str_cls, *function_cls,
    *none_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls,
    *enumerate_cls, *xrange_cls, *member_descriptor_cls, *method_cls, *closure_cls, *generator_cls, *complex_cls,
    *basestring_cls, *property_cls, *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *pyston_getset_cls,
    *capi_getset_cls, *builtin_function_or_method_cls, *set_cls, *frozenset_cls, *code_cls, *frame_cls;
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
extern "C" Box* boxInstanceMethod(Box* obj, Box* func, Box* type);
extern "C" Box* boxUnboundInstanceMethod(Box* func, Box* type);

extern "C" Box* boxStringPtr(const std::string* s);
Box* boxString(llvm::StringRef s);
Box* boxStringTwine(const llvm::Twine& s);

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
extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, Box* globals, std::initializer_list<Box*> defaults);
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
    // Negative offset is from the end of the class (useful for variable-size objects with the attrs at the end)
    // Analogous to tp_dictoffset
    // A class should have at most of one attrs_offset and tp_dictoffset be nonzero.
    // (But having nonzero attrs_offset here would map to having nonzero tp_dictoffset in CPython)
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
    friend void setupThread();
};

class BoxedHeapClass : public BoxedClass {
public:
    PyNumberMethods as_number;
    PyMappingMethods as_mapping;
    PySequenceMethods as_sequence;
    PyBufferProcs as_buffer;

    BoxedString* ht_name;
    PyObject* ht_slots;

    typedef size_t SlotOffset;
    SlotOffset* slotOffsets() { return (BoxedHeapClass::SlotOffset*)((char*)this + this->cls->tp_basicsize); }
    size_t nslots() { return this->ob_size; }

    // These functions are the preferred way to construct new types:
    static BoxedHeapClass* create(BoxedClass* metatype, BoxedClass* base, gcvisit_func gc_visit, int attrs_offset,
                                  int weaklist_offset, int instance_size, bool is_user_defined, BoxedString* name,
                                  BoxedTuple* bases, size_t nslots);
    static BoxedHeapClass* create(BoxedClass* metatype, BoxedClass* base, gcvisit_func gc_visit, int attrs_offset,
                                  int weaklist_offset, int instance_size, bool is_user_defined, llvm::StringRef name);

private:
    // These functions are not meant for external callers and will mostly just be called
    // by BoxedHeapClass::create(), but setupRuntime() also needs to do some manual class
    // creation due to bootstrapping issues.
    BoxedHeapClass(BoxedClass* base, gcvisit_func gc_visit, int attrs_offset, int weaklist_offset, int instance_size,
                   bool is_user_defined, BoxedString* name);

    friend void setupRuntime();
    friend void setupSys();
    friend void setupThread();

    DEFAULT_CLASS_VAR(type_cls, sizeof(SlotOffset));
};

static_assert(sizeof(pyston::Box) == sizeof(struct _object), "");
static_assert(offsetof(pyston::Box, cls) == offsetof(struct _object, ob_type), "");

static_assert(offsetof(pyston::BoxedClass, cls) == offsetof(struct _typeobject, ob_type), "");
static_assert(offsetof(pyston::BoxedClass, tp_name) == offsetof(struct _typeobject, tp_name), "");
static_assert(offsetof(pyston::BoxedClass, attrs) == offsetof(struct _typeobject, _hcls), "");
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
        SINGLETON,   // name->offset map stored in hidden class, but hcls is mutable
    } const type;

    static HiddenClass* dict_backed;

private:
    HiddenClass(HCType type) : type(type) {}
    HiddenClass(HiddenClass* parent) : type(NORMAL), attr_offsets(), attrwrapper_offset(parent->attrwrapper_offset) {
        assert(parent->type == NORMAL);
        for (auto& p : parent->attr_offsets) {
            this->attr_offsets.insert(&p);
        }
    }

    // These fields only make sense for NORMAL or SINGLETON hidden classes:
    llvm::StringMap<int> attr_offsets;
    // If >= 0, is the offset where we stored an attrwrapper object
    int attrwrapper_offset = -1;

    // These are only for NORMAL hidden classes:
    ContiguousMap<llvm::StringRef, HiddenClass*, llvm::StringMap<int>> children;
    HiddenClass* attrwrapper_child = NULL;

    // Only for SINGLETON hidden classes:
    ICInvalidator dependent_getattrs;

public:
    static HiddenClass* makeSingleton() { return new HiddenClass(SINGLETON); }

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
        visitor->visitRange((void* const*)&children.vector()[0], (void* const*)&children.vector()[children.size()]);
        if (attrwrapper_child)
            visitor->visit(attrwrapper_child);
    }

    // The total size of the attribute array.  The slots in the attribute array may not correspond 1:1 to Python
    // attributes.
    int attributeArraySize() {
        if (type == DICT_BACKED)
            return 1;

        ASSERT(type == NORMAL || type == SINGLETON, "%d", type);
        int r = attr_offsets.size();
        if (attrwrapper_offset != -1)
            r += 1;
        return r;
    }

    // The mapping from string attribute names to attribute offsets.  There may be other objects in the attributes
    // array.
    // Only valid for NORMAL or SINGLETON hidden classes
    const llvm::StringMap<int>& getStrAttrOffsets() {
        assert(type == NORMAL || type == SINGLETON);
        return attr_offsets;
    }

    // Only valid for NORMAL hidden classes:
    HiddenClass* getOrMakeChild(llvm::StringRef attr);

    // Only valid for NORMAL or SINGLETON hidden classes:
    int getOffset(llvm::StringRef attr) {
        assert(type == NORMAL || type == SINGLETON);
        auto it = attr_offsets.find(attr);
        if (it == attr_offsets.end())
            return -1;
        return it->second;
    }

    int getAttrwrapperOffset() {
        assert(type == NORMAL || type == SINGLETON);
        return attrwrapper_offset;
    }

    // Only valid for SINGLETON hidden classes:
    void appendAttribute(llvm::StringRef attr);
    void appendAttrwrapper();
    void delAttribute(llvm::StringRef attr);
    void addDependence(Rewriter* rewriter);

    // Only valid for NORMAL hidden classes:
    HiddenClass* getAttrwrapperChild();

    // Only valid for NORMAL hidden classes:
    HiddenClass* delAttrToMakeHC(llvm::StringRef attr);
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
static_assert(sizeof(BoxedFloat) == sizeof(PyFloatObject), "");
static_assert(offsetof(BoxedFloat, d) == offsetof(PyFloatObject, ob_fval), "");

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

class BoxedString : public BoxVar {
public:
    // llvm::StringRef is basically just a pointer and a length, so with proper compiler
    // optimizations and inlining, creating a new one each time shouldn't have any cost.
    llvm::StringRef s() const { return llvm::StringRef(s_data, ob_size); };

    char interned_state;

    char* data() { return s_data; }
    size_t size() { return this->ob_size; }

    // DEFAULT_CLASS_VAR_SIMPLE doesn't work because of the +1 for the null byte
    void* operator new(size_t size, BoxedClass* cls, size_t nitems) __attribute__((visibility("default"))) {
        ALLOC_STATS_VAR(str_cls)

        assert(cls->tp_itemsize == sizeof(char));
        return BoxVar::operator new(size, cls, nitems);
    }
    void* operator new(size_t size, size_t nitems) __attribute__((visibility("default"))) {
        ALLOC_STATS_VAR(str_cls)

        assert(str_cls->tp_alloc == PystonType_GenericAlloc);
        assert(str_cls->tp_itemsize == 1);
        assert(str_cls->tp_basicsize == sizeof(BoxedString) + 1);
        assert(str_cls->is_pyston_class);
        assert(str_cls->attrs_offset == 0);

        void* mem = gc_alloc(sizeof(BoxedString) + 1 + nitems, gc::GCKind::PYTHON);
        assert(mem);

        BoxVar* rtn = static_cast<BoxVar*>(mem);
        rtn->cls = str_cls;
        rtn->ob_size = nitems;
        return rtn;
    }

    // these should be private, but strNew needs them
    BoxedString(const char* s, size_t n) __attribute__((visibility("default")));
    explicit BoxedString(size_t n, char c) __attribute__((visibility("default")));
    explicit BoxedString(llvm::StringRef s) __attribute__((visibility("default")));
    explicit BoxedString(llvm::StringRef lhs, llvm::StringRef rhs) __attribute__((visibility("default")));

private:
    void* operator new(size_t size) = delete;

    char s_data[0];
};

template <typename T> struct StringHash {
    size_t operator()(const T* str) {
        size_t hash = 5381;
        T c;

        while ((c = *str++))
            hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

        return hash;
    }
    size_t operator()(const T* str, int len) {
        size_t hash = 5381;
        T c;

        while (--len >= 0) {
            c = *str++;
            hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
        }

        return hash;
    }
};

template <> struct StringHash<std::string> {
    size_t operator()(const std::string& str) {
        StringHash<char> H;
        return H(&str[0], str.size());
    }
};


class BoxedInstanceMethod : public Box {
public:
    Box** in_weakreflist;

    // obj is NULL for unbound instancemethod
    Box* obj, *func, *im_class;

    BoxedInstanceMethod(Box* obj, Box* func, Box* im_class) __attribute__((visibility("default")))
    : in_weakreflist(NULL), obj(obj), func(func), im_class(im_class) {}

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

class BoxedTuple : public BoxVar {
public:
    typedef std::vector<Box*, StlCompatAllocator<Box*>> GCVector;

    DEFAULT_CLASS_VAR_SIMPLE(tuple_cls, sizeof(Box*));

    static BoxedTuple* create(int64_t size) { return new (size) BoxedTuple(size); }
    static BoxedTuple* create(int64_t nelts, Box** elts) {
        BoxedTuple* rtn = new (nelts) BoxedTuple(nelts);
        memmove(&rtn->elts[0], elts, sizeof(Box*) * nelts);
        return rtn;
    }
    static BoxedTuple* create1(Box* elt0) {
        BoxedTuple* rtn = new (1) BoxedTuple(1);
        rtn->elts[0] = elt0;
        return rtn;
    }
    static BoxedTuple* create2(Box* elt0, Box* elt1) {
        BoxedTuple* rtn = new (2) BoxedTuple(2);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        return rtn;
    }
    static BoxedTuple* create3(Box* elt0, Box* elt1, Box* elt2) {
        BoxedTuple* rtn = new (3) BoxedTuple(3);
        rtn->elts[0] = elt0;
        rtn->elts[1] = elt1;
        rtn->elts[2] = elt2;
        return rtn;
    }
    static BoxedTuple* create(std::initializer_list<Box*> members) { return new (members.size()) BoxedTuple(members); }

    static BoxedTuple* create(int64_t size, BoxedClass* cls) { return new (cls, size) BoxedTuple(size); }
    static BoxedTuple* create(int64_t nelts, Box** elts, BoxedClass* cls) {
        BoxedTuple* rtn = new (cls, nelts) BoxedTuple(nelts);
        memmove(&rtn->elts[0], elts, sizeof(Box*) * nelts);
        return rtn;
    }
    static BoxedTuple* create(std::initializer_list<Box*> members, BoxedClass* cls) {
        return new (cls, members.size()) BoxedTuple(members);
    }

    static int Resize(BoxedTuple** pt, size_t newsize) noexcept;

    Box* const* begin() const { return &elts[0]; }
    Box* const* end() const { return &elts[ob_size]; }
    Box*& operator[](size_t index) { return elts[index]; }

    size_t size() const { return ob_size; }

private:
    BoxedTuple(size_t size) { memset(elts, 0, sizeof(Box*) * size); }

    BoxedTuple(std::initializer_list<Box*>& members) {
        // by the time we make it here elts[] is big enough to contain members
        Box** p = &elts[0];
        for (auto b : members) {
            *p++ = b;
        }
    }

public:
    Box* elts[0];
};

extern "C" BoxedTuple* EmptyTuple;
extern "C" BoxedString* EmptyString;

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

    // TODO these should really go in BoxedFunction but it's annoying because they don't get
    // initializd until after BoxedFunctionBase's constructor is run which means they could have
    // garbage values when the GC is run (BoxedFunctionBase's constructor might call the GC).
    // So ick... needs to be fixed.
    BoxedClosure* closure;
    Box* globals;

    int ndefaults;
    GCdArray* defaults;

    ICInvalidator dependent_ics;

    // Accessed via member descriptor
    Box* modname;      // __module__
    BoxedString* name; // __name__ (should be here or in one of the derived classes?)
    Box* doc;          // __doc__

    BoxedFunctionBase(CLFunction* f);
    BoxedFunctionBase(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL);
};

class BoxedFunction : public BoxedFunctionBase {
public:
    HCAttrs attrs;

    BoxedFunction(CLFunction* f);
    BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                  Box* globals = NULL);

    DEFAULT_CLASS(function_cls);
};

class BoxedBuiltinFunctionOrMethod : public BoxedFunctionBase {
public:
    BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name, const char* doc = NULL);
    BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name, std::initializer_list<Box*> defaults,
                                 BoxedClosure* closure = NULL, const char* doc = NULL);

    DEFAULT_CLASS(builtin_function_or_method_cls);
};

class BoxedModule : public Box {
public:
    HCAttrs attrs;

    FutureFlags future_flags;

    BoxedModule() {} // noop constructor to disable zero-initialization of cls
    std::string name();

    Box* getStringConstant(llvm::StringRef ast_str);

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

    DEFAULT_CLASS_SIMPLE(member_descriptor_cls);
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
    bool getter_doc;

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
    bool iterated_from__hasnext__;
    ExcInfo exception;

    struct Context* context, *returnContext;
    void* stack_begin;

#if STAT_TIMERS
    StatTimer* statTimers;
    uint64_t timer_time;
#endif

    BoxedGenerator(BoxedFunctionBase* function, Box* arg1, Box* arg2, Box* arg3, Box** args);

    DEFAULT_CLASS(generator_cls);
};

extern "C" void boxGCHandler(GCVisitor* v, Box* b);

Box* objectNewNoArgs(BoxedClass* cls);
Box* objectSetattr(Box* obj, Box* attr, Box* value);

Box* unwrapAttrWrapper(Box* b);
Box* attrwrapperKeys(Box* b);
void attrwrapperDel(Box* b, llvm::StringRef attr);

Box* boxAst(AST* ast);
AST* unboxAst(Box* b);

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
Box* codeForCLFunction(CLFunction*);
CLFunction* clfunctionFromCode(Box* code);

Box* getFrame(int depth);
}

#endif
