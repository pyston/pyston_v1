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

#include "runtime/tuple.h"

#include <algorithm>

#include "capi/typeobject.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" Box* createTuple(int64_t nelts, Box** elts) {
    return BoxedTuple::create(nelts, elts);
}

Box* _tupleSlice(BoxedTuple* self, i64 start, i64 stop, i64 step, i64 length) {
    i64 size = self->size();
    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= size);
    } else {
        assert(start < size);
        assert(-1 <= stop);
    }

    // FIXME: No need to initialize with NULL, since we're going to fill it in
    auto rtn = BoxedTuple::create(length);
    if (length > 0)
        copySlice(&rtn->elts[0], &self->elts[0], start, step, length);
    for (int i = 0; i < length; i++) {
        Py_INCREF(rtn->elts[i]);
    }
    return rtn;
}

BORROWED(Box*) tupleGetitemUnboxedBorrowed(BoxedTuple* self, i64 n) {
    i64 size = self->size();

    if (n < 0)
        n = size + n;
    if (n < 0 || n >= size)
        raiseExcHelper(IndexError, "tuple index out of range");

    Box* rtn = self->elts[n];
    return rtn;
}

Box* tupleGetitemUnboxed(BoxedTuple* self, i64 n) {
    return incref(tupleGetitemUnboxedBorrowed(self, n));
}

Box* tupleGetitemInt(BoxedTuple* self, BoxedInt* slice) {
    return tupleGetitemUnboxed(self, slice->n);
}

extern "C" BORROWED(PyObject*) PyTuple_GetItem(PyObject* op, Py_ssize_t i) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");
    RELEASE_ASSERT(i >= 0, ""); // unlike tuple.__getitem__, PyTuple_GetItem doesn't do index wrapping
    try {
        return tupleGetitemUnboxedBorrowed(static_cast<BoxedTuple*>(op), i);
    } catch (ExcInfo e) {
        abort();
    }
}

Box* tupleGetitemSlice(BoxedTuple* self, BoxedSlice* slice) {
    assert(PyTuple_Check(self));
    assert(slice->cls == slice_cls);

    i64 start, stop, step, length;
    parseSlice(slice, self->size(), &start, &stop, &step, &length);
    return _tupleSlice(self, start, stop, step, length);
}

extern "C" PyObject* PyTuple_GetSlice(PyObject* p, Py_ssize_t low, Py_ssize_t high) noexcept {
    RELEASE_ASSERT(PyTuple_Check(p), "");
    BoxedTuple* t = static_cast<BoxedTuple*>(p);

    Py_ssize_t n = t->size();
    if (low < 0)
        low = 0;
    if (high > n)
        high = n;
    if (high < low)
        high = low;

    if (low == 0 && high == n)
        return p;

    return BoxedTuple::create(high - low, &t->elts[low]);
}

extern "C" int _PyTuple_Resize(PyObject** pv, Py_ssize_t newsize) noexcept {
    // This is only allowed to be called when there is only one user of the tuple (ie a refcount of 1 in CPython)
    assert(pv);
    return BoxedTuple::Resize((BoxedTuple**)pv, newsize);
}

int BoxedTuple::Resize(BoxedTuple** pv, size_t newsize) noexcept {
    // cpythons _PyTuple_Resize with small s/PyTupleObject/BoxedTuple modifications
    BoxedTuple* v;
    BoxedTuple* sv;
    Py_ssize_t i;
    Py_ssize_t oldsize;

    v = *pv;
    if (v == NULL || v->cls != &PyTuple_Type || (Py_SIZE(v) != 0 && Py_REFCNT(v) != 1)) {
        *pv = 0;
        Py_XDECREF(v);
        PyErr_BadInternalCall();
        return -1;
    }
    oldsize = Py_SIZE(v);
    if (oldsize == newsize)
        return 0;

    if (oldsize == 0) {
        /* Empty tuples are often shared, so we should never
           resize them in-place even if we do own the only
           (current) reference */
        Py_DECREF(v);
        *pv = (BoxedTuple*)PyTuple_New(newsize);
        return *pv == NULL ? -1 : 0;
    }

    /* XXX UNREF/NEWREF interface should be more symmetrical */
    _Py_DEC_REFTOTAL;
    if (_PyObject_GC_IS_TRACKED(v))
        _PyObject_GC_UNTRACK(v);
    _Py_ForgetReference((PyObject*)v);
    /* DECREF items deleted by shrinkage */
    for (i = newsize; i < oldsize; i++) {
        Py_CLEAR(v->elts[i]);
    }
    sv = PyObject_GC_Resize(BoxedTuple, v, newsize);
    if (sv == NULL) {
        *pv = NULL;
        PyObject_GC_Del(v);
        return -1;
    }
    _Py_NewReference((PyObject*)sv);
    /* Zero out items added by growing */
    if (newsize > oldsize)
        memset(&sv->elts[oldsize], 0, sizeof(*sv->elts) * (newsize - oldsize));
    *pv = sv;
    _PyObject_GC_TRACK(sv);
    return 0;
}

template <ExceptionStyle S> Box* tupleGetitem(BoxedTuple* self, Box* slice) {
    if (S == CAPI) {
        try {
            return tupleGetitem<CXX>(self, slice);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    }

    assert(PyTuple_Check(self));

    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            throwCAPIException();
        return tupleGetitemUnboxed(self, i);
    } else if (slice->cls == slice_cls)
        return tupleGetitemSlice(self, static_cast<BoxedSlice*>(slice));
    else
        raiseExcHelper(TypeError, "tuple indices must be integers, not %s", getTypeName(slice));
}

Box* tupleAdd(BoxedTuple* self, Box* rhs) {
    if (!PyTuple_Check(rhs)) {
        return incref(NotImplemented);
    }

    BoxedTuple* _rhs = static_cast<BoxedTuple*>(rhs);

    BoxedTuple* rtn = BoxedTuple::create(self->size() + _rhs->size());
    memmove(&rtn->elts[0], &self->elts[0], self->size() * sizeof(Box*));
    memmove(&rtn->elts[self->size()], &_rhs->elts[0], _rhs->size() * sizeof(Box*));
    for (int i = 0; i < rtn->size(); i++)
        Py_INCREF(rtn->elts[i]);
    return rtn;
}

Box* tupleMulInt(BoxedTuple* self, int n) {
    int s = self->size();

    if (n < 0)
        n = 0;

    if ((s == 0 || n == 1) && PyTuple_CheckExact(self)) {
        return incref(self);
    } else {
        BoxedTuple* rtn = BoxedTuple::create(n * s);
        int rtn_i = 0;

        for (int i = 0; i < n; ++i) {
            memmove(&rtn->elts[rtn_i], &self->elts[0], sizeof(Box*) * s);
            rtn_i += s;
        }

        for (int i = 0; i < rtn->size(); i++)
            Py_INCREF(rtn->elts[i]);

        return rtn;
    }
}

Box* tupleMul(BoxedTuple* self, Box* rhs) {
    Py_ssize_t n;

    if (PyIndex_Check(rhs)) {
        n = PyNumber_AsSsize_t(rhs, PyExc_OverflowError);
        if (n == -1 && PyErr_Occurred())
            throwCAPIException();
        return tupleMulInt(self, n);
    } else {
        return incref(NotImplemented);
    }
}

Box* tupleLen(BoxedTuple* t) {
    assert(PyTuple_Check(t));
    return boxInt(t->size());
}

extern "C" Py_ssize_t PyTuple_Size(PyObject* op) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");
    return static_cast<BoxedTuple*>(op)->size();
}

Box* tupleRepr(BoxedTuple* t) {

    assert(PyTuple_Check(t));
    int n;
    std::vector<char> chars;
    int status = Py_ReprEnter((PyObject*)t);
    n = t->size();
    if (n == 0) {
        chars.push_back('(');
        chars.push_back(')');
        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }

    if (status != 0) {
        if (status < 0)
            throwCAPIException();
        chars.push_back('(');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back(')');

        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }

    try {
        chars.push_back('(');
        for (int i = 0; i < n; i++) {
            if (i) {
                chars.push_back(',');
                chars.push_back(' ');
            }
            BoxedString* elt_repr = static_cast<BoxedString*>(repr(t->elts[i]));
            chars.insert(chars.end(), elt_repr->s().begin(), elt_repr->s().end());
            Py_DECREF(elt_repr);
        }

        if (n == 1)
            chars.push_back(',');

        chars.push_back(')');
    } catch (ExcInfo e) {
        Py_ReprLeave((PyObject*)t);
        throw e;
    }
    Py_ReprLeave((PyObject*)t);

    return boxString(llvm::StringRef(&chars[0], chars.size()));
}

Box* tupleNonzero(BoxedTuple* self) {
    RELEASE_ASSERT(PyTuple_Check(self), "");
    return boxBool(self->size() != 0);
}

Box* tupleContains(BoxedTuple* self, Box* elt) {
    int size = self->size();
    for (Box* e : *self) {
        int r = PyObject_RichCompareBool(elt, e, Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r)
            Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

Box* tupleIndex(BoxedTuple* self, Box* elt, Box* startBox, Box** args) {
    Box* endBox = args[0];

    Py_ssize_t start, end;
    _PyEval_SliceIndex(startBox, &start);
    _PyEval_SliceIndex(endBox, &end);

    Py_ssize_t size = self->size();

    if (start < 0) {
        start += size;
        if (start < 0) {
            start = 0;
        }
    }
    if (end < 0) {
        end += size;
        if (end < 0) {
            end = 0;
        }
    } else if (end > size) {
        end = size;
    }

    for (Py_ssize_t i = start; i < end; i++) {
        Box* e = self->elts[i];

        int r = PyObject_RichCompareBool(e, elt, Py_EQ);
        if (r == -1)
            throwCAPIException();

        if (r)
            return boxInt(i);
    }

    raiseExcHelper(ValueError, "tuple.index(x): x not in tuple");
}

Box* tupleCount(BoxedTuple* self, Box* elt) {
    int size = self->size();
    int count = 0;
    for (int i = 0; i < size; i++) {
        Box* e = self->elts[i];
        int r = PyObject_RichCompareBool(e, elt, Py_EQ);
        if (r == -1)
            throwCAPIException();
        if (r)
            count++;
    }
    return boxInt(count);
}

extern "C" Box* tupleNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!PyType_Check(_cls))
        raiseExcHelper(TypeError, "tuple.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, tuple_cls))
        raiseExcHelper(TypeError, "tuple.__new__(%s): %s is not a subtype of tuple", getNameOfClass(cls),
                       getNameOfClass(cls));

    int args_sz = args->size();
    int kwargs_sz = kwargs ? kwargs->d.size() : 0;

    if (args_sz + kwargs_sz > 1)
        raiseExcHelper(TypeError, "tuple() takes at most 1 argument (%d given)", args_sz + kwargs_sz);

    if (args_sz || kwargs_sz) {
        Box* elements;
        // if initializing from iterable argument, check common case positional args first
        if (args_sz) {
            elements = args->elts[0];
        } else {
            assert(kwargs_sz);
            auto const seq = *(kwargs->d.begin());
            auto const kw = static_cast<BoxedString*>(seq.first.value);

            if (kw->s() == "sequence")
                elements = seq.second;
            else
                raiseExcHelper(TypeError, "'%s' is an invalid keyword argument for this function", kw->data());
        }

        if (cls == tuple_cls) {
            // Call PySequence_Tuple since it has some perf special-cases
            // that can make it quite a bit faster than the generic pyElements iteration:
            Box* r = PySequence_Tuple(elements);
            if (!r)
                throwCAPIException();
            return r;
        }

        std::vector<Box*> elts;
        try {
            for (auto e : elements->pyElements())
                elts.push_back(e);
        } catch (ExcInfo e) {
            for (auto e : elts)
                Py_DECREF(e);
            throw e;
        }

        auto rtn = BoxedTuple::create(elts.size(), cls);
        memcpy(&rtn->elts[0], &elts[0], elts.size() * sizeof(Box*));

        return rtn;
    } else {
        if (cls == tuple_cls)
            return incref(EmptyTuple);
        return BoxedTuple::create(0, cls);
    }
}

extern "C" int PyTuple_SetItem(PyObject* op, Py_ssize_t i, STOLEN(PyObject*) newitem) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");

    BoxedTuple* t = static_cast<BoxedTuple*>(op);
    RELEASE_ASSERT(i >= 0 && i < t->size(), "");

    auto olditem = t->elts[i];
    t->elts[i] = newitem;
    Py_XDECREF(olditem);
    return 0;
}

extern "C" PyObject* PyTuple_Pack(Py_ssize_t n, ...) noexcept {
    va_list vargs;

    va_start(vargs, n);
    PyObject* result = PyTuple_New(n);

    if (result == NULL) {
        va_end(vargs);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* o = va_arg(vargs, PyObject*);
        Py_INCREF(o);
        PyTuple_SetItem(result, i, o);
    }
    va_end(vargs);
    return result;
}

extern "C" PyObject* PyTuple_New(Py_ssize_t size) noexcept {
    RELEASE_ASSERT(size >= 0, "");

    if (size == 0) {
        Py_INCREF(EmptyTuple);
        return EmptyTuple;
    }
    return BoxedTuple::create(size);
}


BoxedClass* tuple_iterator_cls = NULL;
static int64_t tuple_hash(BoxedTuple* v) noexcept {
    long x, y;
    Py_ssize_t len = Py_SIZE(v);
    PyObject** p;
    long mult = 1000003L;
    x = 0x345678L;
    p = v->elts;
    while (--len >= 0) {
        y = PyObject_Hash(*p++);
        if (y == -1)
            return -1;
        x = (x ^ y) * mult;
        /* the cast might truncate len; that doesn't change hash stability */
        mult += (long)(82520L + len + len);
    }
    x += 97531L;
    if (x == -1)
        x = -2;
    return x;
}

static PyObject* tuplerichcompare(PyObject* v, PyObject* w, int op) noexcept {
    BoxedTuple* vt, *wt;
    Py_ssize_t i;
    Py_ssize_t vlen, wlen;

    if (!PyTuple_Check(v) || !PyTuple_Check(w)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    vt = (BoxedTuple*)v;
    wt = (BoxedTuple*)w;

    vlen = Py_SIZE(vt);
    wlen = Py_SIZE(wt);

    /* Note:  the corresponding code for lists has an "early out" test
     * here when op is EQ or NE and the lengths differ.  That pays there,
     * but Tim was unable to find any real code where EQ/NE tuple
     * compares don't have the same length, so testing for it here would
     * have cost without benefit.
     */

    /* Search for the first index where items are different.
     * Note that because tuples are immutable, it's safe to reuse
     * vlen and wlen across the comparison calls.
     */
    for (i = 0; i < vlen && i < wlen; i++) {
        int k = PyObject_RichCompareBool(vt->elts[i], wt->elts[i], Py_EQ);
        if (k < 0)
            return NULL;
        if (!k)
            break;
    }

    if (i >= vlen || i >= wlen) {
        /* No more items to compare -- compare sizes */
        int cmp;
        PyObject* res;
        switch (op) {
            case Py_LT:
                cmp = vlen < wlen;
                break;
            case Py_LE:
                cmp = vlen <= wlen;
                break;
            case Py_EQ:
                cmp = vlen == wlen;
                break;
            case Py_NE:
                cmp = vlen != wlen;
                break;
            case Py_GT:
                cmp = vlen > wlen;
                break;
            case Py_GE:
                cmp = vlen >= wlen;
                break;
            default:
                return NULL; /* cannot happen */
        }
        if (cmp)
            res = Py_True;
        else
            res = Py_False;
        Py_INCREF(res);
        return res;
    }

    /* We have an item that differs -- shortcuts for EQ/NE */
    if (op == Py_EQ) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    if (op == Py_NE) {
        Py_INCREF(Py_True);
        return Py_True;
    }

    /* Compare the final item again using the proper operator */
    return PyObject_RichCompare(vt->elts[i], wt->elts[i], op);
}

static PyObject* tupleslice(PyTupleObject* a, Py_ssize_t ilow, Py_ssize_t ihigh) noexcept {
    PyTupleObject* np;
    PyObject** src, **dest;
    Py_ssize_t i;
    Py_ssize_t len;
    if (ilow < 0)
        ilow = 0;
    if (ihigh > Py_SIZE(a))
        ihigh = Py_SIZE(a);
    if (ihigh < ilow)
        ihigh = ilow;
    if (ilow == 0 && ihigh == Py_SIZE(a) && PyTuple_CheckExact((PyObject*)a)) {
        Py_INCREF(a);
        return (PyObject*)a;
    }
    len = ihigh - ilow;
    np = (PyTupleObject*)PyTuple_New(len);
    if (np == NULL)
        return NULL;
    src = a->ob_item + ilow;
    dest = np->ob_item;
    for (i = 0; i < len; i++) {
        PyObject* v = src[i];
        Py_INCREF(v);
        dest[i] = v;
    }
    return (PyObject*)np;
}

static PyObject* tupleitem(register PyTupleObject* a, register Py_ssize_t i) {
    if (i < 0 || i >= Py_SIZE(a)) {
        PyErr_SetString(PyExc_IndexError, "tuple index out of range");
        return NULL;
    }
    Py_INCREF(a->ob_item[i]);
    return a->ob_item[i];
}

static Py_ssize_t tuplelength(PyTupleObject* a) noexcept {
    return Py_SIZE(a);
}

static Box* tupleconcat(PyTupleObject* a, Box* bb) noexcept {
    Py_ssize_t size;
    Py_ssize_t i;
    PyObject** src, **dest;
    PyTupleObject* np;
    if (!PyTuple_Check(bb)) {
        PyErr_Format(PyExc_TypeError, "can only concatenate tuple (not \"%.200s\") to tuple", Py_TYPE(bb)->tp_name);
        return NULL;
    }
#define b ((PyTupleObject*)bb)
    size = Py_SIZE(a) + Py_SIZE(b);
    if (size < 0)
        return PyErr_NoMemory();
    np = (PyTupleObject*)PyTuple_New(size);
    if (np == NULL) {
        return NULL;
    }
    src = a->ob_item;
    dest = np->ob_item;
    for (i = 0; i < Py_SIZE(a); i++) {
        PyObject* v = src[i];
        Py_INCREF(v);
        dest[i] = v;
    }
    src = b->ob_item;
    dest = np->ob_item + Py_SIZE(a);
    for (i = 0; i < Py_SIZE(b); i++) {
        PyObject* v = src[i];
        Py_INCREF(v);
        dest[i] = v;
    }
    return (PyObject*)np;
#undef b
}

static PyObject* tuplerepeat(PyTupleObject* a, Py_ssize_t n) noexcept {
    Py_ssize_t i, j;
    Py_ssize_t size;
    PyTupleObject* np;
    PyObject** p, **items;
    if (n < 0)
        n = 0;
    if (Py_SIZE(a) == 0 || n == 1) {
        if (PyTuple_CheckExact((BoxedTuple*)a)) {
            /* Since tuples are immutable, we can return a shared
             *                copy in this case */
            Py_INCREF(a);
            return (PyObject*)a;
        }
        if (Py_SIZE(a) == 0)
            return PyTuple_New(0);
    }
    size = Py_SIZE(a) * n;
    if (size / Py_SIZE(a) != n)
        return PyErr_NoMemory();
    np = (PyTupleObject*)PyTuple_New(size);
    if (np == NULL)
        return NULL;
    p = np->ob_item;
    items = a->ob_item;
    for (i = 0; i < n; i++) {
        for (j = 0; j < Py_SIZE(a); j++) {
            *p = items[j];
            Py_INCREF(*p);
            p++;
        }
    }
    return (PyObject*)np;
}

static Box* tuple_getnewargs(Box* _self) noexcept {
    RELEASE_ASSERT(PyTuple_Check(_self), "");
    PyTupleObject* v = reinterpret_cast<PyTupleObject*>(_self);
    return Py_BuildValue("(N)", tupleslice(v, 0, Py_SIZE(v)));
}

extern "C" void _PyTuple_MaybeUntrack(PyObject* op) noexcept {
    PyTupleObject* t;
    Py_ssize_t i, n;

    if (!PyTuple_CheckExact(op) || !_PyObject_GC_IS_TRACKED(op))
        return;
    t = (PyTupleObject*)op;
    n = Py_SIZE(t);
    for (i = 0; i < n; i++) {
        PyObject* elt = PyTuple_GET_ITEM(t, i);
        /* Tuple with NULL elements aren't
           fully constructed, don't untrack
           them yet. */
        if (!elt || _PyObject_GC_MAY_BE_TRACKED(elt))
            return;
    }
#ifdef SHOW_TRACK_COUNT
    count_tracked--;
    count_untracked++;
#endif
    _PyObject_GC_UNTRACK(op);
}

extern "C" int PyTuple_ClearFreeList() noexcept {
    return 0; // number of entries cleared
}

void setupTuple() {
    static PySequenceMethods tuple_as_sequence;
    tuple_cls->tp_as_sequence = &tuple_as_sequence;
    static PyMappingMethods tuple_as_mapping;
    tuple_cls->tp_as_mapping = &tuple_as_mapping;

    tuple_iterator_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedTupleIterator), false,
                                            "tupleiterator", false, (destructor)BoxedTupleIterator::dealloc, NULL, true,
                                            (traverseproc)BoxedTupleIterator::traverse, NOCLEAR);

    tuple_cls->giveAttr("__new__",
                        new BoxedFunction(FunctionMetadata::create((void*)tupleNew, UNKNOWN, 1, true, true)));
    FunctionMetadata* getitem = new FunctionMetadata(2, 0, 0);
    getitem->addVersion((void*)tupleGetitemInt, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, BOXED_INT });
    getitem->addVersion((void*)tupleGetitemSlice, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, SLICE });
    getitem->addVersion((void*)tupleGetitem<CXX>, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN }, CXX);
    getitem->addVersion((void*)tupleGetitem<CAPI>, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN },
                        CAPI);
    tuple_cls->giveAttr("__getitem__", new BoxedFunction(getitem));

    tuple_cls->giveAttr("__contains__",
                        new BoxedFunction(FunctionMetadata::create((void*)tupleContains, BOXED_BOOL, 2)));
    tuple_cls->giveAttr(
        "index",
        new BoxedFunction(FunctionMetadata::create((void*)tupleIndex, BOXED_INT, 4, false, false),
                          { autoDecref(boxInt(0)), autoDecref(boxInt(std::numeric_limits<Py_ssize_t>::max())) }));
    tuple_cls->giveAttr("count", new BoxedFunction(FunctionMetadata::create((void*)tupleCount, BOXED_INT, 2)));

    tuple_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create((void*)tupleIter,
                                                                               typeFromClass(tuple_iterator_cls), 1)));


    tuple_cls->tp_richcompare = tuplerichcompare;

    tuple_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)tupleNonzero, BOXED_BOOL, 1)));

    tuple_cls->giveAttr("__len__", new BoxedFunction(FunctionMetadata::create((void*)tupleLen, BOXED_INT, 1)));
    tuple_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)tupleRepr, STR, 1)));

    // Return type is UNKNOWN as it could be NotImplemented.
    tuple_cls->giveAttr("__add__", new BoxedFunction(FunctionMetadata::create((void*)tupleAdd, UNKNOWN, 2)));
    tuple_cls->giveAttr("__mul__", new BoxedFunction(FunctionMetadata::create((void*)tupleMul, UNKNOWN, 2)));
    tuple_cls->giveAttr("__rmul__", new BoxedFunction(FunctionMetadata::create((void*)tupleMul, UNKNOWN, 2)));

    tuple_cls->giveAttr("__getnewargs__", new BoxedFunction(FunctionMetadata::create((void*)tuple_getnewargs, UNKNOWN,
                                                                                     1, ParamNames::empty(), CAPI)));

    tuple_cls->tp_hash = (hashfunc)tuple_hash;
    tuple_cls->tp_as_sequence->sq_slice = (ssizessizeargfunc)&tupleslice;
    add_operators(tuple_cls);

    tuple_cls->freeze();

    tuple_cls->tp_as_sequence->sq_item = (ssizeargfunc)tupleitem;
    tuple_cls->tp_as_sequence->sq_length = (lenfunc)tuplelength;
    tuple_cls->tp_as_sequence->sq_concat = (binaryfunc)tupleconcat;
    tuple_cls->tp_as_sequence->sq_repeat = (ssizeargfunc)tuplerepeat;
    tuple_cls->tp_iter = tupleIter;

    FunctionMetadata* hasnext = FunctionMetadata::create((void*)tupleiterHasnextUnboxed, BOOL, 1);
    hasnext->addVersion((void*)tupleiterHasnext, BOXED_BOOL);
    tuple_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    tuple_iterator_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create(
                                                 (void*)tupleIterIter, typeFromClass(tuple_iterator_cls), 1)));
    tuple_iterator_cls->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)tupleiterNext, UNKNOWN, 1)));

    tuple_iterator_cls->freeze();
    tuple_iterator_cls->tpp_hasnext = tupleiterHasnextUnboxed;
    tuple_iterator_cls->tp_iternext = tupleiter_next;
    tuple_iterator_cls->tp_iter = PyObject_SelfIter;
}
}
