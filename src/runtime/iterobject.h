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

#ifndef PYSTON_RUNTIME_ITEROBJECT_H
#define PYSTON_RUNTIME_ITEROBJECT_H

#include <climits>

#include "core/common.h"
#include "runtime/types.h"

namespace pyston {

extern BoxedClass* seqiter_cls;
extern BoxedClass* seqreviter_cls;

// Analogue of CPython's PySeqIter: wraps an object that has a __getitem__
// and uses that to iterate.
class BoxedSeqIter : public Box {
public:
    Box* b;
    int64_t idx;
    Box* next;

    BoxedSeqIter(Box* b, int64_t start) : b(b), idx(start), next(NULL) {}

    DEFAULT_CLASS(seqiter_cls);
};

extern BoxedClass* iterwrapper_cls;
// Pyston wrapper that wraps CPython-style iterators (next() which throws StopException)
// and converts it to Pyston-style (__hasnext__)
class BoxedIterWrapper : public Box {
public:
    Box* iter;
    Box* next;

    BoxedIterWrapper(Box* iter) : iter(iter), next(NULL) {}

    DEFAULT_CLASS(iterwrapper_cls);
};

bool calliter_hasnext(Box* b);

void setupIter();
}

#endif
