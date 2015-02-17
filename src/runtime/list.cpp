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

#include "runtime/list.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "capi/types.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" int PyList_Append(PyObject* op, PyObject* newitem) noexcept {
    try {
        listAppend(op, newitem);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

extern "C" Box* listRepr(BoxedList* self) {
    LOCK_REGION(self->lock.asRead());

    // TODO highly inefficient with all the string copying
    std::ostringstream os;
    os << '[';
    for (int i = 0; i < self->size; i++) {
        if (i > 0)
            os << ", ";

        BoxedString* s = static_cast<BoxedString*>(self->elts->elts[i]->reprIC());
        os << s->s;
    }
    os << ']';
    return new BoxedString(os.str());
}

extern "C" Box* listNonzero(BoxedList* self) {
    return boxBool(self->size != 0);
}

extern "C" Box* listPop(BoxedList* self, Box* idx) {
    LOCK_REGION(self->lock.asWrite());

    if (idx == None) {
        if (self->size == 0) {
            raiseExcHelper(IndexError, "pop from empty list");
        }

        self->size--;
        Box* rtn = self->elts->elts[self->size];
        return rtn;
    }

    if (idx->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    int64_t n = static_cast<BoxedInt*>(idx)->n;
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        if (self->size == 0)
            fprintf(stderr, "IndexError: pop from empty list\n");
        else
            fprintf(stderr, "IndexError: pop index out of range\n");
        raiseExcHelper(IndexError, "");
    }

    Box* rtn = self->elts->elts[n];
    memmove(self->elts->elts + n, self->elts->elts + n + 1, (self->size - n - 1) * sizeof(Box*));
    self->size--;

    return rtn;
}

extern "C" Py_ssize_t PyList_Size(PyObject* self) noexcept {
    RELEASE_ASSERT(self->cls == list_cls, "");
    return static_cast<BoxedList*>(self)->size;
}

extern "C" Box* listLen(BoxedList* self) {
    assert(self->cls == list_cls);
    return boxInt(self->size);
}

Box* _listSlice(BoxedList* self, i64 start, i64 stop, i64 step, i64 length) {
    // printf("%ld %ld %ld\n", start, stop, step);
    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= self->size);
    } else {
        assert(start < self->size);
        assert(-1 <= stop);
    }

    BoxedList* rtn = new BoxedList();
    if (length > 0) {
        rtn->ensure(length);
        copySlice(&rtn->elts->elts[0], &self->elts->elts[0], start, step, length);
        rtn->size += length;
    }
    return rtn;
}

extern "C" Box* listGetitemUnboxed(BoxedList* self, int64_t n) {
    LOCK_REGION(self->lock.asRead());

    assert(self->cls == list_cls);
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }
    Box* rtn = self->elts->elts[n];
    return rtn;
}

extern "C" Box* listGetitemInt(BoxedList* self, BoxedInt* slice) {
    assert(isSubclass(slice->cls, int_cls));
    return listGetitemUnboxed(self, slice->n);
}

extern "C" PyObject* PyList_GetItem(PyObject* op, Py_ssize_t i) noexcept {
    RELEASE_ASSERT(PyList_Check(op), "");
    RELEASE_ASSERT(i >= 0, ""); // unlike list.__getitem__, PyList_GetItem doesn't do index wrapping
    try {
        return listGetitemUnboxed(static_cast<BoxedList*>(op), i);
    } catch (ExcInfo e) {
        abort();
    }
}

extern "C" Box* listGetitemSlice(BoxedList* self, BoxedSlice* slice) {
    LOCK_REGION(self->lock.asRead());

    assert(self->cls == list_cls);
    assert(slice->cls == slice_cls);
    i64 start, stop, step, length;
    parseSlice(slice, self->size, &start, &stop, &step, &length);
    return _listSlice(self, start, stop, step, length);
}

extern "C" Box* listGetitem(BoxedList* self, Box* slice) {
    assert(self->cls == list_cls);
    if (isSubclass(slice->cls, int_cls)) {
        return listGetitemInt(self, static_cast<BoxedInt*>(slice));
    } else if (slice->cls == slice_cls) {
        return listGetitemSlice(self, static_cast<BoxedSlice*>(slice));
    } else {
        raiseExcHelper(TypeError, "list indices must be integers, not %s", getTypeName(slice));
    }
}

static void _listSetitem(BoxedList* self, int64_t n, Box* v) {
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }

    self->elts->elts[n] = v;
}

extern "C" Box* listSetitemInt(BoxedList* self, BoxedInt* slice, Box* v) {
    // I think r lock is ok here, since we don't change the list structure:
    LOCK_REGION(self->lock.asRead());

    assert(self->cls == list_cls);
    assert(isSubclass(slice->cls, int_cls));
    int64_t n = slice->n;

    _listSetitem(self, n, v);

    return None;
}

extern "C" int PyList_SetItem(PyObject* op, Py_ssize_t i, PyObject* newitem) noexcept {
    assert(op->cls == list_cls);
    try {
        _listSetitem(static_cast<BoxedList*>(op), i, newitem);
    } catch (ExcInfo e) {
        abort();
    }
    return 0;
}

Box* listIAdd(BoxedList* self, Box* _rhs);

// Analogue of _PyEval_SliceIndex
static void sliceIndex(Box* b, int64_t* out) {
    if (b->cls == none_cls)
        return;

    RELEASE_ASSERT(b->cls == int_cls, "");
    *out = static_cast<BoxedInt*>(b)->n;
}

extern "C" Box* listSetitemSlice(BoxedList* self, BoxedSlice* slice, Box* v) {
    LOCK_REGION(self->lock.asWrite());

    assert(self->cls == list_cls);
    assert(slice->cls == slice_cls);

    i64 start = 0, stop = self->size, step = 1;

    sliceIndex(slice->start, &start);
    sliceIndex(slice->stop, &stop);
    sliceIndex(slice->step, &step);

    RELEASE_ASSERT(step == 1, "step sizes must be 1 for now");

    // Logic from PySequence_GetSlice:
    if (start < 0)
        start += self->size;
    if (stop < 0)
        stop += self->size;

    // Logic from list_ass_slice:
    if (start < 0)
        start = 0;
    else if (start > self->size)
        start = self->size;

    if (stop < start)
        stop = start;
    else if (stop > self->size)
        stop = self->size;

    assert(0 <= start && start <= stop && stop <= self->size);

    RELEASE_ASSERT(v->cls == list_cls, "unsupported %s", getTypeName(v));
    BoxedList* lv = static_cast<BoxedList*>(v);

    RELEASE_ASSERT(self->elts != lv->elts, "Slice self-assignment currently unsupported");

    int delts = lv->size - (stop - start);
    int remaining_elts = self->size - stop;
    self->ensure(delts);

    memmove(self->elts->elts + start + lv->size, self->elts->elts + stop, remaining_elts * sizeof(Box*));
    for (int i = 0; i < lv->size; i++) {
        Box* r = lv->elts->elts[i];
        self->elts->elts[start + i] = r;
    }

    self->size += delts;

    return None;
}

extern "C" Box* listSetitem(BoxedList* self, Box* slice, Box* v) {
    assert(self->cls == list_cls);
    if (isSubclass(slice->cls, int_cls)) {
        return listSetitemInt(self, static_cast<BoxedInt*>(slice), v);
    } else if (slice->cls == slice_cls) {
        return listSetitemSlice(self, static_cast<BoxedSlice*>(slice), v);
    } else {
        raiseExcHelper(TypeError, "list indices must be integers, not %s", getTypeName(slice));
    }
}

extern "C" Box* listDelitemInt(BoxedList* self, BoxedInt* slice) {
    LOCK_REGION(self->lock.asWrite());

    int64_t n = slice->n;
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }
    memmove(self->elts->elts + n, self->elts->elts + n + 1, (self->size - n - 1) * sizeof(Box*));
    self->size--;
    return None;
}

extern "C" Box* listDelitemSlice(BoxedList* self, BoxedSlice* slice) {
    LOCK_REGION(self->lock.asWrite());

    i64 start, stop, step;
    parseSlice(slice, self->size, &start, &stop, &step);
    RELEASE_ASSERT(step == 1, "step sizes must be 1 for now");

    // TODO this should reuse listSetitemSlice which does proper index handling
    assert(0 <= start && start < self->size);
    ASSERT(0 <= stop && stop <= self->size, "%ld %ld", self->size, stop);
    assert(start <= stop);

    int remaining_elts = self->size - stop;

    memmove(self->elts->elts + start, self->elts->elts + stop, remaining_elts * sizeof(Box*));
    self->size -= (stop - start);
    return None;
}

extern "C" Box* listDelitem(BoxedList* self, Box* slice) {
    LOCK_REGION(self->lock.asWrite());

    Box* rtn;

    if (isSubclass(slice->cls, int_cls)) {
        rtn = listDelitemInt(self, static_cast<BoxedInt*>(slice));
    } else if (slice->cls == slice_cls) {
        rtn = listDelitemSlice(self, static_cast<BoxedSlice*>(slice));
    } else {
        raiseExcHelper(TypeError, "list indices must be integers, not %s", getTypeName(slice));
    }
    self->shrink();
    return rtn;
}

extern "C" Box* listInsert(BoxedList* self, Box* idx, Box* v) {
    if (idx->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    LOCK_REGION(self->lock.asWrite());

    int64_t n = static_cast<BoxedInt*>(idx)->n;
    if (n < 0)
        n = self->size + n;

    if (n >= self->size) {
        listAppendInternal(self, v);
    } else {
        if (n < 0)
            n = 0;
        assert(0 <= n && n < self->size);

        self->ensure(1);
        memmove(self->elts->elts + n + 1, self->elts->elts + n, (self->size - n) * sizeof(Box*));

        self->size++;
        self->elts->elts[n] = v;
    }

    return None;
}

Box* listMul(BoxedList* self, Box* rhs) {
    if (rhs->cls != int_cls) {
        raiseExcHelper(TypeError, "can't multiply sequence by non-int of type '%s'", getTypeName(rhs));
    }

    LOCK_REGION(self->lock.asRead());

    int n = static_cast<BoxedInt*>(rhs)->n;
    int s = self->size;

    BoxedList* rtn = new BoxedList();
    rtn->ensure(n * s);
    if (s == 1) {
        for (int i = 0; i < n; i++) {
            listAppendInternal(rtn, self->elts->elts[0]);
        }
    } else {
        for (int i = 0; i < n; i++) {
            listAppendArrayInternal(rtn, &self->elts->elts[0], s);
        }
    }

    return rtn;
}

Box* listIAdd(BoxedList* self, Box* _rhs) {
    LOCK_REGION(self->lock.asWrite());

    if (_rhs->cls == list_cls) {
        // This branch is safe if self==rhs:
        BoxedList* rhs = static_cast<BoxedList*>(_rhs);

        int s1 = self->size;
        int s2 = rhs->size;
        self->ensure(s1 + s2);

        memcpy(self->elts->elts + s1, rhs->elts->elts, sizeof(rhs->elts->elts[0]) * s2);
        self->size = s1 + s2;
        return self;
    }

    RELEASE_ASSERT(_rhs != self, "unsupported");

    for (auto* b : _rhs->pyElements())
        listAppendInternal(self, b);

    return self;
}

Box* listAdd(BoxedList* self, Box* _rhs) {
    if (_rhs->cls != list_cls) {
        raiseExcHelper(TypeError, "can only concatenate list (not \"%s\") to list", getTypeName(_rhs));
    }

    LOCK_REGION(self->lock.asRead());

    BoxedList* rhs = static_cast<BoxedList*>(_rhs);

    BoxedList* rtn = new BoxedList();

    int s1 = self->size;
    int s2 = rhs->size;
    rtn->ensure(s1 + s2);

    memcpy(rtn->elts->elts, self->elts->elts, sizeof(self->elts->elts[0]) * s1);
    memcpy(rtn->elts->elts + s1, rhs->elts->elts, sizeof(rhs->elts->elts[0]) * s2);
    rtn->size = s1 + s2;
    return rtn;
}

Box* listReverse(BoxedList* self) {
    LOCK_REGION(self->lock.asWrite());

    assert(self->cls == list_cls);
    for (int i = 0, j = self->size - 1; i < j; i++, j--) {
        Box* e = self->elts->elts[i];
        self->elts->elts[i] = self->elts->elts[j];
        self->elts->elts[j] = e;
    }

    return None;
}

void listSort(BoxedList* self, Box* cmp, Box* key, Box* reverse) {
    LOCK_REGION(self->lock.asWrite());
    assert(self->cls == list_cls);

    RELEASE_ASSERT(cmp == None, "The 'cmp' keyword is currently not supported");

    if (key == None)
        key = NULL;

    int num_keys_added = 0;
    auto remove_keys = [&]() {
        for (int i = 0; i < num_keys_added; i++) {
            Box** obj_loc = &self->elts->elts[i];
            assert((*obj_loc)->cls == tuple_cls);
            *obj_loc = static_cast<BoxedTuple*>(*obj_loc)->elts[2];
        }
    };

    try {
        if (key) {
            for (int i = 0; i < self->size; i++) {
                Box** obj_loc = &self->elts->elts[i];

                Box* key_val = runtimeCall(key, ArgPassSpec(1), *obj_loc, NULL, NULL, NULL, NULL);
                // Add the index as part of the new tuple so that the comparison never hits the
                // original object.
                // TODO we could potentially make this faster by copying the CPython approach of
                // creating special sortwrapper objects that compare only based on the key.
                Box* new_obj = new BoxedTuple({ key_val, boxInt(i), *obj_loc });

                *obj_loc = new_obj;
                num_keys_added++;
            }
        }

        // We don't need to do a stable sort if there's a keyfunc, since we explicitly added the index
        // as part of the sort key.
        // But we might want to get rid of that approach?  CPython doesn't do that (they create special
        // wrapper objects that compare only based on the key).
        std::stable_sort<Box**, PyLt>(self->elts->elts, self->elts->elts + self->size, PyLt());
    } catch (ExcInfo e) {
        remove_keys();
        throw e;
    }

    remove_keys();

    if (nonzero(reverse)) {
        listReverse(self);
    }
}

Box* listSortFunc(BoxedList* self, Box* cmp, Box* key, Box** _args) {
    Box* reverse = _args[0];
    listSort(self, cmp, key, reverse);
    return None;
}

extern "C" int PyList_Sort(PyObject* v) noexcept {
    if (v == NULL || !PyList_Check(v)) {
        PyErr_BadInternalCall();
        return -1;
    }

    try {
        listSort((BoxedList*)v, None, None, False);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }

    return 0;
}

Box* listContains(BoxedList* self, Box* elt) {
    LOCK_REGION(self->lock.asRead());

    int size = self->size;
    for (int i = 0; i < size; i++) {
        Box* e = self->elts->elts[i];

        bool identity_eq = e == elt;
        if (identity_eq)
            return True;

        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            return True;
    }
    return False;
}

Box* listCount(BoxedList* self, Box* elt) {
    LOCK_REGION(self->lock.asRead());

    int size = self->size;
    int count = 0;

    for (int i = 0; i < size; i++) {
        Box* e = self->elts->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            count++;
    }
    return boxInt(count);
}

Box* listIndex(BoxedList* self, Box* elt) {
    LOCK_REGION(self->lock.asRead());

    int size = self->size;

    for (int i = 0; i < size; i++) {
        Box* e = self->elts->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            return boxInt(i);
    }

    BoxedString* tostr = static_cast<BoxedString*>(repr(elt));
    raiseExcHelper(ValueError, "%s is not in list", tostr->s.c_str());
}

Box* listRemove(BoxedList* self, Box* elt) {
    LOCK_REGION(self->lock.asWrite());

    assert(self->cls == list_cls);

    for (int i = 0; i < self->size; i++) {
        Box* e = self->elts->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);

        if (b) {
            memmove(self->elts->elts + i, self->elts->elts + i + 1, (self->size - i - 1) * sizeof(Box*));
            self->size--;
            return None;
        }
    }

    raiseExcHelper(ValueError, "list.remove(x): x not in list");
}

BoxedClass* list_iterator_cls = NULL;
BoxedClass* list_reverse_iterator_cls = NULL;

extern "C" void listIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);
    BoxedListIterator* it = (BoxedListIterator*)b;
    v->visit(it->l);
}

extern "C" Box* listNew(Box* cls, Box* container) {

    if (container == None)
        return new BoxedList();

    BoxedList* rtn = new BoxedList();
    for (Box* e : container->pyElements()) {
        listAppendInternal(rtn, e);
    }
    return rtn;
}

extern "C" PyObject* PyList_New(Py_ssize_t size) noexcept {
    try {
        BoxedList* l = new BoxedList();
        if (size) {
            // This function is supposed to return a list of `size` NULL elements.
            // That will probably trip an assert somewhere if we try to create that (ex
            // I think the GC will expect them to be real objects so they can be relocated),
            // so put None in instead
            l->ensure(size);

            for (Py_ssize_t i = 0; i < size; i++) {
                l->elts->elts[i] = None;
            }
            l->size = size;
        }
        return l;
    } catch (ExcInfo e) {
        abort();
    }
}

Box* _listCmp(BoxedList* lhs, BoxedList* rhs, AST_TYPE::AST_TYPE op_type) {
    int lsz = lhs->size;
    int rsz = rhs->size;

    bool is_order
        = (op_type == AST_TYPE::Lt || op_type == AST_TYPE::LtE || op_type == AST_TYPE::Gt || op_type == AST_TYPE::GtE);

    if (lsz != rsz) {
        if (op_type == AST_TYPE::Eq)
            return False;
        if (op_type == AST_TYPE::NotEq)
            return True;
    }

    int n = std::min(lsz, rsz);
    for (int i = 0; i < n; i++) {
        bool identity_eq = lhs->elts->elts[i] == rhs->elts->elts[i];
        if (identity_eq)
            continue;

        Box* is_eq = compareInternal(lhs->elts->elts[i], rhs->elts->elts[i], AST_TYPE::Eq, NULL);
        bool bis_eq = nonzero(is_eq);

        if (bis_eq)
            continue;

        if (op_type == AST_TYPE::Eq) {
            return boxBool(false);
        } else if (op_type == AST_TYPE::NotEq) {
            return boxBool(true);
        } else {
            Box* r = compareInternal(lhs->elts->elts[i], rhs->elts->elts[i], op_type, NULL);
            return r;
        }
    }

    if (op_type == AST_TYPE::Lt)
        return boxBool(lsz < rsz);
    else if (op_type == AST_TYPE::LtE)
        return boxBool(lsz <= rsz);
    else if (op_type == AST_TYPE::Gt)
        return boxBool(lsz > rsz);
    else if (op_type == AST_TYPE::GtE)
        return boxBool(lsz >= rsz);
    else if (op_type == AST_TYPE::Eq)
        return boxBool(lsz == rsz);
    else if (op_type == AST_TYPE::NotEq)
        return boxBool(lsz != rsz);

    RELEASE_ASSERT(0, "%d", op_type);
}

Box* listEq(BoxedList* self, Box* rhs) {
    if (rhs->cls != list_cls) {
        return NotImplemented;
    }

    LOCK_REGION(self->lock.asRead());

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::Eq);
}

Box* listNe(BoxedList* self, Box* rhs) {
    if (rhs->cls != list_cls) {
        return NotImplemented;
    }

    LOCK_REGION(self->lock.asRead());

    return _listCmp(self, static_cast<BoxedList*>(rhs), AST_TYPE::NotEq);
}

extern "C" PyObject* _PyList_Extend(PyListObject* self, PyObject* b) noexcept {
    BoxedList* l = (BoxedList*)self;
    assert(l->cls == list_cls);

    try {
        return listIAdd(l, b);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyList_SetSlice(PyObject* a, Py_ssize_t ilow, Py_ssize_t ihigh, PyObject* v) noexcept {
    if (!PyList_Check(a)) {
        PyErr_BadInternalCall();
        return -1;
    }

    BoxedList* l = (BoxedList*)a;
    ASSERT(l->cls == list_cls, "%s", l->cls->tp_name);

    try {
        if (v)
            listSetitemSlice(l, new BoxedSlice(boxInt(ilow), boxInt(ihigh), None), v);
        else
            listDelitemSlice(l, new BoxedSlice(boxInt(ilow), boxInt(ihigh), None));
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

void setupList() {
    list_iterator_cls
        = new BoxedHeapClass(object_cls, &listIteratorGCHandler, 0, sizeof(BoxedList), false, "listiterator");
    list_reverse_iterator_cls = new BoxedHeapClass(object_cls, &listIteratorGCHandler, 0, sizeof(BoxedListIterator),
                                                   false, "listreverseiterator");

    list_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)listLen, BOXED_INT, 1)));

    CLFunction* getitem = createRTFunction(2, 0, 0, 0);
    addRTFunction(getitem, (void*)listGetitemInt, UNKNOWN, std::vector<ConcreteCompilerType*>{ LIST, BOXED_INT });
    addRTFunction(getitem, (void*)listGetitemSlice, LIST, std::vector<ConcreteCompilerType*>{ LIST, SLICE });
    addRTFunction(getitem, (void*)listGetitem, UNKNOWN, std::vector<ConcreteCompilerType*>{ LIST, UNKNOWN });
    list_cls->giveAttr("__getitem__", new BoxedFunction(getitem));

    list_cls->giveAttr("__iter__",
                       new BoxedFunction(boxRTFunction((void*)listIter, typeFromClass(list_iterator_cls), 1)));

    list_cls->giveAttr("__reversed__", new BoxedFunction(boxRTFunction((void*)listReversed,
                                                                       typeFromClass(list_reverse_iterator_cls), 1)));

    list_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)listEq, UNKNOWN, 2)));
    list_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)listNe, UNKNOWN, 2)));

    list_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)listRepr, STR, 1)));
    list_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)listNonzero, BOXED_BOOL, 1)));

    list_cls->giveAttr("pop", new BoxedFunction(boxRTFunction((void*)listPop, UNKNOWN, 2, 1, false, false), { None }));

    list_cls->giveAttr("append", new BoxedFunction(boxRTFunction((void*)listAppend, NONE, 2)));
    list_cls->giveAttr("extend", new BoxedFunction(boxRTFunction((void*)listIAdd, LIST, 2)));

    CLFunction* setitem = createRTFunction(3, 0, false, false);
    addRTFunction(setitem, (void*)listSetitemInt, NONE, std::vector<ConcreteCompilerType*>{ LIST, BOXED_INT, UNKNOWN });
    addRTFunction(setitem, (void*)listSetitemSlice, NONE, std::vector<ConcreteCompilerType*>{ LIST, SLICE, UNKNOWN });
    addRTFunction(setitem, (void*)listSetitem, NONE, std::vector<ConcreteCompilerType*>{ LIST, UNKNOWN, UNKNOWN });
    list_cls->giveAttr("__setitem__", new BoxedFunction(setitem));

    CLFunction* delitem = createRTFunction(2, 0, false, false);
    addRTFunction(delitem, (void*)listDelitemInt, NONE, std::vector<ConcreteCompilerType*>{ LIST, BOXED_INT });
    addRTFunction(delitem, (void*)listDelitemSlice, NONE, std::vector<ConcreteCompilerType*>{ LIST, SLICE });
    addRTFunction(delitem, (void*)listDelitem, NONE, std::vector<ConcreteCompilerType*>{ LIST, UNKNOWN });
    list_cls->giveAttr("__delitem__", new BoxedFunction(delitem));

    list_cls->giveAttr("insert", new BoxedFunction(boxRTFunction((void*)listInsert, NONE, 3)));
    list_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)listMul, LIST, 2)));
    list_cls->giveAttr("__rmul__", new BoxedFunction(boxRTFunction((void*)listMul, LIST, 2)));

    list_cls->giveAttr("__iadd__", new BoxedFunction(boxRTFunction((void*)listIAdd, UNKNOWN, 2)));
    list_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)listAdd, UNKNOWN, 2)));

    list_cls->giveAttr("sort", new BoxedFunction(boxRTFunction((void*)listSortFunc, NONE, 4, 3, false, false,
                                                               ParamNames({ "", "cmp", "key", "reverse" }, "", "")),
                                                 { None, None, False }));
    list_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)listContains, BOXED_BOOL, 2)));

    list_cls->giveAttr("__new__",
                       new BoxedFunction(boxRTFunction((void*)listNew, UNKNOWN, 2, 1, false, false), { None }));

    list_cls->giveAttr("count", new BoxedFunction(boxRTFunction((void*)listCount, BOXED_INT, 2)));
    list_cls->giveAttr("index", new BoxedFunction(boxRTFunction((void*)listIndex, BOXED_INT, 2)));
    list_cls->giveAttr("remove", new BoxedFunction(boxRTFunction((void*)listRemove, NONE, 2)));
    list_cls->giveAttr("reverse", new BoxedFunction(boxRTFunction((void*)listReverse, NONE, 1)));
    list_cls->freeze();

    CLFunction* hasnext = boxRTFunction((void*)listiterHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)listiterHasnext, BOXED_BOOL);
    list_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    list_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)listIterIter, typeFromClass(list_iterator_cls), 1)));
    list_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)listiterNext, UNKNOWN, 1)));

    list_iterator_cls->freeze();

    list_reverse_iterator_cls->giveAttr("__name__", boxStrConstant("listreverseiterator"));

    hasnext = boxRTFunction((void*)listreviterHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)listreviterHasnext, BOXED_BOOL);
    list_reverse_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    list_reverse_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)listIterIter, typeFromClass(list_reverse_iterator_cls), 1)));
    list_reverse_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)listreviterNext, UNKNOWN, 1)));

    list_reverse_iterator_cls->freeze();
}

void teardownList() {
    // TODO do clearattrs?
    // decref(list_iterator_cls);
    // decref(list_reverse_iterator_cls);
}
}
