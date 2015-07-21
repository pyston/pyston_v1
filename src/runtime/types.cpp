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
#include <stdint.h>

#include "llvm/Support/raw_ostream.h"

#include "analysis/scoping_analysis.h"
#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/ast_interpreter.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/classobj.h"
#include "runtime/complex.h"
#include "runtime/dict.h"
#include "runtime/file.h"
#include "runtime/ics.h"
#include "runtime/iterobject.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
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
extern "C" void PyMarshal_Init();
extern "C" void initstrop();

namespace pyston {

void setupGC();

bool IN_SHUTDOWN = false;

std::vector<BoxedClass*> exception_types;

void FrameInfo::gcVisit(GCVisitor* visitor) {
    visitor->visit(boxedLocals);
    visitor->visit(exc.traceback);
    visitor->visit(exc.type);
    visitor->visit(exc.value);
    visitor->visit(frame_obj);
}

// Analogue of PyType_GenericAlloc (default tp_alloc), but should only be used for Pyston classes!
extern "C" PyObject* PystonType_GenericAlloc(BoxedClass* cls, Py_ssize_t nitems) noexcept {
    assert(cls);

    // See PyType_GenericAlloc for note about the +1 here:
    const size_t size = _PyObject_VAR_SIZE(cls, nitems + 1);

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
        for (auto b : *static_cast<BoxedTuple*>(cls->tp_mro)) {
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

    if (cls->tp_itemsize != 0)
        static_cast<BoxVar*>(rtn)->ob_size = nitems;

    PyObject_INIT(rtn, cls);
    assert(rtn->cls);

    return rtn;
}

extern "C" PyObject* PyType_GenericAlloc(PyTypeObject* type, Py_ssize_t nitems) noexcept {
    PyObject* obj;
    const size_t size = _PyObject_VAR_SIZE(type, nitems + 1);
    // I don't understand why there is a +1 in this method; _PyObject_NewVar doesn't do that.
    // CPython has the following comment:
    /* note that we need to add one, for the sentinel */
    // I think that regardless of the reasoning behind them having it, we should do what they do?

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
void* BoxVar::operator new(size_t size, BoxedClass* cls, size_t nitems) {
    ALLOC_STATS_VAR(cls);

    assert(cls);
    // See definition of BoxedTuple for some notes on why we need this special case:
    ASSERT(isSubclass(cls, tuple_cls) || cls->tp_basicsize >= size, "%s", cls->tp_name);
    assert(cls->tp_itemsize > 0);
    assert(cls->tp_alloc);

    void* mem = cls->tp_alloc(cls, nitems);
    RELEASE_ASSERT(mem, "");
    return mem;
}

void* Box::operator new(size_t size, BoxedClass* cls) {
    ALLOC_STATS(cls);

    assert(cls);
    ASSERT(cls->tp_basicsize >= size, "%s", cls->tp_name);
    assert(cls->tp_itemsize == 0);
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

    static BoxedString* hasnext_str = internStringImmortal("__hasnext__");
    CallattrFlags callattr_flags
        = {.cls_only = true, .null_on_nonexistent = null_on_nonexistent, .argspec = ArgPassSpec(0) };
    return ic->call(obj, hasnext_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
}

extern "C" PyObject* PyIter_Next(PyObject* iter) noexcept {
    if (iter->cls->tp_iternext != slot_tp_iternext) {
        PyObject* result;
        result = (*iter->cls->tp_iternext)(iter);
        if (result == NULL && PyErr_Occurred() && PyErr_ExceptionMatches(PyExc_StopIteration))
            PyErr_Clear();
        return result;
    }

    try {
        Box* hasnext = iter->hasnextOrNullIC();
        if (hasnext) {
            if (hasnext->nonzeroIC())
                return iter->cls->callNextIC(iter);
            else
                return NULL;
        } else {
            return iter->cls->callNextIC(iter);
        }
    } catch (ExcInfo e) {
        if (!e.matches(StopIteration))
            setCAPIException(e);
        return NULL;
    }
}

Box* BoxedClass::callNextIC(Box* obj) {
    assert(obj->cls == this);

    // This would work, but it would have been better to just call tp_iternext
    assert(this->tp_iternext == slot_tp_iternext);

    auto ic = next_ic.get();
    if (!ic) {
        ic = new CallattrIC();
        next_ic.reset(ic);
    }

    static BoxedString* next_str = internStringImmortal("next");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
    return ic->call(obj, next_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
}

Box* BoxedClass::callReprIC(Box* obj) {
    assert(obj->cls == this);

    auto ic = repr_ic.get();
    if (!ic) {
        ic = new CallattrIC();
        repr_ic.reset(ic);
    }

    static BoxedString* repr_str = internStringImmortal("__repr__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
    return ic->call(obj, repr_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
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

extern "C" BoxedFunctionBase::BoxedFunctionBase(CLFunction* f)
    : in_weakreflist(NULL), f(f), closure(NULL), ndefaults(0), defaults(NULL), modname(NULL), name(NULL), doc(NULL) {
    if (f->source) {
        assert(f->source->scoping->areGlobalsFromModule());
        Box* globals_for_name = f->source->parent_module;

        static BoxedString* name_str = internStringImmortal("__name__");
        this->modname = globals_for_name->getattr(name_str);
        this->doc = f->source->getDocString();
    } else {
        this->modname = PyString_InternFromString("__builtin__");
        this->doc = None;
    }

    assert(f->paramspec.num_defaults == ndefaults);
}

extern "C" BoxedFunctionBase::BoxedFunctionBase(CLFunction* f, std::initializer_list<Box*> defaults,
                                                BoxedClosure* closure, Box* globals)
    : in_weakreflist(NULL),
      f(f),
      closure(closure),
      globals(globals),
      ndefaults(0),
      defaults(NULL),
      modname(NULL),
      name(NULL),
      doc(NULL) {
    assert((!globals) == (!f->source || f->source->scoping->areGlobalsFromModule()));

    if (defaults.size()) {
        // make sure to initialize defaults first, since the GC behavior is triggered by ndefaults,
        // and a GC can happen within this constructor:
        this->defaults = new (defaults.size()) GCdArray();
        memcpy(this->defaults->elts, defaults.begin(), defaults.size() * sizeof(Box*));
        this->ndefaults = defaults.size();
    }

    if (f->source) {
        Box* globals_for_name = globals;
        if (!globals_for_name) {
            assert(f->source->scoping->areGlobalsFromModule());
            globals_for_name = f->source->parent_module;
        }

        static BoxedString* name_str = internStringImmortal("__name__");
        if (globals_for_name->cls == module_cls) {
            this->modname = globals_for_name->getattr(name_str);
        } else {
            this->modname = PyDict_GetItem(globals_for_name, name_str);
        }
        // It's ok for modname to be NULL

        this->doc = f->source->getDocString();
    } else {
        this->modname = PyString_InternFromString("__builtin__");
        this->doc = None;
    }

    assert(f->paramspec.num_defaults == ndefaults);
}

BoxedFunction::BoxedFunction(CLFunction* f) : BoxedFunction(f, {}) {
}

BoxedFunction::BoxedFunction(CLFunction* f, std::initializer_list<Box*> defaults, BoxedClosure* closure, Box* globals)
    : BoxedFunctionBase(f, defaults, closure, globals) {

    // TODO eventually we want this to assert(f->source), I think, but there are still
    // some builtin functions that are BoxedFunctions but really ought to be a type that
    // we don't have yet.
    if (f->source) {
        this->name = static_cast<BoxedString*>(boxString(f->source->getName()));
    }
}

BoxedBuiltinFunctionOrMethod::BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name, const char* doc)
    : BoxedBuiltinFunctionOrMethod(f, name, {}) {

    this->doc = doc ? boxString(doc) : None;
}

BoxedBuiltinFunctionOrMethod::BoxedBuiltinFunctionOrMethod(CLFunction* f, const char* name,
                                                           std::initializer_list<Box*> defaults, BoxedClosure* closure,
                                                           const char* doc)
    : BoxedFunctionBase(f, defaults, closure) {

    assert(name);
    this->name = static_cast<BoxedString*>(boxString(name));
    this->doc = doc ? boxString(doc) : None;
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

    if (f->globals)
        v->visit(f->globals);

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

std::string BoxedModule::name() {
    static BoxedString* name_str = internStringImmortal("__name__");
    Box* name = this->getattr(name_str);
    if (!name || name->cls != str_cls) {
        return "?";
    } else {
        BoxedString* sname = static_cast<BoxedString*>(name);
        return sname->s();
    }
}

BoxedString* BoxedModule::getStringConstant(llvm::StringRef ast_str, bool intern) {
    BoxedString*& r = str_constants[ast_str];
    if (intern)
        r = internStringMortal(ast_str);
    else if (!r)
        r = boxString(ast_str);
    return r;
}

Box* BoxedModule::getUnicodeConstant(llvm::StringRef ast_str) {
    Box*& r = unicode_constants[ast_str];
    if (!r)
        r = decodeUTF8StringPtr(ast_str);
    return r;
}

BoxedInt* BoxedModule::getIntConstant(int64_t n) {
    BoxedInt*& r = int_constants[n];
    if (!r)
        r = new BoxedInt(n);
    return r;
}

static int64_t getDoubleBits(double d) {
    int64_t rtn;
    static_assert(sizeof(rtn) == sizeof(d), "");
    memcpy(&rtn, &d, sizeof(d));
    return rtn;
}

BoxedFloat* BoxedModule::getFloatConstant(double d) {
    BoxedFloat*& r = float_constants[getDoubleBits(d)];
    if (!r)
        r = static_cast<BoxedFloat*>(boxFloat(d));
    return r;
}

Box* BoxedModule::getPureImaginaryConstant(double d) {
    Box*& r = imaginary_constants[getDoubleBits(d)];
    if (!r)
        r = createPureImaginary(d);
    return r;
}

Box* BoxedModule::getLongConstant(llvm::StringRef ast_str) {
    Box*& r = long_constants[ast_str];
    if (!r)
        r = createLong(ast_str);
    return r;
}

template <typename A, typename B, typename C> void visitContiguousMap(GCVisitor* v, ContiguousMap<A, B, C>& map) {
    v->visitRange((void* const*)&map.vector()[0], (void* const*)&map.vector()[map.size()]);
}

void BoxedModule::gcHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedModule* d = (BoxedModule*)b;

    visitContiguousMap(v, d->str_constants);
    visitContiguousMap(v, d->unicode_constants);
    visitContiguousMap(v, d->int_constants);
    visitContiguousMap(v, d->float_constants);
    visitContiguousMap(v, d->imaginary_constants);
    visitContiguousMap(v, d->long_constants);
}

// This mustn't throw; our IR generator generates calls to it without "invoke" even when there are exception handlers /
// finally-blocks in scope.
extern "C" Box* boxCLFunction(CLFunction* f, BoxedClosure* closure, Box* globals,
                              std::initializer_list<Box*> defaults) noexcept {
    STAT_TIMER(t0, "us_timer_boxclfunction", 10);

    if (closure)
        assert(closure->cls == closure_cls);

    return new BoxedFunction(f, defaults, closure, globals);
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

        if (b->cls->tp_flags & Py_TPFLAGS_HEAPTYPE) {
            BoxedHeapClass* heap_cls = static_cast<BoxedHeapClass*>(b->cls);
            BoxedHeapClass::SlotOffset* slotOffsets = heap_cls->slotOffsets();
            for (int i = 0; i < heap_cls->nslots(); i++) {
                v->visit(*((Box**)((char*)b + slotOffsets[i])));
            }
        }
    } else {
        assert(type_cls == NULL || b == type_cls);
    }
}

static Box* typeCallInner(CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                          Box** args, const std::vector<BoxedString*>* keyword_names);

static Box* typeTppCall(Box* self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                        Box** args, const std::vector<BoxedString*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    if (argspec.has_starargs) {
        // This would fail in typeCallInner
        rewrite_args = NULL;
    }

    Box** new_args = NULL;
    if (npassed_args >= 3) {
        new_args = (Box**)alloca(sizeof(Box*) * (npassed_args + 1 - 3));
    }

    RewriterVar* r_bind_obj = NULL;
    if (rewrite_args) {
        r_bind_obj = rewrite_args->obj;
        rewrite_args->obj = NULL;
    }

    ArgPassSpec new_argspec
        = bindObjIntoArgs(self, r_bind_obj, rewrite_args, argspec, arg1, arg2, arg3, args, new_args);

    return typeCallInner(rewrite_args, new_argspec, arg1, arg2, arg3, new_args, keyword_names);
}

static Box* typeCallInternal(BoxedFunctionBase* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                             Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    if (rewrite_args)
        assert(rewrite_args->func_guarded);

    static StatCounter slowpath_typecall("slowpath_typecall");
    slowpath_typecall.log();

    if (argspec.has_starargs)
        return callFunc(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    return typeCallInner(rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
}

// For use on __init__ return values
static void assertInitNone(Box* obj) {
    if (obj != None) {
        raiseExcHelper(TypeError, "__init__() should return None, not '%s'", getTypeName(obj));
    }
}

static PyObject* cpython_type_call(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept {
    PyObject* obj;

    if (type->tp_new == NULL) {
        PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", type->tp_name);
        return NULL;
    }

    obj = type->tp_new(type, args, kwds);
    if (obj != NULL) {
        /* Ugly exception: when the call was type(something),
         *            don't call tp_init on the result. */
        if (type == &PyType_Type && PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 1
            && (kwds == NULL || (PyDict_Check(kwds) && PyDict_Size(kwds) == 0)))
            return obj;
        /* If the returned object is not an instance of type,
         *            it won't be initialized. */
        if (!PyType_IsSubtype(obj->cls, type))
            return obj;
        type = obj->cls;
        if (PyType_HasFeature(type, Py_TPFLAGS_HAVE_CLASS) && type->tp_init != NULL
            && type->tp_init(obj, args, kwds) < 0) {
            Py_DECREF(obj);
            obj = NULL;
        }
    }
    return obj;
}

static PyObject* cpythonTypeCall(BoxedClass* type, PyObject* args, PyObject* kwds) {
    Box* r = cpython_type_call(type, args, kwds);
    if (!r)
        throwCAPIException();
    return r;
}

static Box* typeCallInner(CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                          Box** args, const std::vector<BoxedString*>* keyword_names) {
    int npassed_args = argspec.totalPassed();

    assert(argspec.num_args >= 1);
    Box* _cls = arg1;

    if (!isSubclass(_cls->cls, type_cls)) {
        raiseExcHelper(TypeError, "descriptor '__call__' requires a 'type' object but received an '%s'",
                       getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);

    if (cls->tp_new != object_cls->tp_new && cls->tp_new != slot_tp_new) {
        // Looks like we're calling an extension class and we're not going to be able to
        // separately rewrite the new + init calls.  But we can rewrite the fact that we
        // should just call the cpython version, which will end up working pretty well.
        ParamReceiveSpec paramspec(1, false, true, true);
        bool rewrite_success = false;
        Box* oarg1, *oarg2, *oarg3, ** oargs = NULL;
        rearrangeArguments(paramspec, NULL, "", NULL, rewrite_args, rewrite_success, argspec, arg1, arg2, arg3, args,
                           keyword_names, oarg1, oarg2, oarg3, oargs);
        assert(oarg1 == cls);

        if (!rewrite_success)
            rewrite_args = NULL;

        if (rewrite_args) {
            rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)cpythonTypeCall, rewrite_args->arg1,
                                                                 rewrite_args->arg2, rewrite_args->arg3);
            rewrite_args->out_success = true;
        }

        return cpythonTypeCall(cls, oarg2, oarg3);
    }

    RewriterVar* r_ccls = NULL;
    RewriterVar* r_new = NULL;
    RewriterVar* r_init = NULL;
    Box* new_attr, *init_attr;
    if (rewrite_args) {
        assert(!argspec.has_starargs);
        assert(argspec.num_args > 0);

        r_ccls = rewrite_args->arg1;
        // This is probably a duplicate, but it's hard to really convince myself of that.
        // Need to create a clear contract of who guards on what
        r_ccls->addGuard((intptr_t)arg1 /* = _cls */);


        if (!rewrite_args->args_guarded) {
            // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
            // already fit, either since the type inferencer could determine that,
            // or because they only need to fit into an UNKNOWN slot.

            if (npassed_args >= 1)
                rewrite_args->arg1->addAttrGuard(offsetof(Box, cls), (intptr_t)arg1->cls);
            if (npassed_args >= 2)
                rewrite_args->arg2->addAttrGuard(offsetof(Box, cls), (intptr_t)arg2->cls);
            if (npassed_args >= 3)
                rewrite_args->arg3->addAttrGuard(offsetof(Box, cls), (intptr_t)arg3->cls);
            for (int i = 3; i < npassed_args; i++) {
                RewriterVar* v = rewrite_args->args->getAttr((i - 3) * sizeof(Box*), Location::any());
                v->addAttrGuard(offsetof(Box, cls), (intptr_t)args[i - 3]->cls);
            }
            rewrite_args->args_guarded = true;
        }
    }

    static BoxedString* new_str = internStringImmortal("__new__");
    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls, rewrite_args->destination);
        // TODO: if tp_new != Py_CallPythonNew, call that instead?
        new_attr = typeLookup(cls, new_str, &grewrite_args);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            assert(new_attr);
            r_new = grewrite_args.out_rtn;
            r_new->addGuard((intptr_t)new_attr);
        }

        // Special-case functions to allow them to still rewrite:
        if (new_attr->cls != function_cls) {
            Box* descr_r = processDescriptorOrNull(new_attr, None, cls);
            if (descr_r) {
                new_attr = descr_r;
                rewrite_args = NULL;
            }
        }
    } else {
        new_attr = typeLookup(cls, new_str, NULL);
        new_attr = processDescriptor(new_attr, None, cls);
    }
    assert(new_attr && "This should always resolve");

    // typeCall is tricky to rewrite since it has complicated behavior: we are supposed to
    // call the __init__ method of the *result of the __new__ call*, not of the original
    // class.  (And only if the result is an instance of the original class, but that's not
    // even the tricky part here.)
    //
    // By the time we know the type of the result of __new__(), it's too late to add traditional
    // guards.  So, instead of doing that, we're going to add a guard that makes sure that __new__
    // has the property that __new__(kls) always returns an instance of kls.
    //
    // Whitelist a set of __new__ methods that we know work like this.  Most importantly: object.__new__.
    //
    // Most builtin classes behave this way, but not all!
    // Notably, "type" itself does not.  For instance, assuming M is a subclass of
    // type, type.__new__(M, 1) will return the int class, which is not an instance of M.

    // this is ok with not using StlCompatAllocator since we will manually register these objects with the GC
    static std::vector<Box*> allowable_news;
    if (allowable_news.empty()) {
        for (BoxedClass* allowed_cls : { object_cls, enumerate_cls, xrange_cls, tuple_cls, list_cls, dict_cls }) {
            auto new_obj = typeLookup(allowed_cls, new_str, NULL);
            gc::registerPermanentRoot(new_obj);
            allowable_news.push_back(new_obj);
        }
    }

    // For debugging, keep track of why we think we can rewrite this:
    enum { NOT_ALLOWED, VERIFIED, NO_INIT, TYPE_NEW_SPECIAL_CASE, } why_rewrite_allowed = NOT_ALLOWED;

    if (rewrite_args) {
        for (auto b : allowable_news) {
            if (b == new_attr) {
                why_rewrite_allowed = VERIFIED;
                break;
            }
        }

        if (cls == int_cls || cls == float_cls || cls == long_cls) {
            if (npassed_args == 1) {
                why_rewrite_allowed = VERIFIED;
            } else if (npassed_args == 2 && (arg2->cls == int_cls || arg2->cls == str_cls || arg2->cls == float_cls)) {
                why_rewrite_allowed = NO_INIT;
                rewrite_args->arg2->addAttrGuard(offsetof(Box, cls), (intptr_t)arg2->cls);
            }
        }

        if (cls == type_cls && argspec == ArgPassSpec(2))
            why_rewrite_allowed = TYPE_NEW_SPECIAL_CASE;

        if (why_rewrite_allowed == NOT_ALLOWED) {
            // Uncomment this to try to find __new__ functions that we could either white- or blacklist:
            // ASSERT(cls->is_user_defined || cls == type_cls, "Does '%s' have a well-behaved __new__?  if so, add to
            // allowable_news, otherwise add to the blacklist in this assert", cls->tp_name);
            rewrite_args = NULL;
        }
    }

    static BoxedString* init_str = internStringImmortal("__init__");
    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls, rewrite_args->destination);
        init_attr = typeLookup(cls, init_str, &grewrite_args);

        if (!grewrite_args.out_success)
            rewrite_args = NULL;
        else {
            if (init_attr) {
                r_init = grewrite_args.out_rtn;
                r_init->addGuard((intptr_t)init_attr);
            }
        }
    } else {
        init_attr = typeLookup(cls, init_str, NULL);
    }
    // The init_attr should always resolve as well, but doesn't yet

    Box* made;
    RewriterVar* r_made = NULL;

    ArgPassSpec new_argspec = argspec;

    if (rewrite_args) {
        if (cls->tp_new == object_cls->tp_new && cls->tp_init != object_cls->tp_init) {
            // Fast case: if we are calling object_new, we normally doesn't look at the arguments at all.
            // (Except in the case when init_attr != object_init, in which case object_new looks at the number
            // of arguments and throws an exception.)
            //
            // Another option is to rely on rewriting to make this fast, which would probably require adding
            // a custom internal callable to object.__new__
            made = objectNewNoArgs(cls);
            r_made = rewrite_args->rewriter->call(true, (void*)objectNewNoArgs, r_ccls);
        } else {
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_new, rewrite_args->destination);
            srewrite_args.args_guarded = rewrite_args->args_guarded;
            srewrite_args.func_guarded = true;

            int new_npassed_args = new_argspec.totalPassed();

            if (new_npassed_args >= 1)
                srewrite_args.arg1 = r_ccls;
            if (new_npassed_args >= 2)
                srewrite_args.arg2 = rewrite_args->arg2;
            if (new_npassed_args >= 3)
                srewrite_args.arg3 = rewrite_args->arg3;
            if (new_npassed_args >= 4)
                srewrite_args.args = rewrite_args->args;

            made = runtimeCallInternal(new_attr, &srewrite_args, new_argspec, cls, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                r_made = srewrite_args.out_rtn;
            }
        }

        ASSERT(made->cls == cls || why_rewrite_allowed == TYPE_NEW_SPECIAL_CASE
                   || (why_rewrite_allowed == NO_INIT && cls->tp_init == object_cls->tp_init),
               "We should only have allowed the rewrite to continue if we were guaranteed that made "
               "would have class cls!");
    } else {
        if (cls->tp_new == object_cls->tp_new && cls->tp_init != object_cls->tp_init)
            made = objectNewNoArgs(cls);
        else
            made = runtimeCallInternal(new_attr, NULL, new_argspec, cls, arg2, arg3, args, keyword_names);
    }

    assert(made);

    // Special-case (also a special case in CPython): if we just called type.__new__(arg), don't call __init__
    if (cls == type_cls && argspec == ArgPassSpec(2)) {
        if (rewrite_args) {
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_made;
        }
        return made;
    }

    // If __new__ returns a subclass, supposed to call that subclass's __init__.
    // If __new__ returns a non-subclass, not supposed to call __init__.
    if (made->cls != cls) {
        ASSERT(rewrite_args == NULL || (why_rewrite_allowed == NO_INIT && made->cls->tp_init == object_cls->tp_init
                                        && cls->tp_init == object_cls->tp_init),
               "We should only have allowed the rewrite to continue if we were guaranteed that "
               "made would have class cls!");

        if (!isSubclass(made->cls, cls)) {
            init_attr = NULL;
        } else {
            // We could have skipped the initial __init__ lookup
            init_attr = typeLookup(made->cls, init_str, NULL);
        }
    }

    if (init_attr && made->cls->tp_init != object_cls->tp_init) {
        // TODO apply the same descriptor special-casing as in callattr?

        Box* initrtn;
        // Attempt to rewrite the basic case:
        if (rewrite_args && init_attr->cls == function_cls) {
            // Note: this code path includes the descriptor logic
            CallRewriteArgs srewrite_args(rewrite_args->rewriter, r_init, rewrite_args->destination);
            srewrite_args.arg1 = r_made;
            if (npassed_args >= 2)
                srewrite_args.arg2 = rewrite_args->arg2;
            if (npassed_args >= 3)
                srewrite_args.arg3 = rewrite_args->arg3;
            if (npassed_args >= 4)
                srewrite_args.args = rewrite_args->args;
            srewrite_args.args_guarded = rewrite_args->args_guarded;
            srewrite_args.func_guarded = true;

            // initrtn = callattrInternal(cls, _init_str, INST_ONLY, &srewrite_args, argspec, made, arg2, arg3, args,
            // keyword_names);
            initrtn = runtimeCallInternal(init_attr, &srewrite_args, argspec, made, arg2, arg3, args, keyword_names);

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                rewrite_args->rewriter->call(true, (void*)assertInitNone, srewrite_args.out_rtn);
            }
        } else {
            rewrite_args = NULL;

            init_attr = processDescriptor(init_attr, made, cls);

            ArgPassSpec init_argspec = argspec;
            init_argspec.num_args--;

            int passed = init_argspec.totalPassed();

            // If we weren't passed the args array, it's not safe to index into it
            if (passed <= 2)
                initrtn = runtimeCallInternal(init_attr, NULL, init_argspec, arg2, arg3, NULL, NULL, keyword_names);
            else
                initrtn
                    = runtimeCallInternal(init_attr, NULL, init_argspec, arg2, arg3, args[0], &args[1], keyword_names);
        }
        assertInitNone(initrtn);
    } else {
        if (new_attr == NULL && npassed_args != 1) {
            // TODO not npassed args, since the starargs or kwargs could be null
            raiseExcHelper(TypeError, objectNewParameterTypeErrorMsg());
        }
    }

    if (rewrite_args) {
        rewrite_args->out_rtn = r_made;
        rewrite_args->out_success = true;
    }

    return made;
}

Box* typeCall(Box* obj, BoxedTuple* vararg, BoxedDict* kwargs) {
    assert(vararg->cls == tuple_cls);

    bool pass_kwargs = (kwargs && kwargs->d.size());

    int n = vararg->size();
    int args_to_pass = n + 1 + (pass_kwargs ? 1 : 0); // 1 for obj, 1 for kwargs

    Box** args = NULL;
    if (args_to_pass > 3)
        args = (Box**)alloca(sizeof(Box*) * (args_to_pass - 3));

    Box* arg1, *arg2, *arg3;
    arg1 = obj;
    for (int i = 0; i < n; i++) {
        getArg(i + 1, arg1, arg2, arg3, args) = vararg->elts[i];
    }

    if (pass_kwargs)
        getArg(n + 1, arg1, arg2, arg3, args) = kwargs;

    return typeCallInternal(NULL, NULL, ArgPassSpec(n + 1, 0, false, pass_kwargs), arg1, arg2, arg3, args, NULL);
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
        return PyDictProxy_New(obj->getAttrWrapper());
    abort();
}

static Box* typeSubDict(Box* obj, void* context) {
    if (obj->cls->instancesHaveHCAttrs())
        return obj->getAttrWrapper();
    if (obj->cls->instancesHaveDictAttrs())
        return obj->getDict();
    abort();
}

static void typeSubSetDict(Box* obj, Box* val, void* context) {
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

    v->visit(im->obj);
    v->visit(im->func);
    v->visit(im->im_class);
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

// This probably belongs in tuple.cpp?
extern "C" void tupleGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedTuple* t = (BoxedTuple*)b;
    v->visitRange((void* const*)&t->elts[0], (void* const*)&t->elts[t->size()]);
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
      *file_cls, *member_descriptor_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls, *property_cls,
      *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *pyston_getset_cls, *capi_getset_cls,
      *builtin_function_or_method_cls, *attrwrapperiter_cls, *set_cls, *frozenset_cls;

BoxedTuple* EmptyTuple;
}

extern "C" Box* createUserClass(BoxedString* name, Box* _bases, Box* _attr_dict) {
    ASSERT(_attr_dict->cls == dict_cls, "%s", getTypeName(_attr_dict));
    BoxedDict* attr_dict = static_cast<BoxedDict*>(_attr_dict);

    assert(_bases->cls == tuple_cls);
    BoxedTuple* bases = static_cast<BoxedTuple*>(_bases);

    Box* metaclass = NULL;
    metaclass = attr_dict->getOrNull(boxString("__metaclass__"));

    if (metaclass != NULL) {
    } else if (bases->size() > 0) {
        // TODO Apparently this is supposed to look up __class__, and if that throws
        // an error, then look up ob_type (aka cls)
        metaclass = bases->elts[0]->cls;
    } else {
        Box* gl = getGlobalsDict();
        metaclass = PyDict_GetItemString(gl, "__metaclass__");

        if (!metaclass) {
            metaclass = classobj_cls;
        }
    }
    assert(metaclass);

    try {
        Box* r = runtimeCall(metaclass, ArgPassSpec(3), name, _bases, _attr_dict, NULL, NULL);
        RELEASE_ASSERT(r, "");
        return r;
    } catch (ExcInfo e) {
        RELEASE_ASSERT(e.matches(BaseException), "");

        Box* msg = e.value;
        assert(msg);
        // TODO this is an extra Pyston check and I don't think we should have to do it:
        if (isSubclass(e.value->cls, BaseException)) {
            static BoxedString* message_str = internStringImmortal("message");
            msg = getattr(e.value, message_str);
        }

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

extern "C" Box* boxInstanceMethod(Box* obj, Box* func, Box* type) {
    static StatCounter num_ims("num_instancemethods");
    num_ims.log();

    return new BoxedInstanceMethod(obj, func, type);
}

extern "C" Box* boxUnboundInstanceMethod(Box* func, Box* type) {
    return new BoxedInstanceMethod(NULL, func, type);
}

extern "C" BoxedString* noneRepr(Box* v) {
    return boxString("None");
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
        return boxString("<built-in function repr>");
    if (v == len_obj)
        return boxString("<built-in function len>");
    if (v == hash_obj)
        return boxString("<built-in function hash>");
    if (v == range_obj)
        return boxString("<built-in function range>");
    if (v == abs_obj)
        return boxString("<built-in function abs>");
    if (v == min_obj)
        return boxString("<built-in function min>");
    if (v == max_obj)
        return boxString("<built-in function max>");
    if (v == open_obj)
        return boxString("<built-in function open>");
    if (v == id_obj)
        return boxString("<built-in function id>");
    if (v == chr_obj)
        return boxString("<built-in function chr>");
    if (v == ord_obj)
        return boxString("<built-in function ord>");
    RELEASE_ASSERT(false, "builtinFunctionOrMethodRepr not properly implemented");
}

extern "C" BoxedString* functionRepr(BoxedFunction* v) {
    return boxString("function");
}

static Box* functionGet(BoxedFunction* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == function_cls, "");

    if (inst == None)
        inst = NULL;
    return new BoxedInstanceMethod(inst, self, owner);
}

static Box* function_descr_get(Box* self, Box* inst, Box* owner) noexcept {
    RELEASE_ASSERT(self->cls == function_cls, "");

    if (inst == None)
        inst = NULL;
    return new BoxedInstanceMethod(inst, self, owner);
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
    if (func->name == NULL)
        return boxString("<unknown function name>");
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
    return codeForFunction(func);
}

static Box* functionDefaults(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    if (!func->ndefaults)
        return None;
    return BoxedTuple::create(func->ndefaults, &func->defaults->elts[0]);
}

static Box* functionGlobals(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    if (func->globals) {
        assert(!func->f->source || !func->f->source->scoping->areGlobalsFromModule());
        return func->globals;
    }
    assert(func->f->source);
    assert(func->f->source->scoping->areGlobalsFromModule());

    static BoxedString* dict_str = internStringImmortal("__dict__");
    return getattr(func->f->source->parent_module, dict_str);
}

static void functionSetDefaults(Box* b, Box* v, void*) {
    RELEASE_ASSERT(v, "can't delete __defaults__");

    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);

    if (v == None)
        v = EmptyTuple;

    if (!isSubclass(v->cls, tuple_cls)) {
        raiseExcHelper(TypeError, "__defaults__ must be set to a tuple object");
    }

    BoxedTuple* t = static_cast<BoxedTuple*>(v);

    if (t->size() == func->ndefaults) {
        for (int i = 0; i < func->ndefaults; i++) {
            func->defaults->elts[i] = t->elts[i];
        }
        return;
    } else {
        RELEASE_ASSERT(0, "can't change number of defaults on a function for now");
    }
    abort();
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

    if (!PyObject_IsSubclass(type, self->im_class)) {
        return self;
    }

    if (obj == None)
        obj = NULL;

    return new BoxedInstanceMethod(obj, self->func, self->im_class);
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

    return new BoxedInstanceMethod(self, func, classObj);
}

// Modified from cpython, Objects/object.c, instancemethod_repr
static Box* instancemethodRepr(Box* b) {
    assert(isSubclass(b->cls, instancemethod_cls));
    BoxedInstanceMethod* a = static_cast<BoxedInstanceMethod*>(b);
    Box* self = a->obj;
    Box* func = a->func;
    Box* klass = a->im_class;
    Box* funcname = NULL, * klassname = NULL, * result = NULL;
    const char* sfuncname = "?", * sklassname = "?";

    static BoxedString* name_str = internStringImmortal("__name__");
    funcname = getattrInternal(func, name_str, NULL);

    if (funcname != NULL) {
        if (!PyString_Check(funcname)) {
            funcname = NULL;
        } else
            sfuncname = PyString_AS_STRING(funcname);
    }

    if (klass == NULL) {
        klassname = NULL;
    } else {
        klassname = getattrInternal(klass, name_str, NULL);
        if (klassname != NULL) {
            if (!PyString_Check(klassname)) {
                klassname = NULL;
            } else {
                sklassname = PyString_AS_STRING(klassname);
            }
        }
    }

    if (self == NULL)
        result = PyString_FromFormat("<unbound method %s.%s>", sklassname, sfuncname);
    else {
        // This was a CPython comment: /* XXX Shouldn't use repr() here! */
        Box* selfrepr = repr(self);
        assert(PyString_Check(selfrepr));
        result = PyString_FromFormat("<bound method %s.%s of %s>", sklassname, sfuncname, PyString_AS_STRING(selfrepr));
    }
    return result;
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
    return boxStringTwine(llvm::Twine("slice(") + start->s() + ", " + stop->s() + ", " + step->s() + ")");
}

extern "C" int PySlice_GetIndices(PySliceObject* r, Py_ssize_t length, Py_ssize_t* start, Py_ssize_t* stop,
                                  Py_ssize_t* step) noexcept {
    /* XXX support long ints */
    if (r->step == Py_None) {
        *step = 1;
    } else {
        if (!PyInt_Check(r->step) && !PyLong_Check(r->step))
            return -1;
        *step = PyInt_AsSsize_t(r->step);
    }
    if (r->start == Py_None) {
        *start = *step < 0 ? length - 1 : 0;
    } else {
        if (!PyInt_Check(r->start) && !PyLong_Check(r->step))
            return -1;
        *start = PyInt_AsSsize_t(r->start);
        if (*start < 0)
            *start += length;
    }
    if (r->stop == Py_None) {
        *stop = *step < 0 ? -1 : length;
    } else {
        if (!PyInt_Check(r->stop) && !PyLong_Check(r->step))
            return -1;
        *stop = PyInt_AsSsize_t(r->stop);
        if (*stop < 0)
            *stop += length;
    }
    if (*stop > length)
        return -1;
    if (*start >= length)
        return -1;
    if (*step == 0)
        return -1;
    return 0;
}

extern "C" int PySlice_GetIndicesEx(PySliceObject* _r, Py_ssize_t length, Py_ssize_t* start, Py_ssize_t* stop,
                                    Py_ssize_t* step, Py_ssize_t* slicelength) noexcept {
    BoxedSlice* r = (BoxedSlice*)_r;
    RELEASE_ASSERT(r->cls == slice_cls, "");

    /* this is harder to get right than you might think */

    Py_ssize_t defstart, defstop;

    if (r->step == Py_None) {
        *step = 1;
    } else {
        if (!_PyEval_SliceIndex(r->step, step))
            return -1;
        if (*step == 0) {
            PyErr_SetString(PyExc_ValueError, "slice step cannot be zero");
            return -1;
        }
    }

    defstart = *step < 0 ? length - 1 : 0;
    defstop = *step < 0 ? -1 : length;

    if (r->start == Py_None) {
        *start = defstart;
    } else {
        if (!_PyEval_SliceIndex(r->start, start))
            return -1;
        if (*start < 0)
            *start += length;
        if (*start < 0)
            *start = (*step < 0) ? -1 : 0;
        if (*start >= length)
            *start = (*step < 0) ? length - 1 : length;
    }

    if (r->stop == Py_None) {
        *stop = defstop;
    } else {
        if (!_PyEval_SliceIndex(r->stop, stop))
            return -1;
        if (*stop < 0)
            *stop += length;
        if (*stop < 0)
            *stop = (*step < 0) ? -1 : 0;
        if (*stop >= length)
            *stop = (*step < 0) ? length - 1 : length;
    }

    if ((*step < 0 && *stop >= *start) || (*step > 0 && *start >= *stop)) {
        *slicelength = 0;
    } else if (*step == 1) { // Pyston change: added this branch to make the common step==1 case avoid the div:
        *slicelength = (*stop - *start - 1) + 1;
    } else if (*step < 0) {
        *slicelength = (*stop - *start + 1) / (*step) + 1;
    } else {
        *slicelength = (*stop - *start - 1) / (*step) + 1;
    }

    return 0;
}

extern "C" PyObject* PySlice_New(PyObject* start, PyObject* stop, PyObject* step) noexcept {
    if (step == NULL)
        step = Py_None;
    if (start == NULL)
        start = Py_None;
    if (stop == NULL)
        stop = Py_None;
    return createSlice(start, stop, step);
}

Box* typeRepr(BoxedClass* self) {
    std::string O("");
    llvm::raw_string_ostream os(O);

    if ((self->tp_flags & Py_TPFLAGS_HEAPTYPE) && isUserDefined(self))
        os << "<class '";
    else
        os << "<type '";

    static BoxedString* module_str = internStringImmortal("__module__");
    Box* m = self->getattr(module_str);
    if (m && m->cls == str_cls) {
        BoxedString* sm = static_cast<BoxedString*>(m);
        if (sm->s() != "__builtin__")
            os << sm->s() << '.';
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
        static BoxedString* module_str = internStringImmortal("__module__");
        mod = type->getattr(module_str);
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

    static BoxedString* module_str = internStringImmortal("__module__");
    type->setattr(module_str, value, NULL);
}


Box* typeHash(BoxedClass* self) {
    assert(isSubclass(self->cls, type_cls));
    return boxInt(_Py_HashPointer(self));
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

Box* moduleInit(BoxedModule* self, Box* name, Box* doc) {
    RELEASE_ASSERT(isSubclass(self->cls, module_cls), "");
    RELEASE_ASSERT(name->cls == str_cls, "");
    RELEASE_ASSERT(!doc || doc->cls == str_cls, "");

    HCAttrs* attrs = self->getHCAttrsPtr();
    RELEASE_ASSERT(attrs->hcls->attributeArraySize() == 0, "");
    attrs->hcls = HiddenClass::makeSingleton();

    self->giveAttr("__name__", name);
    self->giveAttr("__doc__", doc ? doc : boxString(""));

    return None;
}

Box* moduleRepr(BoxedModule* m) {
    RELEASE_ASSERT(isSubclass(m->cls, module_cls), "");

    std::string O("");
    llvm::raw_string_ostream os(O);

    os << "<module '" << m->name() << "' ";

    const char* filename = PyModule_GetFilename((PyObject*)m);
    // TODO(kmod): builtin modules are not supposed to have a __file__ attribute
    if (!filename || !strcmp(filename, "__builtin__")) {
        PyErr_Clear();
        os << "(built-in)>";
    } else {
        os << "from '" << filename << "'>";
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
    llvm::DenseMap<BoxedString*, int>::const_iterator it;

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
        assert(b->getHCAttrsPtr()->hcls->type == HiddenClass::NORMAL
               || b->getHCAttrsPtr()->hcls->type == HiddenClass::SINGLETON);
    }

    DEFAULT_CLASS(attrwrapper_cls);

    Box* getUnderlying() { return b; }

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
        internStringMortalInplace(key);

        self->b->setattr(key, value, NULL);
        return None;
    }

    static Box* setdefault(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        internStringMortalInplace(key);

        Box* cur = self->b->getattr(key);
        if (cur)
            return cur;
        self->b->setattr(key, value, NULL);
        return value;
    }

    static Box* get(Box* _self, Box* _key, Box* def) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        internStringMortalInplace(key);

        Box* r = self->b->getattr(key);
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
        internStringMortalInplace(key);

        Box* r = self->b->getattr(key);
        if (!r)
            raiseExcHelper(KeyError, "'%s'", key->data());
        return r;
    }

    static Box* pop(Box* _self, Box* _key, Box* default_) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        internStringMortalInplace(key);

        Box* r = self->b->getattr(key);
        if (r) {
            self->b->delattr(key, NULL);
            return r;
        } else {
            if (default_)
                return default_;
            raiseExcHelper(KeyError, "'%s'", key->data());
        }
    }

    static Box* delitem(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        _key = coerceUnicodeToStr(_key);

        RELEASE_ASSERT(_key->cls == str_cls, "%s", _key->cls->tp_name);
        BoxedString* key = static_cast<BoxedString*>(_key);
        internStringMortalInplace(key);

        if (self->b->getattr(key))
            self->b->delattr(key, NULL);
        else
            raiseExcHelper(KeyError, "'%s'", key->data());
        return None;
    }

    static Box* str(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        std::string O("");
        llvm::raw_string_ostream os(O);

        os << "attrwrapper({";

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        bool first = true;
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            if (!first)
                os << ", ";
            first = false;

            BoxedString* v = attrs->attr_list->attrs[p.second]->reprICAsString();
            os << p.first->s() << ": " << v->s();
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
        internStringMortalInplace(key);

        Box* r = self->b->getattr(key);
        return r ? True : False;
    }

    static Box* keys(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            listAppend(rtn, p.first);
        }
        return rtn;
    }

    static Box* values(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            listAppend(rtn, attrs->attr_list->attrs[p.second]);
        }
        return rtn;
    }

    static Box* items(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            BoxedTuple* t = BoxedTuple::create({ p.first, attrs->attr_list->attrs[p.second] });
            listAppend(rtn, t);
        }
        return rtn;
    }

    static Box* iterkeys(Box* _self) {
        Box* r = AttrWrapper::keys(_self);
        return getiter(r);
    }

    static Box* itervalues(Box* _self) {
        Box* r = AttrWrapper::values(_self);
        return getiter(r);
    }

    static Box* iteritems(Box* _self) {
        Box* r = AttrWrapper::items(_self);
        return getiter(r);
    }

    static Box* copy(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        BoxedDict* rtn = new BoxedDict();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            rtn->d[p.first] = attrs->attr_list->attrs[p.second];
        }
        return rtn;
    }

    static Box* clear(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");

        // Clear the attrs array:
        new ((void*)attrs) HCAttrs(root_hcls);
        // Add the existing attrwrapper object (ie self) back as the attrwrapper:
        self->b->appendNewHCAttr(self, NULL);
        attrs->hcls = attrs->hcls->getAttrwrapperChild();

        return None;
    }

    static Box* len(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        return boxInt(attrs->hcls->getStrAttrOffsets().size());
    }

    static Box* update(Box* _self, BoxedTuple* args, BoxedDict* kwargs) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        assert(args->cls == tuple_cls);
        assert(kwargs);
        assert(kwargs->cls == dict_cls);

        RELEASE_ASSERT(args->size() <= 1, ""); // should throw a TypeError

        auto handle = [&](Box* _container) {
            if (_container->cls == attrwrapper_cls) {
                AttrWrapper* container = static_cast<AttrWrapper*>(_container);
                HCAttrs* attrs = container->b->getHCAttrsPtr();

                RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON,
                               "");
                for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
                    self->b->setattr(p.first, attrs->attr_list->attrs[p.second], NULL);
                }
            } else {
                // The update rules are too complicated to be worth duplicating here;
                // just create a new dict object and defer to dictUpdate.
                // Hopefully this does not happen very often.
                if (!isSubclass(_container->cls, dict_cls)) {
                    BoxedDict* new_container = new BoxedDict();
                    dictUpdate(new_container, BoxedTuple::create({ _container }), new BoxedDict());
                    _container = new_container;
                }
                assert(isSubclass(_container->cls, dict_cls));
                BoxedDict* container = static_cast<BoxedDict*>(_container);

                for (const auto& p : container->d) {
                    AttrWrapper::setitem(self, p.first, p.second);
                }
            }
        };

        for (auto e : *args) {
            handle(e);
        }
        handle(kwargs);

        return None;
    }

    static Box* iter(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        return new AttrWrapperIter(self);
    }

    static Box* eq(Box* _self, Box* _other) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        // In order to not have to reimplement dict cmp: just create a real dict for now and us it.
        BoxedDict* dict = (BoxedDict*)AttrWrapper::copy(_self);
        assert(dict->cls == dict_cls);
        static BoxedString* eq_str = internStringImmortal("__eq__");
        return callattrInternal(dict, eq_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(1), _other, NULL, NULL, NULL,
                                NULL);
    }

    static Box* ne(Box* _self, Box* _other) { return eq(_self, _other) == True ? False : True; }

    friend class AttrWrapperIter;
};

AttrWrapperIter::AttrWrapperIter(AttrWrapper* aw) {
    hcls = aw->b->getHCAttrsPtr()->hcls;
    assert(hcls);
    RELEASE_ASSERT(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON, "");
    it = hcls->getStrAttrOffsets().begin();
}

Box* AttrWrapperIter::hasnext(Box* _self) {
    RELEASE_ASSERT(_self->cls == attrwrapperiter_cls, "");
    AttrWrapperIter* self = static_cast<AttrWrapperIter*>(_self);
    RELEASE_ASSERT(self->hcls->type == HiddenClass::NORMAL || self->hcls->type == HiddenClass::SINGLETON, "");

    return boxBool(self->it != self->hcls->getStrAttrOffsets().end());
}

Box* AttrWrapperIter::next(Box* _self) {
    RELEASE_ASSERT(_self->cls == attrwrapperiter_cls, "");
    AttrWrapperIter* self = static_cast<AttrWrapperIter*>(_self);
    RELEASE_ASSERT(self->hcls->type == HiddenClass::NORMAL || self->hcls->type == HiddenClass::SINGLETON, "");

    assert(self->it != self->hcls->getStrAttrOffsets().end());
    Box* r = self->it->first;
    ++self->it;
    return r;
}

Box* Box::getAttrWrapper() {
    assert(cls->instancesHaveHCAttrs());
    HCAttrs* attrs = getHCAttrsPtr();
    HiddenClass* hcls = attrs->hcls;

    if (hcls->type == HiddenClass::DICT_BACKED) {
        return attrs->attr_list->attrs[0];
    }

    int offset = hcls->getAttrwrapperOffset();
    if (offset == -1) {
        Box* aw = new AttrWrapper(this);
        if (hcls->type == HiddenClass::NORMAL) {
            auto new_hcls = hcls->getAttrwrapperChild();
            appendNewHCAttr(aw, NULL);
            attrs->hcls = new_hcls;
            return aw;
        } else {
            assert(hcls->type == HiddenClass::SINGLETON);
            appendNewHCAttr(aw, NULL);
            hcls->appendAttrwrapper();
            return aw;
        }
    }
    return attrs->attr_list->attrs[offset];
}

Box* unwrapAttrWrapper(Box* b) {
    assert(b->cls == attrwrapper_cls);
    return static_cast<AttrWrapper*>(b)->getUnderlying();
}

Box* attrwrapperKeys(Box* b) {
    return AttrWrapper::keys(b);
}

void attrwrapperDel(Box* b, llvm::StringRef attr) {
    AttrWrapper::delitem(b, boxString(attr));
}

Box* objectNewNoArgs(BoxedClass* cls) {
    assert(isSubclass(cls->cls, type_cls));
#ifndef NDEBUG
    static BoxedString* new_str = internStringImmortal("__new__");
    static BoxedString* init_str = internStringImmortal("__init__");
    assert(typeLookup(cls, new_str, NULL) == typeLookup(object_cls, new_str, NULL)
           && typeLookup(cls, init_str, NULL) != typeLookup(object_cls, init_str, NULL));
#endif
    return new (cls) Box();
}

static int excess_args(PyObject* args, PyObject* kwds) noexcept {
    return PyTuple_GET_SIZE(args) || (kwds && PyDict_Check(kwds) && PyDict_Size(kwds));
}

static PyObject* object_new(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept;

static int object_init(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    int err = 0;
    if (excess_args(args, kwds)) {
        PyTypeObject* type = Py_TYPE(self);
        if (type->tp_init != object_init && type->tp_new != object_new) {
            err = PyErr_WarnEx(PyExc_DeprecationWarning, "object.__init__() takes no parameters", 1);
        } else if (type->tp_init != object_init || type->tp_new == object_new) {
            PyErr_SetString(PyExc_TypeError, "object.__init__() takes no parameters");
            err = -1;
        }
    }
    return err;
}

static PyObject* object_new(PyTypeObject* type, PyObject* args, PyObject* kwds) noexcept {
    int err = 0;
    if (excess_args(args, kwds)) {
        if (type->tp_new != object_new && type->tp_init != object_init) {
            err = PyErr_WarnEx(PyExc_DeprecationWarning, "object() takes no parameters", 1);
        } else if (type->tp_new != object_new || type->tp_init == object_init) {
            PyErr_SetString(PyExc_TypeError, "object() takes no parameters");
            err = -1;
        }
    }
    if (err < 0)
        return NULL;

    if (type->tp_flags & Py_TPFLAGS_IS_ABSTRACT) {
        // I don't know what this is or when it happens, but
        // CPython does something special with it
        Py_FatalError("unimplemented");
    }
    return type->tp_alloc(type, 0);
}

Box* objectRepr(Box* obj) {
    char buf[80];
    if (obj->cls == type_cls) {
        snprintf(buf, 80, "<type '%s'>", getNameOfClass(static_cast<BoxedClass*>(obj)));
    } else {
        snprintf(buf, 80, "<%s object at %p>", getTypeName(obj), obj);
    }
    return boxString(buf);
}

Box* objectStr(Box* obj) {
    return obj->reprIC();
}

Box* objectHash(Box* obj) {
    return boxInt(_Py_HashPointer(obj));
}

Box* objectSetattr(Box* obj, Box* attr, Box* value) {
    attr = coerceUnicodeToStr(attr);
    if (attr->cls != str_cls) {
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", attr->cls->tp_name);
    }

    BoxedString* attr_str = static_cast<BoxedString*>(attr);
    setattrGeneric(obj, attr_str, value, NULL);
    return None;
}

Box* objectSubclasshook(Box* cls, Box* a) {
    return NotImplemented;
}

static PyObject* import_copyreg(void) noexcept {
    static PyObject* copyreg_str;

    if (!copyreg_str) {
        // this is interned in cpython:
        copyreg_str = PyGC_AddRoot(PyString_FromString("copy_reg"));
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
        } else {
            // Pyston change: convert attrwrapper to a real dict
            if (state->cls == attrwrapper_cls) {
                PyObject* real_dict = PyDict_New();
                PyDict_Update(real_dict, state);
                state = real_dict;
            }
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

/*
   from PEP 3101, this code implements:

   class object:
       def __format__(self, format_spec):
       if isinstance(format_spec, str):
           return format(str(self), format_spec)
       elif isinstance(format_spec, unicode):
           return format(unicode(self), format_spec)
*/
static PyObject* object_format(PyObject* self, PyObject* args) noexcept {
    PyObject* format_spec;
    PyObject* self_as_str = NULL;
    PyObject* result = NULL;
    Py_ssize_t format_len;

    if (!PyArg_ParseTuple(args, "O:__format__", &format_spec))
        return NULL;
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(format_spec)) {
        format_len = PyUnicode_GET_SIZE(format_spec);
        self_as_str = PyObject_Unicode(self);
    } else if (PyString_Check(format_spec)) {
#else
    if (PyString_Check(format_spec)) {
#endif
        format_len = PyString_GET_SIZE(format_spec);
        self_as_str = PyObject_Str(self);
    } else {
        PyErr_SetString(PyExc_TypeError, "argument to __format__ must be unicode or str");
        return NULL;
    }

    if (self_as_str != NULL) {
        /* Issue 7994: If we're converting to a string, we
           should reject format specifications */
        if (format_len > 0) {
            if (PyErr_WarnEx(PyExc_PendingDeprecationWarning, "object.__format__ with a non-empty format "
                                                              "string is deprecated",
                             1) < 0) {
                goto done;
            }
            /* Eventually this will become an error:
            PyErr_Format(PyExc_TypeError,
               "non-empty format string passed to object.__format__");
            goto done;
            */
        }
        result = PyObject_Format(self_as_str, format_spec);
    }

done:
    Py_XDECREF(self_as_str);

    return result;
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
    for (auto b : *static_cast<BoxedTuple*>(obj->cls->tp_mro)) {
        BoxedClass* base = static_cast<BoxedClass*>(b);
        RELEASE_ASSERT(base->is_pyston_class, "");
    }
    for (auto b : *static_cast<BoxedTuple*>(new_cls->tp_mro)) {
        BoxedClass* base = static_cast<BoxedClass*>(b);
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
    { "__format__", object_format, METH_VARARGS, PyDoc_STR("default object formatter") },
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
    if (strlen(s->data()) != s->size()) {
        raiseExcHelper(ValueError, "__name__ must not contain null bytes");
    }

    BoxedHeapClass* ht = static_cast<BoxedHeapClass*>(type);
    ht->ht_name = s;
    ht->tp_name = s->data();
}

static Box* typeBases(Box* b, void*) {
    RELEASE_ASSERT(isSubclass(b->cls, type_cls), "");
    BoxedClass* type = static_cast<BoxedClass*>(b);

    assert(type->tp_bases);
    return type->tp_bases;
}

static void typeSetBases(Box* b, Box* v, void* c) {
    RELEASE_ASSERT(isSubclass(b->cls, type_cls), "");
    BoxedClass* type = static_cast<BoxedClass*>(b);
    if (type_set_bases(type, v, c) == -1)
        throwCAPIException();
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

extern "C" void PyCallIter_AddHasNext();

extern "C" PyVarObject* PyObject_InitVar(PyVarObject* op, PyTypeObject* tp, Py_ssize_t size) noexcept {
    assert(op);
    assert(tp);

    assert(gc::isValidGCMemory(op));
    assert(gc::isValidGCObject(tp));

    Py_TYPE(op) = tp;
    op->ob_size = size;

    gc::registerPythonObject(op);

    return op;
}

extern "C" PyObject* PyObject_Init(PyObject* op, PyTypeObject* tp) noexcept {
    assert(op);
    assert(tp);

    assert(gc::isValidGCMemory(op));
    assert(gc::isValidGCObject(tp));

    Py_TYPE(op) = tp;

    gc::registerPythonObject(op);

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

#ifndef NDEBUG
    if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        BoxedHeapClass* heap_cls = static_cast<BoxedHeapClass*>(tp);
        if (heap_cls->nslots() > 0) {
            BoxedHeapClass::SlotOffset* slotOffsets = heap_cls->slotOffsets();
            for (int i = 0; i < heap_cls->nslots(); i++) {
                // This should be set to 0 on allocation:
                // (If it wasn't, we would need to initialize it to 0 here.)
                assert(*(Box**)((char*)op + slotOffsets[i]) == NULL);
            }
        }
    }
#endif

    return op;
}

Box* decodeUTF8StringPtr(llvm::StringRef s) {
    Box* rtn = PyUnicode_DecodeUTF8(s.data(), s.size(), "strict");
    if (!rtn)
        throwCAPIException();
    assert(rtn);
    return rtn;
}

static PyObject* type_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    PyObject* result;
    Py_uintptr_t vv, ww;
    int c;

    /* Make sure both arguments are types. */
    if (!PyType_Check(v) || !PyType_Check(w) ||
        /* If there is a __cmp__ method defined, let it be called instead
           of our dumb function designed merely to warn.  See bug
           #7491. */
        Py_TYPE(v)->tp_compare || Py_TYPE(w)->tp_compare) {
        result = Py_NotImplemented;
        goto out;
    }

    /* Py3K warning if comparison isn't == or !=  */
    if (Py_Py3kWarningFlag && op != Py_EQ && op != Py_NE
        && PyErr_WarnEx(PyExc_DeprecationWarning, "type inequality comparisons not supported "
                                                  "in 3.x",
                        1) < 0) {
        return NULL;
    }

    /* Compare addresses */
    vv = (Py_uintptr_t)v;
    ww = (Py_uintptr_t)w;
    switch (op) {
        case Py_LT:
            c = vv < ww;
            break;
        case Py_LE:
            c = vv <= ww;
            break;
        case Py_EQ:
            c = vv == ww;
            break;
        case Py_NE:
            c = vv != ww;
            break;
        case Py_GT:
            c = vv > ww;
            break;
        case Py_GE:
            c = vv >= ww;
            break;
        default:
            result = Py_NotImplemented;
            goto out;
    }
    result = c ? Py_True : Py_False;

/* incref and return */
out:
    Py_INCREF(result);
    return result;
}

void unicode_visit(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    PyUnicodeObject* u = (PyUnicodeObject*)b;
    v->visit(u->str);
    v->visit(u->defenc);
}

extern "C" PyUnicodeObject* unicode_empty;
extern "C" PyUnicodeObject* _PyUnicode_New(Py_ssize_t length) noexcept {
    PyUnicodeObject* unicode;

    /* Optimization for empty strings */
    if (length == 0 && unicode_empty != NULL) {
        Py_INCREF(unicode_empty);
        return unicode_empty;
    }

    /* Ensure we won't overflow the size. */
    if (length > ((PY_SSIZE_T_MAX / sizeof(Py_UNICODE)) - 1)) {
        return (PyUnicodeObject*)PyErr_NoMemory();
    }

    // Pyston change: allocate ->str first, so that if this allocation
    // causes a collection, we don't see a half-created unicode object:
    size_t new_size = sizeof(Py_UNICODE) * ((size_t)length + 1);
    Py_UNICODE* str = (Py_UNICODE*)gc_alloc(new_size, gc::GCKind::UNTRACKED);
    if (!str)
        return (PyUnicodeObject*)PyErr_NoMemory();

#if STAT_ALLOCATIONS
    {
        size_t size = sizeof(PyUnicodeObject);
        ALLOC_STATS(unicode_cls);
    }
#endif

    // Do a bunch of inlining + constant folding of this line of CPython's:
    // unicode = PyObject_New(PyUnicodeObject, &PyUnicode_Type);
    assert(PyUnicode_Type.tp_basicsize == sizeof(PyUnicodeObject)); // use the compile-time constant
    unicode = (PyUnicodeObject*)gc_alloc(sizeof(PyUnicodeObject), gc::GCKind::PYTHON);
    if (unicode == NULL)
        return (PyUnicodeObject*)PyErr_NoMemory();

    // Inline PyObject_INIT:
    assert(!PyType_SUPPORTS_WEAKREFS(&PyUnicode_Type));
    assert(!PyUnicode_Type.instancesHaveHCAttrs());
    assert(!PyUnicode_Type.instancesHaveDictAttrs());
    unicode->ob_type = (struct _typeobject*)&PyUnicode_Type;

    unicode->str = str;

    /* Initialize the first element to guard against cases where
     * the caller fails before initializing str -- unicode_resize()
     * reads str[0], and the Keep-Alive optimization can keep memory
     * allocated for str alive across a call to unicode_dealloc(unicode).
     * We don't want unicode_resize to read uninitialized memory in
     * that case.
     */
    unicode->str[0] = 0;
    unicode->str[length] = 0;
    unicode->length = length;
    unicode->hash = -1;
    unicode->defenc = NULL;
    return unicode;
}

// Normally we don't call the Python tp_ slots that are present to support
// CPython's reference-counted garbage collection.
static void setTypeGCProxy(BoxedClass* cls) {
    cls->tp_alloc = PystonType_GenericAlloc;
    cls->tp_free = default_free;
    cls->gc_visit = proxy_to_tp_traverse;
    cls->has_safe_tp_dealloc = true;
    cls->is_pyston_class = true;
}

// By calling this function on a class we assign it Pyston's GC handling
// and no finalizers.
static void setTypeGCNone(BoxedClass* cls) {
    cls->tp_alloc = PystonType_GenericAlloc;
    cls->tp_free = default_free;
    cls->tp_dealloc = dealloc_null;
    cls->has_safe_tp_dealloc = true;
    cls->is_pyston_class = true;
}

static void setupDefaultClassGCParticipation() {
    // some additional setup to ensure weakrefs participate in our GC
    setTypeGCProxy(&_PyWeakref_RefType);
    setTypeGCProxy(&_PyWeakref_ProxyType);
    setTypeGCProxy(&_PyWeakref_CallableProxyType);

    // This is an optimization to speed up the handling of unicode objects,
    // exception objects, regular expression objects, etc in garbage collection.
    // There's no reason to have them part of finalizer ordering.
    //
    // This is important in tests like django-template which allocates
    // hundreds of thousands of unicode strings.
    setTypeGCNone(unicode_cls);
    unicode_cls->gc_visit = unicode_visit;

    for (BoxedClass* cls : exception_types) {
        setTypeGCNone(cls);
    }

    for (int i = 0; Itertool_SafeDealloc_Types[i] != NULL; i++) {
        setTypeGCNone(Itertool_SafeDealloc_Types[i]);
    }

    setTypeGCNone(&Scanner_Type);
    setTypeGCNone(&Match_Type);
    setTypeGCNone(&Pattern_Type);
    setTypeGCNone(&PyCallIter_Type);
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
    type_cls->tp_itemsize = sizeof(BoxedHeapClass::SlotOffset);
    PyObject_Init(object_cls, type_cls);
    PyObject_Init(type_cls, type_cls);
    // XXX silly that we have to set this again
    new (&object_cls->attrs) HCAttrs(HiddenClass::makeSingleton());
    new (&type_cls->attrs) HCAttrs(HiddenClass::makeSingleton());

    none_cls = new (0) BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(Box), false, NULL);
    None = new (none_cls) Box();
    assert(None->cls);
    gc::registerPermanentRoot(None);

    // You can't actually have an instance of basestring
    basestring_cls = new (0) BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(Box), false, NULL);

    // We add 1 to the tp_basicsize of the BoxedString in order to hold the null byte at the end.
    // We use offsetof(BoxedString, s_data) as opposed to sizeof(BoxedString) so that we can
    // use the extra padding bytes at the end of the BoxedString.
    str_cls = new (0) BoxedHeapClass(basestring_cls, NULL, 0, 0, offsetof(BoxedString, s_data) + 1, false, NULL);
    str_cls->tp_flags |= Py_TPFLAGS_STRING_SUBCLASS;
    str_cls->tp_itemsize = sizeof(char);

    // Hold off on assigning names until str_cls is ready
    object_cls->tp_name = "object";
    BoxedString* boxed_type_name = static_cast<BoxedString*>(boxString("type"));
    BoxedString* boxed_basestring_name = static_cast<BoxedString*>(boxString("basestring"));
    BoxedString* boxed_str_name = static_cast<BoxedString*>(boxString("str"));
    BoxedString* boxed_none_name = static_cast<BoxedString*>(boxString("NoneType"));
    static_cast<BoxedHeapClass*>(type_cls)->ht_name = boxed_type_name;
    static_cast<BoxedHeapClass*>(basestring_cls)->ht_name = boxed_basestring_name;
    static_cast<BoxedHeapClass*>(str_cls)->ht_name = boxed_str_name;
    static_cast<BoxedHeapClass*>(none_cls)->ht_name = boxed_none_name;
    type_cls->tp_name = boxed_type_name->data();
    basestring_cls->tp_name = boxed_basestring_name->data();
    str_cls->tp_name = boxed_str_name->data();
    none_cls->tp_name = boxed_none_name->data();

    gc::enableGC();

    // It wasn't safe to add __base__ attributes until object+type+str are set up, so do that now:
    type_cls->giveAttr("__base__", object_cls);
    basestring_cls->giveAttr("__base__", object_cls);
    str_cls->giveAttr("__base__", basestring_cls);
    none_cls->giveAttr("__base__", object_cls);
    object_cls->giveAttr("__base__", None);


    // Not sure why CPython defines sizeof(PyTupleObject) to include one element,
    // but we copy that, which means we have to subtract that extra pointer to get the tp_basicsize:
    tuple_cls = new (0)
        BoxedHeapClass(object_cls, &tupleGCHandler, 0, 0, sizeof(BoxedTuple) - sizeof(Box*), false, boxString("tuple"));
    tuple_cls->tp_flags |= Py_TPFLAGS_TUPLE_SUBCLASS;
    tuple_cls->tp_itemsize = sizeof(Box*);
    tuple_cls->tp_mro = BoxedTuple::create({ tuple_cls, object_cls });
    EmptyTuple = BoxedTuple::create({});
    gc::registerPermanentRoot(EmptyTuple);
    list_cls = new (0) BoxedHeapClass(object_cls, &listGCHandler, 0, 0, sizeof(BoxedList), false, boxString("list"));
    list_cls->tp_flags |= Py_TPFLAGS_LIST_SUBCLASS;
    pyston_getset_cls = new (0)
        BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedGetsetDescriptor), false, boxString("getset"));
    attrwrapper_cls = new (0) BoxedHeapClass(object_cls, &AttrWrapper::gcHandler, 0, 0, sizeof(AttrWrapper), false,
                                             static_cast<BoxedString*>(boxString("attrwrapper")));
    dict_cls = new (0) BoxedHeapClass(object_cls, &dictGCHandler, 0, 0, sizeof(BoxedDict), false,
                                      static_cast<BoxedString*>(boxString("dict")));
    dict_cls->tp_flags |= Py_TPFLAGS_DICT_SUBCLASS;
    file_cls = new (0) BoxedHeapClass(object_cls, &BoxedFile::gcHandler, 0, offsetof(BoxedFile, weakreflist),
                                      sizeof(BoxedFile), false, static_cast<BoxedString*>(boxString("file")));
    int_cls = new (0)
        BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedInt), false, static_cast<BoxedString*>(boxString("int")));
    int_cls->tp_flags |= Py_TPFLAGS_INT_SUBCLASS;
    bool_cls = new (0)
        BoxedHeapClass(int_cls, NULL, 0, 0, sizeof(BoxedBool), false, static_cast<BoxedString*>(boxString("bool")));
    complex_cls = new (0) BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedComplex), false,
                                         static_cast<BoxedString*>(boxString("complex")));
    long_cls = new (0) BoxedHeapClass(object_cls, &BoxedLong::gchandler, 0, 0, sizeof(BoxedLong), false,
                                      static_cast<BoxedString*>(boxString("long")));
    long_cls->tp_flags |= Py_TPFLAGS_LONG_SUBCLASS;
    float_cls = new (0) BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedFloat), false,
                                       static_cast<BoxedString*>(boxString("float")));
    function_cls = new (0) BoxedHeapClass(object_cls, &functionGCHandler, offsetof(BoxedFunction, attrs),
                                          offsetof(BoxedFunction, in_weakreflist), sizeof(BoxedFunction), false,
                                          static_cast<BoxedString*>(boxString("function")));
    builtin_function_or_method_cls = new (0)
        BoxedHeapClass(object_cls, &functionGCHandler, 0, offsetof(BoxedBuiltinFunctionOrMethod, in_weakreflist),
                       sizeof(BoxedBuiltinFunctionOrMethod), false,
                       static_cast<BoxedString*>(boxString("builtin_function_or_method")));
    function_cls->tp_dealloc = builtin_function_or_method_cls->tp_dealloc = functionDtor;
    function_cls->has_safe_tp_dealloc = builtin_function_or_method_cls->has_safe_tp_dealloc = true;


    module_cls = new (0) BoxedHeapClass(object_cls, &BoxedModule::gcHandler, offsetof(BoxedModule, attrs), 0,
                                        sizeof(BoxedModule), false, static_cast<BoxedString*>(boxString("module")));
    member_descriptor_cls = new (0) BoxedHeapClass(object_cls, NULL, 0, 0, sizeof(BoxedMemberDescriptor), false,
                                                   static_cast<BoxedString*>(boxString("member_descriptor")));
    capifunc_cls = new (0) BoxedHeapClass(object_cls, BoxedCApiFunction::gcHandler, 0, 0, sizeof(BoxedCApiFunction),
                                          false, static_cast<BoxedString*>(boxString("capifunc")));
    method_cls = new (0)
        BoxedHeapClass(object_cls, BoxedMethodDescriptor::gcHandler, 0, 0, sizeof(BoxedMethodDescriptor), false,
                       static_cast<BoxedString*>(boxString("method")));
    wrapperobject_cls = new (0)
        BoxedHeapClass(object_cls, BoxedWrapperObject::gcHandler, 0, 0, sizeof(BoxedWrapperObject), false,
                       static_cast<BoxedString*>(boxString("method-wrapper")));
    wrapperdescr_cls = new (0)
        BoxedHeapClass(object_cls, BoxedWrapperDescriptor::gcHandler, 0, 0, sizeof(BoxedWrapperDescriptor), false,
                       static_cast<BoxedString*>(boxString("wrapper_descriptor")));

    EmptyString = new (0) BoxedString("");
    // Call InternInPlace rather than InternFromString since that will
    // probably try to return EmptyString
    PyString_InternInPlace((Box**)&EmptyString);
    for (int i = 0; i <= UCHAR_MAX; i++) {
        char c = (char)i;
        BoxedString* s = new (1) BoxedString(llvm::StringRef(&c, 1));
        PyString_InternInPlace((Box**)&s);
        characters[i] = s;
    }

    // Kind of hacky, but it's easier to manually construct the mro for a couple key classes
    // than try to make the MRO construction code be safe against say, tuple_cls not having
    // an mro (since the mro is stored as a tuple).
    object_cls->tp_mro = BoxedTuple::create({ object_cls });
    list_cls->tp_mro = BoxedTuple::create({ list_cls, object_cls });
    type_cls->tp_mro = BoxedTuple::create({ type_cls, object_cls });
    pyston_getset_cls->tp_mro = BoxedTuple::create({ pyston_getset_cls, object_cls });
    attrwrapper_cls->tp_mro = BoxedTuple::create({ attrwrapper_cls, object_cls });
    dict_cls->tp_mro = BoxedTuple::create({ dict_cls, object_cls });
    file_cls->tp_mro = BoxedTuple::create({ file_cls, object_cls });
    int_cls->tp_mro = BoxedTuple::create({ int_cls, object_cls });
    bool_cls->tp_mro = BoxedTuple::create({ bool_cls, object_cls });
    complex_cls->tp_mro = BoxedTuple::create({ complex_cls, object_cls });
    long_cls->tp_mro = BoxedTuple::create({ long_cls, object_cls });
    float_cls->tp_mro = BoxedTuple::create({ float_cls, object_cls });
    function_cls->tp_mro = BoxedTuple::create({ function_cls, object_cls });
    builtin_function_or_method_cls->tp_mro = BoxedTuple::create({ builtin_function_or_method_cls, object_cls });
    member_descriptor_cls->tp_mro = BoxedTuple::create({ member_descriptor_cls, object_cls });
    capifunc_cls->tp_mro = BoxedTuple::create({ capifunc_cls, object_cls });
    module_cls->tp_mro = BoxedTuple::create({ module_cls, object_cls });
    method_cls->tp_mro = BoxedTuple::create({ method_cls, object_cls });
    wrapperobject_cls->tp_mro = BoxedTuple::create({ wrapperobject_cls, object_cls });
    wrapperdescr_cls->tp_mro = BoxedTuple::create({ wrapperdescr_cls, object_cls });

    object_cls->tp_hash = (hashfunc)_Py_HashPointer;

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
    object_cls->tp_init = object_init;
    object_cls->tp_new = object_new;
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
    member_descriptor_cls->finishInitialization();
    module_cls->finishInitialization();
    capifunc_cls->finishInitialization();
    method_cls->finishInitialization();
    wrapperobject_cls->finishInitialization();
    wrapperdescr_cls->finishInitialization();

    str_cls->tp_flags |= Py_TPFLAGS_HAVE_NEWBUFFER;

    dict_descr = new (pyston_getset_cls) BoxedGetsetDescriptor(typeSubDict, typeSubSetDict, NULL);
    gc::registerPermanentRoot(dict_descr);
    type_cls->giveAttr("__dict__", new (pyston_getset_cls) BoxedGetsetDescriptor(typeDict, NULL, NULL));


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

    object_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)objectRepr, UNKNOWN, 1, 0, false, false)));
    object_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)objectStr, UNKNOWN, 1, 0, false, false)));
    object_cls->giveAttr("__subclasshook__",
                         boxInstanceMethod(object_cls,
                                           new BoxedFunction(boxRTFunction((void*)objectSubclasshook, UNKNOWN, 2)),
                                           object_cls));
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
    type_cls->tp_hash = (hashfunc)_Py_HashPointer;
    type_cls->giveAttr("__module__", new (pyston_getset_cls) BoxedGetsetDescriptor(typeModule, typeSetModule, NULL));
    type_cls->giveAttr("__mro__",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedClass, tp_mro)));
    type_cls->giveAttr("__subclasses__", new BoxedFunction(boxRTFunction((void*)typeSubclasses, UNKNOWN, 1)));
    type_cls->giveAttr("mro", new BoxedFunction(boxRTFunction((void*)typeMro, UNKNOWN, 1)));
    type_cls->tp_richcompare = type_richcompare;
    add_operators(type_cls);
    type_cls->freeze();
    type_cls->tpp_call = &typeTppCall;

    none_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)noneRepr, STR, 1)));
    none_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)noneNonzero, BOXED_BOOL, 1)));
    none_cls->tp_hash = (hashfunc)_Py_HashPointer;
    none_cls->freeze();

    module_cls->giveAttr("__init__",
                         new BoxedFunction(boxRTFunction((void*)moduleInit, UNKNOWN, 3, 1, false, false), { NULL }));
    module_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)moduleRepr, STR, 1)));
    module_cls->giveAttr("__dict__", dict_descr);
    module_cls->freeze();

    closure_cls->freeze();

    setupUnwinding();
    setupInterpreter();
    setupCAPI();

    // Can't set up object methods until we set up CAPI support:
    for (auto& md : object_methods) {
        object_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, object_cls));
    }
    object_cls->giveAttr("__class__", new (pyston_getset_cls) BoxedGetsetDescriptor(objectClass, objectSetClass, NULL));
    object_cls->freeze();
    assert(object_cls->tp_init == object_init);
    assert(object_cls->tp_new == object_new);

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
    setupFrame();

    function_cls->giveAttr("__dict__", dict_descr);
    function_cls->giveAttr("__name__", new (pyston_getset_cls) BoxedGetsetDescriptor(funcName, funcSetName, NULL));
    function_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)functionRepr, STR, 1)));
    function_cls->giveAttr("__module__", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                   offsetof(BoxedFunction, modname), false));
    function_cls->giveAttr(
        "__doc__", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFunction, doc), false));
    function_cls->giveAttr("func_doc", function_cls->getattr(internStringMortal("__doc__")));
    function_cls->giveAttr("__globals__", new (pyston_getset_cls) BoxedGetsetDescriptor(functionGlobals, NULL, NULL));
    function_cls->giveAttr("__get__", new BoxedFunction(boxRTFunction((void*)functionGet, UNKNOWN, 3)));
    function_cls->giveAttr("__call__",
                           new BoxedFunction(boxRTFunction((void*)functionCall, UNKNOWN, 1, 0, true, true)));
    function_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)functionNonzero, BOXED_BOOL, 1)));
    function_cls->giveAttr("func_code", new (pyston_getset_cls) BoxedGetsetDescriptor(functionCode, NULL, NULL));
    function_cls->giveAttr("__code__", function_cls->getattr(internStringMortal("func_code")));
    function_cls->giveAttr("func_name", function_cls->getattr(internStringMortal("__name__")));
    function_cls->giveAttr("func_defaults",
                           new (pyston_getset_cls) BoxedGetsetDescriptor(functionDefaults, functionSetDefaults, NULL));
    function_cls->giveAttr("__defaults__", function_cls->getattr(internStringMortal("func_defaults")));
    function_cls->giveAttr("func_globals", function_cls->getattr(internStringMortal("__globals__")));
    function_cls->freeze();
    function_cls->tp_descr_get = function_descr_get;

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
    instancemethod_cls->giveAttr("__func__", instancemethod_cls->getattr(internStringMortal("im_func")));
    instancemethod_cls->giveAttr(
        "im_self", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedInstanceMethod, obj)));
    instancemethod_cls->giveAttr("__self__", instancemethod_cls->getattr(internStringMortal("im_self")));
    instancemethod_cls->freeze();

    instancemethod_cls->giveAttr("im_class", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                       offsetof(BoxedInstanceMethod, im_class), true));

    slice_cls->giveAttr("__new__",
                        new BoxedFunction(boxRTFunction((void*)sliceNew, UNKNOWN, 4, 2, false, false), { NULL, None }));
    slice_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)sliceRepr, STR, 1)));
    slice_cls->giveAttr("start", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSlice, start)));
    slice_cls->giveAttr("stop", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSlice, stop)));
    slice_cls->giveAttr("step", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSlice, step)));
    slice_cls->freeze();

    attrwrapper_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::setitem, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr(
        "pop", new BoxedFunction(boxRTFunction((void*)AttrWrapper::pop, UNKNOWN, 3, 1, false, false), { NULL }));
    attrwrapper_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::getitem, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("__delitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::delitem, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("setdefault",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::setdefault, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr(
        "get", new BoxedFunction(boxRTFunction((void*)AttrWrapper::get, UNKNOWN, 3, 1, false, false), { None }));
    attrwrapper_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::str, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("__contains__",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::contains, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::eq, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::ne, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("keys", new BoxedFunction(boxRTFunction((void*)AttrWrapper::keys, LIST, 1)));
    attrwrapper_cls->giveAttr("values", new BoxedFunction(boxRTFunction((void*)AttrWrapper::values, LIST, 1)));
    attrwrapper_cls->giveAttr("items", new BoxedFunction(boxRTFunction((void*)AttrWrapper::items, LIST, 1)));
    attrwrapper_cls->giveAttr("iterkeys", new BoxedFunction(boxRTFunction((void*)AttrWrapper::iterkeys, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("itervalues",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::itervalues, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("iteritems", new BoxedFunction(boxRTFunction((void*)AttrWrapper::iteritems, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("copy", new BoxedFunction(boxRTFunction((void*)AttrWrapper::copy, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("clear", new BoxedFunction(boxRTFunction((void*)AttrWrapper::clear, NONE, 1)));
    attrwrapper_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::len, BOXED_INT, 1)));
    attrwrapper_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::iter, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("update",
                              new BoxedFunction(boxRTFunction((void*)AttrWrapper::update, NONE, 1, 0, true, true)));
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
    setupAST();

    PyType_Ready(&PyByteArrayIter_Type);
    PyType_Ready(&PyCapsule_Type);
    PyCallIter_AddHasNext();
    PyType_Ready(&PyCallIter_Type);
    PyType_Ready(&PyCObject_Type);
    PyType_Ready(&PyDictProxy_Type);

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
    PyMarshal_Init();
    initstrop();

    setupDefaultClassGCParticipation();

    assert(object_cls->tp_setattro == PyObject_GenericSetAttr);
    assert(none_cls->tp_setattro == PyObject_GenericSetAttr);

    assert(object_cls->tp_hash == (hashfunc)_Py_HashPointer);
    assert(none_cls->tp_hash == (hashfunc)_Py_HashPointer);
    assert(type_cls->tp_hash == (hashfunc)_Py_HashPointer);

    setupSysEnd();

    TRACK_ALLOCATIONS = true;
}

BoxedModule* createModule(const std::string& name, const char* fn, const char* doc) {
    assert((!fn || strlen(fn)) && "probably wanted to set the fn to <stdin>?");

    BoxedDict* d = getSysModulesDict();
    Box* b_name = boxString(name);

    // Surprisingly, there are times that we need to return the existing module if
    // one exists:
    Box* existing = d->getOrNull(b_name);
    if (existing && isSubclass(existing->cls, module_cls)) {
        return static_cast<BoxedModule*>(existing);
    }

    BoxedModule* module = new BoxedModule();
    moduleInit(module, boxString(name), boxString(doc ? doc : ""));
    if (fn)
        module->giveAttr("__file__", boxString(fn));

    d->d[b_name] = module;
    if (name == "__main__")
        module->giveAttr("__builtins__", builtins_module);
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
