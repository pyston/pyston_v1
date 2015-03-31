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

#include "capi/typeobject.h"
#include "capi/types.h"
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
extern "C" void init_socket();
extern "C" void _PyUnicode_Init();
extern "C" void initunicodedata();
extern "C" void init_weakref();
extern "C" void initcStringIO();
extern "C" void init_io();
extern "C" void initzipimport();
extern "C" void init_csv();
extern "C" void init_ssl();
extern "C" void init_sqlite3();

namespace pyston {

void setupGC();

bool IN_SHUTDOWN = false;

#define SLICE_START_OFFSET ((char*)&(((BoxedSlice*)0x01)->start) - (char*)0x1)
#define SLICE_STOP_OFFSET ((char*)&(((BoxedSlice*)0x01)->stop) - (char*)0x1)
#define SLICE_STEP_OFFSET ((char*)&(((BoxedSlice*)0x01)->step) - (char*)0x1)

// Analogue of PyType_GenericAlloc (default tp_alloc), but should only be used for Pyston classes!
extern "C" PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept {
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
    if (!cls->tp_mro) {
        // wrapperdescr_cls is the last class to be set up during bootstrapping:
        ASSERT(!wrapperdescr_cls, "looks like we need to set up the mro for %s manually", cls->tp_name);
    } else {
        assert(cls->tp_mro && "maybe we should just skip these checks if !mro");
        assert(cls->tp_mro->cls == tuple_cls);
        for (auto b : static_cast<BoxedTuple*>(cls->tp_mro)->elts) {
            // old-style classes are always pyston classes:
            if (b->cls == classobj_cls)
                continue;
            assert(isSubclass(b->cls, type_cls));
            ASSERT(static_cast<BoxedClass*>(b)->is_pyston_class, "%s (%s)", cls->tp_name,
                   static_cast<BoxedClass*>(b)->tp_name);
        }
    }
#endif

    void* mem = gc_alloc(size, gc::GCKind::PYTHON);
    RELEASE_ASSERT(mem, "");

    // Not sure if we can get away with not initializing this memory.
    // I think there are small optimizations we can do, like not initializing cls (always
    // the first 8 bytes) since it will get written by PyObject_Init.
    memset(mem, '\0', size);

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

    if (isSubclass(r->cls, unicode_cls)) {
        r = PyUnicode_AsASCIIString(r);
        checkAndThrowCAPIException();
    }
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


std::string builtinStr("__builtin__");

extern "C" BoxedFunctionBase::BoxedFunctionBase(CLFunction* f)
    : in_weakreflist(NULL), f(f), closure(NULL), isGenerator(false), ndefaults(0), defaults(NULL), modname(NULL),
      name(NULL), doc(NULL) {
    if (f->source) {
        this->modname = f->source->parent_module->getattr("__name__", NULL);
        this->doc = f->source->getDocString();
    } else {
        this->modname = boxStringPtr(&builtinStr);
        this->doc = None;
    }

    assert(f->num_defaults == ndefaults);
}

extern "C" BoxedFunctionBase::BoxedFunctionBase(CLFunction* f, std::initializer_list<Box*> defaults,
                                                BoxedClosure* closure, bool isGenerator)
    : in_weakreflist(NULL), f(f), closure(closure), isGenerator(isGenerator), ndefaults(0), defaults(NULL),
      modname(NULL), name(NULL), doc(NULL) {
    if (defaults.size()) {
        // make sure to initialize defaults first, since the GC behavior is triggered by ndefaults,
        // and a GC can happen within this constructor:
        this->defaults = new (defaults.size()) GCdArray();
        memcpy(this->defaults->elts, defaults.begin(), defaults.size() * sizeof(Box*));
        this->ndefaults = defaults.size();
    }

    if (f->source) {
        this->modname = f->source->parent_module->getattr("__name__", NULL);
        this->doc = f->source->getDocString();
    } else {
        this->modname = boxStringPtr(&builtinStr);
        this->doc = None;
    }

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

BoxedBuiltinFunctionOrMethod::BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name, const char* doc)
    : BoxedBuiltinFunctionOrMethod(f, name, {}) {

    this->doc = doc ? boxStrConstant(doc) : None;
}

BoxedBuiltinFunctionOrMethod::BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name,
                                                           std::initializer_list<Box*> defaults, BoxedClosure* closure,
                                                           bool isGenerator, const char* doc)
    : BoxedFunctionBase(f, defaults, closure, isGenerator) {

    assert(name);
    this->name = static_cast<BoxedString*>(boxString(name));
    this->doc = doc ? boxStrConstant(doc) : None;
}

extern "C" void functionGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedFunction* f = (BoxedFunction*)b;

    // TODO eventually f->name should always be non-NULL, then there'd be no need for this check
    if (f->name)
        v->visit(f->name);

    if (f->modname)
        v->visit(f->modname);

    if (f->doc)
        v->visit(f->doc);

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

BoxedModule::BoxedModule(const std::string& name, const std::string& fn, const char* doc) : fn(fn) {
    this->giveAttr("__name__", boxString(name));
    this->giveAttr("__file__", boxString(fn));
    this->giveAttr("__doc__", doc ? boxStrConstant(doc) : None);
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

Box* BoxedModule::getStringConstant(const std::string& ast_str) {
    auto idx_iter = str_const_index.find(ast_str);
    if (idx_iter != str_const_index.end())
        return str_constants[idx_iter->second];

    Box* box = boxString(ast_str);
    str_const_index[ast_str] = str_constants.size();
    str_constants.push_back(box);
    return box;
}

extern "C" void moduleGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedModule* d = (BoxedModule*)b;

    v->visitRange((void* const*)&d->str_constants[0], (void* const*)&d->str_constants[d->str_constants.size()]);
}

// This mustn't throw; our IR generator generates calls to it without "invoke" even when there are exception handlers /
// finally-blocks in scope.
// TODO: should we use C++11 `noexcept' here?
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
            if (attrs->attr_list)
                v->visit(attrs->attr_list);
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
    if (cls->tp_mro)
        v->visit(cls->tp_mro);
    if (cls->tp_bases)
        v->visit(cls->tp_bases);
    if (cls->tp_subclasses)
        v->visit(cls->tp_subclasses);

    if (cls->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        BoxedHeapClass* hcls = static_cast<BoxedHeapClass*>(cls);
        assert(hcls->ht_name);
        v->visit(hcls->ht_name);
    }
}

static Box* typeDict(Box* obj, void* context) {
    if (obj->cls->instancesHaveHCAttrs())
        return makeAttrWrapper(obj);
    if (obj->cls->instancesHaveDictAttrs())
        return obj->getDict();
    abort();
}

static void typeSetDict(Box* obj, Box* val, void* context) {
    if (obj->cls->instancesHaveDictAttrs()) {
        RELEASE_ASSERT(val->cls == dict_cls, "");
        obj->setDict(static_cast<BoxedDict*>(val));
        return;
    }

    if (obj->cls->instancesHaveHCAttrs()) {
        RELEASE_ASSERT(val->cls == dict_cls || val->cls == attrwrapper_cls, "");

        auto new_attr_list
            = (HCAttrs::AttrList*)gc_alloc(sizeof(HCAttrs::AttrList) + sizeof(Box*), gc::GCKind::PRECISE);
        new_attr_list->attrs[0] = val;

        HCAttrs* hcattrs = obj->getHCAttrsPtr();

        hcattrs->hcls = HiddenClass::dict_backed;
        hcattrs->attr_list = new_attr_list;
        return;
    }

    // This should have thrown an exception rather than get here:
    abort();
}

Box* dict_descr = NULL;

extern "C" void instancemethodGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedInstanceMethod* im = (BoxedInstanceMethod*)b;

    if (im->obj) {
        v->visit(im->obj);
    }
    v->visit(im->func);
}

extern "C" void propertyGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

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
    boxGCHandler(v, b);

    BoxedStaticmethod* sm = (BoxedStaticmethod*)b;

    if (sm->sm_callable)
        v->visit(sm->sm_callable);
}

extern "C" void classmethodGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

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

static int call_gc_visit(PyObject* val, void* arg) {
    if (val) {
        GCVisitor* v = static_cast<GCVisitor*>(arg);
        v->visit(val);
    }
    return 0;
}

static void proxy_to_tp_traverse(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    assert(b->cls->tp_traverse);
    b->cls->tp_traverse(b, call_gc_visit, v);
}

static void proxy_to_tp_clear(Box* b) {
    assert(b->cls->tp_clear);
    b->cls->tp_clear(b);
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

    for (int i = 0; i < c->nelts; i++) {
        if (c->elts[i])
            v->visit(c->elts[i]);
    }
}

extern "C" {
BoxedClass* object_cls, *type_cls, *none_cls, *bool_cls, *int_cls, *float_cls,
    * str_cls = NULL, *function_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls,
      *file_cls, *member_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls, *property_cls,
      *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *pyston_getset_cls, *capi_getset_cls,
      *builtin_function_or_method_cls, *attrwrapperiter_cls;

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
        RELEASE_ASSERT(e.matches(BaseException), "");

        Box* msg = e.value;
        assert(msg);
        // TODO this is an extra Pyston check and I don't think we should have to do it:
        if (isSubclass(e.value->cls, BaseException))
            msg = getattr(e.value, "message");

        if (isSubclass(msg->cls, str_cls)) {
            auto newmsg = PyString_FromFormat("Error when calling the metaclass bases\n"
                                              "    %s",
                                              PyString_AS_STRING(msg));
            if (newmsg)
                e.value = newmsg;
        }

        // Go through these routines since they do some normalization:
        PyErr_Restore(e.type, e.value, e.traceback);
        throwCAPIException();
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

static Box* funcName(Box* b, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);
    RELEASE_ASSERT(func->name != NULL, "func->name is not set");
    return func->name;
}

static void funcSetName(Box* b, Box* v, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);

    if (v == NULL || !PyString_Check(v)) {
        raiseExcHelper(TypeError, "__name__ must be set to a string object");
    }

    func->name = static_cast<BoxedString*>(v);
}

static Box* builtinFunctionOrMethodName(Box* b, void*) {
    // In CPython, these guys just store char*, and it gets wrapped here
    // But we already share the BoxedString* field with BoxedFunctions...
    // so it's more convenient to just use that, which is what we do here.
    // Is there any advantage to using the char* way, here?

    assert(b->cls == builtin_function_or_method_cls);
    BoxedBuiltinFunctionOrMethod* func = static_cast<BoxedBuiltinFunctionOrMethod*>(b);
    assert(func->name);
    return func->name;
}

static Box* functionCode(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    // This fails "f.func_code is f.func_code"
    return codeForFunction(func);
}

static Box* functionDefaults(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    if (!func->ndefaults)
        return None;
    return new BoxedTuple(BoxedTuple::GCVector(&func->defaults->elts[0], &func->defaults->elts[func->ndefaults]));
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
HiddenClass* HiddenClass::dict_backed;

extern "C" Box* createSlice(Box* start, Box* stop, Box* step) {
    BoxedSlice* rtn = new BoxedSlice(start, stop, step);
    return rtn;
}

extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure, size_t n) {
    if (parent_closure)
        assert(parent_closure->cls == closure_cls);
    BoxedClosure* closure = new (n) BoxedClosure(parent_closure);
    assert(closure->cls == closure_cls);
    return closure;
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

Box* instancemethodNew(BoxedClass* cls, Box* func, Box* self, Box** args) {
    Box* classObj = args[0];

    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError, "first argument must be callable");
        return NULL;
    }
    if (self == Py_None)
        self = NULL;
    if (self == NULL && classObj == NULL) {
        PyErr_SetString(PyExc_TypeError, "unbound methods must have non-NULL im_class");
        return NULL;
    }

    return new BoxedInstanceMethod(self, func);
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

static PyObject* typeModule(Box* _type, void* context) {
    PyTypeObject* type = static_cast<PyTypeObject*>(_type);

    PyObject* mod;
    const char* s;

    if (type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        mod = type->getattr("__module__");
        if (!mod)
            raiseExcHelper(AttributeError, "__module__");
        return mod;
    } else {
        s = strrchr(type->tp_name, '.');
        if (s != NULL)
            return PyString_FromStringAndSize(type->tp_name, (Py_ssize_t)(s - type->tp_name));
        return PyString_FromString("__builtin__");
    }
}

static void typeSetModule(Box* _type, PyObject* value, void* context) {
    PyTypeObject* type = static_cast<PyTypeObject*>(_type);

    if (!(type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        raiseExcHelper(TypeError, "can't set %s.__module__", type->tp_name);
    }
    if (!value) {
        raiseExcHelper(TypeError, "can't delete %s.__module__", type->tp_name);
    }

    PyType_Modified(type);

    type->setattr("__module__", value, NULL);
}


Box* typeHash(BoxedClass* self) {
    assert(isSubclass(self->cls, type_cls));

    // This is how CPython defines it; seems reasonable enough:
    return boxInt(reinterpret_cast<intptr_t>(self) >> 4);
}

static PyObject* type_subclasses(PyTypeObject* type, PyObject* args_ignored) noexcept {
    PyObject* list, *raw, *ref;
    Py_ssize_t i, n;

    list = PyList_New(0);
    if (list == NULL)
        return NULL;
    raw = type->tp_subclasses;
    if (raw == NULL)
        return list;
    assert(PyList_Check(raw));
    n = PyList_GET_SIZE(raw);
    for (i = 0; i < n; i++) {
        ref = PyList_GET_ITEM(raw, i);
        assert(PyWeakref_CheckRef(ref));
        ref = PyWeakref_GET_OBJECT(ref);
        if (ref != Py_None) {
            if (PyList_Append(list, ref) < 0) {
                Py_DECREF(list);
                return NULL;
            }
        }
    }
    return list;
}

Box* typeSubclasses(BoxedClass* self) {
    assert(isSubclass(self->cls, type_cls));
    Box* rtn = type_subclasses(self, 0);
    checkAndThrowCAPIException();
    return rtn;
}

Box* typeMro(BoxedClass* self) {
    assert(isSubclass(self->cls, type_cls));

    Box* r = mro_external(self);
    if (!r)
        throwCAPIException();
    return r;
}

Box* moduleNew(BoxedClass* cls, BoxedString* name, BoxedString* fn) {
    RELEASE_ASSERT(isSubclass(cls, module_cls), "");
    RELEASE_ASSERT(name->cls == str_cls, "");
    RELEASE_ASSERT(!fn || fn->cls == str_cls, "");

    if (fn)
        return new (cls) BoxedModule(name->s, fn->s);
    else
        return new (cls) BoxedModule(name->s, "__builtin__");
}

Box* moduleRepr(BoxedModule* m) {
    RELEASE_ASSERT(isSubclass(m->cls, module_cls), "");

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

class AttrWrapper;
class AttrWrapperIter : public Box {
private:
    // Iterating over the an attrwrapper (~=dict) just gives the keys, which
    // just depends on the hidden class of the object.  Let's store only that:
    HiddenClass* hcls;
    llvm::StringMap<int>::const_iterator it;

public:
    AttrWrapperIter(AttrWrapper* aw);

    DEFAULT_CLASS(attrwrapperiter_cls);

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        AttrWrapperIter* self = (AttrWrapperIter*)b;
        v->visit(self->hcls);
    }

    static Box* hasnext(Box* _self);
    static Box* next(Box* _self);
};

// A dictionary-like wrapper around the attributes array.
// Not sure if this will be enough to satisfy users who expect __dict__
// or PyModule_GetDict to return real dicts.
class AttrWrapper : public Box {
private:
    Box* b;

public:
    AttrWrapper(Box* b) : b(b) {
        assert(b->cls->instancesHaveHCAttrs());

        // We currently don't support creating an attrwrapper around a dict-backed object,
        // so try asserting that here.
        // This check doesn't cover all cases, since an attrwrapper could be created around
        // a normal object which then becomes dict-backed, so we RELEASE_ASSERT later
        // that that doesn't happen.
        assert(b->getHCAttrsPtr()->hcls->type == HiddenClass::NORMAL);
    }

    DEFAULT_CLASS(attrwrapper_cls);

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        AttrWrapper* aw = (AttrWrapper*)b;
        v->visit(aw->b);
    }

    static Box* setitem(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        self->b->setattr(key->s, value, NULL);
        return None;
    }

    static Box* setdefault(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* cur = self->b->getattr(key->s);
        if (cur)
            return cur;
        self->b->setattr(key->s, value, NULL);
        return value;
    }

    static Box* get(Box* _self, Box* _key, Box* def) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

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

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "%s", _key->cls->tp_name);
        BoxedString* key = static_cast<BoxedString*>(_key);
        Box* r = self->b->getattr(key->s);
        if (!r)
            raiseExcHelper(KeyError, "'%s'", key->s.c_str());
        return r;
    }

    static Box* delitem(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "%s", _key->cls->tp_name);
        BoxedString* key = static_cast<BoxedString*>(_key);
        if (self->b->getattr(key->s))
            self->b->delattr(key->s, NULL);
        else
            raiseExcHelper(KeyError, "'%s'", key->s.c_str());
        return None;
    }

    static Box* str(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        std::ostringstream os("");
        os << "attrwrapper({";

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
        bool first = true;
        for (const auto& p : attrs->hcls->getAttrOffsets()) {
            if (!first)
                os << ", ";
            first = false;

            BoxedString* v = attrs->attr_list->attrs[p.second]->reprICAsString();
            os << p.first().str() << ": " << v->s;
        }
        os << "})";
        return boxString(os.str());
    }

    static Box* contains(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

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
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
        for (const auto& p : attrs->hcls->getAttrOffsets()) {
            listAppend(rtn, boxString(p.first()));
        }
        return rtn;
    }

    static Box* values(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
        for (const auto& p : attrs->hcls->getAttrOffsets()) {
            listAppend(rtn, attrs->attr_list->attrs[p.second]);
        }
        return rtn;
    }

    static Box* items(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
        for (const auto& p : attrs->hcls->getAttrOffsets()) {
            BoxedTuple* t = new BoxedTuple({ boxString(p.first()), attrs->attr_list->attrs[p.second] });
            listAppend(rtn, t);
        }
        return rtn;
    }

    static Box* copy(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedDict* rtn = new BoxedDict();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
        for (const auto& p : attrs->hcls->getAttrOffsets()) {
            rtn->d[boxString(p.first())] = attrs->attr_list->attrs[p.second];
        }
        return rtn;
    }

    static Box* len(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
        return boxInt(attrs->hcls->getAttrOffsets().size());
    }

    static Box* update(Box* _self, Box* _container) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_container->cls == attrwrapper_cls) {
            AttrWrapper* container = static_cast<AttrWrapper*>(_container);
            HCAttrs* attrs = container->b->getHCAttrsPtr();

            RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL, "");
            for (const auto& p : attrs->hcls->getAttrOffsets()) {
                self->b->setattr(p.first(), attrs->attr_list->attrs[p.second], NULL);
            }
        } else if (_container->cls == dict_cls) {
            BoxedDict* container = static_cast<BoxedDict*>(_container);

            for (const auto& p : container->d) {
                AttrWrapper::setitem(self, p.first, p.second);
            }
        } else {
            RELEASE_ASSERT(0, "not implemented: %s", _container->cls->tp_name);
        }
        return None;
    }

    static Box* iter(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        return new AttrWrapperIter(self);
    }

    friend class AttrWrapperIter;
};

AttrWrapperIter::AttrWrapperIter(AttrWrapper* aw) {
    hcls = aw->b->getHCAttrsPtr()->hcls;
    assert(hcls);
    RELEASE_ASSERT(hcls->type == HiddenClass::NORMAL, "");
    it = hcls->getAttrOffsets().begin();
}

Box* AttrWrapperIter::hasnext(Box* _self) {
    RELEASE_ASSERT(_self->cls == attrwrapperiter_cls, "");
    AttrWrapperIter* self = static_cast<AttrWrapperIter*>(_self);
    RELEASE_ASSERT(self->hcls->type == HiddenClass::NORMAL, "");

    return boxBool(self->it != self->hcls->getAttrOffsets().end());
}

Box* AttrWrapperIter::next(Box* _self) {
    RELEASE_ASSERT(_self->cls == attrwrapperiter_cls, "");
    AttrWrapperIter* self = static_cast<AttrWrapperIter*>(_self);
    RELEASE_ASSERT(self->hcls->type == HiddenClass::NORMAL, "");

    assert(self->it != self->hcls->getAttrOffsets().end());
    Box* r = boxString(self->it->first());
    ++self->it;
    return r;
}

Box* makeAttrWrapper(Box* b) {
    assert(b->cls->instancesHaveHCAttrs());
    if (b->getHCAttrsPtr()->hcls->type == HiddenClass::DICT_BACKED) {
        return b->getHCAttrsPtr()->attr_list->attrs[0];
    }

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

Box* objectSetattr(Box* obj, Box* attr, Box* value) {
    attr = coerceUnicodeToStr(attr);
    if (attr->cls != str_cls) {
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", attr->cls->tp_name);
    }

    BoxedString* attr_str = static_cast<BoxedString*>(attr);
    setattrGeneric(obj, attr_str->s, value, NULL);
    return None;
}

Box* objectSubclasshook(Box* cls, Box* a) {
    return NotImplemented;
}

static PyObject* import_copyreg(void) noexcept {
    static PyObject* copyreg_str;

    if (!copyreg_str) {
        copyreg_str = PyGC_AddRoot(PyString_InternFromString("copy_reg"));
        if (copyreg_str == NULL)
            return NULL;
    }

    return PyImport_Import(copyreg_str);
}

static PyObject* slotnames(PyObject* cls) noexcept {
    PyObject* clsdict;
    PyObject* copyreg;
    PyObject* slotnames;

    if (!PyType_Check(cls)) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    clsdict = ((PyTypeObject*)cls)->tp_dict;
    slotnames = PyDict_GetItemString(clsdict, "__slotnames__");
    if (slotnames != NULL && PyList_Check(slotnames)) {
        Py_INCREF(slotnames);
        return slotnames;
    }

    copyreg = import_copyreg();
    if (copyreg == NULL)
        return NULL;

    slotnames = PyObject_CallMethod(copyreg, "_slotnames", "O", cls);
    Py_DECREF(copyreg);
    if (slotnames != NULL && slotnames != Py_None && !PyList_Check(slotnames)) {
        PyErr_SetString(PyExc_TypeError, "copy_reg._slotnames didn't return a list or None");
        Py_DECREF(slotnames);
        slotnames = NULL;
    }

    return slotnames;
}

static PyObject* reduce_2(PyObject* obj) noexcept {
    PyObject* cls, *getnewargs;
    PyObject* args = NULL, * args2 = NULL;
    PyObject* getstate = NULL, * state = NULL, * names = NULL;
    PyObject* slots = NULL, * listitems = NULL, * dictitems = NULL;
    PyObject* copyreg = NULL, * newobj = NULL, * res = NULL;
    Py_ssize_t i, n;

    cls = PyObject_GetAttrString(obj, "__class__");
    if (cls == NULL)
        return NULL;

    getnewargs = PyObject_GetAttrString(obj, "__getnewargs__");
    if (getnewargs != NULL) {
        args = PyObject_CallObject(getnewargs, NULL);
        Py_DECREF(getnewargs);
        if (args != NULL && !PyTuple_Check(args)) {
            PyErr_Format(PyExc_TypeError, "__getnewargs__ should return a tuple, "
                                          "not '%.200s'",
                         Py_TYPE(args)->tp_name);
            goto end;
        }
    } else {
        PyErr_Clear();
        args = PyTuple_New(0);
    }
    if (args == NULL)
        goto end;

    getstate = PyObject_GetAttrString(obj, "__getstate__");
    if (getstate != NULL) {
        state = PyObject_CallObject(getstate, NULL);
        Py_DECREF(getstate);
        if (state == NULL)
            goto end;
    } else {
        PyErr_Clear();
        state = PyObject_GetAttrString(obj, "__dict__");
        if (state == NULL) {
            PyErr_Clear();
            state = Py_None;
            Py_INCREF(state);
        }
        names = slotnames(cls);
        if (names == NULL)
            goto end;
        if (names != Py_None) {
            assert(PyList_Check(names));
            slots = PyDict_New();
            if (slots == NULL)
                goto end;
            n = 0;
            /* Can't pre-compute the list size; the list
               is stored on the class so accessible to other
               threads, which may be run by DECREF */
            for (i = 0; i < PyList_GET_SIZE(names); i++) {
                PyObject* name, *value;
                name = PyList_GET_ITEM(names, i);
                value = PyObject_GetAttr(obj, name);
                if (value == NULL)
                    PyErr_Clear();
                else {
                    int err = PyDict_SetItem(slots, name, value);
                    Py_DECREF(value);
                    if (err)
                        goto end;
                    n++;
                }
            }
            if (n) {
                state = Py_BuildValue("(NO)", state, slots);
                if (state == NULL)
                    goto end;
            }
        }
    }

    if (!PyList_Check(obj)) {
        listitems = Py_None;
        Py_INCREF(listitems);
    } else {
        listitems = PyObject_GetIter(obj);
        if (listitems == NULL)
            goto end;
    }

    if (!PyDict_Check(obj)) {
        dictitems = Py_None;
        Py_INCREF(dictitems);
    } else {
        dictitems = PyObject_CallMethod(obj, "iteritems", "");
        if (dictitems == NULL)
            goto end;
    }

    copyreg = import_copyreg();
    if (copyreg == NULL)
        goto end;
    newobj = PyObject_GetAttrString(copyreg, "__newobj__");
    if (newobj == NULL)
        goto end;

    n = PyTuple_GET_SIZE(args);
    args2 = PyTuple_New(n + 1);
    if (args2 == NULL)
        goto end;
    PyTuple_SET_ITEM(args2, 0, cls);
    cls = NULL;
    for (i = 0; i < n; i++) {
        PyObject* v = PyTuple_GET_ITEM(args, i);
        Py_INCREF(v);
        PyTuple_SET_ITEM(args2, i + 1, v);
    }

    res = PyTuple_Pack(5, newobj, args2, state, listitems, dictitems);

end:
    Py_XDECREF(cls);
    Py_XDECREF(args);
    Py_XDECREF(args2);
    Py_XDECREF(slots);
    Py_XDECREF(state);
    Py_XDECREF(names);
    Py_XDECREF(listitems);
    Py_XDECREF(dictitems);
    Py_XDECREF(copyreg);
    Py_XDECREF(newobj);
    return res;
}

static PyObject* _common_reduce(PyObject* self, int proto) noexcept {
    PyObject* copyreg, *res;

    if (proto >= 2)
        return reduce_2(self);

    copyreg = import_copyreg();
    if (!copyreg)
        return NULL;

    res = PyEval_CallMethod(copyreg, "_reduce_ex", "(Oi)", self, proto);
    Py_DECREF(copyreg);

    return res;
}

static PyObject* object_reduce(PyObject* self, PyObject* args) noexcept {
    int proto = 0;

    if (!PyArg_ParseTuple(args, "|i:__reduce__", &proto))
        return NULL;

    return _common_reduce(self, proto);
}

static PyObject* object_reduce_ex(PyObject* self, PyObject* args) noexcept {
    PyObject* reduce, *res;
    int proto = 0;

    if (!PyArg_ParseTuple(args, "|i:__reduce_ex__", &proto))
        return NULL;

    reduce = PyObject_GetAttrString(self, "__reduce__");
    if (reduce == NULL)
        PyErr_Clear();
    else {
        PyObject* cls, *clsreduce, *objreduce;
        int override;
        cls = PyObject_GetAttrString(self, "__class__");
        if (cls == NULL) {
            Py_DECREF(reduce);
            return NULL;
        }
        clsreduce = PyObject_GetAttrString(cls, "__reduce__");
        Py_DECREF(cls);
        if (clsreduce == NULL) {
            Py_DECREF(reduce);
            return NULL;
        }
        objreduce = PyDict_GetItemString(PyBaseObject_Type.tp_dict, "__reduce__");
        override = (clsreduce != objreduce);
        Py_DECREF(clsreduce);
        if (override) {
            res = PyObject_CallObject(reduce, NULL);
            Py_DECREF(reduce);
            return res;
        } else
            Py_DECREF(reduce);
    }

    return _common_reduce(self, proto);
}

static Box* objectClass(Box* obj, void* context) {
    assert(obj->cls != instance_cls); // should override __class__ in classobj
    return obj->cls;
}

static void objectSetClass(Box* obj, Box* val, void* context) {
    if (!isSubclass(val->cls, type_cls))
        raiseExcHelper(TypeError, "__class__ must be set to new-style class, not '%s' object", val->cls->tp_name);

    auto new_cls = static_cast<BoxedClass*>(val);

    // Conservative Pyston checks: make sure that both classes are derived only from Pyston types,
    // and that they don't define any extra C-level fields
    RELEASE_ASSERT(val->cls == type_cls, "");
    RELEASE_ASSERT(obj->cls->cls == type_cls, "");
    for (auto _base : static_cast<BoxedTuple*>(obj->cls->tp_mro)->elts) {
        BoxedClass* base = static_cast<BoxedClass*>(_base);
        RELEASE_ASSERT(base->is_pyston_class, "");
    }
    for (auto _base : static_cast<BoxedTuple*>(new_cls->tp_mro)->elts) {
        BoxedClass* base = static_cast<BoxedClass*>(_base);
        RELEASE_ASSERT(base->is_pyston_class, "");
    }

    RELEASE_ASSERT(obj->cls->tp_basicsize == object_cls->tp_basicsize + sizeof(HCAttrs) + sizeof(Box**), "");
    RELEASE_ASSERT(new_cls->tp_basicsize == object_cls->tp_basicsize + sizeof(HCAttrs) + sizeof(Box**), "");
    RELEASE_ASSERT(obj->cls->attrs_offset != 0, "");
    RELEASE_ASSERT(new_cls->attrs_offset != 0, "");
    RELEASE_ASSERT(obj->cls->tp_weaklistoffset != 0, "");
    RELEASE_ASSERT(new_cls->tp_weaklistoffset != 0, "");

    // Normal Python checks.
    // TODO there are more checks to add here, and they should throw errors not asserts
    RELEASE_ASSERT(obj->cls->tp_basicsize == new_cls->tp_basicsize, "");
    RELEASE_ASSERT(obj->cls->tp_dictoffset == new_cls->tp_dictoffset, "");
    RELEASE_ASSERT(obj->cls->tp_weaklistoffset == new_cls->tp_weaklistoffset, "");
    RELEASE_ASSERT(obj->cls->attrs_offset == new_cls->attrs_offset, "");

    obj->cls = new_cls;
}

static PyMethodDef object_methods[] = {
    { "__reduce_ex__", object_reduce_ex, METH_VARARGS, NULL }, //
    { "__reduce__", object_reduce, METH_VARARGS, NULL },       //
};

static Box* typeName(Box* b, void*) {
    RELEASE_ASSERT(isSubclass(b->cls, type_cls), "");
    BoxedClass* type = static_cast<BoxedClass*>(b);

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

static void typeSetName(Box* b, Box* v, void*) {
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

static Box* typeBases(Box* b, void*) {
    RELEASE_ASSERT(isSubclass(b->cls, type_cls), "");
    BoxedClass* type = static_cast<BoxedClass*>(b);

    assert(type->tp_bases);
    return type->tp_bases;
}

static void typeSetBases(Box* b, Box* v, void*) {
    Py_FatalError("unimplemented");
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

    if (PyType_SUPPORTS_WEAKREFS(tp)) {
        *PyObject_GET_WEAKREFS_LISTPTR(op) = NULL;
    }

    // I think CPython defers the dict creation (equivalent of our initUserAttrs) to the
    // first time that an attribute gets set.
    // Our HCAttrs object already includes this optimization of no-allocation-if-empty,
    // but it's nice to initialize the hcls here so we don't have to check it on every getattr/setattr.
    // TODO It does mean that anything not defering to this function will have to call
    // initUserAttrs themselves, though.
    initUserAttrs(op, tp);

    return op;
}

Box* decodeUTF8StringPtr(const std::string* s) {
    Box* rtn = PyUnicode_DecodeUTF8(s->c_str(), s->size(), "strict");
    checkAndThrowCAPIException();
    assert(rtn);
    return rtn;
}

bool TRACK_ALLOCATIONS = false;
void setupRuntime() {
    root_hcls = HiddenClass::makeRoot();
    gc::registerPermanentRoot(root_hcls);
    HiddenClass::dict_backed = HiddenClass::makeDictBacked();
    gc::registerPermanentRoot(HiddenClass::dict_backed);

    // Disable the GC while we do some manual initialization of the object hierarchy:
    gc::disableGC();

    // We have to do a little dance to get object_cls and type_cls set up, since the normal
    // object-creation routines look at the class to see the allocation size.
    void* mem = gc_alloc(sizeof(BoxedClass), gc::GCKind::PYTHON);
    object_cls = ::new (mem) BoxedClass(NULL, &boxGCHandler, 0, 0, sizeof(Box), false);
    mem = gc_alloc(sizeof(BoxedHeapClass), gc::GCKind::PYTHON);
    type_cls = ::new (mem) BoxedHeapClass(object_cls, &typeGCHandler, offsetof(BoxedClass, attrs),
                                          offsetof(BoxedClass, tp_weaklist), sizeof(BoxedHeapClass), false, NULL);
    type_cls->tp_flags |= Py_TPFLAGS_TYPE_SUBCLASS;
    PyObject_Init(object_cls, type_cls);
    PyObject_Init(type_cls, type_cls);

    none_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(Box), false, NULL);
    None = new (none_cls) Box();
    assert(None->cls);
    gc::registerPermanentRoot(None);

    // You can't actually have an instance of basestring
    basestring_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(Box), false, NULL);

    str_cls = new BoxedHeapClass(basestring_cls, NULL, 0, 0, sizeof(BoxedString), false, NULL);
    str_cls->tp_flags |= Py_TPFLAGS_STRING_SUBCLASS;

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

    // It wasn't safe to add __base__ attributes until object+type+str are set up, so do that now:
    type_cls->giveAttr("__base__", object_cls);
    basestring_cls->giveAttr("__base__", object_cls);
    str_cls->giveAttr("__base__", basestring_cls);
    none_cls->giveAttr("__base__", object_cls);
    object_cls->giveAttr("__base__", None);


    tuple_cls
        = new BoxedHeapClass(object_cls, &tupleGCHandler, 0, 0, sizeof(BoxedTuple), false, boxStrConstant("tuple"));
    tuple_cls->tp_flags |= Py_TPFLAGS_TUPLE_SUBCLASS;
    EmptyTuple = new BoxedTuple({});
    gc::registerPermanentRoot(EmptyTuple);
    list_cls = new BoxedHeapClass(object_cls, &listGCHandler, 0, 0, sizeof(BoxedList), false, boxStrConstant("list"));
    list_cls->tp_flags |= Py_TPFLAGS_LIST_SUBCLASS;
    pyston_getset_cls
        = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedGetsetDescriptor), false, boxStrConstant("getset"));
    attrwrapper_cls = new BoxedHeapClass(object_cls, &AttrWrapper::gcHandler, 0, 0, sizeof(AttrWrapper), false,
                                         new BoxedString("attrwrapper"));
    dict_cls = new BoxedHeapClass(object_cls, &dictGCHandler, 0, 0, sizeof(BoxedDict), false, new BoxedString("dict"));
    dict_cls->tp_flags |= Py_TPFLAGS_DICT_SUBCLASS;
    file_cls = new BoxedHeapClass(object_cls, &BoxedFile::gcHandler, 0, offsetof(BoxedFile, weakreflist),
                                  sizeof(BoxedFile), false, new BoxedString("file"));
    int_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedInt), false, new BoxedString("int"));
    int_cls->tp_flags |= Py_TPFLAGS_INT_SUBCLASS;
    bool_cls = new BoxedHeapClass(int_cls, NULL, 0, 0, sizeof(BoxedBool), false, new BoxedString("bool"));
    complex_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedComplex), false, new BoxedString("complex"));
    long_cls = new BoxedHeapClass(object_cls, &BoxedLong::gchandler, 0, 0, sizeof(BoxedLong), false,
                                  new BoxedString("long"));
    long_cls->tp_flags |= Py_TPFLAGS_LONG_SUBCLASS;
    float_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedFloat), false, new BoxedString("float"));
    function_cls = new BoxedHeapClass(object_cls, &functionGCHandler, offsetof(BoxedFunction, attrs),
                                      offsetof(BoxedFunction, in_weakreflist), sizeof(BoxedFunction), false,
                                      new BoxedString("function"));
    builtin_function_or_method_cls = new BoxedHeapClass(
        object_cls, &functionGCHandler, 0, offsetof(BoxedBuiltinFunctionOrMethod, in_weakreflist),
        sizeof(BoxedBuiltinFunctionOrMethod), false, new BoxedString("builtin_function_or_method"));
    function_cls->simple_destructor = builtin_function_or_method_cls->simple_destructor = functionDtor;


    module_cls = new BoxedHeapClass(object_cls, &moduleGCHandler, offsetof(BoxedModule, attrs), 0, sizeof(BoxedModule),
                                    false, new BoxedString("module"));
    member_cls
        = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedMemberDescriptor), false, new BoxedString("member"));
    capifunc_cls
        = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedCApiFunction), false, new BoxedString("capifunc"));
    method_cls
        = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedMethodDescriptor), false, new BoxedString("method"));
    wrapperobject_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedWrapperObject), false,
                                           new BoxedString("method-wrapper"));
    wrapperdescr_cls = new BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedWrapperDescriptor), false,
                                          new BoxedString("wrapper_descriptor"));


    // Kind of hacky, but it's easier to manually construct the mro for a couple key classes
    // than try to make the MRO construction code be safe against say, tuple_cls not having
    // an mro (since the mro is stored as a tuple).
    object_cls->tp_mro = new BoxedTuple({ object_cls });
    tuple_cls->tp_mro = new BoxedTuple({ tuple_cls, object_cls });
    list_cls->tp_mro = new BoxedTuple({ list_cls, object_cls });
    type_cls->tp_mro = new BoxedTuple({ type_cls, object_cls });
    pyston_getset_cls->tp_mro = new BoxedTuple({ pyston_getset_cls, object_cls });
    attrwrapper_cls->tp_mro = new BoxedTuple({ attrwrapper_cls, object_cls });
    dict_cls->tp_mro = new BoxedTuple({ dict_cls, object_cls });
    file_cls->tp_mro = new BoxedTuple({ file_cls, object_cls });
    int_cls->tp_mro = new BoxedTuple({ int_cls, object_cls });
    bool_cls->tp_mro = new BoxedTuple({ bool_cls, object_cls });
    complex_cls->tp_mro = new BoxedTuple({ complex_cls, object_cls });
    long_cls->tp_mro = new BoxedTuple({ long_cls, object_cls });
    float_cls->tp_mro = new BoxedTuple({ float_cls, object_cls });
    function_cls->tp_mro = new BoxedTuple({ function_cls, object_cls });
    builtin_function_or_method_cls->tp_mro = new BoxedTuple({ builtin_function_or_method_cls, object_cls });
    member_cls->tp_mro = new BoxedTuple({ member_cls, object_cls });
    capifunc_cls->tp_mro = new BoxedTuple({ capifunc_cls, object_cls });
    module_cls->tp_mro = new BoxedTuple({ module_cls, object_cls });
    method_cls->tp_mro = new BoxedTuple({ method_cls, object_cls });
    wrapperobject_cls->tp_mro = new BoxedTuple({ wrapperobject_cls, object_cls });
    wrapperdescr_cls->tp_mro = new BoxedTuple({ wrapperdescr_cls, object_cls });

    STR = typeFromClass(str_cls);
    BOXED_INT = typeFromClass(int_cls);
    BOXED_FLOAT = typeFromClass(float_cls);
    BOXED_BOOL = typeFromClass(bool_cls);
    NONE = typeFromClass(none_cls);
    LIST = typeFromClass(list_cls);
    MODULE = typeFromClass(module_cls);
    DICT = typeFromClass(dict_cls);
    BOXED_TUPLE = typeFromClass(tuple_cls);
    LONG = typeFromClass(long_cls);
    BOXED_COMPLEX = typeFromClass(complex_cls);

    True = new BoxedBool(true);
    False = new BoxedBool(false);

    gc::registerPermanentRoot(True);
    gc::registerPermanentRoot(False);

    // Need to initialize interned_ints early:
    setupInt();
    // sys is the first module that needs to be set up, due to modules
    // being tracked in sys.modules:
    setupSys();
    // Weakrefs are used for tp_subclasses:
    init_weakref();

    object_cls->tp_getattro = PyObject_GenericGetAttr;
    object_cls->tp_setattro = PyObject_GenericSetAttr;
    add_operators(object_cls);

    object_cls->finishInitialization();
    type_cls->finishInitialization();
    basestring_cls->finishInitialization();
    str_cls->finishInitialization();
    none_cls->finishInitialization();
    tuple_cls->finishInitialization();
    list_cls->finishInitialization();
    pyston_getset_cls->finishInitialization();
    attrwrapper_cls->finishInitialization();
    dict_cls->finishInitialization();
    file_cls->finishInitialization();
    int_cls->finishInitialization();
    bool_cls->finishInitialization();
    complex_cls->finishInitialization();
    long_cls->finishInitialization();
    float_cls->finishInitialization();
    function_cls->finishInitialization();
    builtin_function_or_method_cls->finishInitialization();
    member_cls->finishInitialization();
    module_cls->finishInitialization();
    capifunc_cls->finishInitialization();
    method_cls->finishInitialization();
    wrapperobject_cls->finishInitialization();
    wrapperdescr_cls->finishInitialization();

    str_cls->tp_flags |= Py_TPFLAGS_HAVE_NEWBUFFER;

    dict_descr = new (pyston_getset_cls) BoxedGetsetDescriptor(typeDict, typeSetDict, NULL);
    gc::registerPermanentRoot(dict_descr);
    type_cls->giveAttr("__dict__", dict_descr);


    instancemethod_cls = BoxedHeapClass::create(type_cls, object_cls, &instancemethodGCHandler, 0,
                                                offsetof(BoxedInstanceMethod, in_weakreflist),
                                                sizeof(BoxedInstanceMethod), false, "instancemethod");

    slice_cls = BoxedHeapClass::create(type_cls, object_cls, &sliceGCHandler, 0, 0, sizeof(BoxedSlice), false, "slice");
    set_cls = BoxedHeapClass::create(type_cls, object_cls, &setGCHandler, 0, offsetof(BoxedSet, weakreflist),
                                     sizeof(BoxedSet), false, "set");
    frozenset_cls = BoxedHeapClass::create(type_cls, object_cls, &setGCHandler, 0, offsetof(BoxedSet, weakreflist),
                                           sizeof(BoxedSet), false, "frozenset");
    capi_getset_cls
        = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(BoxedGetsetDescriptor), false, "getset");
    closure_cls
        = BoxedHeapClass::create(type_cls, object_cls, &closureGCHandler, 0, 0, sizeof(BoxedClosure), false, "closure");
    property_cls = BoxedHeapClass::create(type_cls, object_cls, &propertyGCHandler, 0, 0, sizeof(BoxedProperty), false,
                                          "property");
    staticmethod_cls = BoxedHeapClass::create(type_cls, object_cls, &staticmethodGCHandler, 0, 0,
                                              sizeof(BoxedStaticmethod), false, "staticmethod");
    classmethod_cls = BoxedHeapClass::create(type_cls, object_cls, &classmethodGCHandler, 0, 0,
                                             sizeof(BoxedClassmethod), false, "classmethod");
    attrwrapperiter_cls = BoxedHeapClass::create(type_cls, object_cls, &AttrWrapperIter::gcHandler, 0, 0,
                                                 sizeof(AttrWrapperIter), false, "attrwrapperiter");

    // TODO: add explicit __get__ and __set__ methods to these
    pyston_getset_cls->freeze();
    capi_getset_cls->freeze();

    SLICE = typeFromClass(slice_cls);
    SET = typeFromClass(set_cls);
    FROZENSET = typeFromClass(frozenset_cls);

    object_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)objectNew, UNKNOWN, 1, 0, true, true)));
    object_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)objectInit, UNKNOWN, 1, 0, true, false)));
    object_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)objectRepr, UNKNOWN, 1, 0, false, false)));
    object_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)objectStr, UNKNOWN, 1, 0, false, false)));
    object_cls->giveAttr(
        "__subclasshook__",
        boxInstanceMethod(object_cls, new BoxedFunction(boxRTFunction((void*)objectSubclasshook, UNKNOWN, 2))));
    // __setattr__ was already set to a WrapperDescriptor; it'd be nice to set this to a faster BoxedFunction
    // object_cls->setattr("__setattr__", new BoxedFunction(boxRTFunction((void*)objectSetattr, UNKNOWN, 3)), NULL);
    // but unfortunately that will set tp_setattro to slot_tp_setattro on object_cls and all already-made subclasses!
    // Punting on that until needed; hopefully by then we will have better Pyston slots support.

    auto typeCallObj = boxRTFunction((void*)typeCall, UNKNOWN, 1, 0, true, true);
    typeCallObj->internal_callable = &typeCallInternal;

    type_cls->giveAttr("__name__", new (pyston_getset_cls) BoxedGetsetDescriptor(typeName, typeSetName, NULL));
    type_cls->giveAttr("__bases__", new (pyston_getset_cls) BoxedGetsetDescriptor(typeBases, typeSetBases, NULL));
    type_cls->giveAttr("__call__", new BoxedFunction(typeCallObj));

    type_cls->giveAttr("__new__",
                       new BoxedFunction(boxRTFunction((void*)typeNew, UNKNOWN, 4, 2, false, false), { NULL, NULL }));
    type_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)typeRepr, STR, 1)));
    type_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)typeHash, BOXED_INT, 1)));
    type_cls->giveAttr("__module__", new (pyston_getset_cls) BoxedGetsetDescriptor(typeModule, typeSetModule, NULL));
    type_cls->giveAttr("__mro__",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedClass, tp_mro)));
    type_cls->giveAttr("__subclasses__", new BoxedFunction(boxRTFunction((void*)typeSubclasses, UNKNOWN, 1)));
    type_cls->giveAttr("mro", new BoxedFunction(boxRTFunction((void*)typeMro, UNKNOWN, 1)));
    type_cls->freeze();

    none_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)noneRepr, STR, 1)));
    none_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)noneHash, UNKNOWN, 1)));
    none_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)noneNonzero, BOXED_BOOL, 1)));
    none_cls->freeze();

    module_cls->giveAttr("__new__",
                         new BoxedFunction(boxRTFunction((void*)moduleNew, UNKNOWN, 3, 1, false, false), { NULL }));
    module_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)moduleRepr, STR, 1)));
    module_cls->giveAttr("__dict__", dict_descr);
    module_cls->freeze();

    closure_cls->freeze();

    setupCAPI();

    // Can't set up object methods until we set up CAPI support:
    for (auto& md : object_methods) {
        object_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, object_cls));
    }
    object_cls->giveAttr("__class__", new (pyston_getset_cls) BoxedGetsetDescriptor(objectClass, objectSetClass, NULL));
    object_cls->freeze();

    setupBool();
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
    _PyUnicode_Init();
    setupDescr();
    setupTraceback();
    setupCode();

    function_cls->giveAttr("__dict__", dict_descr);
    function_cls->giveAttr("__name__", new (pyston_getset_cls) BoxedGetsetDescriptor(funcName, funcSetName, NULL));
    function_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)functionRepr, STR, 1)));
    function_cls->giveAttr("__module__", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                   offsetof(BoxedFunction, modname), false));
    function_cls->giveAttr(
        "__doc__", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFunction, doc), false));
    function_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)functionGet, UNKNOWN, 3)));
    function_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)functionCall, UNKNOWN, 1, 0, true, true)));
    function_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)functionNonzero, BOXED_BOOL, 1)));
    function_cls->giveAttr("func_code", new (pyston_getset_cls) BoxedGetsetDescriptor(functionCode, NULL, NULL));
    function_cls->giveAttr("func_defaults",
                           new (pyston_getset_cls) BoxedGetsetDescriptor(functionDefaults, NULL, NULL));
    function_cls->freeze();

    builtin_function_or_method_cls->giveAttr(
        "__module__",
        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedBuiltinFunctionOrMethod, modname)));
    builtin_function_or_method_cls->giveAttr(
        "__repr__", new BoxedFunction(boxRTFunction((void*)builtinFunctionOrMethodRepr, STR, 1)));
    builtin_function_or_method_cls->giveAttr(
        "__name__", new (pyston_getset_cls) BoxedGetsetDescriptor(builtinFunctionOrMethodName, NULL, NULL));
    builtin_function_or_method_cls->giveAttr(
        "__doc__",
        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedBuiltinFunctionOrMethod, doc), false));
    builtin_function_or_method_cls->freeze();

    instancemethod_cls->giveAttr(
        "__new__", new BoxedFunction(boxRTFunction((void*)instancemethodNew, UNKNOWN, 4, 1, false, false), { NULL }));
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
    attrwrapper_cls->giveAttr("__delitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::delitem, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("setdefault",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::setdefault, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr(
        "get", new BoxedFunction(boxRTFunction((void*)AttrWrapper::get, UNKNOWN, 3, 1, false, false), { None }));
    attrwrapper_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::str, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("__contains__",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::contains, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("keys", new BoxedFunction(boxRTFunction((void*)AttrWrapper::keys, LIST, 1)));
    attrwrapper_cls->giveAttr("values", new BoxedFunction(boxRTFunction((void*)AttrWrapper::values, LIST, 1)));
    attrwrapper_cls->giveAttr("items", new BoxedFunction(boxRTFunction((void*)AttrWrapper::items, LIST, 1)));
    // TODO: not quite right
    attrwrapper_cls->giveAttr("iterkeys", attrwrapper_cls->getattr("keys"));
    attrwrapper_cls->giveAttr("itervalues", attrwrapper_cls->getattr("values"));
    attrwrapper_cls->giveAttr("iteritems", attrwrapper_cls->getattr("items"));
    attrwrapper_cls->giveAttr("copy", new BoxedFunction(boxRTFunction((void*)AttrWrapper::copy, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::len, BOXED_INT, 1)));
    attrwrapper_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::iter, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("update", new BoxedFunction(boxRTFunction((void*)AttrWrapper::update, NONE, 2)));
    attrwrapper_cls->freeze();

    attrwrapperiter_cls->giveAttr("__hasnext__",
                                  new BoxedFunction(boxRTFunction((void*)AttrWrapperIter::hasnext, UNKNOWN, 1)));
    attrwrapperiter_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)AttrWrapperIter::next, UNKNOWN, 1)));
    attrwrapperiter_cls->freeze();

    setupBuiltins();
    _PyExc_Init();
    setupThread();
    setupGC();
    setupImport();
    setupPyston();

    PyType_Ready(&PyByteArrayIter_Type);
    PyType_Ready(&PyCapsule_Type);
    PyType_Ready(&PyCallIter_Type);

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
    init_socket();
    initunicodedata();
    initcStringIO();
    init_io();
    initzipimport();
    init_csv();
    init_ssl();
    init_sqlite3();

    // some additional setup to ensure weakrefs participate in our GC
    BoxedClass* weakref_ref_cls = &_PyWeakref_RefType;
    weakref_ref_cls->tp_alloc = PystonType_GenericAlloc;
    weakref_ref_cls->tp_dealloc = NULL;
    weakref_ref_cls->gc_visit = proxy_to_tp_traverse;
    weakref_ref_cls->simple_destructor = proxy_to_tp_clear;
    weakref_ref_cls->is_pyston_class = true;

    BoxedClass* weakref_proxy_cls = &_PyWeakref_ProxyType;
    weakref_proxy_cls->tp_alloc = PystonType_GenericAlloc;
    weakref_proxy_cls->tp_dealloc = NULL;
    weakref_proxy_cls->gc_visit = proxy_to_tp_traverse;
    weakref_proxy_cls->simple_destructor = proxy_to_tp_clear;
    weakref_proxy_cls->is_pyston_class = true;

    BoxedClass* weakref_callableproxy = &_PyWeakref_CallableProxyType;
    weakref_callableproxy->tp_alloc = PystonType_GenericAlloc;
    weakref_callableproxy->tp_dealloc = NULL;
    weakref_callableproxy->gc_visit = proxy_to_tp_traverse;
    weakref_callableproxy->simple_destructor = proxy_to_tp_clear;
    weakref_callableproxy->is_pyston_class = true;

    assert(object_cls->tp_setattro == PyObject_GenericSetAttr);
    assert(none_cls->tp_setattro == PyObject_GenericSetAttr);

    setupSysEnd();

    Stats::endOfInit();

    TRACK_ALLOCATIONS = true;
}

BoxedModule* createModule(const std::string& name, const std::string& fn, const char* doc) {
    assert(fn.size() && "probably wanted to set the fn to <stdin>?");
    BoxedModule* module = new BoxedModule(name, fn, doc);

    BoxedDict* d = getSysModulesDict();
    Box* b_name = boxStringPtr(&name);
    ASSERT(d->d.count(b_name) == 0, "%s", name.c_str());
    d->d[b_name] = module;

    return module;
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
}
}
