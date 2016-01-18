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

    static BoxedString* dict_str = internStringImmortal("__dict__");
    Box* rtn = getattrInternal<ExceptionStyle::CAPI>(obj, dict_str);
    if (!rtn)
        raiseExcHelper(TypeError, "vars() argument must have __dict__ attribute");
    return rtn;
}

extern "C" Box* abs_(Box* x) {
    if (PyInt_Check(x)) {
        i64 n = static_cast<BoxedInt*>(x)->n;
        return boxInt(n >= 0 ? n : -n);
    } else if (x->cls == float_cls) {
        double d = static_cast<BoxedFloat*>(x)->d;
        return boxFloat(std::abs(d));
    } else if (x->cls == long_cls) {
        return longAbs(static_cast<BoxedLong*>(x));
    } else {
        static BoxedString* abs_str = internStringImmortal("__abs__");
        CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = false, .argspec = ArgPassSpec(0) };
        return callattr(x, abs_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    }
}

extern "C" Box* binFunc(Box* x) {
    static BoxedString* bin_str = internStringImmortal("__bin__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(x, bin_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (!r)
        raiseExcHelper(TypeError, "bin() argument can't be converted to bin");

    if (!PyString_Check(r))
        raiseExcHelper(TypeError, "__bin__() returned non-string (type %.200s)", r->cls->tp_name);

    return r;
}

extern "C" Box* hexFunc(Box* x) {
    static BoxedString* hex_str = internStringImmortal("__hex__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(x, hex_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (!r)
        raiseExcHelper(TypeError, "hex() argument can't be converted to hex");

    if (!PyString_Check(r))
        raiseExcHelper(TypeError, "__hex__() returned non-string (type %.200s)", r->cls->tp_name);

    return r;
}

extern "C" Box* octFunc(Box* x) {
    static BoxedString* oct_str = internStringImmortal("__oct__");
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(x, oct_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (!r)
        raiseExcHelper(TypeError, "oct() argument can't be converted to oct");

    if (!PyString_Check(r))
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

    name = coerceUnicodeToStr<CXX>(name);

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
    _str = coerceUnicodeToStr<CXX>(_str);

    if (_str->cls != str_cls)
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(_str));
    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);

    delattr(obj, str);
    return None;
}

static Box* getattrFuncHelper(Box* return_val, Box* obj, BoxedString* str, Box* default_val) noexcept {
    assert(PyString_Check(str));

    if (return_val)
        return return_val;

    bool exc = PyErr_Occurred();
    if (exc && !PyErr_ExceptionMatches(AttributeError))
        return NULL;

    if (default_val) {
        if (exc)
            PyErr_Clear();
        return default_val;
    }
    if (!exc)
        raiseAttributeErrorCapi(obj, str->s());
    return NULL;
}

template <ExceptionStyle S>
Box* getattrFuncInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    static Box* defaults[] = { NULL };
    bool rewrite_success = false;
    rearrangeArguments(ParamReceiveSpec(3, 1, false, false), NULL, "getattr", defaults, rewrite_args, rewrite_success,
                       argspec, arg1, arg2, arg3, args, NULL, keyword_names);
    if (!rewrite_success)
        rewrite_args = NULL;

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
        return NULL;

    if (!PyString_Check(_str)) {
        if (S == CAPI) {
            PyErr_SetString(TypeError, "getattr(): attribute name must be string");
            return NULL;
        } else
            raiseExcHelper(TypeError, "getattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    if (!PyString_CHECK_INTERNED(str))
        internStringMortalInplace(str);

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
            if (return_convention == ReturnConvention::NO_RETURN)
                r_rtn = rewrite_args->rewriter->loadConst(0);
        }
    } else {
        rtn = getattrInternal<CAPI>(obj, str);
    }

    if (rewrite_args) {
        assert(PyString_CHECK_INTERNED(str) == SSTATE_INTERNED_IMMORTAL);
        RewriterVar* r_str = rewrite_args->rewriter->loadConst((intptr_t)str, Location::forArg(2));
        RewriterVar* final_rtn = rewrite_args->rewriter->call(false, (void*)getattrFuncHelper, r_rtn,
                                                              rewrite_args->arg1, r_str, rewrite_args->arg3);

        if (S == CXX)
            rewrite_args->rewriter->checkAndThrowCAPIException(final_rtn);
        rewrite_args->out_success = true;
        rewrite_args->out_rtn = final_rtn;
    }

    Box* r = getattrFuncHelper(rtn, obj, str, default_value);
    if (S == CXX && !r)
        throwCAPIException();
    return r;
}

Box* setattrFunc(Box* obj, Box* _str, Box* value) {
    _str = coerceUnicodeToStr<CXX>(_str);

    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "attribute name must be string, not '%s'", _str->cls->tp_name);
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    internStringMortalInplace(str);

    setattr(obj, str, value);
    return None;
}

static Box* hasattrFuncHelper(Box* return_val) noexcept {
    if (return_val)
        return True;

    if (PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_Exception))
            return NULL;

        PyErr_Clear();
    }
    return False;
}

template <ExceptionStyle S>
Box* hasattrFuncInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                         Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    bool rewrite_success = false;
    rearrangeArguments(ParamReceiveSpec(2, 0, false, false), NULL, "hasattr", NULL, rewrite_args, rewrite_success,
                       argspec, arg1, arg2, arg3, args, NULL, keyword_names);
    if (!rewrite_success)
        rewrite_args = NULL;

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
        return NULL;

    if (!PyString_Check(_str)) {
        if (S == CAPI) {
            PyErr_SetString(TypeError, "hasattr(): attribute name must be string");
            return NULL;
        } else
            raiseExcHelper(TypeError, "hasattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    if (!PyString_CHECK_INTERNED(str))
        internStringMortalInplace(str);

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
            if (return_convention == ReturnConvention::NO_RETURN)
                r_rtn = rewrite_args->rewriter->loadConst(0);
        }
    } else {
        rtn = getattrInternal<CAPI>(obj, str);
    }

    if (rewrite_args) {
        RewriterVar* final_rtn = rewrite_args->rewriter->call(false, (void*)hasattrFuncHelper, r_rtn);

        if (S == CXX)
            rewrite_args->rewriter->checkAndThrowCAPIException(final_rtn);
        rewrite_args->out_success = true;
        rewrite_args->out_rtn = final_rtn;
    }

    Box* r = hasattrFuncHelper(rtn);
    if (S == CXX && !r)
        throwCAPIException();
    return r;
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
    if (!PyType_Check(cls))
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

    BoxedClass* cls = BoxedClass::create(type_cls, base, NULL, offsetof(BoxedException, attrs), 0, size, false, name);
    cls->giveAttr("__module__", boxString("exceptions"));

    if (base == object_cls) {
        cls->giveAttr("__new__",
                      new BoxedFunction(FunctionMetadata::create((void*)exceptionNew, UNKNOWN, 1, true, true)));
        cls->giveAttr("__str__", new BoxedFunction(FunctionMetadata::create((void*)exceptionStr, STR, 1)));
        cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)exceptionRepr, STR, 1)));
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
        RELEASE_ASSERT(PyType_Check(_base), "");
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
        RELEASE_ASSERT(PyInt_Check(start), "");
        int64_t idx = static_cast<BoxedInt*>(start)->n;

        llvm::iterator_range<BoxIterator> range = obj->pyElements();
        return new BoxedEnumerate(range.begin(), range.end(), idx);
    }

    static Box* iter(Box* _self) noexcept {
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
        Box::gcHandler(v, b);

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

Box* ellipsisRepr(Box* self) {
    return boxString("Ellipsis");
}
Box* divmod(Box* lhs, Box* rhs) {
    return binopInternal<NOT_REWRITABLE>(lhs, rhs, AST_TYPE::DivMod, false, NULL);
}

Box* powFunc(Box* x, Box* y, Box* z) {
    Box* rtn = PyNumber_Power(x, y, z);
    checkAndThrowCAPIException();
    return rtn;
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

    // common case:
    if (o->cls == list_cls) {
        return listReversed(o);
    }

    // TODO add rewriting to this?  probably want to try to avoid this path though
    CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
    Box* r = callattr(o, reversed_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    if (r)
        return r;

    static BoxedString* getitem_str = internStringImmortal("__getitem__");
    if (!typeLookup(o->cls, getitem_str)) {
        raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o));
    }
    int64_t len = unboxedLen(o); // this will throw an exception if __len__ isn't there

    return new (seqreviter_cls) BoxedSeqIter(o, len - 1);
}

Box* pydump(Box* p, BoxedInt* level) {
    dumpEx(p, level->n);
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
If the iterable is empty, return True.");

PyDoc_STRVAR(any_doc, "any(iterable) -> bool\n\
\n\
Return True if bool(x) is True for any x in the iterable.\n\
If the iterable is empty, return False.");

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

void setupBuiltins() {
    builtins_module = createModule(boxString("__builtin__"), NULL,
                                   "Built-in functions, exceptions, and other objects.\n\nNoteworthy: None is "
                                   "the `nil' object; Ellipsis represents `...' in slices.");

    ellipsis_cls = BoxedClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(Box), false, "ellipsis");
    ellipsis_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)ellipsisRepr, STR, 1)));
    Ellipsis = new (ellipsis_cls) Box();
    assert(Ellipsis->cls);
    gc::registerPermanentRoot(Ellipsis);

    builtins_module->giveAttr("Ellipsis", Ellipsis);
    builtins_module->giveAttr("None", None);

    builtins_module->giveAttr("__debug__", False);

    builtins_module->giveAttr(
        "print", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)print, NONE, 0, true, true), "print",
                                                  print_doc));

    notimplemented_cls = BoxedClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(Box), false, "NotImplementedType");
    notimplemented_cls->giveAttr("__repr__",
                                 new BoxedFunction(FunctionMetadata::create((void*)notimplementedRepr, STR, 1)));
    notimplemented_cls->freeze();
    notimplemented_cls->instances_are_nonzero = true;
    NotImplemented = new (notimplemented_cls) Box();
    gc::registerPermanentRoot(NotImplemented);

    builtins_module->giveAttr("NotImplemented", NotImplemented);

    builtins_module->giveAttr(
        "all", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)all, BOXED_BOOL, 1), "all", all_doc));
    builtins_module->giveAttr(
        "any", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)any, BOXED_BOOL, 1), "any", any_doc));

    builtins_module->giveAttr("apply", new BoxedBuiltinFunctionOrMethod(
                                           FunctionMetadata::create((void*)builtinApply, UNKNOWN, 3, false, false),
                                           "apply", { NULL }, NULL, apply_doc));

    repr_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)repr, UNKNOWN, 1), "repr", repr_doc);
    builtins_module->giveAttr("repr", repr_obj);

    auto len_func = FunctionMetadata::create((void*)len, UNKNOWN, 1);
    len_func->internal_callable.cxx_val = lenCallInternal;
    len_obj = new BoxedBuiltinFunctionOrMethod(len_func, "len", len_doc);
    builtins_module->giveAttr("len", len_obj);

    hash_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)hash, UNKNOWN, 1), "hash", hash_doc);
    builtins_module->giveAttr("hash", hash_obj);
    abs_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)abs_, UNKNOWN, 1), "abs", abs_doc);
    builtins_module->giveAttr("abs", abs_obj);
    builtins_module->giveAttr(
        "bin", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)binFunc, UNKNOWN, 1), "bin", bin_doc));
    builtins_module->giveAttr(
        "hex", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)hexFunc, UNKNOWN, 1), "hex", hex_doc));
    builtins_module->giveAttr(
        "oct", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)octFunc, UNKNOWN, 1), "oct", oct_doc));

    min_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)min, UNKNOWN, 1, true, true), "min",
                                               { None }, NULL, min_doc);
    builtins_module->giveAttr("min", min_obj);

    max_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)max, UNKNOWN, 1, true, true), "max",
                                               { None }, NULL, max_doc);
    builtins_module->giveAttr("max", max_obj);

    builtins_module->giveAttr(
        "next", new BoxedBuiltinFunctionOrMethod(
                    FunctionMetadata::create((void*)next, UNKNOWN, 2, false, false, ParamNames::empty(), CAPI), "next",
                    { NULL }, NULL, next_doc));

    builtins_module->giveAttr(
        "sum", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)sum, UNKNOWN, 2, false, false), "sum",
                                                { boxInt(0) }, NULL, sum_doc));

    id_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)id, BOXED_INT, 1), "id", id_doc);
    builtins_module->giveAttr("id", id_obj);
    chr_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)chr, STR, 1), "chr", chr_doc);
    builtins_module->giveAttr("chr", chr_obj);
    builtins_module->giveAttr("unichr", new BoxedBuiltinFunctionOrMethod(
                                            FunctionMetadata::create((void*)unichr, UNKNOWN, 1), "unichr", unichr_doc));
    ord_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)ord, BOXED_INT, 1), "ord", ord_doc);
    builtins_module->giveAttr("ord", ord_obj);
    trap_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)trap, UNKNOWN, 0), "trap");
    builtins_module->giveAttr("trap", trap_obj);
    builtins_module->giveAttr("dump", new BoxedBuiltinFunctionOrMethod(
                                          FunctionMetadata::create((void*)pydump, UNKNOWN, 2), "dump", { boxInt(0) }));
    builtins_module->giveAttr("dumpAddr", new BoxedBuiltinFunctionOrMethod(
                                              FunctionMetadata::create((void*)pydumpAddr, UNKNOWN, 1), "dumpAddr"));

    builtins_module->giveAttr("delattr",
                              new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)delattrFunc, NONE, 2),
                                                               "delattr", delattr_doc));

    auto getattr_func = new FunctionMetadata(3, true, true, ParamNames::empty());
    getattr_func->internal_callable.capi_val = &getattrFuncInternal<CAPI>;
    getattr_func->internal_callable.cxx_val = &getattrFuncInternal<CXX>;
    builtins_module->giveAttr("getattr",
                              new BoxedBuiltinFunctionOrMethod(getattr_func, "getattr", { NULL }, NULL, getattr_doc));

    builtins_module->giveAttr(
        "setattr", new BoxedBuiltinFunctionOrMethod(
                       FunctionMetadata::create((void*)setattrFunc, UNKNOWN, 3, false, false), "setattr", setattr_doc));

    auto hasattr_func = new FunctionMetadata(2, false, false);
    hasattr_func->internal_callable.capi_val = &hasattrFuncInternal<CAPI>;
    hasattr_func->internal_callable.cxx_val = &hasattrFuncInternal<CXX>;
    builtins_module->giveAttr("hasattr", new BoxedBuiltinFunctionOrMethod(hasattr_func, "hasattr", hasattr_doc));

    builtins_module->giveAttr(
        "pow", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)powFunc, UNKNOWN, 3, false, false),
                                                "pow", { None }, NULL, pow_doc));

    Box* isinstance_obj = new BoxedBuiltinFunctionOrMethod(
        FunctionMetadata::create((void*)isinstance_func, BOXED_BOOL, 2), "isinstance", isinstance_doc);
    builtins_module->giveAttr("isinstance", isinstance_obj);

    Box* issubclass_obj = new BoxedBuiltinFunctionOrMethod(
        FunctionMetadata::create((void*)issubclass_func, BOXED_BOOL, 2), "issubclass", issubclass_doc);
    builtins_module->giveAttr("issubclass", issubclass_obj);

    Box* intern_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)intern_func, UNKNOWN, 1),
                                                       "intern", intern_doc);
    builtins_module->giveAttr("intern", intern_obj);

    FunctionMetadata* import_func
        = FunctionMetadata::create((void*)bltinImport, UNKNOWN, 5, false, false,
                                   ParamNames({ "name", "globals", "locals", "fromlist", "level" }, "", ""));
    builtins_module->giveAttr("__import__",
                              new BoxedBuiltinFunctionOrMethod(import_func, "__import__",
                                                               { None, None, None, boxInt(-1) }, NULL, import_doc));

    enumerate_cls = BoxedClass::create(type_cls, object_cls, &BoxedEnumerate::gcHandler, 0, 0, sizeof(BoxedEnumerate),
                                       false, "enumerate");
    enumerate_cls->giveAttr(
        "__new__", new BoxedFunction(FunctionMetadata::create((void*)BoxedEnumerate::new_, UNKNOWN, 3, false, false),
                                     { boxInt(0) }));
    enumerate_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create((void*)BoxedEnumerate::iter,
                                                                                   typeFromClass(enumerate_cls), 1)));
    enumerate_cls->giveAttr("next",
                            new BoxedFunction(FunctionMetadata::create((void*)BoxedEnumerate::next, BOXED_TUPLE, 1)));
    enumerate_cls->giveAttr("__hasnext__",
                            new BoxedFunction(FunctionMetadata::create((void*)BoxedEnumerate::hasnext, BOXED_BOOL, 1)));
    enumerate_cls->freeze();
    enumerate_cls->tp_iter = PyObject_SelfIter;
    builtins_module->giveAttr("enumerate", enumerate_cls);


    FunctionMetadata* sorted_func
        = new FunctionMetadata(4, false, false, ParamNames({ "", "cmp", "key", "reverse" }, "", ""));
    sorted_func->addVersion((void*)sorted, LIST, { UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN });
    builtins_module->giveAttr(
        "sorted", new BoxedBuiltinFunctionOrMethod(sorted_func, "sorted", { None, None, False }, NULL, sorted_doc));

    builtins_module->giveAttr("True", True);
    builtins_module->giveAttr("False", False);

    range_obj = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)range, LIST, 3, false, false), "range",
                                                 { NULL, NULL }, NULL, range_doc);
    builtins_module->giveAttr("range", range_obj);

    auto* round_obj
        = new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)builtinRound, BOXED_FLOAT, 2, false, false),
                                           "round", { boxInt(0) }, NULL, round_doc);
    builtins_module->giveAttr("round", round_obj);

    setupXrange();
    builtins_module->giveAttr("xrange", xrange_cls);

    open_obj = new BoxedBuiltinFunctionOrMethod(
        FunctionMetadata::create((void*)open, typeFromClass(file_cls), 3, false, false,
                                 ParamNames({ "name", "mode", "buffering" }, "", "")),
        "open", { boxString("r"), boxInt(-1) }, NULL, open_doc);
    builtins_module->giveAttr("open", open_obj);

    builtins_module->giveAttr(
        "globals", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)globals, UNKNOWN, 0, false, false),
                                                    "globals", globals_doc));
    builtins_module->giveAttr(
        "locals", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)locals, UNKNOWN, 0, false, false),
                                                   "locals", locals_doc));

    builtins_module->giveAttr(
        "iter", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)builtinIter, UNKNOWN, 2, false, false),
                                                 "iter", { NULL }, NULL, iter_doc));
    builtins_module->giveAttr("reversed",
                              new BoxedBuiltinFunctionOrMethod(
                                  FunctionMetadata::create((void*)getreversed, UNKNOWN, 1, false, false), "reversed"));
    builtins_module->giveAttr(
        "coerce", new BoxedBuiltinFunctionOrMethod(
                      FunctionMetadata::create((void*)coerceFunc, UNKNOWN, 2, false, false), "coerce", coerce_doc));
    builtins_module->giveAttr("divmod", new BoxedBuiltinFunctionOrMethod(
                                            FunctionMetadata::create((void*)divmod, UNKNOWN, 2), "divmod", divmod_doc));

    builtins_module->giveAttr("execfile", new BoxedBuiltinFunctionOrMethod(
                                              FunctionMetadata::create((void*)execfile, UNKNOWN, 3, false, false),
                                              "execfile", { NULL, NULL }, NULL, execfile_doc));

    FunctionMetadata* compile_func = new FunctionMetadata(
        5, false, false, ParamNames({ "source", "filename", "mode", "flags", "dont_inherit" }, "", ""));
    compile_func->addVersion((void*)compile, UNKNOWN, { UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN });
    builtins_module->giveAttr("compile", new BoxedBuiltinFunctionOrMethod(compile_func, "compile",
                                                                          { boxInt(0), boxInt(0) }, NULL, compile_doc));

    builtins_module->giveAttr("map", new BoxedBuiltinFunctionOrMethod(
                                         FunctionMetadata::create((void*)map, LIST, 1, true, false), "map", map_doc));
    builtins_module->giveAttr(
        "reduce", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)reduce, UNKNOWN, 3, false, false),
                                                   "reduce", { NULL }, NULL, reduce_doc));
    builtins_module->giveAttr(
        "filter",
        new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)filter2, UNKNOWN, 2), "filter", filter_doc));
    builtins_module->giveAttr("zip", new BoxedBuiltinFunctionOrMethod(
                                         FunctionMetadata::create((void*)zip, LIST, 0, true, false), "zip", zip_doc));
    builtins_module->giveAttr(
        "dir", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)dir, LIST, 1, false, false), "dir",
                                                { NULL }, NULL, dir_doc));
    builtins_module->giveAttr(
        "vars", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)vars, UNKNOWN, 1, false, false),
                                                 "vars", { NULL }, NULL, vars_doc));
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

    builtins_module->giveAttr(
        "eval", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)eval, UNKNOWN, 3, false, false),
                                                 "eval", { NULL, NULL }, NULL, eval_doc));
    builtins_module->giveAttr("callable",
                              new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)callable, UNKNOWN, 1),
                                                               "callable", callable_doc));

    builtins_module->giveAttr("raw_input", new BoxedBuiltinFunctionOrMethod(
                                               FunctionMetadata::create((void*)rawInput, UNKNOWN, 1, false, false),
                                               "raw_input", { NULL }, NULL, raw_input_doc));
    builtins_module->giveAttr(
        "input", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)input, UNKNOWN, 1, false, false),
                                                  "input", { NULL }, NULL, input_doc));
    builtins_module->giveAttr("cmp", new BoxedBuiltinFunctionOrMethod(
                                         FunctionMetadata::create((void*)builtinCmp, UNKNOWN, 2), "cmp", cmp_doc));
    builtins_module->giveAttr(
        "format", new BoxedBuiltinFunctionOrMethod(FunctionMetadata::create((void*)builtinFormat, UNKNOWN, 2), "format",
                                                   format_doc));
}
}
