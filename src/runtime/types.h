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

#ifndef PYSTON_RUNTIME_TYPES_H
#define PYSTON_RUNTIME_TYPES_H

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

void setupInt();
void teardownInt();
void setupFloat();
void teardownFloat();
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

void setupSys();
void setupBuiltins();
void setupMath();
void setupTime();
void setupThread();
void setupErrno();
void setupPosix();
void setupSysEnd();

BoxedDict* getSysModulesDict();
BoxedList* getSysPath();

extern "C" {
extern BoxedClass* object_cls, *type_cls, *bool_cls, *int_cls, *float_cls, *str_cls, *function_cls, *none_cls,
    *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls, *xrange_cls, *member_cls,
    *closure_cls;
}
extern "C" {
extern const ObjectFlavor object_flavor, type_flavor, bool_flavor, int_flavor, float_flavor, str_flavor,
    function_flavor, none_flavor, instancemethod_flavor, list_flavor, slice_flavor, module_flavor, dict_flavor,
    tuple_flavor, file_flavor, xrange_flavor, member_flavor, closure_flavor;
}
extern "C" { extern Box* None, *NotImplemented, *True, *False; }
extern "C" {
extern Box* repr_obj, *len_obj, *hash_obj, *range_obj, *abs_obj, *min_obj, *max_obj, *open_obj, *chr_obj, *ord_obj,
    *trap_obj;
} // these are only needed for functionRepr, which is hacky
extern "C" { extern BoxedModule* sys_module, *builtins_module, *math_module, *time_module, *thread_module; }

extern "C" Box* boxBool(bool);
extern "C" Box* boxInt(i64);
extern "C" i64 unboxInt(Box*);
extern "C" Box* boxFloat(double d);
extern "C" Box* boxInstanceMethod(Box* obj, Box* func);
extern "C" Box* boxStringPtr(const std::string* s);
Box* boxString(const std::string& s);
extern "C" BoxedString* boxStrConstant(const char* chars);
extern "C" void listAppendInternal(Box* self, Box* v);
extern "C" void listAppendArrayInternal(Box* self, Box** v, int nelts);
extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, std::initializer_list<Box*> defaults);
extern "C" CLFunction* unboxCLFunction(Box* b);
extern "C" Box* createUserClass(std::string* name, Box* base, Box* attr_dict);
extern "C" double unboxFloat(Box* b);
extern "C" Box* createDict();
extern "C" Box* createList();
extern "C" Box* createSlice(Box* start, Box* stop, Box* step);
extern "C" Box* createTuple(int64_t nelts, Box** elts);
extern "C" void printFloat(double d);


class ConservativeWrapper : public GCObject {
public:
    void* data[0];

    ConservativeWrapper(size_t data_size) : GCObject(&conservative_kind), data() {
        assert(data_size % sizeof(void*) == 0);
        gc_header.kind_data = data_size;
    }

    void* operator new(size_t size, size_t data_size) {
        assert(size == sizeof(ConservativeWrapper));
        return rt_alloc(data_size + size);
    }

    static ConservativeWrapper* fromPointer(void* p) {
        ConservativeWrapper* o = (ConservativeWrapper*)((void**)p - 1);
        assert(&o->data == p);
        return o;
    }
};

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

        ConservativeWrapper* rtn = new (to_allocate) ConservativeWrapper(to_allocate);
        return (pointer)&rtn->data[0];
    }

    void deallocate(pointer p, size_t n) {
        ConservativeWrapper* o = ConservativeWrapper::fromPointer(p);
        rt_free(o);
    }

    // I would never be able to come up with this on my own:
    // http://en.cppreference.com/w/cpp/memory/allocator/construct
    template <class U, class... Args> void construct(U* p, Args&&... args) {
        ::new ((void*)p) U(std::forward<Args>(args)...);
    }

    template <class U> void destroy(U* p) { p->~U(); }

    bool operator==(const StlCompatAllocator<T>& rhs) const { return true; }
    bool operator!=(const StlCompatAllocator<T>& rhs) const { return false; }
};


class BoxedInt : public Box {
public:
    int64_t n;

    BoxedInt(BoxedClass* cls, int64_t n) __attribute__((visibility("default"))) : Box(&int_flavor, cls), n(n) {}
};

class BoxedFloat : public Box {
public:
    double d;

    BoxedFloat(double d) __attribute__((visibility("default"))) : Box(&float_flavor, float_cls), d(d) {}
};

class BoxedBool : public Box {
public:
    bool b;

    BoxedBool(bool b) __attribute__((visibility("default"))) : Box(&bool_flavor, bool_cls), b(b) {}
};

class BoxedString : public Box {
public:
    // const std::basic_string<char, std::char_traits<char>, StlCompatAllocator<char> > s;
    const std::string s;

    BoxedString(const std::string&& s) __attribute__((visibility("default")))
    : Box(&str_flavor, str_cls), s(std::move(s)) {}
    BoxedString(const std::string& s) __attribute__((visibility("default"))) : Box(&str_flavor, str_cls), s(s) {}
};

class BoxedInstanceMethod : public Box {
public:
    Box* obj, *func;

    BoxedInstanceMethod(Box* obj, Box* func) __attribute__((visibility("default")))
    : Box(&instancemethod_flavor, instancemethod_cls), obj(obj), func(func) {}
};

class GCdArray : public GCObject {
public:
    Box* elts[0];

    GCdArray() : GCObject(&untracked_kind) {}

    void* operator new(size_t size, int capacity) {
        assert(size == sizeof(GCdArray));
        return rt_alloc(capacity * sizeof(Box*) + size);
    }

    static GCdArray* realloc(GCdArray* array, int capacity) {
        return (GCdArray*)rt_realloc(array, capacity * sizeof(Box*) + sizeof(GCdArray));
    }
};

class BoxedList : public Box {
public:
    int64_t size, capacity;
    GCdArray* elts;

    DS_DEFINE_MUTEX(lock);

    BoxedList() __attribute__((visibility("default"))) : Box(&list_flavor, list_cls), size(0), capacity(0) {}

    void ensure(int space);
    void shrink();
    static const int INITIAL_CAPACITY;
};

class BoxedTuple : public Box {
public:
    typedef std::vector<Box*, StlCompatAllocator<Box*> > GCVector;
    const GCVector elts;

    BoxedTuple(std::vector<Box*, StlCompatAllocator<Box*> >& elts) __attribute__((visibility("default")))
    : Box(&tuple_flavor, tuple_cls), elts(elts) {}
    BoxedTuple(std::vector<Box*, StlCompatAllocator<Box*> >&& elts) __attribute__((visibility("default")))
    : Box(&tuple_flavor, tuple_cls), elts(std::move(elts)) {}
};
extern "C" BoxedTuple* EmptyTuple;

class BoxedFile : public Box {
public:
    FILE* f;
    bool closed;
    BoxedFile(FILE* f) __attribute__((visibility("default"))) : Box(&file_flavor, file_cls), f(f), closed(false) {}
};

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
    typedef std::unordered_map<Box*, Box*, PyHasher, PyEq, StlCompatAllocator<std::pair<Box*, Box*> > > DictMap;

    DictMap d;

    BoxedDict() __attribute__((visibility("default"))) : Box(&dict_flavor, dict_cls) {}
};

class BoxedFunction : public Box {
public:
    HCAttrs attrs;
    CLFunction* f;
    BoxedClosure* closure;

    int ndefaults;
    GCdArray* defaults;

    BoxedFunction(CLFunction* f);
    BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL);
};

class BoxedModule : public Box {
public:
    HCAttrs attrs;
    std::string fn; // for traceback purposes; not the same as __file__

    BoxedModule(const std::string& name, const std::string& fn);
    std::string name();
};

class BoxedSlice : public Box {
public:
    Box* start, *stop, *step;
    BoxedSlice(Box* lower, Box* upper, Box* step)
        : Box(&slice_flavor, slice_cls), start(lower), stop(upper), step(step) {}
};

class BoxedMemberDescriptor : public Box {
public:
    enum MemberType {
        OBJECT,
    } type;

    int offset;

    BoxedMemberDescriptor(MemberType type, int offset) : Box(&member_flavor, member_cls), type(type), offset(offset) {}
};

// TODO is there any particular reason to make this a Box, ie a python-level object?
class BoxedClosure : public Box {
public:
    HCAttrs attrs;
    BoxedClosure* parent;

    BoxedClosure(BoxedClosure* parent) : Box(&closure_flavor, closure_cls), parent(parent) {}
};

extern "C" void boxGCHandler(GCVisitor* v, void* p);

Box* exceptionNew1(BoxedClass* cls);
Box* exceptionNew2(BoxedClass* cls, Box* message);

extern BoxedClass* Exception, *AssertionError, *AttributeError, *TypeError, *NameError, *KeyError, *IndexError,
    *IOError, *OSError, *ZeroDivisionError, *ValueError, *UnboundLocalError, *RuntimeError, *ImportError,
    *StopIteration;

// cls should be obj->cls.
// Added as parameter because it should typically be available
inline void initUserAttrs(Box* obj, BoxedClass* cls) {
    assert(obj->cls == cls);
    if (cls->attrs_offset) {
        HCAttrs* attrs = obj->getAttrsPtr();
        attrs = new ((void*)attrs) HCAttrs();
    }
}
}
#endif
