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

#include <algorithm>
#include <cstddef>
#include <err.h>

#include "llvm/Support/FileSystem.h"

#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "core/ast.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/inline/xrange.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/super.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" Box* trap() {
    raise(SIGTRAP);

    return None;
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
    if (obj->cls->instancesHaveAttrs()) {
        HCAttrs* attrs = obj->getAttrsPtr();
        for (auto const& kv : attrs->hcls->attr_offsets) {
            listAppend(result, boxString(kv.first));
        }
    }
    return result;
}

extern "C" Box* abs_(Box* x) {
    if (x->cls == int_cls) {
        i64 n = static_cast<BoxedInt*>(x)->n;
        return boxInt(n >= 0 ? n : -n);
    } else if (x->cls == float_cls) {
        double d = static_cast<BoxedFloat*>(x)->d;
        return boxFloat(d >= 0 ? d : -d);
    } else if (x->cls == long_cls) {
        return longAbs(static_cast<BoxedLong*>(x));
    } else {
        RELEASE_ASSERT(0, "%s", getTypeName(x)->c_str());
    }
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

    Box* cur = initial;
    for (Box* e : container->pyElements()) {
        cur = binopInternal(cur, e, AST_TYPE::Add, false, NULL);
    }
    return cur;
}

extern "C" Box* id(Box* arg) {
    i64 addr = (i64)(arg) ^ 0xdeadbeef00000003;
    return boxInt(addr);
}


Box* open(Box* arg1, Box* arg2) {
    assert(arg2);

    if (arg1->cls != str_cls) {
        fprintf(stderr, "TypeError: coercing to Unicode: need string of buffer, %s found\n",
                getTypeName(arg1)->c_str());
        raiseExcHelper(TypeError, "");
    }
    if (arg2->cls != str_cls) {
        fprintf(stderr, "TypeError: coercing to Unicode: need string of buffer, %s found\n",
                getTypeName(arg2)->c_str());
        raiseExcHelper(TypeError, "");
    }

    const std::string& fn = static_cast<BoxedString*>(arg1)->s;
    const std::string& mode = static_cast<BoxedString*>(arg2)->s;

    FILE* f = fopen(fn.c_str(), mode.c_str());
    if (!f)
        raiseExcHelper(IOError, "%s: '%s' '%s'", strerror(errno), fn.c_str());

    return new BoxedFile(f);
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
        raiseExcHelper(TypeError, "ord() expected string of length 1, but %s found", getTypeName(arg)->c_str());
    }
    const std::string& s = static_cast<BoxedString*>(arg)->s;

    if (s.size() != 1)
        raiseExcHelper(TypeError, "ord() expected string of length 1, but string of length %d found", s.size());

    return boxInt(s[0]);
}

Box* range(Box* start, Box* stop, Box* step) {
    i64 istart, istop, istep;
    if (stop == NULL) {
        RELEASE_ASSERT(start->cls == int_cls, "%s", getTypeName(start)->c_str());

        istart = 0;
        istop = static_cast<BoxedInt*>(start)->n;
        istep = 1;
    } else if (step == NULL) {
        RELEASE_ASSERT(start->cls == int_cls, "%s", getTypeName(start)->c_str());
        RELEASE_ASSERT(stop->cls == int_cls, "%s", getTypeName(stop)->c_str());

        istart = static_cast<BoxedInt*>(start)->n;
        istop = static_cast<BoxedInt*>(stop)->n;
        istep = 1;
    } else {
        RELEASE_ASSERT(start->cls == int_cls, "%s", getTypeName(start)->c_str());
        RELEASE_ASSERT(stop->cls == int_cls, "%s", getTypeName(stop)->c_str());
        RELEASE_ASSERT(step->cls == int_cls, "%s", getTypeName(step)->c_str());

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
    RELEASE_ASSERT(child->cls == type_cls, "");
    // TODO parent can also be a tuple of classes
    RELEASE_ASSERT(parent->cls == type_cls, "");

    return boxBool(isSubclass(static_cast<BoxedClass*>(child), static_cast<BoxedClass*>(parent)));
}

Box* getattrFunc(Box* obj, Box* _str, Box* default_value) {
    if (_str->cls != str_cls) {
        raiseExcHelper(TypeError, "getattr(): attribute name must be string");
    }

    BoxedString* str = static_cast<BoxedString*>(_str);
    Box* rtn = getattrInternal(obj, str->s, NULL);

    if (!rtn) {
        if (default_value)
            return default_value;
        else
            raiseExcHelper(AttributeError, "'%s' object has no attribute '%s'", getTypeName(obj)->c_str(),
                           str->s.c_str());
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
    Box* attr = getattrInternal(obj, str->s, NULL);

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
    *MemoryError, *LookupError, *EnvironmentError, *ArithmeticError, *BufferError;
}

Box* exceptionNew1(BoxedClass* cls) {
    return exceptionNew2(cls, boxStrConstant(""));
}

class BoxedException : public Box {
public:
    HCAttrs attrs;
    BoxedException(BoxedClass* cls) : Box(cls) {}
};

Box* exceptionNew2(BoxedClass* cls, Box* message) {
    assert(cls->tp_basicsize == sizeof(BoxedException));
    Box* r = new BoxedException(cls);
    // TODO: maybe this should be a MemberDescriptor?
    r->giveAttr("message", message);
    return r;
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
    return boxString(*getTypeName(b) + "(" + message_s->s + ",)");
}

static BoxedClass* makeBuiltinException(BoxedClass* base, const char* name) {
    BoxedClass* cls
        = new BoxedClass(type_cls, base, NULL, offsetof(BoxedException, attrs), sizeof(BoxedException), false);
    cls->giveAttr("__name__", boxStrConstant(name));
    cls->giveAttr("__module__", boxStrConstant("exceptions"));

    if (base == object_cls) {
        cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)exceptionNew2, UNKNOWN, 2, 1, false, false),
                                                   { boxStrConstant("") }));
        cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)exceptionStr, STR, 1)));
        cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)exceptionRepr, STR, 1)));
    }

    cls->freeze();

    builtins_module->giveAttr(name, cls);
    return cls;
}

extern "C" PyObject* PyErr_NewException(char* name, PyObject* _base, PyObject* dict) {
    RELEASE_ASSERT(_base == NULL, "unimplemented");
    RELEASE_ASSERT(dict == NULL, "unimplemented");

    try {
        BoxedClass* base = Exception;
        BoxedClass* cls
            = new BoxedClass(type_cls, base, NULL, offsetof(BoxedException, attrs), sizeof(BoxedException), true);

        char* dot_pos = strchr(name, '.');
        RELEASE_ASSERT(dot_pos, "");
        int n = strlen(name);

        cls->giveAttr("__module__", boxStrConstantSize(name, dot_pos - name));
        cls->giveAttr("__name__", boxStrConstantSize(dot_pos + 1, n - (dot_pos - name) - 1));
        return cls;
    } catch (Box* e) {
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
        : Box(enumerate_cls), iterator(iterator_begin), iterator_end(iterator_end), idx(idx) {}

    static Box* new_(Box* cls, Box* obj, Box* start) {
        RELEASE_ASSERT(cls == enumerate_cls, "");
        RELEASE_ASSERT(start->cls == int_cls, "");
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
        return new BoxedTuple({ boxInt(self->idx++), *self->iterator++ });
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

Box* divmod(Box* lhs, Box* rhs) {
    return binopInternal(lhs, rhs, AST_TYPE::DivMod, false, NULL);
}

Box* execfile(Box* _fn) {
    // The "globals" and "locals" arguments aren't implemented for now
    if (!isSubclass(_fn->cls, str_cls)) {
        raiseExcHelper(TypeError, "must be string, not %s", getTypeName(_fn)->c_str());
    }

    BoxedString* fn = static_cast<BoxedString*>(_fn);

    bool exists;
    llvm::error_code code = llvm::sys::fs::exists(fn->s, exists);
#if LLVMREV < 210072
    ASSERT(code == 0, "%s: %s", code.message().c_str(), fn->s.c_str());
#else
    assert(!code);
#endif
    if (!exists)
        raiseExcHelper(IOError, "No such file or directory: '%s'", fn->s.c_str());

    // Run directly inside the current module:
    AST_Module* ast = caching_parse(fn->s.c_str());
    compileAndRunModule(ast, getCurrentModule());

    return None;
}

void setupBuiltins() {
    builtins_module = createModule("__builtin__", "__builtin__");

    builtins_module->giveAttr("None", None);

    notimplemented_cls = new BoxedClass(type_cls, object_cls, NULL, 0, sizeof(Box), false);
    notimplemented_cls->giveAttr("__name__", boxStrConstant("NotImplementedType"));
    notimplemented_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)notimplementedRepr, STR, 1)));
    notimplemented_cls->freeze();
    NotImplemented = new Box(notimplemented_cls);
    gc::registerPermanentRoot(NotImplemented);

    builtins_module->giveAttr("NotImplemented", NotImplemented);
    builtins_module->giveAttr("NotImplementedType", notimplemented_cls);

    builtins_module->giveAttr("all", new BoxedFunction(boxRTFunction((void*)all, BOXED_BOOL, 1)));
    builtins_module->giveAttr("any", new BoxedFunction(boxRTFunction((void*)any, BOXED_BOOL, 1)));

    BaseException = makeBuiltinException(object_cls, "BaseException");
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
    EnvironmentError = makeBuiltinException(StandardError, "EnvironmentError");
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
    /*PendingDeprecationWarning =*/makeBuiltinException(Warning, "PendingDeprecationWarning");
    DeprecationWarning = makeBuiltinException(Warning, "DeprecationWarning");
    /*BytesWarning =*/makeBuiltinException(Warning, "BytesWarning");
    MemoryError = makeBuiltinException(StandardError, "MemoryError");
    BufferError = makeBuiltinException(StandardError, "BufferError");
    /*NotImplementedError=*/makeBuiltinException(RuntimeError, "NotImplementedError");

    repr_obj = new BoxedFunction(boxRTFunction((void*)repr, UNKNOWN, 1));
    builtins_module->giveAttr("repr", repr_obj);
    len_obj = new BoxedFunction(boxRTFunction((void*)len, UNKNOWN, 1));
    builtins_module->giveAttr("len", len_obj);
    hash_obj = new BoxedFunction(boxRTFunction((void*)hash, UNKNOWN, 1));
    builtins_module->giveAttr("hash", hash_obj);
    abs_obj = new BoxedFunction(boxRTFunction((void*)abs_, UNKNOWN, 1));
    builtins_module->giveAttr("abs", abs_obj);

    min_obj = new BoxedFunction(boxRTFunction((void*)min, UNKNOWN, 1, 0, true, false));
    builtins_module->giveAttr("min", min_obj);

    max_obj = new BoxedFunction(boxRTFunction((void*)max, UNKNOWN, 1, 0, true, false));
    builtins_module->giveAttr("max", max_obj);

    builtins_module->giveAttr("sum",
                              new BoxedFunction(boxRTFunction((void*)sum, UNKNOWN, 2, 1, false, false), { boxInt(0) }));

    id_obj = new BoxedFunction(boxRTFunction((void*)id, BOXED_INT, 1));
    builtins_module->giveAttr("id", id_obj);
    chr_obj = new BoxedFunction(boxRTFunction((void*)chr, STR, 1));
    builtins_module->giveAttr("chr", chr_obj);
    ord_obj = new BoxedFunction(boxRTFunction((void*)ord, BOXED_INT, 1));
    builtins_module->giveAttr("ord", ord_obj);
    trap_obj = new BoxedFunction(boxRTFunction((void*)trap, UNKNOWN, 0));
    builtins_module->giveAttr("trap", trap_obj);

    builtins_module->giveAttr(
        "getattr", new BoxedFunction(boxRTFunction((void*)getattrFunc, UNKNOWN, 3, 1, false, false), { NULL }));

    builtins_module->giveAttr("setattr",
                              new BoxedFunction(boxRTFunction((void*)setattrFunc, UNKNOWN, 3, 0, false, false)));

    Box* hasattr_obj = new BoxedFunction(boxRTFunction((void*)hasattr, BOXED_BOOL, 2));
    builtins_module->giveAttr("hasattr", hasattr_obj);


    Box* isinstance_obj = new BoxedFunction(boxRTFunction((void*)isinstance_func, BOXED_BOOL, 2));
    builtins_module->giveAttr("isinstance", isinstance_obj);

    Box* issubclass_obj = new BoxedFunction(boxRTFunction((void*)issubclass_func, BOXED_BOOL, 2));
    builtins_module->giveAttr("issubclass", issubclass_obj);


    enumerate_cls = new BoxedClass(type_cls, object_cls, &BoxedEnumerate::gcHandler, 0, sizeof(BoxedEnumerate), false);
    enumerate_cls->giveAttr("__name__", boxStrConstant("enumerate"));
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
    builtins_module->giveAttr("sorted", new BoxedFunction(sorted_func));

    builtins_module->giveAttr("True", True);
    builtins_module->giveAttr("False", False);

    range_obj = new BoxedFunction(boxRTFunction((void*)range, LIST, 3, 2, false, false), { NULL, NULL });
    builtins_module->giveAttr("range", range_obj);

    setupXrange();
    builtins_module->giveAttr("xrange", xrange_cls);

    open_obj = new BoxedFunction(boxRTFunction((void*)open, typeFromClass(file_cls), 2, 1, false, false),
                                 { boxStrConstant("r") });
    builtins_module->giveAttr("open", open_obj);

    builtins_module->giveAttr("globals", new BoxedFunction(boxRTFunction((void*)globals, UNKNOWN, 0, 0, false, false)));

    builtins_module->giveAttr("iter", new BoxedFunction(boxRTFunction((void*)getiter, UNKNOWN, 1, 0, false, false)));

    builtins_module->giveAttr("divmod", new BoxedFunction(boxRTFunction((void*)divmod, UNKNOWN, 2)));

    builtins_module->giveAttr("execfile", new BoxedFunction(boxRTFunction((void*)execfile, UNKNOWN, 1)));

    builtins_module->giveAttr("map", new BoxedFunction(boxRTFunction((void*)map2, LIST, 2)));
    builtins_module->giveAttr("reduce",
                              new BoxedFunction(boxRTFunction((void*)reduce, UNKNOWN, 3, 1, false, false), { NULL }));
    builtins_module->giveAttr("filter", new BoxedFunction(boxRTFunction((void*)filter2, LIST, 2)));
    builtins_module->giveAttr("zip", new BoxedFunction(boxRTFunction((void*)zip2, LIST, 2)));
    builtins_module->giveAttr("dir", new BoxedFunction(boxRTFunction((void*)dir, LIST, 1, 1, false, false), { NULL }));
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
}
}
