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

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* method_cls;
class BoxedMethodDescriptor : public Box {
public:
    PyMethodDef* method;
    BoxedClass* type;

    BoxedMethodDescriptor(PyMethodDef* method, BoxedClass* type) : Box(method_cls), method(method), type(type) {}

    static Box* __get__(BoxedMethodDescriptor* self, Box* inst, Box* owner) {
        RELEASE_ASSERT(self->cls == method_cls, "");

        if (inst == None)
            return self;
        // CPython apparently returns a "builtin_function_or_method" object
        return boxInstanceMethod(inst, self);
    }

    static Box* __call__(BoxedMethodDescriptor* self, Box* obj, BoxedTuple* varargs, Box** _args) {
        BoxedDict* kwargs = static_cast<BoxedDict*>(_args[0]);

        assert(self->cls == method_cls);
        assert(varargs->cls == tuple_cls);
        assert(kwargs->cls == dict_cls);

        if (!isSubclass(obj->cls, self->type))
            raiseExcHelper(TypeError, "descriptor '%s' requires a '%s' object but received a '%s'",
                           self->method->ml_name, getFullNameOfClass(self->type).c_str(), getFullTypeName(obj).c_str());

        threading::GLPromoteRegion _gil_lock;

        int ml_flags = self->method->ml_flags;
        Box* rtn;
        if (ml_flags == METH_NOARGS) {
            assert(varargs->elts.size() == 0);
            assert(kwargs->d.size() == 0);
            rtn = (Box*)self->method->ml_meth(obj, NULL);
        } else if (ml_flags == METH_VARARGS) {
            assert(kwargs->d.size() == 0);
            rtn = (Box*)self->method->ml_meth(obj, varargs);
        } else if (ml_flags == (METH_VARARGS | METH_KEYWORDS)) {
            rtn = (Box*)((PyCFunctionWithKeywords)self->method->ml_meth)(obj, varargs, kwargs);
        } else if (ml_flags == METH_O) {
            assert(kwargs->d.size() == 0);
            assert(varargs->elts.size() == 1);
            rtn = (Box*)self->method->ml_meth(obj, varargs->elts[0]);
        } else {
            RELEASE_ASSERT(0, "0x%x", ml_flags);
        }
        assert(rtn);
        return rtn;
    }
};

#define MAKE_CHECK(NAME, cls_name)                                                                                     \
    extern "C" bool Py##NAME##_Check(PyObject* op) { return isSubclass(op->cls, cls_name); }

MAKE_CHECK(Int, int_cls)
MAKE_CHECK(String, str_cls)
MAKE_CHECK(Long, long_cls)
MAKE_CHECK(List, list_cls)
MAKE_CHECK(Tuple, tuple_cls)
MAKE_CHECK(Dict, dict_cls)

#ifdef Py_USING_UNICODE
MAKE_CHECK(Unicode, unicode_cls)
#endif

#undef MAKE_CHECK

extern "C" { int Py_Py3kWarningFlag; }

extern "C" PyObject* PyDict_New() {
    return new BoxedDict();
}

extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) {
    Box* b = static_cast<Box*>(mp);
    Box* key = static_cast<Box*>(_key);
    Box* item = static_cast<Box*>(_item);

    static std::string setitem_str("__setitem__");
    Box* r;
    try {
        // TODO should demote GIL?
        r = callattrInternal(b, &setitem_str, CLASS_ONLY, NULL, ArgPassSpec(2), key, item, NULL, NULL, NULL);
    } catch (Box* b) {
        fprintf(stderr, "Error: uncaught error would be propagated to C code!\n");
        Py_FatalError("unimplemented");
    }

    RELEASE_ASSERT(r, "");
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) {
    return PyDict_SetItem(mp, boxStrConstant(key), item);
}

BoxedClass* capifunc_cls;

extern "C" void conservativeGCHandler(GCVisitor* v, Box* b) {
    v->visitPotentialRange((void* const*)b, (void* const*)((char*)b + b->cls->tp_basicsize));
}

extern "C" PyObject* PyType_GenericAlloc(PyTypeObject* cls, Py_ssize_t nitems) {
    RELEASE_ASSERT(nitems == 0, "unimplemented");
    RELEASE_ASSERT(cls->tp_itemsize == 0, "unimplemented");

    auto rtn = (PyObject*)gc_alloc(cls->tp_basicsize, gc::GCKind::PYTHON);
    memset(rtn, 0, cls->tp_basicsize);

    PyObject_Init(rtn, cls);
    return rtn;
}

BoxedClass* wrapperdescr_cls, *wrapperobject_cls;
struct wrapper_def {
    const char* name;
    int offset;
    int flags;
};

wrapper_def call_wrapper = { "__call__", offsetof(PyTypeObject, tp_call), PyWrapperFlag_KEYWORDS };

class BoxedWrapperDescriptor : public Box {
public:
    const wrapper_def* wrapper;
    BoxedClass* type;
    BoxedWrapperDescriptor(const wrapper_def* wrapper, BoxedClass* type)
        : Box(wrapperdescr_cls), wrapper(wrapper), type(type) {}

    static Box* __get__(BoxedWrapperDescriptor* self, Box* inst, Box* owner);
};

class BoxedWrapperObject : public Box {
public:
    BoxedWrapperDescriptor* descr;
    Box* obj;

    BoxedWrapperObject(BoxedWrapperDescriptor* descr, Box* obj) : Box(wrapperobject_cls), descr(descr), obj(obj) {}

    static Box* __call__(BoxedWrapperObject* self, Box* args, Box* kwds) {
        assert(self->cls == wrapperobject_cls);
        assert(args->cls == tuple_cls);
        assert(kwds->cls == dict_cls);

        int flags = self->descr->wrapper->flags;
        char* ptr = (char*)self->descr->type + self->descr->wrapper->offset;

        if (flags & PyWrapperFlag_KEYWORDS) {
            PyCFunctionWithKeywords f = *(PyCFunctionWithKeywords*)ptr;
            return f(self->obj, args, kwds);
        } else {
            abort();
        }
        abort();
    }
};

Box* BoxedWrapperDescriptor::__get__(BoxedWrapperDescriptor* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == wrapperdescr_cls, "");

    if (inst == None)
        return self;

    if (!isSubclass(inst->cls, self->type))
        raiseExcHelper(TypeError, "Descriptor '' for '%s' objects doesn't apply to '%s' object",
                       getFullNameOfClass(self->type).c_str(), getFullTypeName(inst).c_str());

    return new BoxedWrapperObject(self, inst);
}

PyObject* tp_new_wrapper(PyTypeObject* self, BoxedTuple* args, Box* kwds) {
    RELEASE_ASSERT(isSubclass(self->cls, type_cls), "");

    // ASSERT(self->tp_new != Py_CallPythonNew, "going to get in an infinite loop");

    RELEASE_ASSERT(args->cls == tuple_cls, "");
    RELEASE_ASSERT(kwds->cls == dict_cls, "");
    RELEASE_ASSERT(args->elts.size() >= 1, "");

    BoxedClass* subtype = static_cast<BoxedClass*>(args->elts[0]);
    RELEASE_ASSERT(isSubclass(subtype->cls, type_cls), "");
    RELEASE_ASSERT(isSubclass(subtype, self), "");

    BoxedTuple* new_args = new BoxedTuple(BoxedTuple::GCVector(args->elts.begin() + 1, args->elts.end()));

    return self->tp_new(subtype, new_args, kwds);
}

extern "C" int PyType_Ready(PyTypeObject* cls) {
    gc::registerNonheapRootObject(cls);

    // unhandled fields:
    RELEASE_ASSERT(cls->tp_print == NULL, "");
    RELEASE_ASSERT(cls->tp_getattr == NULL, "");
    RELEASE_ASSERT(cls->tp_setattr == NULL, "");
    RELEASE_ASSERT(cls->tp_compare == NULL, "");
    RELEASE_ASSERT(cls->tp_repr == NULL, "");
    RELEASE_ASSERT(cls->tp_as_number == NULL, "");
    RELEASE_ASSERT(cls->tp_as_sequence == NULL, "");
    RELEASE_ASSERT(cls->tp_as_mapping == NULL, "");
    RELEASE_ASSERT(cls->tp_hash == NULL, "");
    RELEASE_ASSERT(cls->tp_str == NULL, "");
    RELEASE_ASSERT(cls->tp_getattro == NULL || cls->tp_getattro == PyObject_GenericGetAttr, "");
    RELEASE_ASSERT(cls->tp_setattro == NULL, "");
    RELEASE_ASSERT(cls->tp_as_buffer == NULL, "");

    int ALLOWABLE_FLAGS = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC;
    RELEASE_ASSERT((cls->tp_flags & ~ALLOWABLE_FLAGS) == 0, "");
    // RELEASE_ASSERT(cls->tp_traverse == NULL, "");
    // RELEASE_ASSERT(cls->tp_clear == NULL, "");

    RELEASE_ASSERT(cls->tp_richcompare == NULL, "");
    RELEASE_ASSERT(cls->tp_iter == NULL, "");
    RELEASE_ASSERT(cls->tp_iternext == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_dict == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_get == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_set == NULL, "");
    RELEASE_ASSERT(cls->tp_init == NULL, "");
    RELEASE_ASSERT(cls->tp_alloc == NULL, "");
    RELEASE_ASSERT(cls->tp_free == NULL || cls->tp_free == PyObject_Del, "");
    RELEASE_ASSERT(cls->tp_is_gc == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_mro == NULL, "");
    RELEASE_ASSERT(cls->tp_cache == NULL, "");
    RELEASE_ASSERT(cls->tp_subclasses == NULL, "");
    RELEASE_ASSERT(cls->tp_weaklist == NULL, "");
    RELEASE_ASSERT(cls->tp_del == NULL, "");
    RELEASE_ASSERT(cls->tp_version_tag == 0, "");

// I think it is safe to ignore tp_weaklistoffset for now:
// RELEASE_ASSERT(cls->tp_weaklistoffset == 0, "");

#define INITIALIZE(a) new (&(a)) decltype(a)
    INITIALIZE(cls->attrs);
    INITIALIZE(cls->dependent_icgetattrs);
#undef INITIALIZE

    BoxedClass* base = cls->base = object_cls;
    if (!cls->cls)
        cls->cls = cls->base->cls;

    assert(cls->tp_name);
    cls->giveAttr("__name__", boxStrConstant(cls->tp_name));
    // tp_name
    // tp_basicsize, tp_itemsize
    // tp_doc

    if (!cls->tp_new && base != object_cls)
        cls->tp_new = base->tp_new;

    if (cls->tp_new) {
        cls->giveAttr("__new__",
                      new BoxedCApiFunction(METH_VARARGS | METH_KEYWORDS, cls, "__new__", (PyCFunction)tp_new_wrapper));
    }

    if (cls->tp_call) {
        cls->giveAttr("__call__", new BoxedWrapperDescriptor(&call_wrapper, cls));
    }

    if (!cls->tp_alloc) {
        cls->tp_alloc = reinterpret_cast<decltype(cls->tp_alloc)>(PyType_GenericAlloc);
    }

    for (PyMethodDef* method = cls->tp_methods; method && method->ml_name; ++method) {
        cls->giveAttr(method->ml_name, new BoxedMethodDescriptor(method, cls));
    }

    for (PyMemberDef* member = cls->tp_members; member && member->name; ++member) {
        cls->giveAttr(member->name, new BoxedMemberDescriptor(member));
    }

    if (cls->tp_getset) {
        if (VERBOSITY())
            printf("warning: ignoring tp_getset for now\n");
    }

    cls->gc_visit = &conservativeGCHandler;

    // TODO not sure how we can handle extension types that manually
    // specify a dict...
    RELEASE_ASSERT(cls->tp_dictoffset == 0, "");
    // this should get automatically initialized to 0 on this path:
    assert(cls->attrs_offset == 0);

    return 0;
}

extern "C" int PyType_IsSubtype(PyTypeObject*, PyTypeObject*) {
    Py_FatalError("unimplemented");
}

// copied from CPython's getargs.c:
extern "C" int PyBuffer_FillInfo(Py_buffer* view, PyObject* obj, void* buf, Py_ssize_t len, int readonly, int flags) {
    if (view == NULL)
        return 0;
    if (((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) && (readonly == 1)) {
        // Don't support PyErr_SetString yet:
        assert(0);
        // PyErr_SetString(PyExc_BufferError, "Object is not writable.");
        // return -1;
    }

    view->obj = obj;
    if (obj)
        Py_INCREF(obj);
    view->buf = buf;
    view->len = len;
    view->readonly = readonly;
    view->itemsize = 1;
    view->format = NULL;
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT)
        view->format = "B";
    view->ndim = 1;
    view->shape = NULL;
    if ((flags & PyBUF_ND) == PyBUF_ND)
        view->shape = &(view->len);
    view->strides = NULL;
    if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES)
        view->strides = &(view->itemsize);
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

extern "C" void PyBuffer_Release(Py_buffer* view) {
    if (!view->buf) {
        assert(!view->obj);
        return;
    }

    PyObject* obj = view->obj;
    assert(obj);
    assert(obj->cls == str_cls);
    if (obj && Py_TYPE(obj)->tp_as_buffer && Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer)
        Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer(obj, view);
    Py_XDECREF(obj);
    view->obj = NULL;
}

// Not sure why we need another declaration here:
extern "C" void Py_FatalError(const char* msg) __attribute__((__noreturn__));
extern "C" void Py_FatalError(const char* msg) {
    fprintf(stderr, "\nFatal Python error: %s\n", msg);
    abort();
}

extern "C" void _PyErr_BadInternalCall(const char* filename, int lineno) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_Init(PyObject* op, PyTypeObject* tp) {
    RELEASE_ASSERT(op, "");
    RELEASE_ASSERT(tp, "");

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

extern "C" PyVarObject* PyObject_InitVar(PyVarObject* op, PyTypeObject* tp, Py_ssize_t size) {
    assert(gc::isValidGCObject(op));
    assert(gc::isValidGCObject(tp));

    RELEASE_ASSERT(op, "");
    RELEASE_ASSERT(tp, "");
    Py_TYPE(op) = tp;
    op->ob_size = size;
    return op;
}

extern "C" PyObject* _PyObject_New(PyTypeObject* cls) {
    assert(cls->tp_itemsize == 0);

    auto rtn = (PyObject*)gc_alloc(cls->tp_basicsize, gc::GCKind::PYTHON);
    // no memset for this function

    PyObject_Init(rtn, cls);
    return rtn;
}

extern "C" void PyObject_Free(void* p) {
    gc::gc_free(p);
    ASSERT(0, "I think this is good enough but I'm not sure; should test");
}

extern "C" PyObject* _PyObject_GC_Malloc(size_t) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* _PyObject_GC_New(PyTypeObject* cls) {
    return _PyObject_New(cls);
}

extern "C" PyVarObject* _PyObject_GC_NewVar(PyTypeObject*, Py_ssize_t) {
    Py_FatalError("unimplemented");
}

extern "C" void PyObject_GC_Track(void*) {
    // TODO do we have to do anything to support the C API GC protocol?
}

extern "C" void PyObject_GC_UnTrack(void*) {
    // TODO do we have to do anything to support the C API GC protocol?
}

extern "C" void PyObject_GC_Del(void*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_CallObject(PyObject* obj, PyObject* args) {
    RELEASE_ASSERT(args, ""); // actually it looks like this is allowed to be NULL
    RELEASE_ASSERT(args->cls == tuple_cls, "");

    // TODO do something like this?  not sure if this is safe; will people expect that calling into a known function
    // won't end up doing a GIL check?
    // threading::GLDemoteRegion _gil_demote;

    try {
        Box* r = runtimeCall(obj, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
        return r;
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyObject_CallMethod(PyObject* o, char* name, char* format, ...) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* _PyObject_CallMethod_SizeT(PyObject* o, char* name, char* format, ...) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_GetAttrString(PyObject* o, const char* attr) {
    // TODO do something like this?  not sure if this is safe; will people expect that calling into a known function
    // won't end up doing a GIL check?
    // threading::GLDemoteRegion _gil_demote;

    try {
        return getattr(o, attr);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" Py_ssize_t PyObject_Size(PyObject* o) {
    try {
        return len(o)->n;
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyObject_GetIter(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_GetAttr(PyObject* o, PyObject* attr_name) {
    if (!isSubclass(attr_name->cls, str_cls)) {
        PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr_name)->tp_name);
        return NULL;
    }

    try {
        return getattr(o, static_cast<BoxedString*>(attr_name)->s.c_str());
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyObject_GenericGetAttr(PyObject* o, PyObject* name) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_GetItem(PyObject* o, PyObject* key) {
    try {
        return getitem(o, key);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" int PyObject_SetItem(PyObject* o, PyObject* key, PyObject* v) {
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_DelItem(PyObject* o, PyObject* key) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_RichCompare(PyObject* o1, PyObject* o2, int opid) {
    Py_FatalError("unimplemented");
}

extern "C" long PyObject_Hash(PyObject* o) {
    try {
        return hash(o)->n;
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" int PyObject_IsTrue(PyObject* o) {
    try {
        return nonzero(o);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}


extern "C" int PyObject_Not(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyObject_Call(PyObject* callable_object, PyObject* args, PyObject* kw) {
    try {
        return runtimeCall(callable_object, ArgPassSpec(0, 0, true, true), args, kw, NULL, NULL, NULL);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" void PyObject_ClearWeakRefs(PyObject* object) {
    Py_FatalError("unimplemented");
}

extern "C" int PyObject_GetBuffer(PyObject* exporter, Py_buffer* view, int flags) {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_Check(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PySequence_Size(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_Concat(PyObject* o1, PyObject* o2) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_Repeat(PyObject* o, Py_ssize_t count) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_InPlaceConcat(PyObject* o1, PyObject* o2) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_InPlaceRepeat(PyObject* o, Py_ssize_t count) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_GetItem(PyObject* o, Py_ssize_t i) {
    try {
        // Not sure if this is really the same:
        return getitem(o, boxInt(i));
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PySequence_GetSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) {
    try {
        // Not sure if this is really the same:
        return getitem(o, new BoxedSlice(boxInt(i1), boxInt(i2), None));
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" int PySequence_SetItem(PyObject* o, Py_ssize_t i, PyObject* v) {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_DelItem(PyObject* o, Py_ssize_t i) {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_SetSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2, PyObject* v) {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_DelSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PySequence_Count(PyObject* o, PyObject* value) {
    Py_FatalError("unimplemented");
}

extern "C" int PySequence_Contains(PyObject* o, PyObject* value) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PySequence_Index(PyObject* o, PyObject* value) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_List(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_Tuple(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PySequence_Fast(PyObject* o, const char* m) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyIter_Next(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" int PyCallable_Check(PyObject* x) {
    if (x == NULL)
        return 0;

    static const std::string call_attr("__call__");
    return typeLookup(x->cls, call_attr, NULL) != NULL;
}


extern "C" void PyErr_Restore(PyObject* type, PyObject* value, PyObject* traceback) {
    Py_FatalError("setting exceptions from the C API is current unimplemented");
}

extern "C" void PyErr_Clear() {
    PyErr_Restore(NULL, NULL, NULL);
}

extern "C" void PyErr_SetString(PyObject* exception, const char* string) {
    PyErr_SetObject(exception, boxStrConstant(string));
}

extern "C" void PyErr_SetObject(PyObject* exception, PyObject* value) {
    PyErr_Restore(exception, value, NULL);
}

extern "C" PyObject* PyErr_Format(PyObject* exception, const char* format, ...) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyErr_NoMemory() {
    Py_FatalError("unimplemented");
}

extern "C" int PyErr_CheckSignals() {
    Py_FatalError("unimplemented");
}

extern "C" int PyErr_ExceptionMatches(PyObject* exc) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyErr_Occurred() {
    // While there clearly needs to be more here, I think this is ok for now because all of the exception-setting
    // functions will abort()
    return NULL;
}

extern "C" int PyErr_WarnEx(PyObject* category, const char* text, Py_ssize_t stacklevel) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyErr_SetFromErrno(PyObject* type) {
    Py_FatalError("unimplemented");
    return NULL;
}

extern "C" PyObject* PyImport_Import(PyObject* module_name) {
    RELEASE_ASSERT(module_name, "");
    RELEASE_ASSERT(module_name->cls == str_cls, "");

    try {
        return import(-1, None, &static_cast<BoxedString*>(module_name)->s);
    } catch (Box* e) {
        Py_FatalError("unimplemented");
    }
}


extern "C" PyObject* PyCallIter_New(PyObject* callable, PyObject* sentinel) {
    Py_FatalError("unimplemented");
}

extern "C" void* PyMem_Malloc(size_t sz) {
    return gc_compat_malloc(sz);
}

extern "C" void* PyMem_Realloc(void* ptr, size_t sz) {
    return gc_compat_realloc(ptr, sz);
}

extern "C" void PyMem_Free(void* ptr) {
    gc_compat_free(ptr);
}

extern "C" int PyNumber_Check(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Add(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::Add);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Subtract(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::Sub);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Multiply(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::Mult);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Divide(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::Div);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_FloorDivide(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_TrueDivide(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Remainder(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::Mod);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Divmod(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Power(PyObject*, PyObject*, PyObject* o3) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Negative(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Positive(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Absolute(PyObject* o) {
    try {
        return abs_(o);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Invert(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Lshift(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Rshift(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::RShift);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_And(PyObject* lhs, PyObject* rhs) {
    try {
        return binop(lhs, rhs, AST_TYPE::BitAnd);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" PyObject* PyNumber_Xor(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Or(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceAdd(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceSubtract(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceMultiply(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceDivide(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceFloorDivide(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceTrueDivide(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceRemainder(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlacePower(PyObject*, PyObject*, PyObject* o3) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceLshift(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceRshift(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceAnd(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceXor(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_InPlaceOr(PyObject*, PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" int PyNumber_Coerce(PyObject**, PyObject**) {
    Py_FatalError("unimplemented");
}

extern "C" int PyNumber_CoerceEx(PyObject**, PyObject**) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Int(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Long(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Float(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_Index(PyObject* o) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyNumber_ToBase(PyObject* n, int base) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyNumber_AsSsize_t(PyObject* o, PyObject* exc) {
    Py_FatalError("unimplemented");
}

BoxedModule* importTestExtension(const std::string& name) {
    std::string pathname_name = "../test/test_extension/" + name + ".pyston.so";
    const char* pathname = pathname_name.c_str();
    void* handle = dlopen(pathname, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    assert(handle);

    std::string initname = "init" + name;
    void (*init)() = (void (*)())dlsym(handle, initname.c_str());

    char* error;
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    assert(init);
    (*init)();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStrConstant(name.c_str());
    Box* _m = sys_modules->d[s];
    RELEASE_ASSERT(_m, "module failed to initialize properly?");
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);
    m->setattr("__file__", boxStrConstant(pathname), NULL);
    m->fn = pathname;
    return m;
}

void setupCAPI() {
    capifunc_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedCApiFunction), false);
    capifunc_cls->giveAttr("__name__", boxStrConstant("capifunc"));

    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));
    capifunc_cls->giveAttr("__str__", capifunc_cls->getattr("__repr__"));

    capifunc_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, true)));

    capifunc_cls->freeze();

    method_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedMethodDescriptor), false);
    method_cls->giveAttr("__name__", boxStrConstant("method"));
    method_cls->giveAttr("__get__",
                         new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__get__, UNKNOWN, 3)));
    method_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__call__, UNKNOWN, 2,
                                                                     0, true, true)));
    method_cls->freeze();

    wrapperdescr_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedWrapperDescriptor), false);
    wrapperdescr_cls->giveAttr("__name__", boxStrConstant("wrapper_descriptor"));
    wrapperdescr_cls->giveAttr("__get__",
                               new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__get__, UNKNOWN, 3)));
    wrapperdescr_cls->freeze();

    wrapperobject_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(BoxedWrapperObject), false);
    wrapperobject_cls->giveAttr("__name__", boxStrConstant("method-wrapper"));
    wrapperobject_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperObject::__call__, UNKNOWN, 1, 0, true, true)));
    wrapperobject_cls->freeze();
}

void teardownCAPI() {
}
}
