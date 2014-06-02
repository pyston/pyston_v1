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

#include "runtime/types.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdint.h>

#include "codegen/compvars.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"

namespace pyston {

bool IN_SHUTDOWN = false;

#define SLICE_START_OFFSET ((char*)&(((BoxedSlice*)0x01)->start) - (char*)0x1)
#define SLICE_STOP_OFFSET ((char*)&(((BoxedSlice*)0x01)->stop) - (char*)0x1)
#define SLICE_STEP_OFFSET ((char*)&(((BoxedSlice*)0x01)->step) - (char*)0x1)

BoxIterator& BoxIterator::operator++() {
    static std::string hasnext_str("__hasnext__");
    static std::string next_str("next");

    Box* hasnext = callattrInternal(iter, &hasnext_str, CLASS_ONLY, NULL, 0, NULL, NULL, NULL, NULL);
    if (nonzero(hasnext)) {
        value = callattrInternal(iter, &next_str, CLASS_ONLY, NULL, 0, NULL, NULL, NULL, NULL);
    } else {
        iter = nullptr;
        value = nullptr;
    }
    return *this;
}

llvm::iterator_range<BoxIterator> Box::pyElements() {
    static std::string iter_str("__iter__");

    Box* iter = callattr(const_cast<Box*>(this), &iter_str, true, 0, NULL, NULL, NULL, NULL);
    if (iter) {
        return llvm::iterator_range<BoxIterator>(++BoxIterator(iter), BoxIterator(nullptr));
    }

    raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(this)->c_str());
}

extern "C" BoxedFunction::BoxedFunction(CLFunction* f) : Box(&function_flavor, function_cls), f(f) {
    if (f->source) {
        assert(f->source->ast);
        // this->giveAttr("__name__", boxString(&f->source->ast->name));
        this->giveAttr("__name__", boxString(f->source->getName()));

        Box* modname = f->source->parent_module->getattr("__name__", NULL, NULL);
        this->giveAttr("__module__", modname);
    }
}

BoxedModule::BoxedModule(const std::string& name, const std::string& fn) : Box(&module_flavor, module_cls), fn(fn) {
    this->giveAttr("__name__", boxString(name));
    this->giveAttr("__file__", boxString(fn));
}

std::string BoxedModule::name() {
    Box* name = this->getattr("__name__");
    if (!name || name->cls != str_cls) {
        return "?";
    } else {
        BoxedString* sname = static_cast<BoxedString*>(name);
        return sname->s;
    }
}

extern "C" Box* boxCLFunction(CLFunction* f) {
    return new BoxedFunction(f);
}

extern "C" CLFunction* unboxCLFunction(Box* b) {
    return static_cast<BoxedFunction*>(b)->f;
}

extern "C" void boxGCHandler(GCVisitor* v, void* p) {
    Box* b = (Box*)p;

    if (b->cls) {
        v->visit(b->cls);

        if (b->cls->instancesHaveAttrs()) {
            HCAttrs* attrs = b->getAttrsPtr();

            v->visit(attrs->hcls);
            int nattrs = attrs->hcls->attr_offsets.size();
            if (nattrs) {
                HCAttrs::AttrList* attr_list = attrs->attr_list;
                assert(attr_list);
                v->visit(attr_list);
                v->visitRange((void**)&attr_list->attrs[0], (void**)&attr_list->attrs[nattrs]);
            }
        }
    } else {
        assert(type_cls == NULL || p == type_cls);
    }
}

extern "C" void typeGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedClass* b = (BoxedClass*)p;
}

extern "C" void hcGCHandler(GCVisitor* v, void* p) {
    HiddenClass* hc = (HiddenClass*)p;
    for (const auto& p : hc->children) {
        v->visit(p.second);
    }
}

extern "C" void instancemethodGCHandler(GCVisitor* v, void* p) {
    BoxedInstanceMethod* im = (BoxedInstanceMethod*)p;

    v->visit(im->obj);
    v->visit(im->func);
}

// This probably belongs in list.cpp?
extern "C" void listGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedList* l = (BoxedList*)p;
    int size = l->size;
    int capacity = l->capacity;
    assert(capacity >= size);
    if (capacity)
        v->visit(l->elts);
    if (size)
        v->visitRange((void**)&l->elts->elts[0], (void**)&l->elts->elts[size]);

    static StatCounter sc("gc_listelts_visited");
    sc.log(size);
}

extern "C" void sliceGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedSlice* sl = static_cast<BoxedSlice*>(p);
    assert(sl->cls == slice_cls);

    v->visit(sl->start);
    v->visit(sl->stop);
    v->visit(sl->step);
}

// This probably belongs in tuple.cpp?
extern "C" void tupleGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedTuple* t = (BoxedTuple*)p;
    v->visitPotentialRange((void* const*)&t->elts, (void* const*)(&t->elts + 1));
}

// This probably belongs in dict.cpp?
extern "C" void dictGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);

    BoxedDict* d = (BoxedDict*)p;

    // This feels like a cludge, but we need to find anything that
    // the unordered_map might have allocated.
    // Another way to handle this would be to rt_alloc the unordered_map
    // as well, though that incurs extra memory dereferences which would
    // be nice to avoid.
    void** start = (void**)&d->d;
    void** end = start + (sizeof(d->d) / 8);
    v->visitPotentialRange(start, end);
}

extern "C" void conservativeGCHandler(GCVisitor* v, void* p) {
    ConservativeWrapper* wrapper = static_cast<ConservativeWrapper*>(p);
    assert(wrapper->gc_header.kind_id == conservative_kind.kind_id);

    int size = wrapper->gc_header.kind_data;
    assert(size % sizeof(void*) == 0);

    void** start = &wrapper->data[0];
    // printf("Found a %d-byte object; header is %p (object is %p)\n", size, p, start);
    v->visitPotentialRange(start, start + (size / sizeof(void*)));
}

extern "C" {
BoxedClass* object_cls, *type_cls, *none_cls, *bool_cls, *int_cls, *float_cls, *str_cls, *function_cls,
    *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls, *file_cls, *member_cls;

const ObjectFlavor object_flavor(&boxGCHandler, NULL);
const ObjectFlavor type_flavor(&typeGCHandler, NULL);
const ObjectFlavor none_flavor(&boxGCHandler, NULL);
const ObjectFlavor bool_flavor(&boxGCHandler, NULL);
const ObjectFlavor int_flavor(&boxGCHandler, NULL);
const ObjectFlavor float_flavor(&boxGCHandler, NULL);
const ObjectFlavor str_flavor(&boxGCHandler, NULL);
const ObjectFlavor function_flavor(&boxGCHandler, NULL);
const ObjectFlavor instancemethod_flavor(&instancemethodGCHandler, NULL);
const ObjectFlavor list_flavor(&listGCHandler, NULL);
const ObjectFlavor slice_flavor(&sliceGCHandler, NULL);
const ObjectFlavor module_flavor(&boxGCHandler, NULL);
const ObjectFlavor dict_flavor(&dictGCHandler, NULL);
const ObjectFlavor tuple_flavor(&tupleGCHandler, NULL);
const ObjectFlavor file_flavor(&boxGCHandler, NULL);
const ObjectFlavor member_flavor(&boxGCHandler, NULL);

const AllocationKind untracked_kind(NULL, NULL);
const AllocationKind hc_kind(&hcGCHandler, NULL);
const AllocationKind conservative_kind(&conservativeGCHandler, NULL);
}

extern "C" Box* createUserClass(std::string* name, Box* _base, BoxedModule* parent_module) {
    assert(_base);
    assert(isSubclass(_base->cls, type_cls));
    BoxedClass* base = static_cast<BoxedClass*>(_base);

    BoxedClass* made;

    if (base->instancesHaveAttrs()) {
        made = new BoxedClass(base, base->attrs_offset, base->instance_size, true);
    } else {
        assert(base->instance_size % sizeof(void*) == 0);
        made = new BoxedClass(base, base->instance_size, base->instance_size + sizeof(HCAttrs), true);
    }

    made->giveAttr("__name__", boxString(*name));

    Box* modname = parent_module->getattr("__name__", NULL, NULL);
    made->giveAttr("__module__", modname);

    return made;
}

extern "C" Box* boxInstanceMethod(Box* obj, Box* func) {
    static StatCounter num_ims("num_instancemethods");
    num_ims.log();

    return new BoxedInstanceMethod(obj, func);
}

extern "C" BoxedString* noneRepr(Box* v) {
    return new BoxedString("None");
}

extern "C" BoxedString* functionRepr(BoxedFunction* v) {
    // TODO there has to be a better way
    if (v == repr_obj)
        return boxStrConstant("<built-in function repr>");
    if (v == len_obj)
        return boxStrConstant("<built-in function len>");
    if (v == hash_obj)
        return boxStrConstant("<built-in function hash>");
    if (v == range_obj)
        return boxStrConstant("<built-in function range>");
    if (v == abs_obj)
        return boxStrConstant("<built-in function abs>");
    if (v == min_obj)
        return boxStrConstant("<built-in function min>");
    if (v == max_obj)
        return boxStrConstant("<built-in function max>");
    if (v == open_obj)
        return boxStrConstant("<built-in function open>");
    if (v == chr_obj)
        return boxStrConstant("<built-in function chr>");
    if (v == ord_obj)
        return boxStrConstant("<built-in function ord>");
    return new BoxedString("function");
}

extern "C" {
Box* None = NULL;
Box* NotImplemented = NULL;
Box* repr_obj = NULL;
Box* len_obj = NULL;
Box* hash_obj = NULL;
Box* abs_obj = NULL;
Box* min_obj = NULL;
Box* max_obj = NULL;
Box* open_obj = NULL;
Box* chr_obj = NULL;
Box* ord_obj = NULL;
Box* trap_obj = NULL;
Box* range_obj = NULL;
}

extern "C" Box* createSlice(Box* start, Box* stop, Box* step) {
    BoxedSlice* rtn = new BoxedSlice(start, stop, step);
    return rtn;
}

extern "C" Box* sliceNew2(Box* cls, Box* stop) {
    assert(cls == slice_cls);
    return createSlice(None, stop, None);
}

extern "C" Box* sliceNew3(Box* cls, Box* start, Box* stop) {
    assert(cls == slice_cls);
    return createSlice(start, stop, None);
}

extern "C" Box* sliceNew4(Box* cls, Box* start, Box* stop, Box** args) {
    assert(cls == slice_cls);
    Box* step = args[0];
    return createSlice(start, stop, step);
}

Box* instancemethodRepr(BoxedInstanceMethod* self) {
    return boxStrConstant("<bound instancemethod object>");
}

Box* sliceRepr(BoxedSlice* self) {
    BoxedString* start = static_cast<BoxedString*>(repr(self->start));
    BoxedString* stop = static_cast<BoxedString*>(repr(self->stop));
    BoxedString* step = static_cast<BoxedString*>(repr(self->step));
    std::string s = "slice(" + start->s + ", " + stop->s + ", " + step->s + ")";
    return new BoxedString(s);
}

Box* typeRepr(BoxedClass* self) {
    if (isUserDefined(self)) {
        std::ostringstream os;
        os << "<class '";

        Box* m = self->getattr("__module__");
        RELEASE_ASSERT(m, "");
        if (m->cls == str_cls) {
            BoxedString* sm = static_cast<BoxedString*>(m);
            os << sm->s << '.';
        }

        Box* n = self->getattr("__name__");
        RELEASE_ASSERT(n, "");
        RELEASE_ASSERT(n->cls == str_cls, "should have prevented you from setting __name__ to non-string");
        BoxedString* sn = static_cast<BoxedString*>(n);
        os << sn->s;

        os << "'>";

        return boxString(os.str());
    } else {
        char buf[80];
        snprintf(buf, 80, "<type '%s'>", getNameOfClass(self)->c_str());
        return boxStrConstant(buf);
    }
}

Box* moduleRepr(BoxedModule* m) {
    assert(m->cls == module_cls);

    std::ostringstream os;
    os << "<module '" << m->name() << "' ";

    if (m->fn == "__builtin__") {
        os << "(built-in)>";
    } else {
        os << "from '" << m->fn << "'>";
    }
    return boxString(os.str());
}

CLFunction* unboxRTFunction(Box* b) {
    assert(b->cls == function_cls);
    return static_cast<BoxedFunction*>(b)->f;
}

Box* objectNew1(BoxedClass* cls) {
    assert(cls->instance_size >= sizeof(Box));
    void* mem = rt_alloc(cls->instance_size);

    Box* rtn = ::new (mem) Box(&object_flavor, cls);
    initUserAttrs(rtn, cls);
    return rtn;
}

Box* objectNew(BoxedClass* cls, BoxedList* args) {
    assert(isSubclass(cls->cls, type_cls));

    if (args->size != 0) {
        if (typeLookup(cls, "__init__", NULL, NULL) == NULL)
            raiseExcHelper(TypeError, "object.__new__() takes no parameters");
    }

    return objectNew1(cls);
}

bool TRACK_ALLOCATIONS = false;
void setupRuntime() {
    HiddenClass::getRoot();

    object_cls = new BoxedClass(NULL, 0, sizeof(Box), false);
    type_cls = new BoxedClass(object_cls, offsetof(BoxedClass, attrs), sizeof(BoxedClass), false);
    type_cls->cls = type_cls;
    object_cls->cls = type_cls;

    none_cls = new BoxedClass(object_cls, 0, sizeof(Box), false);
    None = new Box(&none_flavor, none_cls);

    str_cls = new BoxedClass(object_cls, 0, sizeof(BoxedString), false);

    // It wasn't safe to add __base__ attributes until object+type+str are set up, so do that now:
    type_cls->giveAttr("__base__", object_cls);
    str_cls->giveAttr("__base__", object_cls);
    none_cls->giveAttr("__base__", object_cls);
    object_cls->giveAttr("__base__", None);


    module_cls = new BoxedClass(object_cls, offsetof(BoxedModule, attrs), sizeof(BoxedModule), false);

    // TODO it'd be nice to be able to do these in the respective setupType methods,
    // but those setup methods probably want access to these objects.
    // We could have a multi-stage setup process, but that seems overkill for now.
    bool_cls = new BoxedClass(object_cls, 0, sizeof(BoxedBool), false);
    int_cls = new BoxedClass(object_cls, 0, sizeof(BoxedInt), false);
    float_cls = new BoxedClass(object_cls, 0, sizeof(BoxedFloat), false);
    function_cls = new BoxedClass(object_cls, offsetof(BoxedFunction, attrs), sizeof(BoxedFunction), false);
    instancemethod_cls = new BoxedClass(object_cls, 0, sizeof(BoxedInstanceMethod), false);
    list_cls = new BoxedClass(object_cls, 0, sizeof(BoxedList), false);
    slice_cls = new BoxedClass(object_cls, 0, sizeof(BoxedSlice), false);
    dict_cls = new BoxedClass(object_cls, 0, sizeof(BoxedDict), false);
    tuple_cls = new BoxedClass(object_cls, 0, sizeof(BoxedTuple), false);
    file_cls = new BoxedClass(object_cls, 0, sizeof(BoxedFile), false);
    set_cls = new BoxedClass(object_cls, 0, sizeof(BoxedSet), false);
    member_cls = new BoxedClass(object_cls, 0, sizeof(BoxedMemberDescriptor), false);

    STR = typeFromClass(str_cls);
    BOXED_INT = typeFromClass(int_cls);
    BOXED_FLOAT = typeFromClass(float_cls);
    BOXED_BOOL = typeFromClass(bool_cls);
    NONE = typeFromClass(none_cls);
    LIST = typeFromClass(list_cls);
    SLICE = typeFromClass(slice_cls);
    MODULE = typeFromClass(module_cls);
    DICT = typeFromClass(dict_cls);
    SET = typeFromClass(set_cls);
    BOXED_TUPLE = typeFromClass(tuple_cls);

    object_cls->giveAttr("__name__", boxStrConstant("object"));
    auto object_new = boxRTFunction((void*)objectNew1, NULL, 1, false);
    addRTFunction(object_new, (void*)objectNew, NULL, 1, true);
    object_cls->giveAttr("__new__", new BoxedFunction(object_new));
    object_cls->freeze();

    type_cls->giveAttr("__name__", boxStrConstant("type"));
    type_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)typeCall, NULL, 1, true)));
    type_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)typeNew, NULL, 2, true)));
    type_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)typeRepr, NULL, 1, true)));
    type_cls->giveAttr("__str__", type_cls->getattr("__repr__"));
    type_cls->freeze();

    none_cls->giveAttr("__name__", boxStrConstant("NoneType"));
    none_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)noneRepr, NULL, 1, false)));
    none_cls->giveAttr("__str__", none_cls->getattr("__repr__"));
    none_cls->freeze();

    module_cls->giveAttr("__name__", boxStrConstant("module"));
    module_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)moduleRepr, NULL, 1, false)));
    module_cls->giveAttr("__str__", module_cls->getattr("__repr__"));
    module_cls->freeze();

    member_cls->giveAttr("__name__", boxStrConstant("member"));
    member_cls->freeze();

    setupBool();
    setupInt();
    setupFloat();
    setupStr();
    setupList();
    setupDict();
    setupSet();
    setupTuple();
    setupFile();

    function_cls->giveAttr("__name__", boxStrConstant("function"));
    function_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)functionRepr, NULL, 1, false)));
    function_cls->giveAttr("__str__", function_cls->getattr("__repr__"));
    function_cls->freeze();

    instancemethod_cls->giveAttr("__name__", boxStrConstant("instancemethod"));
    instancemethod_cls->giveAttr("__repr__",
                                 new BoxedFunction(boxRTFunction((void*)instancemethodRepr, NULL, 1, true)));
    instancemethod_cls->freeze();

    slice_cls->giveAttr("__name__", boxStrConstant("slice"));
    CLFunction* slice_new = boxRTFunction((void*)sliceNew2, NULL, 2, false);
    addRTFunction(slice_new, (void*)sliceNew3, NULL, 3, false);
    addRTFunction(slice_new, (void*)sliceNew4, NULL, 4, false);
    slice_cls->giveAttr("__new__", new BoxedFunction(slice_new));
    slice_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)sliceRepr, NULL, 1, true)));
    slice_cls->giveAttr("__str__", slice_cls->getattr("__repr__"));
    slice_cls->giveAttr("start", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_START_OFFSET));
    slice_cls->giveAttr("stop", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_STOP_OFFSET));
    slice_cls->giveAttr("step", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_STEP_OFFSET));
    slice_cls->freeze();

    // sys is the first module that needs to be set up, due to modules
    // being tracked in sys.modules:
    setupSys();

    setupMath();
    setupTime();
    setupBuiltins();

    setupCAPI();

    TRACK_ALLOCATIONS = true;
}

BoxedModule* createModule(const std::string& name, const std::string& fn) {
    assert(fn.size() && "probably wanted to set the fn to <stdin>?");
    BoxedModule* module = new BoxedModule(name, fn);

    BoxedDict* d = getSysModulesDict();
    Box* b_name = boxStringPtr(&name);
    assert(d->d.count(b_name) == 0);
    d->d[b_name] = module;
    return module;
}

void freeHiddenClasses(HiddenClass* hcls) {
    for (const auto& p : hcls->children) {
        freeHiddenClasses(p.second);
    }
    rt_free(hcls);
}

void teardownRuntime() {
    // Things start to become very precarious after this point, as the basic classes stop to work.
    // TODO it's probably a waste of time to tear these down in non-debugging mode
    IN_SHUTDOWN = true;

    if (VERBOSITY("runtime") >= 1)
        printf("In teardownRuntime\n");

    teardownCAPI();

    teardownList();
    teardownInt();
    teardownFloat();
    teardownStr();
    teardownBool();
    teardownDict();
    teardownSet();
    teardownTuple();
    teardownFile();

    /*
    // clear all the attributes on the base classes before freeing the classes themselves,
    // since we will run into problem if we free a class but there is an object somewhere
    // else that refers to it.
    clearAttrs(bool_cls);
    clearAttrs(int_cls);
    clearAttrs(float_cls);
    clearAttrs(none_cls);
    clearAttrs(function_cls);
    clearAttrs(instancemethod_cls);
    clearAttrs(str_cls);
    clearAttrs(list_cls);
    clearAttrs(slice_cls);
    clearAttrs(type_cls);
    clearAttrs(module_cls);
    clearAttrs(dict_cls);
    clearAttrs(tuple_cls);
    clearAttrs(file_cls);

    decref(bool_cls);
    decref(int_cls);
    decref(float_cls);
    decref(function_cls);
    decref(instancemethod_cls);
    decref(str_cls);
    decref(list_cls);
    decref(slice_cls);
    decref(module_cls);
    decref(dict_cls);
    decref(tuple_cls);
    decref(file_cls);

    ASSERT(None->nrefs == 1, "%ld", None->nrefs);
    decref(None);

    decref(none_cls);
    decref(type_cls);
    */

    freeHiddenClasses(HiddenClass::getRoot());

    gc_teardown();
}
}
