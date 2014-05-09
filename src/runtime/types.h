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

#include "core/types.h"

namespace pyston {

extern bool IN_SHUTDOWN;

class BoxedString;
class BoxedList;
class BoxedDict;
class BoxedTuple;
class BoxedFile;

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
void setupMath();
void setupTime();
void setupBuiltins();

BoxedDict* getSysModulesDict();
BoxedList* getSysPath();

extern "C" { extern BoxedClass *type_cls, *bool_cls, *int_cls, *float_cls, *str_cls, *function_cls, *none_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls, *xrange_cls; }
extern "C" { extern const ObjectFlavor type_flavor, bool_flavor, int_flavor, float_flavor, str_flavor, function_flavor, none_flavor, instancemethod_flavor, list_flavor, slice_flavor, module_flavor, dict_flavor, tuple_flavor, file_flavor, xrange_flavor; }
extern "C" { extern const ObjectFlavor user_flavor; }

extern "C" { extern Box *None, *NotImplemented, *True, *False; }
extern "C" { extern Box *repr_obj, *len_obj, *hash_obj, *range_obj, *abs_obj, *min_obj, *max_obj, *open_obj, *chr_obj, *trap_obj; } // these are only needed for functionRepr, which is hacky
extern "C" { extern BoxedModule *sys_module, *math_module, *time_module, *builtins_module; }

extern "C" Box* boxBool(bool);
extern "C" Box* boxInt(i64);
extern "C" i64 unboxInt(Box*);
extern "C" Box* boxFloat(double d);
extern "C" Box* boxInstanceMethod(Box* obj, Box* func);
extern "C" Box* boxStringPtr(const std::string *s);
Box* boxString(const std::string &s);
extern "C" BoxedString* boxStrConstant(const char* chars);
extern "C" void listAppendInternal(Box* self, Box* v);
extern "C" void listAppendArrayInternal(Box* self, Box** v, int nelts);
extern "C" Box* boxCLFunction(CLFunction *f);
extern "C" CLFunction* unboxCLFunction(Box* b);
extern "C" Box* createClass(std::string *name, BoxedModule *parent_module);
extern "C" double unboxFloat(Box *b);
extern "C" Box* createDict();
extern "C" Box* createList();
extern "C" Box* createSlice(Box* start, Box* stop, Box* step);
extern "C" Box* createTuple(int64_t nelts, Box* *elts);
extern "C" void printFloat(double d);


class BoxedInt : public Box {
    public:
        int64_t n;

        BoxedInt(int64_t n) __attribute__((visibility("default"))) : Box(&int_flavor, int_cls), n(n) {}
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
        const std::string s;

        BoxedString(const std::string &&s) __attribute__((visibility("default"))) : Box(&str_flavor, str_cls), s(std::move(s)) {}
        BoxedString(const std::string &s) __attribute__((visibility("default"))) : Box(&str_flavor, str_cls), s(s) {}
};

class BoxedInstanceMethod : public Box {
    public:
        Box *obj, *func;

        BoxedInstanceMethod(Box *obj, Box *func) __attribute__((visibility("default"))) : Box(&instancemethod_flavor, instancemethod_cls), obj(obj), func(func) {}
};

class BoxedList : public Box {
    public:
        class ElementArray : GCObject {
            public:
                Box* elts[0];

                ElementArray() : GCObject(&untracked_kind) {}

                void *operator new(size_t size, int capacity) {
                    return rt_alloc(capacity * sizeof(Box*) + sizeof(BoxedList::ElementArray));
                }
        };

        int64_t size, capacity;
        ElementArray *elts;

        BoxedList() __attribute__((visibility("default"))) : Box(&list_flavor, list_cls), size(0), capacity(0) {}

        void ensure(int space);
};

class BoxedTuple : public Box {
    public:
        const std::vector<Box*> elts;

        BoxedTuple(std::vector<Box*> &elts) __attribute__((visibility("default"))) : Box(&tuple_flavor, tuple_cls), elts(elts) {}
};

class BoxedFile : public Box {
    public:
        FILE *f;
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

class ConservativeWrapper : public GCObject {
    public:
        void* data[0];

        ConservativeWrapper(size_t data_size) : GCObject(&conservative_kind), data() {
            assert(data_size % sizeof(void*) == 0);
            gc_header.kind_data = data_size;
        }

        void *operator new(size_t size, size_t data_size) {
            assert(size == sizeof(ConservativeWrapper));
            return rt_alloc(data_size + size);
        }

        static ConservativeWrapper* fromPointer(void* p) {
            ConservativeWrapper* o = (ConservativeWrapper*)((void**)p - 1);
            assert(&o->data == p);
            return o;
        }
};

template <class T>
class StlCompatAllocator {
    public:
        typedef size_t size_type;
        typedef T value_type;
        typedef T* pointer;
        typedef const T* const_pointer;
        typedef T& reference;
        typedef const T& const_reference;
        typedef std::ptrdiff_t difference_type;

        StlCompatAllocator() {}
        template <class U>
        StlCompatAllocator(const StlCompatAllocator<U>& other) {}

        template <class U>
        struct rebind {
            typedef StlCompatAllocator<U> other;
        };

        pointer allocate(size_t n) {
            size_t to_allocate = n * sizeof(value_type);
            //assert(to_allocate < (1<<16));

            ConservativeWrapper* rtn = new (to_allocate) ConservativeWrapper(to_allocate);
            return (pointer)&rtn->data[0];
        }

        void deallocate(pointer p, size_t n) {
            ConservativeWrapper* o = ConservativeWrapper::fromPointer(p);
            rt_free(o);
        }

        // I would never be able to come up with this on my own:
        // http://en.cppreference.com/w/cpp/memory/allocator/construct
        template <class U, class... Args>
        void construct(U* p, Args&&... args) {
            ::new((void*)p) U(std::forward<Args>(args)...);
        }

        template <class U>
        void destroy(U* p) {
            p->~U();
        }
};

class BoxedDict : public Box {
    public:
        std::unordered_map<Box*, Box*, PyHasher, PyEq, StlCompatAllocator<std::pair<Box*, Box*> > > d;

        BoxedDict() __attribute__((visibility("default"))) : Box(&dict_flavor, dict_cls) {}
};

class BoxedFunction : public HCBox {
    public:
        CLFunction *f;

        BoxedFunction(CLFunction *f);
};

class BoxedModule : public HCBox {
    public:
        const std::string fn; // for traceback purposes; not the same as __file__

        BoxedModule(const std::string &name, const std::string &fn);
        std::string name();
};

class BoxedSlice : public HCBox {
    public:
        Box *start, *stop, *step;
        BoxedSlice(Box *lower, Box *upper, Box *step) : HCBox(&slice_flavor, slice_cls), start(lower), stop(upper), step(step) {}
};

extern "C" void boxGCHandler(GCVisitor *v, void* p);

}
#endif

