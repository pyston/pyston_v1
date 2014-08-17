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

#include <ucontext.h>

#include "Python.h"
#include "structmember.h"

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

void setupSys();
void setupBuiltins();
void setupMath();
void setupTime();
void setupThread();
void setupPosix();
void setupSre();
void setupSysEnd();

BoxedDict* getSysModulesDict();
BoxedList* getSysPath();
Box* getSysStdout();

extern "C" {
extern BoxedClass* object_cls, *type_cls, *bool_cls, *int_cls, *float_cls, *str_cls, *function_cls, *none_cls,
    *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls, *xrange_cls, *member_cls,
    *method_cls, *closure_cls, *generator_cls;
}
extern "C" { extern Box* None, *NotImplemented, *True, *False; }
extern "C" {
extern Box* repr_obj, *len_obj, *hash_obj, *range_obj, *abs_obj, *min_obj, *max_obj, *open_obj, *id_obj, *chr_obj,
    *ord_obj, *trap_obj;
} // these are only needed for functionRepr, which is hacky
extern "C" { extern BoxedModule* sys_module, *builtins_module, *math_module, *time_module, *thread_module; }

extern "C" Box* boxBool(bool);
extern "C" Box* boxInt(i64);
extern "C" i64 unboxInt(Box*);
extern "C" Box* boxFloat(double d);
extern "C" Box* boxInstanceMethod(Box* obj, Box* func);
extern "C" Box* boxStringPtr(const std::string* s);
Box* boxString(const std::string& s);
Box* boxString(std::string&& s);
extern "C" BoxedString* boxStrConstant(const char* chars);
extern "C" BoxedString* boxStrConstantSize(const char* chars, size_t n);
extern "C" void listAppendInternal(Box* self, Box* v);
extern "C" void listAppendArrayInternal(Box* self, Box** v, int nelts);
extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, bool isGenerator,
                              std::initializer_list<Box*> defaults);
extern "C" CLFunction* unboxCLFunction(Box* b);
extern "C" Box* createUserClass(std::string* name, Box* base, Box* attr_dict);
extern "C" double unboxFloat(Box* b);
extern "C" Box* createDict();
extern "C" Box* createList();
extern "C" Box* createSlice(Box* start, Box* stop, Box* step);
extern "C" Box* createTuple(int64_t nelts, Box** elts);
extern "C" void printFloat(double d);


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

template <typename K, typename V, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K> >
class conservative_unordered_map
    : public std::unordered_map<K, V, Hash, KeyEqual, StlCompatAllocator<std::pair<const K, V> > > {};

class HiddenClass : public ConservativeGCObject {
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

    conservative_unordered_map<std::string, int> attr_offsets;
    conservative_unordered_map<std::string, HiddenClass*> children;

    HiddenClass* getOrMakeChild(const std::string& attr);

    int getOffset(const std::string& attr) {
        auto it = attr_offsets.find(attr);
        if (it == attr_offsets.end())
            return -1;
        return it->second;
    }
    HiddenClass* delAttrToMakeHC(const std::string& attr);
};


class BoxedInt : public Box {
public:
    int64_t n;

    BoxedInt(BoxedClass* cls, int64_t n) __attribute__((visibility("default"))) : Box(cls), n(n) {}
};

class BoxedFloat : public Box {
public:
    double d;

    BoxedFloat(double d) __attribute__((visibility("default"))) : Box(float_cls), d(d) {}
};

class BoxedBool : public Box {
public:
    bool b;

    BoxedBool(bool b) __attribute__((visibility("default"))) : Box(bool_cls), b(b) {}
};

class BoxedString : public Box {
public:
    // const std::basic_string<char, std::char_traits<char>, StlCompatAllocator<char> > s;
    const std::string s;

    BoxedString(const char* s, size_t n) __attribute__((visibility("default"))) : Box(str_cls), s(s, n) {}
    BoxedString(const std::string&& s) __attribute__((visibility("default"))) : Box(str_cls), s(std::move(s)) {}
    BoxedString(const std::string& s) __attribute__((visibility("default"))) : Box(str_cls), s(s) {}
};

class BoxedInstanceMethod : public Box {
public:
    Box* obj, *func;

    BoxedInstanceMethod(Box* obj, Box* func) __attribute__((visibility("default")))
    : Box(instancemethod_cls), obj(obj), func(func) {}
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

    BoxedList() __attribute__((visibility("default"))) : Box(list_cls), size(0), capacity(0) {}

    void ensure(int space);
    void shrink();
    static const int INITIAL_CAPACITY;
};

class BoxedTuple : public Box {
public:
    typedef std::vector<Box*, StlCompatAllocator<Box*> > GCVector;
    const GCVector elts;

    BoxedTuple(std::vector<Box*, StlCompatAllocator<Box*> >& elts) __attribute__((visibility("default")))
    : Box(tuple_cls), elts(elts) {}
    BoxedTuple(std::vector<Box*, StlCompatAllocator<Box*> >&& elts) __attribute__((visibility("default")))
    : Box(tuple_cls), elts(std::move(elts)) {}
};
extern "C" BoxedTuple* EmptyTuple;

class BoxedFile : public Box {
public:
    FILE* f;
    bool closed;
    bool softspace;
    BoxedFile(FILE* f) __attribute__((visibility("default"))) : Box(file_cls), f(f), closed(false), softspace(false) {}
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

    BoxedDict() __attribute__((visibility("default"))) : Box(dict_cls) {}
};

class BoxedFunction : public Box {
public:
    HCAttrs attrs;
    CLFunction* f;
    BoxedClosure* closure;

    bool isGenerator;
    int ndefaults;
    GCdArray* defaults;

    BoxedFunction(CLFunction* f);
    BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure = NULL,
                  bool isGenerator = false);
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
    BoxedSlice(Box* lower, Box* upper, Box* step) : Box(slice_cls), start(lower), stop(upper), step(step) {}
};

class BoxedMemberDescriptor : public Box {
public:
    enum MemberType {
        BOOL = T_BOOL,
        BYTE = T_BYTE,
        INT = T_INT,
        OBJECT = T_OBJECT,
    } type;

    int offset;

    BoxedMemberDescriptor(MemberType type, int offset) : Box(member_cls), type(type), offset(offset) {}
    BoxedMemberDescriptor(PyMemberDef* member)
        : Box(member_cls), type((MemberType)member->type), offset(member->offset) {}
};

// TODO is there any particular reason to make this a Box, ie a python-level object?
class BoxedClosure : public Box {
public:
    HCAttrs attrs;
    BoxedClosure* parent;

    BoxedClosure(BoxedClosure* parent) : Box(closure_cls), parent(parent) {}
};

class BoxedGenerator : public Box {
public:
    enum { STACK_SIZE = SIGSTKSZ * 5 };

    HCAttrs attrs;
    BoxedFunction* function;
    Box* arg1, *arg2, *arg3;
    GCdArray* args;

    bool entryExited;
    Box* returnValue;
    Box* exception;

    ucontext_t context, returnContext;
    char stack[STACK_SIZE];

    BoxedGenerator(BoxedFunction* function, Box* arg1, Box* arg2, Box* arg3, Box** args);
};

extern "C" void boxGCHandler(GCVisitor* v, Box* b);

Box* exceptionNew1(BoxedClass* cls);
Box* exceptionNew2(BoxedClass* cls, Box* message);

extern BoxedClass* Exception, *AssertionError, *AttributeError, *TypeError, *NameError, *KeyError, *IndexError,
    *IOError, *OSError, *ZeroDivisionError, *ValueError, *UnboundLocalError, *RuntimeError, *ImportError,
    *StopIteration, *GeneratorExit, *SyntaxError;

// cls should be obj->cls.
// Added as parameter because it should typically be available
inline void initUserAttrs(Box* obj, BoxedClass* cls) {
    assert(obj->cls == cls);
    if (cls->attrs_offset) {
        HCAttrs* attrs = obj->getAttrsPtr();
        attrs = new ((void*)attrs) HCAttrs();
    }
}

Box* makeAttrWrapper(Box* b);
}
#endif
