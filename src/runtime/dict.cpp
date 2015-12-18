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

#include "runtime/dict.h"

#include "capi/types.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/ics.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedClass* dict_iterator_cls = NULL;
BoxedClass* dict_keys_cls = NULL;
BoxedClass* dict_values_cls = NULL;
BoxedClass* dict_items_cls = NULL;

Box* dictRepr(BoxedDict* self) {
    std::vector<char> chars;
    int status = Py_ReprEnter((PyObject*)self);
    if (status != 0) {
        if (status < 0)
            throwCAPIException();

        chars.push_back('{');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('}');
        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }

    try {
        chars.push_back('{');
        bool first = true;
        for (const auto& p : *self) {
            if (!first) {
                chars.push_back(',');
                chars.push_back(' ');
            }
            first = false;
            BoxedString* k = static_cast<BoxedString*>(repr(p.first));
            BoxedString* v = static_cast<BoxedString*>(repr(p.second));
            chars.insert(chars.end(), k->s().begin(), k->s().end());
            chars.push_back(':');
            chars.push_back(' ');
            chars.insert(chars.end(), v->s().begin(), v->s().end());
        }
        chars.push_back('}');
    } catch (ExcInfo e) {
        Py_ReprLeave((PyObject*)self);
        throw e;
    }
    Py_ReprLeave((PyObject*)self);
    return boxString(llvm::StringRef(&chars[0], chars.size()));
}

Box* dictClear(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'clear' requires a 'dict' object but received a '%s'", getTypeName(self));

    self->d.clear();
    return None;
}

Box* dictCopy(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'copy' requires a 'dict' object but received a '%s'", getTypeName(self));

    BoxedDict* r = new BoxedDict();
    r->d = self->d;
    return r;
}

Box* dictItems(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();

    rtn->ensure(self->d.size());
    for (const auto& p : *self) {
        BoxedTuple* t = BoxedTuple::create({ p.first, p.second });
        listAppendInternal(rtn, t);
    }

    return rtn;
}

Box* dictValues(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    rtn->ensure(self->d.size());
    for (const auto& p : *self) {
        listAppendInternal(rtn, p.second);
    }
    return rtn;
}

Box* dictKeys(BoxedDict* self) {
    RELEASE_ASSERT(PyDict_Check(self), "");

    BoxedList* rtn = new BoxedList();
    rtn->ensure(self->d.size());
    for (const auto& p : *self) {
        listAppendInternal(rtn, p.first);
    }
    return rtn;
}

static PyObject* dict_helper(PyObject* mp, std::function<Box*(BoxedDict*)> f) noexcept {
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    try {
        return f(static_cast<BoxedDict*>(mp));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyDict_Keys(PyObject* mp) noexcept {
    return dict_helper(mp, dictKeys);
}

extern "C" PyObject* PyDict_Values(PyObject* mp) noexcept {
    return dict_helper(mp, dictValues);
}

extern "C" PyObject* PyDict_Items(PyObject* mp) noexcept {
    return dict_helper(mp, dictItems);
}

Box* dictViewKeys(BoxedDict* self) {
    if (!PyDict_Check(self)) {
        raiseExcHelper(TypeError, "descriptor 'viewkeys' requires a 'dict' object but received a '%s'",
                       getTypeName(self));
    }
    BoxedDictView* rtn = new (dict_keys_cls) BoxedDictView(self);
    return rtn;
}

Box* dictViewValues(BoxedDict* self) {
    if (!PyDict_Check(self)) {
        raiseExcHelper(TypeError, "descriptor 'viewvalues' requires a 'dict' object but received a '%s'",
                       getTypeName(self));
    }
    BoxedDictView* rtn = new (dict_values_cls) BoxedDictView(self);
    return rtn;
}

Box* dictViewItems(BoxedDict* self) {
    if (!PyDict_Check(self)) {
        raiseExcHelper(TypeError, "descriptor 'viewitems' requires a 'dict' object but received a '%s'",
                       getTypeName(self));
    }
    BoxedDictView* rtn = new (dict_items_cls) BoxedDictView(self);
    return rtn;
}

// Analoguous to CPython's, used for sq_ slots.
static Py_ssize_t dict_length(PyDictObject* mp) {
    return ((BoxedDict*)mp)->d.size();
}

Box* dictLen(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__len__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return boxInt(self->d.size());
}

extern "C" Py_ssize_t PyDict_Size(PyObject* op) noexcept {
    RELEASE_ASSERT(PyDict_Check(op), "");
    return static_cast<BoxedDict*>(op)->d.size();
}

extern "C" void PyDict_Clear(PyObject* op) noexcept {
    RELEASE_ASSERT(PyDict_Check(op), "");
    static_cast<BoxedDict*>(op)->d.clear();
}

extern "C" PyObject* PyDict_Copy(PyObject* o) noexcept {
    RELEASE_ASSERT(PyDict_Check(o), "");
    try {
        return dictCopy(static_cast<BoxedDict*>(o));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyDict_Update(PyObject* a, PyObject* b) noexcept {
    return PyDict_Merge(a, b, 1);
}

template <enum ExceptionStyle S> Box* dictGetitem(BoxedDict* self, Box* k) noexcept(S == CAPI) {
    if (!PyDict_Check(self)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                         getTypeName(self));
            return NULL;
        } else {
            raiseExcHelper(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                           getTypeName(self));
        }
    }

    BoxedDict::DictMap::iterator it;
    try {
        it = self->d.find(k);
    } catch (ExcInfo e) {
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        } else {
            throw e;
        }
    }

    if (it == self->d.end()) {
        // Try calling __missing__ if this is a subclass
        if (self->cls != dict_cls) {
            // Special-case defaultdict, assuming that it's the main time we will actually hit this.
            // We could just use a single runtime IC here, or have a small cache that maps type->runtimeic.
            // Or use a polymorphic runtime ic.
            static BoxedClass* defaultdict_cls = NULL;
            static CallattrIC defaultdict_ic;
            if (defaultdict_cls == NULL && strcmp(self->cls->tp_name, "collections.defaultdict") == 0) {
                defaultdict_cls = self->cls;
            }

            static BoxedString* missing_str = internStringImmortal("__missing__");
            CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(1) };
            Box* r;
            try {
                if (self->cls == defaultdict_cls) {
                    r = defaultdict_ic.call(self, missing_str, callattr_flags, k, NULL, NULL, NULL, NULL);
                } else {
                    r = callattr(self, missing_str, callattr_flags, k, NULL, NULL, NULL, NULL);
                }
            } catch (ExcInfo e) {
                if (S == CAPI) {
                    setCAPIException(e);
                    return NULL;
                } else
                    throw e;
            }
            if (r)
                return r;
        }

        if (S == CAPI) {
            PyErr_SetObject(KeyError, BoxedTuple::create1(k));
            return NULL;
        } else
            raiseExcHelper(KeyError, k);
    }
    return it->second;
}

extern "C" PyObject* PyDict_New() noexcept {
    return new BoxedDict();
}

// We don't assume that dicts passed to PyDict are necessarily dicts, since there are a couple places
// that we provide dict-like objects instead of proper dicts.
// The performance should hopefully be comparable to the CPython fast case, since we can use
// runtimeICs.
extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) noexcept {
    ASSERT(PyDict_Check(mp) || mp->cls == attrwrapper_cls, "%s", getTypeName(mp));

    assert(mp);
    Box* b = static_cast<Box*>(mp);
    Box* key = static_cast<Box*>(_key);
    Box* item = static_cast<Box*>(_item);

    assert(key);
    assert(item);

    try {
        setitem(b, key, item);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) noexcept {
    Box* key_s;
    try {
        key_s = boxString(key);
    } catch (ExcInfo e) {
        abort();
    }

    return PyDict_SetItem(mp, key_s, item);
}

extern "C" PyObject* PyDict_GetItem(PyObject* dict, PyObject* key) noexcept {
    ASSERT(PyDict_Check(dict) || dict->cls == attrwrapper_cls, "%s", getTypeName(dict));
    if (PyDict_Check(dict)) {
        BoxedDict* d = static_cast<BoxedDict*>(dict);
        return d->getOrNull(key);
    }

    auto&& tstate = _PyThreadState_Current;
    if (tstate != NULL && tstate->curexc_type != NULL) {
        /* preserve the existing exception */
        PyObject* err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        Box* b = getitemInternal<CAPI>(dict, key);
        /* ignore errors */
        PyErr_Restore(err_type, err_value, err_tb);
        return b;
    } else {
        Box* b = getitemInternal<CAPI>(dict, key);
        if (b == NULL)
            PyErr_Clear();
        return b;
    }
}

extern "C" int PyDict_Next(PyObject* op, Py_ssize_t* ppos, PyObject** pkey, PyObject** pvalue) noexcept {
    assert(PyDict_Check(op));
    BoxedDict* self = static_cast<BoxedDict*>(op);

    // Callers of PyDict_New() provide a pointer to some storage for this function to use, in
    // the form of a Py_ssize_t* -- ie they allocate a Py_ssize_t on their stack, and let us use
    // it.
    //
    // We want to store an unordered_map::iterator in that.  In my glibc it would fit, but to keep
    // things a little bit more portable, allocate separate storage for the iterator, and store the
    // pointer to this storage in the Py_ssize_t slot.
    //
    // Results in lots of indirection unfortunately.  If it becomes an issue we can try to switch
    // to storing the iterator directly in the stack slot.

    typedef BoxedDict::DictMap::iterator iterator;

    static_assert(sizeof(Py_ssize_t) == sizeof(iterator*), "");
    iterator** it_ptr = reinterpret_cast<iterator**>(ppos);

    // Clients are supposed to zero-initialize *ppos:
    if (*it_ptr == NULL) {
        *it_ptr = (iterator*)malloc(sizeof(iterator));
        **it_ptr = self->d.begin();
    }

    iterator* it = *it_ptr;

    if (*it == self->d.end()) {
        free(it);
        return 0;
    }

    *pkey = (*it)->first.value;
    *pvalue = (*it)->second;

    ++(*it);

    return 1;
}

extern "C" PyObject* PyDict_GetItemString(PyObject* dict, const char* key) noexcept {
    if (dict->cls == attrwrapper_cls)
        return unwrapAttrWrapper(dict)->getattr(internStringMortal(key));

    Box* key_s;
    try {
        key_s = boxString(key);
    } catch (ExcInfo e) {
        abort();
    }
    return PyDict_GetItem(dict, key_s);
}

Box* dictSetitem(BoxedDict* self, Box* k, Box* v) {
    Box*& pos = self->d[k];

    if (pos != NULL) {
        pos = v;
    } else {
        pos = v;
    }

    return None;
}

Box* dictDelitem(BoxedDict* self, Box* k) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__delitem__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        raiseExcHelper(KeyError, k);
    }

    self->d.erase(it);

    return None;
}

// Analoguous to CPython's, used for sq_ slots.
static int dict_ass_sub(PyDictObject* mp, PyObject* v, PyObject* w) noexcept {
    try {
        Box* res;
        if (w == NULL) {
            res = dictDelitem((BoxedDict*)mp, v);
        } else {
            res = dictSetitem((BoxedDict*)mp, v, w);
        }
        assert(res == None);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyDict_DelItem(PyObject* op, PyObject* key) noexcept {
    ASSERT(PyDict_Check(op) || op->cls == attrwrapper_cls, "%s", getTypeName(op));
    try {
        delitem(op, key);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" int PyDict_DelItemString(PyObject* v, const char* key) noexcept {
    PyObject* kv;
    int err;
    kv = PyString_FromString(key);
    if (kv == NULL)
        return -1;
    err = PyDict_DelItem(v, kv);
    Py_DECREF(kv);
    return err;
}

Box* dictPop(BoxedDict* self, Box* k, Box* d) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'pop' requires a 'dict' object but received a '%s'", getTypeName(self));

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        if (d)
            return d;

        raiseExcHelper(KeyError, k);
    }

    Box* rtn = it->second;
    self->d.erase(it);
    return rtn;
}

Box* dictPopitem(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'popitem' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    auto it = self->d.begin();
    if (it == self->d.end()) {
        raiseExcHelper(KeyError, "popitem(): dictionary is empty");
    }

    Box* key = it->first.value;
    Box* value = it->second;
    self->d.erase(it);

    auto rtn = BoxedTuple::create({ key, value });
    return rtn;
}

Box* dictGet(BoxedDict* self, Box* k, Box* d) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'get' requires a 'dict' object but received a '%s'", getTypeName(self));

    auto it = self->d.find(k);
    if (it == self->d.end())
        return d;

    return it->second;
}

Box* dictSetdefault(BoxedDict* self, Box* k, Box* v) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'setdefault' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    typedef BoxedDict::DictMap::iterator iterator;

    std::pair<iterator, bool> it_flag_pair = self->d.insert(std::make_pair(k, v));
    if (it_flag_pair.second == false)
        return it_flag_pair.first->second;
    return v;
}

Box* dictContains(BoxedDict* self, Box* k) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__contains__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return boxBool(self->d.count(k) != 0);
}

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
extern "C" int PyDict_Contains(PyObject* op, PyObject* key) noexcept {

    try {
        if (op->cls == attrwrapper_cls) {
            if (key->cls == str_cls) {
                BoxedString* key_str = (BoxedString*)key;
                internStringMortalInplace(key_str);
                return unwrapAttrWrapper(op)->hasattr(key_str);
            }

            Box* rtn = PyObject_CallMethod(op, "__contains__", "O", key);
            if (!rtn)
                return -1;
            return rtn == True;
        }

        BoxedDict* mp = (BoxedDict*)op;
        assert(PyDict_Check(mp));
        return mp->getOrNull(key) ? 1 : 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}


Box* dictNonzero(BoxedDict* self) {
    return boxBool(self->d.size());
}

Box* dictFromkeys(Box* _cls, Box* iterable, Box* default_value) {
    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    BoxedDict* rtn = (BoxedDict*)PyObject_CallObject(cls, NULL);
    checkAndThrowCAPIException();

    int status;
    if (PyAnySet_Check(iterable)) {
        for (auto&& elt : ((BoxedSet*)iterable)->s) {
            rtn->d.insert(std::make_pair(elt, default_value));
        }
    } else {
        if (PyDict_CheckExact(rtn)) {
            for (Box* e : iterable->pyElements()) {
                status = PyDict_SetItem(rtn, e, default_value);
                if (status < 0)
                    throwCAPIException();
            }
        } else {
            for (Box* e : iterable->pyElements()) {
                status = PyObject_SetItem(rtn, e, default_value);
                if (status < 0)
                    throwCAPIException();
            }
        }
    }

    return rtn;
}

Box* characterize(BoxedDict* lhs, BoxedDict* rhs, Box** pval) {
    Box* akey = None;
    Box* aval = None;
    int cmp;

    for (const auto& p : lhs->d) {
        Box* thiskey, *thislhsVal, *thisrhsVal;
        auto it = rhs->d.find(p.first);
        if (it->second == None)
            continue;

        thiskey = lhs->d.find(p.first)->first.value;
        if (akey != None) {
            cmp = PyObject_RichCompareBool(akey, thiskey, Py_LT);

            if (cmp < 0) {
                *pval = NULL;
                return NULL;
            }

            if (cmp > 0 || it == lhs->d.end() || it->second == None) {
                /* Not the *smallest* a key; or maybe it is
                 * but the compare shrunk the dict so we can't
                 * find its associated value anymore; or
                 * maybe it is but the compare deleted the
                 * a[thiskey] entry.
                 */
                continue;
            }
        }
        thislhsVal = p.second;
        thisrhsVal = PyDict_GetItem((PyObject*)rhs, thiskey);
        if (thisrhsVal == NULL)
            cmp = 0;
        else {
            // both dicts have thiskey, same values?
            cmp = PyObject_RichCompareBool(thislhsVal, thisrhsVal, Py_EQ);
            if (cmp < 0) {
                *pval = NULL;
                return NULL;
            }
        }
        if (cmp == 0) {
            // New winner.
            akey = thiskey;
            aval = thislhsVal;
        }
    }

    *pval = aval;
    return akey;
}

static int dict_compare(PyObject* _self, PyObject* _rhs) {
    if (_rhs->cls == attrwrapper_cls)
        _rhs = attrwrapperToDict(_rhs);

    BoxedDict* self = static_cast<BoxedDict*>(_self);
    BoxedDict* rhs = static_cast<BoxedDict*>(_rhs);

    if (self->d.size() < rhs->d.size())
        return -1;
    else if (self->d.size() > rhs->d.size())
        return 1;

    Box* adiff, *bdiff, *aval, *bval;
    int res = 0;
    adiff = characterize(self, rhs, &aval);
    if (adiff == NULL) {
        assert(!aval);
        /* Either an error, or a is a subset with the same length so
         * must be equal.
         */
        res = PyErr_Occurred() ? -1 : 0;
        goto Finished;
    }

    bdiff = characterize(rhs, self, &bval);
    if (bdiff == NULL && PyErr_Occurred()) {
        assert(!bval);
        res = -1;
        goto Finished;
    }
    if (bdiff) {
        /* bdiff == NULL "should be" impossible now, but perhaps
         * the last comparison done by the characterize() on a had
         * the side effect of making the dicts equal!
         */
        res = PyObject_Compare(adiff, bdiff);
    }
    if (res == 0 && bval != NULL)
        res = PyObject_Compare(aval, bval);

Finished:
    return res;
}

Box* dictCmp(BoxedDict* self, Box* rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__cmp__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    int res = dict_compare(self, rhs);
    if (res == -1 && PyErr_Occurred())
        throwCAPIException();

    return boxInt(res);
}

Box* dictLt(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__lt__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return NotImplemented;
}

Box* dictLe(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__le__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return NotImplemented;
}

Box* dictGt(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__gt__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return NotImplemented;
}

Box* dictGe(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__ge__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return NotImplemented;
}

Box* dictEq(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    if (_rhs->cls == attrwrapper_cls)
        _rhs = attrwrapperToDict(_rhs);

    if (!PyDict_Check(_rhs))
        return NotImplemented;

    BoxedDict* rhs = static_cast<BoxedDict*>(_rhs);

    if (self->d.size() != rhs->d.size())
        return False;

    for (const auto& p : self->d) {
        auto it = rhs->d.find(p.first);
        if (it == rhs->d.end())
            return False;
        if (!PyEq()(p.second, it->second))
            return False;
    }

    return True;
}

Box* dictNe(BoxedDict* self, Box* _rhs) {
    Box* eq = dictEq(self, _rhs);
    if (eq == NotImplemented)
        return eq;
    if (eq == True)
        return False;
    return True;
}

static PyObject* dict_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    int cmp;
    PyObject* res;

    if (!PyDict_Check(v) || !PyDict_Check(w)) {
        res = Py_NotImplemented;
    } else if (op == Py_EQ || op == Py_NE) {
        Box* result = dictEq((BoxedDict*)v, w);
        cmp = ((BoxedInt*)result)->n;
        if (cmp < 0)
            return NULL;
        res = (cmp == (op == Py_EQ)) ? Py_True : Py_False;
    } else {
        /* Py3K warning if comparison isn't == or !=  */
        if (PyErr_WarnPy3k("dict inequality comparisons not supported "
                           "in 3.x",
                           1) < 0) {
            return NULL;
        }
        res = Py_NotImplemented;
    }
    Py_INCREF(res);
    return res;
}

extern "C" Box* dictNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!PyType_Check(_cls))
        raiseExcHelper(TypeError, "dict.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, dict_cls))
        raiseExcHelper(TypeError, "dict.__new__(%s): %s is not a subtype of dict", getNameOfClass(cls),
                       getNameOfClass(cls));

    return new (cls) BoxedDict();
}

void dictMerge(BoxedDict* self, Box* other) {
    if (PyDict_Check(other)) {
        for (const auto& p : static_cast<BoxedDict*>(other)->d)
            self->d[p.first] = p.second;
        return;
    }

    Box* keys;
    if (other->cls == attrwrapper_cls) {
        keys = attrwrapperKeys(other);
    } else {
        static BoxedString* keys_str = internStringImmortal("keys");
        CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
        keys = callattr(other, keys_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    }
    assert(keys);

    for (Box* k : keys->pyElements()) {
        self->d[k] = getitemInternal<CXX>(other, k);
    }
}

void dictMergeFromSeq2(BoxedDict* self, Box* other) {
    int idx = 0;

    // raises if not iterable
    for (const auto& element : other->pyElements()) {

        // should this check subclasses? anyway to check if something is iterable...
        if (element->cls == list_cls) {
            BoxedList* list = static_cast<BoxedList*>(element);
            if (list->size != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %ld; 2 is required", idx,
                               list->size);

            self->d[list->elts->elts[0]] = list->elts->elts[1];
        } else if (element->cls == tuple_cls) {
            BoxedTuple* tuple = static_cast<BoxedTuple*>(element);
            if (tuple->size() != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %ld; 2 is required", idx,
                               tuple->size());

            self->d[tuple->elts[0]] = tuple->elts[1];
        } else
            raiseExcHelper(TypeError, "cannot convert dictionary update sequence element #%d to a sequence", idx);

        idx++;
    }
}

extern "C" int PyDict_Merge(PyObject* a, PyObject* b, int override_) noexcept {
    if (a == NULL || !PyDict_Check(a) || b == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }

    if (override_ != 1)
        Py_FatalError("unimplemented");

    try {
        dictMerge(static_cast<BoxedDict*>(a), b);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* dictUpdate(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    RELEASE_ASSERT(args->size() <= 1, ""); // should throw a TypeError
    if (args->size()) {
        Box* arg = args->elts[0];
        static BoxedString* keys_str = internStringImmortal("keys");
        if (getattrInternal<ExceptionStyle::CXX>(arg, keys_str)) {
            dictMerge(self, arg);
        } else {
            dictMergeFromSeq2(self, arg);
        }
    }

    if (kwargs && kwargs->d.size())
        dictMerge(self, kwargs);

    return None;
}

extern "C" Box* dictInit(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    int args_sz = args->size();
    int kwargs_sz = kwargs ? kwargs->d.size() : 0;

    // CPython accepts a single positional and keyword arguments, in any combination
    if (args_sz > 1)
        raiseExcHelper(TypeError, "dict expected at most 1 arguments, got %d", args_sz);

    dictUpdate(self, args, kwargs);

    if (kwargs) {
        // handle keyword arguments by merging (possibly over positional entries per CPy)
        assert(kwargs->cls == dict_cls);

        for (const auto& p : kwargs->d)
            self->d[p.first] = p.second;
    }

    return None;
}

void BoxedDict::gcHandler(GCVisitor* v, Box* b) {
    assert(PyDict_Check(b));

    Box::gcHandler(v, b);

    BoxedDict* d = (BoxedDict*)b;

    for (auto p : *d) {
        v->visit(&p.first);
        v->visit(&p.second);
    }
}

void BoxedDictIterator::gcHandler(GCVisitor* v, Box* b) {
    assert(b->cls == dict_iterator_cls);
    Box::gcHandler(v, b);

    BoxedDictIterator* it = static_cast<BoxedDictIterator*>(b);
    v->visit(&it->d);
}

void BoxedDictView::gcHandler(GCVisitor* v, Box* b) {
    assert(b->cls == dict_items_cls || b->cls == dict_values_cls || b->cls == dict_keys_cls);
    Box::gcHandler(v, b);

    BoxedDictView* view = static_cast<BoxedDictView*>(b);
    v->visit(&view->d);
}

static int dict_init(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    assert(PyDict_Check(self));
    try {
        dictInit(static_cast<BoxedDict*>(self), static_cast<BoxedTuple*>(args), static_cast<BoxedDict*>(kwds));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

static Box* dict_repr(PyObject* self) noexcept {
    assert(PyDict_Check(self));
    try {
        return dictRepr(static_cast<BoxedDict*>(self));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

void BoxedDict::dealloc(Box* b) noexcept {
    assert(PyDict_Check(b));
    static_cast<BoxedDict*>(b)->d.freeAllMemory();
}

void setupDict() {
    static PyMappingMethods dict_as_mapping;
    dict_cls->tp_as_mapping = &dict_as_mapping;
    static PySequenceMethods dict_as_sequence;
    dict_cls->tp_as_sequence = &dict_as_sequence;

    dict_iterator_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictIterator::gcHandler, 0, 0,
                                           sizeof(BoxedDictIterator), false, "dictionary-itemiterator");

    dict_keys_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictView::gcHandler, 0, 0, sizeof(BoxedDictView),
                                       false, "dict_keys");
    dict_values_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictView::gcHandler, 0, 0, sizeof(BoxedDictView),
                                         false, "dict_values");
    dict_items_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictView::gcHandler, 0, 0, sizeof(BoxedDictView),
                                        false, "dict_items");

    dict_iterator_cls->instances_are_nonzero = dict_keys_cls->instances_are_nonzero
        = dict_values_cls->instances_are_nonzero = dict_items_cls->instances_are_nonzero = true;

    dict_cls->tp_dealloc = &BoxedDict::dealloc;
    dict_cls->has_safe_tp_dealloc = true;

    dict_cls->giveAttr("__len__", new BoxedFunction(FunctionMetadata::create((void*)dictLen, BOXED_INT, 1)));
    dict_cls->giveAttr("__new__", new BoxedFunction(FunctionMetadata::create((void*)dictNew, UNKNOWN, 1, true, true)));
    dict_cls->giveAttr("__init__", new BoxedFunction(FunctionMetadata::create((void*)dictInit, NONE, 1, true, true)));
    dict_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)dictRepr, STR, 1)));

    dict_cls->giveAttr("__cmp__", new BoxedFunction(FunctionMetadata::create((void*)dictCmp, UNKNOWN, 2)));
    dict_cls->giveAttr("__eq__", new BoxedFunction(FunctionMetadata::create((void*)dictEq, UNKNOWN, 2)));
    dict_cls->giveAttr("__ne__", new BoxedFunction(FunctionMetadata::create((void*)dictNe, UNKNOWN, 2)));
    dict_cls->giveAttr("__lt__", new BoxedFunction(FunctionMetadata::create((void*)dictLt, UNKNOWN, 2)));
    dict_cls->giveAttr("__le__", new BoxedFunction(FunctionMetadata::create((void*)dictLe, UNKNOWN, 2)));
    dict_cls->giveAttr("__gt__", new BoxedFunction(FunctionMetadata::create((void*)dictGt, UNKNOWN, 2)));
    dict_cls->giveAttr("__ge__", new BoxedFunction(FunctionMetadata::create((void*)dictGe, UNKNOWN, 2)));

    dict_cls->tp_richcompare = dict_richcompare;
    dict_cls->tp_compare = dict_compare;
    dict_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create((void*)dictIterKeys,
                                                                              typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("update", new BoxedFunction(FunctionMetadata::create((void*)dictUpdate, NONE, 1, true, true)));

    dict_cls->giveAttr("clear", new BoxedFunction(FunctionMetadata::create((void*)dictClear, NONE, 1)));
    dict_cls->giveAttr("copy", new BoxedFunction(FunctionMetadata::create((void*)dictCopy, DICT, 1)));

    dict_cls->giveAttr("has_key", new BoxedFunction(FunctionMetadata::create((void*)dictContains, BOXED_BOOL, 2)));
    dict_cls->giveAttr("items", new BoxedFunction(FunctionMetadata::create((void*)dictItems, LIST, 1)));
    dict_cls->giveAttr("iteritems", new BoxedFunction(FunctionMetadata::create((void*)dictIterItems,
                                                                               typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("values", new BoxedFunction(FunctionMetadata::create((void*)dictValues, LIST, 1)));
    dict_cls->giveAttr("itervalues", new BoxedFunction(FunctionMetadata::create((void*)dictIterValues,
                                                                                typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("keys", new BoxedFunction(FunctionMetadata::create((void*)dictKeys, LIST, 1)));
    dict_cls->giveAttr("iterkeys", dict_cls->getattr(internStringMortal("__iter__")));

    dict_cls->giveAttr("pop",
                       new BoxedFunction(FunctionMetadata::create((void*)dictPop, UNKNOWN, 3, false, false), { NULL }));
    dict_cls->giveAttr("popitem", new BoxedFunction(FunctionMetadata::create((void*)dictPopitem, BOXED_TUPLE, 1)));

    auto* fromkeys_func
        = new BoxedFunction(FunctionMetadata::create((void*)dictFromkeys, UNKNOWN, 3, false, false), { None });
    dict_cls->giveAttr("fromkeys", PyClassMethod_New(fromkeys_func));

    dict_cls->giveAttr("viewkeys", new BoxedFunction(FunctionMetadata::create((void*)dictViewKeys, UNKNOWN, 1)));
    dict_cls->giveAttr("viewvalues", new BoxedFunction(FunctionMetadata::create((void*)dictViewValues, UNKNOWN, 1)));
    dict_cls->giveAttr("viewitems", new BoxedFunction(FunctionMetadata::create((void*)dictViewItems, UNKNOWN, 1)));

    dict_cls->giveAttr("get",
                       new BoxedFunction(FunctionMetadata::create((void*)dictGet, UNKNOWN, 3, false, false), { None }));

    dict_cls->giveAttr(
        "setdefault",
        new BoxedFunction(FunctionMetadata::create((void*)dictSetdefault, UNKNOWN, 3, false, false), { None }));

    auto dict_getitem = FunctionMetadata::create((void*)dictGetitem<CXX>, UNKNOWN, 2, ParamNames::empty(), CXX);
    dict_getitem->addVersion((void*)dictGetitem<CAPI>, UNKNOWN, CAPI);
    dict_cls->giveAttr("__getitem__", new BoxedFunction(dict_getitem));
    dict_cls->giveAttr("__setitem__", new BoxedFunction(FunctionMetadata::create((void*)dictSetitem, NONE, 3)));
    dict_cls->giveAttr("__delitem__", new BoxedFunction(FunctionMetadata::create((void*)dictDelitem, UNKNOWN, 2)));
    dict_cls->giveAttr("__contains__", new BoxedFunction(FunctionMetadata::create((void*)dictContains, BOXED_BOOL, 2)));

    dict_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)dictNonzero, BOXED_BOOL, 1)));

    dict_cls->freeze();

    FunctionMetadata* hasnext = FunctionMetadata::create((void*)dictIterHasnextUnboxed, BOOL, 1);
    hasnext->addVersion((void*)dictIterHasnext, BOXED_BOOL);
    dict_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    dict_iterator_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                                (void*)dictIterIter, typeFromClass(dict_iterator_cls), 1)));
    dict_iterator_cls->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)dictIterNext, UNKNOWN, 1)));
    dict_iterator_cls->freeze();
    dict_iterator_cls->tp_iter = PyObject_SelfIter;
    dict_iterator_cls->tp_iternext = dictiter_next;

    // Manually set some tp_* slots *after* calling freeze() -> fixup_slot_dispatchers().
    // fixup_slot_dispatchers will insert a wrapper like slot_tp_init into tp_init, which calls the python-level
    // __init__ function.  This is all well and good, until a C extension tries to subclass from dict and then
    // creates a new tp_init function which calls Py_DictType.tp_init().  That tp_init is slot_tp_init, which calls
    // self.__init__, which is the *subclasses* init function not dict's.
    //
    // This seems to happen pretty rarely, and only with dict, so for now let's just work around it by manually
    // setting the couple functions that get used.
    //
    // I'm not sure if CPython has a better mechanism for this, since I assume they allow having extension classes
    // subclass Python classes.
    dict_cls->tp_init = dict_init;
    dict_cls->tp_repr = dict_repr;
    dict_cls->tp_iter = dict_iter;

    dict_cls->tp_as_mapping->mp_length = (lenfunc)dict_length;
    dict_cls->tp_as_mapping->mp_subscript = (binaryfunc)dictGetitem<CAPI>;
    dict_cls->tp_as_mapping->mp_ass_subscript = (objobjargproc)dict_ass_sub;

    dict_cls->tp_as_sequence->sq_contains = (objobjproc)PyDict_Contains;

    dict_keys_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                            (void*)dictViewKeysIter, typeFromClass(dict_iterator_cls), 1)));
    dict_keys_cls->freeze();
    dict_values_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                              (void*)dictViewValuesIter, typeFromClass(dict_iterator_cls), 1)));
    dict_values_cls->freeze();
    dict_items_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                             (void*)dictViewItemsIter, typeFromClass(dict_iterator_cls), 1)));
    dict_items_cls->freeze();
}

void teardownDict() {
}
}
