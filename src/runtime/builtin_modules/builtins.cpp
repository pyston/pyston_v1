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
#include "runtime/capi.h"
#include "runtime/classobj.h"
#include "runtime/file.h"
#include "runtime/ics.h"
#include "runtime/import.h"
#include "runtime/inline/xrange.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/super.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
// Copied from CPython:
#if defined(MS_WINDOWS) && defined(HAVE_USABLE_WCHAR_T)
const char* Py_FileSystemDefaultEncoding = "mbcs";
#elif defined(__APPLE__)
const char* Py_FileSystemDefaultEncoding = "utf-8";
#else
const char* Py_FileSystemDefaultEncoding = NULL; /* use default */
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

static int merge_class_dict(PyObject* dict, PyObject* aclass) {
    PyObject* classdict;
    PyObject* bases;

    assert(PyDict_Check(dict));
    assert(aclass);

    /* Merge in the type's dict (if any). */
    classdict = PyObject_GetAttrString(aclass, "__dict__");
    if (classdict == NULL)
        PyErr_Clear();
    else {
        int status = PyDict_Update(dict, classdict);
        Py_DECREF(classdict);
        if (status < 0)
            return -1;
    }

    /* Recursively merge in the base types' (if any) dicts. */
    bases = PyObject_GetAttrString(aclass, "__bases__");
    if (bases == NULL)
        PyErr_Clear();
    else {
        /* We have no guarantee that bases is a real tuple */
        Py_ssize_t i, n;
        n = PySequence_Size(bases); /* This better be right */
        if (n < 0)
            PyErr_Clear();
        else {
            for (i = 0; i < n; i++) {
                int status;
                PyObject* base = PySequence_GetItem(bases, i);
                if (base == NULL) {
                    Py_DECREF(bases);
                    return -1;
                }
                status = merge_class_dict(dict, base);
                Py_DECREF(base);
                if (status < 0) {
                    Py_DECREF(bases);
                    return -1;
                }
            }
        }
        Py_DECREF(bases);
    }
    return 0;
}
/* Helper for PyObject_Dir of type objects: returns __dict__ and __bases__.
   We deliberately don't suck up its __class__, as methods belonging to the
   metaclass would probably be more confusing than helpful.
*/
static PyObject* _specialized_dir_type(PyObject* obj) {
    PyObject* result = NULL;
    PyObject* dict = PyDict_New();

    if (dict != NULL && merge_class_dict(dict, obj) == 0)
        result = PyDict_Keys(dict);

    Py_XDECREF(dict);
    return result;
}

extern "C" Box* dir(Box* obj) {
    if (obj == NULL) {
        // TODO: This should actually return the elements in the current local
        // scope not the content of the builtins_module
        obj = builtins_module;
    }

    // TODO: Recursive class traversal for lookup of types and eliminating
    // duplicates afterwards
    BoxedList* result = nullptr;
    // If __dir__ is present just call it and return what it returns
    static std::string attr_dir = "__dir__";
    Box* dir_result = callattrInternal(obj, &attr_dir, CLASS_ONLY, nullptr, ArgPassSpec(0), nullptr, nullptr, nullptr,
                                       nullptr, nullptr);
    if (dir_result && dir_result->cls == list_cls) {
        return dir_result;
    }

    if (isSubclass(obj->cls, type_cls)) {
        Box* r = _specialized_dir_type(obj);
        checkAndThrowCAPIException();
        assert(r);
        return r;
    }

    // If __dict__ is present use its keys and add the reset below
    Box* obj_dict = getattrInternal(obj, "__dict__", nullptr);
    if (obj_dict && obj_dict->cls == dict_cls) {
        result = new BoxedList();
        for (auto& kv : static_cast<BoxedDict*>(obj_dict)->d) {
            listAppend(result, kv.first);
        }
    }
    if (!result) {
        result = new BoxedList();
    }

    for (auto const& kv : obj->cls->attrs.hcls->attr_offsets) {
        listAppend(result, boxString(kv.first));
    }
    if (obj->cls->instancesHaveHCAttrs()) {
        HCAttrs* attrs = obj->getHCAttrsPtr();
        for (auto const& kv : attrs->hcls->attr_offsets) {
            listAppend(result, boxString(kv.first));
        }
    }
    if (obj->cls->instancesHaveDictAttrs()) {
        Py_FatalError("unimplemented");
    }
    return result;
}

extern "C" Box* vars(Box* obj) {
    RELEASE_ASSERT(obj, "Don't support 0-arg vars() calls yet");

    return makeAttrWrapper(obj);
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
        static const std::string abs_str("__abs__");
        return callattr(x, &abs_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = false }), ArgPassSpec(0),
                        NULL, NULL, NULL, NULL, NULL);
    }
}

extern "C" Box* hexFunc(Box* x) {
    static const std::string hex_str("__hex__");
    Box* r = callattr(x, &hex_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = true }), ArgPassSpec(0),
                      NULL, NULL, NULL, NULL, NULL);
    if (!r)
        raiseExcHelper(TypeError, "hex() argument can't be converted to hex");

    if (!isSubclass(r->cls, str_cls))
        raiseExcHelper(TypeError, "__hex__() returned non-string (type %.200s)", r->cls->tp_name);

    return r;
}

extern "C" Box* octFunc(Box* x) {
    static const std::string oct_str("__oct__");
    Box* r = callattr(x, &oct_str, CallattrFlags({.cls_only = true, .null_on_nonexistent = true }), ArgPassSpec(0),
                      NULL, NULL, NULL, NULL, NULL);
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

extern "C" Box* min(Box* arg0, BoxedTuple* args) {
    assert(args->cls == tuple_cls);

    Box* minElement;
    Box* container;
    if (args->elts.size() == 0) {
        minElement = nullptr;
        container = arg0;
    } else {
        minElement = arg0;
        container = args;
    }

    for (Box* e : container->pyElements()) {
        if (!minElement) {
            minElement = e;
        } else {
            Box* comp_result = compareInternal(minElement, e, AST_TYPE::Gt, NULL);
            if (nonzero(comp_result)) {
                minElement = e;
            }
        }
    }

    if (!minElement) {
        raiseExcHelper(ValueError, "min() arg is an empty sequence");
    }
    return minElement;
}

extern "C" Box* max(Box* arg0, BoxedTuple* args) {
    assert(args->cls == tuple_cls);

    Box* maxElement;
    Box* container;
    if (args->elts.size() == 0) {
        maxElement = nullptr;
        container = arg0;
    } else {
        maxElement = arg0;
        container = args;
    }

    for (Box* e : container->pyElements()) {
        if (!maxElement) {
            maxElement = e;
        } else {
            Box* comp_result = compareInternal(maxElement, e, AST_TYPE::Lt, NULL);
            if (nonzero(comp_result)) {
                maxElement = e;
            }
        }
    }

    if (!maxElement) {
        raiseExcHelper(ValueError, "max() arg is an empty sequence");
    }
    return maxElement;
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


Box* open(Box* arg1, Box* arg2) {
    assert(arg2);
    // This could be optimized quite a bit if it ends up being important:
    return runtimeCall(file_cls, ArgPassSpec(2), arg1, arg2, NULL, NULL, NULL);
}

extern "C" Box* chr(Box* arg) {
    if (arg->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }
    i64 n = static_cast<BoxedInt*>(arg)->n;
    if (n < 0 || n >= 256) {
        raiseExcHelper(ValueError, "chr() arg not in range(256)");
    }

    return boxString(std::string(1, (char)n));
}

extern "C" Box* ord(Box* arg) {
    if (arg->cls != str_cls) {
        raiseExcHelper(TypeError, "ord() expected string of length 1, but %s found", getTypeName(arg));
    }
    const std::string& s = static_cast<BoxedString*>(arg)->s;

    if (s.size() != 1)
        raiseExcHelper(TypeError, "ord() expected string of length 1, but string of length %d found", s.size());

    return boxInt(s[0]);
}

Box* range(Box* start, Box* stop, Box* step) {
    i64 istart, istop, istep;
    if (stop == NULL) {
        RELEASE_ASSERT(isSubclass(start->cls, int_cls), "%s", getTypeName(start));

        istart = 0;
        istop = static_cast<BoxedInt*>(start)->n;
        istep = 1;
    } else if (step == NULL) {
        RELEASE_ASSERT(isSubclass(start->cls, int_cls), "%s", getTypeName(start));
        RELEASE_ASSERT(isSubclass(stop->cls, int_cls), "%s", getTypeName(stop));

        istart = static_cast<BoxedInt*>(start)->n;
        istop = static_cast<BoxedInt*>(stop)->n;
        istep = 1;
    } else {
        RELEASE_ASSERT(isSubclass(start->cls, int_cls), "%s", getTypeName(start));
        RELEASE_ASSERT(isSubclass(stop->cls, int_cls), "%s", getTypeName(stop));
        RELEASE_ASSERT(isSubclass(step->cls, int_cls), "%s", getTypeName(step));

        istart = static_cast<BoxedInt*>(start)->n;
        istop = static_cast<BoxedInt*>(stop)->n;
        istep = static_cast<BoxedInt*>(step)->n;
        RELEASE_ASSERT(istep != 0, "step can't be 0");
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
    return boxStrConstant("NotImplemented");
}

Box* sorted(Box* obj) {
    BoxedList* rtn = new BoxedList();
    for (Box* e : obj->pyElements()) {
        listAppendInternal(rtn, e);
    }

    std::sort<Box**, PyLt>(rtn->elts->elts, rtn->elts->elts + rtn->size, PyLt());

    return rtn;
}

Box* sortedList(Box* obj) {
    RELEASE_ASSERT(obj->cls == list_cls, "");

    BoxedList* lobj = static_cast<BoxedList*>(obj);
    BoxedList* rtn = new BoxedList();

    int size = lobj->size;
    rtn->elts = new (size) GCdArray();
    rtn->size = size;
    rtn->capacity = size;
    for (int i = 0; i < size; i++) {
        Box* t = rtn->elts->elts[i] = lobj->elts->elts[i];
    }

    std::sort<Box**, PyLt>(rtn->elts->elts, rtn->elts->elts + size, PyLt());

    return rtn;
}

Box* isinstance_func(Box* obj, Box* cls) {
    return boxBool(isinstance(obj, cls, 0));
}

Box* issubclass_func(Box* child, Box* parent) {
    if (child->cls != type_cls && child->cls != classobj_cls)
        raiseExcHelper(TypeError, "issubclass() arg 1 must be a class");

    RELEASE_ASSERT(parent->cls != tuple_cls, "unsupported");

    if (child->cls == classobj_cls) {
        if (parent->cls != classobj_cls)
            return False;

        return boxBool(classobjIssubclass(static_cast<BoxedClassobj*>(child), static_cast<BoxedClassobj*>(parent)));
    }

    assert(child->cls == type_cls);
    if (parent->cls != type_cls)
        return False;

    return boxBool(isSubclass(static_cast<BoxedClass*>(child), static_cast<BoxedClass*>(parent)));
}

Box* bltinImport(Box* arg) {
    if (arg->cls != str_cls) {
        raiseExcHelper(TypeError, "__import__() argument 1 must be string, not %s", getTypeName(arg));
    }

    return import(-1, new BoxedTuple({}), &static_cast<BoxedString*>(arg)->s);
}

Box* getattrFunc(Box* obj, Box* _str, Box* default_value) {
    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "getattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);

    Box* rtn = NULL;
    try {
        rtn = getattrInternal(obj, str->s, NULL);
    } catch (ExcInfo e) {
        if (!e.matches(AttributeError))
            throw e;
    }

    if (!rtn) {
        if (default_value)
            return default_value;
        else
            raiseExcHelper(AttributeError, "'%s' object has no attribute '%s'", getTypeName(obj), str->s.c_str());
    }

    return rtn;
}

Box* setattrFunc(Box* obj, Box* _str, Box* value) {
    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "getattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    setattrInternal(obj, str->s, value, NULL);
    return None;
}

Box* hasattr(Box* obj, Box* _str) {
    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "hasattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    Box* attr;
    try {
        attr = getattrInternal(obj, str->s, NULL);
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
    for (Box* e : container->pyElements()) {
        listAppendInternal(rtn, runtimeCall(f, ArgPassSpec(1), e, NULL, NULL, NULL, NULL));
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

Box* filter2(Box* f, Box* container) {
    // If the filter-function argument is None, filter() works by only returning
    // the elements that are truthy.  This is equivalent to using the bool() constructor.
    // - actually since we call nonzero() afterwards, we could use an ident() function
    //   but we don't have one.
    // If this is a common case we could speed it up with special handling.
    if (f == None)
        f = bool_cls;

    Box* rtn = new BoxedList();
    for (Box* e : container->pyElements()) {
        Box* r = runtimeCall(f, ArgPassSpec(1), e, NULL, NULL, NULL, NULL);
        bool b = nonzero(r);
        if (b)
            listAppendInternal(rtn, e);
    }
    return rtn;
}

Box* zip2(Box* container1, Box* container2) {
    BoxedList* rtn = new BoxedList();

    llvm::iterator_range<BoxIterator> range1 = container1->pyElements();
    llvm::iterator_range<BoxIterator> range2 = container2->pyElements();

    BoxIterator it1 = range1.begin();
    BoxIterator it2 = range2.begin();

    for (; it1 != range1.end() && it2 != range2.end(); ++it1, ++it2) {
        BoxedTuple::GCVector elts{ *it1, *it2 };
        listAppendInternal(rtn, new BoxedTuple(std::move(elts)));
    }
    return rtn;
}

BoxedClass* notimplemented_cls;
BoxedModule* builtins_module;

// TODO looks like CPython and pypy put this into an "exceptions" module:
extern "C" {
BoxedClass* BaseException, *Exception, *StandardError, *AssertionError, *AttributeError, *GeneratorExit, *TypeError,
    *NameError, *KeyError, *IndexError, *IOError, *OSError, *ZeroDivisionError, *ValueError, *UnboundLocalError,
    *RuntimeError, *ImportError, *StopIteration, *Warning, *SyntaxError, *OverflowError, *DeprecationWarning,
    *MemoryError, *LookupError, *EnvironmentError, *ArithmeticError, *BufferError, *KeyboardInterrupt, *SystemExit,
    *SystemError, *NotImplementedError, *PendingDeprecationWarning, *EOFError;

Box* PyExc_RecursionErrorInst;
Box* PyExc_MemoryErrorInst;
}

Box* exceptionNew1(BoxedClass* cls) {
    return exceptionNew(cls, EmptyTuple);
}

class BoxedException : public Box {
public:
    HCAttrs attrs;
    BoxedException() {}

    static Box* __reduce__(Box* self) {
        RELEASE_ASSERT(isSubclass(self->cls, BaseException), "");
        BoxedException* exc = static_cast<BoxedException*>(self);

        return new BoxedTuple({ self->cls, EmptyTuple, makeAttrWrapper(self) });
    }
};

Box* exceptionNew2(BoxedClass* cls, Box* message) {
    return exceptionNew(cls, new BoxedTuple({ message }));
}

Box* exceptionNew(BoxedClass* cls, BoxedTuple* args) {
    if (!isSubclass(cls->cls, type_cls))
        raiseExcHelper(TypeError, "exceptions.__new__(X): X is not a type object (%s)", getTypeName(cls));

    if (!isSubclass(cls, BaseException))
        raiseExcHelper(TypeError, "BaseException.__new__(%s): %s is not a subtype of BaseException",
                       getNameOfClass(cls), getNameOfClass(cls));

    BoxedException* rtn = new (cls) BoxedException();

    // TODO: this should be a MemberDescriptor and set during init
    if (args->elts.size() == 1)
        rtn->giveAttr("message", args->elts[0]);
    else
        rtn->giveAttr("message", boxStrConstant(""));
    return rtn;
}

Box* exceptionStr(Box* b) {
    // TODO In CPython __str__ and __repr__ pull from an internalized message field, but for now do this:
    Box* message = b->getattr("message");
    assert(message);
    message = str(message);
    assert(message->cls == str_cls);

    return message;
}

Box* exceptionRepr(Box* b) {
    // TODO In CPython __str__ and __repr__ pull from an internalized message field, but for now do this:
    Box* message = b->getattr("message");
    assert(message);
    message = repr(message);
    assert(message->cls == str_cls);

    BoxedString* message_s = static_cast<BoxedString*>(message);
    return boxString(std::string(getTypeName(b)) + "(" + message_s->s + ",)");
}

static BoxedClass* makeBuiltinException(BoxedClass* base, const char* name, int size = 0) {
    if (size == 0)
        size = base->tp_basicsize;

    BoxedClass* cls = new BoxedHeapClass(base, NULL, offsetof(BoxedException, attrs), size, false, name);
    cls->giveAttr("__module__", boxStrConstant("exceptions"));

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
    RELEASE_ASSERT(_base == NULL, "unimplemented");
    RELEASE_ASSERT(dict == NULL, "unimplemented");

    try {
        char* dot_pos = strchr(name, '.');
        RELEASE_ASSERT(dot_pos, "");
        int n = strlen(name);
        BoxedString* boxedName = boxStrConstantSize(dot_pos + 1, n - (dot_pos - name) - 1);

        BoxedClass* base = Exception;
        BoxedClass* cls
            = new BoxedHeapClass(base, NULL, offsetof(BoxedException, attrs), sizeof(BoxedException), true, boxedName);

        cls->giveAttr("__module__", boxStrConstantSize(name, dot_pos - name));
        // TODO Not sure if this should be called here
        fixup_slot_dispatchers(cls);
        return cls;
    } catch (ExcInfo e) {
        abort();
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
        return new BoxedTuple({ boxInt(self->idx++), val });
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
    BoxedModule* m = getCurrentModule();
    // TODO is it ok that we don't return a real dict here?
    return makeAttrWrapper(m);
}

Box* locals() {
    return getLocals(true /* filter */);
}

Box* divmod(Box* lhs, Box* rhs) {
    return binopInternal(lhs, rhs, AST_TYPE::DivMod, false, NULL);
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
    bool exists = llvm::sys::fs::exists(fn->s);
#endif

    if (!exists)
        raiseExcHelper(IOError, "No such file or directory: '%s'", fn->s.c_str());

    // Run directly inside the current module:
    AST_Module* ast = caching_parse(fn->s.c_str());
    compileAndRunModule(ast, getCurrentModule());

    return None;
}

Box* print(BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(kwargs->cls == dict_cls);

    Box* dest, *end;

    auto it = kwargs->d.find(new BoxedString("file"));
    if (it != kwargs->d.end()) {
        dest = it->second;
        kwargs->d.erase(it);
    } else {
        dest = getSysStdout();
    }

    it = kwargs->d.find(new BoxedString("end"));
    if (it != kwargs->d.end()) {
        end = it->second;
        kwargs->d.erase(it);
    } else {
        end = new BoxedString("\n");
    }

    RELEASE_ASSERT(kwargs->d.size() == 0, "print() got unexpected keyword arguments");

    static const std::string write_str("write");

    Box* space_box = new BoxedString(" ");

    // TODO softspace handling?
    bool first = true;
    for (auto e : args->elts) {
        BoxedString* s = str(e);

        if (!first) {
            Box* r = callattr(dest, &write_str, CallattrFlags({.cls_only = false, .null_on_nonexistent = false }),
                              ArgPassSpec(1), space_box, NULL, NULL, NULL, NULL);
            RELEASE_ASSERT(r, "");
        }
        first = false;

        Box* r = callattr(dest, &write_str, CallattrFlags({.cls_only = false, .null_on_nonexistent = false }),
                          ArgPassSpec(1), s, NULL, NULL, NULL, NULL);
        RELEASE_ASSERT(r, "");
    }

    Box* r = callattr(dest, &write_str, CallattrFlags({.cls_only = false, .null_on_nonexistent = false }),
                      ArgPassSpec(1), end, NULL, NULL, NULL, NULL);
    RELEASE_ASSERT(r, "");

    return None;
}

Box* pydump(void* p) {
    dump(p);
    return None;
}

class BoxedEnvironmentError : public BoxedException {
public:
    // Box* args, *message, *myerrno, *strerror, *filename;
    Box* myerrno, *strerror, *filename;

    static Box* __init__(BoxedEnvironmentError* self, Box* errno_, Box* strerror, Box** _args) {
        Box* filename = _args[0];

        if (!errno_)
            return None;

        RELEASE_ASSERT(isSubclass(self->cls, EnvironmentError), "");

        self->myerrno = errno_;
        self->strerror = strerror;
        self->filename = filename;
        return None;
    }

    static Box* __reduce__(Box* self) { Py_FatalError("unimplemented"); }

    static PyObject* __str__(BoxedEnvironmentError* self) noexcept {
        PyObject* rtnval = NULL;

        if (self->filename) {
            PyObject* fmt;
            PyObject* repr;
            PyObject* tuple;

            fmt = PyString_FromString("[Errno %s] %s: %s");
            if (!fmt)
                return NULL;

            repr = PyObject_Repr(self->filename);
            if (!repr) {
                Py_DECREF(fmt);
                return NULL;
            }
            tuple = PyTuple_New(3);
            if (!tuple) {
                Py_DECREF(repr);
                Py_DECREF(fmt);
                return NULL;
            }

            if (self->myerrno) {
                Py_INCREF(self->myerrno);
                PyTuple_SET_ITEM(tuple, 0, self->myerrno);
            } else {
                Py_INCREF(Py_None);
                PyTuple_SET_ITEM(tuple, 0, Py_None);
            }
            if (self->strerror) {
                Py_INCREF(self->strerror);
                PyTuple_SET_ITEM(tuple, 1, self->strerror);
            } else {
                Py_INCREF(Py_None);
                PyTuple_SET_ITEM(tuple, 1, Py_None);
            }

            PyTuple_SET_ITEM(tuple, 2, repr);

            rtnval = PyString_Format(fmt, tuple);

            Py_DECREF(fmt);
            Py_DECREF(tuple);
        } else if (self->myerrno && self->strerror) {
            PyObject* fmt;
            PyObject* tuple;

            fmt = PyString_FromString("[Errno %s] %s");
            if (!fmt)
                return NULL;

            tuple = PyTuple_New(2);
            if (!tuple) {
                Py_DECREF(fmt);
                return NULL;
            }

            if (self->myerrno) {
                Py_INCREF(self->myerrno);
                PyTuple_SET_ITEM(tuple, 0, self->myerrno);
            } else {
                Py_INCREF(Py_None);
                PyTuple_SET_ITEM(tuple, 0, Py_None);
            }
            if (self->strerror) {
                Py_INCREF(self->strerror);
                PyTuple_SET_ITEM(tuple, 1, self->strerror);
            } else {
                Py_INCREF(Py_None);
                PyTuple_SET_ITEM(tuple, 1, Py_None);
            }

            rtnval = PyString_Format(fmt, tuple);

            Py_DECREF(fmt);
            Py_DECREF(tuple);
        } else
            rtnval = exceptionStr(self);

        return rtnval;
    }

    static void gcHandler(GCVisitor* v, Box* _b) {
        assert(isSubclass(_b->cls, EnvironmentError));

        boxGCHandler(v, _b);

        BoxedEnvironmentError* ee = static_cast<BoxedEnvironmentError*>(_b);
        if (ee->myerrno)
            v->visit(ee->myerrno);
        if (ee->strerror)
            v->visit(ee->strerror);
        if (ee->filename)
            v->visit(ee->filename);
    }
};

void setupBuiltins() {
    builtins_module = createModule("__builtin__", "__builtin__");

    builtins_module->giveAttr("None", None);

    builtins_module->giveAttr("__debug__", False);

    builtins_module->giveAttr("print",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)print, NONE, 0, 0, true, true)));

    notimplemented_cls = new BoxedHeapClass(object_cls, NULL, 0, sizeof(Box), false, "NotImplementedType");
    notimplemented_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)notimplementedRepr, STR, 1)));
    notimplemented_cls->freeze();
    NotImplemented = new (notimplemented_cls) Box();
    gc::registerPermanentRoot(NotImplemented);

    builtins_module->giveAttr("NotImplemented", NotImplemented);
    builtins_module->giveAttr("NotImplementedType", notimplemented_cls);

    builtins_module->giveAttr("all", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)all, BOXED_BOOL, 1)));
    builtins_module->giveAttr("any", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)any, BOXED_BOOL, 1)));

    BaseException = makeBuiltinException(object_cls, "BaseException", sizeof(BoxedException));
    Exception = makeBuiltinException(BaseException, "Exception");
    StandardError = makeBuiltinException(Exception, "StandardError");
    AssertionError = makeBuiltinException(StandardError, "AssertionError");
    AttributeError = makeBuiltinException(StandardError, "AttributeError");
    GeneratorExit = makeBuiltinException(BaseException, "GeneratorExit");
    TypeError = makeBuiltinException(StandardError, "TypeError");
    NameError = makeBuiltinException(StandardError, "NameError");
    LookupError = makeBuiltinException(StandardError, "LookupError");
    KeyError = makeBuiltinException(LookupError, "KeyError");
    IndexError = makeBuiltinException(LookupError, "IndexError");
    EnvironmentError = makeBuiltinException(StandardError, "EnvironmentError", sizeof(BoxedEnvironmentError));
    IOError = makeBuiltinException(EnvironmentError, "IOError");
    OSError = makeBuiltinException(EnvironmentError, "OSError");
    ArithmeticError = makeBuiltinException(StandardError, "ArithmeticError");
    ZeroDivisionError = makeBuiltinException(ArithmeticError, "ZeroDivisionError");
    ValueError = makeBuiltinException(StandardError, "ValueError");
    UnboundLocalError = makeBuiltinException(NameError, "UnboundLocalError");
    RuntimeError = makeBuiltinException(StandardError, "RuntimeError");
    ImportError = makeBuiltinException(StandardError, "ImportError");
    StopIteration = makeBuiltinException(Exception, "StopIteration");
    Warning = makeBuiltinException(Exception, "Warning");
    SyntaxError = makeBuiltinException(StandardError, "SyntaxError");
    OverflowError = makeBuiltinException(ArithmeticError, "OverflowError");
    /*ImportWarning =*/makeBuiltinException(Warning, "ImportWarning");
    DeprecationWarning = makeBuiltinException(Warning, "DeprecationWarning");
    /*BytesWarning =*/makeBuiltinException(Warning, "BytesWarning");
    MemoryError = makeBuiltinException(StandardError, "MemoryError");
    BufferError = makeBuiltinException(StandardError, "BufferError");
    KeyboardInterrupt = makeBuiltinException(BaseException, "KeyboardInterrupt");
    SystemExit = makeBuiltinException(BaseException, "SystemExit");
    SystemError = makeBuiltinException(StandardError, "SystemError");
    NotImplementedError = makeBuiltinException(RuntimeError, "NotImplementedError");
    PendingDeprecationWarning = makeBuiltinException(Warning, "PendingDeprecationWarning");
    EOFError = makeBuiltinException(StandardError, "EOFError");

    EnvironmentError->gc_visit = BoxedEnvironmentError::gcHandler;
    EnvironmentError->giveAttr(
        "__init__", new BoxedFunction(boxRTFunction((void*)BoxedEnvironmentError::__init__, NONE, 4, 3, false, false),
                                      { NULL, NULL, NULL }));
    EnvironmentError->giveAttr(
        "errno", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT, offsetof(BoxedEnvironmentError, myerrno)));
    EnvironmentError->giveAttr("strerror", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                     offsetof(BoxedEnvironmentError, strerror)));
    EnvironmentError->giveAttr("filename", new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT,
                                                                     offsetof(BoxedEnvironmentError, filename)));
    EnvironmentError->giveAttr("__str__",
                               new BoxedFunction(boxRTFunction((void*)BoxedEnvironmentError::__str__, UNKNOWN, 1)));

    repr_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)repr, UNKNOWN, 1));
    builtins_module->giveAttr("repr", repr_obj);
    len_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)len, UNKNOWN, 1));
    builtins_module->giveAttr("len", len_obj);
    hash_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)hash, UNKNOWN, 1));
    builtins_module->giveAttr("hash", hash_obj);
    abs_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)abs_, UNKNOWN, 1));
    builtins_module->giveAttr("abs", abs_obj);
    builtins_module->giveAttr("hex", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)hexFunc, UNKNOWN, 1)));
    builtins_module->giveAttr("oct", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)octFunc, UNKNOWN, 1)));

    min_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)min, UNKNOWN, 1, 0, true, false));
    builtins_module->giveAttr("min", min_obj);

    max_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)max, UNKNOWN, 1, 0, true, false));
    builtins_module->giveAttr("max", max_obj);

    builtins_module->giveAttr(
        "sum", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)sum, UNKNOWN, 2, 1, false, false), { boxInt(0) }));

    id_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)id, BOXED_INT, 1));
    builtins_module->giveAttr("id", id_obj);
    chr_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)chr, STR, 1));
    builtins_module->giveAttr("chr", chr_obj);
    ord_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)ord, BOXED_INT, 1));
    builtins_module->giveAttr("ord", ord_obj);
    trap_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)trap, UNKNOWN, 0));
    builtins_module->giveAttr("trap", trap_obj);
    builtins_module->giveAttr("dump", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)pydump, UNKNOWN, 1)));

    builtins_module->giveAttr("getattr", new BoxedBuiltinFunctionOrMethod(
                                             boxRTFunction((void*)getattrFunc, UNKNOWN, 3, 1, false, false), { NULL }));

    builtins_module->giveAttr(
        "setattr", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)setattrFunc, UNKNOWN, 3, 0, false, false)));

    Box* hasattr_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)hasattr, BOXED_BOOL, 2));
    builtins_module->giveAttr("hasattr", hasattr_obj);


    Box* isinstance_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)isinstance_func, BOXED_BOOL, 2));
    builtins_module->giveAttr("isinstance", isinstance_obj);

    Box* issubclass_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)issubclass_func, BOXED_BOOL, 2));
    builtins_module->giveAttr("issubclass", issubclass_obj);

    builtins_module->giveAttr("__import__",
                              new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)bltinImport, UNKNOWN, 1)));

    enumerate_cls
        = new BoxedHeapClass(object_cls, &BoxedEnumerate::gcHandler, 0, sizeof(BoxedEnumerate), false, "enumerate");
    enumerate_cls->giveAttr(
        "__new__",
        new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::new_, UNKNOWN, 3, 1, false, false), { boxInt(0) }));
    enumerate_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::iter, typeFromClass(enumerate_cls), 1)));
    enumerate_cls->giveAttr(
        "next", new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::next, typeFromClass(enumerate_cls), 1)));
    enumerate_cls->giveAttr("__hasnext__", new BoxedFunction(boxRTFunction((void*)BoxedEnumerate::hasnext,
                                                                           typeFromClass(enumerate_cls), 1)));
    enumerate_cls->freeze();
    builtins_module->giveAttr("enumerate", enumerate_cls);


    CLFunction* sorted_func = createRTFunction(1, 0, false, false);
    addRTFunction(sorted_func, (void*)sortedList, LIST, { LIST });
    addRTFunction(sorted_func, (void*)sorted, LIST, { UNKNOWN });
    builtins_module->giveAttr("sorted", new BoxedBuiltinFunctionOrMethod(sorted_func));

    builtins_module->giveAttr("True", True);
    builtins_module->giveAttr("False", False);

    range_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)range, LIST, 3, 2, false, false), { NULL, NULL });
    builtins_module->giveAttr("range", range_obj);

    setupXrange();
    builtins_module->giveAttr("xrange", xrange_cls);

    open_obj = new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)open, typeFromClass(file_cls), 2, 1, false, false),
                                                { boxStrConstant("r") });
    builtins_module->giveAttr("open", open_obj);

    builtins_module->giveAttr(
        "globals", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)globals, UNKNOWN, 0, 0, false, false)));
    builtins_module->giveAttr(
        "locals", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)locals, UNKNOWN, 0, 0, false, false)));

    builtins_module->giveAttr(
        "iter", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)getiter, UNKNOWN, 1, 0, false, false)));

    builtins_module->giveAttr("divmod", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)divmod, UNKNOWN, 2)));

    builtins_module->giveAttr("execfile", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)execfile, UNKNOWN, 1)));

    builtins_module->giveAttr("map", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)map2, LIST, 2)));
    builtins_module->giveAttr("reduce", new BoxedBuiltinFunctionOrMethod(
                                            boxRTFunction((void*)reduce, UNKNOWN, 3, 1, false, false), { NULL }));
    builtins_module->giveAttr("filter", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)filter2, LIST, 2)));
    builtins_module->giveAttr("zip", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)zip2, LIST, 2)));
    builtins_module->giveAttr(
        "dir", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)dir, LIST, 1, 1, false, false), { NULL }));
    builtins_module->giveAttr(
        "vars", new BoxedBuiltinFunctionOrMethod(boxRTFunction((void*)vars, LIST, 1, 1, false, false), { NULL }));
    builtins_module->giveAttr("object", object_cls);
    builtins_module->giveAttr("str", str_cls);
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


    PyExc_RecursionErrorInst = new (RuntimeError) BoxedException();
    gc::registerPermanentRoot(PyExc_RecursionErrorInst);
    PyExc_MemoryErrorInst = new (MemoryError) BoxedException();
    gc::registerPermanentRoot(PyExc_MemoryErrorInst);
}
}
