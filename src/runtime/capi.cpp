// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include <fcntl.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"
#include "pythread.h"

#include "codegen/cpython_ast.h"
#include "grammar.h"
#include "node.h"
#include "token.h"
#include "parsetok.h"
#include "errcode.h"
#include "ast.h"
#undef BYTE
#undef STRING

#include "llvm/Support/ErrorHandling.h" // For llvm_unreachable
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/irgen/hooks.h"
#include "codegen/unwinding.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"

namespace pyston {

extern "C" int _PyIndex_Check(PyObject* obj) noexcept {
    return (Py_TYPE(obj)->tp_as_number != NULL && PyType_HasFeature(Py_TYPE(obj), Py_TPFLAGS_HAVE_INDEX)
            && Py_TYPE(obj)->tp_as_number->nb_index != NULL);
}

extern "C" int _PyObject_CheckBuffer(PyObject* obj) noexcept {
    return ((Py_TYPE(obj)->tp_as_buffer != NULL) && (PyType_HasFeature(Py_TYPE(obj), Py_TPFLAGS_HAVE_NEWBUFFER))
            && (Py_TYPE(obj)->tp_as_buffer->bf_getbuffer != NULL));
}

extern "C" {
int Py_Py3kWarningFlag;

BoxedClass* capifunc_cls;
}

extern "C" PyObject* PyObject_GetAttr(PyObject* o, PyObject* attr) noexcept {
    if (!PyString_Check(attr)) {
        if (PyUnicode_Check(attr)) {
            attr = _PyUnicode_AsDefaultEncodedString(attr, NULL);
            if (attr == NULL)
                return NULL;
        } else {
            PyErr_Format(TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr)->tp_name);
            return NULL;
        }
    }

    BoxedString* s = static_cast<BoxedString*>(attr);
    Py_INCREF(s);
    internStringMortalInplace(s);
    AUTO_DECREF(s);

    Box* r = getattrInternal<ExceptionStyle::CAPI>(o, s);

    if (!r && !PyErr_Occurred()) {
        PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name,
                     PyString_AS_STRING(attr));
    }

    return r;
}

extern "C" PyObject* PyObject_GenericGetAttr(PyObject* o, PyObject* name) noexcept {
    try {
        BoxedString* s = static_cast<BoxedString*>(name);
        Py_INCREF(s);
        internStringMortalInplace(s);
        AUTO_DECREF(s);
        Box* r = getattrInternalGeneric<false, NOT_REWRITABLE>(o, s, NULL, false, false, NULL, NULL);
        if (!r)
            PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", o->cls->tp_name,
                         PyString_AS_STRING(name));
        return r;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

// Note (kmod): I don't feel great about including an alternate code-path for lookups.  I also, however, don't feel
// great about modifying our code paths to take a custom dict, and since this code is just copied from CPython
// I feel like the risk is pretty low.
extern "C" PyObject* _PyObject_GenericGetAttrWithDict(PyObject* obj, PyObject* name, PyObject* dict) noexcept {
    PyTypeObject* tp = Py_TYPE(obj);
    PyObject* descr = NULL;
    PyObject* res = NULL;
    descrgetfunc f;
    Py_ssize_t dictoffset;
    PyObject** dictptr;

    if (!PyString_Check(name)) {
#ifdef Py_USING_UNICODE
        /* The Unicode to string conversion is done here because the
           existing tp_setattro slots expect a string object as name
           and we wouldn't want to break those. */
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return NULL;
        } else
#endif
        {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return NULL;
        }
    } else
        Py_INCREF(name);

    if (tp->tp_dict == NULL) {
        if (PyType_Ready(tp) < 0)
            goto done;
    }

#if 0 /* XXX this is not quite _PyType_Lookup anymore */
    /* Inline _PyType_Lookup */
    {
        Py_ssize_t i, n;
        PyObject *mro, *base, *dict;

        /* Look in tp_dict of types in MRO */
        mro = tp->tp_mro;
        assert(mro != NULL);
        assert(PyTuple_Check(mro));
        n = PyTuple_GET_SIZE(mro);
        for (i = 0; i < n; i++) {
            base = PyTuple_GET_ITEM(mro, i);
            if (PyClass_Check(base))
                dict = ((PyClassObject *)base)->cl_dict;
            else {
                assert(PyType_Check(base));
                dict = ((PyTypeObject *)base)->tp_dict;
            }
            assert(dict && PyDict_Check(dict));
            descr = PyDict_GetItem(dict, name);
            if (descr != NULL)
                break;
        }
    }
#else
    descr = _PyType_Lookup(tp, name);
#endif

    Py_XINCREF(descr);

    f = NULL;
    if (descr != NULL && PyType_HasFeature(descr->cls, Py_TPFLAGS_HAVE_CLASS)) {
        f = descr->cls->tp_descr_get;
        if (f != NULL && PyDescr_IsData(descr)) {
            res = f(descr, obj, (PyObject*)obj->cls);
            Py_DECREF(descr);
            goto done;
        }
    }

    if (dict == NULL) {
        /* Inline _PyObject_GetDictPtr */
        dictoffset = tp->tp_dictoffset;
        if (dictoffset != 0) {
            if (dictoffset < 0) {
                Py_ssize_t tsize;
                size_t size;

                tsize = ((PyVarObject*)obj)->ob_size;
                if (tsize < 0)
                    tsize = -tsize;
                size = _PyObject_VAR_SIZE(tp, tsize);

                dictoffset += (long)size;
                assert(dictoffset > 0);
                assert(dictoffset % SIZEOF_VOID_P == 0);
            }
            dictptr = (PyObject**)((char*)obj + dictoffset);
            dict = *dictptr;
        }
    }
    if (dict != NULL) {
        Py_INCREF(dict);
        res = PyDict_GetItem(dict, name);
        if (res != NULL) {
            Py_INCREF(res);
            Py_XDECREF(descr);
            Py_DECREF(dict);
            goto done;
        }
        Py_DECREF(dict);
    }

    if (f != NULL) {
        res = f(descr, obj, (PyObject*)Py_TYPE(obj));
        Py_DECREF(descr);
        goto done;
    }

    if (descr != NULL) {
        res = descr;
        /* descr was already increfed above */
        goto done;
    }

    PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%.400s'", tp->tp_name,
                 PyString_AS_STRING(name));
done:
    Py_DECREF(name);
    return res;
}

// (see note for _PyObject_GenericGetAttrWithDict)
extern "C" int _PyObject_GenericSetAttrWithDict(PyObject* obj, PyObject* name, PyObject* value,
                                                PyObject* dict) noexcept {
    PyTypeObject* tp = Py_TYPE(obj);
    PyObject* descr;
    descrsetfunc f;
    PyObject** dictptr;
    int res = -1;

    if (!PyString_Check(name)) {
#ifdef Py_USING_UNICODE
        /* The Unicode to string conversion is done here because the
           existing tp_setattro slots expect a string object as name
           and we wouldn't want to break those. */
        if (PyUnicode_Check(name)) {
            name = PyUnicode_AsEncodedString(name, NULL, NULL);
            if (name == NULL)
                return -1;
        } else
#endif
        {
            PyErr_Format(PyExc_TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(name)->tp_name);
            return -1;
        }
    } else
        Py_INCREF(name);

    if (tp->tp_dict == NULL) {
        if (PyType_Ready(tp) < 0)
            goto done;
    }

    descr = _PyType_Lookup(tp, name);
    f = NULL;
    if (descr != NULL && PyType_HasFeature(descr->cls, Py_TPFLAGS_HAVE_CLASS)) {
        f = descr->cls->tp_descr_set;
        if (f != NULL && PyDescr_IsData(descr)) {
            res = f(descr, obj, value);
            goto done;
        }
    }

    if (dict == NULL) {
        dictptr = _PyObject_GetDictPtr(obj);
        if (dictptr != NULL) {
            dict = *dictptr;
            if (dict == NULL && value != NULL) {
                dict = PyDict_New();
                if (dict == NULL)
                    goto done;
                *dictptr = dict;
            }
        }
    }
    if (dict != NULL) {
        Py_INCREF(dict);
        if (value == NULL)
            res = PyDict_DelItem(dict, name);
        else
            res = PyDict_SetItem(dict, name, value);
        if (res < 0 && PyErr_ExceptionMatches(PyExc_KeyError))
            PyErr_SetObject(PyExc_AttributeError, name);
        Py_DECREF(dict);
        goto done;
    }

    if (f != NULL) {
        res = f(descr, obj, value);
        goto done;
    }

    if (descr == NULL) {
        PyErr_Format(PyExc_AttributeError, "'%.100s' object has no attribute '%.200s'", tp->tp_name,
                     PyString_AS_STRING(name));
        goto done;
    }

    PyErr_Format(PyExc_AttributeError, "'%.50s' object attribute '%.400s' is read-only", tp->tp_name,
                 PyString_AS_STRING(name));
done:
    Py_DECREF(name);
    return res;
}


extern "C" PyObject* PyObject_GetItem(PyObject* o, PyObject* key) noexcept {
    return getitemInternal<ExceptionStyle::CAPI>(o, key);
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
    try {
        delitem(o, key);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
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

extern "C" long _Py_HashDouble(double v) noexcept {
    double intpart, fractpart;
    int expo;
    long hipart;
    long x; /* the final hash value */
            /* This is designed so that Python numbers of different types
             * that compare equal hash to the same value; otherwise comparisons
             * of mapping keys will turn out weird.
             */

    // pyston change: was if (!Py_IS_FINITE(v)) {
    if (!std::isfinite(v)) {
        // pyston change: was if (Py_IS_INFINITY(v)) {
        if (std::isinf(v))
            return v < 0 ? -271828 : 314159;
        else
            return 0;
    }
    fractpart = modf(v, &intpart);
    if (fractpart == 0.0) {
        /* This must return the same hash as an equal int or long. */
        if (intpart > LONG_MAX / 2 || -intpart > LONG_MAX / 2) {
            /* Convert to long and use its hash. */
            PyObject* plong; /* converted to Python long */
            plong = PyLong_FromDouble(v);
            if (plong == NULL)
                return -1;
            x = PyObject_Hash(plong);
            Py_DECREF(plong);
            return x;
        }
        /* Fits in a C long == a Python int, so is its own hash. */
        x = (long)intpart;
        if (x == -1)
            x = -2;
        return x;
    }
    /* The fractional part is non-zero, so we don't have to worry about
     * making this match the hash of some other type.
     * Use frexp to get at the bits in the double.
     * Since the VAX D double format has 56 mantissa bits, which is the
     * most of any double format in use, each of these parts may have as
     * many as (but no more than) 56 significant bits.
     * So, assuming sizeof(long) >= 4, each part can be broken into two
     * longs; frexp and multiplication are used to do that.
     * Also, since the Cray double format has 15 exponent bits, which is
     * the most of any double format in use, shifting the exponent field
     * left by 15 won't overflow a long (again assuming sizeof(long) >= 4).
     */
    v = frexp(v, &expo);
    v *= 2147483648.0;                       /* 2**31 */
    hipart = (long)v;                        /* take the top 32 bits */
    v = (v - (double)hipart) * 2147483648.0; /* get the next 32 bits */
    x = hipart + (long)v + (expo << 15);
    if (x == -1)
        x = -2;
    return x;
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
    if (o->cls == bool_cls)
        return o == Py_True;

    try {
        return o->nonzeroIC();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}


extern "C" int PyObject_Not(PyObject* o) noexcept {
    int res;
    res = PyObject_IsTrue(o);
    if (res < 0)
        return res;
    return res == 0;
}

extern "C" PyObject* PyObject_Call(PyObject* callable_object, PyObject* args, PyObject* kw) noexcept {
    if (kw)
        return runtimeCallInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(
            callable_object, NULL, ArgPassSpec(0, 0, true, true), args, kw, NULL, NULL, NULL);
    else
        return runtimeCallInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(
            callable_object, NULL, ArgPassSpec(0, 0, true, false), args, NULL, NULL, NULL, NULL);
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

extern "C" int PyCallable_Check(PyObject* x) noexcept {
    if (x == NULL)
        return 0;
    if (PyInstance_Check(x)) {
        PyObject* call = PyObject_GetAttrString(x, "__call__");
        if (call == NULL) {
            PyErr_Clear();
            return 0;
        }
        /* Could test recursively but don't, for fear of endless
           recursion if some joker sets self.__call__ = self */
        Py_DECREF(call);
        return 1;
    } else {
        return x->cls->tp_call != NULL;
    }
}

extern "C" int Py_FlushLine(void) noexcept {
    PyObject* f = PySys_GetObject("stdout");
    if (f == NULL)
        return 0;
    if (!PyFile_SoftSpace(f, 0))
        return 0;
    return PyFile_WriteString("\n", f);
}

void setCAPIException(STOLEN(const ExcInfo&) e) {
    PyErr_Restore(e.type, e.value, e.traceback);
}

void ensureCAPIExceptionSet() {
    if (!cur_thread_state.curexc_type)
        PyErr_SetString(SystemError, "error return without exception set");
}

void throwCAPIException() {
    checkAndThrowCAPIException();
    raiseExcHelper(SystemError, "error return without exception set");
}

void checkAndThrowCAPIException() {
    // Log these since these are expensive and usually avoidable:
    static StatCounter num_checkAndThrowCAPIException("num_checkAndThrowCAPIException");
    num_checkAndThrowCAPIException.log();

    Box* _type, *value, *tb;
    PyErr_Fetch(&_type, &value, &tb);

    if (!_type)
        assert(!value);

    if (_type) {
        BoxedClass* type = static_cast<BoxedClass*>(_type);
        assert(PyType_Check(_type) && isSubclass(static_cast<BoxedClass*>(type), BaseException)
               && "Only support throwing subclass of BaseException for now");

        if (!value)
            value = incref(Py_None);

        // This is similar to PyErr_NormalizeException:
        if (!isSubclass(value->cls, type)) {
            if (value->cls == tuple_cls) {
                value = runtimeCall(type, ArgPassSpec(0, 0, true, false), autoDecref(value), NULL, NULL, NULL, NULL);
            } else if (value == Py_None) {
                value = runtimeCall(type, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
                Py_DECREF(Py_None);
            } else {
                value = runtimeCall(type, ArgPassSpec(1), autoDecref(value), NULL, NULL, NULL, NULL);
            }
        }

        RELEASE_ASSERT(value->cls == type, "unsupported");

        if (tb)
            throw ExcInfo(value->cls, value, tb);
        Py_DECREF(type);
        Py_XDECREF(tb);
        raiseExc(value);
    }
}

extern "C" void Py_Exit(int sts) noexcept {
    // Py_Finalize();

    Stats::dump(false);
    exit(sts);
}

extern "C" void PyErr_GetExcInfo(PyObject** ptype, PyObject** pvalue, PyObject** ptraceback) noexcept {
    ExcInfo* exc = getFrameExcInfo();
    *ptype = xincref(exc->type);
    *pvalue = xincref(exc->value);
    *ptraceback = xincref(exc->traceback);
}

extern "C" void PyErr_SetExcInfo(PyObject* type, PyObject* value, PyObject* traceback) noexcept {
    ExcInfo* exc = getFrameExcInfo();
    AUTO_XDECREF(exc->type);
    AUTO_XDECREF(exc->value);
    AUTO_XDECREF(exc->traceback);

    exc->type = type ? type : incref(Py_None);
    exc->value = value ? value : incref(Py_None);
    exc->traceback = traceback;
}

extern "C" const char* PyExceptionClass_Name(PyObject* o) noexcept {
    return PyClass_Check(o) ? PyString_AS_STRING(static_cast<BoxedClassobj*>(o)->name)
                            : static_cast<BoxedClass*>(o)->tp_name;
}

extern "C" PyObject* PyExceptionInstance_Class(PyObject* o) noexcept {
    return PyInstance_Check(o) ? (Box*)static_cast<BoxedInstance*>(o)->inst_cls : o->cls;
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

extern "C" void* PyMem_Malloc(size_t nbytes) noexcept {
    return PyMem_MALLOC(nbytes);
}

extern "C" void* PyMem_Realloc(void* p, size_t nbytes) noexcept {
    return PyMem_REALLOC(p, nbytes);
}

extern "C" void PyMem_Free(void* p) noexcept {
    PyMem_FREE(p);
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

extern "C" int Py_FdIsInteractive(FILE* fp, const char* filename) noexcept {
    if (isatty((int)fileno(fp)))
        return 1;
    if (!Py_InteractiveFlag)
        return 0;
    return (filename == NULL) || (strcmp(filename, "<stdin>") == 0) || (strcmp(filename, "???") == 0);
}

extern "C" int PyRun_InteractiveOneFlags(FILE* fp, const char* filename, PyCompilerFlags* flags) noexcept {
    PyObject* m, *d, *v, *w;
    mod_ty mod;
    PyArena* arena;

    char _buf[1] = "";
    char* ps1 = _buf, * ps2 = _buf;
    int errcode = 0;

    v = PySys_GetObject("ps1");
    if (v != NULL) {
        v = PyObject_Str(v);
        if (v == NULL)
            PyErr_Clear();
        else if (PyString_Check(v))
            ps1 = PyString_AsString(v);
    }
    w = PySys_GetObject("ps2");
    if (w != NULL) {
        w = PyObject_Str(w);
        if (w == NULL)
            PyErr_Clear();
        else if (PyString_Check(w))
            ps2 = PyString_AsString(w);
    }
    arena = PyArena_New();
    if (arena == NULL) {
        Py_XDECREF(v);
        Py_XDECREF(w);
        return -1;
    }
    mod = PyParser_ASTFromFile(fp, filename, Py_single_input, ps1, ps2, flags, &errcode, arena);
    Py_XDECREF(v);
    Py_XDECREF(w);
    if (mod == NULL) {
        PyArena_Free(arena);
        if (errcode == E_EOF) {
            PyErr_Clear();
            return E_EOF;
        }
        PyErr_Print();
        return -1;
    }
    m = PyImport_AddModule("__main__");
    if (m == NULL) {
        PyArena_Free(arena);
        return -1;
    }

    // Pyston change:
    // d = PyModule_GetDict(m);
    // v = run_mod(mod, filename, d, d, flags, arena);
    assert(PyModule_Check(m));
    bool failed = false;
    try {
        assert(mod->kind == Interactive_kind);
        AST_Module* pyston_module = static_cast<AST_Module*>(cpythonToPystonAST(mod, filename));
        compileAndRunModule(pyston_module, static_cast<BoxedModule*>(m));
    } catch (ExcInfo e) {
        setCAPIException(e);
        failed = true;
    }

    PyArena_Free(arena);
    if (failed) {
        PyErr_Print();
        return -1;
    }
    // Pyston change: we dont't have v
    // Py_DECREF(v);

    if (Py_FlushLine())
        PyErr_Clear();
    return 0;
}

/* Set the error appropriate to the given input error code (see errcode.h) */

static void err_input(perrdetail* err) noexcept {
    PyObject* v, *w, *errtype;
    PyObject* u = NULL;
    const char* msg = NULL;
    errtype = PyExc_SyntaxError;
    switch (err->error) {
        case E_ERROR:
            return;
        case E_SYNTAX:
            errtype = PyExc_IndentationError;
            if (err->expected == INDENT)
                msg = "expected an indented block";
            else if (err->token == INDENT)
                msg = "unexpected indent";
            else if (err->token == DEDENT)
                msg = "unexpected unindent";
            else {
                errtype = PyExc_SyntaxError;
                msg = "invalid syntax";
            }
            break;
        case E_TOKEN:
            msg = "invalid token";
            break;
        case E_EOFS:
            msg = "EOF while scanning triple-quoted string literal";
            break;
        case E_EOLS:
            msg = "EOL while scanning string literal";
            break;
        case E_INTR:
            if (!PyErr_Occurred())
                PyErr_SetNone(PyExc_KeyboardInterrupt);
            goto cleanup;
        case E_NOMEM:
            PyErr_NoMemory();
            goto cleanup;
        case E_EOF:
            msg = "unexpected EOF while parsing";
            break;
        case E_TABSPACE:
            errtype = PyExc_TabError;
            msg = "inconsistent use of tabs and spaces in indentation";
            break;
        case E_OVERFLOW:
            msg = "expression too long";
            break;
        case E_DEDENT:
            errtype = PyExc_IndentationError;
            msg = "unindent does not match any outer indentation level";
            break;
        case E_TOODEEP:
            errtype = PyExc_IndentationError;
            msg = "too many levels of indentation";
            break;
        case E_DECODE: {
            PyObject* type, *value, *tb;
            PyErr_Fetch(&type, &value, &tb);
            if (value != NULL) {
                u = PyObject_Str(value);
                if (u != NULL) {
                    msg = PyString_AsString(u);
                }
            }
            if (msg == NULL)
                msg = "unknown decode error";
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(tb);
            break;
        }
        case E_LINECONT:
            msg = "unexpected character after line continuation character";
            break;
        default:
            fprintf(stderr, "error=%d\n", err->error);
            msg = "unknown parsing error";
            break;
    }
    v = Py_BuildValue("(ziiz)", err->filename, err->lineno, err->offset, err->text);
    w = NULL;
    if (v != NULL)
        w = Py_BuildValue("(sO)", msg, v);
    Py_XDECREF(u);
    Py_XDECREF(v);
    PyErr_SetObject(errtype, w);
    Py_XDECREF(w);
cleanup:
    if (err->text != NULL) {
        PyObject_FREE(err->text);
        err->text = NULL;
    }
}
#if 0
/* compute parser flags based on compiler flags */
#define PARSER_FLAGS(flags)                                                                                            \
    ((flags) ? ((((flags)->cf_flags & PyCF_DONT_IMPLY_DEDENT) ? PyPARSE_DONT_IMPLY_DEDENT : 0)) : 0)
#endif
#if 1
/* Keep an example of flags with future keyword support. */
#define PARSER_FLAGS(flags)                                                                                            \
    ((flags) ? ((((flags)->cf_flags & PyCF_DONT_IMPLY_DEDENT) ? PyPARSE_DONT_IMPLY_DEDENT : 0)                         \
                | (((flags)->cf_flags & CO_FUTURE_PRINT_FUNCTION) ? PyPARSE_PRINT_IS_FUNCTION : 0)                     \
                | (((flags)->cf_flags & CO_FUTURE_UNICODE_LITERALS) ? PyPARSE_UNICODE_LITERALS : 0))                   \
             : 0)
#endif

extern "C" void PyParser_SetError(perrdetail* err) noexcept {
    err_input(err);
}


extern "C" grammar _PyParser_Grammar;

/* Preferred access to parser is through AST. */
extern "C" mod_ty PyParser_ASTFromString(const char* s, const char* filename, int start, PyCompilerFlags* flags,
                                         PyArena* arena) noexcept {
    mod_ty mod;
    PyCompilerFlags localflags;
    perrdetail err;
    int iflags = PARSER_FLAGS(flags);

    node* n = PyParser_ParseStringFlagsFilenameEx(s, filename, &_PyParser_Grammar, start, &err, &iflags);
    if (flags == NULL) {
        localflags.cf_flags = 0;
        flags = &localflags;
    }
    if (n) {
        flags->cf_flags |= iflags & PyCF_MASK;
        mod = PyAST_FromNode(n, flags, filename, arena);
        PyNode_Free(n);
        return mod;
    } else {
        err_input(&err);
        return NULL;
    }
}

extern "C" mod_ty PyParser_ASTFromFile(FILE* fp, const char* filename, int start, char* ps1, char* ps2,
                                       PyCompilerFlags* flags, int* errcode, PyArena* arena) noexcept {
    mod_ty mod;
    PyCompilerFlags localflags;
    perrdetail err;
    int iflags = PARSER_FLAGS(flags);

    node* n = PyParser_ParseFileFlagsEx(fp, filename, &_PyParser_Grammar, start, ps1, ps2, &err, &iflags);
    if (flags == NULL) {
        localflags.cf_flags = 0;
        flags = &localflags;
    }
    if (n) {
        flags->cf_flags |= iflags & PyCF_MASK;
        mod = PyAST_FromNode(n, flags, filename, arena);
        PyNode_Free(n);
        return mod;
    } else {
        err_input(&err);
        if (errcode)
            *errcode = err.error;
        return NULL;
    }
}

extern "C" PyObject* Py_CompileStringFlags(const char* str, const char* filename, int start,
                                           PyCompilerFlags* flags) noexcept {
    PyCodeObject* co;
    mod_ty mod;
    PyArena* arena = PyArena_New();
    if (arena == NULL)
        return NULL;

    mod = PyParser_ASTFromString(str, filename, start, flags, arena);
    if (mod == NULL) {
        PyArena_Free(arena);
        return NULL;
    }
    if (flags && (flags->cf_flags & PyCF_ONLY_AST)) {
        PyObject* result = PyAST_mod2obj(mod);
        PyArena_Free(arena);
        return result;
    }
    co = PyAST_Compile(mod, filename, flags, arena);
    PyArena_Free(arena);
    return (PyObject*)co;
}

static PyObject* run_mod(mod_ty mod, const char* filename, PyObject* globals, PyObject* locals, PyCompilerFlags* flags,
                         PyArena* arena) noexcept {
    PyCodeObject* co;
    PyObject* v;
    co = PyAST_Compile(mod, filename, flags, arena);
    if (co == NULL)
        return NULL;
    v = PyEval_EvalCode(co, globals, locals);
    Py_DECREF(co);
    return v;
}

extern "C" PyObject* PyRun_FileExFlags(FILE* fp, const char* filename, int start, PyObject* globals, PyObject* locals,
                                       int closeit, PyCompilerFlags* flags) noexcept {
    PyObject* ret;
    mod_ty mod;
    PyArena* arena = PyArena_New();
    if (arena == NULL)
        return NULL;

    mod = PyParser_ASTFromFile(fp, filename, start, 0, 0, flags, NULL, arena);
    if (closeit)
        fclose(fp);
    if (mod == NULL) {
        PyArena_Free(arena);
        return NULL;
    }
    ret = run_mod(mod, filename, globals, locals, flags, arena);
    PyArena_Free(arena);
    return ret;
}

extern "C" PyObject* PyRun_StringFlags(const char* str, int start, PyObject* globals, PyObject* locals,
                                       PyCompilerFlags* flags) noexcept {
    PyObject* ret = NULL;
    mod_ty mod;
    PyArena* arena = PyArena_New();
    if (arena == NULL)
        return NULL;

    mod = PyParser_ASTFromString(str, "<string>", start, flags, arena);
    if (mod != NULL)
        ret = run_mod(mod, "<string>", globals, locals, flags, arena);
    PyArena_Free(arena);
    return ret;
}

extern "C" int PyRun_InteractiveLoopFlags(FILE* fp, const char* filename, PyCompilerFlags* flags) noexcept {
    PyObject* v;
    int ret;
    PyCompilerFlags local_flags;

    if (flags == NULL) {
        flags = &local_flags;
        local_flags.cf_flags = 0;
    }
    v = PySys_GetObject("ps1");
    if (v == NULL) {
        PySys_SetObject("ps1", v = PyString_FromString(">> "));
        Py_XDECREF(v);
    }
    v = PySys_GetObject("ps2");
    if (v == NULL) {
        PySys_SetObject("ps2", v = PyString_FromString("... "));
        Py_XDECREF(v);
    }
    for (;;) {
        ret = PyRun_InteractiveOneFlags(fp, filename, flags);
        // PRINT_TOTAL_REFS();
        if (ret == E_EOF)
            return 0;
        if (ret == E_NOMEM)
            return -1;
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

extern "C" BORROWED(PyObject*) PyThreadState_GetDict(void) noexcept {
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

static PyThread_type_lock pending_lock = 0; /* for pending calls */

/* The WITH_THREAD implementation is thread-safe.  It allows
   scheduling to be made from any thread, and even from an executing
   callback.
 */

#define NPENDINGCALLS 32
static struct {
    int (*func)(void*);
    void* arg;
} pendingcalls[NPENDINGCALLS];
static int pendingfirst = 0;
static int pendinglast = 0;
// Pyston change
// static volatile int pendingcalls_to_do = 1; /* trigger initialization of lock */
extern "C" {
volatile int _pendingcalls_to_do = 1;
}
static char pendingbusy = 0;

extern "C" int Py_AddPendingCall(int (*func)(void*), void* arg) noexcept {
    int i, j, result = 0;
    PyThread_type_lock lock = pending_lock;

    /* try a few times for the lock.  Since this mechanism is used
     * for signal handling (on the main thread), there is a (slim)
     * chance that a signal is delivered on the same thread while we
     * hold the lock during the Py_MakePendingCalls() function.
     * This avoids a deadlock in that case.
     * Note that signals can be delivered on any thread.  In particular,
     * on Windows, a SIGINT is delivered on a system-created worker
     * thread.
     * We also check for lock being NULL, in the unlikely case that
     * this function is called before any bytecode evaluation takes place.
     */
    if (lock != NULL) {
        for (i = 0; i < 100; i++) {
            if (PyThread_acquire_lock(lock, NOWAIT_LOCK))
                break;
        }
        if (i == 100)
            return -1;
    }

    i = pendinglast;
    j = (i + 1) % NPENDINGCALLS;
    if (j == pendingfirst) {
        result = -1; /* Queue full */
    } else {
        pendingcalls[i].func = func;
        pendingcalls[i].arg = arg;
        pendinglast = j;
    }
    /* signal main loop */
    // Pyston change: we don't have a _Py_Ticker
    // _Py_Ticker = 0;

    _pendingcalls_to_do = 1;
    if (lock != NULL)
        PyThread_release_lock(lock);
    return result;
}

extern "C" int Py_MakePendingCalls(void) noexcept {
    int i;
    int r = 0;

    if (!pending_lock) {
        /* initial allocation of the lock */
        pending_lock = PyThread_allocate_lock();
        if (pending_lock == NULL)
            return -1;
    }

    /* only service pending calls on main thread */
    // Pyston change:
    // if (main_thread && PyThread_get_thread_ident() != main_thread)
    if (!threading::isMainThread())
        return 0;
    /* don't perform recursive pending calls */
    if (pendingbusy)
        return 0;
    pendingbusy = 1;
    /* perform a bounded number of calls, in case of recursion */
    for (i = 0; i < NPENDINGCALLS; i++) {
        int j;
        int (*func)(void*);
        void* arg = NULL;

        /* pop one item off the queue while holding the lock */
        PyThread_acquire_lock(pending_lock, WAIT_LOCK);
        j = pendingfirst;
        if (j == pendinglast) {
            func = NULL; /* Queue empty */
        } else {
            func = pendingcalls[j].func;
            arg = pendingcalls[j].arg;
            pendingfirst = (j + 1) % NPENDINGCALLS;
        }
        _pendingcalls_to_do = pendingfirst != pendinglast;
        PyThread_release_lock(pending_lock);
        /* having released the lock, perform the callback */
        if (func == NULL)
            break;
        r = func(arg);
        if (r)
            break;
    }
    pendingbusy = 0;
    return r;
}

extern "C" void makePendingCalls() {
    int ret = Py_MakePendingCalls();
    if (ret != 0)
        throwCAPIException();
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
    return new BoxedCApiFunction(ml, self, module);
}

extern "C" PyCFunction PyCFunction_GetFunction(PyObject* op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedCApiFunction*>(op)->getFunction();
}

extern "C" PyObject* PyCFunction_GetSelf(BORROWED(PyObject*) op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return static_cast<BoxedCApiFunction*>(op)->passthrough;
}

extern "C" int PyCFunction_GetFlags(PyObject* op) noexcept {
    if (!PyCFunction_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return static_cast<BoxedCApiFunction*>(op)->method_def->ml_flags;
}

extern "C" PyObject* PyCFunction_Call(PyObject* func, PyObject* arg, PyObject* kw) noexcept {
    assert(arg->cls == tuple_cls);
    assert(!kw || kw->cls == dict_cls);
    return BoxedCApiFunction::tppCall<CAPI>(func, NULL, ArgPassSpec(0, 0, true, true), arg, kw, NULL, NULL, NULL);
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

extern "C" BORROWED(struct _frame*) PyEval_GetFrame(void) noexcept {
    Box* frame = NULL;
    try {
        frame = getFrame(0);
    } catch (ExcInfo e) {
        e.clear();
        RELEASE_ASSERT(0, "untested");
    }
    return (struct _frame*)frame;
}

extern "C" char* PyModule_GetName(PyObject* m) noexcept {
    PyObject* d;
    PyObject* nameobj;
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    static BoxedString* name_str = getStaticString("__name__");
    if ((nameobj = m->getattr(name_str)) == NULL || !PyString_Check(nameobj)) {
        PyErr_SetString(PyExc_SystemError, "nameless module");
        return NULL;
    }
    return PyString_AsString(nameobj);
}

extern "C" char* PyModule_GetFilename(PyObject* m) noexcept {
    PyObject* d;
    PyObject* fileobj;
    if (!PyModule_Check(m)) {
        PyErr_BadArgument();
        return NULL;
    }
    static BoxedString* file_str = getStaticString("__file__");
    if ((fileobj = m->getattr(file_str)) == NULL || !PyString_Check(fileobj)) {
        PyErr_SetString(PyExc_SystemError, "module filename missing");
        return NULL;
    }
    return PyString_AsString(fileobj);
}

Box* BoxedCApiFunction::__call__(BoxedCApiFunction* self, BoxedTuple* varargs, BoxedDict* kwargs) {
    STAT_TIMER(t0, "us_timer_boxedcapifunction__call__", (self->cls->is_user_defined ? 10 : 20));
    assert(self->cls == capifunc_cls);
    assert(varargs->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    // Kind of silly to have asked callFunc to rearrange the arguments for us, just to pass things
    // off to tppCall, but this case should be very uncommon (people explicitly asking for __call__)

    return BoxedCApiFunction::tppCall<CXX>(self, NULL, ArgPassSpec(0, 0, true, true), varargs, kwargs, NULL, NULL,
                                           NULL);
}

template <ExceptionStyle S>
Box* BoxedCApiFunction::tppCall(Box* _self, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                                Box* arg3, Box** args,
                                const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_boxedcapifunction__call__", 10);

    BoxedCApiFunction* self = static_cast<BoxedCApiFunction*>(_self);

    if (rewrite_args) {
        rewrite_args->obj->addGuard((intptr_t)self);
        rewrite_args->rewriter->addGCReference(self);
    }

    int flags = self->method_def->ml_flags;
    auto func = self->method_def->ml_meth;

    flags &= ~(METH_CLASS | METH_STATIC | METH_COEXIST);

    ParamReceiveSpec paramspec(0, 0, true, false);
    Box** defaults = NULL;
    if (flags == METH_VARARGS) {
        paramspec = ParamReceiveSpec(0, 0, true, false);
    } else if (flags == (METH_VARARGS | METH_KEYWORDS)) {
        paramspec = ParamReceiveSpec(0, 0, true, true);
    } else if (flags == METH_NOARGS) {
        paramspec = ParamReceiveSpec(0, 0, false, false);
    } else if (flags == METH_O) {
        paramspec = ParamReceiveSpec(1, 0, false, false);
    } else if (flags == METH_OLDARGS) {
        paramspec = ParamReceiveSpec(1, 0, false, false);
    } else if ((flags & ~(METH_O3 | METH_D3)) == 0) {
        int num_args = 0;
        if (flags & METH_O)
            num_args++;
        if (flags & METH_O2)
            num_args += 2;

        int num_defaults = 0;
        if (flags & METH_D1)
            num_defaults++;
        if (flags & METH_D2)
            num_defaults += 2;

        paramspec = ParamReceiveSpec(num_args, num_defaults, false, false);
        if (num_defaults) {
            static Box* _defaults[] = { NULL, NULL, NULL };
            assert(num_defaults <= 3);
            defaults = _defaults;
        }

        assert(paramspec.totalReceived() <= 3); // would need to allocate oargs
    } else {
        RELEASE_ASSERT(0, "0x%x", flags);
    }

    Box** oargs = NULL;

    if (func == (void*)tp_new_wrapper) {
        assert(PyType_Check(self->passthrough));
        BoxedClass* passthrough_cls = static_cast<BoxedClass*>(self->passthrough);
        if (passthrough_cls->tp_new == BaseException->tp_new && argspec.num_args >= 1) {
            // Optimization: BaseException->tp_new doesn't look at its arguments.
            // Don't bother allocating any
            assert(paramspec == ParamReceiveSpec(0, 0, true, true));

            assert(PyType_Check(arg1));
            Box* rtn = BaseException->tp_new(static_cast<BoxedClass*>(arg1), NULL, NULL);
            if (rewrite_args) {
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)BaseException->tp_new, rewrite_args->arg1,
                                                   rewrite_args->rewriter->loadConst(0, Location::forArg(1)),
                                                   rewrite_args->rewriter->loadConst(0, Location::forArg(2)));
                rewrite_args->out_rtn->setType(RefType::OWNED);
                rewrite_args->out_success = true;
            }
            assert(rtn); // This shouldn't throw, otherwise we would need to check the return value
            return rtn;
        }
        // TODO rewrite these cases specially; tp_new_wrapper just slices the args array,
        // so we could just rearrangeArguments to the form that it wants and then call tp_new directly.
    }

    auto continuation = [=](CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        RewriterVar* r_passthrough = NULL;
        if (rewrite_args)
            r_passthrough = rewrite_args->rewriter->loadConst((intptr_t)self->passthrough, Location::forArg(0));

        Box* rtn;
        if (flags == METH_VARARGS) {
            rtn = (Box*)func(self->passthrough, arg1);
            if (rewrite_args)
                rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                                     rewrite_args->arg1)->setType(RefType::OWNED);
        } else if (flags == (METH_VARARGS | METH_KEYWORDS)) {
            rtn = (Box*)((PyCFunctionWithKeywords)func)(self->passthrough, arg1, arg2);
            if (rewrite_args)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)func, r_passthrough, rewrite_args->arg1,
                                                   rewrite_args->arg2)->setType(RefType::OWNED);
        } else if (flags == METH_NOARGS) {
            rtn = (Box*)func(self->passthrough, NULL);
            if (rewrite_args)
                rewrite_args->out_rtn
                    = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                   rewrite_args->rewriter->loadConst(0, Location::forArg(1)))
                          ->setType(RefType::OWNED);
        } else if (flags == METH_O) {
            rtn = (Box*)func(self->passthrough, arg1);
            if (rewrite_args)
                rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                                     rewrite_args->arg1)->setType(RefType::OWNED);
        } else if ((flags & ~(METH_O3 | METH_D3)) == 0) {
            assert(paramspec.totalReceived() <= 3); // would need to pass through oargs
            rtn = ((Box * (*)(Box*, Box*, Box*, Box**))func)(self->passthrough, arg1, arg2, &arg3);
            if (rewrite_args) {
                if (paramspec.totalReceived() == 1)
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)func, r_passthrough,
                                                                         rewrite_args->arg1)->setType(RefType::OWNED);
                else if (paramspec.totalReceived() == 2)
                    rewrite_args->out_rtn
                        = rewrite_args->rewriter->call(true, (void*)func, r_passthrough, rewrite_args->arg1,
                                                       rewrite_args->arg2)->setType(RefType::OWNED);
                else if (paramspec.totalReceived() == 3) {
                    auto args = rewrite_args->rewriter->allocate(1);
                    args->setAttr(0, rewrite_args->arg3);
                    rewrite_args->out_rtn
                        = rewrite_args->rewriter->call(true, (void*)func, r_passthrough, rewrite_args->arg1,
                                                       rewrite_args->arg2, args)->setType(RefType::OWNED);
                } else
                    abort();
            }
        } else if (flags == METH_OLDARGS) {
            /* the really old style */

            rewrite_args = NULL;

            int size = PyTuple_GET_SIZE(arg1);
            Box* arg = arg1;
            if (size == 1)
                arg = PyTuple_GET_ITEM(arg1, 0);
            else if (size == 0)
                arg = NULL;
            rtn = func(self->passthrough, arg);
        } else {
            RELEASE_ASSERT(0, "0x%x", flags);
        }

        if (rewrite_args) {
            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(rewrite_args->out_rtn);
            rewrite_args->out_success = true;
        }

        if (S == CXX && !rtn)
            throwCAPIException();
        return rtn;
    };

    return callCXXFromStyle<S>([=]() {
        return rearrangeArgumentsAndCall(paramspec, NULL, self->method_def->ml_name, defaults, rewrite_args, argspec,
                                         arg1, arg2, arg3, args, keyword_names, continuation);
    });
}

/* extension modules might be compiled with GC support so these
   functions must always be available */

#undef PyObject_GC_Track
#undef PyObject_GC_UnTrack
#undef PyObject_GC_Del
#undef _PyObject_GC_Malloc

#ifdef HAVE_GCC_ASM_FOR_X87

/* inline assembly for getting and setting the 387 FPU control word on
   gcc/x86 */

extern "C" unsigned short _Py_get_387controlword(void) noexcept {
    unsigned short cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    return cw;
}

extern "C" void _Py_set_387controlword(unsigned short cw) noexcept {
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
}

#endif

extern "C" void _Py_FatalError(const char* fmt, const char* function, const char* message) {
    fprintf(stderr, fmt, function, message);
    fflush(stderr); /* it helps in Windows debug build */
    abort();
}

extern "C" int PyCFunction_ClearFreeList() noexcept {
    return 0; // number of entries cleared
}

extern "C" int PyNumber_Coerce(PyObject** pv, PyObject** pw) noexcept {
    int err = PyNumber_CoerceEx(pv, pw);
    if (err <= 0)
        return err;
    PyErr_SetString(PyExc_TypeError, "number coercion failed");
    return -1;
}

extern "C" int PyNumber_CoerceEx(PyObject** pv, PyObject** pw) noexcept {
    PyObject* v = *pv;
    PyObject* w = *pw;
    int res;

    /* Shortcut only for old-style types */
    if (v->cls == w->cls && !PyType_HasFeature(v->cls, Py_TPFLAGS_CHECKTYPES)) {
        Py_INCREF(v);
        Py_INCREF(w);
        return 0;
    }
    if (v->cls->tp_as_number && v->cls->tp_as_number->nb_coerce) {
        res = (*v->cls->tp_as_number->nb_coerce)(pv, pw);
        if (res <= 0)
            return res;
    }
    if (w->cls->tp_as_number && w->cls->tp_as_number->nb_coerce) {
        res = (*w->cls->tp_as_number->nb_coerce)(pw, pv);
        if (res <= 0)
            return res;
    }
    return 1;
}

void setupCAPI() {
    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(BoxedCode::create((void*)BoxedCApiFunction::__repr__<CXX>, UNKNOWN, 1)));

    auto capi_call = new BoxedFunction(BoxedCode::create((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, true, true));
    capifunc_cls->giveAttr("__call__", capi_call);
    capifunc_cls->tpp_call.capi_val = BoxedCApiFunction::tppCall<CAPI>;
    capifunc_cls->tpp_call.cxx_val = BoxedCApiFunction::tppCall<CXX>;
    capifunc_cls->giveAttrDescriptor("__name__", BoxedCApiFunction::getname, NULL);
    capifunc_cls->giveAttrDescriptor("__doc__", BoxedCApiFunction::doc, NULL);
    capifunc_cls->giveAttrMember("__module__", T_OBJECT, offsetof(BoxedCApiFunction, module));

    capifunc_cls->freeze();
    capifunc_cls->tp_repr = BoxedCApiFunction::__repr__<CAPI>;
}
}
