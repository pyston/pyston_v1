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

public:
    BoxIteratorGeneric(Box* container) : iterator(nullptr), value(nullptr) {
        if (container) {
            // TODO: this should probably call getPystonIter
            iterator = getiter(container);
            if (iterator) {
                // try catch block to manually decref the iterator because if the constructor throwes the destructor
                // won't get called
                // but we should probably just change the code to not call next inside the constructor...
                try {
                    next();
                } catch (ExcInfo e) {
                    Py_CLEAR(iterator);
                    throw e;
                }
            } else
                *this = *end();
        }
    }

    ~BoxIteratorGeneric() {
        Py_XDECREF(value);
        Py_XDECREF(iterator);
    }

    void next() override {
        STAT_TIMER(t0, "us_timer_iteratorgeneric_next", 0);
        assert(!value);

        Box* next = PyIter_Next(iterator);
        if (next) {
            value = next;
        } else {
            checkAndThrowCAPIException();
            Py_CLEAR(iterator);
            *this = *end();
        }
    }

    Box* getValue() override {
        Box* r = value;
        assert(r);
        value = NULL;
        return r;
    }

    bool isSame(const BoxIteratorImpl* _rhs) override {
        const BoxIteratorGeneric* rhs = (const BoxIteratorGeneric*)_rhs;
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
        std::unique_ptr<BoxIteratorImpl> begin(new BoxIteratorList((BoxedList*)this));
        BoxIteratorImpl* end = BoxIteratorList::end();
        return BoxIteratorRange(std::move(begin), end);
    } else if (this->cls == tuple_cls) {
        using BoxIteratorTuple = BoxIteratorIndex<BoxedTuple>;
        std::unique_ptr<BoxIteratorImpl> begin(new BoxIteratorTuple((BoxedTuple*)this));
        BoxIteratorImpl* end = BoxIteratorTuple::end();
        return BoxIteratorRange(std::move(begin), end);
    } else if (this->cls == str_cls) {
        using BoxIteratorString = BoxIteratorIndex<BoxedString>;
        std::unique_ptr<BoxIteratorImpl> begin(new BoxIteratorString((BoxedString*)this));
        BoxIteratorImpl* end = BoxIteratorString::end();
        return BoxIteratorRange(std::move(begin), end);
    } else {
        std::unique_ptr<BoxIteratorImpl> begin(new BoxIteratorGeneric(this));
        BoxIteratorImpl* end = BoxIteratorGeneric::end();
        return BoxIteratorRange(std::move(begin), end);
    }
}
}
