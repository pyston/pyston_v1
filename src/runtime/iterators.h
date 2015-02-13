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

#ifndef PYSTON_RUNTIME_ITERATORS_H
#define PYSTON_RUNTIME_ITERATORS_H

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

class BoxIteratorGeneric : public BoxIteratorImpl {
private:
    Box* iterator;
    Box* value;

public:
    BoxIteratorGeneric(Box* container);

    void next() override;
    Box* getValue() override { return value; }
    void gcHandler(GCVisitor* v) override;
    bool isSame(const BoxIteratorImpl* rhs) override;

    static BoxIteratorGeneric end() { return BoxIteratorGeneric(nullptr); }
};

class BoxIteratorList : public BoxIteratorImpl {
private:
    BoxedList* list;
    int64_t index;

public:
    BoxIteratorList(Box* list);

    void next() override;
    Box* getValue() override;
    void gcHandler(GCVisitor* v) override;
    bool isSame(const BoxIteratorImpl* rhs) override;

    static BoxIteratorList end() { return BoxIteratorList(nullptr); }
};

class BoxIteratorTuple : public BoxIteratorImpl {
private:
    BoxedTuple* tuple;
    int64_t index;

public:
    BoxIteratorTuple(Box* tuple);

    void next() override;
    Box* getValue() override;
    void gcHandler(GCVisitor* v) override;
    bool isSame(const BoxIteratorImpl* rhs) override;

    static BoxIteratorTuple end() { return BoxIteratorTuple(nullptr); }
};

class BoxIteratorString : public BoxIteratorImpl {
private:
    BoxedString* string;
    int64_t index;

public:
    BoxIteratorString(Box* string);

    void next() override;
    Box* getValue() override;
    void gcHandler(GCVisitor* v) override;
    bool isSame(const BoxIteratorImpl* rhs) override;

    static BoxIteratorString end() { return BoxIteratorString(nullptr); }
};
}

#endif
