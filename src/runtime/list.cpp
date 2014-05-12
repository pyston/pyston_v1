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

#include <cstring>
#include <sstream>
#include <algorithm>

#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"

#include "runtime/gc_runtime.h"
#include "runtime/list.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

#include "codegen/compvars.h"

#include "gc/collector.h"

namespace pyston {

extern "C" Box* listRepr(BoxedList* self) {
    // TODO highly inefficient with all the string copying
    std::ostringstream os;
    os << '[';
    for (int i = 0; i < self->size; i++) {
        if (i > 0)
            os << ", ";

        BoxedString *s = static_cast<BoxedString*>(repr(self->elts->elts[i]));
        os << s->s;
    }
    os << ']';
    return new BoxedString(os.str());
}

extern "C" Box* listNonzero(BoxedList* self) {
    return boxBool(self->size != 0);
}

extern "C" Box* listPop1(BoxedList* self) {
    if (self->size == 0) {
        fprintf(stderr, "IndexError: pop from empty list\n");
        raiseExc();
    }

    self->size--;
    Box* rtn = self->elts->elts[self->size];
    return rtn;
}

extern "C" Box* listPop2(BoxedList* self, Box* idx) {
    if (idx->cls != int_cls) {
        fprintf(stderr, "TypeError: an integer is required\n");
        raiseExc();
    }

    int64_t n = static_cast<BoxedInt*>(idx)->n;
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        if (self->size == 0)
            fprintf(stderr, "IndexError: pop from empty list\n");
        else
            fprintf(stderr, "IndexError: pop index out of range\n");
        raiseExc();
    }

    Box* rtn = self->elts->elts[n];
    memmove(self->elts->elts + n, self->elts->elts + n + 1, (self->size - n - 1) * sizeof(Box*));
    self->size--;

    return rtn;
}

extern "C" Box* listLen(BoxedList* self) {
    return new BoxedInt(self->size);
}

Box* _listSlice(BoxedList *self, i64 start, i64 stop, i64 step) {
    //printf("%ld %ld %ld\n", start, stop, step);
    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= self->size);
    } else {
        assert(start < self->size);
        assert(-1 <= stop);
    }

    BoxedList *rtn = new BoxedList();

    int cur = start;
    while ((step > 0 && cur < stop) || (step < 0 && cur > stop)) {
        listAppendInternal(rtn, self->elts->elts[cur]);
        cur += step;
    }
    return rtn;
}

extern "C" Box* listGetitemInt(BoxedList* self, BoxedInt* slice) {
    assert(self->cls == list_cls);
    assert(slice->cls == int_cls);
    int64_t n = slice->n;
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        fprintf(stderr, "IndexError: list index out of range\n");
        raiseExc();
    }
    Box* rtn = self->elts->elts[n];
    return rtn;
}

extern "C" Box* listGetitemSlice(BoxedList* self, BoxedSlice* slice) {
    assert(self->cls == list_cls);
    assert(slice->cls == slice_cls);
    i64 start, stop, step;
    parseSlice(slice, self->size, &start, &stop, &step);
    return _listSlice(self, start, stop, step);
}

extern "C" Box* listGetitem(BoxedList* self, Box* slice) {
    assert(self->cls == list_cls);
    if (slice->cls == int_cls) {
        return listGetitemInt(self, static_cast<BoxedInt*>(slice));
    } else if (slice->cls == slice_cls) {
        return listGetitemSlice(self, static_cast<BoxedSlice*>(slice));
    } else {
        fprintf(stderr, "TypeError: list indices must be integers, not %s\n", getTypeName(slice)->c_str());
        raiseExc();
    }
}


extern "C" Box* listSetitemInt(BoxedList* self, BoxedInt* slice, Box* v) {
    assert(self->cls == list_cls);
    assert(slice->cls == int_cls);
    int64_t n = slice->n;
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        fprintf(stderr, "IndexError: list index out of range\n");
        raiseExc();
    }

    self->elts->elts[n] = v;
    return None;
}

extern "C" Box* listSetitemSlice(BoxedList* self, BoxedSlice* slice, Box* v) {
    assert(self->cls == list_cls);
    assert(slice->cls == slice_cls);
    i64 start, stop, step;
    parseSlice(slice, self->size, &start, &stop, &step);
    RELEASE_ASSERT(step == 1, "step sizes must be 1 for now");

    assert(0 <= start && start < self->size);
    ASSERT(0 <= stop && stop <= self->size, "%ld %ld", self->size, stop);
    assert(start <= stop);

    ASSERT(v->cls == list_cls, "unsupported %s", getTypeName(v)->c_str());
    BoxedList *lv = static_cast<BoxedList*>(v);

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
    if (slice->cls == int_cls) {
        return listSetitemInt(self, static_cast<BoxedInt*>(slice), v);
    } else if (slice->cls == slice_cls) {
        return listSetitemSlice(self, static_cast<BoxedSlice*>(slice), v);
    } else {
        fprintf(stderr, "TypeError: list indices must be integers, not %s\n", getTypeName(slice)->c_str());
        raiseExc();
    }
}

extern "C" Box * listDelitem(BoxedList* self, Box* slice) {
    if (slice->cls == int_cls){
        BoxedInt* islice = static_cast<BoxedInt*>(slice);
        int64_t n = islice->n;
        if (n < 0)
            n = self->size + n;
	
        if (n < 0 || n >= self->size) {
            fprintf(stderr, "IndexError: list index out of range\n");
            raiseExc();
        }
        memmove(self->elts->elts + n, self->elts->elts + n + 1, (self->size - n - 1) * sizeof(Box*));
        self->size--;
    } else if(slice->cls == slice_cls){
        BoxedSlice *sslice = static_cast<BoxedSlice*>(slice);
        
        i64 start, stop, step;
        parseSlice(sslice, self->size, &start, &stop, &step);
        RELEASE_ASSERT(step == 1, "step sizes must be 1 for now");
	
        assert(0 <= start && start < self->size);
        ASSERT(0 <= stop && stop <= self->size, "%ld %ld", self->size, stop);
        assert(start <= stop);
	
        int remaining_elts = self->size - stop;
	
        memmove(self->elts->elts + start, self->elts->elts + stop, remaining_elts * sizeof(Box*));
        self->size -= (stop - start);
    }else{
        fprintf(stderr, "TypeError: list indices must be integers, not %s\n", getTypeName(slice)->c_str());
        raiseExc();
    }      
    //TODO maybe we need to realloc the elts here
    return None;
}

extern "C" Box* listInsert(BoxedList* self, Box* idx, Box* v) {
    if (idx->cls != int_cls) {
        fprintf(stderr, "TypeError: an integer is required\n");
        raiseExc();
    }

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
        fprintf(stderr, "TypeError: can't multiply sequence by non-int of type '%s'\n", getTypeName(rhs)->c_str());
        raiseExc();
    }

    int n = static_cast<BoxedInt*>(rhs)->n;
    int s = self->size;

    BoxedList* rtn = new BoxedList();
    rtn->ensure(n*s);
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
    if (_rhs->cls != list_cls) {
        fprintf(stderr, "TypeError: can only concatenate list (not \"%s\") to list\n", getTypeName(_rhs)->c_str());
        raiseExc();
    }

    BoxedList* rhs = static_cast<BoxedList*>(_rhs);

    int s1 = self->size;
    int s2 = rhs->size;
    self->ensure(s1 + s2);

    memcpy(self->elts->elts + s1, rhs->elts->elts, sizeof(rhs->elts->elts[0]) * s2);
    self->size = s1 + s2;
    return self;
}

Box* listAdd(BoxedList* self, Box* _rhs) {
    if (_rhs->cls != list_cls) {
        fprintf(stderr, "TypeError: can only concatenate list (not \"%s\") to list\n", getTypeName(_rhs)->c_str());
        raiseExc();
    }

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

Box* listSort1(BoxedList* self) {
    assert(self->cls == list_cls);

    std::sort<Box**, PyLt>(self->elts->elts, self->elts->elts + self->size, PyLt());

    return None;
}

Box* listContains(BoxedList* self, Box *elt) {
    int size = self->size;
    for (int i = 0; i < size; i++) {
        Box* e = self->elts->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            return True;
    }
    return False;
}



BoxedClass *list_iterator_cls = NULL;
extern "C" void listIteratorGCHandler(GCVisitor *v, void* p) {
    boxGCHandler(v, p);
    BoxedListIterator *it = (BoxedListIterator*)p;
    v->visit(it->l);
}

extern "C" const ObjectFlavor list_iterator_flavor(&listIteratorGCHandler, NULL);

void listiterDtor(BoxedListIterator *self) {
}

extern "C" Box* listNew1(Box* cls) {
    assert(cls == list_cls);
    return new BoxedList();
}

extern "C" Box* listNew2(Box* cls, Box* container) {
    assert(cls == list_cls);

    static std::string _iter("__iter__");
    static std::string _hasnext("__hasnext__");
    static std::string _next("next");

    Box* iter = callattr(container, &_iter, true, 0, NULL, NULL, NULL, NULL);

    Box* rtn = new BoxedList();

    while (true) {
        Box* hasnext = callattr(iter, &_hasnext, true, 0, NULL, NULL, NULL, NULL);
        bool hasnext_bool = nonzero(hasnext);
        if (!hasnext_bool)
            break;

        Box* next = callattr(iter, &_next, true, 0, NULL, NULL, NULL, NULL);
        listAppendInternal(rtn, next);
    }
    return rtn;
}

void list_dtor(BoxedList* self) {
    if (self->capacity)
        rt_free(self->elts);
}

void setupList() {
    list_iterator_cls = new BoxedClass(false, (BoxedClass::Dtor)listiterDtor);

    list_cls->giveAttr("__name__", boxStrConstant("list"));

    list_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)listLen, BOXED_INT, 1, false)));

    CLFunction *getitem = createRTFunction();
    addRTFunction(getitem, (void*)listGetitemInt, NULL, std::vector<ConcreteCompilerType*>{LIST, BOXED_INT}, false);
    addRTFunction(getitem, (void*)listGetitemSlice, NULL, std::vector<ConcreteCompilerType*>{LIST, SLICE}, false);
    addRTFunction(getitem, (void*)listGetitem, NULL, std::vector<ConcreteCompilerType*>{LIST, NULL}, false);
    list_cls->giveAttr("__getitem__", new BoxedFunction(getitem));
	list_cls->giveAttr("__delitem__", new BoxedFunction(boxRTFunction((void*)listDelitem, NULL, 2, false)));

    list_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)listIter, typeFromClass(list_iterator_cls), 1, false)));

    list_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)listRepr, STR, 1, false)));
    list_cls->setattr("__str__", list_cls->peekattr("__repr__"), NULL, NULL);
    list_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)listNonzero, BOXED_BOOL, 1, false)));

    CLFunction *pop = boxRTFunction((void*)listPop1, NULL, 1, false);
    addRTFunction(pop, (void*)listPop2, NULL, 2, false);
    list_cls->giveAttr("pop", new BoxedFunction(pop));

    list_cls->giveAttr("append", new BoxedFunction(boxRTFunction((void*)listAppend, NULL, 2, false)));

    CLFunction *setitem = createRTFunction();
    addRTFunction(setitem, (void*)listSetitemInt, NULL, std::vector<ConcreteCompilerType*>{LIST, BOXED_INT, NULL}, false);
    addRTFunction(setitem, (void*)listSetitemSlice, NULL, std::vector<ConcreteCompilerType*>{LIST, SLICE, NULL}, false);
    addRTFunction(setitem, (void*)listSetitem, NULL, std::vector<ConcreteCompilerType*>{LIST, NULL, NULL}, false);
    list_cls->giveAttr("__setitem__", new BoxedFunction(setitem));

    list_cls->giveAttr("insert", new BoxedFunction(boxRTFunction((void*)listInsert, NULL, 3, false)));
    list_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)listMul, NULL, 2, false)));

    list_cls->giveAttr("__iadd__", new BoxedFunction(boxRTFunction((void*)listIAdd, NULL, 2, false)));
    list_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)listAdd, NULL, 2, false)));

    list_cls->giveAttr("sort", new BoxedFunction(boxRTFunction((void*)listSort1, NULL, 1, false)));
    list_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)listContains, BOXED_BOOL, 2, false)));

    CLFunction *new_ = boxRTFunction((void*)listNew1, NULL, 1, false);
    addRTFunction(new_, (void*)listNew2, NULL, 2, false);
    list_cls->giveAttr("__new__", new BoxedFunction(new_));

    list_cls->freeze();


    gc::registerStaticRootObj(list_iterator_cls);
    list_iterator_cls->giveAttr("__name__", boxStrConstant("listiterator"));

    CLFunction *hasnext = boxRTFunction((void*)listiterHasnextUnboxed, BOOL, 1, false);
    addRTFunction(hasnext, (void*)listiterHasnext, BOXED_BOOL, 1, false);
    list_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    list_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)listiterNext, UNKNOWN, 1, false)));

    list_iterator_cls->freeze();
}

void teardownList() {
    // TODO do clearattrs?
    //decref(list_iterator_cls);
}

}
