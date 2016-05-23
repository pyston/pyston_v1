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

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {
namespace {

class BoxIteratorGeneric : public BoxIteratorImpl {
private:
    Box* iterator;
    Box* value;
    bool need_to_fetch_value;

public:
    BoxIteratorGeneric(Box* container) : iterator(nullptr), value(nullptr), need_to_fetch_value(false) {
        if (container) {
            // TODO: this should probably call getPystonIter
            iterator = getiter(container);
            if (iterator) {
                need_to_fetch_value = true;
            } else
                *this = *end();
        }
    }

    ~BoxIteratorGeneric() {
        Py_XDECREF(value);
        Py_XDECREF(iterator);
    }

    void next() override {
        assert(!need_to_fetch_value);
        need_to_fetch_value = true;
    }

    Box* getValue() override {
        if (need_to_fetch_value)
            fetchNextValue();
        Box* r = value;
        assert(r);
        value = NULL;
        return r;
    }

    bool isSame(const BoxIteratorImpl* _rhs) override {
        const BoxIteratorGeneric* rhs = (const BoxIteratorGeneric*)_rhs;
        assert(!rhs->need_to_fetch_value); // we can't fetch the value here because rhs is const
        if (need_to_fetch_value)
            fetchNextValue();
        return iterator == rhs->iterator && value == rhs->value;
    }

    int traverse(visitproc visit, void* arg) override {
        Py_VISIT(iterator);
        Py_VISIT(value);

        return 0;
    }

    static BoxIteratorGeneric* end() {
        static BoxIteratorGeneric _end(nullptr);
        return &_end;
    }

private:
    void fetchNextValue() {
        STAT_TIMER(t0, "us_timer_iteratorgeneric_next", 0);
        assert(!value);
        assert(need_to_fetch_value);
        Box* next = PyIter_Next(iterator);
        need_to_fetch_value = false;
        if (next) {
            value = next;
        } else {
            if (PyErr_Occurred())
                throwCAPIException();
            Py_CLEAR(iterator);
            *this = *end();
        }
    }
};


template <typename T> class BoxIteratorIndex : public BoxIteratorImpl {
private:
    T* obj;
    uint64_t index;

    static bool hasnext(BoxedList* o, uint64_t i) { return i < o->size; }
    static Box* getValue(BoxedList* o, uint64_t i) { return incref(o->elts->elts[i]); }

    static bool hasnext(BoxedTuple* o, uint64_t i) { return i < o->size(); }
    static Box* getValue(BoxedTuple* o, uint64_t i) { return incref(o->elts[i]); }

    static bool hasnext(BoxedString* o, uint64_t i) { return i < o->size(); }
    static Box* getValue(BoxedString* o, uint64_t i) { return boxString(llvm::StringRef(o->data() + i, 1)); }

public:
    explicit BoxIteratorIndex(T* obj) : obj(obj), index(0) {
        Py_XINCREF(obj);
        if (obj && !hasnext(obj, index)) {
            Py_CLEAR(obj);
            *this = *end();
        }
    }

    BoxIteratorIndex(const BoxIteratorIndex& rhs) : obj(rhs.obj), index(rhs.index) { Py_XINCREF(obj); }
    BoxIteratorIndex(BoxIteratorIndex&& rhs) : obj(rhs.obj), index(rhs.index) { Py_XINCREF(obj); }
    BoxIteratorIndex& operator=(const BoxIteratorIndex& rhs) {
        obj = rhs.obj;
        index = rhs.index;
        Py_XINCREF(obj);
        return *this;
    }
    BoxIteratorIndex& operator=(BoxIteratorIndex&& rhs) {
        obj = rhs.obj;
        index = rhs.index;
        Py_XINCREF(obj);
        return *this;
    }

    ~BoxIteratorIndex() { Py_CLEAR(obj); }

    void next() override {
        if (!end()->isSame(this)) {
            ++index;
            if (!hasnext(obj, index)) {
                Py_CLEAR(obj);
                *this = *end();
            }
        }
    }

    Box* getValue() override {
        Box* r = getValue(obj, index);
        return r;
    }

    bool isSame(const BoxIteratorImpl* _rhs) override {
        const auto rhs = (const BoxIteratorIndex*)_rhs;
        return obj == rhs->obj && index == rhs->index;
    }

    int traverse(visitproc visit, void* arg) override {
        Py_VISIT(obj);

        return 0;
    }

    static BoxIteratorIndex* end() {
        static BoxIteratorIndex _end(nullptr);
        return &_end;
    }
};
}

BoxIteratorRange Box::pyElements() {
    if (this->cls == list_cls) {
        using BoxIteratorList = BoxIteratorIndex<BoxedList>;
        BoxIteratorImpl* end = BoxIteratorList::end();
        return BoxIteratorRange(end, (BoxedList*)this, (BoxIteratorList*)nullptr);
    } else if (this->cls == tuple_cls) {
        using BoxIteratorTuple = BoxIteratorIndex<BoxedTuple>;
        BoxIteratorImpl* end = BoxIteratorTuple::end();
        return BoxIteratorRange(end, (BoxedTuple*)this, (BoxIteratorTuple*)nullptr);
    } else if (this->cls == str_cls) {
        using BoxIteratorString = BoxIteratorIndex<BoxedString>;
        BoxIteratorImpl* end = BoxIteratorString::end();
        return BoxIteratorRange(end, (BoxedString*)this, (BoxIteratorString*)nullptr);
    } else {
        BoxIteratorImpl* end = BoxIteratorGeneric::end();
        return BoxIteratorRange(end, this, (BoxIteratorGeneric*)nullptr);
    }
}
}
