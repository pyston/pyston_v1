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

#include "runtime/iterators.h"

#include "runtime/objmodel.h"

namespace pyston {

llvm::iterator_range<BoxIterator> BoxIterator::getRange(Box* container) {
    if (container->cls == list_cls) {
        BoxIterator begin(std::make_shared<BoxIteratorList>(container));
        static BoxIterator end(std::make_shared<BoxIteratorList>(BoxIteratorList::end()));
        return llvm::iterator_range<BoxIterator>(std::move(begin), end);
    } else if (container->cls == tuple_cls) {
        BoxIterator begin(std::make_shared<BoxIteratorTuple>(container));
        static BoxIterator end(std::make_shared<BoxIteratorTuple>(BoxIteratorTuple::end()));
        return llvm::iterator_range<BoxIterator>(std::move(begin), end);
    } else if (container->cls == str_cls) {
        BoxIterator begin(std::make_shared<BoxIteratorString>(container));
        static BoxIterator end(std::make_shared<BoxIteratorString>(BoxIteratorString::end()));
        return llvm::iterator_range<BoxIterator>(std::move(begin), end);
    }

    BoxIterator begin(std::make_shared<BoxIteratorGeneric>(container));
    static BoxIterator end(std::make_shared<BoxIteratorGeneric>(BoxIteratorGeneric::end()));
    return llvm::iterator_range<BoxIterator>(std::move(begin), end);
}

BoxIteratorGeneric::BoxIteratorGeneric(Box* container) : BoxIteratorImpl(container), iterator(nullptr), value(nullptr) {
    if (container) {
        // TODO: this should probably call getPystonIter
        iterator = getiter(container);
        if (iterator)
            next();
        else
            *this = end();
    }
}

static std::string next_str("next");
void BoxIteratorGeneric::next() {
    assert(iterator);
    Box* hasnext = iterator->hasnextOrNullIC();
    if (hasnext) {
        if (hasnext->nonzeroIC()) {
            value = iterator->nextIC();
        } else {
            *this = end();
        }
    } else {
        try {
            value = iterator->nextIC();
        } catch (ExcInfo e) {
            if (e.matches(StopIteration))
                *this = end();
            else
                throw e;
        }
    }
}

bool BoxIteratorGeneric::isSame(const BoxIteratorImpl* _rhs) {
    BoxIteratorGeneric* rhs = (BoxIteratorGeneric*)_rhs;
    return iterator == rhs->iterator && value == rhs->value;
}

void BoxIteratorGeneric::gcHandler(GCVisitor* v) {
    v->visitPotential(iterator);
    v->visitPotential(value);
}

BoxIteratorList::BoxIteratorList(Box* container) : BoxIteratorImpl(container), list((BoxedList*)container), index(0) {
    assert(!container || container->cls == list_cls);
    if (list && index >= list->size)
        *this = end();
}

void BoxIteratorList::next() {
    if (!end().isSame(this)) {
        ++index;
        if (index >= list->size)
            *this = end();
    }
}

Box* BoxIteratorList::getValue() {
    return list->elts->elts[index];
}

void BoxIteratorList::gcHandler(GCVisitor* v) {
    v->visitPotential(list);
}

bool BoxIteratorList::isSame(const BoxIteratorImpl* _rhs) {
    BoxIteratorList* rhs = (BoxIteratorList*)_rhs;
    return list == rhs->list && index == rhs->index;
}

BoxIteratorTuple::BoxIteratorTuple(Box* container)
    : BoxIteratorImpl(container), tuple((BoxedTuple*)container), index(0) {
    assert(!container || container->cls == tuple_cls);
    if (tuple && index >= tuple->elts.size())
        *this = end();
}

void BoxIteratorTuple::next() {
    if (!end().isSame(this)) {
        ++index;
        if (index >= tuple->elts.size())
            *this = end();
    }
}

Box* BoxIteratorTuple::getValue() {
    return tuple->elts[index];
}

void BoxIteratorTuple::gcHandler(GCVisitor* v) {
    v->visitPotential(tuple);
}

bool BoxIteratorTuple::isSame(const BoxIteratorImpl* _rhs) {
    BoxIteratorTuple* rhs = (BoxIteratorTuple*)_rhs;
    return tuple == rhs->tuple && index == rhs->index;
}

BoxIteratorString::BoxIteratorString(Box* container)
    : BoxIteratorImpl(container), string((BoxedString*)container), index(0) {
    assert(!container || container->cls == str_cls);
    if (string && index >= string->s.size())
        *this = end();
}

void BoxIteratorString::next() {
    if (!end().isSame(this)) {
        ++index;
        if (index >= string->s.size())
            *this = end();
    }
}

Box* BoxIteratorString::getValue() {
    return new BoxedString(std::string(1, string->s[index]));
}

void BoxIteratorString::gcHandler(GCVisitor* v) {
    v->visitPotential(string);
}

bool BoxIteratorString::isSame(const BoxIteratorImpl* _rhs) {
    BoxIteratorString* rhs = (BoxIteratorString*)_rhs;
    return string == rhs->string && index == rhs->index;
}
}
