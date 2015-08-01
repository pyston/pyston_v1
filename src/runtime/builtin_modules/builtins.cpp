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

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <err.h>

#include "llvm/Support/FileSystem.h"

#include "capi/typeobject.h"
#include "codegen/ast_interpreter.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/classobj.h"
#include "runtime/file.h"
#include "runtime/ics.h"
#include "runtime/import.h"
#include "runtime/inline/list.h"
#include "runtime/inline/xrange.h"
#include "runtime/iterobject.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
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

    return None;
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
        return fastLocalsToBoxedLocals();

    return obj->getAttrWrapper();
}

extern "C" Box* abs_(Box* x) {
    if (isSubclass(x->cls, int_cls)) {
        i64 n = static_cast<BoxedInt*>(x)->n;
        return boxInt(n >= 0 ? n : -n);
    } else if (x->cls == float_cls) {
        double d = static_cast<BoxedFloat*>(x)->d;
        return boxFloat(d >= 0 ? d : -d);
    } else if (x->cls == long_cls) {
        return longAbs(static_cast<BoxedLong*>(x));
    } else {
        static BoxedString* abs_str = internStringImmortal("__abs__");
        CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
        return callattr(x, abs_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    }
}

extern "C" Box* hexFunc(Box* x) {
    static BoxedString* hex_str = internStringImmortal("__hex__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(x, hex_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (!r)
        raiseExcHelper(TypeError, "hex() argument can't be converted to hex");

    if (!isSubclass(r->cls, str_cls))
        raiseExcHelper(TypeError, "__hex__() returned non-string (type %.200s)", r->cls->tp_name);

    return r;
}

extern "C" Box* octFunc(Box* x) {
    static BoxedString* oct_str = internStringImmortal("__oct__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(x, oct_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (!r)
        raiseExcHelper(TypeError, "oct() argument can't be converted to oct");

    if (!isSubclass(r->cls, str_cls))
        raiseExcHelper(TypeError, "__oct__() returned non-string (type %.200s)", r->cls->tp_name);

    return r;
}

extern "C" Box* all(Box* container) {
    for (Box* e : container->pyElements()) {
        if (!nonzero(e)) {
            return boxBool(false);
        }
    }
    return boxBool(true);
}

extern "C" Box* any(Box* container) {
    for (Box* e : container->pyElements()) {
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
        static BoxedString* key_str = static_cast<BoxedString*>(PyString_InternFromString("key"));
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

    if (args->size() == 0) {
        extremElement = nullptr;
        extremVal = nullptr;
        container = arg0;
    } else {
        extremElement = arg0;
        if (key_func != NULL) {
            extremVal = runtimeCall(key_func, ArgPassSpec(1), extremElement, NULL, NULL, NULL, NULL);
        } else {
            extremVal = extremElement;
        }
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
            curVal = runtimeCall(key_func, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
        } else {
            if (!extremElement) {
                extremVal = e;
                extremElement = e;
                continue;
            }
            curVal = e;
        }
        int r = PyObject_RichCompareBool(curVal, extremVal, opid);
        if (r == -1)
            throwCAPIException();
        if (r) {
            extremElement = e;
            extremVal = curVal;
        }
    }
    return extremElement;
}

extern "C" Box* min(Box* arg0, BoxedTuple* args, BoxedDict* kwargs) {
    if (arg0 == None && args->size() == 0) {
        raiseExcHelper(TypeError, "min expected 1 arguments, got 0");
    }

    Box* minElement = min_max(arg0, args, kwargs, Py_LT);

    if (!minElement) {
        raiseExcHelper(ValueError, "min() arg is an empty sequence");
    }
    return minElement;
}

extern "C" Box* max(Box* arg0, BoxedTuple* args, BoxedDict* kwargs) {
    if (arg0 == None && args->size() == 0) {
        raiseExcHelper(TypeError, "max expected 1 arguments, got 0");
    }

    Box* maxElement = min_max(arg0, args, kwargs, Py_GT);

    if (!maxElement) {
        raiseExcHelper(ValueError, "max() arg is an empty sequence");
    }
    return maxElement;
}

extern "C" Box* next(Box* iterator, Box* _default) {
    try {
        static BoxedString* next_str = internStringImmortal("next");
        CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
        return callattr(iterator, next_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        if (_default && e.matches(StopIteration))
            return _default;
        throw e;
    }
}

extern "C" Box* sum(Box* container, Box* initial) {
    if (initial->cls == str_cls)
        raiseExcHelper(TypeError, "sum() can't sum strings [use ''.join(seq) instead]");

    static RuntimeICCache<BinopIC, 3> runtime_ic_cache;
    std::shared_ptr<BinopIC> pp = runtime_ic_cache.getIC(__builtin_return_address(0));

    Box* cur = initial;
    for (Box* e : container->pyElements()) {
        cur = pp->call(cur, e, AST_TYPE::Add);
    }
    return cur;
}

extern "C" Box* id(Box* arg) {
    i64 addr = (i64)(arg) ^ 0xdeadbeef00000003;
    return boxInt(addr);
}


Box* open(Box* arg1, Box* arg2, Box* arg3) {
    assert(arg2);
    assert(arg3);
    // This could be optimized quite a bit if it ends up being important:
    return runtimeCall(file_cls, ArgPassSpec(3), arg1, arg2, arg3, NULL, NULL);
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
    if (arg->cls != int_cls)
        raiseExcHelper(TypeError, "an integer is required");

    i64 n = static_cast<BoxedInt*>(arg)->n;
    Box* rtn = PyUnicode_FromOrdinal(n);
    checkAndThrowCAPIException();
    return rtn;
}

Box* coerceFunc(Box* vv, Box* ww) {
    Box* res;

    if (PyErr_WarnPy3k("coerce() not supported in 3.x", 1) < 0)
        throwCAPIException();

    if (PyNumber_Coerce(&vv, &ww) < 0)
        throwCAPIException();
    res = PyTuple_Pack(2, vv, ww);
    return res;
}

extern "C" Box* ord(Box* obj) {
    long ord;
    Py_ssize_t size;

    if (PyString_Check(obj)) {
        size = PyString_GET_SIZE(obj);
        if (size == 1) {
            ord = (long)((unsigned char)*PyString_AS_STRING(obj));
            return new BoxedInt(ord);
        }
    } else if (PyByteArray_Check(obj)) {
        size = PyByteArray_GET_SIZE(obj);
        if (size == 1) {
            ord = (long)((unsigned char)*PyByteArray_AS_STRING(obj));
            return new BoxedInt(ord);
        }

#ifdef Py_USING_UNICODE
    } else if (PyUnicode_Check(obj)) {
        size = PyUnicode_GET_SIZE(obj);
        if (size == 1) {
            ord = (long)*PyUnicode_AS_UNICODE(obj);
            return new BoxedInt(ord);
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
        checkAndThrowCAPIException();
        istep = 1;
    } else if (step == NULL) {
        istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        istep = 1;
    } else {
        istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        istep = PyLong_AsLong(step);
        checkAndThrowCAPIException();
    }

    BoxedList* rtn = new BoxedList();
    rtn->ensure(std::max(0l, 1 + (istop - istart) / istep));
    if (istep > 0) {
        for (i64 i = istart; i < istop; i += istep) {
            Box* bi = boxInt(i);
            listAppendInternal(rtn, bi);
        }
    } else {
        for (i64 i = istart; i > istop; i += istep) {
            Box* bi = boxInt(i);
            listAppendInternal(rtn, bi);
        }
    }
    return rtn;
}

Box* notimplementedRepr(Box* self) {
    assert(self == NotImplemented);
    return boxString("NotImplemented");
}

Box* sorted(Box* obj, Box* cmp, Box* key, Box** args) {
    Box* reverse = args[0];

    BoxedList* rtn = new BoxedList();
    for (Box* e : obj->pyElements()) {
        listAppendInternal(rtn, e);
    }

    listSort(rtn, cmp, key, reverse);
    return rtn;
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
        checkAndThrowCAPIException();
    return boxBool(rtn);
}

Box* intern_func(Box* str) {
    if (!PyString_CheckExact(str)) // have to use exact check!
        raiseExcHelper(TypeError, "can't intern subclass of string");
    PyString_InternInPlace(&str);
    checkAndThrowCAPIException();
    return str;
}

Box* bltinImport(Box* name, Box* globals, Box* locals, Box** args) {
    Box* fromlist = args[0];
    Box* level = args[1];

    // __import__ takes a 'locals' argument, but it doesn't get used in CPython.
    // Well, it gets passed to PyImport_ImportModuleLevel() and then import_module_level(),
    // which ignores it.  So we don't even pass it through.

    name = coerceUnicodeToStr(name);

    if (name->cls != str_cls) {
        raiseExcHelper(TypeError, "__import__() argument 1 must be string, not %s", getTypeName(name));
    }

    if (level->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    std::string _name = static_cast<BoxedString*>(name)->s();
    return importModuleLevel(_name, globals, fromlist, ((BoxedInt*)level)->n);
}

Box* delattrFunc(Box* obj, Box* _str) {
    _str = coerceUnicodeToStr(_str);

    if (_str->cls != str_cls)
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(_str));
    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);

    delattr(obj, str);
    return None;
}

Box* getattrFunc(Box* obj, Box* _str, Box* default_value) {
    _str = coerceUnicodeToStr(_str);

    if (!isSubclass(_str->cls, str_cls)) {
        raiseExcHelper(TypeError, "getattr(): attribute name must be string");
    }

    Box* rtn = PyObject_GetAttr(obj, _str);
    if (rtn == NULL && default_value != NULL && PyErr_ExceptionMatches(AttributeError)) {
        PyErr_Clear();
        return default_value;
    }

    if (!rtn)
        throwCAPIException();
    return rtn;
}

Box* setattrFunc(Box* obj, Box* _str, Box* value) {
    _str = coerceUnicodeToStr(_str);

    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "setattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);

    setattr(obj, str, value);
    return None;
}

Box* hasattr(Box* obj, Box* _str) {
    _str = coerceUnicodeToStr(_str);

    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "hasattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);

    Box* attr;
    try {
        attr = getattrInternal<ExceptionStyle::CXX>(obj, str, NULL);
    } catch (ExcInfo e) {
        if (e.matches(Exception))
            return False;
        throw e;
    }

    Box* rtn = attr ? True : False;
    return rtn;
}

Box* map2(Box* f, Box* container) {
    Box* rtn = new BoxedList();
    bool use_identity_func = f == None;
    for (Box* e : container->pyElements()) {
        Box* val;
        if (use_identity_func)
            val = e;
        else
            val = runtimeCall(f, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
        listAppendInternal(rtn, val);
    }
    return rtn;
}

Box* map(Box* f, BoxedTuple* args) {
    assert(args->cls == tuple_cls);
    auto num_iterable = args->size();
    if (num_iterable < 1)
        raiseExcHelper(TypeError, "map() requires at least two args");

    // performance optimization for the case where we only have one iterable
    if (num_iterable == 1)
        return map2(f, args->elts[0]);

    std::vector<BoxIterator, StlCompatAllocator<BoxIterator>> args_it;
    std::vector<BoxIterator, StlCompatAllocator<BoxIterator>> args_end;

    for (auto e : *args) {
        auto range = e->pyElements();
        args_it.emplace_back(range.begin());
        args_end.emplace_back(range.end());
    }
    assert(args_it.size() == num_iterable);
    assert(args_end.size() == num_iterable);

    bool use_identity_func = f == None;
    Box* rtn = new BoxedList();
    std::vector<Box*, StlCompatAllocator<Box*>> current_val(num_iterable);
    while (true) {
        int num_done = 0;
        for (int i = 0; i < num_iterable; ++i) {
            if (args_it[i] == args_end[i]) {
                ++num_done;
                current_val[i] = None;
            } else {
                current_val[i] = *args_it[i];
            }
        }

        if (num_done == num_iterable)
            break;

        Box* entry;
        if (!use_identity_func) {
            auto v = getTupleFromArgsArray(&current_val[0], num_iterable);
            entry = runtimeCall(f, ArgPassSpec(num_iterable), std::get<0>(v), std::get<1>(v), std::get<2>(v),
                                std::get<3>(v), NULL);
        } else
            entry = BoxedTuple::create(num_iterable, &current_val[0]);
        listAppendInternal(rtn, entry);

        for (int i = 0; i < num_iterable; ++i) {
            if (args_it[i] != args_end[i])
                ++args_it[i];
        }
    }
    return rtn;
}

Box* reduce(Box* f, Box* container, Box* initial) {
    Box* current = initial;

    for (Box* e : container->pyElements()) {
        assert(e);
        if (current == NULL) {
            current = e;
        } else {
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
    if (f == None)
        f = bool_cls;

    // Special cases depending on the type of container influences the return type
    // TODO There are other special cases like this
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

    Box* rtn = new BoxedList();
    for (Box* e : container->pyElements()) {
        Box* r = runtimeCall(f, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
        bool b = nonzero(r);
        if (b)
            listAppendInternal(rtn, e);
    }
    return rtn;
}

Box* zip(BoxedTuple* containers) {
    assert(containers->cls == tuple_cls);

    BoxedList* rtn = new BoxedList();
    if (containers->size() == 0)
        return rtn;

    std::vector<llvm::iterator_range<BoxIterator>, StlCompatAllocator<llvm::iterator_range<BoxIterator>>> ranges;
    for (auto container : *containers) {
        ranges.push_back(container->pyElements());
    }

    std::vector<BoxIterator, StlCompatAllocator<BoxIterator>> iterators;
    for (auto range : ranges) {
        iterators.push_back(range.begin());
    }

    while (true) {
        for (int i = 0; i < iterators.size(); i++) {
            if (iterators[i] == ranges[i].end())
                return rtn;
        }

        auto el = BoxedTuple::create(iterators.size());
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

Box* exceptionNew(BoxedClass* cls, BoxedTuple* args) {
    if (!isSubclass(cls->cls, type_cls))
        raiseExcHelper(TypeError, "exceptions.__new__(X): X is not a type object (%s)", getTypeName(cls));

    if (!isSubclass(cls, BaseException))
        raiseExcHelper(TypeError, "BaseException.__new__(%s): %s is not a subtype of BaseException",
                       getNameOfClass(cls), getNameOfClass(cls));

    BoxedException* rtn = new (cls) BoxedException();

    // TODO: this should be a MemberDescriptor and set during init
    if (args->size() == 1)
        rtn->giveAttr("message", args->elts[0]);
    else
        rtn->giveAttr("message", boxString(""));
    return rtn;
}

Box* exceptionStr(Box* b) {
    // TODO In CPython __str__ and __repr__ pull from an internalized message field, but for now do this:
    static BoxedString* message_str = internStringImmortal("message");
    Box* message = b->getattr(message_str);
    assert(message);
    message = str(message);
    assert(message->cls == str_cls);

    return message;
}

Box* exceptionRepr(Box* b) {
    // TODO In CPython __str__ and __repr__ pull from an internalized message field, but for now do this:
    static BoxedString* message_str = internStringImmortal("message");
    Box* message = b->getattr(message_str);
    assert(message);
    message = repr(message);
    assert(message->cls == str_cls);

    BoxedString* message_s = static_cast<BoxedString*>(message);
    return boxStringTwine(llvm::Twine(getTypeName(b)) + "(" + message_s->s() + ",)");
}

static BoxedClass* makeBuiltinException(BoxedClass* base, const char* name, int size = 0) {
    if (size == 0)
        size = base->tp_basicsize;

    BoxedClass* cls
        = BoxedHeapClass::create(type_cls, base, NULL, offsetof(BoxedException, attrs), 0, size, false, name);
    cls->giveAttr("__module__", boxString("exceptions"));

    if (base == object_cls) {
        cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)exceptionNew, UNKNOWN, 1, 0, true, true)));
        cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)exceptionStr, STR, 1)));
        cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)exceptionRepr, STR, 1)));
    }

    cls->freeze();

    builtins_module->giveAttr(name, cls);
    return cls;
}

extern "C" PyObject* PyErr_NewException(char* name, PyObject* _base, PyObject* dict) noexcept {
    if (_base == NULL)
        _base = Exception;
    if (dict == NULL)
        dict = new BoxedDict();

    try {
        char* dot_pos = strchr(name, '.');
        RELEASE_ASSERT(dot_pos, "");
        int n = strlen(name);
        BoxedString* boxedName = boxString(llvm::StringRef(dot_pos + 1, n - (dot_pos - name) - 1));

        // It can also be a tuple of bases
        RELEASE_ASSERT(isSubclass(_base->cls, type_cls), "");
        BoxedClass* base = static_cast<BoxedClass*>(_base);

        if (PyDict_GetItemString(dict, "__module__") == NULL) {
            PyDict_SetItemString(dict, "__module__", boxString(llvm::StringRef(name, dot_pos - name)));
        }
        checkAndThrowCAPIException();

        Box* cls = runtimeCall(type_cls, ArgPassSpec(3), boxedName, BoxedTuple::create({ base }), dict, NULL, NULL);
        return cls;
    } catch (ExcInfo e) {
        // PyErr_NewException isn't supposed to fail, and callers sometimes take advantage of that
        // by not checking the return value.  Since failing probably indicates a bug anyway,
        // to be safe just print the traceback and die.
        e.printExcAndTraceback();
        RELEASE_ASSERT(0, "PyErr_NewException failed");

        // The proper way of handling it:
        setCAPIException(e);
        return NULL;
    }
}

BoxedClass* enumerate_cls;
class BoxedEnumerate : public Box {
private:
    BoxIterator iterator, iterator_end;
    int64_t idx;

public:
    BoxedEnumerate(BoxIterator iterator_begin, BoxIterator iterator_end, int64_t idx)
        : iterator(iterator_begin), iterator_end(iterator_end), idx(idx) {}

    DEFAULT_CLASS(enumerate_cls);

    static Box* new_(Box* cls, Box* obj, Box* start) {
        RELEASE_ASSERT(cls == enumerate_cls, "");
        RELEASE_ASSERT(isSubclass(start->cls, int_cls), "");
        int64_t idx = static_cast<BoxedInt*>(start)->n;

        llvm::iterator_range<BoxIterator> range = obj->pyElements();
        return new BoxedEnumerate(range.begin(), range.end(), idx);
    }

    static Box* iter(Box* _self) {
        assert(_self->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        return self;
    }

    static Box* next(Box* _self) {
        assert(_self->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        Box* val = *self->iterator;
        ++self->iterator;
        return BoxedTuple::create({ boxInt(self->idx++), val });
    }

    static Box* hasnext(Box* _self) {
        assert(_self->cls == enumerate_cls);
        BoxedEnumerate* self = static_cast<BoxedEnumerate*>(_self);
        return boxBool(self->iterator != self->iterator_end);
    }

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        BoxedEnumerate* it = (BoxedEnumerate*)b;
        it->iterator.gcHandler(v);
        it->iterator_end.gcHandler(v);
    }
};

Box* globals() {
    // TODO is it ok that we don't return a real dict here?
    return getGlobalsDict();
}

Box* locals() {
    return fastLocalsToBoxedLocals();
}

extern "C" PyObject* PyEval_GetLocals(void) noexcept {
    try {
        return locals();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyEval_GetGlobals(void) noexcept {
    try {
        return globals();
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyEval_GetBuiltins(void) noexcept {
    return builtins_module;
}

Box* divmod(Box* lhs, Box* rhs) {
    return binopInternal(lhs, rhs, AST_TYPE::DivMod, false, NULL);
}

Box* powFunc(Box* x, Box* y, Box* z) {
    Box* rtn = PyNumber_Power(x, y, z);
    checkAndThrowCAPIException();
    return rtn;
}

Box* execfile(Box* _fn) {
    // The "globals" and "locals" arguments aren't implemented for now
    if (!isSubclass(_fn->cls, str_cls)) {
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(_fn));
    }

    BoxedString* fn = static_cast<BoxedString*>(_fn);

#if LLVMREV < 217625
    bool exists;
    llvm_error_code code = llvm::sys::fs::exists(fn->s, exists);

#if LLVMREV < 210072
    ASSERT(code == 0, "%s: %s", code.message().c_str(), fn->s.c_str());
#else
    assert(!code);
#endif

#else
    bool exists = llvm::sys::fs::exists(std::string(fn->s()));
#endif

    if (!exists)
        raiseExcHelper(IOError, "No such file or directory: '%s'", fn->data());

    // Run directly inside the current module:
    AST_Module* ast = caching_parse_file(fn->data());

    ASSERT(getTopPythonFunction()->source->scoping->areGlobalsFromModule(), "need to pass custom globals in");
    compileAndRunModule(ast, getCurrentModule());

    return None;
}

Box* print(BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    Box* dest, *end;

    static BoxedString* file_str = internStringImmortal("file");
    static BoxedString* end_str = internStringImmortal("end");
    static BoxedString* space_str = internStringImmortal(" ");

    BoxedDict::DictMap::iterator it;
    if (kwargs && ((it = kwargs->d.find(file_str)) != kwargs->d.end())) {
        dest = it->second;
        kwargs->d.erase(it);
    } else {
        dest = getSysStdout();
    }

    if (kwargs && ((it = kwargs->d.find(end_str)) != kwargs->d.end())) {
        end = it->second;
        kwargs->d.erase(it);
    } else {
        end = boxString("\n");
    }

    RELEASE_ASSERT(!kwargs || kwargs->d.size() == 0, "print() got unexpected keyword arguments");

    static BoxedString* write_str = internStringImmortal("write");
    CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(1) };

    // TODO softspace handling?
    // TODO: duplicates code with ASTInterpreter::visit_print()

    bool first = true;
    for (auto e : *args) {
        BoxedString* s = str(e);
        if (!first) {
            Box* r = callattr(dest, write_str, callattr_flags, space_str, NULL, NULL, NULL, NULL);
            RELEASE_ASSERT(r, "");
        }
        first = false;
        Box* r = callattr(dest, write_str, callattr_flags, s, NULL, NULL, NULL, NULL);
        RELEASE_ASSERT(r, "");
    }
    Box* r = callattr(dest, write_str, callattr_flags, end, NULL, NULL, NULL, NULL);
    RELEASE_ASSERT(r, "");

    return None;
}

Box* getreversed(Box* o) {
    static BoxedString* reversed_str = internStringImmortal("__reversed__");

    // TODO add rewriting to this?  probably want to try to avoid this path though
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(o, reversed_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (r)
        return r;

    static BoxedString* getitem_str = internStringImmortal("__getitem__");
    if (!typeLookup(o->cls, getitem_str, NULL)) {
        raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o));
    }
    int64_t len = unboxedLen(o); // this will throw an exception if __len__ isn't there

    return new (seqreviter_cls) BoxedSeqIter(o, len - 1);
}

Box* pydump(Box* p) {
    dump(p);
    return None;
}

Box* pydumpAddr(Box* p) {
    if (p->cls != int_cls)
        raiseExcHelper(TypeError, "Requires an int");

    dump((void*)static_cast<BoxedInt*>(p)->n);
    return None;
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
    char* str;

    PyObject* line = raw_input(prompt);
    if (line == NULL)
        throwCAPIException();

    if (!PyArg_Parse(line, "s;embedded '\\0' in input line", &str))
        throwCAPIException();

    // CPython trims the string first, but our eval function takes care of that.
    // while (*str == ' ' || *str == '\t')
    //    str++;

    Box* gbls = globals();
    Box* lcls = locals();

    // CPython has these safety checks that the builtin functions exist
    // in the current global scope.
    // e.g. eval('input()', {})
    if (PyDict_GetItemString(gbls, "__builtins__") == NULL) {
        if (PyDict_SetItemString(gbls, "__builtins__", builtins_module) != 0)
            throwCAPIException();
    }

    return eval(line, gbls, lcls);
}

Box* builtinRound(Box* _number, Box* _ndigits) {
    double x = PyFloat_AsDouble(_number);
    if (PyErr_Occurred())
        raiseExcHelper(TypeError, "a float is required");

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

Box* builtinApply(Box* func, Box* args, Box* keywords) {
    if (!PyTuple_Check(args)) {
        if (!PySequence_Check(args))
            raiseExcHelper(TypeError, "apply() arg 2 expected sequence, found %s", getTypeName(args));
        args = PySequence_Tuple(args);
        checkAndThrowCAPIException();
    }
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

void setupBuiltins() {
    builtins_module
        = createModule("__builtin__", NULL, "Built-in functions, exceptions, and other objects.\n\nNoteworthy: None is "
                                            "the `nil' object; Ellipsis represents `...' in slices.");

    BoxedHeapClass* ellipsis_cls
        = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(Box), false, "ellipsis");
    Ellipsis = new (ellipsis_cls) Box();
    assert(Ellipsis->cls);
    gc::registerPermanentRoot(Ellipsis);

    builtins_module->giveAttr("Ellipsis", Ellipsis);
    builtins_module->giveAttr("None", None);

    builtins_module->giveAttr("__debug__", False);

    builtins_module->giveAttr(
        "print", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)print, NONE, 0, 0, true, true), "print"));

    notimplemented_cls
        = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(Box), false, "NotImplementedType");
    notimplemented_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)notimplementedRepr, STR, 1)));
    notimplemented_cls->freeze();
    NotImplemented = new (notimplemented_cls) Box();
    gc::registerPermanentRoot(NotImplemented);

    builtins_module->giveAttr("NotImplemented", NotImplemented);
    builtins_module->giveAttr("NotImplementedType", notimplemented_cls);

    builtins_module->giveAttr("all", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)all, BOXED_BOOL, 1), "all"));
    builtins_module->giveAttr("any", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)any, BOXED_BOOL, 1), "any"));

    builtins_module->giveAttr(
        "apply", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)builtinApply, UNKNOWN, 3, 1, false, false),
                                                  "apply", { NULL }));

    repr_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)repr, UNKNOWN, 1), "repr");
    builtins_module->giveAttr("repr", repr_obj);

    auto len_func = boxRTFunction((void*)len, UNKNOWN, 1);
    len_func->internal_callable.cxx_val = lenCallInternal;
    len_obj = new BoxedBuiltinFunctionOrMethod(len_func, "len");
    builtins_module->giveAttr("len", len_obj);

    hash_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)hash, UNKNOWN, 1), "hash");
    builtins_module->giveAttr("hash", hash_obj);
    abs_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)abs_, UNKNOWN, 1), "abs");
    builtins_module->giveAttr("abs", abs_obj);
    builtins_module->giveAttr("hex",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)hexFunc, UNKNOWN, 1), "hex"));
    builtins_module->giveAttr("oct",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)octFunc, UNKNOWN, 1), "oct"));

    min_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)min, UNKNOWN, 1, 1, true, true), "min", { None });
    builtins_module->giveAttr("min", min_obj);

    max_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)max, UNKNOWN, 1, 1, true, true), "max", { None });
    builtins_module->giveAttr("max", max_obj);

    builtins_module->giveAttr("next", new BoxedBuiltinFunctionOrMethod(
                                          boxRTFunction((void*)next, UNKNOWN, 2, 1, false, false), "next", { NULL }));

    builtins_module->giveAttr("sum", new BoxedBuiltinFunctionOrMethod(
                                         boxRTFunction((void*)sum, UNKNOWN, 2, 1, false, false), "sum", { boxInt(0) }));

    id_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)id, BOXED_INT, 1), "id");
    builtins_module->giveAttr("id", id_obj);
    chr_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)chr, STR, 1), "chr");
    builtins_module->giveAttr("chr", chr_obj);
    builtins_module->giveAttr("unichr",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)unichr, UNKNOWN, 1), "unichr"));
    ord_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)ord, BOXED_INT, 1), "ord");
    builtins_module->giveAttr("ord", ord_obj);
    trap_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)trap, UNKNOWN, 0), "trap");
    builtins_module->giveAttr("trap", trap_obj);
    builtins_module->giveAttr("dump",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)pydump, UNKNOWN, 1), "dump"));
    builtins_module->giveAttr(
        "dumpAddr", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)pydumpAddr, UNKNOWN, 1), "dumpAddr"));

    builtins_module->giveAttr("delattr",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)delattrFunc, NONE, 2), "delattr"));

    builtins_module->giveAttr(
        "getattr", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)getattrFunc, UNKNOWN, 3, 1, false, false),
                                                    "getattr", { NULL }));

    builtins_module->giveAttr(
        "setattr",
        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)setattrFunc, UNKNOWN, 3, 0, false, false), "setattr"));

    Box* hasattr_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)hasattr, BOXED_BOOL, 2), "hasattr");
    builtins_module->giveAttr("hasattr", hasattr_obj);

    builtins_module->giveAttr("pow", new BoxedBuiltinFunctionOrMethod(
                                         boxRTFunction((void*)powFunc, UNKNOWN, 3, 1, false, false), "pow", { None }));

    Box* isinstance_obj
        = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)isinstance_func, BOXED_BOOL, 2), "isinstance");
    builtins_module->giveAttr("isinstance", isinstance_obj);

    Box* issubclass_obj
        = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)issubclass_func, BOXED_BOOL, 2), "issubclass");
    builtins_module->giveAttr("issubclass", issubclass_obj);

    Box* intern_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)intern_func, UNKNOWN, 1), "intern");
    builtins_module->giveAttr("intern", intern_obj);

    CLFunction* import_func = boxRTFunction((void*)bltinImport, UNKNOWN, 5, 4, false, false,
                                            ParamNames({ "name", "globals", "locals", "fromlist", "level" }, "", ""));
    builtins_module->giveAttr("__import__", new BoxedBuiltinFunctionOrMethod(import_func, "__import__",
                                                                             { None, None, None, new BoxedInt(-1) }));

    enumerate_cls = BoxedHeapClass::create(type_cls, object_cls, &BoxedEnumerate::gcHandler, 0, 0,
                                           sizeof(BoxedEnumerate), false, "enumerate");
    enumerate_cls->giveAttr(
        "__new__",
        new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::new_, UNKNOWN, 3, 1, false, false), { boxInt(0) }));
    enumerate_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::iter, typeFromClass(enumerate_cls), 1)));
    enumerate_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::next, BOXED_TUPLE, 1)));
    enumerate_cls->giveAttr("__hasnext__",
                            new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::hasnext, BOXED_BOOL, 1)));
    enumerate_cls->freeze();
    builtins_module->giveAttr("enumerate", enumerate_cls);


    CLFunction* sorted_func = createRTFunction(4, 3, false, false, ParamNames({ "", "cmp", "key", "reverse" }, "", ""));
    addRTFunction(sorted_func, (void*)sorted, LIST, { UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN });
    builtins_module->giveAttr("sorted", new BoxedBuiltinFunctionOrMethod(sorted_func, "sorted", { None, None, False }));

    builtins_module->giveAttr("True", True);
    builtins_module->giveAttr("False", False);

    range_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)range, LIST, 3, 2, false, false), "range",
                                                 { NULL, NULL });
    builtins_module->giveAttr("range", range_obj);

    auto* round_obj = new BoxedBuiltinFunctionOrMethod(
        boxRTFunction((void*)builtinRound, BOXED_FLOAT, 2, 1, false, false), "round", { boxInt(0) });
    builtins_module->giveAttr("round", round_obj);

    setupXrange();
    builtins_module->giveAttr("xrange", xrange_cls);

    open_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)open, typeFromClass(file_cls), 3, 2, false, false,
                                                              ParamNames({ "name", "mode", "buffering" }, "", "")),
                                                "open", { boxString("r"), boxInt(-1) });
    builtins_module->giveAttr("open", open_obj);

    builtins_module->giveAttr("globals", new BoxedBuiltinFunctionOrMethod(
                                             boxRTFunction((void*)globals, UNKNOWN, 0, 0, false, false), "globals"));
    builtins_module->giveAttr("locals", new BoxedBuiltinFunctionOrMethod(
                                            boxRTFunction((void*)locals, UNKNOWN, 0, 0, false, false), "locals"));

    builtins_module->giveAttr(
        "iter", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)builtinIter, UNKNOWN, 2, 1, false, false), "iter",
                                                 { NULL }));
    builtins_module->giveAttr(
        "reversed",
        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)getreversed, UNKNOWN, 1, 0, false, false), "reversed"));
    builtins_module->giveAttr("coerce", new BoxedBuiltinFunctionOrMethod(
                                            boxRTFunction((void*)coerceFunc, UNKNOWN, 2, 0, false, false), "coerce"));
    builtins_module->giveAttr("divmod",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)divmod, UNKNOWN, 2), "divmod"));
    builtins_module->giveAttr("execfile",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)execfile, UNKNOWN, 1), "execfile"));

    CLFunction* compile_func = createRTFunction(
        5, 2, false, false, ParamNames({ "source", "filename", "mode", "flags", "dont_inherit" }, "", ""));
    addRTFunction(compile_func, (void*)compile, UNKNOWN, { UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN });
    builtins_module->giveAttr("compile",
                              new BoxedBuiltinFunctionOrMethod(compile_func, "compile", { boxInt(0), boxInt(0) }));

    builtins_module->giveAttr(
        "map", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)map, LIST, 1, 0, true, false), "map"));
    builtins_module->giveAttr(
        "reduce", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)reduce, UNKNOWN, 3, 1, false, false), "reduce",
                                                   { NULL }));
    builtins_module->giveAttr("filter",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)filter2, UNKNOWN, 2), "filter"));
    builtins_module->giveAttr(
        "zip", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)zip, LIST, 0, 0, true, false), "zip"));
    builtins_module->giveAttr(
        "dir", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)dir, LIST, 1, 1, false, false), "dir", { NULL }));
    builtins_module->giveAttr("vars", new BoxedBuiltinFunctionOrMethod(
                                          boxRTFunction((void*)vars, UNKNOWN, 1, 1, false, false), "vars", { NULL }));
    builtins_module->giveAttr("object", object_cls);
    builtins_module->giveAttr("str", str_cls);
    builtins_module->giveAttr("bytes", str_cls);
    assert(unicode_cls);
    builtins_module->giveAttr("unicode", unicode_cls);
    builtins_module->giveAttr("basestring", basestring_cls);
    // builtins_module->giveAttr("unicode", unicode_cls);
    builtins_module->giveAttr("int", int_cls);
    builtins_module->giveAttr("long", long_cls);
    builtins_module->giveAttr("float", float_cls);
    builtins_module->giveAttr("list", list_cls);
    builtins_module->giveAttr("slice", slice_cls);
    builtins_module->giveAttr("type", type_cls);
    builtins_module->giveAttr("file", file_cls);
    builtins_module->giveAttr("bool", bool_cls);
    builtins_module->giveAttr("dict", dict_cls);
    builtins_module->giveAttr("set", set_cls);
    builtins_module->giveAttr("frozenset", frozenset_cls);
    builtins_module->giveAttr("tuple", tuple_cls);
    builtins_module->giveAttr("instancemethod", instancemethod_cls);
    builtins_module->giveAttr("complex", complex_cls);
    builtins_module->giveAttr("super", super_cls);
    builtins_module->giveAttr("property", property_cls);
    builtins_module->giveAttr("staticmethod", staticmethod_cls);
    builtins_module->giveAttr("classmethod", classmethod_cls);

    assert(memoryview_cls);
    Py_TYPE(&PyMemoryView_Type) = &PyType_Type;
    PyType_Ready(&PyMemoryView_Type);
    builtins_module->giveAttr("memoryview", memoryview_cls);
    PyType_Ready(&PyByteArray_Type);
    builtins_module->giveAttr("bytearray", &PyByteArray_Type);
    Py_TYPE(&PyBuffer_Type) = &PyType_Type;
    PyType_Ready(&PyBuffer_Type);
    builtins_module->giveAttr("buffer", &PyBuffer_Type);

    builtins_module->giveAttr("eval",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)eval, UNKNOWN, 3, 2, false, false),
                                                               "eval", { NULL, NULL }));
    builtins_module->giveAttr("callable",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)callable, UNKNOWN, 1), "callable"));

    builtins_module->giveAttr(
        "raw_input", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)rawInput, UNKNOWN, 1, 1, false, false),
                                                      "raw_input", { NULL }));
    builtins_module->giveAttr(
        "input",
        new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)input, UNKNOWN, 1, 1, false, false), "input", { NULL }));
    builtins_module->giveAttr("cmp",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)builtinCmp, UNKNOWN, 2), "cmp"));
    builtins_module->giveAttr(
        "format", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)builtinFormat, UNKNOWN, 2), "format"));
}
}
