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

#ifndef PYSTON_RUNTIME_SET_H
#define PYSTON_RUNTIME_SET_H

#include "core/from_llvm/DenseSet.h"
#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

void setupSet();

extern "C" Box* createSet();

class BoxedSet : public Box {
public:
    typedef pyston::DenseSet<BoxAndHash, BoxAndHash::Comparisons, /* MinSize= */ 8> Set;
    Set s;
    Box** weakreflist; /* List of weak references */

    BoxedSet() __attribute__((visibility("default"))) : weakreflist(NULL) {}

    template <typename T> __attribute__((visibility("default"))) BoxedSet(T&& s) : s(std::forward<T>(s)) {}

    DEFAULT_CLASS_SIMPLE(set_cls, true);

    static void dealloc(Box* b) noexcept;
    static int traverse(Box* self, visitproc visit, void* arg) noexcept;
    static int clear(Box* self) noexcept;
};

void _setAddStolen(BoxedSet* self, STOLEN(Box*) val);
}

#endif
