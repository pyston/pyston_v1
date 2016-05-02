// Copyright (c) 2014-2016 Dropbox, Inc.
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

#define PY_SSIZE_T_CLEAN

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
#include "codegen/entry.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/code.h"
#include "runtime/complex.h"
#include "runtime/dict.h"
#include "runtime/hiddenclass.h"
#include "runtime/ics.h"
#include "runtime/inline/list.h"
#include "runtime/iterobject.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/set.h"
#include "runtime/super.h"
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
extern "C" void _PyWarnings_Init() noexcept;
extern "C" void _string_init();
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
extern "C" void initgc();
extern "C" void init_ast();
extern "C" void initimp();

namespace pyston {

std::vector<BoxedClass*> exception_types;

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
    ASSERT(cls->tp_basicsize >= size || isSubclass(cls, tuple_cls), "%s", cls->tp_name);
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

    static BoxedString* hasnext_str = getStaticString("__hasnext__");
    CallattrFlags callattr_flags
        = {.cls_only = true, .null_on_nonexistent = null_on_nonexistent, .argspec = ArgPassSpec(0) };
    return ic->call(obj, hasnext_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
}

extern "C" PyObject* PyIter_Next(PyObject* iter) noexcept {
    Box* result = NULL;
    if (iter->cls->tp_iternext != slot_tp_iternext)
        result = (*iter->cls->tp_iternext)(iter);
    else {
        try {
            Box* hasnext = iter->hasnextOrNullIC();
            AUTO_XDECREF(hasnext);
            if (hasnext && !hasnext->nonzeroIC())
                return NULL;
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
        result = iter->cls->call_nextIC(iter);
    }

    if (result == NULL && PyErr_Occurred() && PyErr_ExceptionMatches(PyExc_StopIteration))
        PyErr_Clear();
    return result;
}

Box* BoxedClass::call_nextIC(Box* obj) noexcept {
    assert(obj->cls == this);

    // This would work, but it would have been better to just call tp_iternext
    assert(this->tp_iternext == slot_tp_iternext);

    auto ic = next_ic.get();
    if (!ic) {
        ic = new CallattrCapiIC();
        next_ic.reset(ic);
    }

    static BoxedString* next_str = getStaticString("next");
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

    static BoxedString* repr_str = getStaticString("__repr__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
    return ic->call(obj, repr_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
}

Box* BoxedClass::callIterIC(Box* obj) {
    assert(obj->cls == this);

    auto ic = iter_ic.get();
    if (!ic) {
        ic = new CallattrIC();
        iter_ic.reset(ic);
    }

    static BoxedString* iter_str = getStaticString("__iter__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    return ic->call(obj, iter_str, callattr_flags, nullptr, nullptr, nullptr, nullptr, nullptr);
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
        r = PyUnicode_AsASCIIString(autoDecref(r));
        checkAndThrowCAPIException();
    }
    if (r->cls != str_cls) {
        Py_DECREF(r);
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

extern "C" BoxedFunctionBase::BoxedFunctionBase(FunctionMetadata* md)
    : weakreflist(NULL),
      md(md),
      closure(NULL),
      defaults(NULL),
      can_change_defaults(false),
      modname(NULL),
      name(NULL),
      doc(NULL) {

    if (md->source) {
        assert(md->source->scoping->areGlobalsFromModule());
        Box* globals_for_name = md->source->parent_module;

        assert(0 && "check the refcounting here");
        static BoxedString* name_str = getStaticString("__name__");
        this->modname = xincref(globals_for_name->getattr(name_str));
        this->doc = md->source->getDocString();
    } else {
        this->modname = PyString_InternFromString("__builtin__");
        this->doc = incref(None);
    }
}

extern "C" BoxedFunctionBase::BoxedFunctionBase(FunctionMetadata* md, std::initializer_list<Box*> defaults,
                                                BoxedClosure* closure, Box* globals, bool can_change_defaults)
    : weakreflist(NULL),
      md(md),
      closure(closure),
      globals(globals),
      defaults(NULL),
      can_change_defaults(can_change_defaults),
      modname(NULL),
      name(NULL),
      doc(NULL) {
    assert((!globals) == (!md->source || md->source->scoping->areGlobalsFromModule()));

    Py_XINCREF(closure);
    Py_XINCREF(globals);

    if (defaults.size()) {
        // HAX copy+modify the BoxedTuple constructor so that we can put NULLs into the tuple.
        // We are going to separately be careful to make sure that those NULLs don't escape
        // to the user (see functionDefaults)
        this->defaults = BoxedTuple::create(defaults.size());
        int i = 0;
        for (auto e : defaults) {
            Py_XINCREF(e);
            this->defaults->elts[i] = e;
            ++i;
        }
    }

    if (md->source) {
        auto parent_module = md->source->parent_module;
        Py_INCREF(parent_module);

        Box* globals_for_name = globals;
        if (!globals_for_name) {
            assert(md->source->scoping->areGlobalsFromModule());
            globals_for_name = md->source->parent_module;
        }

        static BoxedString* name_str = getStaticString("__name__");
        if (globals_for_name->cls == module_cls) {
            this->modname = xincref(globals_for_name->getattr(name_str));
        } else {
            this->modname = xincref(PyDict_GetItem(globals_for_name, name_str));
        }
        // It's ok for modname to be NULL

        this->doc = md->source->getDocString();
    } else {
        this->modname = PyString_InternFromString("__builtin__");
        this->doc = incref(None);
    }
}

BoxedFunction::BoxedFunction(FunctionMetadata* md) : BoxedFunction(md, {}) {
}

BoxedFunction::BoxedFunction(FunctionMetadata* md, std::initializer_list<Box*> defaults, BoxedClosure* closure,
                             Box* globals, bool can_change_defaults)
    : BoxedFunctionBase(md, defaults, closure, globals, can_change_defaults) {

    // TODO eventually we want this to assert(f->source), I think, but there are still
    // some builtin functions that are BoxedFunctions but really ought to be a type that
    // we don't have yet.
    if (md->source) {
        assert(!this->name);
        this->name = incref(static_cast<BoxedString*>(md->source->getName()));
    }
}

BoxedBuiltinFunctionOrMethod::BoxedBuiltinFunctionOrMethod(FunctionMetadata* md, const char* name, const char* doc)
    : BoxedBuiltinFunctionOrMethod(md, name, {}) {

    Py_DECREF(this->doc);
    this->doc = doc ? boxString(doc) : incref(None);
}

BoxedBuiltinFunctionOrMethod::BoxedBuiltinFunctionOrMethod(FunctionMetadata* md, const char* name,
                                                           std::initializer_list<Box*> defaults, BoxedClosure* closure,
                                                           const char* doc)
    : BoxedFunctionBase(md, defaults, closure) {

    assert(name);
    assert(!this->name);
    this->name = static_cast<BoxedString*>(boxString(name));
    Py_DECREF(this->doc);
    this->doc = doc ? boxString(doc) : incref(None);
}

static void functionDtor(Box* b) {
    assert(isSubclass(b->cls, function_cls) || isSubclass(b->cls, builtin_function_or_method_cls));

    BoxedFunctionBase* self = static_cast<BoxedFunctionBase*>(b);
    PyObject_GC_UnTrack(self);

    PyObject_ClearWeakRefs((PyObject*)self);

    self->dependent_ics.invalidateAll();
    self->dependent_ics.~ICInvalidator();

    self->clearAttrsForDealloc();

    Py_XDECREF(self->doc);
    Py_XDECREF(self->modname);
    Py_XDECREF(self->name);
    Py_XDECREF(self->closure);
    Py_XDECREF(self->globals);
    if (self->md->source) {
        auto parent_module = self->md->source->parent_module;
        Py_DECREF(parent_module);
    }
    Py_XDECREF(self->defaults);

    self->cls->tp_free(self);
}

static int func_traverse(BoxedFunction* f, visitproc visit, void* arg) noexcept {
    // Py_VISIT(f->func_code);
    Py_VISIT(f->globals);
    Py_VISIT(f->modname);
    Py_VISIT(f->defaults);
    Py_VISIT(f->doc);
    Py_VISIT(f->name);
    Py_VISIT(f->closure);

    if (f->md->source) {
        Py_VISIT(f->md->source->parent_module);
    }

    // Py_VISIT(f->func_dict);
    Py_TRAVERSE(f->attrs);
    return 0;
}

static int func_clear(Box* b) noexcept {
    assert(isSubclass(b->cls, function_cls));

    BoxedFunction* f = static_cast<BoxedFunction*>(b);

    Py_CLEAR(f->globals);
    Py_CLEAR(f->modname);
    Py_CLEAR(f->defaults);
    Py_CLEAR(f->doc);
    Py_CLEAR(f->name);
    Py_CLEAR(f->closure);

    // I think it's ok to not clear the parent_module?

    b->clearAttrsForDealloc();

    return 0;
}

static int builtin_func_traverse(BoxedBuiltinFunctionOrMethod* f, visitproc visit, void* arg) noexcept {
    // Py_VISIT(f->func_code);
    Py_VISIT(f->globals);
    Py_VISIT(f->modname);
    Py_VISIT(f->defaults);
    Py_VISIT(f->doc);
    Py_VISIT(f->name);
    Py_VISIT(f->closure);

    if (f->md->source) {
        Py_VISIT(f->md->source->parent_module);
    }

    return 0;
}

std::string BoxedModule::name() {
    static BoxedString* name_str = getStaticString("__name__");
    Box* name = this->getattr(name_str);
    if (!name || name->cls != str_cls) {
        return "?";
    } else {
        BoxedString* sname = static_cast<BoxedString*>(name);
        return sname->s();
    }
}

BORROWED(BoxedString*) BoxedModule::getStringConstant(llvm::StringRef ast_str, bool intern) {
    BoxedString*& r = str_constants[ast_str];
    if (intern) {
        // If we had previously created a box for this string, we have to create a new
        // string (or at least, be prepared to return a different value that we had already
        // interned).  This is fine, except we have to be careful because we promised
        // that we would keep the previously-created string alive.
        // So, make sure to put it onto the keep_alive list.
        if (r && !PyString_CHECK_INTERNED(r)) {
            assert(0 && "check refcounting");
            keep_alive.push_back(r);
            r = NULL;
        }
        if (!r)
            r = internStringMortal(ast_str);
    } else if (!r)
        r = boxString(ast_str);
    return r;
}

BORROWED(Box*) BoxedModule::getUnicodeConstant(llvm::StringRef ast_str) {
    Box*& r = unicode_constants[ast_str];
    if (!r)
        r = decodeUTF8StringPtr(ast_str);
    return r;
}

BORROWED(BoxedInt*) BoxedModule::getIntConstant(int64_t n) {
    BoxedInt*& r = int_constants[n];
    if (!r)
        r = (BoxedInt*)boxInt(n);
    return r;
}

static int64_t getDoubleBits(double d) {
    int64_t rtn;
    static_assert(sizeof(rtn) == sizeof(d), "");
    memcpy(&rtn, &d, sizeof(d));
    return rtn;
}

BORROWED(BoxedFloat*) BoxedModule::getFloatConstant(double d) {
    BoxedFloat*& r = float_constants[getDoubleBits(d)];
    if (!r)
        r = static_cast<BoxedFloat*>(boxFloat(d));
    return r;
}

BORROWED(Box*) BoxedModule::getPureImaginaryConstant(double d) {
    Box*& r = imaginary_constants[getDoubleBits(d)];
    if (!r)
        r = createPureImaginary(d);
    return r;
}

BORROWED(Box*) BoxedModule::getLongConstant(llvm::StringRef ast_str) {
    Box*& r = long_constants[ast_str];
    if (!r)
        r = createLong(ast_str);
    return r;
}

// This mustn't throw; our IR generator generates calls to it without "invoke" even when there are exception handlers /
// finally-blocks in scope.
extern "C" Box* createFunctionFromMetadata(FunctionMetadata* md, BoxedClosure* closure, Box* globals,
                                           std::initializer_list<Box*> defaults) noexcept {
    STAT_TIMER(t0, "us_timer_createfunctionfrommetadata", 10);

    if (closure)
        assert(closure->cls == closure_cls);

    return new BoxedFunction(md, defaults, closure, globals, /* can_change_defaults = */ true);
}

extern "C" FunctionMetadata* getFunctionMetadata(Box* b) {
    assert(isSubclass(b->cls, function_cls) || isSubclass(b->cls, builtin_function_or_method_cls));
    return static_cast<BoxedFunction*>(b)->md;
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

template <ExceptionStyle S>
static Box* typeCallInner(CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                          Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI);

template <ExceptionStyle S>
static Box* typeTppCall(Box* self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                        Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    int npassed_args = argspec.totalPassed();

    // Common CAPI path call this function with *args, **kw.
    if (argspec == ArgPassSpec(0, 0, true, false) || argspec == ArgPassSpec(0, 0, true, true)) {
        // Wouldn't be able to rewrite anyway:
        assert(!rewrite_args || !rewrite_args->out_success);

        arg1 = PySequence_Tuple(arg1);
        if (!arg1) {
            if (S == CAPI)
                return NULL;
            else
                throwCAPIException();
        }
        AUTO_DECREF(arg1);

        Box* r = cpython_type_call(static_cast<BoxedClass*>(self), arg1, argspec.has_kwargs ? arg2 : NULL);
        if (S == CXX && !r)
            throwCAPIException();
        return r;
    }

    if (argspec.has_starargs || argspec.has_kwargs) {
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

    return typeCallInner<S>(rewrite_args, new_argspec, arg1, arg2, arg3, new_args, keyword_names);
}

template <ExceptionStyle S>
static Box* typeCallInternal(BoxedFunctionBase* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                             Box* arg2, Box* arg3, Box** args,
                             const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    if (rewrite_args)
        assert(rewrite_args->func_guarded);

    static StatCounter slowpath_typecall("slowpath_typecall");
    slowpath_typecall.log();

    if (argspec.has_starargs || argspec.num_args == 0) {
        // Get callFunc to expand the arguments.
        // TODO: update this to use rearrangeArguments instead.
        KEEP_ALIVE(f);
        return callFunc<S>(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
    }

    return typeCallInner<S>(rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
}

// This function only exists for the corner case in typeCallInternal that should get removed anyway:
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

    return typeCallInternal<CXX>(NULL, NULL, ArgPassSpec(n + 1, 0, false, pass_kwargs), arg1, arg2, arg3, args, NULL);
}

// For use on __init__ return values
static Box* assertInitNone(STOLEN(Box*) rtn, STOLEN(Box*) obj) {
    AUTO_DECREF(rtn);
    if (rtn != None) {
        Py_DECREF(obj);
        raiseExcHelper(TypeError, "__init__() should return None, not '%s'", getTypeName(rtn));
    }
    return obj;
}

static PyObject* cpythonTypeCall(BoxedClass* type, PyObject* args, PyObject* kwds) {
    Box* r = cpython_type_call(type, args, kwds);
    if (!r)
        throwCAPIException();
    return r;
}

static Box* unicodeNewHelper(BoxedClass* type, Box* string, Box* encoding_obj, Box** _args) {
    Box* errors_obj = _args[0];

    assert(isSubclass(type, unicode_cls));
    assert(type->tp_new == unicode_cls->tp_new);

    char* encoding = NULL;
    char* errors = NULL;
    if (encoding_obj)
        if (!PyArg_ParseSingle(encoding_obj, 1, "unicode", "s", &encoding))
            throwCAPIException();
    if (errors_obj)
        if (!PyArg_ParseSingle(errors_obj, 1, "unicode", "s", &errors))
            throwCAPIException();

    Box* r = unicode_new_inner(type, string, encoding, errors);
    if (!r)
        throwCAPIException();

    if (type == unicode_cls) {
        if (r->cls == unicode_cls) // no init
            return r;
        if (!PyUnicode_Check(r)) // skip init
            return r;
    } else {
        if (!isSubclass(r->cls, type)) // skip init
            return r;
    }

    // Call tp_init:

    if (r->cls->tp_init == object_cls->tp_init)
        return r;

    if (!string)
        assert(!encoding_obj);
    if (!encoding_obj)
        assert(!errors_obj);

    Box* args;
    if (!string)
        args = incref(EmptyTuple);
    else if (!encoding_obj)
        args = BoxedTuple::create1(string);
    else if (!errors_obj)
        args = BoxedTuple::create2(string, encoding_obj);
    else
        args = BoxedTuple::create3(string, encoding_obj, errors_obj);

    AUTO_DECREF(args);

    int init_code = r->cls->tp_init(r, args, NULL);
    if (init_code == -1) {
        Py_DECREF(r);
        throwCAPIException();
    }

    return r;
}

static Box* objectNewNoArgs(BoxedClass* cls) noexcept {
    assert(PyType_Check(cls));
#ifndef NDEBUG
    static BoxedString* new_str = getStaticString("__new__");
    static BoxedString* init_str = getStaticString("__init__");
    assert(typeLookup(cls, new_str) == typeLookup(object_cls, new_str)
           && typeLookup(cls, init_str) != typeLookup(object_cls, init_str));
#endif
    return new (cls) Box();
}

template <ExceptionStyle S>
static Box* typeCallInner(CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                          Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    int npassed_args = argspec.totalPassed();
    int npositional = argspec.num_args;

    // We need to know what the class is.  We could potentially call rearrangeArguments here
    assert(argspec.num_args >= 1);
    Box* _cls = arg1;

    if (!PyType_Check(_cls)) {
        if (S == CAPI)
            PyErr_Format(TypeError, "descriptor '__call__' requires a 'type' object but received an '%s'",
                         getTypeName(_cls));
        else
            raiseExcHelper(TypeError, "descriptor '__call__' requires a 'type' object but received an '%s'",
                           getTypeName(_cls));
    }

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);

    if (cls->tp_new == NULL) {
        if (S == CAPI) {
            PyErr_Format(PyExc_TypeError, "cannot create '%.100s' instances", cls->tp_name);
            return NULL;
        } else {
            raiseExcHelper(TypeError, "cannot create '%.100s' instances", cls->tp_name);
        }
    }

    // Special-case unicode for now, maybe there's something about this that can eventually be generalized:
    if (cls->tp_new == unicode_cls->tp_new) {
        // TODO: implement
        if (S == CAPI) {
            try {
                return typeCallInner<CXX>(NULL, argspec, arg1, arg2, arg3, args, keyword_names);
            } catch (ExcInfo e) {
                setCAPIException(e);
                return NULL;
            }
        }

        assert(S == CXX && "implement me");

        if (rewrite_args) {
            rewrite_args->arg1->addGuard((intptr_t)cls);
        }

        ParamReceiveSpec paramspec(4, 3, false, false);
        bool rewrite_success = false;
        static ParamNames param_names({ "", "string", "encoding", "errors" }, "", "");
        static Box* defaults[3] = { NULL, NULL, NULL };
        Box* oargs[1];
        bool oargs_owned[1];

        rearrangeArguments(paramspec, &param_names, "unicode", defaults, rewrite_args, rewrite_success, argspec, arg1,
                           arg2, arg3, args, oargs, keyword_names, oargs_owned);
        assert(arg1 == cls);

        AUTO_DECREF_ARGS(paramspec, arg1, arg2, arg3, oargs);

        if (!rewrite_success)
            rewrite_args = NULL;

        if (rewrite_args) {
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)unicodeNewHelper, rewrite_args->arg1, rewrite_args->arg2,
                                               rewrite_args->arg3, rewrite_args->args)->setType(RefType::OWNED);

            decrefOargs(rewrite_args->args, oargs_owned, 1);

            rewrite_args->out_success = true;
        }

        return unicodeNewHelper(cls, arg2, arg3, oargs);
    }

    if (cls->tp_new != object_cls->tp_new && cls->tp_new != slot_tp_new && cls->tp_new != BaseException->tp_new
        && cls->tp_new != type_cls->tp_new && S == CXX) {
        // Looks like we're calling an extension class and we're not going to be able to
        // separately rewrite the new + init calls.  But we can rewrite the fact that we
        // should just call the cpython version, which will end up working pretty well.

        assert(S == CXX && "implement me");

        ParamReceiveSpec paramspec(1, false, true, true);
        bool rewrite_success = false;
        Box** oargs = NULL;
        rearrangeArguments(paramspec, NULL, "", NULL, rewrite_args, rewrite_success, argspec, arg1, arg2, arg3, args,
                           oargs, keyword_names, NULL);
        assert(arg1 == cls);

        AUTO_DECREF(arg1);
        AUTO_DECREF(arg2);
        AUTO_XDECREF(arg3);

        if (!rewrite_success)
            rewrite_args = NULL;

        if (rewrite_args) {
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)cpythonTypeCall, rewrite_args->arg1, rewrite_args->arg2,
                                               rewrite_args->arg3)->setType(RefType::OWNED);
            rewrite_args->out_success = true;
        }

        return cpythonTypeCall(cls, arg2, arg3);
    }

    if (argspec.has_starargs || argspec.has_kwargs)
        rewrite_args = NULL;

    RewriterVar* r_ccls = NULL;
    RewriterVar* r_new = NULL;
    RewriterVar* r_init = NULL;
    // TODO: shouldn't cache it like this.  What if __new__ changes __init__
    DecrefHandle<Box, true> init_attr(nullptr);
    if (rewrite_args) {
        assert(!argspec.has_starargs);
        assert(!argspec.has_kwargs);
        assert(argspec.num_args > 0);

        r_ccls = rewrite_args->arg1;
        // Guard on the requested class.  We could potentially support multiple classes in a rewrite,
        // but there are parts of this function's rewrite that currently take advantage of the fact
        // that the requested class is fixed.
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

    DecrefHandle<Box, true> new_attr(nullptr);
    static BoxedString* new_str = getStaticString("__new__");
    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls, rewrite_args->destination);
        // TODO: if tp_new != Py_CallPythonNew, call that instead?
        new_attr = incref(typeLookup(cls, new_str, &grewrite_args));
        assert(new_attr);

        if (!grewrite_args.isSuccessful())
            rewrite_args = NULL;
        else {
            r_new = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
            r_new->addGuard((intptr_t)new_attr);
        }

        // Special-case functions to allow them to still rewrite:
        if (new_attr->cls != function_cls) {
            try {
                Box* descr_r = processDescriptorOrNull(new_attr, None, cls);
                // TODO do we need to guard here on the class of new_attr (or that it's a class that can't change
                // classes?)
                if (descr_r) {
                    new_attr = descr_r;
                    rewrite_args = NULL;
                }
            } catch (ExcInfo e) {
                if (S == CAPI) {
                    setCAPIException(e);
                    return NULL;
                } else
                    throw e;
            }
        }
    } else {
        new_attr = incref(typeLookup(cls, new_str));
        try {
            if (new_attr->cls != function_cls) // optimization
                new_attr = processDescriptor(new_attr, None, cls);
        } catch (ExcInfo e) {
            if (S == CAPI) {
                setCAPIException(e);
                return NULL;
            } else
                throw e;
        }
    }
    assert(new_attr && "This should always resolve");

    // typeCall is tricky to rewrite since it has complicated behavior: we are supposed to
    // call the __init__ method of the *result of the __new__ call*, not of the original
    // class.  (And only if the result is an instance of the original class (or a subclass),
    // but that's not even the tricky part here.)
    //
    // By the time we know the type of the result of __new__(), it's too late to add traditional
    // guards.  So, instead of doing that, we're going to add a guard that makes sure that __new__
    // has the property that it will always return an instance where we know what __init__ has to be
    // called on it.  There are a couple cases:
    // - Some __new__ functions, such as object.__new__, always return an instance of the requested class.
    //   We can whitelist these __new__ functions.
    // - There are cls+arg pairs such that cls(arg) always returns an instance of cls.  For example,
    //   str() of an int is always a str, but str of arbitrary types does not necessarily return a str
    //   (could return a subtype of str)
    // - There are cls+arg pairs where we know that we don't have to call an __init__, despite the return
    //   value having variable type.  For instance, int(float) can return a long on overflow, but in either
    //   case no __init__ should be called.
    // - There's a final special case that type(obj) does not call __init__ even if type.__new__(type, obj)
    //   happens to return a subclass of type.  This is a special case in cpython's code that we have as well.
    //

    // For debugging, keep track of why we think we can rewrite this:
    enum { UNKNOWN, MAKES_CLS, NO_INIT, TYPE_NEW_SPECIAL_CASE, } which_init = UNKNOWN;

    // These are __new__ functions that have the property that __new__(kls) always returns an instance of kls.
    // These are ok to call regardless of what type was requested.
    //
    // TODO what if an extension type defines a tp_alloc that returns something that's not an instance of that
    // type?  then object.__new__ would not be able to be here:
    //
    // this array is ok with not using StlCompatAllocator since we will manually register these objects with the GC
    static std::vector<Box*> class_making_news;
    if (class_making_news.empty()) {
        for (BoxedClass* allowed_cls : { object_cls, enumerate_cls, xrange_cls, tuple_cls, list_cls, dict_cls }) {
            auto new_obj = typeLookup(allowed_cls, new_str);
            class_making_news.push_back(new_obj);
        }
    }

    if (rewrite_args) {
        for (auto b : class_making_news) {
            if (b == new_attr) {
                which_init = MAKES_CLS;
                break;
            }
        }

        if (cls->tp_new == BaseException->tp_new)
            which_init = MAKES_CLS;

        bool know_first_arg = !argspec.has_starargs && !argspec.has_kwargs && argspec.num_keywords == 0;

        if (know_first_arg) {
            if (argspec.num_args == 1
                && (cls == int_cls || cls == float_cls || cls == long_cls || cls == str_cls || cls == unicode_cls))
                which_init = MAKES_CLS;

            if (argspec.num_args == 2 && (cls == int_cls || cls == float_cls || cls == long_cls)
                && (arg2->cls == int_cls || arg2->cls == str_cls || arg2->cls == float_cls
                    || arg2->cls == unicode_cls)) {
                which_init = NO_INIT;
                rewrite_args->arg2->addAttrGuard(offsetof(Box, cls), (intptr_t)arg2->cls);
            }

            // str(obj) can return str-subtypes, but for builtin types it won't:
            if (argspec.num_args == 2 && cls == str_cls && (arg2->cls == int_cls || arg2->cls == float_cls)) {
                which_init = MAKES_CLS;
                rewrite_args->arg2->addAttrGuard(offsetof(Box, cls), (intptr_t)arg2->cls);
            }

            // int(str, base) can only return int/long
            if (argspec.num_args == 3 && cls == int_cls) {
                which_init = NO_INIT;
            }

#if 0
            if (which_init == NOT_ALLOWED) {
                std::string per_name_stat_name = "zzz_norewrite_" + std::string(cls->tp_name);
                if (argspec.num_args == 1)
                    per_name_stat_name += "_1arg";
                else if (argspec.num_args == 2)
                    per_name_stat_name += "_" + std::string(arg2->cls->tp_name);
                else
                    per_name_stat_name += "_narg";
                uint64_t* counter = Stats::getStatCounter(per_name_stat_name);
                Stats::log(counter);
            }
#endif
        }

        if (cls == type_cls && argspec == ArgPassSpec(2))
            which_init = TYPE_NEW_SPECIAL_CASE;
    }

    static BoxedString* init_str = getStaticString("__init__");
    if (cls->tp_init == slot_tp_init) {
        // If there's a Python-level tp_init, try getting it, since calling it might be faster than calling
        // tp_init if we can manage to rewrite it.
        if (rewrite_args && which_init != UNKNOWN) {
            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_ccls, rewrite_args->destination);
            init_attr = incref(typeLookup(cls, init_str, &grewrite_args));

            if (!grewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                if (init_attr) {
                    r_init = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
                    r_init->addGuard((intptr_t)init_attr);
                } else {
                    grewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
                }
            }
        } else {
            init_attr = incref(typeLookup(cls, init_str));
        }
    }

    Box* made;
    RewriterVar* r_made = NULL;

    ArgPassSpec new_argspec = argspec;

    if (S == CAPI && cls->tp_new != object_cls->tp_new && cls->tp_init != object_cls->tp_init) {
        // If there's a custom new and custom init, in CAPI mode we don't have any way of handling
        // any exceptions thrown by the new.
        rewrite_args = NULL;
    }

    if (S == CAPI && cls->tp_init != object_cls->tp_init) {
        // If there's a custom init, in CAPI mode we don't have any way of handling the exception that
        // we assertInitNone might have to throw from the init returning non-None.
        // TODO actually looks we only have to be doing that check for Python-level objects; ie in CPython
        // that check is done in slot_tp_init
        rewrite_args = NULL;
    }

    if (rewrite_args) {
        if (cls->tp_new == object_cls->tp_new && cls->tp_init != object_cls->tp_init) {
            // Fast case: if we are calling object_new, we normally doesn't look at the arguments at all.
            // (Except in the case when init_attr != object_init, in which case object_new looks at the number
            // of arguments and throws an exception.)
            //
            // Another option is to rely on rewriting to make this fast, which would probably require adding
            // a custom internal callable to object.__new__
            made = objectNewNoArgs(cls);
            r_made = rewrite_args->rewriter->call(true, (void*)objectNewNoArgs, r_ccls)->setType(RefType::OWNED);
            assert(made);
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

            made = runtimeCallInternal<S, REWRITABLE>(new_attr, &srewrite_args, new_argspec, cls, arg2, arg3, args,
                                                      keyword_names);
            if (!made) {
                assert(S == CAPI);

                if (srewrite_args.out_success && which_init == NO_INIT) {
                    rewrite_args->out_rtn = srewrite_args.out_rtn;
                    rewrite_args->out_success = true;
                }

                return NULL;
            }

            if (!srewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                r_made = srewrite_args.out_rtn;
            }
        }
    } else {
        if (cls->tp_new == object_cls->tp_new && cls->tp_init != object_cls->tp_init) {
            made = objectNewNoArgs(cls);
            assert(made);
        } else
            made = runtimeCallInternal<S, NOT_REWRITABLE>(new_attr, NULL, new_argspec, cls, arg2, arg3, args,
                                                          keyword_names);

        if (!made) {
            assert(S == CAPI);
            return NULL;
        }
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

    bool skip_init = false;

    // For __new__ functions that we have no information about, try to rewrite to a helper.
    if (rewrite_args && which_init == UNKNOWN) {
        // TODO this whole block is very similar to the call-tpinit block a bit farther down.
        // The later block is slightly different since it can know what the tp_init function
        // will be, whereas this block can't; I'm not sure how to merge the functionality.  That's
        // probably just evidence of the overall convolutedness of this function.

        // TODO: instead of rewriting to the capi-format, maybe we should do the rearrangearguments
        // inside the helper?
        bool rewrite_success = false;
        try {
            rearrangeArguments(ParamReceiveSpec(1, 0, true, true), NULL, "", NULL, rewrite_args, rewrite_success,
                               argspec, made, arg2, arg3, args, NULL, keyword_names, NULL);
        } catch (ExcInfo e) {
            if (S == CAPI) {
                setCAPIException(e);
                return NULL;
            } else
                throw e;
        }

        AUTO_DECREF(made);
        AUTO_DECREF(arg2);
        AUTO_XDECREF(arg3);

        if (!rewrite_success)
            rewrite_args = NULL;

        class InitHelper {
        public:
            static Box* call(STOLEN(Box*) made, BoxedClass* cls, Box* args, Box* kwargs) noexcept(S == CAPI) {
                if (!isSubclass(made->cls, cls))
                    return made;

                int err = made->cls->tp_init(made, args, kwargs);
                if (err == -1) {
                    if (S == CAPI)
                        return NULL;
                    else
                        throwCAPIException();
                }
                return made;
            }
        };

        assert(arg2->cls == tuple_cls);
        assert(!arg3 || arg3->cls == dict_cls);

        if (rewrite_args) {
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)InitHelper::call, r_made, r_ccls, rewrite_args->arg2,
                                               rewrite_args->arg3)->setType(RefType::OWNED);
            r_made->refConsumed();
            rewrite_args->out_success = true;
        }
        return InitHelper::call(made, cls, arg2, arg3);
    }

    // If __new__ returns a subclass, supposed to call that subclass's __init__.
    // If __new__ returns a non-subclass, not supposed to call __init__.
    if (made->cls != cls) {
        assert(which_init != MAKES_CLS);

        if (!isSubclass(made->cls, cls)) {
            skip_init = true;
        } else {
            if (init_attr) {
                // Getting here means the init_attr is wrong; set it to NULL so that we don't use it.
                init_attr = NULL;
            }
        }
    }

    assert(!rewrite_args || !skip_init || (which_init == NO_INIT && made->cls->tp_init == object_cls->tp_init));

    if (!skip_init && made->cls->tp_init != object_cls->tp_init) {
        Box* initrtn;
        // If there's a Python-level __init__ function, try calling it.
        if (init_attr && init_attr->cls == function_cls) {
            AUTO_DECREF(made); // In case init throws:

            if (rewrite_args) {
                // We are going to rewrite as a call to cls.init:
                assert(which_init == MAKES_CLS);
                assert(made->cls == cls);
            }

            // Note: this code path includes the descriptor logic
            if (rewrite_args) {
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

                initrtn = runtimeCallInternal<S, REWRITABLE>(init_attr, &srewrite_args, argspec, made, arg2, arg3, args,
                                                             keyword_names);

                if (!srewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    assert(S == CXX && "this need to be converted");
                    auto new_r_made
                        = rewrite_args->rewriter->call(true, (void*)assertInitNone, srewrite_args.out_rtn, r_made);
                    r_made->refConsumed();
                    new_r_made->setType(RefType::OWNED);
                    r_made = new_r_made;
                    srewrite_args.out_rtn->refConsumed();
                }
            } else {
                initrtn = runtimeCallInternal<S, NOT_REWRITABLE>(init_attr, NULL, argspec, made, arg2, arg3, args,
                                                                 keyword_names);
            }

            incref(made); // to cancel out the AUTO_DECREF

            if (!initrtn) {
                assert(S == CAPI);
                Py_DECREF(made);
                return NULL;
            }

            assert(initrtn);

            if (S == CAPI) {
                if (initrtn != None) {
                    Py_DECREF(made);
                    PyErr_Format(TypeError, "__init__() should return None, not '%s'", getTypeName(initrtn));
                    Py_DECREF(initrtn);
                    return NULL;
                }
                Py_DECREF(initrtn);
            } else
                made = assertInitNone(initrtn, made); // assertInitNone is optimized for the rewriter case; this call
                                                      // here could be improved if it is an issue.
        } else {
            // Otherwise, just call tp_init.  This will work out well for extension classes, and no worse
            // than failing the rewrite for Python non-extension non-functions (when does that happen?).

            initproc tpinit = made->cls->tp_init;

            if (rewrite_args) {
                // This is the only case that should get here:
                assert(which_init == MAKES_CLS && made->cls == cls);
                // We're going to emit a call to cls->tp_init, but really we should be calling made->cls->tp_init,
                // but the MAKES_CLS condition tells us that made->cls is cls so the two tp_inits are the same.
                assert(tpinit == cls->tp_init);
            }

            bool rewrite_success = false;
            try {
                rearrangeArguments(ParamReceiveSpec(1, 0, true, true), NULL, "", NULL, rewrite_args, rewrite_success,
                                   argspec, made, arg2, arg3, args, NULL, keyword_names, NULL);
            } catch (ExcInfo e) {
                Py_DECREF(made);
                if (S == CAPI) {
                    setCAPIException(e);
                    return NULL;
                } else
                    throw e;
            }

            if (!rewrite_success)
                rewrite_args = NULL;

            assert(arg2->cls == tuple_cls);
            assert(!arg3 || arg3->cls == dict_cls);

            int err = tpinit(made, arg2, arg3);
            Py_DECREF(made);
            Py_DECREF(arg2);
            Py_XDECREF(arg3);
            if (err == -1) {
                Py_DECREF(made);
                if (S == CAPI)
                    return NULL;
                else
                    throwCAPIException();
            }

            if (rewrite_args) {
                auto r_err
                    = rewrite_args->rewriter->call(true, (void*)tpinit, r_made, rewrite_args->arg2, rewrite_args->arg3);

                assert(S == CXX && "this need to be converted");
                rewrite_args->rewriter->checkAndThrowCAPIException(r_err, -1);
            }
        }
    } else {
        if (new_attr == NULL && npassed_args != 1) {
            // TODO not npassed args, since the starargs or kwargs could be null
            if (S == CAPI) {
                PyErr_SetString(TypeError, objectNewParameterTypeErrorMsg());
                return NULL;
            } else
                raiseExcHelper(TypeError, "%s", objectNewParameterTypeErrorMsg());
        }
    }

    if (rewrite_args) {
        rewrite_args->out_rtn = r_made;
        rewrite_args->out_success = true;
    }

    assert(made);
    return made;
}

static Box* typeDict(Box* obj, void* context) {
    if (obj->cls->instancesHaveHCAttrs())
        return PyDictProxy_New(obj->getAttrWrapper());
    abort();
}

static Box* typeSubDict(Box* obj, void* context) {
    if (obj->cls->instancesHaveHCAttrs())
        return incref(obj->getAttrWrapper());
    if (obj->cls->instancesHaveDictAttrs())
        return obj->getDict();
    abort();
}

static void typeSubSetDict(BORROWED(Box*) obj, BORROWED(Box*) val, void* context) {
    Py_INCREF(val);
    if (obj->cls->instancesHaveDictAttrs()) {
        RELEASE_ASSERT(val->cls == dict_cls, "");
        obj->setDict(static_cast<BoxedDict*>(val));
        return;
    }

    if (obj->cls->instancesHaveHCAttrs()) {
        obj->setDictBacked(val);
        return;
    }

    // This should have thrown an exception rather than get here:
    abort();
}

extern "C" void PyType_SetDict(PyTypeObject* type, STOLEN(PyObject*) dict) noexcept {
    typeSubSetDict(type, dict, NULL);
    Box* old_dict = type->tp_dict;
    type->tp_dict = dict;
    Py_DECREF(old_dict);
}

Box* dict_descr = NULL;

extern "C" {
BoxedClass* object_cls, *type_cls, *none_cls, *bool_cls, *int_cls, *float_cls,
    * str_cls = NULL, *function_cls, *instancemethod_cls, *list_cls, *slice_cls, *module_cls, *dict_cls, *tuple_cls,
      *member_descriptor_cls, *closure_cls, *generator_cls, *complex_cls, *basestring_cls, *property_cls,
      *staticmethod_cls, *classmethod_cls, *attrwrapper_cls, *pyston_getset_cls, *capi_getset_cls,
      *builtin_function_or_method_cls, *attrwrapperiter_cls, *set_cls, *frozenset_cls;

BoxedTuple* EmptyTuple;
}

extern "C" Box* createUserClass(BoxedString* name, Box* _bases, Box* _attr_dict) {
    ASSERT(_attr_dict->cls == dict_cls, "%s", getTypeName(_attr_dict));
    BoxedDict* attr_dict = static_cast<BoxedDict*>(_attr_dict);

    assert(_bases->cls == tuple_cls);
    BoxedTuple* bases = static_cast<BoxedTuple*>(_bases);

    static BoxedString* metaclass_str = getStaticString("__metaclass__");

    Box* metaclass = NULL;
    metaclass = attr_dict->getOrNull(metaclass_str);

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

        if (isSubclass(r->cls, type_cls))
            assert(classes.back() != r);

        return r;
    } catch (ExcInfo e) {
        RELEASE_ASSERT(e.matches(BaseException), "");

        Box* msg = e.value;
        assert(msg);
        // TODO this is an extra Pyston check and I don't think we should have to do it:
        if (isSubclass(e.value->cls, BaseException)) {
            static BoxedString* message_str = getStaticString("message");
            msg = getattr(e.value, message_str);
        } else {
            incref(msg);
        }

        if (PyString_Check(msg)) {
            auto newmsg = PyString_FromFormat("Error when calling the metaclass bases\n"
                                              "    %s",
                                              PyString_AS_STRING(msg));
            if (newmsg) {
                Py_DECREF(e.value);
                e.value = newmsg;
            }
        }

        Py_DECREF(msg);

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

extern "C" Box* none_repr(Box* v) noexcept {
    return boxString("None");
}

extern "C" Box* noneNonzero(Box* v) {
    Py_RETURN_FALSE;
}

extern "C" BoxedString* builtinFunctionOrMethodRepr(BoxedBuiltinFunctionOrMethod* v) {
    if (v->name != NULL)
        return (BoxedString*)PyString_FromFormat("<built-in function %s>", PyString_AsString(v->name));
    RELEASE_ASSERT(false, "builtinFunctionOrMethodRepr not properly implemented");
}

static Box* builtinFunctionOrMethodCall(BoxedBuiltinFunctionOrMethod* self, Box* args, Box* kwargs) {
    return runtimeCallInternal<CXX, NOT_REWRITABLE>(self, NULL, ArgPassSpec(0, 0, true, true), args, kwargs, NULL, NULL,
                                                    NULL);
}

extern "C" BoxedString* functionRepr(BoxedFunction* v) {
    if (!v->name)
        return (BoxedString*)PyString_FromFormat("<function <name_missing?> at %p>", v);
    return (BoxedString*)PyString_FromFormat("<function %s at %p>", PyString_AsString(v->name), v);
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
    assert(!kwargs || kwargs->cls == dict_cls);
    return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwargs, NULL, NULL, NULL);
}

static Box* funcName(Box* b, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);
    if (func->name == NULL)
        return boxString("<unknown function name>");
    return incref(func->name);
}

static void funcSetName(Box* b, Box* v, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);

    if (v == NULL || !PyString_Check(v)) {
        raiseExcHelper(TypeError, "__name__ must be set to a string object");
    }

    auto old_name = func->name;
    func->name = incref(static_cast<BoxedString*>(v));
    Py_XDECREF(old_name);
}

static Box* builtinFunctionOrMethodName(Box* b, void*) {
    // In CPython, these guys just store char*, and it gets wrapped here
    // But we already share the BoxedString* field with BoxedFunctions...
    // so it's more convenient to just use that, which is what we do here.
    // Is there any advantage to using the char* way, here?

    assert(b->cls == builtin_function_or_method_cls);
    BoxedBuiltinFunctionOrMethod* func = static_cast<BoxedBuiltinFunctionOrMethod*>(b);
    assert(func->name);
    return incref(func->name);
}

static Box* functionCode(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    return incref(func->md->getCode());
}

extern "C" PyObject* PyFunction_GetCode(PyObject* func) noexcept {
    try {
        return functionCode((Box*)func, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static void functionSetCode(Box* self, Box* v, void*) {
    assert(self->cls == function_cls);

    if (v == NULL || !PyCode_Check(v))
        raiseExcHelper(TypeError, "__code__ must be set to a code object");

    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    BoxedCode* code = static_cast<BoxedCode*>(v);

    RELEASE_ASSERT(func->md->source && code->f->source, "__code__ can only be set on python functions");

    RELEASE_ASSERT(!func->md->internal_callable.get<CXX>() && !func->md->internal_callable.get<CAPI>(),
                   "this could cause invalidation issues");

    func->md = code->f;
    func->dependent_ics.invalidateAll();
}

static Box* functionDefaults(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    if (!func->defaults)
        return incref(None);
    for (auto e : *func->defaults) {
        RELEASE_ASSERT(e, "this function's defaults should not be available");
    }
    return incref(func->defaults);
}

static Box* functionGlobals(Box* self, void*) {
    assert(self->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(self);
    if (func->globals) {
        assert(!func->md->source || !func->md->source->scoping->areGlobalsFromModule());
        return incref(func->globals);
    }
    assert(func->md->source);
    assert(func->md->source->scoping->areGlobalsFromModule());

    static BoxedString* dict_str = getStaticString("__dict__");
    return getattr(func->md->source->parent_module, dict_str);
}

static void functionSetDefaults(Box* b, Box* v, void*) {
    assert(b->cls == function_cls);
    BoxedFunction* func = static_cast<BoxedFunction*>(b);

    // I guess this could happen if the user tried to do something like `min.__defaults__ = ()`.
    // In CPython that would be an AttributeError, but I think we might allow it to get to this point.
    // The better way to handle this would be to not add the __defaults__ descriptor to the
    // builtin_function_or_method_cls so that it would be an AttributeError to try to access it,
    // and leave this assert.
    RELEASE_ASSERT(func->can_change_defaults, "trying to change the defaults on a non-defaults-changable function.");

    if (v == None)
        v = NULL;
    else if (v && !PyTuple_Check(v)) {
        raiseExcHelper(TypeError, "__defaults__ must be set to a tuple object");
    }

    BoxedTuple* t = static_cast<BoxedTuple*>(v);

    auto old_defaults = func->defaults;
    func->defaults = xincref(t); // t can be NULL for 'del f.func_defaults'
    Py_XDECREF(old_defaults);
    func->dependent_ics.invalidateAll();
}

static Box* functionNonzero(BoxedFunction* self) {
    Py_RETURN_TRUE;
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
    // Not the most effficient, but it works:
    return runtimeCallInternal<CXX, NOT_REWRITABLE>(self, NULL, ArgPassSpec(0, 0, true, true), args, kwargs, NULL, NULL,
                                                    NULL);
    // TODO add a tpp_call
}

Box* instancemethodGet(BoxedInstanceMethod* self, Box* obj, Box* type) {
    RELEASE_ASSERT(self->cls == instancemethod_cls, "");

    if (self->obj != NULL) {
        return incref(self);
    }

    if (!PyObject_IsSubclass(type, self->im_class)) {
        return incref(self);
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

    static BoxedString* name_str = getStaticString("__name__");
    funcname = getattrInternal<CXX>(func, name_str);
    AUTO_XDECREF(funcname);

    if (funcname != NULL) {
        if (!PyString_Check(funcname)) {
            funcname = NULL;
        } else
            sfuncname = PyString_AS_STRING(funcname);
    }

    if (klass == NULL) {
        klassname = NULL;
    } else {
        klassname = getattrInternal<CXX>(klass, name_str);
        if (klassname != NULL) {
            if (!PyString_Check(klassname)) {
                klassname = NULL;
            } else {
                sklassname = PyString_AS_STRING(klassname);
            }
        }
    }
    AUTO_XDECREF(klassname);

    if (self == NULL)
        result = PyString_FromFormat("<unbound method %s.%s>", sklassname, sfuncname);
    else {
        // This was a CPython comment: /* XXX Shouldn't use repr() here! */
        Box* selfrepr = repr(self);
        AUTO_DECREF(selfrepr);
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
                return compareInternal<NOT_REWRITABLE>(self->obj, rhs_im->obj, AST_TYPE::Eq, NULL);
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
    AUTO_DECREF(start);
    BoxedString* stop = static_cast<BoxedString*>(repr(self->stop));
    AUTO_DECREF(stop);
    BoxedString* step = static_cast<BoxedString*>(repr(self->step));
    AUTO_DECREF(step);
    return boxStringTwine(llvm::Twine("slice(") + start->s() + ", " + stop->s() + ", " + step->s() + ")");
}

Box* sliceHash(BoxedSlice* self) {
    raiseExcHelper(TypeError, "unhashable type");
    return boxLong(-1l);
}

Box* sliceReduce(BoxedSlice* self) {
    return Py_BuildValue("O(OOO)", self->cls, self->start, self->stop, self->step);
}

Box* sliceIndices(BoxedSlice* self, Box* len) {
    Py_ssize_t ilen, start, stop, step, slicelength;

    ilen = PyNumber_AsSsize_t(len, PyExc_OverflowError);

    if (ilen == -1 && PyErr_Occurred()) {
        throwCAPIException();
    }

    if (PySlice_GetIndicesEx((PySliceObject*)self, ilen, &start, &stop, &step, &slicelength) < 0) {
        throwCAPIException();
    }
    return BoxedTuple::create({ autoDecref(boxInt(start)), autoDecref(boxInt(stop)), autoDecref(boxInt(step)) });
}

static int slice_compare(PySliceObject* v, PySliceObject* w) noexcept {
    int result = 0;

    if (v == w)
        return 0;

    if (PyObject_Cmp(v->start, w->start, &result) < 0)
        return -2;
    if (result != 0)
        return result;
    if (PyObject_Cmp(v->stop, w->stop, &result) < 0)
        return -2;
    if (result != 0)
        return result;
    if (PyObject_Cmp(v->step, w->step, &result) < 0)
        return -2;
    return result;
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

    if ((self->tp_flags & Py_TPFLAGS_HEAPTYPE) && self->is_user_defined)
        os << "<class '";
    else
        os << "<type '";

    static BoxedString* module_str = getStaticString("__module__");
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

    if (type->tp_flags & Py_TPFLAGS_HEAPTYPE && type->is_user_defined) {
        static BoxedString* module_str = getStaticString("__module__");
        mod = type->getattr(module_str);
        if (!mod)
            raiseExcHelper(AttributeError, "__module__");
        return incref(mod);
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

    static BoxedString* module_str = getStaticString("__module__");
    type->setattr(module_str, value, NULL);
}


Box* typeHash(BoxedClass* self) {
    assert(PyType_Check(self));
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
    assert(PyType_Check(self));
    Box* rtn = type_subclasses(self, 0);
    checkAndThrowCAPIException();
    return rtn;
}

Box* typeMro(BoxedClass* self) {
    assert(PyType_Check(self));

    Box* r = mro_external(self);
    if (!r)
        throwCAPIException();
    return r;
}

Box* moduleInit(BoxedModule* self, Box* name, Box* doc) {
    RELEASE_ASSERT(PyModule_Check(self), "");
    RELEASE_ASSERT(name->cls == str_cls, "");
    RELEASE_ASSERT(!doc || doc->cls == str_cls || doc->cls == unicode_cls, "");

    doc = doc ? doc : None;

    HCAttrs* attrs = self->getHCAttrsPtr();

    if (!attrs->hcls || attrs->hcls->attributeArraySize() == 0) {
        attrs->hcls = HiddenClass::makeSingleton();

        self->giveAttrBorrowed("__name__", name);
        self->giveAttrBorrowed("__doc__", doc);
    } else {
        self->setattr(autoDecref(internStringMortal("__name__")), name, NULL);
        self->setattr(autoDecref(internStringMortal("__doc__")), doc, NULL);
    }

    Py_RETURN_NONE;
}

Box* moduleRepr(BoxedModule* m) {
    RELEASE_ASSERT(PyModule_Check(m), "");

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

    static Box* hasnext(Box* _self);
    static Box* next(Box* _self);
    static Box* next_capi(Box* _self) noexcept;

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
};

// A dictionary-like wrapper around the attributes array.
// Not sure if this will be enough to satisfy users who expect __dict__
// or PyModule_GetDict to return real dicts.
class AttrWrapper : public Box {
private:
    // TODO: not sure if b should become owned (it would create a cycle)
    // The fields are not not allowed to be set at the same time
    BORROWED(Box* b);        // set when this attribute wrapper wraps a Box (default mode)
    BoxedDict* private_dict; // set when the attribute wrapper does not wrap a Box anymore because the Box.__dict__
                             // changed to another dict.

    // This is useful when we assign non str_cls keys, because the attribute array only supports strs.
    void convertToDictBacked() {
        if (isDictBacked())
            return;

        // TODO: this means that future accesses to __dict__ will return something other than
        // this attrwrapper.  We should store the attrwrapper in the attributes array.

        HCAttrs* attrs = this->b->getHCAttrsPtr();
        assert(attrs->hcls->type != HiddenClass::DICT_BACKED);
        BoxedDict* d = (BoxedDict*)AttrWrapper::copy(this);

        b->getHCAttrsPtr()->_clearRaw();
        assert(this->b);
        assert(!private_dict);

        HCAttrs* hcattrs = b->getHCAttrsPtr();
        // Skips the attrlist freelist:
        auto new_attr_list = (HCAttrs::AttrList*)PyObject_MALLOC(sizeof(HCAttrs::AttrList) + sizeof(Box*));
        new_attr_list->attrs[0] = d;

        hcattrs->hcls = HiddenClass::dict_backed;
        hcattrs->attr_list = new_attr_list;
    }

    bool isDictBacked() {
        if (private_dict)
            return private_dict;
        return b->getHCAttrsPtr()->hcls->type == HiddenClass::DICT_BACKED;
    }

    Box* getDictBacking() {
        assert(isDictBacked());
        if (private_dict)
            return private_dict;
        return b->getHCAttrsPtr()->attr_list->attrs[0];
    }


public:
    AttrWrapper(Box* b) : b(b), private_dict(NULL) {
        assert(b->cls->instancesHaveHCAttrs());

        // We currently don't support creating an attrwrapper around a dict-backed object,
        // so try asserting that here.
        // This check doesn't cover all cases, since an attrwrapper could be created around
        // a normal object which then becomes dict-backed, so we RELEASE_ASSERT later
        // that that doesn't happen.
        assert(!b->getHCAttrsPtr()->hcls || b->getHCAttrsPtr()->hcls->type == HiddenClass::NORMAL
               || b->getHCAttrsPtr()->hcls->type == HiddenClass::SINGLETON);
    }

    void convertToPrivateDict() {
        RELEASE_ASSERT(!private_dict, "");
        RELEASE_ASSERT(b, "");
        private_dict = (BoxedDict*)AttrWrapper::copy(this);
        assert(PyDict_CheckExact(private_dict));
        b = NULL;
    }

    DEFAULT_CLASS(attrwrapper_cls);


    BORROWED(Box*) getUnderlying() {
        if (private_dict)
            return private_dict;
        return b;
    }

    static void dealloc(Box* b) noexcept {
        if (_PyObject_GC_IS_TRACKED(b))
            _PyObject_GC_UNTRACK(b);

        AttrWrapper::tp_clear(b);
        b->cls->tp_free(b);
    }

    static int traverse(PyObject* op, visitproc visit, void* arg) noexcept {
        AttrWrapper* attr_wrapper = (AttrWrapper*)op;
        Py_VISIT(attr_wrapper->private_dict);
        return 0;
    }

    static int tp_clear(PyObject* op) noexcept {
        AttrWrapper* attr_wrapper = (AttrWrapper*)op;
        Py_CLEAR(attr_wrapper->private_dict);
        return 0;
    }

    static Box* setitem(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* setitem_str = getStaticString("__setitem__");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), setitem_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(2), _key, value, NULL, NULL, NULL);
        }

        assert(_key->cls == str_cls);
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key); // TODO should be able to avoid this (if we change the refcount contract around interning)
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        self->b->setattr(key, value, NULL);
        Py_RETURN_NONE;
    }

    static int ass_sub(PyDictObject* mp, PyObject* v, PyObject* w) noexcept {
        try {
            Box* res;
            if (w == NULL) {
                res = AttrWrapper::delitem((Box*)mp, v);
            } else {
                res = AttrWrapper::setitem((Box*)mp, v, w);
            }
            assert(res == None);
            Py_DECREF(res);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return -1;
        }
        return 0;
    }

    static Box* setdefault(Box* _self, Box* _key, Box* value) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* setdefault_str = getStaticString("setdefault");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), setdefault_str,
                                                         LookupScope::CLASS_ONLY, NULL, ArgPassSpec(2), _key, value,
                                                         NULL, NULL, NULL);
        }

        assert(_key->cls == str_cls);
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key);
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        Box* cur = self->b->getattr(key);
        if (cur)
            return incref(cur);
        self->b->setattr(key, value, NULL);
        return incref(value);
    }

    static Box* get(Box* _self, Box* _key, Box* def) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* get_str = getStaticString("get");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), get_str, LookupScope::CLASS_ONLY, NULL,
                                                         ArgPassSpec(2), _key, def, NULL, NULL, NULL);
        }

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key);
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        Box* r = self->b->getattr(key);
        if (!r)
            return incref(def);
        return incref(r);
    }

    template <ExceptionStyle S> static Box* getitem(Box* _self, Box* _key) noexcept(S == CAPI) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* getitem_str = getStaticString("__getitem__");
            return callattrInternal<S, NOT_REWRITABLE>(self->getDictBacking(), getitem_str, LookupScope::CLASS_ONLY,
                                                       NULL, ArgPassSpec(1), _key, NULL, NULL, NULL, NULL);
        }

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key);
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        Box* r = self->b->getattr(key);
        if (r)
            return incref(r);

        if (S == CXX)
            raiseExcHelper(KeyError, "'%s'", key->data());
        else {
            PyErr_Format(KeyError, "'%s'", key->data());
            return NULL;
        }
    }

    static Box* pop(Box* _self, Box* _key, Box* default_) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* pop_str = getStaticString("pop");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), pop_str, LookupScope::CLASS_ONLY, NULL,
                                                         ArgPassSpec(2), _key, default_, NULL, NULL, NULL);
        }

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key);
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        Box* r = self->b->getattr(key);
        if (r) {
            Py_INCREF(r);
            self->b->delattr(key, NULL);
            return r;
        } else {
            if (default_)
                return incref(default_);
            raiseExcHelper(KeyError, "'%s'", key->data());
        }
    }

    static Box* delitem(Box* _self, Box* _key) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* delitem_str = getStaticString("__delitem__");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), delitem_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(1), _key, NULL, NULL, NULL, NULL);
        }

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key);
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        if (self->b->getattr(key))
            self->b->delattr(key, NULL);
        else
            raiseExcHelper(KeyError, "'%s'", key->data());
        Py_RETURN_NONE;
    }

    static Box* str(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        std::string O("");
        llvm::raw_string_ostream os(O);

        os << "attrwrapper(";
        if (self->isDictBacked()) {
            static BoxedString* repr_str = getStaticString("__repr__");
            Box* dict_repr
                = callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), repr_str, LookupScope::CLASS_ONLY, NULL,
                                                        ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
            RELEASE_ASSERT(PyString_CheckExact(dict_repr), "");
            os << ((BoxedString*)dict_repr)->s();
            Py_DECREF(dict_repr);
        } else {
            HCAttrs* attrs = self->b->getHCAttrsPtr();
            os << "{";
            RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
            bool first = true;
            for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
                if (!first)
                    os << ", ";
                first = false;

                BoxedString* v = attrs->attr_list->attrs[p.second]->reprICAsString();
                os << p.first->s() << ": " << v->s();
                Py_DECREF(v);
            }
            os << "}";
        }
        os << ")";

        return boxString(os.str());
    }

    template <ExceptionStyle S> static Box* contains(Box* _self, Box* _key) noexcept(S == CAPI) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (_key->cls != str_cls)
            self->convertToDictBacked();

        if (self->isDictBacked()) {
            static BoxedString* contains_str = getStaticString("__contains__");
            return callattrInternal<S, NOT_REWRITABLE>(self->getDictBacking(), contains_str, LookupScope::CLASS_ONLY,
                                                       NULL, ArgPassSpec(1), _key, NULL, NULL, NULL, NULL);
        }

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        Py_INCREF(key);
        internStringMortalInplace(key);
        AUTO_DECREF(key);

        Box* r = self->b->getattr(key);
        return incref(r ? True : False);
    }

    static Box* hasKey(Box* _self, Box* _key) {
        if (PyErr_WarnPy3k("dict.has_key() not supported in 3.x; use the in operator", 1) < 0)
            throwCAPIException();
        return contains<CXX>(_self, _key);
    }

    static int sq_contains(Box* _self, Box* _key) noexcept {
        Box* rtn = contains<CAPI>(_self, _key);
        if (!rtn)
            return -1;
        AUTO_DECREF(rtn);
        return rtn == True;
    }

    static Box* keys(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* keys_str = getStaticString("keys");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), keys_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        }


        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            listAppendInternal(rtn, p.first);
        }
        return rtn;
    }

    static Box* values(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* values_str = getStaticString("values");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), values_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        }

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            listAppendInternal(rtn, attrs->attr_list->attrs[p.second]);
        }
        return rtn;
    }

    static Box* items(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* items_str = getStaticString("items");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), items_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        }

        BoxedList* rtn = new BoxedList();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            BoxedTuple* t = BoxedTuple::create({ p.first, attrs->attr_list->attrs[p.second] });
            listAppendInternalStolen(rtn, t);
        }
        return rtn;
    }

    static Box* iterkeys(Box* _self) {
        Box* r = AttrWrapper::keys(_self);
        return getiter(autoDecref(r));
    }

    static Box* itervalues(Box* _self) {
        Box* r = AttrWrapper::values(_self);
        return getiter(autoDecref(r));
    }

    static Box* iteritems(Box* _self) {
        Box* r = AttrWrapper::items(_self);
        return getiter(autoDecref(r));
    }

    static Box* copy(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* copy_str = getStaticString("copy");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), copy_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        }

        BoxedDict* rtn = new BoxedDict();

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            ASSERT(rtn->d.count(p.first) == 0, "need to decref existing keys");
            rtn->d[incref(p.first)] = incref(attrs->attr_list->attrs[p.second]);
        }
        return rtn;
    }

    static void _clear(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* clear_str = getStaticString("clear");
            auto rtn = callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), clear_str, LookupScope::CLASS_ONLY,
                                                             NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
            assert(rtn == None);
            Py_DECREF(rtn);
            return;
        }

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        attrs->_clearRaw();

        // Add the existing attrwrapper object (ie self) back as the attrwrapper:
        self->b->appendNewHCAttr(self, NULL);
        if (!attrs->hcls)
            attrs->hcls = root_hcls;
        attrs->hcls = attrs->hcls->getAttrwrapperChild();
    }

    static Box* clear(Box* _self) {
        _clear(_self);
        Py_RETURN_NONE;
    }



    static Box* len(Box* _self) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* len_str = getStaticString("__len__");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), len_str, LookupScope::CLASS_ONLY, NULL,
                                                         ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        }

        HCAttrs* attrs = self->b->getHCAttrsPtr();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        return boxInt(attrs->hcls->getStrAttrOffsets().size());
    }

    static Box* update(Box* _self, BoxedTuple* args, BoxedDict* kwargs) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        assert(args->cls == tuple_cls);
        assert(!kwargs || kwargs->cls == dict_cls);

        RELEASE_ASSERT(args->size() <= 1, ""); // should throw a TypeError

        if (self->isDictBacked()) {
            static BoxedString* update_str = getStaticString("update");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), update_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(0, 0, true, true), args, kwargs, NULL, NULL,
                                                         NULL);
        }

        auto handle = [&](Box* _container) {
            if (_container->cls == attrwrapper_cls) {
                AttrWrapper* container = static_cast<AttrWrapper*>(_container);
                RELEASE_ASSERT(!container->isDictBacked(), "not implemented");
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
                if (!PyDict_Check(_container)) {
                    BoxedDict* new_container = new BoxedDict();
                    autoDecref(dictUpdate(new_container, autoDecref(BoxedTuple::create({ _container })), NULL));
                    _container = new_container;
                } else {
                    Py_INCREF(_container);
                }
                AUTO_DECREF(_container);

                assert(PyDict_Check(_container));
                BoxedDict* container = static_cast<BoxedDict*>(_container);

                for (const auto& p : *container) {
                    autoDecref(AttrWrapper::setitem(self, p.first, p.second));
                }
            }
        };

        for (auto e : *args) {
            handle(e);
        }
        if (kwargs)
            handle(kwargs);

        Py_RETURN_NONE;
    }

    static Box* iter(Box* _self) noexcept {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        if (self->isDictBacked()) {
            static BoxedString* iter_str = getStaticString("__iter__");
            return callattrInternal<CXX, NOT_REWRITABLE>(self->getDictBacking(), iter_str, LookupScope::CLASS_ONLY,
                                                         NULL, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        }

        return new AttrWrapperIter(self);
    }

    static Box* eq(Box* _self, Box* _other) {
        RELEASE_ASSERT(_self->cls == attrwrapper_cls, "");
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        // In order to not have to reimplement dict cmp: just create a real dict for now and us it.
        BoxedDict* dict = (BoxedDict*)AttrWrapper::copy(_self);
        AUTO_DECREF(dict);
        assert(dict->cls == dict_cls);
        static BoxedString* eq_str = getStaticString("__eq__");
        return callattrInternal<CXX, NOT_REWRITABLE>(dict, eq_str, LookupScope::CLASS_ONLY, NULL, ArgPassSpec(1),
                                                     _other, NULL, NULL, NULL, NULL);
    }

    static Box* ne(Box* _self, Box* _other) { return incref(eq(_self, _other) == True ? False : True); }

    friend class AttrWrapperIter;
};

void convertAttrwrapperToPrivateDict(Box* b) {
    assert(b->cls == attrwrapper_cls);
    static_cast<AttrWrapper*>(b)->convertToPrivateDict();
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
    return incref(r);
}

Box* AttrWrapperIter::next_capi(Box* _self) noexcept {
    RELEASE_ASSERT(_self->cls == attrwrapperiter_cls, "");
    AttrWrapperIter* self = static_cast<AttrWrapperIter*>(_self);
    RELEASE_ASSERT(self->hcls->type == HiddenClass::NORMAL || self->hcls->type == HiddenClass::SINGLETON, "");

    if (self->it == self->hcls->getStrAttrOffsets().end())
        return NULL;
    Box* r = self->it->first;
    ++self->it;
    return incref(r);
}

void Box::giveAttrDescriptor(const char* attr, Box* (*get)(Box*, void*), void (*set)(Box*, Box*, void*)) {
    BoxedString* bstr = internStringMortal(attr);
    giveAttr(bstr, new (pyston_getset_cls) BoxedGetsetDescriptor(bstr, get, set, NULL));
}

void Box::giveCapiAttrDescriptor(const char* attr, Box* (*get)(Box*, void*), int (*set)(Box*, Box*, void*)) {
    BoxedString* bstr = internStringMortal(attr);
    giveAttr(bstr, new (capi_getset_cls) BoxedGetsetDescriptor(bstr, get, set, NULL));
}

BORROWED(Box*) Box::getAttrWrapper() {
    assert(cls->instancesHaveHCAttrs());
    HCAttrs* attrs = getHCAttrsPtr();
    HiddenClass* hcls = attrs->hcls;

    if (!hcls)
        hcls = root_hcls;

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
            Py_DECREF(aw);
            return aw;
        } else {
            assert(hcls->type == HiddenClass::SINGLETON);
            appendNewHCAttr(aw, NULL);
            hcls->appendAttrwrapper();
            Py_DECREF(aw);
            return aw;
        }
    }
    return attrs->attr_list->attrs[offset];
}

extern "C" BORROWED(PyObject*) PyObject_GetAttrWrapper(PyObject* obj) noexcept {
    assert(0 && "check refcounting");
    return obj->getAttrWrapper();
}

BORROWED(Box*) unwrapAttrWrapper(Box* b) {
    assert(b->cls == attrwrapper_cls);
    return static_cast<AttrWrapper*>(b)->getUnderlying();
}

void attrwrapperSet(Box* b, Box* k, Box* v) {
    assert(b->cls == attrwrapper_cls);

    autoDecref(AttrWrapper::setitem(b, k, v));
}


Box* attrwrapperKeys(Box* b) {
    return AttrWrapper::keys(b);
}

void attrwrapperDel(Box* b, llvm::StringRef attr) {
    AttrWrapper::delitem(b, boxString(attr));
}

void attrwrapperClear(Box* aw) {
    AttrWrapper::_clear(aw);
}

BoxedDict* attrwrapperToDict(Box* b) {
    assert(b->cls == attrwrapper_cls);
    Box* d = AttrWrapper::copy(static_cast<AttrWrapper*>(b));
    assert(d->cls == dict_cls);
    return static_cast<BoxedDict*>(d);
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

static int type_set_abstractmethods(PyTypeObject* type, PyObject* value, void* context) noexcept {
    /* __abstractmethods__ should only be set once on a type, in
       abc.ABCMeta.__new__, so this function doesn't do anything
       special to update subclasses.
    */
    int abstract, res;
    if (value != NULL) {
        abstract = PyObject_IsTrue(value);
        if (abstract < 0)
            return -1;
        res = PyDict_SetItemString(type->tp_dict, "__abstractmethods__", value);
    } else {
        abstract = 0;
        res = PyDict_DelItemString(type->tp_dict, "__abstractmethods__");
        if (res && PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_SetString(PyExc_AttributeError, "__abstractmethods__");
            return -1;
        }
    }
    if (res == 0) {
        PyType_Modified(type);
        if (abstract)
            type->tp_flags |= Py_TPFLAGS_IS_ABSTRACT;
        else
            type->tp_flags &= ~Py_TPFLAGS_IS_ABSTRACT;
    }
    return res;
}

static void typeSetAbstractMethods(Box* _type, PyObject* value, void* context) {
    RELEASE_ASSERT(PyType_Check(_type), "");
    PyTypeObject* type = static_cast<PyTypeObject*>(_type);

    if (type_set_abstractmethods(type, value, context) == -1)
        throwCAPIException();
}

static PyObject* type_abstractmethods(PyTypeObject* type, void* context) noexcept {
    PyObject* mod = NULL;
    /* type itself has an __abstractmethods__ descriptor (this). Don't return
       that. */
    if (type != &PyType_Type)
        mod = PyDict_GetItemString(type->tp_dict, "__abstractmethods__");
    if (!mod) {
        PyErr_SetString(PyExc_AttributeError, "__abstractmethods__");
        return NULL;
    }
    Py_XINCREF(mod);
    return mod;
}

static Box* typeAbstractMethods(Box* _type, void* c) {
    RELEASE_ASSERT(PyType_Check(_type), "");
    PyTypeObject* type = static_cast<PyTypeObject*>(_type);
    Box* rtn = type_abstractmethods(type, c);
    if (!rtn)
        throwCAPIException();
    return rtn;
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
        static PyObject* comma = NULL;
        PyObject* abstract_methods = NULL;
        PyObject* builtins;
        PyObject* sorted;
        PyObject* sorted_methods = NULL;
        PyObject* joined = NULL;
        const char* joined_str;

        /* Compute ", ".join(sorted(type.__abstractmethods__))
           into joined. */
        abstract_methods = type_abstractmethods(type, NULL);
        if (abstract_methods == NULL)
            goto error;
        builtins = PyEval_GetBuiltins();
        if (builtins == NULL)
            goto error;
        // Pyston change: builtins is a module not a dict
        // sorted = PyDict_GetItemString(builtins, "sorted");
        sorted = builtins->getattr(autoDecref(internStringMortal("sorted")));
        if (sorted == NULL)
            goto error;
        sorted_methods = PyObject_CallFunctionObjArgs(sorted, abstract_methods, NULL);
        if (sorted_methods == NULL)
            goto error;
        if (comma == NULL) {
            // Pyston change:
            // comma = PyGC_ PyString_InternFromString(", ");
            comma = getStaticString(", ");
            if (comma == NULL)
                goto error;
        }
        joined = PyObject_CallMethod(comma, "join", "O", sorted_methods);
        if (joined == NULL)
            goto error;
        joined_str = PyString_AsString(joined);
        if (joined_str == NULL)
            goto error;

        PyErr_Format(PyExc_TypeError, "Can't instantiate abstract class %s "
                                      "with abstract methods %s",
                     type->tp_name, joined_str);
    error:
        Py_XDECREF(joined);
        Py_XDECREF(sorted_methods);
        Py_XDECREF(abstract_methods);
        return NULL;
    }
    return type->tp_alloc(type, 0);
}

static Box* typeName(Box* b, void*);
Box* objectRepr(Box* self) {
    BoxedClass* type = self->cls;
    DecrefHandle<Box, true> mod(NULL);
    try {
        mod = typeModule(type, NULL);
        if (!PyString_Check(mod))
            mod = NULL;
    } catch (ExcInfo e) {
        e.clear();
    }

    DecrefHandle<Box> name(typeName(type, NULL));
    if (mod != NULL && strcmp(PyString_AS_STRING(mod), "__builtin__"))
        return PyString_FromFormat("<%s.%s object at %p>", PyString_AS_STRING(mod), PyString_AS_STRING(name), self);
    return PyString_FromFormat("<%s object at %p>", type->tp_name, self);
}

static Box* object_str(Box* obj) noexcept {
    try {
        return obj->reprIC();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

Box* objectHash(Box* obj) {
    return boxInt(_Py_HashPointer(obj));
}

Box* objectSetattr(Box* obj, Box* attr, Box* value) {
    attr = coerceUnicodeToStr<CXX>(attr);
    if (attr->cls != str_cls) {
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", attr->cls->tp_name);
    }

    BoxedString* attr_str = static_cast<BoxedString*>(attr);
    setattrGeneric<NOT_REWRITABLE>(obj, attr_str, value, NULL);
    return incref(None);
}

Box* objectSubclasshook(Box* cls, Box* a) {
    return incref(NotImplemented);
}

static PyObject* import_copyreg(void) noexcept {
    static PyObject* copyreg_str;

    if (!copyreg_str) {
        // this is interned in cpython:
        copyreg_str = getStaticString("copy_reg");
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
                Py_DECREF(state);
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

static PyObject* object_get_class(PyObject* self, void* closure) noexcept {
    Py_INCREF(Py_TYPE(self));
    return (PyObject*)(Py_TYPE(self));
}

static int object_set_class(PyObject* self, PyObject* value, void* closure) noexcept {
    PyTypeObject* oldto = Py_TYPE(self);
    PyTypeObject* newto;

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "can't delete __class__ attribute");
        return -1;
    }
    if (!PyType_Check(value)) {
        PyErr_Format(PyExc_TypeError, "__class__ must be set to new-style class, not '%s' object",
                     Py_TYPE(value)->tp_name);
        return -1;
    }
    newto = (PyTypeObject*)value;
    if (!(newto->tp_flags & Py_TPFLAGS_HEAPTYPE) || !(oldto->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
        PyErr_Format(PyExc_TypeError, "__class__ assignment: only for heap types");
        return -1;
    }
    if (compatible_for_assignment(newto, oldto, "__class__")) {
        Py_INCREF(newto);
        Py_TYPE(self) = newto;
        Py_DECREF(oldto);
        return 0;
    } else {
        return -1;
    }
}

static PyMethodDef object_methods[] = {
    { "__reduce_ex__", object_reduce_ex, METH_VARARGS, NULL }, //
    { "__reduce__", object_reduce, METH_VARARGS, NULL },       //
    { "__format__", object_format, METH_VARARGS, PyDoc_STR("default object formatter") },
};

static Box* typeName(Box* b, void*) {
    RELEASE_ASSERT(PyType_Check(b), "");
    BoxedClass* type = static_cast<BoxedClass*>(b);

    if (type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        BoxedHeapClass* et = static_cast<BoxedHeapClass*>(type);
        return incref(et->ht_name);
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
    assert(PyType_Check(b));
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
    auto old_name = ht->ht_name;
    ht->ht_name = incref(s);
    ht->tp_name = s->data();
    Py_DECREF(old_name);
}

static Box* typeBases(Box* b, void*) {
    RELEASE_ASSERT(PyType_Check(b), "");
    BoxedClass* type = static_cast<BoxedClass*>(b);

    assert(type->tp_bases);
    return incref(type->tp_bases);
}

static void typeSetBases(Box* b, Box* v, void* c) {
    RELEASE_ASSERT(PyType_Check(b), "");
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

extern "C" void PyObject_InitHcAttrs(HCAttrs* attrs) noexcept {
    new ((void*)attrs) HCAttrs();
}

extern "C" PyObject* PyObject_GetHcAttrString(PyObject* obj, const char* attr) PYSTON_NOEXCEPT {
    return obj->getattr(autoDecref(internStringMortal(attr)));
}

extern "C" int PyObject_SetHcAttrString(PyObject* obj, const char* attr, PyObject* val) PYSTON_NOEXCEPT {
    obj->setattr(autoDecref(internStringMortal(attr)), val, NULL);
    return 0;
}

extern "C" int PyObject_DelHcAttrString(PyObject* obj, const char* attr) PYSTON_NOEXCEPT {
    BoxedString* attr_str = internStringMortal(attr);
    AUTO_DECREF(attr_str);
    bool has = obj->hasattr(attr_str);
    if (!has)
        return -1;
    obj->delattr(attr_str, NULL);
    return 0;
}

extern "C" int PyObject_ClearHcAttrs(HCAttrs* attrs) noexcept {
    attrs->clearForDealloc();
    return 0;
}

extern "C" int PyObject_TraverseHcAttrs(HCAttrs* attrs, visitproc visit, void* arg) noexcept {
    return attrs->traverse(visit, arg);
}

extern "C" PyVarObject* PyObject_InitVar(PyVarObject* op, PyTypeObject* tp, Py_ssize_t size) noexcept {
    assert(op);
    assert(tp);

    Py_TYPE(op) = tp;
    op->ob_size = size;
    _Py_NewReference(op);

    return op;
}

extern "C" PyObject* PyObject_Init(PyObject* op, PyTypeObject* tp) noexcept {
    if (op == NULL)
        return PyErr_NoMemory();

    assert(tp);

    Py_TYPE(op) = tp;
    _Py_NewReference(op);

    if (PyType_SUPPORTS_WEAKREFS(tp)) {
        *PyObject_GET_WEAKREFS_LISTPTR(op) = NULL;
    }

// I think CPython defers the dict creation (equivalent of our initUserAttrs) to the
// first time that an attribute gets set.
// Our HCAttrs object already includes this optimization of no-allocation-if-empty,
// but it's nice to initialize the hcls here so we don't have to check it on every getattr/setattr.
// TODO It does mean that anything not defering to this function will have to call
// initUserAttrs themselves, though.
// initUserAttrs(op, tp);

#ifndef NDEBUG
    if (tp->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        BoxedHeapClass* heap_cls = static_cast<BoxedHeapClass*>(tp);
        if (heap_cls->nslots() > 0) {
            PyMemberDef* mp = PyHeapType_GET_MEMBERS(heap_cls);
            for (int i = 0; i < heap_cls->nslots(); i++) {
                // This should be set to 0 on allocation:
                // (If it wasn't, we would need to initialize it to 0 here.)
                assert(*(Box**)((char*)op + mp[i].offset) == NULL);
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
    unicode = PyObject_New(PyUnicodeObject, &PyUnicode_Type);
    if (unicode == NULL)
        return NULL;
    unicode->str = (Py_UNICODE*)PyObject_MALLOC(new_size);
    if (!unicode->str) {
        Py_DECREF(unicode);
        return (PyUnicodeObject*)PyErr_NoMemory();
    }

#if STAT_ALLOCATIONS
    {
        size_t size = sizeof(PyUnicodeObject);
        ALLOC_STATS(unicode_cls);
    }
#endif

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

void dealloc_null(Box* box) {
    assert(box->cls->tp_del == NULL);
}

static Box* getsetGet(Box* self, Box* obj, Box* type) {
    // TODO: should call the full descr_check instead
    if (obj == NULL || obj == None)
        return incref(self);

    BoxedGetsetDescriptor* getset_descr = static_cast<BoxedGetsetDescriptor*>(self);

    assert(getset_descr->get != NULL);

    if (isSubclass(self->cls, pyston_getset_cls)) {
        return getset_descr->get(obj, getset_descr->closure);
    } else {
        RELEASE_ASSERT(isSubclass(self->cls, capi_getset_cls), "");
        Box* r = getset_descr->get(obj, getset_descr->closure);
        if (!r)
            throwCAPIException();
        return r;
    }
}

static Box* getsetSet(Box* self, Box* obj, Box* val) {
    assert(obj != NULL && obj != None);

    BoxedGetsetDescriptor* getset_descr = static_cast<BoxedGetsetDescriptor*>(self);

    if (getset_descr->set == NULL) {
        raiseExcHelper(AttributeError, "attribute '%s' of '%s' objects is not writable", getset_descr->name->data(),
                       obj->cls->tp_name);
    }

    if (isSubclass(self->cls, pyston_getset_cls)) {
        getset_descr->set_pyston(obj, val, getset_descr->closure);
        return incref(None);
    } else {
        RELEASE_ASSERT(isSubclass(self->cls, capi_getset_cls), "");
        int r = getset_descr->set_capi(obj, val, getset_descr->closure);
        if (r)
            throwCAPIException();
        return incref(None);
    }
}

static Box* getsetDelete(Box* self, Box* obj) {
    return getsetSet(self, obj, NULL);
}

void Box::clearAttrsForDealloc() {
    if (cls->instancesHaveHCAttrs()) {
        HCAttrs* attrs = getHCAttrsPtr();
        attrs->clearForDealloc();
        return;
    }

    if (cls->instancesHaveDictAttrs()) {
        BoxedDict** d = getDictPtr();
        Py_CLEAR(*d);
        return;
    }
}

void HiddenClass::dump() noexcept {
    if (type == SINGLETON || type == NORMAL) {
        if (type == SINGLETON)
            printf("Singleton hcls:\n");
        else
            printf("Normal hcls:\n");
        printf("Attrwrapper offset: %d\n", attrwrapper_offset);
        for (auto p : attr_offsets) {
            // printf("%d: %s\n", p.second, p.first->c_str());
            printf("%d: %p\n", p.second, p.first);
        }
    }
}

static void tupledealloc(PyTupleObject* op) noexcept {
    Py_ssize_t i;
    Py_ssize_t len = Py_SIZE(op);
    PyObject_GC_UnTrack(op);
    Py_TRASHCAN_SAFE_BEGIN(op) if (len > 0) {
        i = len;
        while (--i >= 0)
            Py_XDECREF(op->ob_item[i]);
#if PyTuple_MAXSAVESIZE > 0
        if (len < PyTuple_MAXSAVESIZE && numfree[len] < PyTuple_MAXFREELIST && Py_TYPE(op) == &PyTuple_Type) {
            op->ob_item[0] = (PyObject*)free_list[len];
            numfree[len]++;
            free_list[len] = op;
            goto done; /* return */
        }
#endif
    }
    op->ob_type->tp_free((PyObject*)op);
done:
    Py_TRASHCAN_SAFE_END(op)
}

static int tupletraverse(PyTupleObject* o, visitproc visit, void* arg) {
    Py_ssize_t i;

    for (i = Py_SIZE(o); --i >= 0;)
        Py_VISIT(o->ob_item[i]);
    return 0;
}

void BoxedModule::dealloc(Box* b) noexcept {
    BoxedModule* self = static_cast<BoxedModule*>(b);

    _PyObject_GC_UNTRACK(self);

    BoxedModule::clear(self);

    self->str_constants.~ContiguousMap();
    self->unicode_constants.~ContiguousMap();
    self->int_constants.~ContiguousMap();
    self->float_constants.~ContiguousMap();
    self->imaginary_constants.~ContiguousMap();
    self->long_constants.~ContiguousMap();
    assert(!self->keep_alive.size());

    b->cls->tp_free(self);
}

int BoxedModule::traverse(Box* _m, visitproc visit, void* arg) noexcept {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    Py_TRAVERSE(m->attrs);
    assert(!m->keep_alive.size());
    return 0;
}

template <typename CM> void clearContiguousMap(CM& cm) {
    CM tmp;
    std::swap(cm, tmp);

    for (auto&& p : tmp) {
        Py_DECREF(tmp.getMapped(p.second));
    }
}

extern "C" void _PyModule_Clear(PyObject* b) noexcept {
    BoxedModule* self = static_cast<BoxedModule*>(b);

    HCAttrs* attrs = self->getHCAttrsPtr();
    attrs->moduleClear();

    clearContiguousMap(self->str_constants);
    clearContiguousMap(self->unicode_constants);
    clearContiguousMap(self->int_constants);
    clearContiguousMap(self->float_constants);
    clearContiguousMap(self->imaginary_constants);
    clearContiguousMap(self->long_constants);

    assert(!self->keep_alive.size());
}

int BoxedModule::clear(Box* b) noexcept {
    _PyModule_Clear(b);
    b->clearAttrsForDealloc();

    return 0;
}

void BoxedSlice::dealloc(Box* b) noexcept {
    BoxedSlice* self = static_cast<BoxedSlice*>(b);

    Py_DECREF(self->step);
    Py_DECREF(self->start);
    Py_DECREF(self->stop);

    PyObject_Del(b);
}

void BoxedInstanceMethod::dealloc(Box* b) noexcept {
    BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(b);

    im->clearAttrsForDealloc();

    _PyObject_GC_UNTRACK(im);
    if (im->im_weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject*)im);
    Py_DECREF(im->func);
    Py_XDECREF(im->obj);
    Py_XDECREF(im->im_class);

#if 0
    if (numfree < PyMethod_MAXFREELIST) {
        im->obj = (PyObject *)free_list;
        free_list = im;
        numfree++;
    }
    else {
#endif
    PyObject_GC_Del(im);
#if 0
    }
#endif
}

int BoxedInstanceMethod::traverse(Box* _im, visitproc visit, void* arg) noexcept {
    BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(_im);
    Py_VISIT(im->func);
    Py_VISIT(im->obj);
    Py_VISIT(im->im_class);
    return 0;
}

bool IN_SHUTDOWN = false;
void BoxedClass::dealloc(Box* b) noexcept {
    BoxedClass* type = static_cast<BoxedClass*>(b);

    bool is_heaptype = (type->tp_flags & Py_TPFLAGS_HEAPTYPE);

    if (PyObject_IS_GC(type))
        _PyObject_GC_UNTRACK(type);

    // We can for the most part avoid this, but I think it's best not to:
    PyObject_ClearWeakRefs((PyObject*)type);

    type->clearAttrsForDealloc();

    Py_XDECREF(type->tp_dict);
    Py_XDECREF(type->tp_bases);
    Py_XDECREF(type->tp_subclasses);
    Py_XDECREF(type->tp_mro);
    Py_XDECREF(type->tp_base);

    if (is_heaptype) {
        BoxedHeapClass* htype = static_cast<BoxedHeapClass*>(type);
        Py_XDECREF(htype->ht_name);
        Py_XDECREF(htype->ht_slots);
    }

    // During shutdown, don't free class objects since that would make
    // debugging very hard.
    if (!IN_SHUTDOWN)
        Py_TYPE(type)->tp_free(type);

// Copied in the CPython implementation for reference:
#if 0
    /* Assert this is a heap-allocated type object */
    assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);
    _PyObject_GC_UNTRACK(type);
    PyObject_ClearWeakRefs((PyObject *)type);
    BoxedHeapClass* et = (BoxedHeapClass *)type;
    Py_XDECREF(type->tp_base);
    Py_XDECREF(type->tp_dict);
    Py_XDECREF(type->tp_bases);
    Py_XDECREF(type->tp_mro);
    Py_XDECREF(type->tp_cache);
    Py_XDECREF(type->tp_subclasses);
    /* A type's tp_doc is heap allocated, unlike the tp_doc slots
     * of most other objects.  It's okay to cast it to char *.
     */
    PyObject_Free((char *)type->tp_doc);
    Py_XDECREF(et->ht_name);
    Py_XDECREF(et->ht_slots);
    Py_TYPE(type)->tp_free((PyObject *)type);
#endif
}

static void object_dealloc(PyObject* self) {
    Py_TYPE(self)->tp_free(self);
}

static int type_is_gc(BoxedClass* type) {
    return type->is_pyston_class || (type->tp_flags & Py_TPFLAGS_HEAPTYPE);
}

static int type_traverse(PyTypeObject* type, visitproc visit, void* arg) {
    /* Because of type_is_gc(), the collector only calls this
       for heaptypes. */
    // Pyston change: HEAPTYPE is not about whether it is in GC or not
    // assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);

    Py_TRAVERSE(type->attrs);

    Py_VISIT(type->tp_dict);
    Py_VISIT(type->tp_cache);
    Py_VISIT(type->tp_mro);
    Py_VISIT(type->tp_bases);
    Py_VISIT(type->tp_base);

    /* There's no need to visit type->tp_subclasses or
       ((PyHeapTypeObject *)type)->ht_slots, because they can't be involved
       in cycles; tp_subclasses is a list of weak references,
       and slots is a tuple of strings. */

    return 0;
}

static int type_clear(PyTypeObject* type) {
    /* Because of type_is_gc(), the collector only calls this
       for heaptypes. */
    // Pyston change: HEAPTYPE is not about whether it is in GC or not
    // assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);

    /* We need to invalidate the method cache carefully before clearing
       the dict, so that other objects caught in a reference cycle
       don't start calling destroyed methods.

       Otherwise, the only field we need to clear is tp_mro, which is
       part of a hard cycle (its first element is the class itself) that
       won't be broken otherwise (it's a tuple and tuples don't have a
       tp_clear handler).  None of the other fields need to be
       cleared, and here's why:

       tp_cache:
           Not used; if it were, it would be a dict.

       tp_bases, tp_base:
           If these are involved in a cycle, there must be at least
           one other, mutable object in the cycle, e.g. a base
           class's dict; the cycle will be broken that way.

       tp_subclasses:
           A list of weak references can't be part of a cycle; and
           lists have their own tp_clear.

       slots (in PyHeapTypeObject):
           A tuple of strings can't be part of a cycle.
    */

    PyType_Modified(type);
    if (type->tp_dict)
        PyDict_Clear(type->tp_dict);
    type->attrs.clearForDealloc();
    Py_CLEAR(type->tp_dict);
    Py_CLEAR(type->tp_mro);

    return 0;
}

int HCAttrs::traverse(visitproc visit, void* arg) noexcept {
    if (!hcls)
        return 0;

    int nattrs = hcls->attributeArraySize();
    for (int i = 0; i < nattrs; i++) {
        Py_VISIT(attr_list->attrs[i]);
    }
    return 0;
}

void AttrWrapperIter::dealloc(Box* _o) noexcept {
    AttrWrapperIter* o = (AttrWrapperIter*)_o;

    o->cls->tp_free(o);
}

void BoxedClosure::dealloc(Box* _o) noexcept {
    BoxedClosure* o = (BoxedClosure*)_o;

    for (int i = 0; i < o->nelts; i++) {
        Py_XDECREF(o->elts[i]);
    }

    Py_XDECREF(o->parent);

    o->cls->tp_free(o);
}

int BoxedClosure::traverse(Box* _o, visitproc visit, void* arg) noexcept {
    BoxedClosure* o = (BoxedClosure*)_o;

    for (int i = 0; i < o->nelts; i++) {
        Py_VISIT(o->elts[i]);
    }

    return 0;
}

int BoxedClosure::clear(Box* _o) noexcept {
    BoxedClosure* o = (BoxedClosure*)_o;

    for (int i = 0; i < o->nelts; i++) {
        Py_CLEAR(o->elts[i]);
    }

    return 0;
}

void BoxedGetsetDescriptor::dealloc(Box* _o) noexcept {
    BoxedGetsetDescriptor* o = (BoxedGetsetDescriptor*)_o;

    Py_XDECREF(o->name);

    o->cls->tp_free(o);
}

#ifndef Py_REF_DEBUG
#define PRINT_TOTAL_REFS()
#else /* Py_REF_DEBUG */
#define PRINT_TOTAL_REFS() fprintf(stderr, "[%" PY_FORMAT_SIZE_T "d refs]\n", _Py_GetRefTotal())
#endif

std::vector<Box*> constants;
std::vector<Box**> constant_locations;
std::vector<Box*> late_constants;
extern "C" void PyGC_RegisterStaticConstantLocation(Box** ptr) noexcept {
    constant_locations.push_back(ptr);
}

extern "C" PyObject* PyGC_RegisterStaticConstant(Box* b) noexcept {
    constants.push_back(b);
    return b;
}

extern "C" void _PyUnicode_Fini(void);

static int _check_and_flush(FILE* stream) {
    int prev_fail = ferror(stream);
    return fflush(stream) || prev_fail ? EOF : 0;
}

extern "C" {
struct _inittab _PyImport_Inittab[] = { { "array", initarray },
                                        { "_ast", init_ast },
                                        { "binascii", initbinascii },
                                        { "_codecs", init_codecs },
                                        { "_collections", init_collections },
                                        { "cStringIO", initcStringIO },
                                        { "_csv", init_csv },
                                        { "datetime", initdatetime },
                                        { "errno", initerrno },
                                        { "fcntl", initfcntl },
                                        { "_functools", init_functools },
                                        { "imp", initimp },
                                        { "_io", init_io },
                                        { "itertools", inititertools },
                                        { "marshal", PyMarshal_Init },
                                        { "math", initmath },
                                        { "_md5", init_md5 },
                                        { "operator", initoperator },
                                        { "posix", initposix },
                                        { "pwd", initpwd },
                                        { "_random", init_random },
                                        { "resource", initresource },
                                        { "select", initselect },
                                        { "_sha", init_sha },
                                        { "_sha256", init_sha256 },
                                        { "_sha512", init_sha512 },
                                        { "signal", initsignal },
                                        { "_socket", init_socket },
                                        { "_sqlite3", init_sqlite3 },
                                        { "_sre", init_sre },
                                        { "_ssl", init_ssl },
                                        { "strop", initstrop },
                                        { "_struct", init_struct },
                                        { "time", inittime },
                                        { "unicodedata", initunicodedata },
                                        { "_warnings", _PyWarnings_Init },
                                        { "_weakref", init_weakref },
                                        { "zipimport", initzipimport },
                                        { "zlib", initzlib },

                                        { "gc", initgc },
                                        { "__pyston__", setupPyston },
                                        { "thread", setupThread },

                                        { "__main__", NULL },
                                        { "__builtin__", NULL },
                                        { "sys", NULL },
                                        { "exceptions", NULL },


                                        { 0, 0 } };
}

bool TRACK_ALLOCATIONS = false;
void setupRuntime() {
    root_hcls = HiddenClass::makeRoot();
    HiddenClass::dict_backed = HiddenClass::makeDictBacked();

    // We have to do a little dance to get object_cls and type_cls set up, since the normal
    // object-creation routines look at the class to see the allocation size.
    object_cls = static_cast<BoxedClass*>(_PyObject_GC_Malloc(sizeof(BoxedClass)));
    type_cls = static_cast<BoxedClass*>(_PyObject_GC_Malloc(sizeof(BoxedClass)));
    // Usually done by PyType_GenericAlloc:
    memset(object_cls, 0, sizeof(BoxedClass));
    memset(type_cls, 0, sizeof(BoxedClass));
    PyObject_INIT(object_cls, type_cls);
    PyObject_INIT(type_cls, type_cls);
    ::new (object_cls)
        BoxedClass(NULL, 0, 0, sizeof(Box), false, "object", true, object_dealloc, PyObject_Del, /* is_gc */ false);
    ::new (type_cls) BoxedClass(object_cls, offsetof(BoxedClass, attrs), offsetof(BoxedClass, tp_weaklist),
                                sizeof(BoxedHeapClass), false, "type", true, BoxedClass::dealloc, PyObject_GC_Del, true,
                                (traverseproc)type_traverse, (inquiry)type_clear);
    _PyObject_GC_TRACK(object_cls);
    _PyObject_GC_TRACK(type_cls);
    type_cls->tp_is_gc = (inquiry)type_is_gc;

    type_cls->tp_flags |= Py_TPFLAGS_TYPE_SUBCLASS;
    type_cls->tp_itemsize = sizeof(PyMemberDef);

    // XXX silly that we have to set this again
    new (&object_cls->attrs) HCAttrs(HiddenClass::makeSingleton());
    new (&type_cls->attrs) HCAttrs(HiddenClass::makeSingleton());
    object_cls->instances_are_nonzero = true;
    object_cls->tp_getattro = PyObject_GenericGetAttr;
    object_cls->tp_setattro = PyObject_GenericSetAttr;
    object_cls->tp_init = object_init;
    object_cls->tp_new = object_new;
    type_cls->tp_getattro = type_getattro;

    none_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(Box), false, "NoneType", false, NULL, NULL, /* is_gc */ false);
    None = new (none_cls) Box();
    constants.push_back(None);
    assert(None->cls);

    // You can't actually have an instance of basestring
    basestring_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(Box), false, "basestring", true, NULL, NULL, false);

    // We add 1 to the tp_basicsize of the BoxedString in order to hold the null byte at the end.
    // We use offsetof(BoxedString, s_data) as opposed to sizeof(BoxedString) so that we can
    // use the extra padding bytes at the end of the BoxedString.
    str_cls = new (0)
        BoxedClass(basestring_cls, 0, 0, offsetof(BoxedString, s_data) + 1, false, "str", true, NULL, NULL, false);
    str_cls->tp_flags |= Py_TPFLAGS_STRING_SUBCLASS;
    str_cls->tp_itemsize = sizeof(char);

    // It wasn't safe to add __base__ attributes until object+type+str are set up, so do that now:
    Py_INCREF(object_cls);
    type_cls->giveAttr("__base__", object_cls);
    Py_INCREF(object_cls);
    basestring_cls->giveAttr("__base__", object_cls);
    Py_INCREF(basestring_cls);
    str_cls->giveAttr("__base__", basestring_cls);
    Py_INCREF(object_cls);
    none_cls->giveAttr("__base__", object_cls);
    Py_INCREF(None);
    object_cls->giveAttr("__base__", None);

    // Not sure why CPython defines sizeof(PyTupleObject) to include one element,
    // but we copy that, which means we have to subtract that extra pointer to get the tp_basicsize:
    tuple_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedTuple) - sizeof(Box*), false, "tuple", true,
                                   (destructor)tupledealloc, NULL, true, (traverseproc)tupletraverse, NOCLEAR);

    tuple_cls->tp_flags |= Py_TPFLAGS_TUPLE_SUBCLASS;
    tuple_cls->tp_itemsize = sizeof(Box*);
    tuple_cls->tp_mro = BoxedTuple::create({ tuple_cls, object_cls });
    EmptyTuple = BoxedTuple::create({});
    late_constants.push_back(EmptyTuple);
    list_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedList), false, "list", true, BoxedList::dealloc, NULL,
                                  true, BoxedList::traverse, BoxedList::clear);
    list_cls->tp_flags |= Py_TPFLAGS_LIST_SUBCLASS;
    pyston_getset_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedGetsetDescriptor), false, "getset_descriptor",
                                           false, BoxedGetsetDescriptor::dealloc, NULL, false);
    attrwrapper_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(AttrWrapper), false, "attrwrapper", false, AttrWrapper::dealloc, NULL, true,
                   AttrWrapper::traverse, AttrWrapper::tp_clear);
    dict_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedDict), false, "dict", true, BoxedDict::dealloc, NULL,
                                  true, BoxedDict::traverse, BoxedDict::clear);
    dict_cls->tp_flags |= Py_TPFLAGS_DICT_SUBCLASS;
    int_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedInt), false, "int", true, BoxedInt::tp_dealloc,
                                 /*BoxedInt::tp_free*/ NULL, false);
    int_cls->tp_flags |= Py_TPFLAGS_INT_SUBCLASS;
    bool_cls = new (0) BoxedClass(int_cls, 0, 0, sizeof(BoxedBool), false, "bool", false, NULL, NULL, false);
    complex_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedComplex), false, "complex", true, NULL, NULL, false);
    long_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedLong), false, "long", true, NULL, NULL, false);
    long_cls->tp_flags |= Py_TPFLAGS_LONG_SUBCLASS;
    float_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(BoxedFloat), false, "float", true, BoxedFloat::tp_dealloc, NULL, false);
    function_cls = new (0) BoxedClass(object_cls, offsetof(BoxedFunction, attrs), offsetof(BoxedFunction, weakreflist),
                                      sizeof(BoxedFunction), false, "function", false, functionDtor, NULL, true,
                                      (traverseproc)func_traverse, func_clear);
    builtin_function_or_method_cls = new (0) BoxedClass(
        object_cls, 0, offsetof(BoxedBuiltinFunctionOrMethod, weakreflist), sizeof(BoxedBuiltinFunctionOrMethod), false,
        "builtin_function_or_method", false, functionDtor, NULL, true, (traverseproc)builtin_func_traverse, NOCLEAR);

    module_cls = new (0) BoxedClass(object_cls, offsetof(BoxedModule, attrs), 0, sizeof(BoxedModule), false, "module",
                                    true, BoxedModule::dealloc, NULL, true, BoxedModule::traverse, BoxedModule::clear);
    member_descriptor_cls = new (0) BoxedClass(object_cls, 0, 0, sizeof(BoxedMemberDescriptor), false,
                                               "member_descriptor", false, NULL, NULL, false);
    capifunc_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(BoxedCApiFunction), false, "capifunc", true, BoxedCApiFunction::dealloc,
                   NULL, true, BoxedCApiFunction::traverse, BoxedCApiFunction::clear);
    method_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(BoxedMethodDescriptor), false, "method_descriptor", false,
                   BoxedMethodDescriptor::dealloc, NULL, true, BoxedMethodDescriptor::traverse, NOCLEAR);
    wrapperobject_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(BoxedWrapperObject), false, "method-wrapper", false,
                   BoxedWrapperObject::dealloc, NULL, true, BoxedWrapperObject::traverse, NOCLEAR);
    wrapperdescr_cls = new (0)
        BoxedClass(object_cls, 0, 0, sizeof(BoxedWrapperDescriptor), false, "wrapper_descriptor", false,
                   BoxedWrapperDescriptor::dealloc, NULL, true, BoxedWrapperDescriptor::traverse, NOCLEAR);

    EmptyString = new (0) BoxedString("");
    constants.push_back(EmptyString);
    // Call InternInPlace rather than InternFromString since that will
    // probably try to return EmptyString
    PyString_InternInPlace((Box**)&EmptyString);
    for (int i = 0; i <= UCHAR_MAX; i++) {
        char c = (char)i;
        BoxedString* s = new (1) BoxedString(llvm::StringRef(&c, 1));
        PyString_InternInPlace((Box**)&s);
        characters[i] = s;
        constants.push_back(s);
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
    int_cls->tp_mro = BoxedTuple::create({ int_cls, object_cls });
    bool_cls->tp_mro = BoxedTuple::create({ bool_cls, int_cls, object_cls });
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
    constants.push_back(True);
    constants.push_back(False);

    // Need to initialize interned_ints early:
    setupInt();
    for (auto b : interned_ints)
        constants.push_back(b);



    // sys is the first module that needs to be set up, due to modules
    // being tracked in sys.modules:
    setupSys();

    // Weakrefs are used for tp_subclasses:
    init_weakref();

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

    dict_descr = new (pyston_getset_cls)
        BoxedGetsetDescriptor(getStaticString("__dict__"), typeSubDict, typeSubSetDict, NULL);
    constants.push_back(dict_descr);
    type_cls->giveAttrDescriptor("__dict__", typeDict, NULL);


    instancemethod_cls = BoxedClass::create(
        type_cls, object_cls, 0, offsetof(BoxedInstanceMethod, im_weakreflist), sizeof(BoxedInstanceMethod), false,
        "instancemethod", false, BoxedInstanceMethod::dealloc, NULL, true, BoxedInstanceMethod::traverse, NOCLEAR);

    slice_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedSlice), false, "slice", false,
                                   BoxedSlice::dealloc, NULL, false);
    set_cls = BoxedClass::create(type_cls, object_cls, 0, offsetof(BoxedSet, weakreflist), sizeof(BoxedSet), false,
                                 "set", true, BoxedSet::dealloc, NULL, true, BoxedSet::traverse, BoxedSet::clear);
    frozenset_cls
        = BoxedClass::create(type_cls, object_cls, 0, offsetof(BoxedSet, weakreflist), sizeof(BoxedSet), false,
                             "frozenset", true, BoxedSet::dealloc, NULL, true, BoxedSet::traverse, BoxedSet::clear);
    capi_getset_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedGetsetDescriptor), false, "getset",
                                         true, BoxedGetsetDescriptor::dealloc, NULL, false);
    closure_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedClosure), false, "closure", true,
                                     BoxedClosure::dealloc, NULL, true, BoxedClosure::traverse, BoxedClosure::clear);
    property_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedProperty), false, "property", true,
                                      BoxedProperty::dealloc, NULL, true, BoxedProperty::traverse, NOCLEAR);
    staticmethod_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedStaticmethod), false, "staticmethod",
                                          true, BoxedStaticmethod::dealloc, NULL, true, BoxedStaticmethod::traverse,
                                          BoxedStaticmethod::clear);
    classmethod_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedClassmethod), false, "classmethod",
                                         true, BoxedClassmethod::dealloc, NULL, true, BoxedClassmethod::traverse,
                                         BoxedClassmethod::clear);
    attrwrapperiter_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(AttrWrapperIter), false,
                                             "attrwrapperiter", true, AttrWrapperIter::dealloc, NULL, false);

    pyston_getset_cls->giveAttr("__get__",
                                new BoxedFunction(FunctionMetadata::create((void*)getsetGet, UNKNOWN, 3), { None }));
    capi_getset_cls->giveAttr("__get__",
                              new BoxedFunction(FunctionMetadata::create((void*)getsetGet, UNKNOWN, 3), { None }));
    pyston_getset_cls->giveAttr("__set__", new BoxedFunction(FunctionMetadata::create((void*)getsetSet, UNKNOWN, 3)));
    capi_getset_cls->giveAttr("__set__", new BoxedFunction(FunctionMetadata::create((void*)getsetSet, UNKNOWN, 3)));
    pyston_getset_cls->giveAttr("__delete__",
                                new BoxedFunction(FunctionMetadata::create((void*)getsetDelete, UNKNOWN, 2)));
    capi_getset_cls->giveAttr("__delete__",
                              new BoxedFunction(FunctionMetadata::create((void*)getsetDelete, UNKNOWN, 2)));
    pyston_getset_cls->freeze();
    capi_getset_cls->freeze();

    SLICE = typeFromClass(slice_cls);
    SET = typeFromClass(set_cls);
    FROZENSET = typeFromClass(frozenset_cls);

    object_cls->giveAttr("__repr__",
                         new BoxedFunction(FunctionMetadata::create((void*)objectRepr, UNKNOWN, 1, false, false)));
    object_cls->giveAttr("__subclasshook__",
                         boxInstanceMethod(object_cls, autoDecref(new BoxedFunction(FunctionMetadata::create(
                                                           (void*)objectSubclasshook, UNKNOWN, 2))),
                                           object_cls));
    // __setattr__ was already set to a WrapperDescriptor; it'd be nice to set this to a faster BoxedFunction
    // object_cls->setattr("__setattr__", new BoxedFunction(FunctionMetadata::create((void*)objectSetattr, UNKNOWN, 3)),
    // NULL);
    // but unfortunately that will set tp_setattro to slot_tp_setattro on object_cls and all already-made subclasses!
    // Punting on that until needed; hopefully by then we will have better Pyston slots support.

    auto typeCallObj = FunctionMetadata::create((void*)typeCall, UNKNOWN, 1, true, true);
    typeCallObj->internal_callable.capi_val = &typeCallInternal<CAPI>;
    typeCallObj->internal_callable.cxx_val = &typeCallInternal<CXX>;

    type_cls->giveAttrDescriptor("__name__", typeName, typeSetName);
    type_cls->giveAttrDescriptor("__bases__", typeBases, typeSetBases);
    type_cls->giveAttrDescriptor("__abstractmethods__", typeAbstractMethods, typeSetAbstractMethods);

    type_cls->giveAttr("__call__", new BoxedFunction(typeCallObj));

    type_cls->giveAttr(
        "__new__",
        new BoxedFunction(FunctionMetadata::create((void*)typeNewGeneric, UNKNOWN, 4, false, false), { NULL, NULL }));
    type_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)typeRepr, STR, 1)));
    type_cls->tp_hash = (hashfunc)_Py_HashPointer;
    type_cls->giveAttrDescriptor("__module__", typeModule, typeSetModule);
    type_cls->giveAttr("__mro__",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedClass, tp_mro)));
    type_cls->giveAttr("__flags__",
                       new BoxedMemberDescriptor(BoxedMemberDescriptor::LONG, offsetof(BoxedClass, tp_flags)));
    type_cls->giveAttr("__subclasses__",
                       new BoxedFunction(FunctionMetadata::create((void*)typeSubclasses, UNKNOWN, 1)));
    type_cls->giveAttr("mro", new BoxedFunction(FunctionMetadata::create((void*)typeMro, UNKNOWN, 1)));
    type_cls->tp_richcompare = type_richcompare;
    add_operators(type_cls);
    type_cls->freeze();

    type_cls->tp_new = type_new;
    type_cls->tpp_call.capi_val = &typeTppCall<CAPI>;
    type_cls->tpp_call.cxx_val = &typeTppCall<CXX>;

    none_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)none_repr, STR, 1)));
    none_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)noneNonzero, BOXED_BOOL, 1)));
    none_cls->giveAttrBorrowed("__doc__", None);
    none_cls->tp_hash = (hashfunc)_Py_HashPointer;
    none_cls->tp_new = NULL; // don't allow creating instances
    none_cls->freeze();
    none_cls->tp_repr = none_repr;

    module_cls->giveAttr(
        "__init__", new BoxedFunction(FunctionMetadata::create((void*)moduleInit, UNKNOWN, 3, false, false), { NULL }));
    module_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)moduleRepr, STR, 1)));
    module_cls->giveAttrBorrowed("__dict__", dict_descr);
    module_cls->freeze();

    closure_cls->freeze();

    setupUnwinding();
    setupCAPI();

    // Can't set up object methods until we set up CAPI support:
    for (auto& md : object_methods) {
        object_cls->giveAttr(md.ml_name, new BoxedMethodDescriptor(&md, object_cls));
    }
    object_cls->giveCapiAttrDescriptor("__class__", object_get_class, object_set_class);

    object_cls->tp_str = object_str;
    add_operators(object_cls);

    object_cls->freeze();

    assert(object_cls->tp_init == object_init);
    assert(object_cls->tp_new == object_new);
    assert(object_cls->tp_str == object_str);

    setupBool();
    setupLong();
    setupFloat();
    setupComplex();
    setupStr();
    setupList();
    setupDict();
    setupSet();
    setupTuple();
    setupGenerator();
    setupIter();
    setupClassobj();
    setupSuper();
    _PyUnicode_Init();
    unicode_cls->is_constant = true;
    unicode_cls->is_user_defined = false;
    _string_init();
    setupDescr();
    setupCode();
    setupFrame();

    function_cls->giveAttrBorrowed("__dict__", dict_descr);
    function_cls->giveAttrDescriptor("__name__", funcName, funcSetName);
    function_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)functionRepr, STR, 1)));
    function_cls->giveAttr("__module__", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                   offsetof(BoxedFunction, modname), false));
    function_cls->giveAttr(
        "__doc__", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedFunction, doc), false));
    function_cls->giveAttrBorrowed("func_doc", function_cls->getattr(getStaticString("__doc__")));
    function_cls->giveAttrDescriptor("__globals__", functionGlobals, NULL);
    function_cls->giveAttr("__get__", new BoxedFunction(FunctionMetadata::create((void*)functionGet, UNKNOWN, 3)));
    function_cls->giveAttr("__call__",
                           new BoxedFunction(FunctionMetadata::create((void*)functionCall, UNKNOWN, 1, true, true)));
    function_cls->giveAttr("__nonzero__",
                           new BoxedFunction(FunctionMetadata::create((void*)functionNonzero, BOXED_BOOL, 1)));
    function_cls->giveAttrDescriptor("func_code", functionCode, functionSetCode);
    function_cls->giveAttrBorrowed("__code__", function_cls->getattr(getStaticString("func_code")));
    function_cls->giveAttrBorrowed("func_name", function_cls->getattr(getStaticString("__name__")));
    function_cls->giveAttrDescriptor("func_defaults", functionDefaults, functionSetDefaults);
    function_cls->giveAttrBorrowed("__defaults__", function_cls->getattr(getStaticString("func_defaults")));
    function_cls->giveAttrBorrowed("func_globals", function_cls->getattr(getStaticString("__globals__")));
    function_cls->freeze();
    function_cls->tp_descr_get = function_descr_get;

    builtin_function_or_method_cls->giveAttr(
        "__module__",
        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedBuiltinFunctionOrMethod, modname)));
    builtin_function_or_method_cls->giveAttr(
        "__call__",
        new BoxedFunction(FunctionMetadata::create((void*)builtinFunctionOrMethodCall, UNKNOWN, 1, true, true)));

    builtin_function_or_method_cls->giveAttr(
        "__repr__", new BoxedFunction(FunctionMetadata::create((void*)builtinFunctionOrMethodRepr, STR, 1)));
    builtin_function_or_method_cls->giveAttrDescriptor("__name__", builtinFunctionOrMethodName, NULL);
    builtin_function_or_method_cls->giveAttr(
        "__doc__",
        new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedBuiltinFunctionOrMethod, doc), false));
    builtin_function_or_method_cls->freeze();

    instancemethod_cls->giveAttr(
        "__new__",
        new BoxedFunction(FunctionMetadata::create((void*)instancemethodNew, UNKNOWN, 4, false, false), { NULL }));
    instancemethod_cls->giveAttr("__repr__",
                                 new BoxedFunction(FunctionMetadata::create((void*)instancemethodRepr, STR, 1)));
    instancemethod_cls->giveAttr("__eq__",
                                 new BoxedFunction(FunctionMetadata::create((void*)instancemethodEq, UNKNOWN, 2)));
    instancemethod_cls->giveAttr(
        "__get__", new BoxedFunction(FunctionMetadata::create((void*)instancemethodGet, UNKNOWN, 3, false, false)));
    instancemethod_cls->giveAttr(
        "__call__", new BoxedFunction(FunctionMetadata::create((void*)instancemethodCall, UNKNOWN, 1, true, true)));
    instancemethod_cls->giveAttr(
        "im_func", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedInstanceMethod, func)));
    instancemethod_cls->giveAttrBorrowed("__func__", instancemethod_cls->getattr(getStaticString("im_func")));
    instancemethod_cls->giveAttr(
        "im_self", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedInstanceMethod, obj)));
    instancemethod_cls->giveAttrBorrowed("__self__", instancemethod_cls->getattr(getStaticString("im_self")));
    instancemethod_cls->freeze();

    instancemethod_cls->giveAttr("im_class", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                       offsetof(BoxedInstanceMethod, im_class), true));

    slice_cls->giveAttr(
        "__new__",
        new BoxedFunction(FunctionMetadata::create((void*)sliceNew, UNKNOWN, 4, false, false), { NULL, None }));
    slice_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)sliceRepr, STR, 1)));
    slice_cls->giveAttr("__hash__", new BoxedFunction(FunctionMetadata::create((void*)sliceHash, UNKNOWN, 1)));
    slice_cls->giveAttr("indices", new BoxedFunction(FunctionMetadata::create((void*)sliceIndices, BOXED_TUPLE, 2)));
    slice_cls->giveAttr("__reduce__", new BoxedFunction(FunctionMetadata::create((void*)sliceReduce, BOXED_TUPLE, 1)));
    slice_cls->giveAttr("start", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSlice, start)));
    slice_cls->giveAttr("stop", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSlice, stop)));
    slice_cls->giveAttr("step", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedSlice, step)));
    slice_cls->freeze();
    slice_cls->tp_compare = (cmpfunc)slice_compare;

    static PyMappingMethods attrwrapper_as_mapping;
    attrwrapper_cls->tp_as_mapping = &attrwrapper_as_mapping;
    static PySequenceMethods attrwrapper_as_sequence;
    attrwrapper_cls->tp_as_sequence = &attrwrapper_as_sequence;
    attrwrapper_cls->giveAttr("__setitem__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::setitem, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr(
        "pop",
        new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::pop, UNKNOWN, 3, false, false), { NULL }));
    attrwrapper_cls->giveAttr(
        "__getitem__", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::getitem<CXX>, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("__delitem__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::delitem, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("setdefault",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::setdefault, UNKNOWN, 3)));
    attrwrapper_cls->giveAttr(
        "get",
        new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::get, UNKNOWN, 3, false, false), { None }));
    attrwrapper_cls->giveAttr("__str__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::str, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr(
        "__contains__", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::contains<CXX>, BOXED_BOOL, 2)));
    attrwrapper_cls->giveAttr("has_key",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::hasKey, BOXED_BOOL, 2)));
    attrwrapper_cls->giveAttr("__eq__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::eq, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("__ne__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::ne, UNKNOWN, 2)));
    attrwrapper_cls->giveAttr("keys", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::keys, LIST, 1)));
    attrwrapper_cls->giveAttr("values",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::values, LIST, 1)));
    attrwrapper_cls->giveAttr("items", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::items, LIST, 1)));
    attrwrapper_cls->giveAttr("iterkeys",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::iterkeys, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("itervalues",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::itervalues, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("iteritems",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::iteritems, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("copy",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::copy, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr("clear", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::clear, NONE, 1)));
    attrwrapper_cls->giveAttr("__len__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::len, BOXED_INT, 1)));
    attrwrapper_cls->giveAttr("__iter__",
                              new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::iter, UNKNOWN, 1)));
    attrwrapper_cls->giveAttr(
        "update", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapper::update, NONE, 1, true, true)));
    attrwrapper_cls->freeze();
    attrwrapper_cls->tp_iter = AttrWrapper::iter;
    attrwrapper_cls->tp_as_mapping->mp_subscript = (binaryfunc)AttrWrapper::getitem<CAPI>;
    attrwrapper_cls->tp_as_mapping->mp_ass_subscript = (objobjargproc)AttrWrapper::ass_sub;
    attrwrapper_cls->tp_as_sequence->sq_contains = (objobjproc)AttrWrapper::sq_contains;

    attrwrapperiter_cls->giveAttr(
        "__hasnext__", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapperIter::hasnext, UNKNOWN, 1)));
    attrwrapperiter_cls->giveAttr(
        "next", new BoxedFunction(FunctionMetadata::create((void*)AttrWrapperIter::next, UNKNOWN, 1)));
    attrwrapperiter_cls->freeze();
    attrwrapperiter_cls->tp_iter = PyObject_SelfIter;
    attrwrapperiter_cls->tp_iternext = AttrWrapperIter::next_capi;

    PyType_Ready(&PyFile_Type);
    PyObject* sysin, *sysout, *syserr;
    sysin = PyFile_FromFile(stdin, "<stdin>", "r", NULL);
    sysout = PyFile_FromFile(stdout, "<stdout>", "w", _check_and_flush);
    syserr = PyFile_FromFile(stderr, "<stderr>", "w", _check_and_flush);
    RELEASE_ASSERT(!PyErr_Occurred(), "");

    sys_module->giveAttrBorrowed("stdout", sysout);
    sys_module->giveAttrBorrowed("stdin", sysin);
    sys_module->giveAttrBorrowed("stderr", syserr);
    sys_module->giveAttr("__stdout__", sysout);
    sys_module->giveAttr("__stdin__", sysin);
    sys_module->giveAttr("__stderr__", syserr);


    setupBuiltins();
    _PyExc_Init();
    _PyImport_Init();
    _PyImportHooks_Init();
    _PyWarnings_Init();

    PyType_Ready(&PyByteArrayIter_Type);
    PyType_Ready(&PyCapsule_Type);

    PyCallIter_Type.tpp_hasnext = calliterHasnextUnboxed;
    PyType_Ready(&PyCallIter_Type);

    PyType_Ready(&PyCObject_Type);
    PyType_Ready(&PyDictProxy_Type);
    PyType_Ready(&PyTraceBack_Type);

    assert(object_cls->tp_setattro == PyObject_GenericSetAttr);
    assert(none_cls->tp_setattro == PyObject_GenericSetAttr);

    assert(object_cls->tp_hash == (hashfunc)_Py_HashPointer);
    assert(none_cls->tp_hash == (hashfunc)_Py_HashPointer);
    assert(type_cls->tp_hash == (hashfunc)_Py_HashPointer);

    setupSysEnd();

    TRACK_ALLOCATIONS = true;

#if 0
    Box* l = NULL;
    for (int i = 0; i < 1000000000; i++) {
        //if (i % 10000 == 0) {
            //Py_XDECREF(l);
            //l = PyList_New(0);
        //}
        //PyList_Append(l, autoDecref(boxInt(i)));
        autoDecref(boxInt(i));
    }
    Py_XDECREF(l);

    // XXX
    PyGC_Collect(); // To make sure it creates any static objects
    IN_SHUTDOWN = true;
    PyType_ClearCache();
    PyOS_FiniInterrupts();
    _PyUnicode_Fini();
    for (auto b : constants) {
        Py_DECREF(b);
    }
    // May need to run multiple collections to collect everything:
    while (PyGC_Collect())
        ;
    _Py_ReleaseInternedStrings();
    for (auto b : classes) {
        if (!PyObject_IS_GC(b)) {
            b->clearAttrs();
            Py_CLEAR(b->tp_mro);
        }
        Py_DECREF(b);
    }
    // May need to run multiple collections to collect everything:
    while (PyGC_Collect())
        ;
    PRINT_TOTAL_REFS();
    exit(0);
    // XXX
#endif
}

BORROWED(BoxedModule*) createModule(BoxedString* name, const char* fn, const char* doc) noexcept {
    assert((!fn || strlen(fn)) && "probably wanted to set the fn to <stdin>?");

    BoxedDict* d = getSysModulesDict();

    BoxedModule* module = NULL;

    // Surprisingly, there are times that we need to return the existing module if
    // one exists:
    Box* existing = d->getOrNull(name);
    if (existing && PyModule_Check(existing)) {
        module = static_cast<BoxedModule*>(existing);
    } else {
        module = new BoxedModule();
        autoDecref(moduleInit(module, name, autoDecref(boxString(doc ? doc : ""))));
        d->d[incref(name)] = module;
    }

    if (fn)
        module->setattr(autoDecref(internStringMortal("__file__")), autoDecref(boxString(fn)), NULL);

    if (name->s() == "__main__")
        module->setattr(autoDecref(internStringMortal("__builtins__")), builtins_module, NULL);
    return module;
}

static void call_sys_exitfunc(void) {
    PyObject* exitfunc = PySys_GetObject("exitfunc");

    if (exitfunc) {
        PyObject* res;
        Py_INCREF(exitfunc);
        PySys_SetObject("exitfunc", (PyObject*)NULL);
        res = PyEval_CallObject(exitfunc, (PyObject*)NULL);
        if (res == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_SystemExit)) {
                PySys_WriteStderr("Error in sys.exitfunc:\n");
            }
            PyErr_Print();
        } else {
            Py_DECREF(res);
        }
        Py_DECREF(exitfunc);
    }

    if (Py_FlushLine())
        PyErr_Clear();
}

// this is a pyston addition defined in codecs.cpp
extern "C" int _PyCodecRegistry_Deinit(void) noexcept;

extern "C" void Py_Finalize() noexcept {
    // In the future this will have to wait for non-daemon
    // threads to finish

    // wait_for_thread_shutdown();

    if (PROFILE)
        g.func_addr_registry.dumpPerfMap();

    call_sys_exitfunc();
    // initialized = 0;

    PyImport_Cleanup();

#ifdef Py_REF_DEBUG
    IN_SHUTDOWN = true;

    // May need to run multiple collections to collect everything:
    while (true) {
        int freed = 0;
        freed += PyGC_Collect();

        // Doing a gc collection can create more constants (and others -- may have to do this whole thing in a loop)
        freed += constants.size();
        for (auto b : constants) {
            Py_DECREF(b);
        }
        constants.clear();

        if (!freed)
            break;
    }

    for (auto p : constant_locations) {
        Py_XDECREF(*p);
    }
    constant_locations.clear();

    clearAllICs();
    PyType_ClearCache();
    PyOS_FiniInterrupts();
    _PyCodecRegistry_Deinit();
    // TODO: we might have to do this in a loop:
    _PyUnicode_Fini();
    PyInterpreterState_Clear(PyThreadState_GET()->interp);

    for (auto b : constants) {
        Py_DECREF(b);
    }
    constants.clear();
    for (auto b : late_constants) {
        Py_DECREF(b);
    }
    late_constants.clear();

    for (auto b : classes) {
        if (!PyObject_IS_GC(b)) {
            b->getHCAttrsPtr()->_clearRaw();
            Py_CLEAR(b->tp_mro);
        }
        Py_DECREF(b);
    }
    // May need to run multiple collections to collect everything:
    while (PyGC_Collect())
        ;
    assert(!constants.size());
    assert(!constant_locations.size());

    _Py_ReleaseInternedStrings();

    BoxedList* garbage = static_cast<BoxedList*>(_PyGC_GetGarbage());
    int num_garbage_objects = garbage->size;

    // Free the garbage list, but let all the elements in it stay alive:
    for (int i = 0; i < num_garbage_objects; i++) {
        Py_INCREF(garbage->elts->elts[i]);
    }
    Py_DECREF(garbage);

    auto ts = PyThreadState_GET();
    bool other_threads = ((bool)ts->next) || (ts != ts->interp->tstate_head);

#endif

// PyGC_Collect());

// PyImport_Cleanup();
// _PyImport_Fini();

// _PyExc_Fini();

// _PyGILState_Fini();

#if 0
    /* Delete current thread */
    PyThreadState_Swap(NULL);
    PyInterpreterState_Delete(interp);

    /* Sundry finalizers */
    PyMethod_Fini();
    PyFrame_Fini();
    PyCFunction_Fini();
    PyTuple_Fini();
    PyList_Fini();
    PySet_Fini();
    PyString_Fini();
    PyByteArray_Fini();
    PyInt_Fini();
    PyFloat_Fini();
    PyDict_Fini();

#ifdef Py_USING_UNICODE
    /* Cleanup Unicode implementation */
    _PyUnicode_Fini();
#endif

    /* XXX Still allocated:
     * - various static ad-hoc pointers to interned strings
     * - int and float free list blocks
     * - whatever various modules and libraries allocate
     * */

    PyGrammar_RemoveAccelerators(&_PyParser_Grammar);

#ifdef Py_TRACE_REFS
    /* Display addresses (& refcnts) of all objects still alive.
     * * An address can be used to find the repr of the object, printed
     * * above by _Py_PrintReferences.
     * */
    if (Py_GETENV("PYTHONDUMPREFS"))
        _Py_PrintReferenceAddresses(stderr);
#endif /* Py_TRACE_REFS */
#ifdef PYMALLOC_DEBUG
    if (Py_GETENV("PYTHONMALLOCSTATS"))
        _PyObject_DebugMallocStats();
#endif
#endif

    teardownCodegen();

#ifdef Py_REF_DEBUG
    if (VERBOSITY())
        PRINT_TOTAL_REFS();

    bool assert_refs = true;
    if (num_garbage_objects) {
        if (VERBOSITY())
            fprintf(stderr, "[%d garbage objects]\n", num_garbage_objects);
        assert_refs = false;
    }

    if (other_threads) {
        if (VERBOSITY())
            fprintf(stderr, "[Other threads alive, can't free their refs]\n");
        assert_refs = false;
    }

    if (threading::forgot_refs_via_fork) {
        if (VERBOSITY())
            fprintf(stderr, "[Leaked refs via multithreaded fork]\n");
        assert_refs = false;
    }

    if (assert_refs) {
#ifdef Py_TRACE_REFS
        if (_Py_RefTotal != 0)
            _Py_PrintReferenceAddressesCapped(stderr, 10);
#endif

        RELEASE_ASSERT(_Py_RefTotal == 0, "%ld refs remaining!", _Py_RefTotal);
    }
#endif // Py_REF_DEBUG
}
}
