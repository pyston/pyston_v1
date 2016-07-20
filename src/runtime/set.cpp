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

#include "runtime/set.h"

#include "capi/typeobject.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedClass* set_iterator_cls;

extern "C" Box* createSet() {
    return new BoxedSet();
}

static void _setAddStolen(BoxedSet* self, STOLEN(BoxAndHash) val) {
    try {
        auto&& p = self->s.insert(val);

        // Is there a nicer way to represent try-else?
        try {
            if (!p.second /* already exists */) {
                // keep the original key
                Py_DECREF(val.value);
            }
        } catch (ExcInfo e) {
            abort();
        }
    } catch (ExcInfo e) {
        Py_DECREF(val.value);
        throw e;
    }
}

void _setAddStolen(BoxedSet* self, STOLEN(Box*) val) {
    AUTO_DECREF(val);
    BoxAndHash val_hashed(val); // this can throw!
    incref(val_hashed);
    _setAddStolen(self, val_hashed);
}

namespace set {

class BoxedSetIterator : public Box {
public:
    BoxedSet* s;
    decltype(BoxedSet::s)::iterator it;
    long size;

    BoxedSetIterator(BoxedSet* s) : s(s), it(s->s.begin()), size(s->s.size()) { Py_INCREF(s); }

    DEFAULT_CLASS(set_iterator_cls);

    bool hasNext() { return it != s->s.end(); }

    Box* next() {
        Box* rtn = it->value;
        ++it;
        return incref(rtn);
    }

    static void dealloc(BoxedSetIterator* o) noexcept {
        PyObject_GC_UnTrack(o);

        Py_DECREF(o->s);

        o->cls->tp_free(o);
    }

    static int traverse(BoxedSetIterator* self, visitproc visit, void* arg) noexcept {
        Py_VISIT(self->s);
        return 0;
    }
};

Box* setiteratorHasnext(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    return boxBool(self->hasNext());
}

Box* setiteratorNext(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    if (self->s->s.size() != self->size) {
        raiseExcHelper(RuntimeError, "Set changed size during iteration");
    }
    return self->next();
}

Box* setiteratorLength(BoxedSetIterator* self) {
    RELEASE_ASSERT(self->cls == set_iterator_cls, "");
    return boxInt(self->s->s.size());
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
    return incref(self);
}

static void _setAdd(BoxedSet* self, BoxAndHash val) {
    Py_INCREF(val.value);
    _setAddStolen(self, val);
}

static bool _setRemove(BoxedSet* self, BoxAndHash val) {
    auto it = self->s.find(val);
    if (it == self->s.end())
        return false;
    Box* to_decref = it->value;
    self->s.erase(it);
    Py_DECREF(to_decref);
    return true;
}

// Creates a set of type 'cls' from 'container' (NULL to get an empty set).
// Works for frozenset and normal set types.
BoxedSet* makeNewSet(BoxedClass* cls, Box* container) {
    assert(isSubclass(cls, frozenset_cls) || isSubclass(cls, set_cls));

    BoxedSet* rtn = new (cls) BoxedSet();
    if (container) {
        AUTO_DECREF(rtn);
        if (PyAnySet_Check(container)) {
            for (auto&& elt : ((BoxedSet*)container)->s) {
                rtn->s.insert(incref(elt));
            }
        } else if (PyDict_CheckExact(container)) {
            for (auto&& elt : ((BoxedDict*)container)->d) {
                rtn->s.insert(incref(elt.first));
            }
        } else {
            for (auto elt : container->pyElements()) {
                _setAddStolen(rtn, elt);
            }
        }
        return incref(rtn);
    }
    return rtn;
}

Box* frozensetNew(Box* _cls, Box* container, BoxedDict* kwargs) {
    RELEASE_ASSERT(_cls->cls == type_cls, "");
    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    RELEASE_ASSERT(isSubclass(cls, frozenset_cls), "");
    if (_cls == frozenset_cls && !_PyArg_NoKeywords("frozenset()", kwargs)) {
        throwCAPIException();
    }

    if (_cls != frozenset_cls) {
        return makeNewSet(cls, container);
    }

    if (container != NULL) {
        if (container->cls == frozenset_cls)
            return incref(container);

        BoxedSet* result = makeNewSet(cls, container);
        if (result->s.size() != 0) {
            return result;
        }
        Py_DECREF(result);
    }

    static Box* emptyfrozenset = NULL;
    if (!emptyfrozenset) {
        emptyfrozenset = new (frozenset_cls) BoxedSet();
        PyGC_RegisterStaticConstant(emptyfrozenset);
    }

    Py_INCREF(emptyfrozenset);
    return emptyfrozenset;
}

Box* setNew(Box* _cls, Box* container, BoxedDict* kwargs) {
    RELEASE_ASSERT(_cls->cls == type_cls, "");
    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    RELEASE_ASSERT(isSubclass(cls, set_cls), "");

    if (_cls == set_cls && !_PyArg_NoKeywords("set()", kwargs)) {
        throwCAPIException();
    }

    // Note: set.__new__ explicitly ignores the container argument.
    return makeNewSet(cls, NULL);
}

static void setClearInternal(BoxedSet* self) {
    ASSERT(PyAnySet_Check(self), "");

    if (self->s.size()) {
        BoxedSet::Set tmp;
        std::swap(tmp, self->s);
        for (auto p : tmp) {
            Py_DECREF(p.value);
        }
        self->s.clear();
    }
}

Box* setInit(Box* _self, Box* container, BoxedDict* kwargs) {
    RELEASE_ASSERT(PySet_Check(_self), "");

    if (PySet_Check(_self) && !_PyArg_NoKeywords("set()", kwargs)) {
        throwCAPIException();
    }

    if (!container)
        return incref(Py_None);

    BoxedSet* self = static_cast<BoxedSet*>(_self);

    setClearInternal(self);

    if (PyAnySet_Check(container)) {
        for (auto&& elt : ((BoxedSet*)container)->s) {
            self->s.insert(incref(elt));
        }
    } else if (PyDict_CheckExact(container)) {
        for (auto&& elt : ((BoxedDict*)container)->d) {
            self->s.insert(incref(elt.first));
        }

    } else {
        for (auto elt : container->pyElements()) {
            _setAddStolen(self, elt);
        }
    }

    return incref(Py_None);
}

static Box* setRepr(Box* _self) {
    if (!PyAnySet_Check(_self))
        return setDescrTypeError<CXX>(_self, "set", "__repr__");

    BoxedSet* self = (BoxedSet*)_self;
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
            AUTO_DECREF(str);
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
static Box* set_repr(Box* self) noexcept {
    return callCXXFromStyle<CAPI>(setRepr, self);
}

static void _setSymmetricDifferenceUpdate(BoxedSet* self, Box* other) {
    if (!PyAnySet_Check(other)) {
        other = makeNewSet(self->cls, other);
    } else {
        Py_INCREF(other);
    }
    AUTO_DECREF(other);

    BoxedSet* other_set = static_cast<BoxedSet*>(other);

    for (auto elt : other_set->s) {
        auto&& p = self->s.insert(elt);
        if (!p.second /* already exists */) {
            _setRemove(self, elt);
        } else {
            Py_INCREF(elt.value);
        }
    }
}

static BoxedSet* setIntersection2(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    BoxedSet* rtn = makeNewSet(self->cls, NULL);
    AUTO_DECREF(rtn);
    for (auto elt : container->pyElements()) {
        AUTO_DECREF(elt);
        BoxAndHash elt_hashed(elt); // this can throw!
        if (self->s.count(elt_hashed))
            _setAdd(rtn, elt_hashed);
    }
    return incref(rtn);
}

static Box* setIntersectionUpdate2(BoxedSet* self, Box* other) {
    Box* tmp = setIntersection2(self, other);
    std::swap(self->s, ((BoxedSet*)tmp)->s);
    Py_DECREF(tmp);
    return incref(Py_None);
}

Box* setIOr(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    // TODO just [write and] call setUnionUpdate2
    for (auto&& elt : rhs->s) {
        _setAdd(lhs, elt);
    }
    return incref(lhs);
}

Box* setOr(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    BoxedSet* rtn = makeNewSet(lhs->cls, lhs);
    AUTO_DECREF(rtn);
    return setIOr(rtn, rhs);
}

Box* setIAnd(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    auto r = setIntersectionUpdate2(lhs, rhs);
    Py_DECREF(r);
    return incref(lhs);
}

Box* setAnd(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    return setIntersection2(lhs, rhs);
}

Box* setISub(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    // TODO: write and call setDifferenceUpdate2
    for (auto&& elt : rhs->s) {
        _setRemove(lhs, elt);
    }
    return incref(lhs);
}

Box* setSub(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    BoxedSet* rtn = makeNewSet(lhs->cls, lhs);
    AUTO_DECREF(rtn);
    return setISub(rtn, rhs);
}

Box* setIXor(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    _setSymmetricDifferenceUpdate(lhs, rhs);

    return incref(lhs);
}

Box* setXor(BoxedSet* lhs, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(lhs), "");
    if (!PyAnySet_Check(rhs))
        return incref(NotImplemented);

    BoxedSet* rtn = makeNewSet(lhs->cls, lhs);
    AUTO_DECREF(rtn);
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

    _setAdd(self, v);
    return incref(Py_None);
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
        _setAdd(static_cast<BoxedSet*>(set), key);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" Py_ssize_t PySet_Size(PyObject* anyset) noexcept {
    if (!PyAnySet_Check(anyset)) {
        PyErr_BadInternalCall();
        return -1;
    }
    BoxedSet* self = (BoxedSet*)anyset;
    return self->s.size();
}

Box* setClear(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    setClearInternal(self);
    Py_RETURN_NONE;
}

extern "C" int PySet_Clear(PyObject* set) noexcept {
    if (!PySet_Check(set)) {
        PyErr_BadInternalCall();
        return -1;
    }
    setClearInternal(static_cast<BoxedSet*>(set));
    return 0;
}

Box* setUpdate(BoxedSet* self, BoxedTuple* args) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    assert(args->cls == tuple_cls);

    for (auto l : *args) {
        if (l->cls == set_cls) {
            BoxedSet* s2 = static_cast<BoxedSet*>(l);
            for (auto&& p : s2->s) {
                _setAdd(self, p);
            }
        } else {
            for (auto e : l->pyElements()) {
                _setAddStolen(self, e);
            }
        }
    }

    return incref(Py_None);
}

Box* setUnion(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'union' requires a 'set' object but received a '%s'", getTypeName(self));

    BoxedSet* rtn = makeNewSet(self->cls, self);
    AUTO_DECREF(rtn);

    for (auto&& p : self->s)
        _setAdd(rtn, p);

    for (auto container : args->pyElements()) {
        AUTO_DECREF(container);
        for (auto elt : container->pyElements()) {
            _setAddStolen(rtn, elt);
        }
    }
    return incref(rtn);
}

static void _setDifferenceUpdate(BoxedSet* self, BoxedTuple* args) {
    for (auto container : args->pyElements()) {
        AUTO_DECREF(container);
        if (PyAnySet_Check(container)) {
            for (auto&& elt : ((BoxedSet*)container)->s) {
                _setRemove(self, elt);
            }
        } else if (PyDict_CheckExact(container)) {
            for (auto&& elt : ((BoxedDict*)container)->d) {
                _setRemove(self, elt.first);
            }
        } else {
            for (auto elt : container->pyElements()) {
                AUTO_DECREF(elt);
                _setRemove(self, elt);
            }
        }
    }
}

Box* setDifferenceUpdate(BoxedSet* self, BoxedTuple* args) {
    if (!PySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'difference_update' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    _setDifferenceUpdate(self, args);
    return incref(Py_None);
}

Box* setDifference(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'difference' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    BoxedSet* rtn = makeNewSet(self->cls, self);
    AUTO_DECREF(rtn);
    _setDifferenceUpdate(rtn, args);
    return incref(rtn);
}

Box* setSymmetricDifferenceUpdate(BoxedSet* self, Box* other) {
    if (!PySet_Check(self))
        raiseExcHelper(TypeError,
                       "descriptor 'symmetric_difference_update' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    _setSymmetricDifferenceUpdate(self, other);
    return incref(Py_None);
}

Box* setSymmetricDifference(BoxedSet* self, Box* other) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'symmetric_difference' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    BoxedSet* rtn = makeNewSet(self->cls, self);
    AUTO_DECREF(rtn);
    _setSymmetricDifferenceUpdate(rtn, other);
    return incref(rtn);
}

static Box* setIssubset(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (!PyAnySet_Check(container)) {
        container = makeNewSet(set_cls, container);
    } else {
        Py_INCREF(container);
    }
    AUTO_DECREF(container);
    assert(PyAnySet_Check(container));

    BoxedSet* rhs = static_cast<BoxedSet*>(container);
    if (self->s.size() > rhs->s.size())
        Py_RETURN_FALSE;

    for (auto e : self->s) {
        if (rhs->s.find(e) == rhs->s.end())
            Py_RETURN_FALSE;
    }
    Py_RETURN_TRUE;
}

static Box* setIssuperset(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (!PyAnySet_Check(container)) {
        container = makeNewSet(set_cls, container);
    } else {
        Py_INCREF(container);
    }
    AUTO_DECREF(container);
    assert(PyAnySet_Check(container));
    return setIssubset((BoxedSet*)container, self);
}

static Box* setIsdisjoint(BoxedSet* self, Box* container) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    for (auto e : container->pyElements()) {
        AUTO_DECREF(e);
        if (self->s.find(e) != self->s.end())
            Py_RETURN_FALSE;
    }
    Py_RETURN_TRUE;
}

static Box* setIntersection(BoxedSet* self, BoxedTuple* args) {
    if (!PyAnySet_Check(self))
        raiseExcHelper(TypeError, "descriptor 'intersection' requires a 'set' object but received a '%s'",
                       getTypeName(self));

    if (args->size() == 0)
        return makeNewSet(self->cls, self);

    BoxedSet* rtn = incref(self);
    for (auto container : *args) {
        AUTO_DECREF(rtn);
        rtn = setIntersection2(rtn, container);
    }
    return rtn;
}

static Box* setIntersectionUpdate(BoxedSet* self, BoxedTuple* args) {
    Box* tmp = setIntersection(self, args);
    AUTO_DECREF(tmp);
    std::swap(self->s, ((BoxedSet*)tmp)->s);
    return incref(Py_None);
}

Box* setCopy(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    BoxedSet* rtn = new BoxedSet();
    for (auto&& p : self->s)
        Py_INCREF(p.value);
    rtn->s = self->s;
    return rtn;
}

Box* frozensetCopy(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (self->cls == frozenset_cls) {
        return incref(self);
    }
    return setCopy(self);
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

Box* setEq(BoxedSet* self, BoxedSet* rhs) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    if (!PyAnySet_Check(rhs))
        Py_RETURN_FALSE;

    if (self->s.size() != rhs->s.size())
        Py_RETURN_FALSE;

    return setIssubset(self, rhs);
}

Box* setNe(BoxedSet* self, BoxedSet* rhs) {
    Box* r = setEq(self, rhs);
    AUTO_DECREF(r);
    assert(r->cls == bool_cls);
    return boxBool(r == Py_False);
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
        Py_RETURN_FALSE;

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
        Py_RETURN_FALSE;

    return setIssuperset(self, rhs);
}

Box* setContains(BoxedSet* self, Box* key) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");

    if (PySet_Check(key)) {
        try {
            BoxAndHash k_hash(key);
            return boxBool(self->s.find(k_hash) != self->s.end());
        } catch (ExcInfo e) {
            if (!e.matches(TypeError))
                throw e;

            e.clear();

            BoxedSet* tmpKey = makeNewSet(frozenset_cls, key);
            AUTO_DECREF(tmpKey);
            return boxBool(self->s.find(tmpKey) != self->s.end());
        }
    }

    return boxBool(self->s.find(key) != self->s.end());
}

Box* setRemove(BoxedSet* self, Box* key) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    if (PySet_Check(key)) {
        try {
            bool existed = _setRemove(self, key);
            if (existed)
                return incref(Py_None);
        } catch (ExcInfo e) {
            if (!e.matches(TypeError))
                throw e;

            e.clear();

            BoxedSet* tmpKey = makeNewSet(frozenset_cls, key);
            AUTO_DECREF(tmpKey);
            bool existed = _setRemove(self, tmpKey);
            if (existed)
                return incref(Py_None);
        }
        raiseExcHelper(KeyError, key);
    }

    bool existed = _setRemove(self, key);
    if (existed)
        return incref(Py_None);
    raiseExcHelper(KeyError, key);
}

Box* setDiscard(BoxedSet* self, Box* key) {
    RELEASE_ASSERT(isSubclass(self->cls, set_cls), "");

    if (PySet_Check(key)) {
        try {
            _setRemove(self, key);
        } catch (ExcInfo e) {
            if (!e.matches(TypeError))
                throw e;

            e.clear();

            BoxedSet* tmpKey = makeNewSet(frozenset_cls, key);
            AUTO_DECREF(tmpKey);
            _setRemove(self, tmpKey);
        }
        return incref(Py_None);
    }

    _setRemove(self, key);

    return incref(Py_None);
}

Box* setNocmp(BoxedSet* self, BoxedSet* rhs) {
    raiseExcHelper(TypeError, "cannot compare sets using cmp()");
}

Box* setNonzero(BoxedSet* self) {
    RELEASE_ASSERT(PyAnySet_Check(self), "");
    return boxBool(self->s.size());
}

Box* setNotImplemented(BoxedSet* self) {
    raiseExcHelper(TypeError, "unhashable type: 'set'");
}

Box* setHash(BoxedSet* self) {
    RELEASE_ASSERT(isSubclass(self->cls, frozenset_cls), "");

    int64_t h, hash = 1927868237L;

    hash *= self->s.size() + 1;
    for (auto&& e : self->s) {
        h = e.hash;
        hash ^= (h ^ (h << 16) ^ 89869747L) * 3644798167u;
    }

    hash = hash * 69069L + 907133923L;
    if (hash == -1)
        hash = 590923713L;

    return boxInt(hash);
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

static PyObject* set_reduce(BoxedSet* so) noexcept {
    PyObject* keys = NULL, * args = NULL, * result = NULL, * dict = NULL;

    keys = PySequence_List((PyObject*)so);
    if (keys == NULL)
        goto done;
    args = PyTuple_Pack(1, keys);
    if (args == NULL)
        goto done;
    dict = PyObject_GetAttrString((PyObject*)so, "__dict__");
    if (dict == NULL) {
        PyErr_Clear();
        dict = Py_None;
        Py_INCREF(dict);
    }
    result = PyTuple_Pack(3, Py_TYPE(so), args, dict);
done:
    Py_XDECREF(args);
    Py_XDECREF(keys);
    Py_XDECREF(dict);
    return result;
}


} // namespace set

using namespace pyston::set;

void BoxedSet::dealloc(Box* _o) noexcept {
    BoxedSet* o = (BoxedSet*)_o;

    PyObject_ClearWeakRefs(o);

    PyObject_GC_UnTrack(o);
    for (auto p : o->s) {
        Py_DECREF(p.value);
    }

    // Unfortunately, this assert requires accessing the type object, which might have been freed already:
    // assert(PyAnySet_Check(b));
    o->s.freeAllMemory();

    o->cls->tp_free(o);
}

int BoxedSet::traverse(Box* _o, visitproc visit, void* arg) noexcept {
    BoxedSet* o = (BoxedSet*)_o;

    for (auto p : o->s) {
        Py_VISIT(p.value);
    }
    return 0;
}

int BoxedSet::clear(Box* _o) noexcept {
    BoxedSet* o = (BoxedSet*)_o;

    setClearInternal(o);

    return 0;
}

static PyMethodDef set_methods[]
    = { { "__reduce__", (PyCFunction)set_reduce, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };
static PyMethodDef frozenset_methods[]
    = { { "__reduce__", (PyCFunction)set_reduce, METH_NOARGS, NULL }, { NULL, NULL, 0, NULL } };

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

    set_iterator_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedSetIterator), false, "setiterator",
                                          false, (destructor)BoxedSetIterator::dealloc, NULL, true,
                                          (traverseproc)BoxedSetIterator::traverse, NOCLEAR);
    set_iterator_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                               (void*)setiteratorIter, typeFromClass(set_iterator_cls), 1)));
    set_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(FunctionMetadata::create((void*)setiteratorHasnext, BOXED_BOOL, 1)));
    set_iterator_cls->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)setiteratorNext, UNKNOWN, 1)));
    set_iterator_cls->giveAttr("__length_hint__",
                               new BoxedFunction(FunctionMetadata::create((void*)setiteratorLength, BOXED_INT, 1)));
    set_iterator_cls->freeze();
    set_iterator_cls->tp_iternext = setiter_next;
    set_iterator_cls->tp_iter = PyObject_SelfIter;

    set_cls->giveAttr("__new__",
                      new BoxedFunction(FunctionMetadata::create((void*)setNew, UNKNOWN, 2, false, true), { NULL }));
    set_cls->giveAttr("__init__",
                      new BoxedFunction(FunctionMetadata::create((void*)setInit, UNKNOWN, 2, false, true), { NULL }));
    frozenset_cls->giveAttr(
        "__new__", new BoxedFunction(FunctionMetadata::create((void*)frozensetNew, UNKNOWN, 2, false, true), { NULL }));

    Box* set_repr_func = new BoxedFunction(FunctionMetadata::create((void*)setRepr, STR, 1));
    set_cls->giveAttrBorrowed("__repr__", set_repr_func);
    frozenset_cls->giveAttr("__repr__", set_repr_func);

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
        auto func_obj = new BoxedFunction(FunctionMetadata::create((void*)func, UNKNOWN, 2, false, false));
        set_cls->giveAttrBorrowed(name, func_obj);
        frozenset_cls->giveAttr(name, func_obj);
        /*
        FunctionMetadata* func_obj = FunctionMetadata::create(2, false, false);
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

    set_cls->giveAttr("__iter__",
                      new BoxedFunction(FunctionMetadata::create((void*)setIter, typeFromClass(set_iterator_cls), 1)));
    frozenset_cls->giveAttrBorrowed("__iter__", set_cls->getattr(getStaticString("__iter__")));

    set_cls->giveAttr("__len__", new BoxedFunction(FunctionMetadata::create((void*)setLen, BOXED_INT, 1)));
    frozenset_cls->giveAttrBorrowed("__len__", set_cls->getattr(getStaticString("__len__")));

    set_cls->giveAttr("__contains__", new BoxedFunction(FunctionMetadata::create((void*)setContains, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__contains__", set_cls->getattr(getStaticString("__contains__")));

    set_cls->giveAttr("__cmp__", new BoxedFunction(FunctionMetadata::create((void*)setNocmp, NONE, 2)));
    frozenset_cls->giveAttr("__cmp__", new BoxedFunction(FunctionMetadata::create((void*)setNocmp, NONE, 2)));
    set_cls->giveAttr("__eq__", new BoxedFunction(FunctionMetadata::create((void*)setEq, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__eq__", set_cls->getattr(getStaticString("__eq__")));
    set_cls->giveAttr("__ne__", new BoxedFunction(FunctionMetadata::create((void*)setNe, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__ne__", set_cls->getattr(getStaticString("__ne__")));
    set_cls->giveAttr("__le__", new BoxedFunction(FunctionMetadata::create((void*)setLe, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__le__", set_cls->getattr(getStaticString("__le__")));
    set_cls->giveAttr("__lt__", new BoxedFunction(FunctionMetadata::create((void*)setLt, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__lt__", set_cls->getattr(getStaticString("__lt__")));
    set_cls->giveAttr("__ge__", new BoxedFunction(FunctionMetadata::create((void*)setGe, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__ge__", set_cls->getattr(getStaticString("__ge__")));
    set_cls->giveAttr("__gt__", new BoxedFunction(FunctionMetadata::create((void*)setGt, BOXED_BOOL, 2)));
    frozenset_cls->giveAttrBorrowed("__gt__", set_cls->getattr(getStaticString("__gt__")));

    set_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)setNonzero, BOXED_BOOL, 1)));
    frozenset_cls->giveAttrBorrowed("__nonzero__", set_cls->getattr(getStaticString("__nonzero__")));

    frozenset_cls->giveAttr("__hash__", new BoxedFunction(FunctionMetadata::create((void*)setHash, BOXED_INT, 1)));
    set_cls->giveAttrBorrowed("__hash__", Py_None);

    set_cls->giveAttr("add", new BoxedFunction(FunctionMetadata::create((void*)setAdd, NONE, 2)));
    set_cls->giveAttr("remove", new BoxedFunction(FunctionMetadata::create((void*)setRemove, NONE, 2)));
    set_cls->giveAttr("discard", new BoxedFunction(FunctionMetadata::create((void*)setDiscard, NONE, 2)));

    set_cls->giveAttr("clear", new BoxedFunction(FunctionMetadata::create((void*)setClear, NONE, 1)));
    set_cls->giveAttr("update", new BoxedFunction(FunctionMetadata::create((void*)setUpdate, NONE, 1, true, false)));
    set_cls->giveAttr("union", new BoxedFunction(FunctionMetadata::create((void*)setUnion, UNKNOWN, 1, true, false)));
    frozenset_cls->giveAttrBorrowed("union", set_cls->getattr(getStaticString("union")));
    set_cls->giveAttr("intersection",
                      new BoxedFunction(FunctionMetadata::create((void*)setIntersection, UNKNOWN, 1, true, false)));
    frozenset_cls->giveAttrBorrowed("intersection", set_cls->getattr(getStaticString("intersection")));
    set_cls->giveAttr("intersection_update", new BoxedFunction(FunctionMetadata::create((void*)setIntersectionUpdate,
                                                                                        UNKNOWN, 1, true, false)));
    set_cls->giveAttr("difference",
                      new BoxedFunction(FunctionMetadata::create((void*)setDifference, UNKNOWN, 1, true, false)));
    frozenset_cls->giveAttrBorrowed("difference", set_cls->getattr(getStaticString("difference")));
    set_cls->giveAttr("difference_update",
                      new BoxedFunction(FunctionMetadata::create((void*)setDifferenceUpdate, UNKNOWN, 1, true, false)));
    set_cls->giveAttr("symmetric_difference", new BoxedFunction(FunctionMetadata::create((void*)setSymmetricDifference,
                                                                                         UNKNOWN, 2, false, false)));
    frozenset_cls->giveAttrBorrowed("symmetric_difference", set_cls->getattr(getStaticString("symmetric_difference")));
    set_cls->giveAttr(
        "symmetric_difference_update",
        new BoxedFunction(FunctionMetadata::create((void*)setSymmetricDifferenceUpdate, UNKNOWN, 2, false, false)));
    set_cls->giveAttr("issubset", new BoxedFunction(FunctionMetadata::create((void*)setIssubset, UNKNOWN, 2)));
    frozenset_cls->giveAttrBorrowed("issubset", set_cls->getattr(getStaticString("issubset")));
    set_cls->giveAttr("issuperset", new BoxedFunction(FunctionMetadata::create((void*)setIssuperset, UNKNOWN, 2)));
    frozenset_cls->giveAttrBorrowed("issuperset", set_cls->getattr(getStaticString("issuperset")));
    set_cls->giveAttr("isdisjoint", new BoxedFunction(FunctionMetadata::create((void*)setIsdisjoint, UNKNOWN, 2)));
    frozenset_cls->giveAttrBorrowed("isdisjoint", set_cls->getattr(getStaticString("isdisjoint")));

    set_cls->giveAttr("copy", new BoxedFunction(FunctionMetadata::create((void*)setCopy, UNKNOWN, 1)));
    frozenset_cls->giveAttr("copy", new BoxedFunction(FunctionMetadata::create((void*)frozensetCopy, UNKNOWN, 1)));
    set_cls->giveAttr("pop", new BoxedFunction(FunctionMetadata::create((void*)setPop, UNKNOWN, 1)));

    add_methods(set_cls, set_methods);
    add_methods(frozenset_cls, frozenset_methods);

    set_cls->freeze();
    frozenset_cls->freeze();

    frozenset_cls->tp_repr = set_cls->tp_repr = set_repr;
    frozenset_cls->tp_iter = set_cls->tp_iter = (decltype(set_cls->tp_iter))setIter;
}
}
