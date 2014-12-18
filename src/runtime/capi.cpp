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
#include <fcntl.h>
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

#define MAKE_CHECK(NAME, cls_name)                                                                                     \
    extern "C" bool Py##NAME##_Check(PyObject* op) { return isSubclass(op->cls, cls_name); }

MAKE_CHECK(Int, int_cls)
MAKE_CHECK(String, str_cls)
MAKE_CHECK(Long, long_cls)
MAKE_CHECK(List, list_cls)
MAKE_CHECK(Tuple, tuple_cls)
MAKE_CHECK(Dict, dict_cls)
MAKE_CHECK(Slice, slice_cls)

#ifdef Py_USING_UNICODE
MAKE_CHECK(Unicode, unicode_cls)
#endif

#undef MAKE_CHECK

extern "C" bool _PyIndex_Check(PyObject* op) {
    // TODO this is wrong (the CPython version checks for things that can be coerced to a number):
    return PyInt_Check(op);
}

extern "C" {
int Py_Py3kWarningFlag;
}

BoxedClass* capifunc_cls;

extern "C" PyObject* PyType_GenericAlloc(PyTypeObject* cls, Py_ssize_t nitems) {
    RELEASE_ASSERT(nitems == 0, "unimplemented");
    RELEASE_ASSERT(cls->tp_itemsize == 0, "unimplemented");

    auto rtn = (PyObject*)gc_alloc(cls->tp_basicsize, gc::GCKind::PYTHON);
    memset(rtn, 0, cls->tp_basicsize);

    PyObject_Init(rtn, cls);
    return rtn;
}

BoxedClass* wrapperdescr_cls, *wrapperobject_cls;

Box* BoxedWrapperDescriptor::__get__(BoxedWrapperDescriptor* self, Box* inst, Box* owner) {
    RELEASE_ASSERT(self->cls == wrapperdescr_cls, "");

    if (inst == None)
        return self;

    if (!isSubclass(inst->cls, self->type))
        raiseExcHelper(TypeError, "Descriptor '' for '%s' objects doesn't apply to '%s' object",
                       getFullNameOfClass(self->type).c_str(), getFullTypeName(inst).c_str());

    return new BoxedWrapperObject(self, inst);
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

extern "C" PyObject* PyObject_Repr(PyObject* obj) {
    try {
        return repr(obj);
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
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
        PyErr_SetObject(b->cls, b);
        return NULL;
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

extern "C" {
int _Py_SwappedOp[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
}

extern "C" long PyObject_Hash(PyObject* o) {
    try {
        return hash(o)->n;
    } catch (Box* b) {
        Py_FatalError("unimplemented");
    }
}

extern "C" long PyObject_HashNotImplemented(PyObject* self) {
    PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'", Py_TYPE(self)->tp_name);
    return -1;
}

extern "C" long _Py_HashPointer(void* p) {
    long x;
    size_t y = (size_t)p;
    /* bottom 3 or 4 bits are likely to be 0; rotate y by 4 to avoid
       excessive hash collisions for dicts and sets */
    y = (y >> 4) | (y << (8 * SIZEOF_VOID_P - 4));
    x = (long)y;
    if (x == -1)
        x = -2;
    return x;
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

extern "C" PyObject* PyEval_CallObjectWithKeywords(PyObject* func, PyObject* arg, PyObject* kw) {
    PyObject* result;

    if (arg == NULL) {
        arg = PyTuple_New(0);
        if (arg == NULL)
            return NULL;
    } else if (!PyTuple_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "argument list must be a tuple");
        return NULL;
    } else
        Py_INCREF(arg);

    if (kw != NULL && !PyDict_Check(kw)) {
        PyErr_SetString(PyExc_TypeError, "keyword list must be a dictionary");
        Py_DECREF(arg);
        return NULL;
    }

    result = PyObject_Call(func, arg, kw);
    Py_DECREF(arg);
    return result;
}

extern "C" PyObject* PyObject_Call(PyObject* callable_object, PyObject* args, PyObject* kw) {
    try {
        if (kw)
            return runtimeCall(callable_object, ArgPassSpec(0, 0, true, true), args, kw, NULL, NULL, NULL);
        else
            return runtimeCall(callable_object, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
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

extern "C" int PyObject_Print(PyObject* obj, FILE* fp, int flags) {
    Py_FatalError("unimplemented");
};

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

void checkAndThrowCAPIException() {
    Box* value = threading::cur_thread_state.curexc_value;
    if (value) {
        RELEASE_ASSERT(threading::cur_thread_state.curexc_traceback == NULL, "unsupported");

        // This doesn't seem like the right behavior...
        if (value->cls != threading::cur_thread_state.curexc_type) {
            if (value->cls == tuple_cls)
                value = runtimeCall(threading::cur_thread_state.curexc_type, ArgPassSpec(0, 0, true, false), value,
                                    NULL, NULL, NULL, NULL);
            else
                value = runtimeCall(threading::cur_thread_state.curexc_type, ArgPassSpec(1), value, NULL, NULL, NULL,
                                    NULL);
        }

        RELEASE_ASSERT(value->cls == threading::cur_thread_state.curexc_type, "unsupported");
        PyErr_Clear();
        throw value;
    }
}

extern "C" void PyErr_Restore(PyObject* type, PyObject* value, PyObject* traceback) {
    threading::cur_thread_state.curexc_type = type;
    threading::cur_thread_state.curexc_value = value;
    threading::cur_thread_state.curexc_traceback = traceback;
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
    return threading::cur_thread_state.curexc_type;
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
    RELEASE_ASSERT(o->cls != long_cls, "unhandled");

    RELEASE_ASSERT(isSubclass(o->cls, int_cls), "??");
    int64_t n = static_cast<BoxedInt*>(o)->n;
    static_assert(sizeof(n) == sizeof(Py_ssize_t), "");
    return n;
}

extern "C" Py_ssize_t PyUnicode_GET_SIZE(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_GET_DATA_SIZE(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" Py_UNICODE* PyUnicode_AS_UNICODE(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" const char* PyUnicode_AS_DATA(PyObject*) {
    Py_FatalError("unimplemented");
}

extern "C" int PyBuffer_IsContiguous(Py_buffer* view, char fort) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyErr_SetFromErrnoWithFilename(PyObject* exc, const char* filename) {
    PyObject* name = filename ? PyString_FromString(filename) : NULL;
    PyObject* result = PyErr_SetFromErrnoWithFilenameObject(exc, name);
    Py_XDECREF(name);
    return result;
}

extern "C" PyObject* PyErr_SetFromErrnoWithFilenameObject(PyObject* exc, PyObject* filenameObject) {
    PyObject* v;
    const char* s;
    int i = errno;
#ifdef PLAN9
    char errbuf[ERRMAX];
#endif
#ifdef MS_WINDOWS
    char* s_buf = NULL;
    char s_small_buf[28]; /* Room for "Windows Error 0xFFFFFFFF" */
#endif
#ifdef EINTR
    if (i == EINTR && PyErr_CheckSignals())
        return NULL;
#endif
#ifdef PLAN9
    rerrstr(errbuf, sizeof errbuf);
    s = errbuf;
#else
    if (i == 0)
        s = "Error"; /* Sometimes errno didn't get set */
    else
#ifndef MS_WINDOWS
        s = strerror(i);
#else
    {
        /* Note that the Win32 errors do not lineup with the
           errno error.  So if the error is in the MSVC error
           table, we use it, otherwise we assume it really _is_
           a Win32 error code
        */
        if (i > 0 && i < _sys_nerr) {
            s = _sys_errlist[i];
        } else {
            int len = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                    | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    NULL, /* no message source */
                                    i, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                    /* Default language */
                                    (LPTSTR)&s_buf, 0, /* size not used */
                                    NULL);             /* no args */
            if (len == 0) {
                /* Only ever seen this in out-of-mem
                   situations */
                sprintf(s_small_buf, "Windows Error 0x%X", i);
                s = s_small_buf;
                s_buf = NULL;
            } else {
                s = s_buf;
                /* remove trailing cr/lf and dots */
                while (len > 0 && (s[len - 1] <= ' ' || s[len - 1] == '.'))
                    s[--len] = '\0';
            }
        }
    }
#endif /* Unix/Windows */
#endif /* PLAN 9*/
    if (filenameObject != NULL)
        v = Py_BuildValue("(isO)", i, s, filenameObject);
    else
        v = Py_BuildValue("(is)", i, s);
    if (v != NULL) {
        PyErr_SetObject(exc, v);
        Py_DECREF(v);
    }
#ifdef MS_WINDOWS
    LocalFree(s_buf);
#endif
    return NULL;
}

extern "C" int PyOS_snprintf(char* str, size_t size, const char* format, ...) {
    int rc;
    va_list va;

    va_start(va, format);
    rc = PyOS_vsnprintf(str, size, format, va);
    va_end(va);
    return rc;
}

extern "C" int PyOS_vsnprintf(char* str, size_t size, const char* format, va_list va) {
    int len; /* # bytes written, excluding \0 */
#ifdef HAVE_SNPRINTF
#define _PyOS_vsnprintf_EXTRA_SPACE 1
#else
#define _PyOS_vsnprintf_EXTRA_SPACE 512
    char* buffer;
#endif
    assert(str != NULL);
    assert(size > 0);
    assert(format != NULL);
    /* We take a size_t as input but return an int.  Sanity check
     * our input so that it won't cause an overflow in the
     * vsnprintf return value or the buffer malloc size.  */
    if (size > INT_MAX - _PyOS_vsnprintf_EXTRA_SPACE) {
        len = -666;
        goto Done;
    }

#ifdef HAVE_SNPRINTF
    len = vsnprintf(str, size, format, va);
#else
    /* Emulate it. */
    buffer = (char*)PyMem_MALLOC(size + _PyOS_vsnprintf_EXTRA_SPACE);
    if (buffer == NULL) {
        len = -666;
        goto Done;
    }

    len = vsprintf(buffer, format, va);
    if (len < 0)
        /* ignore the error */;

    else if ((size_t)len >= size + _PyOS_vsnprintf_EXTRA_SPACE)
        Py_FatalError("Buffer overflow in PyOS_snprintf/PyOS_vsnprintf");

    else {
        const size_t to_copy = (size_t)len < size ? (size_t)len : size - 1;
        assert(to_copy < size);
        memcpy(str, buffer, to_copy);
        str[to_copy] = '\0';
    }
    PyMem_FREE(buffer);
#endif
Done:
    if (size > 0)
        str[size - 1] = '\0';
    return len;
#undef _PyOS_vsnprintf_EXTRA_SPACE
}

extern "C" void PyOS_AfterFork(void) {
    // TODO CPython does a number of things after a fork:
    // - clears pending signals
    // - updates the cached "main_pid"
    // - reinitialize and reacquire the GIL
    // - reinitialize the import lock
    // - change the definition of the main thread to the current thread
    // - call threading._after_fork
    // Also see PyEval_ReInitThreads

    // Should we disable finalizers after a fork?
    // In CPython, I think all garbage from other threads will never be freed and
    // their destructors never run.  I think for us, we will presumably collect it
    // and run the finalizers.  It's probably just safer to run no finalizers?

    // Our handling right now is pretty minimal... you better just call exec().

    PyEval_ReInitThreads();
    _PyImport_ReInitLock();
}

extern "C" {
static int dev_urandom_python(char* buffer, Py_ssize_t size) noexcept {
    int fd;
    Py_ssize_t n;

    if (size <= 0)
        return 0;

    Py_BEGIN_ALLOW_THREADS fd = ::open("/dev/urandom", O_RDONLY);
    Py_END_ALLOW_THREADS if (fd < 0) {
        if (errno == ENOENT || errno == ENXIO || errno == ENODEV || errno == EACCES)
            PyErr_SetString(PyExc_NotImplementedError, "/dev/urandom (or equivalent) not found");
        else
            PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    Py_BEGIN_ALLOW_THREADS do {
        do {
            n = read(fd, buffer, (size_t)size);
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            break;
        buffer += n;
        size -= (Py_ssize_t)n;
    }
    while (0 < size)
        ;
    Py_END_ALLOW_THREADS

        if (n <= 0) {
        /* stop on error or if read(size) returned 0 */
        if (n < 0)
            PyErr_SetFromErrno(PyExc_OSError);
        else
            PyErr_Format(PyExc_RuntimeError, "Failed to read %zi bytes from /dev/urandom", size);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}
}

extern "C" int _PyOS_URandom(void* buffer, Py_ssize_t size) {
    if (size < 0) {
        PyErr_Format(PyExc_ValueError, "negative argument not allowed");
        return -1;
    }
    if (size == 0)
        return 0;

#ifdef MS_WINDOWS
    return win32_urandom((unsigned char*)buffer, size, 1);
#else
#ifdef __VMS
    return vms_urandom((unsigned char*)buffer, size, 1);
#else
    return dev_urandom_python((char*)buffer, size);
#endif
#endif
}

BoxedModule* importTestExtension(const std::string& name) {
    std::string pathname_name = "test/test_extension/" + name + ".pyston.so";
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
    capifunc_cls = new BoxedHeapClass(type_cls, object_cls, NULL, 0, sizeof(BoxedCApiFunction), false);
    capifunc_cls->giveAttr("__name__", boxStrConstant("capifunc"));

    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));
    capifunc_cls->giveAttr("__str__", capifunc_cls->getattr("__repr__"));

    capifunc_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, true)));

    capifunc_cls->freeze();

    method_cls = new BoxedHeapClass(type_cls, object_cls, NULL, 0, sizeof(BoxedMethodDescriptor), false);
    method_cls->giveAttr("__name__", boxStrConstant("method"));
    method_cls->giveAttr("__get__",
                         new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__get__, UNKNOWN, 3)));
    method_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__call__, UNKNOWN, 2,
                                                                     0, true, true)));
    method_cls->freeze();

    wrapperdescr_cls = new BoxedHeapClass(type_cls, object_cls, NULL, 0, sizeof(BoxedWrapperDescriptor), false);
    wrapperdescr_cls->giveAttr("__name__", boxStrConstant("wrapper_descriptor"));
    wrapperdescr_cls->giveAttr("__get__",
                               new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__get__, UNKNOWN, 3)));
    wrapperdescr_cls->freeze();

    wrapperobject_cls = new BoxedHeapClass(type_cls, object_cls, NULL, 0, sizeof(BoxedWrapperObject), false);
    wrapperobject_cls->giveAttr("__name__", boxStrConstant("method-wrapper"));
    wrapperobject_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperObject::__call__, UNKNOWN, 1, 0, true, true)));
    wrapperobject_cls->freeze();
}

void teardownCAPI() {
}
}
