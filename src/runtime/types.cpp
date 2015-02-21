// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.  // You may obtain a copy of the License at
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

#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/capi.h"
#include "runtime/classobj.h"
#include "runtime/file.h"
#include "runtime/ics.h"
#include "runtime/iterobject.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/super.h"
#include "runtime/traceback.h"
#include "runtime/util.h"

extern "C" void initerrno();
extern "C" void init_sha();
extern "C" void init_sha256();
extern "C" void init_sha512();
extern "C" void init_md5();
extern "C" void init_random();
extern "C" void init_sre();
extern "C" void initmath();
extern "C" void initoperator();
extern "C" void initbinascii();
extern "C" void initpwd();
extern "C" void initposix();
extern "C" void init_struct();
extern "C" void initdatetime();
extern "C" void init_functools();
extern "C" void init_collections();
extern "C" void inititertools();
extern "C" void initresource();
extern "C" void initsignal();
extern "C" void initselect();
extern "C" void initfcntl();
extern "C" void inittime();
extern "C" void initarray();
extern "C" void initzlib();
extern "C" void init_codecs();

namespace pyston {

void setupGC();

bool IN_SHUTDOWN = false;

#define SLICE_START_OFFSET ((char*)&(((BoxedSlice*)0x01)->start) - (char*)0x1)
#define SLICE_STOP_OFFSET ((char*)&(((BoxedSlice*)0x01)->stop) - (char*)0x1)
#define SLICE_STEP_OFFSET ((char*)&(((BoxedSlice*)0x01)->step) - (char*)0x1)

// Analogue of PyType_GenericAlloc (default tp_alloc), but should only be used for Pyston classes!
PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept {
    assert(cls);
    RELEASE_ASSERT(nitems == 0, "");
    RELEASE_ASSERT(cls->tp_itemsize == 0, "");

    const size_t size = cls->tp_basicsize;

#ifndef NDEBUG
#if 0
    assert(cls->tp_bases);
    // TODO this should iterate over all subclasses
    for (auto e : cls->tp_bases->pyElements()) {
        assert(e->cls == type_cls); // what about old style classes?
        assert(static_cast<BoxedClass*>(e)->is_pyston_class);
    }
#endif
    BoxedClass* b = cls;
    while (b) {
        ASSERT(b->is_pyston_class, "%s (%s)", cls->tp_name, b->tp_name);
        b = b->tp_base;
    }
#endif

    // Maybe we should only zero the extension memory?
    // I'm not sure we have the information at the moment, but when we were in Box::operator new()
    // we knew which memory was beyond C++ class.
    void* mem = gc_alloc(size, gc::GCKind::PYTHON);
    RELEASE_ASSERT(mem, "");

    Box* rtn = static_cast<Box*>(mem);

    PyObject_Init(rtn, cls);
    assert(rtn->cls);

    return rtn;
}

extern "C" PyObject* PyType_GenericAlloc(PyTypeObject* type, Py_ssize_t nitems) noexcept {
    PyObject* obj;
    const size_t size = _PyObject_VAR_SIZE(type, nitems + 1);
    /* note that we need to add one, for the sentinel */

    if (PyType_IS_GC(type))
        obj = _PyObject_GC_Malloc(size);
    else
        obj = (PyObject*)PyObject_MALLOC(size);

    if (obj == NULL)
        return PyErr_NoMemory();

    memset(obj, '\0', size);

    if (type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        Py_INCREF(type);

    if (type->tp_itemsize == 0)
        PyObject_INIT(obj, type);
    else
        (void)PyObject_INIT_VAR((PyVarObject*)obj, type, nitems);

    if (PyType_IS_GC(type))
        _PyObject_GC_TRACK(obj);
    return obj;
}

extern "C" PyObject* _PyObject_New(PyTypeObject* tp) noexcept {
    PyObject* op;
    op = (PyObject*)PyObject_MALLOC(_PyObject_SIZE(tp));
    if (op == NULL)
        return PyErr_NoMemory();
    return PyObject_INIT(op, tp);
}

// Analogue of PyType_GenericNew
void* Box::operator new(size_t size, BoxedClass* cls) {
    assert(cls);
    ASSERT(cls->tp_basicsize >= size, "%s", cls->tp_name);
    assert(cls->tp_alloc);

    void* mem = cls->tp_alloc(cls, 0);
    RELEASE_ASSERT(mem, "");
    return mem;
}

Box* BoxedClass::callHasnextIC(Box* obj, bool null_on_nonexistent) {
    assert(obj->cls == this);

    auto ic = hasnext_ic.get();
    if (!ic) {
        ic = new CallattrIC();
        hasnext_ic.reset(ic);
    }

    static std::string hasnext_str("__hasnext__");
    return ic->call(obj, &hasnext_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = null_on_nonexistent }),
                    ArgPassSpec(0), nullptr, nullptr, nullptr, nullptr, nullptr);
}

Box* BoxedClass::callNextIC(Box* obj) {
    assert(obj->cls == this);

    auto ic = next_ic.get();
    if (!ic) {
        ic = new CallattrIC();
        next_ic.reset(ic);
    }

    static std::string next_str("next");
    return ic->call(obj, &next_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = false }), ArgPassSpec(0),
                    nullptr, nullptr, nullptr, nullptr, nullptr);
}

Box* BoxedClass::callReprIC(Box* obj) {
    assert(obj->cls == this);

    auto ic = repr_ic.get();
    if (!ic) {
        ic = new CallattrIC();
        repr_ic.reset(ic);
    }

    static std::string repr_str("__repr__");
    return ic->call(obj, &repr_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = false }), ArgPassSpec(0),
                    nullptr, nullptr, nullptr, nullptr, nullptr);
}

bool BoxedClass::callNonzeroIC(Box* obj) {
    assert(obj->cls == this);

    auto ic = nonzero_ic.get();
    if (!ic) {
        ic = new NonzeroIC();
        nonzero_ic.reset(ic);
    }

    return ic->call(obj);
}

Box* Box::reprIC() {
    return this->cls->callReprIC(this);
}

BoxedString* Box::reprICAsString() {
    Box* r = this->reprIC();
    if (r->cls != str_cls) {
        raiseExcHelper(TypeError, "__repr__ did not return a string!");
    }
    return static_cast<BoxedString*>(r);
}

bool Box::nonzeroIC() {
    return this->cls->callNonzeroIC(this);
}

Box* Box::hasnextOrNullIC() {
    return this->cls->callHasnextIC(this, true);
}

Box* Box::nextIC() {
    return this->cls->callNextIC(this);
}

BoxIterator& BoxIterator::operator++() {
    static std::string next_str("next");

    assert(iter);

    Box* hasnext = iter->hasnextOrNullIC();
    if (hasnext) {
        if (hasnext->nonzeroIC()) {
            value = iter->nextIC();
        } else {
            iter = nullptr;
            value = nullptr;
        }
    } else {
        try {
            value = iter->nextIC();
        } catch (ExcInfo e) {
            if (e.matches(StopIteration)) {
                iter = nullptr;
                value = nullptr;
            } else
                throw e;
        }
    }
    return *this;
}

void BoxIterator::gcHandler(GCVisitor* v) {
    v->visitPotential(iter);
    v->visitPotential(value);
}

std::string builtinStr("__builtin__");

extern "C" BoxedFunctionBase::BoxedFunctionBase(CLFunction* f)
    : f(f), closure(NULL), isGenerator(false), ndefaults(0), defaults(NULL), modname(NULL), name(NULL) {
    if (f->source) {
        this->modname = f->source->parent_module->getattr("__name__", NULL);
    } else {
        this->modname = boxStringPtr(&builtinStr);
    }

    this->giveAttr("__doc__", None);

    assert(f->num_defaults == ndefaults);
}

extern "C" BoxedFunctionBase::BoxedFunctionBase(CLFunction* f, std::initializer_list<Box*> defaults,
                                                BoxedClosure* closure, bool isGenerator)
    : f(f), closure(closure), isGenerator(isGenerator), ndefaults(0), defaults(NULL), modname(NULL), name(NULL) {
    if (defaults.size()) {
        // make sure to initialize defaults first, since the GC behavior is triggered by ndefaults,
        // and a GC can happen within this constructor:
        this->defaults = new (defaults.size()) GCdArray();
        memcpy(this->defaults->elts, defaults.begin(), defaults.size() * sizeof(Box*));
        this->ndefaults = defaults.size();
    }

    if (f->source) {
        this->modname = f->source->parent_module->getattr("__name__", NULL);
    } else {
        this->modname = boxStringPtr(&builtinStr);
    }

    this->giveAttr("__doc__", None);

    assert(f->num_defaults == ndefaults);
}

BoxedFunction::BoxedFunction(CLFunction* f) : BoxedFunction(f, {}) {
}

BoxedFunction::BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure,
                             bool isGenerator)
    : BoxedFunctionBase(f, defaults, closure, isGenerator) {

    // TODO eventually we want this to assert(f->source), I think, but there are still
    // some builtin functions that are BoxedFunctions but really ought to be a type that
    // we don't have yet.
    if (f->source) {
        this->name = static_cast<BoxedString*>(boxString(f->source->getName()));
    }
}

extern "C" void functionGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedFunction* f = (BoxedFunction*)b;

    // TODO eventually f->name should always be non-NULL, then there'd be no need for this check
    if (f->name)
        v->visit(f->name);

    if (f->modname)
        v->visit(f->modname);

    if (f->closure)
        v->visit(f->closure);

    // It's ok for f->defaults to be NULL here even if f->ndefaults isn't,
    // since we could be collecting from inside a BoxedFunctionBase constructor
    if (f->ndefaults) {
        assert(f->defaults);
        v->visit(f->defaults);
        // do a conservative scan since there can be NULLs in there:
        v->visitPotentialRange(reinterpret_cast<void* const*>(&f->defaults->elts[0]),
                               reinterpret_cast<void* const*>(&f->defaults->elts[f->ndefaults]));
    }
}

static void functionDtor(Box* b) {
    assert(isSubclass(b->cls, function_cls) || isSubclass(b->cls, builtin_function_or_method_cls));

    BoxedFunctionBase* self = static_cast<BoxedFunctionBase*>(b);
    self->dependent_ics.invalidateAll();
    self->dependent_ics.~ICInvalidator();
}

BoxedModule::BoxedModule(const std::string& name, const std::string& fn) : fn(fn) {
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

extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, bool isGenerator,
                              std::initializer_list<Box*> defaults) {
    if (closure)
        assert(closure->cls == closure_cls);

    return new BoxedFunction(f, defaults, closure, isGenerator);
}

extern "C" CLFunction* unboxCLFunction(Box* b) {
    return static_cast<BoxedFunction*>(b)->f;
}

extern "C" void boxGCHandler(GCVisitor* v, Box* b) {
    if (b->cls) {
        v->visit(b->cls);

        if (b->cls->instancesHaveHCAttrs()) {
            HCAttrs* attrs = b->getHCAttrsPtr();

            v->visit(attrs->hcls);
            int nattrs = attrs->hcls->attr_offsets.size();
            if (nattrs) {
                HCAttrs::AttrList* attr_list = attrs->attr_list;
                assert(attr_list);
                v->visit(attr_list);
                v->visitRange((void**)&attr_list->attrs[0], (void**)&attr_list->attrs[nattrs]);
            }
        }

        if (b->cls->instancesHaveDictAttrs()) {
            RELEASE_ASSERT(0, "Shouldn't all of these objects be conservatively scanned?");
        }
    } else {
        assert(type_cls == NULL || b == type_cls);
    }
}

extern "C" void typeGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedClass* cls = (BoxedClass*)b;

    if (cls->tp_base)
        v->visit(cls->tp_base);
    if (cls->tp_dict)
        v->visit(cls->tp_dict);

    if (cls->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        BoxedHeapClass* hcls = static_cast<BoxedHeapClass*>(cls);
        assert(hcls->ht_name);
        v->visit(hcls->ht_name);
    }
}

extern "C" void instancemethodGCHandler(GCVisitor* v, Box* b) {
    BoxedInstanceMethod* im = (BoxedInstanceMethod*)b;

    if (im->obj) {
        v->visit(im->obj);
    }
    v->visit(im->func);
}

extern "C" void propertyGCHandler(GCVisitor* v, Box* b) {
    BoxedProperty* prop = (BoxedProperty*)b;

    if (prop->prop_get)
        v->visit(prop->prop_get);
    if (prop->prop_set)
        v->visit(prop->prop_set);
    if (prop->prop_del)
        v->visit(prop->prop_del);
    if (prop->prop_doc)
        v->visit(prop->prop_doc);
}

extern "C" void staticmethodGCHandler(GCVisitor* v, Box* b) {
    BoxedStaticmethod* sm = (BoxedStaticmethod*)b;

    if (sm->sm_callable)
        v->visit(sm->sm_callable);
}

extern "C" void classmethodGCHandler(GCVisitor* v, Box* b) {
    BoxedClassmethod* cm = (BoxedClassmethod*)b;

    if (cm->cm_callable)
        v->visit(cm->cm_callable);
}

// This probably belongs in list.cpp?
extern "C" void listGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedList* l = (BoxedList*)b;
    int size = l->size;
    int capacity = l->capacity;
    assert(capacity >= size);
    if (capacity)
        v->visit(l->elts);
    if (size)
        v->visitRange((void**)&l->elts->elts[0], (void**)&l->elts->elts[size]);
}

extern "C" void setGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedSet* s = (BoxedSet*)b;

    // This feels like a cludge, but we need to find anything that
    // the unordered_map might have allocated.
    // Another way to handle this would be to rt_alloc the unordered_map
    // as well, though that incurs extra memory dereferences which would
    // be nice to avoid.
    void** start = (void**)&s->s;
    void** end = start + (sizeof(s->s) / 8);
    v->visitPotentialRange(start, end);
}

extern "C" void sliceGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedSlice* sl = static_cast<BoxedSlice*>(b);
    assert(sl->cls == slice_cls);

    v->visit(sl->start);
    v->visit(sl->stop);
    v->visit(sl->step);
}

// This probably belongs in tuple.cpp?
extern "C" void tupleGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedTuple* t = (BoxedTuple*)b;
    v->visitPotentialRange((void* const*)&t->elts, (void* const*)(&t->elts + 1));
}

// This probably belongs in dict.cpp?
extern "C" void dictGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedDict* d = (BoxedDict*)b;

    // This feels like a cludge, but we need to find anything that
    // the unordered_map might have allocated.
    // Another way to handle this would be to rt_alloc the unordered_map
    // as well, though that incurs extra memory dereferences which would
    // be nice to avoid.
    void** start = (void**)&d->d;
    void** end = start + (sizeof(d->d) / 8);
    v->visitPotentialRange(start, end);
}

extern "C" void closureGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedClosure* c = (BoxedClosure*)b;
    if (c->parent)
        v->visit(c->parent);
}

extern "C" {
BoxedClass* object_cls, *type_cls, *none_cls, *bool_cls, *int_cls, *float_cls,
    * str_cls = NULL, *function_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls,
      *file_cls, *member_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls, *unicode_cls, *property_cls,
      *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *getset_cls, *builtin_function_or_method_cls;

BoxedTuple* EmptyTuple;
}

extern "C" Box* createUserClass(const std::string* name, Box* _bases, Box* _attr_dict) {
    ASSERT(_attr_dict->cls == dict_cls, "%s", getTypeName(_attr_dict));
    BoxedDict* attr_dict = static_cast<BoxedDict*>(_attr_dict);

    assert(_bases->cls == tuple_cls);
    BoxedTuple* bases = static_cast<BoxedTuple*>(_bases);

    Box* metaclass = NULL;
    metaclass = attr_dict->getOrNull(boxStrConstant("__metaclass__"));

    if (metaclass != NULL) {
    } else if (bases->elts.size() > 0) {
        // TODO Apparently this is supposed to look up __class__, and if that throws
        // an error, then look up ob_type (aka cls)
        metaclass = bases->elts[0]->cls;
    } else {
        BoxedModule* m = getCurrentModule();
        metaclass = m->getattr("__metaclass__");

        if (!metaclass) {
            metaclass = classobj_cls;
        }
    }
    assert(metaclass);

    try {
        Box* r = runtimeCall(metaclass, ArgPassSpec(3), boxStringPtr(name), _bases, _attr_dict, NULL, NULL);
        RELEASE_ASSERT(r, "");
        return r;
    } catch (ExcInfo e) {
        // TODO [CAPI] bad error handling...

        RELEASE_ASSERT(e.matches(BaseException), "");

        Box* msg = e.value->getattr("message");
        RELEASE_ASSERT(msg, "");
        RELEASE_ASSERT(msg->cls == str_cls, "");

        PyObject* newmsg;
        newmsg = PyString_FromFormat("Error when calling the metaclass bases\n"
                                     "    %s",
                                     PyString_AS_STRING(msg));

        PyErr_Restore(e.type, newmsg, NULL);
        checkAndThrowCAPIException();

        // Should not reach here
        abort();
    }
}

extern "C" Box* boxInstanceMethod(Box* obj, Box* func) {
    static StatCounter num_ims("num_instancemethods");
    num_ims.log();

    return new BoxedInstanceMethod(obj, func);
}

extern "C" Box* boxUnboundInstanceMethod(Box* func) {
    return new BoxedInstanceMethod(NULL, func);
}

extern "C" BoxedString* noneRepr(Box* v) {
    return new BoxedString("None");
}

extern "C" Box* noneHash(Box* v) {
    return boxInt(819239); // chosen randomly
}

extern "C" Box* noneNonzero(Box* v) {
    return False;
}

extern "C" BoxedString* builtinFunctionOrMethodRepr(BoxedBuiltinFunctionOrMethod* v) {
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
    if (v == id_obj)
        return boxStrConstant("<built-in function id>");
    if (v == chr_obj)
        return boxStrConstant("<built-in function chr>");
    if (v == ord_obj)
        return boxStrConstant("<built-in function ord>");
    RELEASE_ASSERT(false, "builtinFunctionOrMethodRepr not properly implemented");
}

extern "C" BoxedString* functionRepr(BoxedFunction* v) {
    return new BoxedString("function");
}

static Box* functionGet(BoxedFunction* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == function_cls, "");

    if (inst == None)
        return boxUnboundInstanceMethod(self);
    return boxInstanceMethod(inst, self);
}

static Box* functionCall(BoxedFunction* self, Box* args, Box* kwargs) {
    RELEASE_ASSERT(self->cls == function_cls, "%s", getTypeName(self));

    // This won't work if you subclass from function_cls, since runtimeCall will
    // just call back into this function.
    // Fortunately, CPython disallows subclassing FunctionType; we don't currently
    // disallow it but it's good to know.

    assert(args->cls == tuple_cls);
    assert(kwargs->cls == dict_cls);
    return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwargs, NULL, NULL, NULL);
}

static Box* func_name(Box* b, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);
    RELEASE_ASSERT(func->name != NULL, "func->name is not set");
    return func->name;
}

static void func_set_name(Box* b, Box* v, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);

    if (v == NULL || !PyString_Check(v)) {
        raiseExcHelper(TypeError, "__name__ must be set to a string object");
    }

    func->name = static_cast<BoxedString*>(v);
}

static Box* functionNonzero(BoxedFunction* self) {
    return True;
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
Box* id_obj = NULL;
Box* chr_obj = NULL;
Box* ord_obj = NULL;
Box* trap_obj = NULL;
Box* range_obj = NULL;
}

HiddenClass* root_hcls;

extern "C" Box* createSlice(Box* start, Box* stop, Box* step) {
    BoxedSlice* rtn = new BoxedSlice(start, stop, step);
    return rtn;
}

extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure) {
    if (parent_closure)
        assert(parent_closure->cls == closure_cls);
    return new BoxedClosure(parent_closure);
}

extern "C" Box* sliceNew(Box* cls, Box* start, Box* stop, Box** args) {
    RELEASE_ASSERT(cls == slice_cls, "");
    Box* step = args[0];

    if (stop == NULL)
        return createSlice(None, start, None);
    return createSlice(start, stop, step);
}

static Box* instancemethodCall(BoxedInstanceMethod* self, Box* args, Box* kwargs) {
    RELEASE_ASSERT(self->cls == instancemethod_cls, "");
    Py_FatalError("unimplemented");
}

Box* instancemethodGet(BoxedInstanceMethod* self, Box* obj, Box* type) {
    RELEASE_ASSERT(self->cls == instancemethod_cls, "");

    if (self->obj != NULL) {
        return self;
    }

    // TODO subclass test

    return new BoxedInstanceMethod(obj, self->func);
}

Box* instancemethodRepr(BoxedInstanceMethod* self) {
    if (self->obj)
        return boxStrConstant("<bound instancemethod object>");
    else
        return boxStrConstant("<unbound instancemethod object>");
}

Box* instancemethodEq(BoxedInstanceMethod* self, Box* rhs) {
    if (rhs->cls != instancemethod_cls) {
        return boxBool(false);
    }

    BoxedInstanceMethod* rhs_im = static_cast<BoxedInstanceMethod*>(rhs);
    if (self->func == rhs_im->func) {
        if (self->obj == NULL && rhs_im->obj == NULL) {
            return boxBool(true);
        } else {
            if (self->obj != NULL && rhs_im->obj != NULL) {
                return compareInternal(self->obj, rhs_im->obj, AST_TYPE::Eq, NULL);
            } else {
                return boxBool(false);
            }
        }
    } else {
        return boxBool(false);
    }
}

Box* sliceRepr(BoxedSlice* self) {
    BoxedString* start = static_cast<BoxedString*>(repr(self->start));
    BoxedString* stop = static_cast<BoxedString*>(repr(self->stop));
    BoxedString* step = static_cast<BoxedString*>(repr(self->step));
    std::string s = "slice(" + start->s + ", " + stop->s + ", " + step->s + ")";
    return new BoxedString(std::move(s));
}

extern "C" int PySlice_GetIndices(PySliceObject* r, Py_ssize_t length, Py_ssize_t* start, Py_ssize_t* stop,
                                  Py_ssize_t* step) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" int PySlice_GetIndicesEx(PySliceObject* _r, Py_ssize_t length, Py_ssize_t* start, Py_ssize_t* stop,
                                    Py_ssize_t* step, Py_ssize_t* slicelength) noexcept {
    try {
        BoxedSlice* r = (BoxedSlice*)_r;
        assert(r->cls == slice_cls);
        // parseSlice has the exact same behaviour as CPythons PySlice_GetIndicesEx apart from throwing c++ exceptions
        // on error.
        parseSlice(r, length, start, stop, step, slicelength);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

Box* typeRepr(BoxedClass* self) {
    std::ostringstream os;
    if ((self->tp_flags & Py_TPFLAGS_HEAPTYPE) && isUserDefined(self))
        os << "<class '";
    else
        os << "<type '";

    Box* m = self->getattr("__module__");
    if (m && m->cls == str_cls) {
        BoxedString* sm = static_cast<BoxedString*>(m);
        os << sm->s << '.';
    }

    os << self->tp_name;

    os << "'>";

    return boxString(os.str());
}

Box* typeHash(BoxedClass* self) {
    assert(isSubclass(self->cls, type_cls));

    // This is how CPython defines it; seems reasonable enough:
    return boxInt(reinterpret_cast<intptr_t>(self) >> 4);
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

// A dictionary-like wrapper around the attributes array.
// Not sure if this will be enough to satisfy users who expect __dict__
// or PyModule_GetDict to return real dicts.
class AttrWrapper : public Box {
private:
    Box* b;

public:
    AttrWrapper(Box* b) : b(b) { assert(b->cls->instancesHaveHCAttrs()); }

    DEFAULT_CLASS(attrwrapper_cls);

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        AttrWrapper* aw = (AttrWrapper*)b;
        v->visit(aw->b);
    }

    static Box* setitem(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        self->b->setattr(key->s, value, NULL);
        return None;
    }

    static Box* get(Box* _self, Box* _key, Box* def) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* r = self->b->getattr(key->s);
        if (!r)
            return def;
        return r;
    }

    static Box* getitem(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* r = self->b->getattr(key->s);
        if (!r) {
            raiseExcHelper(KeyError, "'%s'", key->s.c_str());
        }
        return r;
    }

    static Box* str(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        std::ostringstream os("");
        os << "attrwrapper({";

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        bool first = true;
        for (const auto& p : attrs->hcls->attr_offsets) {
            if (!first)
                os << ", ";
            first = false;

            BoxedString* v = attrs->attr_list->attrs[p.second]->reprICAsString();
            os << p.first << ": " << v->s;
        }
        os << "})";
        return boxString(os.str());
    }

    static Box* contains(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* r = self->b->getattr(key->s);
        return r ? True : False;
    }

    static Box* keys(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        for (const auto& p : attrs->hcls->attr_offsets) {
            listAppend(rtn, boxString(p.first));
        }
        return rtn;
    }

    static Box* values(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        for (const auto& p : attrs->hcls->attr_offsets) {
            listAppend(rtn, attrs->attr_list->attrs[p.second]);
        }
        return rtn;
    }

    static Box* items(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        for (const auto& p : attrs->hcls->attr_offsets) {
            BoxedTuple* t = new BoxedTuple({ boxString(p.first), attrs->attr_list->attrs[p.second] });
            listAppend(rtn, t);
        }
        return rtn;
    }

    static Box* len(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        return boxInt(attrs->hcls->attr_offsets.size());
    }

    static Box* update(Box* _self, Box* _container) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_container->cls == attrwrapper_cls) {
            AttrWrapper* container = static_cast<AttrWrapper*>(_container);
            HCAttrs* attrs = container->b->getHCAttrsPtr();

            for (const auto& p : attrs->hcls->attr_offsets) {
                self->b->setattr(p.first, attrs->attr_list->attrs[p.second], NULL);
            }
        } else {
            RELEASE_ASSERT(0, "not implemented");
        }
        return None;
    }
};

Box* makeAttrWrapper(Box* b) {
    assert(b->cls->instancesHaveHCAttrs());
    return new AttrWrapper(b);
}

Box* objectNewNoArgs(BoxedClass* cls) {
    assert(isSubclass(cls->cls, type_cls));
    assert(typeLookup(cls, "__new__", NULL) == typeLookup(object_cls, "__new__", NULL)
           && typeLookup(cls, "__init__", NULL) != typeLookup(object_cls, "__init__", NULL));
    return new (cls) Box();
}

Box* objectNew(BoxedClass* cls, BoxedTuple* args, BoxedDict* kwargs) {
    assert(isSubclass(cls->cls, type_cls));
    assert(args->cls == tuple_cls);
    assert(kwargs->cls == dict_cls);

    // We use a different strategy from CPython: we let object.__new__ take extra
    // arguments, but raise an error if they wouldn't be handled by the corresponding init.
    // TODO switch to the CPython approach?
    if (args->elts.size() != 0 || kwargs->d.size() != 0) {
        // TODO slow (We already cache these in typeCall -- should use that here too?)
        if (typeLookup(cls, "__new__", NULL) != typeLookup(object_cls, "__new__", NULL)
            || typeLookup(cls, "__init__", NULL) == typeLookup(object_cls, "__init__", NULL))
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
    }

    return new (cls) Box();
}

Box* objectInit(Box* b, BoxedTuple* args) {
    return None;
}

Box* objectRepr(Box* obj) {
    char buf[80];
    if (obj->cls == type_cls) {
        snprintf(buf, 80, "<type '%s'>", getNameOfClass(static_cast<BoxedClass*>(obj)));
    } else {
        snprintf(buf, 80, "<%s object at %p>", getTypeName(obj), obj);
    }
    return boxStrConstant(buf);
}

Box* objectStr(Box* obj) {
    return obj->reprIC();
}

static Box* type_name(Box* b, void*) {
    assert(b->cls == type_cls);
    BoxedClass* type = static_cast<BoxedClass*>(b);

    // TODO is this predicate right?
    if (type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        BoxedHeapClass* et = static_cast<BoxedHeapClass*>(type);
        return et->ht_name;
    } else {
        const char* s = strrchr(type->tp_name, '.');
        if (s == NULL)
            s = type->tp_name;
        else
            s++;
        return PyString_FromString(s);
    }
}

static void type_set_name(Box* b, Box* v, void*) {
    assert(b->cls == type_cls);
    BoxedClass* type = static_cast<BoxedClass*>(b);

    // Awkward... in CPython you can only set __name__ for heaptype classes
    // (those with ht_name) but in Pyston right now we have some heaptype classes that
    // aren't heaptype in CPython, and we have to restrict those too.

    // TODO is this predicate right?
    bool is_heaptype = (type->tp_flags & Py_TPFLAGS_HEAPTYPE);
    if (!(is_heaptype && type->is_user_defined)) {
        raiseExcHelper(TypeError, "can't set %s.__name__", type->tp_name);
    }
    if (!v) {
        raiseExcHelper(TypeError, "can't delete %s.__name__", type->tp_name);
    }
    if (!PyString_Check(v)) {
        raiseExcHelper(TypeError, "can only assign string to %s.__name__, not '%s'", type->tp_name, getTypeName(v));
    }

    BoxedString* s = static_cast<BoxedString*>(v);
    if (strlen(s->s.c_str()) != s->s.size()) {
        raiseExcHelper(ValueError, "__name__ must not contain null bytes");
    }

    BoxedHeapClass* ht = static_cast<BoxedHeapClass*>(type);
    ht->ht_name = s;
    ht->tp_name = s->s.c_str();
}

// cls should be obj->cls.
// Added as parameter because it should typically be available
inline void initUserAttrs(Box* obj, BoxedClass* cls) {
    assert(obj->cls == cls);
    if (cls->instancesHaveHCAttrs()) {
        HCAttrs* attrs = obj->getHCAttrsPtr();
        attrs = new ((void*)attrs) HCAttrs();
    }
}

extern "C" PyObject* PyObject_Init(PyObject* op, PyTypeObject* tp) noexcept {
    assert(op);
    assert(tp);

    assert(gc::isValidGCObject(op));
    assert(gc::isValidGCObject(tp));

    Py_TYPE(op) = tp;

    // I think CPython defers the dict creation (equivalent of our initUserAttrs) to the
    // first time that an attribute gets set.
    // Our HCAttrs object already includes this optimization of no-allocation-if-empty,
    // but it's nice to initialize the hcls here so we don't have to check it on every getattr/setattr.
    // TODO It does mean that anything not defering to this function will have to call
    // initUserAttrs themselves, though.
    initUserAttrs(op, tp);

    return op;
}

bool TRACK_ALLOCATIONS = false;
void setupRuntime() {
    root_hcls = HiddenClass::makeRoot();
    gc::registerPermanentRoot(root_hcls);

    // Disable the GC while we do some manual initialization of the object hierarchy:
    gc::disableGC();

    // We have to do a little dance to get object_cls and type_cls set up, since the normal
    // object-creation routines look at the class to see the allocation size.
    void* mem = gc_alloc(sizeof(BoxedClass), gc::GCKind::PYTHON);
    object_cls = ::new (mem) BoxedClass(NULL, &boxGCHandler, 0, sizeof(Box), false);
    mem = gc_alloc(sizeof(BoxedHeapClass), gc::GCKind::PYTHON);
    type_cls = ::new (mem)
        BoxedHeapClass(object_cls, &typeGCHandler, offsetof(BoxedClass, attrs), sizeof(BoxedHeapClass), false);
    PyObject_Init(object_cls, type_cls);
    PyObject_Init(type_cls, type_cls);

    none_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(Box), false);
    None = new (none_cls) Box();
    assert(None->cls);
    gc::registerPermanentRoot(None);

    // You can't actually have an instance of basestring
    basestring_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(Box), false);

    // TODO we leak all the string data!
    str_cls = new BoxedHeapClass(basestring_cls, NULL, 0, sizeof(BoxedString), false);

    // Hold off on assigning names until str_cls is ready
    object_cls->tp_name = "object";
    BoxedString* boxed_type_name = new BoxedString("type");
    BoxedString* boxed_basestring_name = new BoxedString("basestring");
    BoxedString* boxed_str_name = new BoxedString("str");
    BoxedString* boxed_none_name = new BoxedString("NoneType");
    static_cast<BoxedHeapClass*>(type_cls)->ht_name = boxed_type_name;
    static_cast<BoxedHeapClass*>(basestring_cls)->ht_name = boxed_basestring_name;
    static_cast<BoxedHeapClass*>(str_cls)->ht_name = boxed_str_name;
    static_cast<BoxedHeapClass*>(none_cls)->ht_name = boxed_none_name;
    type_cls->tp_name = boxed_type_name->s.c_str();
    basestring_cls->tp_name = boxed_basestring_name->s.c_str();
    str_cls->tp_name = boxed_str_name->s.c_str();
    none_cls->tp_name = boxed_none_name->s.c_str();

    gc::enableGC();

    unicode_cls = new BoxedHeapClass(basestring_cls, NULL, 0, sizeof(BoxedUnicode), false, "unicode");

    // It wasn't safe to add __base__ attributes until object+type+str are set up, so do that now:
    type_cls->giveAttr("__base__", object_cls);
    basestring_cls->giveAttr("__base__", object_cls);
    str_cls->giveAttr("__base__", basestring_cls);
    none_cls->giveAttr("__base__", object_cls);
    object_cls->giveAttr("__base__", None);


    tuple_cls = new BoxedHeapClass(object_cls, &tupleGCHandler, 0, sizeof(BoxedTuple), false, "tuple");
    EmptyTuple = new BoxedTuple({});
    gc::registerPermanentRoot(EmptyTuple);


    module_cls
        = new BoxedHeapClass(object_cls, NULL, offsetof(BoxedModule, attrs), sizeof(BoxedModule), false, "module");

    // TODO it'd be nice to be able to do these in the respective setupType methods,
    // but those setup methods probably want access to these objects.
    // We could have a multi-stage setup process, but that seems overkill for now.
    int_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedInt), false, "int");
    bool_cls = new BoxedHeapClass(int_cls, NULL, 0, sizeof(BoxedBool), false, "bool");
    complex_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedComplex), false, "complex");
    long_cls = new BoxedHeapClass(object_cls, &BoxedLong::gchandler, 0, sizeof(BoxedLong), false, "long");
    float_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedFloat), false, "float");
    function_cls = new BoxedHeapClass(object_cls, &functionGCHandler, offsetof(BoxedFunction, attrs),
                                      sizeof(BoxedFunction), false, "function");
    builtin_function_or_method_cls
        = new BoxedHeapClass(object_cls, &functionGCHandler, offsetof(BoxedBuiltinFunctionOrMethod, attrs),
                             sizeof(BoxedBuiltinFunctionOrMethod), false, "builtin_function_or_method");
    function_cls->simple_destructor = builtin_function_or_method_cls->simple_destructor = functionDtor;

    instancemethod_cls = new BoxedHeapClass(object_cls, &instancemethodGCHandler, 0, sizeof(BoxedInstanceMethod), false,
                                            "instancemethod");
    list_cls = new BoxedHeapClass(object_cls, &listGCHandler, 0, sizeof(BoxedList), false, "list");
    slice_cls = new BoxedHeapClass(object_cls, &sliceGCHandler, 0, sizeof(BoxedSlice), false, "slice");
    dict_cls = new BoxedHeapClass(object_cls, &dictGCHandler, 0, sizeof(BoxedDict), false, "dict");
    file_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedFile), false, "file");
    set_cls = new BoxedHeapClass(object_cls, &setGCHandler, 0, sizeof(BoxedSet), false, "set");
    frozenset_cls = new BoxedHeapClass(object_cls, &setGCHandler, 0, sizeof(BoxedSet), false, "frozenset");
    member_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedMemberDescriptor), false, "member");
    getset_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(BoxedGetsetDescriptor), false, "getset");
    closure_cls = new BoxedHeapClass(object_cls, &closureGCHandler, offsetof(BoxedClosure, attrs), sizeof(BoxedClosure),
                                     false, "closure");
    property_cls = new BoxedHeapClass(object_cls, &propertyGCHandler, 0, sizeof(BoxedProperty), false, "property");
    staticmethod_cls
        = new BoxedHeapClass(object_cls, &staticmethodGCHandler, 0, sizeof(BoxedStaticmethod), false, "staticmethod");
    classmethod_cls
        = new BoxedHeapClass(object_cls, &classmethodGCHandler, 0, sizeof(BoxedClassmethod), false, "classmethod");
    attrwrapper_cls
        = new BoxedHeapClass(object_cls, &AttrWrapper::gcHandler, 0, sizeof(AttrWrapper), false, "attrwrapper");

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
    FROZENSET = typeFromClass(frozenset_cls);
    BOXED_TUPLE = typeFromClass(tuple_cls);
    LONG = typeFromClass(long_cls);
    BOXED_COMPLEX = typeFromClass(complex_cls);

    object_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)objectNew, UNKNOWN, 1, 0, true, true)));
    object_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)objectInit, UNKNOWN, 1, 0, true, false)));
    object_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)objectRepr, UNKNOWN, 1, 0, false, false)));
    object_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)objectStr, UNKNOWN, 1, 0, false, false)));
    object_cls->freeze();

    auto typeCallObj = boxRTFunction((void*)typeCall, UNKNOWN, 1, 0, true, true);
    typeCallObj->internal_callable = &typeCallInternal;

    type_cls->giveAttr("__name__", new BoxedGetsetDescriptor(type_name, type_set_name, NULL));
    type_cls->giveAttr("__call__", new BoxedFunction(typeCallObj));

    type_cls->giveAttr("__new__",
                       new BoxedFunction(boxRTFunction((void*)typeNew, UNKNOWN, 4, 2, false, false), { NULL, NULL }));
    type_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)typeRepr, STR, 1)));
    type_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)typeHash, BOXED_INT, 1)));
    type_cls->freeze();

    none_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)noneRepr, STR, 1)));
    none_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)noneHash, UNKNOWN, 1)));
    none_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)noneNonzero, BOXED_BOOL, 1)));
    none_cls->freeze();

    module_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)moduleRepr, STR, 1)));
    module_cls->freeze();

    closure_cls->freeze();

    setupCAPI();

    setupBool();
    setupInt();
    setupLong();
    setupFloat();
    setupComplex();
    setupStr();
    setupList();
    setupDict();
    setupSet();
    setupTuple();
    setupFile();
    setupGenerator();
    setupIter();
    setupClassobj();
    setupSuper();
    setupUnicode();
    setupDescr();
    setupTraceback();

    function_cls->giveAttr("__name__", new BoxedGetsetDescriptor(func_name, func_set_name, NULL));
    function_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)functionRepr, STR, 1)));
    function_cls->giveAttr("__module__",
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFunction, modname)));
    function_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)functionGet, UNKNOWN, 3)));
    function_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)functionCall, UNKNOWN, 1, 0, true, true)));
    function_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)functionNonzero, BOXED_BOOL, 1)));
    function_cls->freeze();

    builtin_function_or_method_cls->giveAttr(
        "__module__",
        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedBuiltinFunctionOrMethod, modname)));
    builtin_function_or_method_cls->giveAttr(
        "__repr__", new BoxedFunction(boxRTFunction((void*)builtinFunctionOrMethodRepr, STR, 1)));
    builtin_function_or_method_cls->freeze();

    instancemethod_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)instancemethodRepr, STR, 1)));
    instancemethod_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)instancemethodEq, UNKNOWN, 2)));
    instancemethod_cls->giveAttr(
        "__get__", new BoxedFunction(boxRTFunction((void*)instancemethodGet, UNKNOWN, 3, 0, false, false)));
    instancemethod_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)instancemethodCall, UNKNOWN, 1, 0, true, true)));
    instancemethod_cls->giveAttr(
        "im_func", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedInstanceMethod, func)));
    instancemethod_cls->giveAttr(
        "im_self", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedInstanceMethod, obj)));
    instancemethod_cls->freeze();

    slice_cls->giveAttr("__new__",
                        new BoxedFunction(boxRTFunction((void*)sliceNew, UNKNOWN, 4, 2, false, false), { NULL, None }));
    slice_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)sliceRepr, STR, 1)));
    slice_cls->giveAttr("start", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_START_OFFSET));
    slice_cls->giveAttr("stop", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_STOP_OFFSET));
    slice_cls->giveAttr("step", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, SLICE_STEP_OFFSET));
    slice_cls->freeze();

    attrwrapper_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::setitem, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::getitem, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr(
        "get", new BoxedFunction(boxRTFunction((void*)AttrWrapper::get, UNKNOWN, 3, 1, false, false), { None }));
    attrwrapper_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::str, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("__contains__",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::contains, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("keys", new BoxedFunction(boxRTFunction((void*)AttrWrapper::keys, LIST, 1)));
    attrwrapper_cls->giveAttr("values", new BoxedFunction(boxRTFunction((void*)AttrWrapper::values, LIST, 1)));
    attrwrapper_cls->giveAttr("items", new BoxedFunction(boxRTFunction((void*)AttrWrapper::items, LIST, 1)));
    attrwrapper_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::len, BOXED_INT, 1)));
    attrwrapper_cls->giveAttr("update", new BoxedFunction(boxRTFunction((void*)AttrWrapper::update, NONE, 2)));
    attrwrapper_cls->freeze();

    // sys is the first module that needs to be set up, due to modules
    // being tracked in sys.modules:
    setupSys();

    setupBuiltins();
    setupThread();
    setupGC();
    setupImport();
    setupPyston();

    PyType_Ready(&PyCapsule_Type);

    initerrno();
    init_sha();
    init_sha256();
    init_sha512();
    init_md5();
    init_random();
    init_sre();
    initmath();
    initoperator();
    initbinascii();
    initpwd();
    initposix();
    init_struct();
    initdatetime();
    init_functools();
    init_collections();
    inititertools();
    initresource();
    initsignal();
    initselect();
    initfcntl();
    inittime();
    initarray();
    initzlib();
    init_codecs();

    setupSysEnd();

    Stats::endOfInit();

    TRACK_ALLOCATIONS = true;
}

BoxedModule* createModule(const std::string& name, const std::string& fn) {
    assert(fn.size() && "probably wanted to set the fn to <stdin>?");
    BoxedModule* module = new BoxedModule(name, fn);

    BoxedDict* d = getSysModulesDict();
    Box* b_name = boxStringPtr(&name);
    ASSERT(d->d.count(b_name) == 0, "%s", name.c_str());
    d->d[b_name] = module;

    module->giveAttr("__doc__", None);
    return module;
}

void freeHiddenClasses(HiddenClass* hcls) {
    for (const auto& p : hcls->children) {
        freeHiddenClasses(p.second);
    }
    gc::gc_free(hcls);
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
    teardownComplex();
    teardownStr();
    teardownBool();
    teardownDict();
    teardownSet();
    teardownTuple();
    teardownFile();
    teardownDescr();

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

    freeHiddenClasses(root_hcls);
}
}
