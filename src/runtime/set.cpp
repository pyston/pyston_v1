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

#include "runtime/set.h"

#include "runtime/objmodel.h"

namespace pyston {

BoxedClass* set_iterator_cls;

extern "C" Box* createSet() {
    return new BoxedSet();
}

void BoxedSet::gcHandler(GCVisitor* v, Box* b) {
    Box::gcHandler(v, b);

    BoxedSet* s = (BoxedSet*)b;
    for (auto&& p : s->s) {
        v->visit(&p.value);
    }
}

namespace set {

class BoxedSetIterator : public Box {
public:
    BoxedSet* s;
    decltype(BoxedSet::s)::iterator it;

    BoxedSetIterator(BoxedSet* s) : s(s), it(s->s.begin()) {}

    DEFAULT_CLASS(set_iterator_cls);

    bool hasNext() { return it != s->s.end(); }

    Box* next() {
        Box* rtn = it->value;
        ++it;
        return rtn;
    }

    static void gcHandler(GCVisitor* v, Box* b) {
        Box::gcHandler(v, b);

        BoxedSetIterator* it = (BoxedSetIterator*)b;

        v->visit(&it->s);
    }
};

Box* setiteratorHasnext(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    return boxBool(self->hasNext());
}

Box* setiteratorNext(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    return self->next();
}

Box* setiter_next(Box* _self) noexcept {
    RELEASE_ASSERT(_self->cls == set_iterator_cls, "");
    BoxedSetIterator* self = (BoxedSetIterator*)_self;
    if (!self->hasNext())
        return NULL;
    return self->next();
}

Box* setiteratorIter(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    return self;
}

Box* setAdd2(Box* _self, Box* b) {
    RELEASE_ASSERT(isSubclass(_self->cls, set_cls), "");
    BoxedSet* self = static_cast<BoxedSet*>(_self);

    self->s.insert(b);
    return None;
}

// Creates a set of type 'cls' from 'container' (NULL to get an empty set).
// Works for frozenset and normal set types.
BoxedSet* makeNewSet(BoxedClass* cls, Box* container) {
    assert(isSubclass(cls, frozenset_cls) || isSubclass(cls, set_cls));

    BoxedSet* rtn = new (cls) BoxedSet();

    if (container) {
        for (Box* e : container->pyElements()) {
            rtn->s.insert(e);
        }
    }
    return rtn;
}

Box* frozensetNew(Box* _cls, Box* container) {
    RELEASE_ASSERT(_cls->cls == type_cls, "");
    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    RELEASE_ASSERT(isSubclass(cls, frozenset_cls), "");

    // Some optimizations from CPython: frozensets can be shared:
    if (_cls == frozenset_cls) {
        if (!container) {
            static Box* emptyfrozenset = PyGC_AddRoot(new (frozenset_cls) BoxedSet());
            return emptyfrozenset;
        }
        if (container->cls == frozenset_cls)
            return container;
    }

    return makeNewSet(cls, container);
}

Box* setNew(Box* _cls, Box* container) {
    RELEASE_ASSERT(_cls->cls == type_cls, "");
    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    RELEASE_ASSERT(isSubclass(cls, set_cls), "");

    // Note: set.__new__ explicitly ignores the container argument.
    return makeNewSet(cls, NULL);
}

Box* setInit(Box* _self, Box* container) {
    RELEASE_ASSERT(PySet_Check(_self), "");

    if (!container)
        return None;

    BoxedSet* self = static_cast<BoxedSet*>(_self);

    for (Box* e : container->pyElements()) {
        self->s.insert(e);
    }
    return None;
}

static Box* setRepr(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    std::vector<char> chars;
    int status = Py_ReprEnter((PyObject*)self);

    if (status != 0) {
        if (status < 0)
            throwCAPIException();

        std::string ty = std::string(self->cls->tp_name);
        chars.insert(chars.end(), ty.begin(), ty.end());
        chars.push_back('(');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back(')');

        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }

    try {
        std::string ty = std::string(self->cls->tp_name);
        chars.insert(chars.end(), ty.begin(), ty.end());

        chars.push_back('(');
        chars.push_back('[');

        bool first = true;
        for (auto&& elt : self->s) {

            if (!first) {
                chars.push_back(',');
                chars.push_back(' ');
            }
            BoxedString* str = static_cast<BoxedString*>(repr(elt.value));
            chars.insert(chars.end(), str->s().begin(), str->s().end());

            first = false;
        }
        chars.push_back(']');
        chars.push_back(')');
    } catch (ExcInfo e) {
        Py_ReprLeave((PyObject*)self);
        throw e;
    }
    Py_ReprLeave((PyObject*)self);
    return boxString(llvm::StringRef(&chars[0], chars.size()));
}

static void _setSymmetricDifferenceUpdate(BoxedSet* self, Box* other) {
    if (!PyAnySet_Check(other))
        other = makeNewSet(self->cls, other);

    BoxedSet* other_set = static_cast<BoxedSet*>(other);
    for (auto elt : other_set->s) {
        bool found = self->s.erase(elt);
        if (!found)
            self->s.insert(elt);
    }
}

static BoxedSet* setIntersection2(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    BoxedSet* rtn = new BoxedSet();
    for (auto elt : container->pyElements()) {
        if (self->s.count(elt))
            rtn->s.insert(elt);
    }
    return rtn;
}

static Box* setIntersectionUpdate2(BoxedSet* self, Box* other) {
    Box* tmp = setIntersection2(self, other);
    std::swap(self->s, ((BoxedSet*)tmp)->s);
    return None;
}

Box* setIOr(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    // TODO just [write and] call setUnionUpdate2
    for (auto&& elt : rhs->s) {
        lhs->s.insert(elt);
    }
    return lhs;
}

Box* setOr(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    BoxedSet* rtn = makeNewSet(lhs->cls, lhs);
    return setIOr(rtn, rhs);
}

Box* setIAnd(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    setIntersectionUpdate2(lhs, rhs);
    return lhs;
}

Box* setAnd(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    return setIntersection2(lhs, rhs);
}

Box* setISub(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    // TODO: write and call setDifferenceUpdate2
    for (auto&& elt : rhs->s) {
        lhs->s.erase(elt);
    }
    return lhs;
}

Box* setSub(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    BoxedSet* rtn = makeNewSet(lhs->cls, lhs);
    return setISub(rtn, rhs);
}

Box* setIXor(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    _setSymmetricDifferenceUpdate(lhs, rhs);

    return lhs;
}

Box* setXor(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return NotImplemented;

    BoxedSet* rtn = makeNewSet(lhs->cls, lhs);
    return setIXor(rtn, rhs);
}

Box* setIter(BoxedSet* self) noexcept {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    return new BoxedSetIterator(self);
}

Box* setLen(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    return boxInt(self->s.size());
}

Box* setAdd(BoxedSet* self, Box* v) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "%s", self->cls->tp_name);

    self->s.insert(v);
    return None;
}

// Note: PySet_Add is allowed to apply to frozenset objects, though CPython has
// an check to make sure the refcount is 1.
// for example, the marshal library uses this to construct frozenset objects.
extern "C" int PySet_Add(PyObject* set, PyObject* key) noexcept {
    if (!PyAnySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }

    try {
        static_cast<BoxedSet*>(set)->s.insert(key);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* setRemove(BoxedSet* self, Box* v) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    auto it = self->s.find(v);
    if (it == self->s.end()) {
        raiseExcHelper(KeyError, v);
    }

    self->s.erase(it);
    return None;
}

Box* setDiscard(BoxedSet* self, Box* v) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    auto it = self->s.find(v);
    if (it != self->s.end())
        self->s.erase(it);

    return None;
}

Box* setClear(BoxedSet* self, Box* v) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    self->s.clear();
    return None;
}

extern "C" int PySet_Clear(PyObject* set) noexcept {
    if (!PySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    ((BoxedSet*)set)->s.clear();
    return 0;
}

Box* setUpdate(BoxedSet* self, BoxedTuple* args) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    assert(args->cls == tuple_cls);

    for (auto l : *args) {
        if (l->cls == set_cls) {
            BoxedSet* s2 = static_cast<BoxedSet*>(l);
            self->s.insert(s2->s.begin(), s2->s.end());
        } else {
            for (auto e : l->pyElements()) {
                self->s.insert(e);
            }
        }
    }

    return None;
}

Box* setUnion(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'union' requires a 'set' object but received a '%s'", getTypeName(self));

    BoxedSet* rtn = new BoxedSet();
    rtn->s.insert(self->s.begin(), self->s.end());

    for (auto container : args->pyElements()) {
        for (auto elt : container->pyElements()) {
            rtn->s.insert(elt);
        }
    }

    return rtn;
}

static void _setDifferenceUpdate(BoxedSet* self, BoxedTuple* args) {
    for (auto container : *args) {
        for (auto elt : container->pyElements()) {
            self->s.erase(elt);
        }
    }
}

Box* setDifferenceUpdate(BoxedSet* self, BoxedTuple* args) {
    if (!PySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'difference_update' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    _setDifferenceUpdate(self, args);
    return None;
}

Box* setDifference(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'difference' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    BoxedSet* rtn = makeNewSet(self->cls, self);
    _setDifferenceUpdate(rtn, args);
    return rtn;
}

Box* setSymmetricDifferenceUpdate(BoxedSet* self, Box* other) {
    if (!PySet_Check(self))
        raiseExcHelper(TypeError,
                       "descriptor 'symmetric_difference_update' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    _setSymmetricDifferenceUpdate(self, other);
    return None;
}

Box* setSymmetricDifference(BoxedSet* self, Box* other) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'symmetric_difference' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    BoxedSet* rtn = makeNewSet(self->cls, self);
    _setSymmetricDifferenceUpdate(rtn, other);
    return rtn;
}

static Box* setIssubset(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (!PyAnySet_Check(container)) {
        container = makeNewSet(set_cls, container);
    }
    assert(PyAnySet_Check(container));

    BoxedSet* rhs = static_cast<BoxedSet*>(container);
    if (self->s.size() > rhs->s.size())
        return False;

    for (auto e : self->s) {
        if (rhs->s.find(e) == rhs->s.end())
            return False;
    }
    return True;
}

static Box* setIssuperset(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (!PyAnySet_Check(container)) {
        container = makeNewSet(set_cls, container);
    }
    assert(PyAnySet_Check(container));
    return setIssubset((BoxedSet*)container, self);
}

static Box* setIsdisjoint(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    for (auto e : container->pyElements()) {
        if (self->s.find(e) != self->s.end())
            return False;
    }
    return True;
}

static Box* setIntersection(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'intersection' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    if (args->size() == 0)
        return makeNewSet(self->cls, self);

    BoxedSet* rtn = self;
    for (auto container : *args) {
        rtn = setIntersection2(rtn, container);
    }
    return rtn;
}

static Box* setIntersectionUpdate(BoxedSet* self, BoxedTuple* args) {
    Box* tmp = setIntersection(self, args);
    std::swap(self->s, ((BoxedSet*)tmp)->s);
    return None;
}

Box* setCopy(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    BoxedSet* rtn = new BoxedSet();
    rtn->s = self->s;
    return rtn;
}

Box* setPop(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    if (!self->s.size())
        raiseExcHelper(KeyError, "pop from an empty set");

    auto it = self->s.begin();
    Box* rtn = it->value;
    self->s.erase(it);
    return rtn;
}

Box* setContains(BoxedSet* self, Box* v) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    return boxBool(self->s.find(v) != self->s.end());
}

Box* setEq(BoxedSet* self, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    if (!PyAnySet_Check(rhs))
        return False;

    if (self->s.size() != rhs->s.size())
        return False;

    return setIssubset(self, rhs);
}

Box* setNe(BoxedSet* self, BoxedSet* rhs) {
    Box* r = setEq(self, rhs);
    assert(r->cls == bool_cls);
    return boxBool(r == False);
}

Box* setLe(BoxedSet* self, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    if (!PyAnySet_Check(rhs))
        raiseExcHelper(TypeError, "can only compare to a set");

    return setIssubset(self, rhs);
}

Box* setLt(BoxedSet* self, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    if (!PyAnySet_Check(rhs))
        raiseExcHelper(TypeError, "can only compare to a set");

    if (self->s.size() >= rhs->s.size())
        return False;

    return setIssubset(self, rhs);
}

Box* setGe(BoxedSet* self, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    if (!PyAnySet_Check(rhs))
        raiseExcHelper(TypeError, "can only compare to a set");

    return setIssuperset(self, rhs);
}

Box* setGt(BoxedSet* self, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    if (!PyAnySet_Check(rhs))
        raiseExcHelper(TypeError, "can only compare to a set");

    if (self->s.size() <= rhs->s.size())
        return False;

    return setIssuperset(self, rhs);
}

Box* setNonzero(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    return boxBool(self->s.size());
}

Box* setHash(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, frozenset_cls), "");

    int64_t rtn = 1927868237L;
    for (auto&& e : self->s) {
        rtn ^= e.hash + 0x9e3779b9 + (rtn << 6) + (rtn >> 2);
    }

    return boxInt(rtn);
}

extern "C" PyObject* PySet_New(PyObject* iterable) noexcept {
    if (!iterable)
        return new BoxedSet(); // Fast path for empty set.

    try {
        return runtimeCall(set_cls, ArgPassSpec(iterable ? 1 : 0), iterable, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyFrozenSet_New(PyObject* iterable) noexcept {
    try {
        return runtimeCall(frozenset_cls, ArgPassSpec(iterable ? 1 : 0), iterable, NULL, NULL, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

} // namespace set

void BoxedSet::dealloc(Box* b) noexcept {
    assert(PyAnySet_Check(b));
    static_cast<BoxedSet*>(b)->s.freeAllMemory();
}

using namespace pyston::set;

void setupSet() {
    static PySequenceMethods set_as_sequence;
    set_cls->tp_as_sequence = &set_as_sequence;
    static PyNumberMethods set_as_number;
    set_cls->tp_as_number = &set_as_number;
    static PySequenceMethods frozenset_as_sequence;
    frozenset_cls->tp_as_sequence = &frozenset_as_sequence;
    static PyNumberMethods frozenset_as_number;
    frozenset_cls->tp_as_number = &frozenset_as_number;

    set_cls->tp_dealloc = frozenset_cls->tp_dealloc = BoxedSet::dealloc;
    set_cls->has_safe_tp_dealloc = frozenset_cls->has_safe_tp_dealloc = true;

    set_iterator_cls = BoxedClass::create(type_cls, object_cls, &BoxedSetIterator::gcHandler, 0, 0,
                                          sizeof(BoxedSetIterator), false, "setiterator");
    set_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)setiteratorIter, typeFromClass(set_iterator_cls), 1)));
    set_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)setiteratorHasnext, BOXED_BOOL, 1)));
    set_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)setiteratorNext, UNKNOWN, 1)));
    set_iterator_cls->freeze();
    set_iterator_cls->tp_iternext = setiter_next;

    set_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)setNew, UNKNOWN, 2, false, false), { NULL }));
    set_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)setInit, UNKNOWN, 2, false, false), { NULL }));
    frozenset_cls->giveAttr("__new__",
                            new BoxedFunction(boxRTFunction((void*)frozensetNew, UNKNOWN, 2, false, false), { NULL }));

    Box* set_repr = new BoxedFunction(boxRTFunction((void*)setRepr, STR, 1));
    set_cls->giveAttr("__repr__", set_repr);
    set_cls->giveAttr("__str__", set_repr);
    frozenset_cls->giveAttr("__repr__", set_repr);
    frozenset_cls->giveAttr("__str__", set_repr);

    std::vector<ConcreteCompilerType*> v_ss, v_sf, v_su, v_ff, v_fs, v_fu;
    v_ss.push_back(SET);
    v_ss.push_back(SET);
    v_sf.push_back(SET);
    v_sf.push_back(FROZENSET);
    v_su.push_back(SET);
    v_su.push_back(UNKNOWN);

    v_ff.push_back(FROZENSET);
    v_ff.push_back(FROZENSET);
    v_fs.push_back(FROZENSET);
    v_fs.push_back(SET);
    v_fu.push_back(FROZENSET);
    v_fu.push_back(UNKNOWN);

    auto add = [&](const char* name, void* func) {
        auto func_obj = new BoxedFunction(boxRTFunction((void*)func, UNKNOWN, 2, false, false));
        set_cls->giveAttr(name, func_obj);
        frozenset_cls->giveAttr(name, func_obj);
        /*
        CLFunction* func_obj = boxRTFunction(2, false, false);
        addRTFunction(func_obj, (void*)func, SET, v_ss);
        addRTFunction(func_obj, (void*)func, SET, v_sf);
        addRTFunction(func_obj, (void*)func, FROZENSET, v_fs);
        addRTFunction(func_obj, (void*)func, FROZENSET, v_ff);
        set_cls->giveAttr(name, new BoxedFunction(func_obj));
        frozenset_cls->giveAttr(name, set_cls->getattr(internStringMortal(name)));
        */
    };

    add("__or__", (void*)setOr);
    add("__sub__", (void*)setSub);
    add("__xor__", (void*)setXor);
    add("__and__", (void*)setAnd);
    add("__ior__", (void*)setIOr);
    add("__isub__", (void*)setISub);
    add("__ixor__", (void*)setIXor);
    add("__iand__", (void*)setIAnd);


    set_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)setIter, typeFromClass(set_iterator_cls), 1)));
    frozenset_cls->giveAttr("__iter__", set_cls->getattr(internStringMortal("__iter__")));

    set_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)setLen, BOXED_INT, 1)));
    frozenset_cls->giveAttr("__len__", set_cls->getattr(internStringMortal("__len__")));

    set_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)setContains, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__contains__", set_cls->getattr(internStringMortal("__contains__")));

    set_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)setEq, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__eq__", set_cls->getattr(internStringMortal("__eq__")));
    set_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)setNe, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__ne__", set_cls->getattr(internStringMortal("__ne__")));
    set_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)setLe, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__le__", set_cls->getattr(internStringMortal("__le__")));
    set_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)setLt, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__lt__", set_cls->getattr(internStringMortal("__lt__")));
    set_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)setGe, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__ge__", set_cls->getattr(internStringMortal("__ge__")));
    set_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)setGt, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__gt__", set_cls->getattr(internStringMortal("__gt__")));

    set_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)setNonzero, BOXED_BOOL, 1)));
    frozenset_cls->giveAttr("__nonzero__", set_cls->getattr(internStringMortal("__nonzero__")));

    frozenset_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)setHash, BOXED_INT, 1)));
    set_cls->giveAttr("__hash__", None);

    set_cls->giveAttr("add", new BoxedFunction(boxRTFunction((void*)setAdd, NONE, 2)));
    set_cls->giveAttr("remove", new BoxedFunction(boxRTFunction((void*)setRemove, NONE, 2)));
    set_cls->giveAttr("discard", new BoxedFunction(boxRTFunction((void*)setDiscard, NONE, 2)));

    set_cls->giveAttr("clear", new BoxedFunction(boxRTFunction((void*)setClear, NONE, 1)));
    set_cls->giveAttr("update", new BoxedFunction(boxRTFunction((void*)setUpdate, NONE, 1, true, false)));
    set_cls->giveAttr("union", new BoxedFunction(boxRTFunction((void*)setUnion, UNKNOWN, 1, true, false)));
    frozenset_cls->giveAttr("union", set_cls->getattr(internStringMortal("union")));
    set_cls->giveAttr("intersection",
                      new BoxedFunction(boxRTFunction((void*)setIntersection, UNKNOWN, 1, true, false)));
    frozenset_cls->giveAttr("intersection", set_cls->getattr(internStringMortal("intersection")));
    set_cls->giveAttr("intersection_update",
                      new BoxedFunction(boxRTFunction((void*)setIntersectionUpdate, UNKNOWN, 1, true, false)));
    set_cls->giveAttr("difference", new BoxedFunction(boxRTFunction((void*)setDifference, UNKNOWN, 1, true, false)));
    frozenset_cls->giveAttr("difference", set_cls->getattr(internStringMortal("difference")));
    set_cls->giveAttr("difference_update",
                      new BoxedFunction(boxRTFunction((void*)setDifferenceUpdate, UNKNOWN, 1, true, false)));
    set_cls->giveAttr("symmetric_difference",
                      new BoxedFunction(boxRTFunction((void*)setSymmetricDifference, UNKNOWN, 2, false, false)));
    frozenset_cls->giveAttr("symmetric_difference", set_cls->getattr(internStringMortal("symmetric_difference")));
    set_cls->giveAttr("symmetric_difference_update",
                      new BoxedFunction(boxRTFunction((void*)setSymmetricDifferenceUpdate, UNKNOWN, 2, false, false)));
    set_cls->giveAttr("issubset", new BoxedFunction(boxRTFunction((void*)setIssubset, UNKNOWN, 2)));
    frozenset_cls->giveAttr("issubset", set_cls->getattr(internStringMortal("issubset")));
    set_cls->giveAttr("issuperset", new BoxedFunction(boxRTFunction((void*)setIssuperset, UNKNOWN, 2)));
    frozenset_cls->giveAttr("issuperset", set_cls->getattr(internStringMortal("issuperset")));
    set_cls->giveAttr("isdisjoint", new BoxedFunction(boxRTFunction((void*)setIsdisjoint, UNKNOWN, 2)));
    frozenset_cls->giveAttr("isdisjoint", set_cls->getattr(internStringMortal("isdisjoint")));

    set_cls->giveAttr("copy", new BoxedFunction(boxRTFunction((void*)setCopy, UNKNOWN, 1)));
    set_cls->giveAttr("pop", new BoxedFunction(boxRTFunction((void*)setPop, UNKNOWN, 1)));

    set_cls->freeze();
    frozenset_cls->freeze();

    set_cls->tp_iter = (decltype(set_cls->tp_iter))setIter;
    frozenset_cls->tp_iter = (decltype(frozenset_cls->tp_iter))setIter;
}

void teardownSet() {
}
}
