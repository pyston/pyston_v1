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

#include <llvm/Support/raw_ostream.h>

#include "gc/collector.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedClass* set_iterator_cls;

extern "C" Box* createSet() {
    return new BoxedSet();
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
        Box* rtn = *it;
        ++it;
        return rtn;
    }
};

extern "C" void setIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedSetIterator* it = (BoxedSetIterator*)b;

    v->visit(it->s);
}

Box* setiteratorHasnext(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    return boxBool(self->hasNext());
}

Box* setiteratorNext(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
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

Box* setNew(Box* _cls, Box* container) {
    RELEASE_ASSERT(_cls->cls == type_cls, "");
    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    RELEASE_ASSERT(isSubclass(cls, set_cls) || isSubclass(cls, frozenset_cls), "");

    BoxedSet* rtn = new (cls) BoxedSet();

    if (container == None)
        return rtn;

    for (Box* e : container->pyElements()) {
        rtn->s.insert(e);
    }

    return rtn;
}

static Box* _setRepr(BoxedSet* self, const char* type_name) {
    std::string O("");
    llvm::raw_string_ostream os(O);

    int status = Py_ReprEnter((PyObject*)self);
    if (status != 0) {
        if (status < 0)
            return boxString(os.str());

        os << type_name << "(...)";
        return boxString(os.str());
    }
    os << type_name << "([";
    bool first = true;
    for (Box* elt : self->s) {
        if (!first) {
            os << ", ";
        }
        os << static_cast<BoxedString*>(repr(elt))->s();
        first = false;
    }
    os << "])";
    Py_ReprLeave((PyObject*)self);
    return boxString(os.str());
}

Box* setRepr(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");
    return _setRepr(self, "set");
}

Box* frozensetRepr(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, frozenset_cls), "");
    return _setRepr(self, "frozenset");
}

Box* setOrSet(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    RELEASE_ASSERT(PyAnySet_Check(rhs), "");

    BoxedSet* rtn = new (lhs->cls) BoxedSet();

    for (Box* elt : lhs->s) {
        rtn->s.insert(elt);
    }
    for (Box* elt : rhs->s) {
        rtn->s.insert(elt);
    }
    return rtn;
}

Box* setAndSet(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    RELEASE_ASSERT(PyAnySet_Check(rhs), "");

    BoxedSet* rtn = new (lhs->cls) BoxedSet();

    for (Box* elt : lhs->s) {
        if (rhs->s.count(elt))
            rtn->s.insert(elt);
    }
    return rtn;
}

Box* setSubSet(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    RELEASE_ASSERT(PyAnySet_Check(rhs), "");

    BoxedSet* rtn = new (lhs->cls) BoxedSet();

    for (Box* elt : lhs->s) {
        // TODO if len(rhs) << len(lhs), it might be more efficient
        // to delete the elements of rhs from lhs?
        if (rhs->s.count(elt) == 0)
            rtn->s.insert(elt);
    }
    return rtn;
}

Box* setXorSet(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    RELEASE_ASSERT(PyAnySet_Check(rhs), "");

    BoxedSet* rtn = new (lhs->cls) BoxedSet();

    for (Box* elt : lhs->s) {
        if (rhs->s.count(elt) == 0)
            rtn->s.insert(elt);
    }

    for (Box* elt : rhs->s) {
        if (lhs->s.count(elt) == 0)
            rtn->s.insert(elt);
    }

    return rtn;
}

Box* setIter(BoxedSet* self) {
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

Box* setDifference(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'difference' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    BoxedSet* rtn = (BoxedSet*)setNew(self->cls, self);

    for (auto container : args->pyElements()) {
        for (auto elt : container->pyElements()) {
            rtn->s.erase(elt);
        }
    }

    return rtn;
}

Box* setDifferenceUpdate(BoxedSet* self, BoxedTuple* args) {
    if (!PySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'difference' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    for (auto container : args->pyElements()) {
        for (auto elt : container->pyElements()) {
            self->s.erase(elt);
        }
    }

    return None;
}

static Box* setIssubset(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (!PyAnySet_Check(container)) {
        container = setNew(set_cls, container);
    }
    assert(PyAnySet_Check(container));

    BoxedSet* rhs = static_cast<BoxedSet*>(container);
    for (auto e : self->s) {
        if (rhs->s.find(e) == rhs->s.end())
            return False;
    }
    return True;
}

static Box* setIssuperset(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (!PyAnySet_Check(container)) {
        container = setNew(set_cls, container);
    }
    assert(PyAnySet_Check(container));

    BoxedSet* rhs = static_cast<BoxedSet*>(container);
    for (auto e : rhs->s) {
        if (self->s.find(e) == self->s.end())
            return False;
    }
    return True;
}

static Box* setIsdisjoint(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    for (auto e : container->pyElements()) {
        if (self->s.find(e) != self->s.end())
            return False;
    }
    return True;
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

static Box* setIntersection(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'intersection' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    BoxedSet* rtn = self;
    for (auto container : args->pyElements()) {
        rtn = setIntersection2(rtn, container);
    }
    return rtn;
}

Box* setCopy(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    BoxedSet* rtn = new BoxedSet();
    rtn->s.insert(self->s.begin(), self->s.end());
    return rtn;
}

Box* setPop(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    if (!self->s.size())
        raiseExcHelper(KeyError, "pop from an empty set");

    auto it = self->s.begin();
    Box* rtn = *it;
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
        return NotImplemented;

    if (self->s.size() != rhs->s.size())
        return False;

    for (auto e : self->s) {
        if (!rhs->s.count(e))
            return False;
    }
    return True;
}

Box* setNe(BoxedSet* self, BoxedSet* rhs) {
    Box* r = setEq(self, rhs);
    if (r->cls == bool_cls)
        return boxBool(r == False);
    assert(r == NotImplemented);
    return r;
}

Box* setNonzero(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    return boxBool(self->s.size());
}

Box* setHash(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, frozenset_cls), "");

    int64_t rtn = 1927868237L;
    for (Box* e : self->s) {
        BoxedInt* h = hash(e);
        assert(isSubclass(h->cls, int_cls));
        rtn ^= h->n + 0x9e3779b9 + (rtn << 6) + (rtn >> 2);
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

using namespace pyston::set;

void setupSet() {
    set_iterator_cls = BoxedHeapClass::create(type_cls, object_cls, &setIteratorGCHandler, 0, 0,
                                              sizeof(BoxedSetIterator), false, "setiterator");
    set_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)setiteratorIter, typeFromClass(set_iterator_cls), 1)));
    set_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)setiteratorHasnext, BOXED_BOOL, 1)));
    set_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)setiteratorNext, UNKNOWN, 1)));
    set_iterator_cls->freeze();

    set_cls->giveAttr("__new__",
                      new BoxedFunction(boxRTFunction((void*)setNew, UNKNOWN, 2, 1, false, false), { None }));
    frozenset_cls->giveAttr("__new__", set_cls->getattr(internStringMortal("__new__")));

    Box* set_repr = new BoxedFunction(boxRTFunction((void*)setRepr, STR, 1));
    set_cls->giveAttr("__repr__", set_repr);
    set_cls->giveAttr("__str__", set_repr);

    Box* frozenset_repr = new BoxedFunction(boxRTFunction((void*)frozensetRepr, STR, 1));
    frozenset_cls->giveAttr("__repr__", frozenset_repr);
    frozenset_cls->giveAttr("__str__", frozenset_repr);

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
        CLFunction* func_obj = createRTFunction(2, 0, false, false);
        addRTFunction(func_obj, (void*)func, SET, v_ss);
        addRTFunction(func_obj, (void*)func, SET, v_sf);
        addRTFunction(func_obj, (void*)func, FROZENSET, v_fs);
        addRTFunction(func_obj, (void*)func, FROZENSET, v_ff);
        set_cls->giveAttr(name, new BoxedFunction(func_obj));
        frozenset_cls->giveAttr(name, set_cls->getattr(internStringMortal(name)));
    };

    add("__or__", (void*)setOrSet);
    add("__sub__", (void*)setSubSet);
    add("__xor__", (void*)setXorSet);
    add("__and__", (void*)setAndSet);

    set_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)setIter, typeFromClass(set_iterator_cls), 1)));
    frozenset_cls->giveAttr("__iter__", set_cls->getattr(internStringMortal("__iter__")));

    set_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)setLen, BOXED_INT, 1)));
    frozenset_cls->giveAttr("__len__", set_cls->getattr(internStringMortal("__len__")));

    set_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)setContains, BOXED_BOOL, 2)));
    frozenset_cls->giveAttr("__contains__", set_cls->getattr(internStringMortal("__contains__")));

    set_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)setEq, UNKNOWN, 2)));
    frozenset_cls->giveAttr("__eq__", set_cls->getattr(internStringMortal("__eq__")));
    set_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)setNe, UNKNOWN, 2)));
    frozenset_cls->giveAttr("__ne__", set_cls->getattr(internStringMortal("__ne__")));

    set_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)setNonzero, BOXED_BOOL, 1)));
    frozenset_cls->giveAttr("__nonzero__", set_cls->getattr(internStringMortal("__nonzero__")));

    frozenset_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)setHash, BOXED_INT, 1)));
    set_cls->giveAttr("__hash__", None);

    set_cls->giveAttr("add", new BoxedFunction(boxRTFunction((void*)setAdd, NONE, 2)));
    set_cls->giveAttr("remove", new BoxedFunction(boxRTFunction((void*)setRemove, NONE, 2)));
    set_cls->giveAttr("discard", new BoxedFunction(boxRTFunction((void*)setDiscard, NONE, 2)));

    set_cls->giveAttr("clear", new BoxedFunction(boxRTFunction((void*)setClear, NONE, 1)));
    set_cls->giveAttr("update", new BoxedFunction(boxRTFunction((void*)setUpdate, NONE, 1, 0, true, false)));
    set_cls->giveAttr("union", new BoxedFunction(boxRTFunction((void*)setUnion, UNKNOWN, 1, 0, true, false)));
    frozenset_cls->giveAttr("union", set_cls->getattr(internStringMortal("union")));
    set_cls->giveAttr("intersection",
                      new BoxedFunction(boxRTFunction((void*)setIntersection, UNKNOWN, 1, 0, true, false)));
    frozenset_cls->giveAttr("intersection", set_cls->getattr(internStringMortal("intersection")));
    set_cls->giveAttr("difference", new BoxedFunction(boxRTFunction((void*)setDifference, UNKNOWN, 1, 0, true, false)));
    frozenset_cls->giveAttr("difference", set_cls->getattr(internStringMortal("difference")));
    set_cls->giveAttr("difference_update",
                      new BoxedFunction(boxRTFunction((void*)setDifferenceUpdate, UNKNOWN, 1, 0, true, false)));
    set_cls->giveAttr("issubset", new BoxedFunction(boxRTFunction((void*)setIssubset, UNKNOWN, 2)));
    set_cls->giveAttr("issuperset", new BoxedFunction(boxRTFunction((void*)setIssuperset, UNKNOWN, 2)));
    set_cls->giveAttr("isdisjoint", new BoxedFunction(boxRTFunction((void*)setIsdisjoint, UNKNOWN, 2)));

    set_cls->giveAttr("copy", new BoxedFunction(boxRTFunction((void*)setCopy, UNKNOWN, 1)));
    set_cls->giveAttr("pop", new BoxedFunction(boxRTFunction((void*)setPop, UNKNOWN, 1)));

    set_cls->freeze();
    frozenset_cls->freeze();
}

void teardownSet() {
}
}
