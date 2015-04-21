// Copyright (c) 2014-2015 Dropbox, Inc.
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

#include "llvm/Support/ErrorHandling.h" // For llvm_unreachable
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/file.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/traceback.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* method_cls;

extern "C" bool _PyIndex_Check(PyObject* op) noexcept {
    // TODO this is wrong (the CPython version checks for things that can be coerced to a number):
    return PyInt_Check(op);
}

extern "C" bool _PyObject_CheckBuffer(PyObject* obj) noexcept {
    return ((Py_TYPE(obj)->tp_as_buffer != NULL) && (PyType_HasFeature(Py_TYPE(obj), Py_TPFLAGS_HAVE_NEWBUFFER))
            && (Py_TYPE(obj)->tp_as_buffer->bf_getbuffer != NULL));
}

extern "C" {
int Py_Py3kWarningFlag;
}

BoxedClass* capifunc_cls;

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

Box* BoxedWrapperDescriptor::__call__(BoxedWrapperDescriptor* descr, PyObject* self, BoxedTuple* args, Box** _args) {
    RELEASE_ASSERT(descr->cls == wrapperdescr_cls, "");

    BoxedDict* kw = static_cast<BoxedDict*>(_args[0]);

    if (!isSubclass(self->cls, descr->type))
        raiseExcHelper(TypeError, "descriptor '' requires a '%s' object but received a '%s'",
                       getFullNameOfClass(descr->type).c_str(), getFullTypeName(self).c_str());

    auto wrapper = new BoxedWrapperObject(descr, self);
    return BoxedWrapperObject::__call__(wrapper, args, kw);
}

extern "C" void _PyErr_BadInternalCall(const char* filename, int lineno) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyVarObject* PyObject_InitVar(PyVarObject* op, PyTypeObject* tp, Py_ssize_t size) noexcept {
    assert(gc::isValidGCObject(op));
    assert(gc::isValidGCObject(tp));

    RELEASE_ASSERT(op, "");
    RELEASE_ASSERT(tp, "");
    Py_TYPE(op) = tp;
    op->ob_size = size;
    return op;
}

extern "C" PyObject* PyObject_Format(PyObject* obj, PyObject* format_spec) noexcept {
    PyObject* empty = NULL;
    PyObject* result = NULL;
#ifdef Py_USING_UNICODE
    int spec_is_unicode;
    int result_is_unicode;
#endif

    /* If no format_spec is provided, use an empty string */
    if (format_spec == NULL) {
        empty = PyString_FromStringAndSize(NULL, 0);
        format_spec = empty;
    }

/* Check the format_spec type, and make sure it's str or unicode */
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(format_spec))
        spec_is_unicode = 1;
    else if (PyString_Check(format_spec))
        spec_is_unicode = 0;
    else {
#else
    if (!PyString_Check(format_spec)) {
#endif
        PyErr_Format(PyExc_TypeError, "format expects arg 2 to be string "
                                      "or unicode, not %.100s",
                     Py_TYPE(format_spec)->tp_name);
        goto done;
    }

    /* Check for a __format__ method and call it. */
    if (PyInstance_Check(obj)) {
        /* We're an instance of a classic class */
        PyObject* bound_method = PyObject_GetAttrString(obj, "__format__");
        if (bound_method != NULL) {
            result = PyObject_CallFunctionObjArgs(bound_method, format_spec, NULL);
            Py_DECREF(bound_method);
        } else {
            PyObject* self_as_str = NULL;
            PyObject* format_method = NULL;
            Py_ssize_t format_len;

            PyErr_Clear();
/* Per the PEP, convert to str (or unicode,
   depending on the type of the format
   specifier).  For new-style classes, this
   logic is done by object.__format__(). */
#ifdef Py_USING_UNICODE
            if (spec_is_unicode) {
                format_len = PyUnicode_GET_SIZE(format_spec);
                self_as_str = PyObject_Unicode(obj);
            } else
#endif
            {
                format_len = PyString_GET_SIZE(format_spec);
                self_as_str = PyObject_Str(obj);
            }
            if (self_as_str == NULL)
                goto done1;

            if (format_len > 0) {
                /* See the almost identical code in
                   typeobject.c for new-style
                   classes. */
                if (PyErr_WarnEx(PyExc_PendingDeprecationWarning, "object.__format__ with a non-empty "
                                                                  "format string is deprecated",
                                 1) < 0) {
                    goto done1;
                }
                /* Eventually this will become an
                   error:
                PyErr_Format(PyExc_TypeError,
                   "non-empty format string passed to "
                   "object.__format__");
                goto done1;
                */
            }

            /* Then call str.__format__ on that result */
            format_method = PyObject_GetAttrString(self_as_str, "__format__");
            if (format_method == NULL) {
                goto done1;
            }
            result = PyObject_CallFunctionObjArgs(format_method, format_spec, NULL);
        done1:
            Py_XDECREF(self_as_str);
            Py_XDECREF(format_method);
            if (result == NULL)
                goto done;
        }
    } else {
        /* Not an instance of a classic class, use the code
           from py3k */
        static PyObject* format_cache = NULL;

        /* Find the (unbound!) __format__ method (a borrowed
           reference) */
        PyObject* method = _PyObject_LookupSpecial(obj, "__format__", &format_cache);
        if (method == NULL) {
            if (!PyErr_Occurred())
                PyErr_Format(PyExc_TypeError, "Type %.100s doesn't define __format__", Py_TYPE(obj)->tp_name);
            goto done;
        }
        /* And call it. */
        result = PyObject_CallFunctionObjArgs(method, format_spec, NULL);
        Py_DECREF(method);
    }

    if (result == NULL)
        goto done;

/* Check the result type, and make sure it's str or unicode */
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(result))
        result_is_unicode = 1;
    else if (PyString_Check(result))
        result_is_unicode = 0;
    else {
#else
    if (!PyString_Check(result)) {
#endif
        PyErr_Format(PyExc_TypeError, "%.100s.__format__ must return string or "
                                      "unicode, not %.100s",
                     Py_TYPE(obj)->tp_name, Py_TYPE(result)->tp_name);
        Py_DECREF(result);
        result = NULL;
        goto done;
    }

/* Convert to unicode, if needed.  Required if spec is unicode
   and result is str */
#ifdef Py_USING_UNICODE
    if (spec_is_unicode && !result_is_unicode) {
        PyObject* tmp = PyObject_Unicode(result);
        /* This logic works whether or not tmp is NULL */
        Py_DECREF(result);
        result = tmp;
    }
#endif

done:
    Py_XDECREF(empty);
    return result;
}


extern "C" PyObject* PyObject_GetAttr(PyObject* o, PyObject* attr_name) noexcept {
    if (!isSubclass(attr_name->cls, str_cls)) {
        PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr_name)->tp_name);
        return NULL;
    }

    try {
        return getattr(o, static_cast<BoxedString*>(attr_name)->data());
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyObject_GenericGetAttr(PyObject* o, PyObject* name) noexcept {
    try {
        Box* r = getattrInternalGeneric(o, static_cast<BoxedString*>(name)->s.data(), NULL, false, false, NULL, NULL);
        if (!r)
            PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name,
                         PyString_AS_STRING(name));
        return r;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* _PyObject_GenericGetAttrWithDict(PyObject* obj, PyObject* name, PyObject* dict) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return nullptr;
}

extern "C" int _PyObject_GenericSetAttrWithDict(PyObject* obj, PyObject* name, PyObject* value,
                                                PyObject* dict) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}


extern "C" PyObject* PyObject_GetItem(PyObject* o, PyObject* key) noexcept {
    try {
        return getitem(o, key);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyObject_SetItem(PyObject* o, PyObject* key, PyObject* v) noexcept {
    try {
        setitem(o, key, v);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" int PyObject_DelItem(PyObject* o, PyObject* key) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" PyObject* PyObject_RichCompare(PyObject* o1, PyObject* o2, int opid) noexcept {
    int translated_op;
    switch (opid) {
        case Py_LT:
            translated_op = AST_TYPE::Lt;
            break;
        case Py_LE:
            translated_op = AST_TYPE::LtE;
            break;
        case Py_EQ:
            translated_op = AST_TYPE::Eq;
            break;
        case Py_NE:
            translated_op = AST_TYPE::NotEq;
            break;
        case Py_GT:
            translated_op = AST_TYPE::Gt;
            break;
        case Py_GE:
            translated_op = AST_TYPE::GtE;
            break;
        default:
            fatalOrError(PyExc_NotImplementedError, "unimplemented");
            return nullptr;
    };

    try {
        return compare(o1, o2, translated_op);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" {
int _Py_SwappedOp[] = { Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE };
}

extern "C" long PyObject_Hash(PyObject* o) noexcept {
    try {
        return hash(o)->n;
    } catch (ExcInfo e) {
        fatalOrError(PyExc_NotImplementedError, "unimplemented");
        return -1;
    }
}

extern "C" long PyObject_HashNotImplemented(PyObject* self) noexcept {
    PyErr_Format(PyExc_TypeError, "unhashable type: '%.200s'", Py_TYPE(self)->tp_name);
    return -1;
}

extern "C" PyObject* _PyObject_NextNotImplemented(PyObject* self) noexcept {
    PyErr_Format(PyExc_TypeError, "'%.200s' object is not iterable", Py_TYPE(self)->tp_name);
    return NULL;
}

extern "C" long _Py_HashPointer(void* p) noexcept {
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

extern "C" int PyObject_IsTrue(PyObject* o) noexcept {
    try {
        return nonzero(o);
    } catch (ExcInfo e) {
        fatalOrError(PyExc_NotImplementedError, "unimplemented");
        return -1;
    }
}


extern "C" int PyObject_Not(PyObject* o) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" PyObject* PyObject_Call(PyObject* callable_object, PyObject* args, PyObject* kw) noexcept {
    try {
        if (kw)
            return runtimeCall(callable_object, ArgPassSpec(0, 0, true, true), args, kw, NULL, NULL, NULL);
        else
            return runtimeCall(callable_object, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyObject_GetBuffer(PyObject* obj, Py_buffer* view, int flags) noexcept {
    if (!PyObject_CheckBuffer(obj)) {
        PyErr_Format(PyExc_TypeError, "'%100s' does not have the buffer interface", Py_TYPE(obj)->tp_name);
        return -1;
    }
    return (*(obj->cls->tp_as_buffer->bf_getbuffer))(obj, view, flags);
}

/* Implementation of PyObject_Print with recursion checking */
static int internal_print(PyObject* op, FILE* fp, int flags, int nesting) noexcept {
    int ret = 0;
    if (nesting > 10) {
        PyErr_SetString(PyExc_RuntimeError, "print recursion");
        return -1;
    }
    if (PyErr_CheckSignals())
        return -1;
#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        PyErr_SetString(PyExc_MemoryError, "stack overflow");
        return -1;
    }
#endif
    clearerr(fp); /* Clear any previous error condition */
    if (op == NULL) {
        Py_BEGIN_ALLOW_THREADS fprintf(fp, "<nil>");
        Py_END_ALLOW_THREADS
    } else {
        if (Py_TYPE(op)->tp_print == NULL) {
            PyObject* s;
            if (flags & Py_PRINT_RAW)
                s = PyObject_Str(op);
            else
                s = PyObject_Repr(op);
            if (s == NULL)
                ret = -1;
            else {
                ret = internal_print(s, fp, Py_PRINT_RAW, nesting + 1);
            }
            Py_XDECREF(s);
        } else
            ret = (*Py_TYPE(op)->tp_print)(op, fp, flags);
    }
    if (ret == 0) {
        if (ferror(fp)) {
            PyErr_SetFromErrno(PyExc_IOError);
            clearerr(fp);
            ret = -1;
        }
    }
    return ret;
}

extern "C" int PyObject_Print(PyObject* obj, FILE* fp, int flags) noexcept {
    return internal_print(obj, fp, flags, 0);
};

extern "C" PyObject* PySequence_Repeat(PyObject* o, Py_ssize_t count) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return nullptr;
}

extern "C" PyObject* PySequence_InPlaceConcat(PyObject* o1, PyObject* o2) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return nullptr;
}

extern "C" PyObject* PySequence_InPlaceRepeat(PyObject* o, Py_ssize_t count) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return nullptr;
}

extern "C" PyObject* PySequence_GetItem(PyObject* o, Py_ssize_t i) noexcept {
    try {
        // Not sure if this is really the same:
        return getitem(o, boxInt(i));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PySequence_GetSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) noexcept {
    try {
        // Not sure if this is really the same:
        return getitem(o, createSlice(boxInt(i1), boxInt(i2), None));
    } catch (ExcInfo e) {
        fatalOrError(PyExc_NotImplementedError, "unimplemented");
        return nullptr;
    }
}

extern "C" int PySequence_SetItem(PyObject* o, Py_ssize_t i, PyObject* v) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" int PySequence_DelItem(PyObject* o, Py_ssize_t i) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" int PySequence_SetSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2, PyObject* v) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" int PySequence_DelSlice(PyObject* o, Py_ssize_t i1, Py_ssize_t i2) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" Py_ssize_t PySequence_Count(PyObject* o, PyObject* value) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" Py_ssize_t PySequence_Index(PyObject* o, PyObject* value) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" PyObject* PyIter_Next(PyObject* iter) noexcept {
    try {
        Box* hasnext = iter->hasnextOrNullIC();
        if (hasnext) {
            if (hasnext->nonzeroIC())
                return iter->nextIC();
            else
                return NULL;
        } else {
            return iter->nextIC();
        }
    } catch (ExcInfo e) {
        if (e.matches(StopIteration))
            return NULL;
        setCAPIException(e);
    }
    return NULL;
}

extern "C" int PyCallable_Check(PyObject* x) noexcept {
    if (x == NULL)
        return 0;

    static const std::string call_attr("__call__");
    return typeLookup(x->cls, call_attr, NULL) != NULL;
}

extern "C" int Py_FlushLine(void) noexcept {
    PyObject* f = PySys_GetObject("stdout");
    if (f == NULL)
        return 0;
    if (!PyFile_SoftSpace(f, 0))
        return 0;
    return PyFile_WriteString("\n", f);
}

extern "C" void PyErr_NormalizeException(PyObject** exc, PyObject** val, PyObject** tb) noexcept {
    PyObject* type = *exc;
    PyObject* value = *val;
    PyObject* inclass = NULL;
    PyObject* initial_tb = NULL;
    PyThreadState* tstate = NULL;

    if (type == NULL) {
        /* There was no exception, so nothing to do. */
        return;
    }

    /* If PyErr_SetNone() was used, the value will have been actually
       set to NULL.
    */
    if (!value) {
        value = Py_None;
        Py_INCREF(value);
    }

    if (PyExceptionInstance_Check(value))
        inclass = PyExceptionInstance_Class(value);

    /* Normalize the exception so that if the type is a class, the
       value will be an instance.
    */
    if (PyExceptionClass_Check(type)) {
        /* if the value was not an instance, or is not an instance
           whose class is (or is derived from) type, then use the
           value as an argument to instantiation of the type
           class.
        */
        if (!inclass || !PyObject_IsSubclass(inclass, type)) {
            PyObject* args, *res;

            if (value == Py_None)
                args = PyTuple_New(0);
            else if (PyTuple_Check(value)) {
                Py_INCREF(value);
                args = value;
            } else
                args = PyTuple_Pack(1, value);

            if (args == NULL)
                goto finally;
            res = PyEval_CallObject(type, args);
            Py_DECREF(args);
            if (res == NULL)
                goto finally;
            Py_DECREF(value);
            value = res;
        }
        /* if the class of the instance doesn't exactly match the
           class of the type, believe the instance
        */
        else if (inclass != type) {
            Py_DECREF(type);
            type = inclass;
            Py_INCREF(type);
        }
    }
    *exc = type;
    *val = value;
    return;
finally:
    Py_DECREF(type);
    Py_DECREF(value);
    /* If the new exception doesn't set a traceback and the old
       exception had a traceback, use the old traceback for the
       new exception.  It's better than nothing.
    */
    initial_tb = *tb;
    PyErr_Fetch(exc, val, tb);
    if (initial_tb != NULL) {
        if (*tb == NULL)
            *tb = initial_tb;
        else
            Py_DECREF(initial_tb);
    }
    /* normalize recursively */
    tstate = PyThreadState_GET();
    if (++tstate->recursion_depth > Py_GetRecursionLimit()) {
        --tstate->recursion_depth;
        /* throw away the old exception... */
        Py_DECREF(*exc);
        Py_DECREF(*val);
        /* ... and use the recursion error instead */
        *exc = PyExc_RuntimeError;
        *val = PyExc_RecursionErrorInst;
        Py_INCREF(*exc);
        Py_INCREF(*val);
        /* just keeping the old traceback */
        return;
    }
    PyErr_NormalizeException(exc, val, tb);
    --tstate->recursion_depth;
}

extern "C" PyGILState_STATE PyGILState_Ensure(void) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" void PyGILState_Release(PyGILState_STATE) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" PyThreadState* PyGILState_GetThisThreadState(void) noexcept {
    Py_FatalError("unimplemented");
}

void setCAPIException(const ExcInfo& e) {
    cur_thread_state.curexc_type = e.type;
    cur_thread_state.curexc_value = e.value;
    cur_thread_state.curexc_traceback = e.traceback;
}

void throwCAPIException() {
    checkAndThrowCAPIException();
    raiseExcHelper(SystemError, "error return without exception set");
}

void checkAndThrowCAPIException() {
    Box* _type = cur_thread_state.curexc_type;
    if (!_type)
        assert(!cur_thread_state.curexc_value);

    if (_type) {
        BoxedClass* type = static_cast<BoxedClass*>(_type);
        assert(isSubclass(_type->cls, type_cls) && isSubclass(static_cast<BoxedClass*>(type), BaseException)
               && "Only support throwing subclass of BaseException for now");

        Box* value = cur_thread_state.curexc_value;
        if (!value)
            value = None;

        Box* tb = cur_thread_state.curexc_traceback;
        if (!tb)
            tb = None;

        // Make sure to call PyErr_Clear() *before* normalizing the exception, since otherwise
        // the normalization can think that it had raised an exception, resulting to a call
        // to checkAndThrowCAPIException, and boom.
        PyErr_Clear();

        // This is similar to PyErr_NormalizeException:
        if (!isSubclass(value->cls, type)) {
            if (value->cls == tuple_cls) {
                value = runtimeCall(type, ArgPassSpec(0, 0, true, false), value, NULL, NULL, NULL, NULL);
            } else if (value == None) {
                value = runtimeCall(type, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
            } else {
                value = runtimeCall(type, ArgPassSpec(1), value, NULL, NULL, NULL, NULL);
            }
        }

        RELEASE_ASSERT(value->cls == type, "unsupported");

        if (tb != None)
            raiseRaw(ExcInfo(value->cls, value, tb));
        raiseExc(value);
    }
}

extern "C" void PyErr_Restore(PyObject* type, PyObject* value, PyObject* traceback) noexcept {
    cur_thread_state.curexc_type = type;
    cur_thread_state.curexc_value = value;
    cur_thread_state.curexc_traceback = traceback;
}

extern "C" void PyErr_Clear() noexcept {
    PyErr_Restore(NULL, NULL, NULL);
}

extern "C" void PyErr_SetString(PyObject* exception, const char* string) noexcept {
    PyErr_SetObject(exception, boxStrConstant(string));
}

extern "C" void PyErr_SetObject(PyObject* exception, PyObject* value) noexcept {
    PyErr_Restore(exception, value, NULL);
}

extern "C" PyObject* PyErr_Format(PyObject* exception, const char* format, ...) noexcept {
    va_list vargs;
    PyObject* string;

#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, format);
#else
    va_start(vargs);
#endif

    string = PyString_FromFormatV(format, vargs);
    PyErr_SetObject(exception, string);
    Py_XDECREF(string);
    va_end(vargs);
    return NULL;
}

extern "C" int PyErr_BadArgument() noexcept {
    // TODO this is untested
    PyErr_SetString(PyExc_TypeError, "bad argument type for built-in operation");
    return 0;
}

extern "C" PyObject* PyErr_NoMemory() noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return nullptr;
}

extern "C" int PyExceptionClass_Check(PyObject* o) noexcept {
    return PyClass_Check(o) || (PyType_Check(o) && isSubclass(static_cast<BoxedClass*>(o), BaseException));
}

extern "C" int PyExceptionInstance_Check(PyObject* o) noexcept {
    return PyInstance_Check(o) || isSubclass(o->cls, BaseException);
}

extern "C" const char* PyExceptionClass_Name(PyObject* o) noexcept {
    return PyClass_Check(o) ? PyString_AS_STRING(static_cast<BoxedClassobj*>(o)->name)
                            : static_cast<BoxedClass*>(o)->tp_name;
}

extern "C" PyObject* PyExceptionInstance_Class(PyObject* o) noexcept {
    return PyInstance_Check(o) ? (Box*)static_cast<BoxedInstance*>(o)->inst_cls : o->cls;
}

extern "C" int PyTraceBack_Print(PyObject* v, PyObject* f) noexcept {
    RELEASE_ASSERT(f->cls == file_cls && static_cast<BoxedFile*>(f)->f_fp == stderr,
                   "sorry will only print tracebacks to stderr right now");
    printTraceback(v);
    return 0;
}

#define Py_DEFAULT_RECURSION_LIMIT 1000
static int recursion_limit = Py_DEFAULT_RECURSION_LIMIT;
extern "C" {
int _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;
}

/* the macro Py_EnterRecursiveCall() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches _Py_CheckRecursionLimit.
   If USE_STACKCHECK, the macro decrements _Py_CheckRecursionLimit
   to guarantee that _Py_CheckRecursiveCall() is regularly called.
   Without USE_STACKCHECK, there is no need for this. */
extern "C" int _Py_CheckRecursiveCall(const char* where) noexcept {
    PyThreadState* tstate = PyThreadState_GET();

#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        --tstate->recursion_depth;
        PyErr_SetString(PyExc_MemoryError, "Stack overflow");
        return -1;
    }
#endif
    if (tstate->recursion_depth > recursion_limit) {
        --tstate->recursion_depth;
        PyErr_Format(PyExc_RuntimeError, "maximum recursion depth exceeded%s", where);
        return -1;
    }
    _Py_CheckRecursionLimit = recursion_limit;
    return 0;
}

extern "C" int Py_GetRecursionLimit(void) noexcept {
    return recursion_limit;
}

extern "C" void Py_SetRecursionLimit(int new_limit) noexcept {
    recursion_limit = new_limit;
    _Py_CheckRecursionLimit = recursion_limit;
}

extern "C" int PyErr_GivenExceptionMatches(PyObject* err, PyObject* exc) noexcept {
    if (err == NULL || exc == NULL) {
        /* maybe caused by "import exceptions" that failed early on */
        return 0;
    }
    if (PyTuple_Check(exc)) {
        Py_ssize_t i, n;
        n = PyTuple_Size(exc);
        for (i = 0; i < n; i++) {
            /* Test recursively */
            if (PyErr_GivenExceptionMatches(err, PyTuple_GET_ITEM(exc, i))) {
                return 1;
            }
        }
        return 0;
    }
    /* err might be an instance, so check its class. */
    if (PyExceptionInstance_Check(err))
        err = PyExceptionInstance_Class(err);

    if (PyExceptionClass_Check(err) && PyExceptionClass_Check(exc)) {
        int res = 0, reclimit;
        PyObject* exception, *value, *tb;
        PyErr_Fetch(&exception, &value, &tb);
        /* Temporarily bump the recursion limit, so that in the most
           common case PyObject_IsSubclass will not raise a recursion
           error we have to ignore anyway.  Don't do it when the limit
           is already insanely high, to avoid overflow */
        reclimit = Py_GetRecursionLimit();
        if (reclimit < (1 << 30))
            Py_SetRecursionLimit(reclimit + 5);
        res = PyObject_IsSubclass(err, exc);
        Py_SetRecursionLimit(reclimit);
        /* This function must not fail, so print the error here */
        if (res == -1) {
            PyErr_WriteUnraisable(err);
            res = 0;
        }
        PyErr_Restore(exception, value, tb);
        return res;
    }

    return err == exc;
}

extern "C" int PyErr_ExceptionMatches(PyObject* exc) noexcept {
    return PyErr_GivenExceptionMatches(PyErr_Occurred(), exc);
}

extern "C" PyObject* PyErr_Occurred() noexcept {
    return cur_thread_state.curexc_type;
}

extern "C" int PyErr_WarnEx(PyObject* category, const char* text, Py_ssize_t stacklevel) noexcept {
    // These warnings are silenced by default:
    // We should copy the real CPython code in here
    if (category == PyExc_DeprecationWarning)
        return 0;

    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" PyObject* PyImport_Import(PyObject* module_name) noexcept {
    RELEASE_ASSERT(module_name, "");
    RELEASE_ASSERT(module_name->cls == str_cls, "");

    try {
        std::string _module_name = static_cast<BoxedString*>(module_name)->s;
        return importModuleLevel(_module_name, None, None, -1);
    } catch (ExcInfo e) {
        fatalOrError(PyExc_NotImplementedError, "unimplemented");
        return nullptr;
    }
}


extern "C" void* PyObject_Malloc(size_t sz) noexcept {
    return gc_compat_malloc(sz);
}

extern "C" void* PyObject_Realloc(void* ptr, size_t sz) noexcept {
    return gc_compat_realloc(ptr, sz);
}

extern "C" void PyObject_Free(void* ptr) noexcept {
    gc_compat_free(ptr);
}

extern "C" void* PyMem_Malloc(size_t sz) noexcept {
    return gc_compat_malloc(sz);
}

extern "C" void* PyMem_Realloc(void* ptr, size_t sz) noexcept {
    return gc_compat_realloc(ptr, sz);
}

extern "C" void PyMem_Free(void* ptr) noexcept {
    gc_compat_free(ptr);
}

extern "C" int PyOS_snprintf(char* str, size_t size, const char* format, ...) noexcept {
    int rc;
    va_list va;

    va_start(va, format);
    rc = PyOS_vsnprintf(str, size, format, va);
    va_end(va);
    return rc;
}

extern "C" int PyOS_vsnprintf(char* str, size_t size, const char* format, va_list va) noexcept {
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

static const char* progname = "pyston";
extern "C" void Py_SetProgramName(char* pn) noexcept {
    if (pn && *pn)
        progname = pn;
}

extern "C" const char* Py_GetProgramName(void) noexcept {
    return progname;
}

static char* default_home = NULL;
extern "C" void Py_SetPythonHome(char* home) noexcept {
    default_home = home;
}

extern "C" char* Py_GetPythonHome(void) noexcept {
    char* home = default_home;
    if (home == NULL && !Py_IgnoreEnvironmentFlag)
        home = Py_GETENV("PYTHONHOME");
    return home;
}

extern "C" PyObject* PyThreadState_GetDict(void) noexcept {
    Box* dict = cur_thread_state.dict;
    if (!dict) {
        dict = cur_thread_state.dict = new BoxedDict();
    }
    return dict;
}

extern "C" int _PyOS_URandom(void* buffer, Py_ssize_t size) noexcept {
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

extern "C" PyOS_sighandler_t PyOS_getsig(int sig) noexcept {
#ifdef HAVE_SIGACTION
    struct sigaction context;
    if (sigaction(sig, NULL, &context) == -1)
        return SIG_ERR;
    return context.sa_handler;
#else
    PyOS_sighandler_t handler;
/* Special signal handling for the secure CRT in Visual Studio 2005 */
#if defined(_MSC_VER) && _MSC_VER >= 1400
    switch (sig) {
        /* Only these signals are valid */
        case SIGINT:
        case SIGILL:
        case SIGFPE:
        case SIGSEGV:
        case SIGTERM:
        case SIGBREAK:
        case SIGABRT:
            break;
        /* Don't call signal() with other values or it will assert */
        default:
            return SIG_ERR;
    }
#endif /* _MSC_VER && _MSC_VER >= 1400 */
    handler = signal(sig, SIG_IGN);
    if (handler != SIG_ERR)
        signal(sig, handler);
    return handler;
#endif
}

extern "C" PyOS_sighandler_t PyOS_setsig(int sig, PyOS_sighandler_t handler) noexcept {
    if (sig == SIGUSR2) {
        Py_FatalError("SIGUSR2 is reserved for Pyston internal use");
    }

#ifdef HAVE_SIGACTION
    /* Some code in Modules/signalmodule.c depends on sigaction() being
     * used here if HAVE_SIGACTION is defined.  Fix that if this code
     * changes to invalidate that assumption.
     */
    struct sigaction context, ocontext;
    context.sa_handler = handler;
    sigemptyset(&context.sa_mask);
    context.sa_flags = 0;
    if (sigaction(sig, &context, &ocontext) == -1)
        return SIG_ERR;
    return ocontext.sa_handler;
#else
    PyOS_sighandler_t oldhandler;
    oldhandler = signal(sig, handler);
#ifdef HAVE_SIGINTERRUPT
    siginterrupt(sig, 1);
#endif
    return oldhandler;
#endif
}

extern "C" int Py_AddPendingCall(int (*func)(void*), void* arg) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return -1;
}

extern "C" PyObject* _PyImport_FixupExtension(char* name, char* filename) noexcept {
    // Don't have to do anything here, since we will error in _PyImport_FindExtension
    // TODO is this ok?
    return NULL;
}

extern "C" PyObject* _PyImport_FindExtension(char* name, char* filename) noexcept {
    fatalOrError(PyExc_NotImplementedError, "unimplemented");
    return nullptr;
}

static PyObject* listmethodchain(PyMethodChain* chain) noexcept {
    PyMethodChain* c;
    PyMethodDef* ml;
    int i, n;
    PyObject* v;

    n = 0;
    for (c = chain; c != NULL; c = c->link) {
        for (ml = c->methods; ml->ml_name != NULL; ml++)
            n++;
    }
    v = PyList_New(n);
    if (v == NULL)
        return NULL;
    i = 0;
    for (c = chain; c != NULL; c = c->link) {
        for (ml = c->methods; ml->ml_name != NULL; ml++) {
            PyList_SetItem(v, i, PyString_FromString(ml->ml_name));
            i++;
        }
    }
    if (PyErr_Occurred()) {
        Py_DECREF(v);
        return NULL;
    }
    PyList_Sort(v);
    return v;
}

extern "C" PyObject* Py_FindMethodInChain(PyMethodChain* chain, PyObject* self, const char* name) noexcept {
    if (name[0] == '_' && name[1] == '_') {
        if (strcmp(name, "__methods__") == 0) {
            if (PyErr_WarnPy3k("__methods__ not supported in 3.x", 1) < 0)
                return NULL;
            return listmethodchain(chain);
        }
        if (strcmp(name, "__doc__") == 0) {
            const char* doc = self->cls->tp_doc;
            if (doc != NULL)
                return PyString_FromString(doc);
        }
    }
    while (chain != NULL) {
        PyMethodDef* ml = chain->methods;
        for (; ml->ml_name != NULL; ml++) {
            if (name[0] == ml->ml_name[0] && strcmp(name + 1, ml->ml_name + 1) == 0)
                /* XXX */
                return PyCFunction_New(ml, self);
        }
        chain = chain->link;
    }
    PyErr_SetString(PyExc_AttributeError, name);
    return NULL;
}

/* Find a method in a single method list */

extern "C" PyObject* Py_FindMethod(PyMethodDef* methods, PyObject* self, const char* name) noexcept {
    PyMethodChain chain;
    chain.methods = methods;
    chain.link = NULL;
    return Py_FindMethodInChain(&chain, self, name);
}

extern "C" PyObject* PyCFunction_NewEx(PyMethodDef* ml, PyObject* self, PyObject* module) noexcept {
    RELEASE_ASSERT(module == NULL, "not implemented");
    assert((ml->ml_flags & (~(METH_VARARGS | METH_KEYWORDS | METH_NOARGS | METH_O))) == 0);

    return new BoxedCApiFunction(ml->ml_flags, self, ml->ml_name, ml->ml_meth);
}

extern "C" PyCFunction PyCFunction_GetFunction(PyObject* op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return ((PyCFunctionObject*)op)->m_ml->ml_meth;
}

extern "C" int _PyEval_SliceIndex(PyObject* v, Py_ssize_t* pi) noexcept {
    if (v != NULL) {
        Py_ssize_t x;
        if (PyInt_Check(v)) {
            /* XXX(nnorwitz): I think PyInt_AS_LONG is correct,
               however, it looks like it should be AsSsize_t.
               There should be a comment here explaining why.
            */
            x = PyInt_AS_LONG(v);
        } else if (PyIndex_Check(v)) {
            x = PyNumber_AsSsize_t(v, NULL);
            if (x == -1 && PyErr_Occurred())
                return 0;
        } else {
            PyErr_SetString(PyExc_TypeError, "slice indices must be integers or "
                                             "None or have an __index__ method");
            return 0;
        }
        *pi = x;
    }
    return 1;
}

extern "C" int PyEval_GetRestricted(void) noexcept {
    return 0; // We don't support restricted mode
}

extern "C" void PyEval_InitThreads(void) noexcept {
    // nothing to do here
}

extern "C" char* PyModule_GetName(PyObject* m) noexcept {
    assert(m->cls == module_cls);
    return &static_cast<BoxedModule*>(m)->fn[0];
}

BoxedModule* importCExtension(const std::string& full_name, const std::string& last_name, const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        // raiseExcHelper(ImportError, "%s", dlerror());
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    assert(handle);

    std::string initname = "init" + last_name;
    void (*init)() = (void (*)())dlsym(handle, initname.c_str());

    char* error;
    if ((error = dlerror()) != NULL) {
        // raiseExcHelper(ImportError, "%s", error);
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    assert(init);

    char* packagecontext = strdup(full_name.c_str());
    char* oldcontext = _Py_PackageContext;
    _Py_PackageContext = packagecontext;
    (*init)();
    _Py_PackageContext = oldcontext;
    free(packagecontext);

    checkAndThrowCAPIException();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStrConstant(full_name.c_str());
    Box* _m = sys_modules->d[s];
    RELEASE_ASSERT(_m, "dynamic module not initialized properly");
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);
    m->setattr("__file__", boxString(path), NULL);
    m->fn = path;
    return m;
}

Box* BoxedCApiFunction::callInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec,
                                     Box* arg1, Box* arg2, Box* arg3, Box** args,
                                     const std::vector<const std::string*>* keyword_names) {
    if (argspec != ArgPassSpec(2))
        return callFunc(func, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    assert(arg1->cls == capifunc_cls);
    BoxedCApiFunction* capifunc = static_cast<BoxedCApiFunction*>(arg1);
    if (capifunc->ml_flags != METH_O)
        return callFunc(func, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    if (rewrite_args) {
        rewrite_args->arg1->addGuard((intptr_t)arg1);
        RewriterVar* r_passthrough = rewrite_args->arg1->getAttr(offsetof(BoxedCApiFunction, passthrough));
        rewrite_args->out_rtn
            = rewrite_args->rewriter->call(true, (void*)capifunc->func, r_passthrough, rewrite_args->arg2);
        rewrite_args->rewriter->call(true, (void*)checkAndThrowCAPIException);
        rewrite_args->out_success = true;
    }
    Box* r = capifunc->func(capifunc->passthrough, arg2);
    checkAndThrowCAPIException();
    assert(r);
    return r;
}

static Box* methodGetDoc(Box* b, void*) {
    assert(b->cls == method_cls);
    const char* s = static_cast<BoxedMethodDescriptor*>(b)->method->ml_doc;
    if (s)
        return boxStrConstant(s);
    return None;
}

void setupCAPI() {
    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));

    auto capi_call = new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, true));
    capi_call->f->internal_callable = BoxedCApiFunction::callInternal;
    capifunc_cls->giveAttr("__call__", capi_call);

    capifunc_cls->freeze();

    method_cls->giveAttr("__get__",
                         new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__get__, UNKNOWN, 3)));
    method_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__call__, UNKNOWN, 2,
                                                                     0, true, true)));
    method_cls->giveAttr("__doc__", new (pyston_getset_cls) BoxedGetsetDescriptor(methodGetDoc, NULL, NULL));
    method_cls->freeze();

    wrapperdescr_cls->giveAttr("__get__",
                               new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__get__, UNKNOWN, 3)));
    wrapperdescr_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperDescriptor::__call__,
                                                                           UNKNOWN, 2, 0, true, true)));
    wrapperdescr_cls->freeze();

    wrapperobject_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedWrapperObject::__call__, UNKNOWN, 1, 0, true, true)));
    wrapperobject_cls->freeze();
}

void teardownCAPI() {
}
}
