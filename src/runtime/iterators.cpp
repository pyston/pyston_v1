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

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {
namespace {

static std::string next_str("next");

class BoxIteratorGeneric : public BoxIteratorImpl {
private:
    Box* iterator;
    Box* value;

public:
    BoxIteratorGeneric(Box* container) : iterator(nullptr), value(nullptr) {
        if (container) {
            // TODO: this should probably call getPystonIter
            iterator = getiter(container);
            if (iterator)
                next();
            else
                *this = *end();
        }
    }

    void next() override {
        STAT_TIMER(t0, "us_timer_iteratorgeneric_next", 0);

        Box* next = PyIter_Next(iterator);
        if (next) {
            assert(gc::isValidGCObject(next));
            value = next;
        } else {
            checkAndThrowCAPIException();
            *this = *end();
        }
    }

    Box* getValue() override { return value; }

    bool isSame(const BoxIteratorImpl* _rhs) override {
        const BoxIteratorGeneric* rhs = (const BoxIteratorGeneric*)_rhs;
        return iterator == rhs->iterator && value == rhs->value;
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
    static Box* getValue(BoxedList* o, uint64_t i) { return o->elts->elts[i]; }

    static bool hasnext(BoxedTuple* o, uint64_t i) { return i < o->size(); }
    static Box* getValue(BoxedTuple* o, uint64_t i) { return o->elts[i]; }

    static bool hasnext(BoxedString* o, uint64_t i) { return i < o->size(); }
    static Box* getValue(BoxedString* o, uint64_t i) { return boxString(llvm::StringRef(o->data() + i, 1)); }

public:
    BoxIteratorIndex(T* obj) : obj(obj), index(0) {
        if (obj && !hasnext(obj, index))
            *this = *end();
    }

    void next() override {
        if (!end()->isSame(this)) {
            ++index;
            if (!hasnext(obj, index))
                *this = *end();
        }
    }

    Box* getValue() override {
        Box* r = getValue(obj, index);
        assert(gc::isValidGCObject(r));
        return r;
    }

    bool isSame(const BoxIteratorImpl* _rhs) override {
        const auto rhs = (const BoxIteratorIndex*)_rhs;
        return obj == rhs->obj && index == rhs->index;
    }

    static BoxIteratorIndex* end() {
        static BoxIteratorIndex _end(nullptr);
        return &_end;
    }
};
}

llvm::iterator_range<BoxIterator> BoxIterator::getRange(Box* container) {
    if (container->cls == list_cls) {
        using BoxIteratorList = BoxIteratorIndex<BoxedList>;
        BoxIterator begin = new BoxIteratorList((BoxedList*)container);
        BoxIterator end = BoxIteratorList::end();
        return llvm::iterator_range<BoxIterator>(std::move(begin), end);
    } else if (container->cls == tuple_cls) {
        using BoxIteratorTuple = BoxIteratorIndex<BoxedTuple>;
        BoxIterator begin = new BoxIteratorTuple((BoxedTuple*)container);
        BoxIterator end = BoxIteratorTuple::end();
        return llvm::iterator_range<BoxIterator>(std::move(begin), end);
    } else if (container->cls == str_cls) {
        using BoxIteratorString = BoxIteratorIndex<BoxedString>;
        BoxIterator begin = new BoxIteratorString((BoxedString*)container);
        BoxIterator end = BoxIteratorString::end();
        return llvm::iterator_range<BoxIterator>(std::move(begin), end);
    }
    BoxIterator begin = new BoxIteratorGeneric(container);
    BoxIterator end = BoxIteratorGeneric::end();
    return llvm::iterator_range<BoxIterator>(std::move(begin), end);
}
}
