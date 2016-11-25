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

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <err.h>

#include "llvm/Support/FileSystem.h"

#include "Python.h"
#include "Python-ast.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/ast_interpreter.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/unwinding.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/ics.h"
#include "runtime/import.h"
#include "runtime/inline/list.h"
#include "runtime/inline/xrange.h"
#include "runtime/iterobject.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/rewrite_args.h"
#include "runtime/set.h"
#include "runtime/super.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
Box* Ellipsis = 0;

// Copied from CPython:
#if defined(MS_WINDOWS) && defined(HAVE_USABLE_WCHAR_T)
const char* Py_FileSystemDefaultEncoding = "mbcs";
#elif defined(__APPLE__)
const char* Py_FileSystemDefaultEncoding = "utf-8";
#else
const char* Py_FileSystemDefaultEncoding = "UTF-8"; // Pyston change: modified to UTF-8
#endif
}

extern "C" Box* trap() {
    raise(SIGTRAP);

    Py_RETURN_NONE;
}

/* Helper for PyObject_Dir.
   Merge the __dict__ of aclass into dict, and recursively also all
   the __dict__s of aclass's base classes.  The order of merging isn't
   defined, as it's expected that only the final set of dict keys is
   interesting.
   Return 0 on success, -1 on error.
*/

extern "C" Box* dir(Box* obj) {
    Box* r = PyObject_Dir(obj);
    if (!r)
        throwCAPIException();
    return r;
}

extern "C" Box* vars(Box* obj) {
    if (!obj)
        return incref(PyEval_GetLocals());

    static BoxedString* dict_str = getStaticString("__dict__");
    Box* rtn = getattrInternal<ExceptionStyle::CAPI>(obj, dict_str);
    if (!rtn)
        raiseExcHelper(TypeError, "vars() argument must have __dict__ attribute");
    return rtn;
}

extern "C" Box* abs_(Box* x) {
    Box* rtn = PyNumber_Absolute(x);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

static PyObject* builtin_bin(PyObject* self, PyObject* v) noexcept {
    return PyNumber_ToBase(v, 2);
}

static PyObject* builtin_hex(PyObject* self, PyObject* v) noexcept {
    PyNumberMethods* nb;
    PyObject* res;

    if ((nb = v->cls->tp_as_number) == NULL || nb->nb_hex == NULL) {
        PyErr_SetString(PyExc_TypeError, "hex() argument can't be converted to hex");
        return NULL;
    }
    res = (*nb->nb_hex)(v);
    if (res && !PyString_Check(res)) {
        PyErr_Format(PyExc_TypeError, "__hex__ returned non-string (type %.200s)", res->cls->tp_name);
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

static PyObject* builtin_oct(PyObject* self, PyObject* v) noexcept {
    PyNumberMethods* nb;
    PyObject* res;

    if (v == NULL || (nb = v->cls->tp_as_number) == NULL || nb->nb_oct == NULL) {
        PyErr_SetString(PyExc_TypeError, "oct() argument can't be converted to oct");
        return NULL;
    }
    res = (*nb->nb_oct)(v);
    if (res && !PyString_Check(res)) {
        PyErr_Format(PyExc_TypeError, "__oct__ returned non-string (type %.200s)", res->cls->tp_name);
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

extern "C" Box* all(Box* container) {
    for (Box* e : container->pyElements()) {
        AUTO_DECREF(e);
        if (!nonzero(e)) {
            return boxBool(false);
        }
    }
    return boxBool(true);
}

extern "C" Box* any(Box* container) {
    for (Box* e : container->pyElements()) {
        AUTO_DECREF(e);
        if (nonzero(e)) {
            return boxBool(true);
        }
    }
    return boxBool(false);
}

Box* min_max(Box* arg0, BoxedTuple* args, BoxedDict* kwargs, int opid) {
    assert(args->cls == tuple_cls);
    if (kwargs)
        assert(kwargs->cls == dict_cls);

    Box* key_func = nullptr;
    Box* extremElement;
    Box* container;
    Box* extremVal;

    if (kwargs && kwargs->d.size()) {
        static BoxedString* key_str = static_cast<BoxedString*>(getStaticString("key"));
        auto it = kwargs->d.find(key_str);
        if (it != kwargs->d.end() && kwargs->d.size() == 1) {
            key_func = it->second;
        } else {
            if (opid == Py_LT)
                raiseExcHelper(TypeError, "min() got an unexpected keyword argument");
            else
                raiseExcHelper(TypeError, "max() got an unexpected keyword argument");
        }
    }

    XKEEP_ALIVE(key_func); // probably not necessary

    if (args->size() == 0) {
        extremElement = nullptr;
        extremVal = nullptr;
        container = arg0;
    } else {
        if (key_func != NULL) {
            extremVal = runtimeCall(key_func, ArgPassSpec(1), arg0, NULL, NULL, NULL, NULL);
        } else {
            extremVal = incref(arg0);
        }
        extremElement = incref(arg0);
        container = args;
    }

    Box* curVal = nullptr;
    for (Box* e : container->pyElements()) {
        if (key_func != NULL) {
            if (!extremElement) {
                extremVal = runtimeCall(key_func, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
                extremElement = e;
                continue;
            }
            try {
                curVal = runtimeCall(key_func, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
            } catch (ExcInfo ex) {
                Py_DECREF(e);
                Py_DECREF(extremVal);
                Py_DECREF(extremElement);
                throw ex;
            }
        } else {
            if (!extremElement) {
                extremVal = incref(e);
                extremElement = e;
                continue;
            }
            curVal = incref(e);
        }
        int r = PyObject_RichCompareBool(curVal, extremVal, opid);
        if (r == -1) {
            Py_DECREF(e);
            Py_DECREF(extremVal);
            Py_DECREF(extremElement);
            Py_DECREF(curVal);
            throwCAPIException();
        }
        if (r) {
            Py_DECREF(extremElement);
            Py_DECREF(extremVal);
            extremElement = e;
            extremVal = curVal;
        } else {
            Py_DECREF(curVal);
            Py_DECREF(e);
        }
    }
    Py_XDECREF(extremVal);
    return extremElement;
}

extern "C" Box* min(Box* arg0, BoxedTuple* args, BoxedDict* kwargs) {
    if (arg0 == Py_None && args->size() == 0) {
        raiseExcHelper(TypeError, "min expected 1 arguments, got 0");
    }

    Box* minElement = min_max(arg0, args, kwargs, Py_LT);

    if (!minElement) {
        raiseExcHelper(ValueError, "min() arg is an empty sequence");
    }
    return minElement;
}

extern "C" Box* max(Box* arg0, BoxedTuple* args, BoxedDict* kwargs) {
    if (arg0 == Py_None && args->size() == 0) {
        raiseExcHelper(TypeError, "max expected 1 arguments, got 0");
    }

    Box* maxElement = min_max(arg0, args, kwargs, Py_GT);

    if (!maxElement) {
        raiseExcHelper(ValueError, "max() arg is an empty sequence");
    }
    return maxElement;
}

extern "C" Box* next(Box* iterator, Box* _default) noexcept {
    if (!PyIter_Check(iterator)) {
        PyErr_Format(PyExc_TypeError, "%.200s object is not an iterator", iterator->cls->tp_name);
        return NULL;
    }

    Box* rtn;

    if (iterator->cls->tp_iternext == slot_tp_iternext) {
        rtn = iterator->cls->call_nextIC(iterator);
    } else {
        rtn = iterator->cls->tp_iternext(iterator);
    }

    if (rtn != NULL) {
        return rtn;
    } else if (_default != NULL) {
        if (PyErr_Occurred()) {
            if (!PyErr_ExceptionMatches(PyExc_StopIteration))
                return NULL;
            PyErr_Clear();
        }
        Py_INCREF(_default);
        return _default;
    } else if (PyErr_Occurred()) {
        return NULL;
    } else {
        PyErr_SetNone(PyExc_StopIteration);
        return NULL;
    }
}

extern "C" Box* sum(Box* container, Box* initial) {
    if (initial->cls == str_cls)
        raiseExcHelper(TypeError, "sum() can't sum strings [use ''.join(seq) instead]");

    static RuntimeICCache<BinopIC, 3> runtime_ic_cache;
    std::shared_ptr<BinopIC> pp = runtime_ic_cache.getIC(__builtin_return_address(0));

    Py_INCREF(initial);
    auto cur = autoDecref(initial);
    for (Box* e : container->pyElements()) {
        AUTO_DECREF(e);
        cur = pp->call(cur, e, AST_TYPE::Add);
    }
    return incref(cur.get());
}

extern "C" Box* id(Box* arg) {
    i64 addr = (i64)(arg);
    return boxInt(addr);
}


Box* open(Box* arg1, Box* arg2, Box* arg3) {
    assert(arg2);
    assert(arg3);
    // This could be optimized quite a bit if it ends up being important:
    return runtimeCall(&PyFile_Type, ArgPassSpec(3), arg1, arg2, arg3, NULL, NULL);
}

extern "C" Box* chr(Box* arg) {
    i64 n = PyInt_AsLong(arg);
    if (n == -1 && PyErr_Occurred())
        throwCAPIException();

    if (n < 0 || n >= 256) {
        raiseExcHelper(ValueError, "chr() arg not in range(256)");
    }

    char c = (char)n;
    return boxString(llvm::StringRef(&c, 1));
}

extern "C" Box* unichr(Box* arg) {
    int n = -1;
    if (!PyArg_ParseSingle(arg, 0, "unichr", "i", &n))
        throwCAPIException();

    Box* rtn = PyUnicode_FromOrdinal(n);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* coerceFunc(Box* vv, Box* ww) {
    Box* res;

    if (PyErr_WarnPy3k("coerce() not supported in 3.x", 1) < 0)
        throwCAPIException();

    if (PyNumber_Coerce(&vv, &ww) < 0)
        throwCAPIException();
    res = PyTuple_Pack(2, vv, ww);
    Py_DECREF(vv);
    Py_DECREF(ww);
    return res;
}

extern "C" Box* ord(Box* obj) {
    long ord;
    Py_ssize_t size;

    if (PyString_Check(obj)) {
        size = PyString_GET_SIZE(obj);
        if (size == 1) {
            ord = (long)((unsigned char)*PyString_AS_STRING(obj));
            return boxInt(ord);
        }
    } else if (PyByteArray_Check(obj)) {
        size = PyByteArray_GET_SIZE(obj);
        if (size == 1) {
            ord = (long)((unsigned char)*PyByteArray_AS_STRING(obj));
            return boxInt(ord);
        }

#ifdef Py_USING_UNICODE
    } else if (PyUnicode_Check(obj)) {
        size = PyUnicode_GET_SIZE(obj);
        if (size == 1) {
            ord = (long)*PyUnicode_AS_UNICODE(obj);
            return boxInt(ord);
        }
#endif
    } else {
        raiseExcHelper(TypeError, "ord() expected string of length 1, but "
                                  "%.200s found",
                       obj->cls->tp_name);
    }

    raiseExcHelper(TypeError, "ord() expected a character, "
                              "but string of length %zd found",
                   size);
}

Box* range(Box* start, Box* stop, Box* step) {
    i64 istart, istop, istep;
    if (stop == NULL) {
        istart = 0;
        istop = PyLong_AsLong(start);
        if ((istop == -1) && PyErr_Occurred())
            throwCAPIException();
        istep = 1;
    } else if (step == NULL) {
        istart = PyLong_AsLong(start);
        if ((istart == -1) && PyErr_Occurred())
            throwCAPIException();
        istop = PyLong_AsLong(stop);
        if ((istop == -1) && PyErr_Occurred())
            throwCAPIException();
        istep = 1;
    } else {
        istart = PyLong_AsLong(start);
        if ((istart == -1) && PyErr_Occurred())
            throwCAPIException();
        istop = PyLong_AsLong(stop);
        if ((istop == -1) && PyErr_Occurred())
            throwCAPIException();
        istep = PyLong_AsLong(step);
        if ((istep == -1) && PyErr_Occurred())
            throwCAPIException();
    }

    BoxedList* rtn = new BoxedList();
    rtn->ensure(std::max(0l, 1 + (istop - istart) / istep));
    if (istep > 0) {
        for (i64 i = istart; i < istop; i += istep) {
            Box* bi = boxInt(i);
            listAppendInternalStolen(rtn, bi);
        }
    } else {
        for (i64 i = istart; i > istop; i += istep) {
            Box* bi = boxInt(i);
            listAppendInternalStolen(rtn, bi);
        }
    }
    return rtn;
}

Box* notimplementedRepr(Box* self) {
    assert(self == NotImplemented);
    return boxString("NotImplemented");
}

// Copied from CPython with some minor modifications
static PyObject* builtin_sorted(PyObject* self, PyObject* args, PyObject* kwds) {
    PyObject* newlist, *v, *seq, * compare = NULL, * keyfunc = NULL, *newargs;
    PyObject* callable;
    static const char* kwlist[] = { "iterable", "cmp", "key", "reverse", 0 };
    int reverse;

    /* args 1-4 should match listsort in Objects/listobject.c */
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|OOi:sorted", const_cast<char**>(kwlist), &seq, &compare, &keyfunc,
                                     &reverse))
        return NULL;

    newlist = PySequence_List(seq);
    if (newlist == NULL)
        return NULL;

    callable = PyObject_GetAttrString(newlist, "sort");
    if (callable == NULL) {
        Py_DECREF(newlist);
        return NULL;
    }

    newargs = PyTuple_GetSlice(args, 1, 4);
    if (newargs == NULL) {
        Py_DECREF(newlist);
        Py_DECREF(callable);
        return NULL;
    }

    v = PyObject_Call(callable, newargs, kwds);
    Py_DECREF(newargs);
    Py_DECREF(callable);
    if (v == NULL) {
        Py_DECREF(newlist);
        return NULL;
    }
    Py_DECREF(v);
    return newlist;
}

Box* isinstance_func(Box* obj, Box* cls) {
    int rtn = PyObject_IsInstance(obj, cls);
    if (rtn < 0)
        throwCAPIException();
    return boxBool(rtn);
}

Box* issubclass_func(Box* child, Box* parent) {
    int rtn = PyObject_IsSubclass(child, parent);
    if (rtn < 0)
        throwCAPIException();
    return boxBool(rtn);
}

Box* intern_func(Box* str) {
    if (!PyString_CheckExact(str)) // have to use exact check!
        raiseExcHelper(TypeError, "can't intern subclass of string");
    Py_INCREF(str);
    PyString_InternInPlace(&str);
    return str;
}

Box* bltinImport(Box* name, Box* globals, Box* locals, Box** args) {
    Box* fromlist = args[0];
    Box* level = args[1];

    // __import__ takes a 'locals' argument, but it doesn't get used in CPython.
    // Well, it gets passed to PyImport_ImportModuleLevel() and then import_module_level(),
    // which ignores it.  So we don't even pass it through.

    name = coerceUnicodeToStr<CXX>(name);
    AUTO_DECREF(name);

    if (name->cls != str_cls) {
        raiseExcHelper(TypeError, "__import__() argument 1 must be string, not %s", getTypeName(name));
    }

    if (level->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    Box* rtn
        = PyImport_ImportModuleLevel(((BoxedString*)name)->c_str(), globals, NULL, fromlist, ((BoxedInt*)level)->n);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* delattrFunc(Box* obj, Box* _str) {
    _str = coerceUnicodeToStr<CXX>(_str);

    if (_str->cls != str_cls) {
        Py_DECREF(_str);
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(_str));
    }
    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);
    AUTO_DECREF(str);

    delattr(obj, str);
    return incref(Py_None);
}

static Box* getattrFuncHelper(STOLEN(Box*) return_val, Box* obj, BoxedString* str, Box* default_val) noexcept {
    assert(PyString_Check(str));

    if (return_val)
        return return_val;

    bool exc = PyErr_Occurred();
    if (exc && !PyErr_ExceptionMatches(AttributeError))
        return NULL;

    if (default_val) {
        if (exc)
            PyErr_Clear();
        return incref(default_val);
    }
    if (!exc)
        raiseAttributeErrorCapi(obj, str->s());
    return NULL;
}

template <ExceptionStyle S>
Box* getattrFuncInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    static Box* defaults[] = { NULL };

    auto continuation = [=](CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        Box* obj = arg1;
        Box* _str = arg2;
        Box* default_value = arg3;

        if (rewrite_args) {
            // We need to make sure that the attribute string will be the same.
            // Even though the passed string might not be the exact attribute name
            // that we end up looking up (because we need to encode it or intern it),
            // guarding on that object means (for strings and unicode) that the string
            // value is fixed.
            if (!PyString_CheckExact(_str) && !PyUnicode_CheckExact(_str))
                rewrite_args = NULL;
            else {
                if (PyString_CheckExact(_str) && PyString_CHECK_INTERNED(_str) == SSTATE_INTERNED_IMMORTAL) {
                    // can avoid keeping the extra gc reference
                } else {
                    rewrite_args->rewriter->addGCReference(_str);
                }

                rewrite_args->arg2->addGuard((intptr_t)arg2);
            }
        }

        _str = coerceUnicodeToStr<S>(_str);

        if (S == CAPI && !_str)
            return (Box*)NULL;

        if (!PyString_Check(_str)) {
            Py_DECREF(_str);
            if (S == CAPI) {
                PyErr_SetString(TypeError, "getattr(): attribute name must be string");
                return (Box*)NULL;
            } else
                raiseExcHelper(TypeError, "getattr(): attribute name must be string");
        }

        BoxedString* str = static_cast<BoxedString*>(_str);
        if (!PyString_CHECK_INTERNED(str))
            internStringMortalInplace(str);
        AUTO_DECREF(str);

        Box* rtn;
        RewriterVar* r_rtn;
        if (rewrite_args) {
            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->arg1, rewrite_args->destination);
            rtn = getattrInternal<CAPI>(obj, str, &grewrite_args);
            // TODO could make the return valid in the NOEXC_POSSIBLE case via a helper
            if (!grewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                ReturnConvention return_convention;
                std::tie(r_rtn, return_convention) = grewrite_args.getReturn();

                // Convert to NOEXC_POSSIBLE:
                if (return_convention == ReturnConvention::NO_RETURN) {
                    return_convention = ReturnConvention::NOEXC_POSSIBLE;
                    r_rtn = rewrite_args->rewriter->loadConst(0);
                } else if (return_convention == ReturnConvention::MAYBE_EXC) {
                    if (default_value)
                        rewrite_args = NULL;
                }
                assert(!rewrite_args || return_convention == ReturnConvention::NOEXC_POSSIBLE
                       || return_convention == ReturnConvention::HAS_RETURN
                       || return_convention == ReturnConvention::CAPI_RETURN
                       || (default_value == NULL && return_convention == ReturnConvention::MAYBE_EXC));
            }
        } else {
            rtn = getattrInternal<CAPI>(obj, str);
        }

        if (rewrite_args) {
            assert(PyString_CHECK_INTERNED(str) == SSTATE_INTERNED_IMMORTAL);
            RewriterVar* r_str = rewrite_args->rewriter->loadConst((intptr_t)str, Location::forArg(2));
            RewriterVar* final_rtn
                = rewrite_args->rewriter->call(false, (void*)getattrFuncHelper, r_rtn, rewrite_args->arg1, r_str,
                                               rewrite_args->arg3)->setType(RefType::OWNED);
            r_rtn->refConsumed();

            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(final_rtn);
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = final_rtn;
        }

        Box* r = getattrFuncHelper(rtn, obj, str, default_value);
        if (S == CXX && !r)
            throwCAPIException();
        return r;
    };

    return callCXXFromStyle<S>([&]() {
        return rearrangeArgumentsAndCall(ParamReceiveSpec(3, 1, false, false), NULL, "getattr", defaults, rewrite_args,
                                         argspec, arg1, arg2, arg3, args, keyword_names, continuation);
    });
}

Box* setattrFunc(Box* obj, Box* _str, Box* value) {
    _str = coerceUnicodeToStr<CXX>(_str);

    if (_str->cls != str_cls) {
        Py_DECREF(_str);
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", _str->cls->tp_name);
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);
    AUTO_DECREF(str);

    setattr(obj, str, incref(value));
    return incref(Py_None);
}

// Not sure if this should be stealing or not:
static Box* hasattrFuncHelper(STOLEN(Box*) return_val) noexcept {
    AUTO_XDECREF(return_val);

    if (return_val)
        Py_RETURN_TRUE;

    if (PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_Exception))
            return NULL;

        PyErr_Clear();
    }
    Py_RETURN_FALSE;
}

template <ExceptionStyle S>
Box* hasattrFuncInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    auto continuation = [=](CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        Box* obj = arg1;
        Box* _str = arg2;

        if (rewrite_args) {
            // We need to make sure that the attribute string will be the same.
            // Even though the passed string might not be the exact attribute name
            // that we end up looking up (because we need to encode it or intern it),
            // guarding on that object means (for strings and unicode) that the string
            // value is fixed.
            if (!PyString_CheckExact(_str) && !PyUnicode_CheckExact(_str))
                rewrite_args = NULL;
            else {
                if (PyString_CheckExact(_str) && PyString_CHECK_INTERNED(_str) == SSTATE_INTERNED_IMMORTAL) {
                    // can avoid keeping the extra gc reference
                } else {
                    rewrite_args->rewriter->addGCReference(_str);
                }

                rewrite_args->arg2->addGuard((intptr_t)arg2);
            }
        }

        _str = coerceUnicodeToStr<S>(_str);

        if (S == CAPI && !_str)
            return (Box*)NULL;

        if (!PyString_Check(_str)) {
            Py_DECREF(_str);
            if (S == CAPI) {
                PyErr_SetString(TypeError, "hasattr(): attribute name must be string");
                return (Box*)NULL;
            } else
                raiseExcHelper(TypeError, "hasattr(): attribute name must be string");
        }

        BoxedString* str = static_cast<BoxedString*>(_str);

        if (!PyString_CHECK_INTERNED(str))
            internStringMortalInplace(str);
        AUTO_DECREF(str);

        Box* rtn;
        RewriterVar* r_rtn;
        if (rewrite_args) {
            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->arg1, rewrite_args->destination);
            rtn = getattrInternal<CAPI>(obj, str, &grewrite_args);
            if (!grewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                ReturnConvention return_convention;
                std::tie(r_rtn, return_convention) = grewrite_args.getReturn();

                // Convert to NOEXC_POSSIBLE:
                if (return_convention == ReturnConvention::NO_RETURN) {
                    return_convention = ReturnConvention::NOEXC_POSSIBLE;
                    r_rtn = rewrite_args->rewriter->loadConst(0);
                } else if (return_convention == ReturnConvention::MAYBE_EXC) {
                    rewrite_args = NULL;
                }
                assert(!rewrite_args || return_convention == ReturnConvention::NOEXC_POSSIBLE
                       || return_convention == ReturnConvention::HAS_RETURN
                       || return_convention == ReturnConvention::CAPI_RETURN);
            }
        } else {
            rtn = getattrInternal<CAPI>(obj, str);
        }

        if (rewrite_args) {
            RewriterVar* final_rtn
                = rewrite_args->rewriter->call(false, (void*)hasattrFuncHelper, r_rtn)->setType(RefType::OWNED);
            r_rtn->refConsumed();

            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(final_rtn);
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = final_rtn;
        }

        Box* r = hasattrFuncHelper(rtn);
        if (S == CXX && !r)
            throwCAPIException();
        return r;
    };

    return callCXXFromStyle<S>([&]() {
        return rearrangeArgumentsAndCall(ParamReceiveSpec(2, 0, false, false), NULL, "hasattr", NULL, rewrite_args,
                                         argspec, arg1, arg2, arg3, args, keyword_names, continuation);
    });
}

Box* map2(Box* f, Box* container) {
    Box* rtn = new BoxedList();
    AUTO_DECREF(rtn);
    bool use_identity_func = f == Py_None;
    for (Box* e : container->pyElements()) {
        Box* val;
        if (use_identity_func)
            val = e;
        else {
            AUTO_DECREF(e);
            val = runtimeCall(f, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
        }
        listAppendInternalStolen(rtn, val);
    }
    return incref(rtn);
}

Box* map(Box* f, BoxedTuple* args) {
    assert(args->cls == tuple_cls);
    auto num_iterable = args->size();
    if (num_iterable < 1)
        raiseExcHelper(TypeError, "map() requires at least two args");

    // performance optimization for the case where we only have one iterable
    if (num_iterable == 1)
        return map2(f, args->elts[0]);

    std::vector<BoxIteratorRange> ranges;
    std::vector<BoxIterator> args_it;
    std::vector<BoxIterator> args_end;

    ranges.reserve(args->size());
    args_it.reserve(args->size());
    args_end.reserve(args->size());

    for (auto e : *args) {
        auto range = e->pyElements();
        ranges.push_back(std::move(range));
        args_it.emplace_back(ranges.back().begin());
        args_end.emplace_back(ranges.back().end());
    }
    assert(args_it.size() == num_iterable);
    assert(args_end.size() == num_iterable);

    bool use_identity_func = f == Py_None;
    Box* rtn = new BoxedList();
    AUTO_DECREF(rtn);
    std::vector<Box*> current_val(num_iterable);
    while (true) {
        int num_done = 0;
        for (int i = 0; i < num_iterable; ++i) {
            if (args_it[i] == args_end[i]) {
                ++num_done;
                current_val[i] = incref(Py_None);
            } else {
                current_val[i] = *args_it[i];
            }
        }

        AUTO_DECREF_ARRAY(&current_val[0], num_iterable);

        if (num_done == num_iterable)
            break;

        Box* entry;
        if (!use_identity_func) {
            auto v = getTupleFromArgsArray(&current_val[0], num_iterable);
            entry = runtimeCall(f, ArgPassSpec(num_iterable), std::get<0>(v), std::get<1>(v), std::get<2>(v),
                                std::get<3>(v), NULL);
        } else
            entry = BoxedTuple::create(num_iterable, &current_val[0]);
        listAppendInternalStolen(rtn, entry);

        for (int i = 0; i < num_iterable; ++i) {
            if (args_it[i] != args_end[i])
                ++args_it[i];
        }
    }
    return incref(rtn);
}

Box* reduce(Box* f, Box* container, Box* initial) {
    Box* current = xincref(initial);

    for (Box* e : container->pyElements()) {
        assert(e);
        if (current == NULL) {
            current = e;
        } else {
            AUTO_DECREF(current);
            AUTO_DECREF(e);
            current = runtimeCall(f, ArgPassSpec(2), current, e, NULL, NULL, NULL);
        }
    }

    if (current == NULL) {
        raiseExcHelper(TypeError, "reduce() of empty sequence with no initial value");
    }

    return current;
}

// from cpython, bltinmodule.c
PyObject* filterstring(PyObject* func, BoxedString* strobj) {
    PyObject* result;
    Py_ssize_t i, j;
    Py_ssize_t len = PyString_Size(strobj);
    Py_ssize_t outlen = len;

    if (func == Py_None) {
        /* If it's a real string we can return the original,
         * as no character is ever false and __getitem__
         * does return this character. If it's a subclass
         * we must go through the __getitem__ loop */
        if (PyString_CheckExact(strobj)) {
            Py_INCREF(strobj);
            return strobj;
        }
    }
    if ((result = PyString_FromStringAndSize(NULL, len)) == NULL)
        return NULL;

    for (i = j = 0; i < len; ++i) {
        PyObject* item;
        int ok;

        item = (*strobj->cls->tp_as_sequence->sq_item)(strobj, i);
        if (item == NULL)
            goto Fail_1;
        if (func == Py_None) {
            ok = 1;
        } else {
            PyObject* arg, *good;
            arg = PyTuple_Pack(1, item);
            if (arg == NULL) {
                Py_DECREF(item);
                goto Fail_1;
            }
            good = PyEval_CallObject(func, arg);
            Py_DECREF(arg);
            if (good == NULL) {
                Py_DECREF(item);
                goto Fail_1;
            }
            ok = PyObject_IsTrue(good);
            Py_DECREF(good);
        }
        if (ok > 0) {
            Py_ssize_t reslen;
            if (!PyString_Check(item)) {
                PyErr_SetString(PyExc_TypeError, "can't filter str to str:"
                                                 " __getitem__ returned different type");
                Py_DECREF(item);
                goto Fail_1;
            }
            reslen = PyString_GET_SIZE(item);
            if (reslen == 1) {
                PyString_AS_STRING(result)[j++] = PyString_AS_STRING(item)[0];
            } else {
                /* do we need more space? */
                Py_ssize_t need = j;

                /* calculate space requirements while checking for overflow */
                if (need > PY_SSIZE_T_MAX - reslen) {
                    Py_DECREF(item);
                    goto Fail_1;
                }

                need += reslen;

                if (need > PY_SSIZE_T_MAX - len) {
                    Py_DECREF(item);
                    goto Fail_1;
                }

                need += len;

                if (need <= i) {
                    Py_DECREF(item);
                    goto Fail_1;
                }

                need = need - i - 1;

                assert(need >= 0);
                assert(outlen >= 0);

                if (need > outlen) {
                    /* overallocate, to avoid reallocations */
                    if (outlen > PY_SSIZE_T_MAX / 2) {
                        Py_DECREF(item);
                        return NULL;
                    }

                    if (need < 2 * outlen) {
                        need = 2 * outlen;
                    }
                    if (_PyString_Resize(&result, need)) {
                        Py_DECREF(item);
                        return NULL;
                    }
                    outlen = need;
                }
                memcpy(PyString_AS_STRING(result) + j, PyString_AS_STRING(item), reslen);
                j += reslen;
            }
        }
        Py_DECREF(item);
        if (ok < 0)
            goto Fail_1;
    }

    if (j < outlen)
        _PyString_Resize(&result, j);

    return result;

Fail_1:
    Py_DECREF(result);
    return NULL;
}

static PyObject* filterunicode(PyObject* func, PyObject* strobj) {
    PyObject* result;
    Py_ssize_t i, j;
    Py_ssize_t len = PyUnicode_GetSize(strobj);
    Py_ssize_t outlen = len;

    if (func == Py_None) {
        /* If it's a real string we can return the original,
         * as no character is ever false and __getitem__
         * does return this character. If it's a subclass
         * we must go through the __getitem__ loop */
        if (PyUnicode_CheckExact(strobj)) {
            Py_INCREF(strobj);
            return strobj;
        }
    }
    if ((result = PyUnicode_FromUnicode(NULL, len)) == NULL)
        return NULL;

    for (i = j = 0; i < len; ++i) {
        PyObject* item, *arg, *good;
        int ok;

        item = (*strobj->cls->tp_as_sequence->sq_item)(strobj, i);
        if (item == NULL)
            goto Fail_1;
        if (func == Py_None) {
            ok = 1;
        } else {
            arg = PyTuple_Pack(1, item);
            if (arg == NULL) {
                Py_DECREF(item);
                goto Fail_1;
            }
            good = PyEval_CallObject(func, arg);
            Py_DECREF(arg);
            if (good == NULL) {
                Py_DECREF(item);
                goto Fail_1;
            }
            ok = PyObject_IsTrue(good);
            Py_DECREF(good);
        }
        if (ok > 0) {
            Py_ssize_t reslen;
            if (!PyUnicode_Check(item)) {
                PyErr_SetString(PyExc_TypeError, "can't filter unicode to unicode:"
                                                 " __getitem__ returned different type");
                Py_DECREF(item);
                goto Fail_1;
            }
            reslen = PyUnicode_GET_SIZE(item);
            if (reslen == 1)
                PyUnicode_AS_UNICODE(result)[j++] = PyUnicode_AS_UNICODE(item)[0];
            else {
                /* do we need more space? */
                Py_ssize_t need = j + reslen + len - i - 1;

                /* check that didnt overflow */
                if ((j > PY_SSIZE_T_MAX - reslen) || ((j + reslen) > PY_SSIZE_T_MAX - len) || ((j + reslen + len) < i)
                    || ((j + reslen + len - i) <= 0)) {
                    Py_DECREF(item);
                    return NULL;
                }

                assert(need >= 0);
                assert(outlen >= 0);

                if (need > outlen) {
                    /* overallocate,
                       to avoid reallocations */
                    if (need < 2 * outlen) {
                        if (outlen > PY_SSIZE_T_MAX / 2) {
                            Py_DECREF(item);
                            return NULL;
                        } else {
                            need = 2 * outlen;
                        }
                    }

                    if (PyUnicode_Resize(&result, need) < 0) {
                        Py_DECREF(item);
                        goto Fail_1;
                    }
                    outlen = need;
                }
                memcpy(PyUnicode_AS_UNICODE(result) + j, PyUnicode_AS_UNICODE(item), reslen * sizeof(Py_UNICODE));
                j += reslen;
            }
        }
        Py_DECREF(item);
        if (ok < 0)
            goto Fail_1;
    }

    if (j < outlen)
        PyUnicode_Resize(&result, j);

    return result;

Fail_1:
    Py_DECREF(result);
    return NULL;
}

static PyObject* filtertuple(PyObject* func, PyObject* tuple) {
    PyObject* result;
    Py_ssize_t i, j;
    Py_ssize_t len = PyTuple_Size(tuple);

    if (len == 0) {
        if (PyTuple_CheckExact(tuple))
            Py_INCREF(tuple);
        else
            tuple = PyTuple_New(0);
        return tuple;
    }

    if ((result = PyTuple_New(len)) == NULL)
        return NULL;

    for (i = j = 0; i < len; ++i) {
        PyObject* item, *good;
        int ok;

        if (tuple->cls->tp_as_sequence && tuple->cls->tp_as_sequence->sq_item) {
            item = tuple->cls->tp_as_sequence->sq_item(tuple, i);
            if (item == NULL)
                goto Fail_1;
        } else {
            PyErr_SetString(PyExc_TypeError, "filter(): unsubscriptable tuple");
            goto Fail_1;
        }
        if (func == Py_None) {
            Py_INCREF(item);
            good = item;
        } else {
            PyObject* arg = PyTuple_Pack(1, item);
            if (arg == NULL) {
                Py_DECREF(item);
                goto Fail_1;
            }
            good = PyEval_CallObject(func, arg);
            Py_DECREF(arg);
            if (good == NULL) {
                Py_DECREF(item);
                goto Fail_1;
            }
        }
        ok = PyObject_IsTrue(good);
        Py_DECREF(good);
        if (ok > 0) {
            if (PyTuple_SetItem(result, j++, item) < 0)
                goto Fail_1;
        } else {
            Py_DECREF(item);
            if (ok < 0)
                goto Fail_1;
        }
    }

    if (_PyTuple_Resize(&result, j) < 0)
        return NULL;

    return result;

Fail_1:
    Py_DECREF(result);
    return NULL;
}

Box* filter2(Box* f, Box* container) {
    // If the filter-function argument is None, filter() works by only returning
    // the elements that are truthy.  This is equivalent to using the bool() constructor.
    // - actually since we call nonzero() afterwards, we could use an ident() function
    //   but we don't have one.
    // If this is a common case we could speed it up with special handling.
    if (f == Py_None)
        f = bool_cls;

    // Special cases depending on the type of container influences the return type
    if (PyTuple_Check(container)) {
        Box* rtn = filtertuple(f, static_cast<BoxedTuple*>(container));
        if (!rtn) {
            throwCAPIException();
        }
        return rtn;
    }

    if (PyString_Check(container)) {
        Box* rtn = filterstring(f, static_cast<BoxedString*>(container));
        if (!rtn) {
            throwCAPIException();
        }
        return rtn;
    }

    if (PyUnicode_Check(container)) {
        Box* rtn = filterunicode(f, container);
        if (!rtn) {
            throwCAPIException();
        }
        return rtn;
    }

    Box* rtn = new BoxedList();
    AUTO_DECREF(rtn);
    for (Box* e : container->pyElements()) {
        AUTO_DECREF(e);
        Box* r = runtimeCall(f, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
        AUTO_DECREF(r);
        bool b = nonzero(r);
        if (b)
            listAppendInternal(rtn, e);
    }
    return incref(rtn);
}

Box* zip(BoxedTuple* containers) {
    assert(containers->cls == tuple_cls);

    BoxedList* rtn = new BoxedList();
    AUTO_DECREF(rtn);
    if (containers->size() == 0)
        return incref(rtn);

    std::vector<BoxIteratorRange> ranges;
    ranges.reserve(containers->size());
    for (auto container : *containers) {
        ranges.push_back(container->pyElements());
    }

    std::vector<BoxIterator> iterators;
    iterators.reserve(containers->size());
    for (auto&& range : ranges) {
        iterators.push_back(range.begin());
    }

    while (true) {
        for (int i = 0; i < iterators.size(); i++) {
            if (iterators[i] == ranges[i].end())
                return incref(rtn);
        }

        auto el = BoxedTuple::create(iterators.size());
        AUTO_DECREF(el);
        for (int i = 0; i < iterators.size(); i++) {
            el->elts[i] = *iterators[i];
            ++(iterators[i]);
        }
        listAppendInternal(rtn, el);
    }
}

static Box* callable(Box* obj) {
    return PyBool_FromLong((long)PyCallable_Check(obj));
}

BoxedClass* notimplemented_cls;
BoxedModule* builtins_module;

class BoxedException : public Box {
public:
    HCAttrs attrs;
    BoxedException() {}

    static Box* __reduce__(Box* self) {
        RELEASE_ASSERT(isSubclass(self->cls, BaseException), "");
        BoxedException* exc = static_cast<BoxedException*>(self);
        return BoxedTuple::create({ self->cls, EmptyTuple, self->getAttrWrapper() });
    }
};

BoxedClass* enumerate_cls;
class BoxedEnumerate : public Box {
private:
    BoxIteratorRange range;
    BoxIterator iterator, iterator_end;
    int64_t idx;
    BoxedLong* idx_long;

public:
    BoxedEnumerate(BoxIteratorRange range, int64_t idx, BoxedLong* idx_long)
        : range(std::move(range)),
          iterator(this->range.begin()),
          iterator_end(this->range.end()),
          idx(idx),
          idx_long(idx_long) {
        Py_XINCREF(idx_long);
    }

    DEFAULT_CLASS_SIMPLE(enumerate_cls, true);

    static Box* new_(Box* cls, Box* obj, Box* start) {
        RELEASE_ASSERT(cls == enumerate_cls, "");
        RELEASE_ASSERT(PyInt_Check(start) || PyLong_Check(start), "");
        int64_t idx = PyInt_AsSsize_t(start);
        BoxedLong* idx_long = NULL;
        if (idx == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            assert(PyLong_Check(start));
            idx_long = (BoxedLong*)start;
        }
        auto&& range = obj->pyElements();
        return new BoxedEnumerate(std::move(range), idx, idx_long);
    }

    static Box* iter(Box* _self) noexcept {
        assert(_self->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        return incref(self);
    }

    static Box* next(Box* _self) {
        assert(_self->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        Box* val = *self->iterator;
        AUTO_DECREF(val);
        ++self->iterator;
        Box* rtn;
        if (self->idx_long)
            rtn = BoxedTuple::create({ self->idx_long, val });
        else
            rtn = BoxedTuple::create({ autoDecref(boxInt(self->idx)), val });

        // check if incrementing the counter would overflow it, if so switch to long counter
        if (self->idx == PY_SSIZE_T_MAX) {
            assert(!self->idx_long);
            self->idx_long = boxLong(self->idx);
            self->idx = -1;
        }
        if (self->idx_long)
            self->idx_long = (BoxedLong*)longAdd(autoDecref(self->idx_long), autoDecref(boxInt(1)));
        else
            ++self->idx;
        return rtn;
    }

    static Box* hasnext(Box* _self) {
        assert(_self->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        return boxBool(self->iterator != self->iterator_end);
    }

    static void dealloc(Box* b) noexcept {
        assert(b->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(b);

        PyObject_GC_UnTrack(self);

        Py_XDECREF(self->idx_long);
        self->iterator.~BoxIterator();
        self->iterator_end.~BoxIterator();
        self->range.~BoxIteratorRange();

        self->cls->tp_free(self);
    }

    static int traverse(Box* b, visitproc visit, void* arg) noexcept {
        assert(b->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(b);

        Py_VISIT(self->idx_long);
        Py_TRAVERSE(self->range);

        return 0;
    }
};

static Box* globals() {
    return incref(getGlobalsDict());
}

static Box* locals() {
    return incref(fastLocalsToBoxedLocals());
}

extern "C" BORROWED(PyObject*) PyEval_GetLocals(void) noexcept {
    try {
        return fastLocalsToBoxedLocals();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" BORROWED(PyObject*) PyEval_GetGlobals(void) noexcept {
    try {
        return getGlobalsDict();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" BORROWED(PyObject*) PyEval_GetBuiltins(void) noexcept {
    return builtins_module->getAttrWrapper();
}

Box* ellipsisRepr(Box* self) {
    return boxString("Ellipsis");
}
Box* divmod(Box* lhs, Box* rhs) {
    return binopInternal<NOT_REWRITABLE, false>(lhs, rhs, AST_TYPE::DivMod, NULL);
}

Box* powFunc(Box* x, Box* y, Box* z) {
    Box* rtn = PyNumber_Power(x, y, z);
    if (!rtn)
        throwCAPIException();
    return rtn;
}

static PyObject* builtin_print(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    static const char* kwlist[] = { "sep", "end", "file", 0 };
    static PyObject* dummy_args = NULL;
    static PyObject* unicode_newline = NULL, * unicode_space = NULL;
    static PyObject* str_newline = NULL, * str_space = NULL;
    PyObject* newline, *space;
    PyObject* sep = NULL, * end = NULL, * file = NULL;
    int i, err, use_unicode = 0;

    if (dummy_args == NULL) {
        if (!(dummy_args = PyTuple_New(0)))
            return NULL;
        constants.push_back(dummy_args);
    }
    if (str_newline == NULL) {
        str_newline = PyString_FromString("\n");
        if (str_newline == NULL)
            return NULL;
        constants.push_back(str_newline);
        str_space = PyString_FromString(" ");
        if (str_space == NULL) {
            Py_CLEAR(str_newline);
            return NULL;
        }
        constants.push_back(str_space);
#ifdef Py_USING_UNICODE
        unicode_newline = PyUnicode_FromString("\n");
        if (unicode_newline == NULL) {
            Py_CLEAR(str_newline);
            Py_CLEAR(str_space);
            return NULL;
        }
        constants.push_back(unicode_newline);
        unicode_space = PyUnicode_FromString(" ");
        if (unicode_space == NULL) {
            Py_CLEAR(str_newline);
            Py_CLEAR(str_space);
            Py_CLEAR(unicode_space);
            return NULL;
        }
        constants.push_back(unicode_space);
#endif
    }
    if (!PyArg_ParseTupleAndKeywords(dummy_args, kwds, "|OOO:print", const_cast<char**>(kwlist), &sep, &end, &file))
        return NULL;
    if (file == NULL || file == Py_None) {
        file = PySys_GetObject("stdout");
        /* sys.stdout may be None when FILE* stdout isn't connected */
        if (file == Py_None)
            Py_RETURN_NONE;
    }
    if (sep == Py_None) {
        sep = NULL;
    } else if (sep) {
        if (PyUnicode_Check(sep)) {
            use_unicode = 1;
        } else if (!PyString_Check(sep)) {
            PyErr_Format(PyExc_TypeError, "sep must be None, str or unicode, not %.200s", sep->cls->tp_name);
            return NULL;
        }
    }
    if (end == Py_None)
        end = NULL;
    else if (end) {
        if (PyUnicode_Check(end)) {
            use_unicode = 1;
        } else if (!PyString_Check(end)) {
            PyErr_Format(PyExc_TypeError, "end must be None, str or unicode, not %.200s", end->cls->tp_name);
            return NULL;
        }
    }

    if (!use_unicode) {
        for (i = 0; i < PyTuple_Size(args); i++) {
            if (PyUnicode_Check(PyTuple_GET_ITEM(args, i))) {
                use_unicode = 1;
                break;
            }
        }
    }
    if (use_unicode) {
        newline = unicode_newline;
        space = unicode_space;
    } else {
        newline = str_newline;
        space = str_space;
    }

    for (i = 0; i < PyTuple_Size(args); i++) {
        if (i > 0) {
            if (sep == NULL)
                err = PyFile_WriteObject(space, file, Py_PRINT_RAW);
            else
                err = PyFile_WriteObject(sep, file, Py_PRINT_RAW);
            if (err)
                return NULL;
        }
        err = PyFile_WriteObject(PyTuple_GetItem(args, i), file, Py_PRINT_RAW);
        if (err)
            return NULL;
    }

    if (end == NULL)
        err = PyFile_WriteObject(newline, file, Py_PRINT_RAW);
    else
        err = PyFile_WriteObject(end, file, Py_PRINT_RAW);
    if (err)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject* builtin_reload(PyObject* self, PyObject* v) noexcept {
    if (PyErr_WarnPy3k("In 3.x, reload() is renamed to imp.reload()", 1) < 0)
        return NULL;

    return PyImport_ReloadModule(v);
}

Box* getreversed(Box* o) {
    static BoxedString* reversed_str = getStaticString("__reversed__");

    // common case:
    if (o->cls == list_cls) {
        return listReversed(o);
    }

    // TODO add rewriting to this?  probably want to try to avoid this path though
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(o, reversed_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (r)
        return r;

    static BoxedString* getitem_str = getStaticString("__getitem__");
    if (!typeLookup(o->cls, getitem_str)) {
        raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o));
    }
    int64_t len = unboxedLen(o); // this will throw an exception if __len__ isn't there

    return new (seqreviter_cls) BoxedSeqIter(o, len - 1);
}

Box* pydump(Box* p, BoxedInt* level) {
    dumpEx(p, level->n);
    return incref(Py_None);
}

Box* pydumpAddr(Box* p) {
    if (p->cls != int_cls)
        raiseExcHelper(TypeError, "Requires an int");

    dump((void*)static_cast<BoxedInt*>(p)->n);
    return incref(Py_None);
}

Box* builtinIter(Box* obj, Box* sentinel) {
    if (sentinel == NULL)
        return getiter(obj);

    if (!PyCallable_Check(obj)) {
        raiseExcHelper(TypeError, "iter(v, w): v must be callable");
    }

    Box* r = PyCallIter_New(obj, sentinel);
    if (!r)
        throwCAPIException();
    return r;
}

// Copied from builtin_raw_input, but without the argument handling
// 'v' is the prompt, and can be NULL corresponding to the arg not getting passed.
static PyObject* raw_input(PyObject* v) noexcept {
    PyObject* fin = PySys_GetObject("stdin");
    PyObject* fout = PySys_GetObject("stdout");

    if (fin == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "[raw_]input: lost sys.stdin");
        return NULL;
    }
    if (fout == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "[raw_]input: lost sys.stdout");
        return NULL;
    }
    if (PyFile_SoftSpace(fout, 0)) {
        if (PyFile_WriteString(" ", fout) != 0)
            return NULL;
    }
    if (PyFile_AsFile(fin) && PyFile_AsFile(fout) && isatty(fileno(PyFile_AsFile(fin)))
        && isatty(fileno(PyFile_AsFile(fout)))) {
        PyObject* po;
        const char* prompt;
        char* s;
        PyObject* result;
        if (v != NULL) {
            po = PyObject_Str(v);
            if (po == NULL)
                return NULL;
            prompt = PyString_AsString(po);
            if (prompt == NULL)
                return NULL;
        } else {
            po = NULL;
            prompt = "";
        }
        s = PyOS_Readline(PyFile_AsFile(fin), PyFile_AsFile(fout), prompt);
        Py_XDECREF(po);
        if (s == NULL) {
            if (!PyErr_Occurred())
                PyErr_SetNone(PyExc_KeyboardInterrupt);
            return NULL;
        }
        if (*s == '\0') {
            PyErr_SetNone(PyExc_EOFError);
            result = NULL;
        } else { /* strip trailing '\n' */
            size_t len = strlen(s);
            if (len > PY_SSIZE_T_MAX) {
                PyErr_SetString(PyExc_OverflowError, "[raw_]input: input too long");
                result = NULL;
            } else {
                result = PyString_FromStringAndSize(s, len - 1);
            }
        }
        PyMem_FREE(s);
        return result;
    }
    if (v != NULL) {
        if (PyFile_WriteObject(v, fout, Py_PRINT_RAW) != 0)
            return NULL;
    }
    return PyFile_GetLine(fin, -1);
}

Box* rawInput(Box* prompt) {
    Box* r = raw_input(prompt);
    if (!r)
        throwCAPIException();
    return r;
}

Box* input(Box* prompt) {
    PyObject* line = rawInput(prompt);
    AUTO_DECREF(line);

    char* str = NULL;
    if (!PyArg_Parse(line, "s;embedded '\\0' in input line", &str))
        throwCAPIException();

    while (*str == ' ' || *str == '\t')
        str++;

    Box* gbls = PyEval_GetGlobals();
    Box* lcls = PyEval_GetLocals();

    // CPython has these safety checks that the builtin functions exist
    // in the current global scope.
    // e.g. eval('input()', {})
    if (PyDict_GetItemString(gbls, "__builtins__") == NULL) {
        if (PyDict_SetItemString(gbls, "__builtins__", PyEval_GetBuiltins()) != 0)
            throwCAPIException();
    }

    PyCompilerFlags cf;
    cf.cf_flags = 0;
    PyEval_MergeCompilerFlags(&cf);
    Box* res = PyRun_StringFlags(str, Py_eval_input, gbls, lcls, &cf);
    if (!res)
        throwCAPIException();
    return res;
}

Box* builtinRound(Box* _number, Box* _ndigits) {
    double x = PyFloat_AsDouble(_number);
    if (PyErr_Occurred())
        throwCAPIException();

    /* interpret 2nd argument as a Py_ssize_t; clip on overflow */
    Py_ssize_t ndigits = PyNumber_AsSsize_t(_ndigits, NULL);
    if (ndigits == -1 && PyErr_Occurred())
        throwCAPIException();

    /* nans, infinities and zeros round to themselves */
    if (!std::isfinite(x) || x == 0.0)
        return boxFloat(x);

/* Deal with extreme values for ndigits. For ndigits > NDIGITS_MAX, x
   always rounds to itself.  For ndigits < NDIGITS_MIN, x always
   rounds to +-0.0.  Here 0.30103 is an upper bound for log10(2). */
#define NDIGITS_MAX ((int)((DBL_MANT_DIG - DBL_MIN_EXP) * 0.30103))
#define NDIGITS_MIN (-(int)((DBL_MAX_EXP + 1) * 0.30103))
    if (ndigits > NDIGITS_MAX)
        /* return x */
        return boxFloat(x);
    else if (ndigits < NDIGITS_MIN)
        /* return 0.0, but with sign of x */
        return boxFloat(0.0 * x);
    else {
        /* finite x, and ndigits is not unreasonably large */
        /* _Py_double_round is defined in floatobject.c */
        Box* rtn = _Py_double_round(x, (int)ndigits);
        if (!rtn)
            throwCAPIException();
        return rtn;
    }
#undef NDIGITS_MAX
#undef NDIGITS_MIN
}

Box* builtinCmp(Box* a, Box* b) {
    int c;
    if (PyObject_Cmp(a, b, &c) < 0)
        throwCAPIException();
    return PyInt_FromLong((long)c);
}

Box* builtinApply(Box* func, Box* _args, Box* keywords) {
    Box* args;
    if (!PyTuple_Check(_args)) {
        if (!PySequence_Check(_args))
            raiseExcHelper(TypeError, "apply() arg 2 expected sequence, found %s", getTypeName(_args));
        args = PySequence_Tuple(_args);
        if (!args)
            throwCAPIException();
    } else {
        args = incref(_args);
    }

    AUTO_DECREF(args);

    if (keywords && !PyDict_Check(keywords))
        raiseExcHelper(TypeError, "apply() arg 3 expected dictionary, found %s", getTypeName(keywords));
    return runtimeCall(func, ArgPassSpec(0, 0, true, keywords != NULL), args, keywords, NULL, NULL, NULL);
}

Box* builtinFormat(Box* value, Box* format_spec) {
    Box* res = PyObject_Format(value, format_spec);
    if (!res) {
        throwCAPIException();
    }
    return res;
}

static PyObject* builtin_compile(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    char* str;
    char* filename;
    char* startstr;
    int mode = -1;
    int dont_inherit = 0;
    int supplied_flags = 0;
    int is_ast;
    PyCompilerFlags cf;
    PyObject* result = NULL, *cmd, * tmp = NULL;
    Py_ssize_t length;
    static const char* kwlist[] = { "source", "filename", "mode", "flags", "dont_inherit", NULL };
    int start[] = { Py_file_input, Py_eval_input, Py_single_input };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Oss|ii:compile", const_cast<char**>(kwlist), &cmd, &filename,
                                     &startstr, &supplied_flags, &dont_inherit))
        return NULL;

    cf.cf_flags = supplied_flags;

    if (supplied_flags & ~(PyCF_MASK | PyCF_MASK_OBSOLETE | PyCF_DONT_IMPLY_DEDENT | PyCF_ONLY_AST)) {
        PyErr_SetString(PyExc_ValueError, "compile(): unrecognised flags");
        return NULL;
    }
    /* XXX Warn if (supplied_flags & PyCF_MASK_OBSOLETE) != 0? */

    if (!dont_inherit) {
        PyEval_MergeCompilerFlags(&cf);
    }

    if (strcmp(startstr, "exec") == 0)
        mode = 0;
    else if (strcmp(startstr, "eval") == 0)
        mode = 1;
    else if (strcmp(startstr, "single") == 0)
        mode = 2;
    else {
        PyErr_SetString(PyExc_ValueError, "compile() arg 3 must be 'exec', 'eval' or 'single'");
        return NULL;
    }

    is_ast = PyAST_Check(cmd);
    if (is_ast == -1)
        return NULL;
    if (is_ast) {
        if (supplied_flags & PyCF_ONLY_AST) {
            Py_INCREF(cmd);
            result = cmd;
        } else {
            PyArena* arena;
            mod_ty mod;

            arena = PyArena_New();
            if (arena == NULL)
                return NULL;
            mod = PyAST_obj2mod(cmd, arena, mode);
            if (mod == NULL) {
                PyArena_Free(arena);
                return NULL;
            }
            result = (PyObject*)PyAST_Compile(mod, filename, &cf, arena);
            PyArena_Free(arena);
        }
        return result;
    }

#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(cmd)) {
        tmp = PyUnicode_AsUTF8String(cmd);
        if (tmp == NULL)
            return NULL;
        cmd = tmp;
        cf.cf_flags |= PyCF_SOURCE_IS_UTF8;
    }
#endif

    if (PyObject_AsReadBuffer(cmd, (const void**)&str, &length))
        goto cleanup;
    if ((size_t)length != strlen(str)) {
        PyErr_SetString(PyExc_TypeError, "compile() expected string without null bytes");
        goto cleanup;
    }
    result = Py_CompileStringFlags(str, filename, start[mode], &cf);
cleanup:
    Py_XDECREF(tmp);
    return result;
}

static PyObject* builtin_eval(PyObject* self, PyObject* args) noexcept {
    PyObject* cmd, *result, * tmp = NULL;
    PyObject* globals = Py_None, * locals = Py_None;
    char* str;
    PyCompilerFlags cf;

    if (!PyArg_UnpackTuple(args, "eval", 1, 3, &cmd, &globals, &locals))
        return NULL;
    if (locals != Py_None && !PyMapping_Check(locals)) {
        PyErr_SetString(PyExc_TypeError, "locals must be a mapping");
        return NULL;
    }
    // Pyston change:
    // if (globals != Py_None && !PyDict_Check(globals)) {
    if (globals != Py_None && !PyDict_Check(globals) && globals->cls != attrwrapper_cls) {
        PyErr_SetString(PyExc_TypeError, PyMapping_Check(globals)
                                             ? "globals must be a real dict; try eval(expr, {}, mapping)"
                                             : "globals must be a dict");
        return NULL;
    }
    if (globals == Py_None) {
        globals = PyEval_GetGlobals();
        if (locals == Py_None)
            locals = PyEval_GetLocals();
    } else if (locals == Py_None)
        locals = globals;

    if (globals == NULL || locals == NULL) {
        PyErr_SetString(PyExc_TypeError, "eval must be given globals and locals "
                                         "when called without a frame");
        return NULL;
    }

    if (PyDict_GetItemString(globals, "__builtins__") == NULL) {
        if (PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) != 0)
            return NULL;
    }

    if (PyCode_Check(cmd)) {
// Pyston change:
#if 0
        if (PyCode_GetNumFree((PyCodeObject *)cmd) > 0) {
            PyErr_SetString(PyExc_TypeError,
        "code object passed to eval() may not contain free variables");
            return NULL;
        }
#endif
        return PyEval_EvalCode((PyCodeObject*)cmd, globals, locals);
    }

    if (!PyString_Check(cmd) && !PyUnicode_Check(cmd)) {
        PyErr_SetString(PyExc_TypeError, "eval() arg 1 must be a string or code object");
        return NULL;
    }
    cf.cf_flags = 0;

#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(cmd)) {
        tmp = PyUnicode_AsUTF8String(cmd);
        if (tmp == NULL)
            return NULL;
        cmd = tmp;
        cf.cf_flags |= PyCF_SOURCE_IS_UTF8;
    }
#endif
    if (PyString_AsStringAndSize(cmd, &str, NULL)) {
        Py_XDECREF(tmp);
        return NULL;
    }
    while (*str == ' ' || *str == '\t')
        str++;

    (void)PyEval_MergeCompilerFlags(&cf);
    result = PyRun_StringFlags(str, Py_eval_input, globals, locals, &cf);
    Py_XDECREF(tmp);
    return result;
}

static PyObject* builtin_execfile(PyObject* self, PyObject* args) noexcept {
    char* filename;
    PyObject* globals = Py_None, * locals = Py_None;
    PyObject* res;
    FILE* fp = NULL;
    PyCompilerFlags cf;
    int exists;

    if (PyErr_WarnPy3k("execfile() not supported in 3.x; use exec()", 1) < 0)
        return NULL;

    // Pyston change: allow attrwrappers here
    if (!PyArg_ParseTuple(args, "s|OO:execfile", &filename, &globals, &locals))
        return NULL;
    if (locals != Py_None && !PyMapping_Check(locals)) {
        PyErr_SetString(PyExc_TypeError, "locals must be a mapping");
        return NULL;
    }
    if (globals == Py_None) {
        globals = PyEval_GetGlobals();
        if (locals == Py_None)
            locals = PyEval_GetLocals();
    } else if (locals == Py_None)
        locals = globals;

    if (!PyDict_CheckExact(globals) && globals->cls != attrwrapper_cls) {
        PyErr_Format(TypeError, "execfile() globals must be dict, not %s", globals->cls->tp_name);
        return NULL;
    }

    if (PyDict_GetItemString(globals, "__builtins__") == NULL) {
        if (PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) != 0)
            return NULL;
    }

    exists = 0;
/* Test for existence or directory. */
#if defined(PLAN9)
    {
        Dir* d;

        if ((d = dirstat(filename)) != nil) {
            if (d->mode & DMDIR)
                werrstr("is a directory");
            else
                exists = 1;
            free(d);
        }
    }
#elif defined(RISCOS)
    if (object_exists(filename)) {
        if (isdir(filename))
            errno = EISDIR;
        else
            exists = 1;
    }
#else /* standard Posix */
    {
        struct stat s;
        if (stat(filename, &s) == 0) {
            if (S_ISDIR(s.st_mode))
#if defined(PYOS_OS2) && defined(PYCC_VACPP)
                errno = EOS2ERR;
#else
                errno = EISDIR;
#endif
            else
                exists = 1;
        }
    }
#endif

    if (exists) {
        Py_BEGIN_ALLOW_THREADS fp = fopen(filename, "r" PY_STDIOTEXTMODE);
        Py_END_ALLOW_THREADS

            if (fp == NULL) {
            exists = 0;
        }
    }

    if (!exists) {
        PyErr_SetFromErrnoWithFilename(PyExc_IOError, filename);
        return NULL;
    }
    cf.cf_flags = 0;
    if (PyEval_MergeCompilerFlags(&cf))
        res = PyRun_FileExFlags(fp, filename, Py_file_input, globals, locals, 1, &cf);
    else
        res = PyRun_FileEx(fp, filename, Py_file_input, globals, locals, 1);
    return res;
}

extern "C" {
BoxedClass* ellipsis_cls;
}

PyDoc_STRVAR(print_doc, "print(value, ..., sep=' ', end='\\n', file=sys.stdout)\n\
\n\
Prints the values to a stream, or to sys.stdout by default.\n\
Optional keyword arguments:\n\
file: a file-like object (stream); defaults to the current sys.stdout.\n\
sep:  string inserted between values, default a space.\n\
end:  string appended after the last value, default a newline.");

PyDoc_STRVAR(range_doc, "range(stop) -> list of integers\n\
range(start, stop[, step]) -> list of integers\n\
\n\
Return a list containing an arithmetic progression of integers.\n\
range(i, j) returns [i, i+1, i+2, ..., j-1]; start (!) defaults to 0.\n\
When step is given, it specifies the increment (or decrement).\n\
For example, range(4) returns [0, 1, 2, 3].  The end point is omitted!\n\
These are exactly the valid indices for a list of 4 elements.");

PyDoc_STRVAR(raw_input_doc, "raw_input([prompt]) -> string\n\
\n\
Read a string from standard input.  The trailing newline is stripped.\n\
If the user hits EOF (Unix: Ctl-D, Windows: Ctl-Z+Return), raise EOFError.\n\
On Unix, GNU readline is used if enabled.  The prompt string, if given,\n\
is printed without a trailing newline before reading.");

PyDoc_STRVAR(reduce_doc, "reduce(function, sequence[, initial]) -> value\n\
\n\
Apply a function of two arguments cumulatively to the items of a sequence,\n\
from left to right, so as to reduce the sequence to a single value.\n\
For example, reduce(lambda x, y: x+y, [1, 2, 3, 4, 5]) calculates\n\
((((1+2)+3)+4)+5).  If initial is present, it is placed before the items\n\
of the sequence in the calculation, and serves as a default when the\n\
sequence is empty.");

PyDoc_STRVAR(reload_doc, "reload(module) -> module\n\
\n\
Reload the module.  The module must have been successfully imported before.");

PyDoc_STRVAR(repr_doc, "repr(object) -> string\n\
\n\
Return the canonical string representation of the object.\n\
For most object types, eval(repr(object)) == object.");

PyDoc_STRVAR(round_doc, "round(number[, ndigits]) -> floating point number\n\
\n\
Round a number to a given precision in decimal digits (default 0 digits).\n\
This always returns a floating point number.  Precision may be negative.");

PyDoc_STRVAR(sorted_doc, "sorted(iterable, cmp=None, key=None, reverse=False) --> new sorted list");

PyDoc_STRVAR(vars_doc, "vars([object]) -> dictionary\n\
\n\
Without arguments, equivalent to locals().\n\
With an argument, equivalent to object.__dict__.");

PyDoc_STRVAR(sum_doc, "sum(sequence[, start]) -> value\n\
\n\
Return the sum of a sequence of numbers (NOT strings) plus the value\n\
of parameter 'start' (which defaults to 0).  When the sequence is\n\
empty, return start.");

PyDoc_STRVAR(isinstance_doc, "isinstance(object, class-or-type-or-tuple) -> bool\n\
\n\
Return whether an object is an instance of a class or of a subclass thereof.\n\
With a type as second argument, return whether that is the object's type.\n\
The form using a tuple, isinstance(x, (A, B, ...)), is a shortcut for\n\
isinstance(x, A) or isinstance(x, B) or ... (etc.).");

PyDoc_STRVAR(issubclass_doc, "issubclass(C, B) -> bool\n\
\n\
Return whether class C is a subclass (i.e., a derived class) of class B.\n\
When using a tuple as the second argument issubclass(X, (A, B, ...)),\n\
is a shortcut for issubclass(X, A) or issubclass(X, B) or ... (etc.).");

PyDoc_STRVAR(zip_doc, "zip(seq1 [, seq2 [...]]) -> [(seq1[0], seq2[0] ...), (...)]\n\
\n\
Return a list of tuples, where each tuple contains the i-th element\n\
from each of the argument sequences.  The returned list is truncated\n\
in length to the length of the shortest argument sequence.");

PyDoc_STRVAR(builtin_doc, "Built-in functions, exceptions, and other objects.\n\
\n\
Noteworthy: None is the `nil' object; Ellipsis represents `...' in slices.");

PyDoc_STRVAR(import_doc, "__import__(name, globals={}, locals={}, fromlist=[], level=-1) -> module\n\
\n\
Import a module. Because this function is meant for use by the Python\n\
interpreter and not for general use it is better to use\n\
importlib.import_module() to programmatically import a module.\n\
\n\
The globals argument is only used to determine the context;\n\
they are not modified.  The locals argument is unused.  The fromlist\n\
should be a list of names to emulate ``from name import ...'', or an\n\
empty list to emulate ``import name''.\n\
When importing a module from a package, note that __import__('A.B', ...)\n\
returns package A when fromlist is empty, but its submodule B when\n\
fromlist is not empty.  Level is used to determine whether to perform \n\
absolute or relative imports.  -1 is the original strategy of attempting\n\
both absolute and relative imports, 0 is absolute, a positive number\n\
is the number of parent directories to search relative to the current module.");

PyDoc_STRVAR(abs_doc, "abs(number) -> number\n\
\n\
Return the absolute value of the argument.");

PyDoc_STRVAR(all_doc, "all(iterable) -> bool\n\
\n\
Return True if bool(x) is True for all values x in the iterable.\n\
If the iterable is empty, Py_RETURN_TRUE.");

PyDoc_STRVAR(any_doc, "any(iterable) -> bool\n\
\n\
Return True if bool(x) is True for any x in the iterable.\n\
If the iterable is empty, Py_RETURN_FALSE.");

PyDoc_STRVAR(apply_doc, "apply(object[, args[, kwargs]]) -> value\n\
\n\
    Call a callable object with positional arguments taken from the tuple args,\n\
    and keyword arguments taken from the optional dictionary kwargs.\n\
    Note that classes are callable, as are instances with a __call__() method.\n\
\n\
    Deprecated since release 2.3. Instead, use the extended call syntax:\n\
        function(*args, **keywords).");

PyDoc_STRVAR(bin_doc, "bin(number) -> string\n\
\n\
Return the binary representation of an integer or long integer.");

PyDoc_STRVAR(callable_doc, "callable(object) -> bool\n\
\n\
Return whether the object is callable (i.e., some kind of function).\n\
Note that classes are callable, as are instances with a __call__() method.");

PyDoc_STRVAR(filter_doc, "filter(function or None, sequence) -> list, tuple, or string\n"
                         "\n"
                         "Return those items of sequence for which function(item) is true.  If\n"
                         "function is None, return the items that are true.  If sequence is a tuple\n"
                         "or string, return the same type, else return a list.");

PyDoc_STRVAR(format_doc, "format(value[, format_spec]) -> string\n\
\n\
Returns value.__format__(format_spec)\n\
format_spec defaults to \"\"");

PyDoc_STRVAR(chr_doc, "chr(i) -> character\n\
\n\
Return a string of one character with ordinal i; 0 <= i < 256.");

PyDoc_STRVAR(unichr_doc, "unichr(i) -> Unicode character\n\
\n\
Return a Unicode string of one character with ordinal i; 0 <= i <= 0x10ffff.");

PyDoc_STRVAR(cmp_doc, "cmp(x, y) -> integer\n\
\n\
Return negative if x<y, zero if x==y, positive if x>y.");

PyDoc_STRVAR(coerce_doc, "coerce(x, y) -> (x1, y1)\n\
\n\
Return a tuple consisting of the two numeric arguments converted to\n\
a common type, using the same rules as used by arithmetic operations.\n\
If coercion is not possible, raise TypeError.");

PyDoc_STRVAR(compile_doc, "compile(source, filename, mode[, flags[, dont_inherit]]) -> code object\n\
\n\
Compile the source string (a Python module, statement or expression)\n\
into a code object that can be executed by the exec statement or eval().\n\
The filename will be used for run-time error messages.\n\
The mode must be 'exec' to compile a module, 'single' to compile a\n\
single (interactive) statement, or 'eval' to compile an expression.\n\
The flags argument, if present, controls which future statements influence\n\
the compilation of the code.\n\
The dont_inherit argument, if non-zero, stops the compilation inheriting\n\
the effects of any future statements in effect in the code calling\n\
compile; if absent or zero these statements do influence the compilation,\n\
in addition to any features explicitly specified.");

PyDoc_STRVAR(dir_doc, "dir([object]) -> list of strings\n"
                      "\n"
                      "If called without an argument, return the names in the current scope.\n"
                      "Else, return an alphabetized list of names comprising (some of) the attributes\n"
                      "of the given object, and of attributes reachable from it.\n"
                      "If the object supplies a method named __dir__, it will be used; otherwise\n"
                      "the default dir() logic is used and returns:\n"
                      "  for a module object: the module's attributes.\n"
                      "  for a class object:  its attributes, and recursively the attributes\n"
                      "    of its bases.\n"
                      "  for any other object: its attributes, its class's attributes, and\n"
                      "    recursively the attributes of its class's base classes.");

PyDoc_STRVAR(divmod_doc, "divmod(x, y) -> (quotient, remainder)\n\
\n\
Return the tuple ((x-x%y)/y, x%y).  Invariant: div*y + mod == x.");

PyDoc_STRVAR(eval_doc, "eval(source[, globals[, locals]]) -> value\n\
\n\
Evaluate the source in the context of globals and locals.\n\
The source may be a string representing a Python expression\n\
or a code object as returned by compile().\n\
The globals must be a dictionary and locals can be any mapping,\n\
defaulting to the current globals and locals.\n\
If only globals is given, locals defaults to it.\n");

PyDoc_STRVAR(execfile_doc, "execfile(filename[, globals[, locals]])\n\
\n\
Read and execute a Python script from a file.\n\
The globals and locals are dictionaries, defaulting to the current\n\
globals and locals.  If only globals is given, locals defaults to it.");

PyDoc_STRVAR(getattr_doc, "getattr(object, name[, default]) -> value\n\
\n\
Get a named attribute from an object; getattr(x, 'y') is equivalent to x.y.\n\
When a default argument is given, it is returned when the attribute doesn't\n\
exist; without it, an exception is raised in that case.");

PyDoc_STRVAR(globals_doc, "globals() -> dictionary\n\
\n\
Return the dictionary containing the current scope's global variables.");

PyDoc_STRVAR(hasattr_doc, "hasattr(object, name) -> bool\n\
\n\
Return whether the object has an attribute with the given name.\n\
(This is done by calling getattr(object, name) and catching exceptions.)");

PyDoc_STRVAR(id_doc, "id(object) -> integer\n\
\n\
Return the identity of an object.  This is guaranteed to be unique among\n\
simultaneously existing objects.  (Hint: it's the object's memory address.)");

PyDoc_STRVAR(map_doc, "map(function, sequence[, sequence, ...]) -> list\n\
\n\
Return a list of the results of applying the function to the items of\n\
the argument sequence(s).  If more than one sequence is given, the\n\
function is called with an argument list consisting of the corresponding\n\
item of each sequence, substituting None for missing values when not all\n\
sequences have the same length.  If the function is None, return a list of\n\
the items of the sequence (or a list of tuples if more than one sequence).");

PyDoc_STRVAR(next_doc, "next(iterator[, default])\n\
\n\
Return the next item from the iterator. If default is given and the iterator\n\
is exhausted, it is returned instead of raising StopIteration.");

PyDoc_STRVAR(setattr_doc, "setattr(object, name, value)\n\
\n\
Set a named attribute on an object; setattr(x, 'y', v) is equivalent to\n\
``x.y = v''.");

PyDoc_STRVAR(delattr_doc, "delattr(object, name)\n\
\n\
Delete a named attribute on an object; delattr(x, 'y') is equivalent to\n\
``del x.y''.");

PyDoc_STRVAR(hash_doc, "hash(object) -> integer\n\
\n\
Return a hash value for the object.  Two objects with the same value have\n\
the same hash value.  The reverse is not necessarily true, but likely.");

PyDoc_STRVAR(hex_doc, "hex(number) -> string\n\
\n\
Return the hexadecimal representation of an integer or long integer.");

PyDoc_STRVAR(input_doc, "input([prompt]) -> value\n\
\n\
Equivalent to eval(raw_input(prompt)).");

PyDoc_STRVAR(intern_doc, "intern(string) -> string\n\
\n\
``Intern'' the given string.  This enters the string in the (global)\n\
table of interned strings whose purpose is to speed up dictionary lookups.\n\
Return the string itself or the previously interned string object with the\n\
same value.");

PyDoc_STRVAR(iter_doc, "iter(collection) -> iterator\n\
iter(callable, sentinel) -> iterator\n\
\n\
Get an iterator from an object.  In the first form, the argument must\n\
supply its own iterator, or be a sequence.\n\
In the second form, the callable is called until it returns the sentinel.");

PyDoc_STRVAR(len_doc, "len(object) -> integer\n\
\n\
Return the number of items of a sequence or mapping.");

PyDoc_STRVAR(locals_doc, "locals() -> dictionary\n\
\n\
Update and return a dictionary containing the current scope's local variables.");

PyDoc_STRVAR(min_doc, "min(iterable[, key=func]) -> value\n\
min(a, b, c, ...[, key=func]) -> value\n\
\n\
With a single iterable argument, return its smallest item.\n\
With two or more arguments, return the smallest argument.");

PyDoc_STRVAR(max_doc, "max(iterable[, key=func]) -> value\n\
max(a, b, c, ...[, key=func]) -> value\n\
\n\
With a single iterable argument, return its largest item.\n\
With two or more arguments, return the largest argument.");

PyDoc_STRVAR(oct_doc, "oct(number) -> string\n\
\n\
Return the octal representation of an integer or long integer.");

PyDoc_STRVAR(open_doc, "open(name[, mode[, buffering]]) -> file object\n\
\n\
Open a file using the file() type, returns a file object.  This is the\n\
preferred way to open a file.  See file.__doc__ for further information.");

PyDoc_STRVAR(ord_doc, "ord(c) -> integer\n\
\n\
Return the integer ordinal of a one-character string.");

PyDoc_STRVAR(pow_doc, "pow(x, y[, z]) -> number\n\
\n\
With two arguments, equivalent to x**y.  With three arguments,\n\
equivalent to (x**y) % z, but may be more efficient (e.g. for longs).");

static Box* lenCallInternalCapi(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                                Box* arg2, Box* arg3, Box** args,
                                const std::vector<BoxedString*>* keyword_names) noexcept {
    try {
        return lenCallInternal(func, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

void setupBuiltins() {
    builtins_module = createModule(autoDecref(boxString("__builtin__")), NULL,
                                   "Built-in functions, exceptions, and other objects.\n\nNoteworthy: None is "
                                   "the `nil' object; Ellipsis represents `...' in slices.");
    PyThreadState_GET()->interp->builtins = incref(builtins_module->getAttrWrapper());

    ellipsis_cls
        = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(Box), false, "ellipsis", false, NULL, NULL, false);
    ellipsis_cls->giveAttr("__repr__",
                           new BoxedFunction(BoxedCode::create((void*)ellipsisRepr, STR, 1, "ellipsis.__repr__")));
    Ellipsis = new (ellipsis_cls) Box();
    assert(Ellipsis->cls);

    constants.push_back(Ellipsis);
    builtins_module->giveAttrBorrowed("Ellipsis", Ellipsis);
    builtins_module->giveAttrBorrowed("None", Py_None);

    builtins_module->giveAttrBorrowed("__debug__", Py_True);

    notimplemented_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(Box), false, "NotImplementedType", false,
                                            NULL, NULL, false);
    notimplemented_cls->giveAttr(
        "__repr__", new BoxedFunction(BoxedCode::create((void*)notimplementedRepr, STR, 1, "notimplemented.__repr__")));
    notimplemented_cls->freeze();
    notimplemented_cls->instances_are_nonzero = true;
    NotImplemented = new (notimplemented_cls) Box();

    constants.push_back(NotImplemented);
    builtins_module->giveAttrBorrowed("NotImplemented", NotImplemented);

    builtins_module->giveAttr(
        "all", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)all, BOXED_BOOL, 1, "all", all_doc)));
    builtins_module->giveAttr(
        "any", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)any, BOXED_BOOL, 1, "any", any_doc)));

    builtins_module->giveAttr(
        "apply",
        new BoxedBuiltinFunctionOrMethod(
            BoxedCode::create((void*)builtinApply, UNKNOWN, 3, false, false, "apply", apply_doc), { NULL }, NULL));

    repr_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)repr, UNKNOWN, 1, "repr", repr_doc));
    builtins_module->giveAttr("repr", repr_obj);

    auto len_func = BoxedCode::create((void*)len, UNKNOWN, 1, "len", len_doc);
    len_func->internal_callable.capi_val = lenCallInternalCapi;
    len_func->internal_callable.cxx_val = lenCallInternal;
    len_obj = new BoxedBuiltinFunctionOrMethod(len_func);
    builtins_module->giveAttr("len", len_obj);

    hash_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)hash, UNKNOWN, 1, "hash", hash_doc));
    builtins_module->giveAttr("hash", hash_obj);
    abs_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)abs_, UNKNOWN, 1, "abs", abs_doc));
    builtins_module->giveAttr("abs", abs_obj);

    min_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)min, UNKNOWN, 1, true, true, "min", min_doc),
                                               { Py_None }, NULL);
    builtins_module->giveAttr("min", min_obj);

    max_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)max, UNKNOWN, 1, true, true, "max", max_doc),
                                               { Py_None }, NULL);
    builtins_module->giveAttr("max", max_obj);

    builtins_module->giveAttr(
        "next", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)next, UNKNOWN, 2, false, false, "next",
                                                                   next_doc, ParamNames::empty(), CAPI),
                                                 { NULL }, NULL));

    builtins_module->giveAttr(
        "sum", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)sum, UNKNOWN, 2, false, false, "sum", sum_doc),
                                                { autoDecref(boxInt(0)) }, NULL));

    id_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)id, BOXED_INT, 1, "id", id_doc));
    builtins_module->giveAttr("id", id_obj);
    chr_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)chr, STR, 1, "chr", chr_doc));
    builtins_module->giveAttr("chr", chr_obj);
    builtins_module->giveAttr(
        "unichr", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)unichr, UNKNOWN, 1, "unichr", unichr_doc)));
    ord_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)ord, BOXED_INT, 1, "ord", ord_doc));
    builtins_module->giveAttr("ord", ord_obj);
    trap_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)trap, UNKNOWN, 0, "trap"));
    builtins_module->giveAttr("trap", trap_obj);
    builtins_module->giveAttr("dump",
                              new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)pydump, UNKNOWN, 2, "dump"),
                                                               { autoDecref(boxInt(0)) }));
    builtins_module->giveAttr(
        "dumpAddr", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)pydumpAddr, UNKNOWN, 1, "dumpAddr")));

    builtins_module->giveAttr("delattr", new BoxedBuiltinFunctionOrMethod(
                                             BoxedCode::create((void*)delattrFunc, NONE, 2, "delattr", delattr_doc)));

    auto getattr_func = new BoxedCode(3, true, true, "getattr", getattr_doc, ParamNames::empty());
    getattr_func->internal_callable.capi_val = &getattrFuncInternal<CAPI>;
    getattr_func->internal_callable.cxx_val = &getattrFuncInternal<CXX>;
    builtins_module->giveAttr("getattr", new BoxedBuiltinFunctionOrMethod(getattr_func, { NULL }, NULL));

    builtins_module->giveAttr("setattr", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                             (void*)setattrFunc, UNKNOWN, 3, false, false, "setattr", setattr_doc)));

    auto hasattr_func = new BoxedCode(2, false, false, "hasattr", hasattr_doc);
    hasattr_func->internal_callable.capi_val = &hasattrFuncInternal<CAPI>;
    hasattr_func->internal_callable.cxx_val = &hasattrFuncInternal<CXX>;
    builtins_module->giveAttr("hasattr", new BoxedBuiltinFunctionOrMethod(hasattr_func));

    builtins_module->giveAttr(
        "pow", new BoxedBuiltinFunctionOrMethod(
                   BoxedCode::create((void*)powFunc, UNKNOWN, 3, false, false, "pow", pow_doc), { Py_None }, NULL));

    Box* isinstance_obj = new BoxedBuiltinFunctionOrMethod(
        BoxedCode::create((void*)isinstance_func, BOXED_BOOL, 2, "isinstance", isinstance_doc));
    builtins_module->giveAttr("isinstance", isinstance_obj);

    Box* issubclass_obj = new BoxedBuiltinFunctionOrMethod(
        BoxedCode::create((void*)issubclass_func, BOXED_BOOL, 2, "issubclass", issubclass_doc));
    builtins_module->giveAttr("issubclass", issubclass_obj);

    Box* intern_obj
        = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)intern_func, UNKNOWN, 1, "intern", intern_doc));
    builtins_module->giveAttr("intern", intern_obj);

    BoxedCode* import_func
        = BoxedCode::create((void*)bltinImport, UNKNOWN, 5, false, false, "__import__", import_doc,
                            ParamNames({ "name", "globals", "locals", "fromlist", "level" }, "", ""));
    builtins_module->giveAttr("__import__", new BoxedBuiltinFunctionOrMethod(
                                                import_func, { NULL, NULL, NULL, autoDecref(boxInt(-1)) }, NULL));

    enumerate_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedEnumerate), false, "enumerate", true,
                                       BoxedEnumerate::dealloc, NULL, true, BoxedEnumerate::traverse, NOCLEAR);
    enumerate_cls->giveAttr(
        "__new__", new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::new_, UNKNOWN, 3, "enumerate.__new__", "",
                                                       ParamNames({ "", "sequence", "start" }, "", "")),
                                     { autoDecref(boxInt(0)) }));
    enumerate_cls->giveAttr(
        "__iter__", new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::iter, typeFromClass(enumerate_cls), 1,
                                                        "enumerate.__iter__")));
    enumerate_cls->giveAttr(
        "next", new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::next, BOXED_TUPLE, 1, "enumerate.next")));
    enumerate_cls->giveAttr(
        "__hasnext__",
        new BoxedFunction(BoxedCode::create((void*)BoxedEnumerate::hasnext, BOXED_BOOL, 1, "enumerate.__hasnext__")));
    enumerate_cls->freeze();
    enumerate_cls->tp_iter = PyObject_SelfIter;
    builtins_module->giveAttrBorrowed("enumerate", enumerate_cls);

    builtins_module->giveAttrBorrowed("True", Py_True);
    builtins_module->giveAttrBorrowed("False", Py_False);

    range_obj = new BoxedBuiltinFunctionOrMethod(
        BoxedCode::create((void*)range, LIST, 3, false, false, "range", range_doc), { NULL, NULL }, NULL);
    builtins_module->giveAttr("range", range_obj);

    auto* round_obj
        = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)builtinRound, BOXED_FLOAT, 2, "round", round_doc,
                                                             ParamNames({ "number", "ndigits" }, "", "")),
                                           { autoDecref(boxInt(0)) }, NULL);
    builtins_module->giveAttr("round", round_obj);

    setupXrange();
    builtins_module->giveAttrBorrowed("xrange", xrange_cls);

    open_obj = new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)open, typeFromClass(&PyFile_Type), 3, false,
                                                                  false, "open", open_doc,
                                                                  ParamNames({ "name", "mode", "buffering" }, "", "")),
                                                { autoDecref(boxString("r")), autoDecref(boxInt(-1)) }, NULL);
    builtins_module->giveAttr("open", open_obj);

    builtins_module->giveAttr("globals", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                             (void*)globals, UNKNOWN, 0, false, false, "globals", globals_doc)));
    builtins_module->giveAttr("locals", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                            (void*)locals, UNKNOWN, 0, false, false, "locals", locals_doc)));

    builtins_module->giveAttr(
        "iter", new BoxedBuiltinFunctionOrMethod(
                    BoxedCode::create((void*)builtinIter, UNKNOWN, 2, false, false, "iter", iter_doc), { NULL }, NULL));
    builtins_module->giveAttr("reversed", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                              (void*)getreversed, UNKNOWN, 1, false, false, "reversed")));
    builtins_module->giveAttr("coerce", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                            (void*)coerceFunc, UNKNOWN, 2, false, false, "coerce", coerce_doc)));
    builtins_module->giveAttr(
        "divmod", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)divmod, UNKNOWN, 2, "divmod", divmod_doc)));

    builtins_module->giveAttr(
        "map", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)map, LIST, 1, true, false, "map", map_doc)));
    builtins_module->giveAttr(
        "reduce",
        new BoxedBuiltinFunctionOrMethod(
            BoxedCode::create((void*)reduce, UNKNOWN, 3, false, false, "reduce", reduce_doc), { NULL }, NULL));
    builtins_module->giveAttr("filter", new BoxedBuiltinFunctionOrMethod(
                                            BoxedCode::create((void*)filter2, UNKNOWN, 2, "filter", filter_doc)));
    builtins_module->giveAttr(
        "zip", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)zip, LIST, 0, true, false, "zip", zip_doc)));
    builtins_module->giveAttr(
        "dir", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)dir, LIST, 1, false, false, "dir", dir_doc),
                                                { NULL }, NULL));
    builtins_module->giveAttr(
        "vars", new BoxedBuiltinFunctionOrMethod(
                    BoxedCode::create((void*)vars, UNKNOWN, 1, false, false, "vars", vars_doc), { NULL }, NULL));
    builtins_module->giveAttrBorrowed("object", object_cls);
    builtins_module->giveAttrBorrowed("str", str_cls);
    builtins_module->giveAttrBorrowed("bytes", str_cls);
    assert(unicode_cls);
    builtins_module->giveAttrBorrowed("unicode", unicode_cls);
    builtins_module->giveAttrBorrowed("basestring", basestring_cls);
    // builtins_module->giveAttr("unicode", unicode_cls);
    builtins_module->giveAttrBorrowed("int", int_cls);
    builtins_module->giveAttrBorrowed("long", long_cls);
    builtins_module->giveAttrBorrowed("float", float_cls);
    builtins_module->giveAttrBorrowed("list", list_cls);
    builtins_module->giveAttrBorrowed("slice", slice_cls);
    builtins_module->giveAttrBorrowed("type", type_cls);
    builtins_module->giveAttrBorrowed("file", &PyFile_Type);
    builtins_module->giveAttrBorrowed("bool", bool_cls);
    builtins_module->giveAttrBorrowed("dict", dict_cls);
    builtins_module->giveAttrBorrowed("set", set_cls);
    builtins_module->giveAttrBorrowed("frozenset", frozenset_cls);
    builtins_module->giveAttrBorrowed("tuple", tuple_cls);
    builtins_module->giveAttrBorrowed("complex", complex_cls);
    builtins_module->giveAttrBorrowed("super", super_cls);
    builtins_module->giveAttrBorrowed("property", property_cls);
    builtins_module->giveAttrBorrowed("staticmethod", staticmethod_cls);
    builtins_module->giveAttrBorrowed("classmethod", classmethod_cls);

    assert(memoryview_cls);
    Py_TYPE(&PyMemoryView_Type) = &PyType_Type;
    PyType_Ready(&PyMemoryView_Type);
    builtins_module->giveAttrBorrowed("memoryview", memoryview_cls);
    PyType_Ready(&PyByteArray_Type);
    builtins_module->giveAttrBorrowed("bytearray", &PyByteArray_Type);
    Py_TYPE(&PyBuffer_Type) = &PyType_Type;
    PyType_Ready(&PyBuffer_Type);
    builtins_module->giveAttrBorrowed("buffer", &PyBuffer_Type);

    builtins_module->giveAttr("callable", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                              (void*)callable, UNKNOWN, 1, "callable", callable_doc)));

    builtins_module->giveAttr(
        "raw_input",
        new BoxedBuiltinFunctionOrMethod(
            BoxedCode::create((void*)rawInput, UNKNOWN, 1, false, false, "raw_input", raw_input_doc), { NULL }, NULL));
    builtins_module->giveAttr(
        "input", new BoxedBuiltinFunctionOrMethod(
                     BoxedCode::create((void*)input, UNKNOWN, 1, false, false, "input", input_doc), { NULL }, NULL));
    builtins_module->giveAttr(
        "cmp", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)builtinCmp, UNKNOWN, 2, "cmp", cmp_doc)));
    builtins_module->giveAttr(
        "format", new BoxedBuiltinFunctionOrMethod(
                      BoxedCode::create((void*)builtinFormat, UNKNOWN, 2, "format", format_doc), { NULL }, NULL));


    static PyMethodDef builtin_methods[] = {
        { "bin", builtin_bin, METH_O, bin_doc },
        { "compile", (PyCFunction)builtin_compile, METH_VARARGS | METH_KEYWORDS, compile_doc },
        { "eval", builtin_eval, METH_VARARGS, eval_doc },
        { "execfile", builtin_execfile, METH_VARARGS, execfile_doc },
        { "hex", builtin_hex, METH_O, hex_doc },
        { "oct", builtin_oct, METH_O, oct_doc },
        { "print", (PyCFunction)builtin_print, METH_VARARGS | METH_KEYWORDS, print_doc },
        { "reload", builtin_reload, METH_O, reload_doc },
        { "sorted", (PyCFunction)builtin_sorted, METH_VARARGS | METH_KEYWORDS, sorted_doc },
    };
    for (auto& md : builtin_methods) {
        builtins_module->giveAttr(md.ml_name, new BoxedCApiFunction(&md, NULL, autoDecref(boxString("__builtin__"))));
    }
}
}
