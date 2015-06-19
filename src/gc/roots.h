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

#ifndef PYSTON_GC_ROOTS_H
#define PYSTON_GC_ROOTS_H

#include "core/common.h"
#include "core/threading.h"

namespace pyston {

#define GC_KEEP_ALIVE(t) asm volatile("" : : "X"(t))

template <class T> class StackRoot {
public:
    explicit StackRoot(T* t) : t(t) {}
    StackRoot(const StackRoot& other) : t(other.t) {}
    template <class... Args> StackRoot(Args&&... args) : t(new T(std::forward(args...))) {}
    ~StackRoot() { GC_KEEP_ALIVE(t); }

    T& operator*() const { return *t; }
    T* operator->() const { return t; }

    operator T*() const { return t; }

private:
    T* t;
};


class Box;
class BoxedString;
typedef StackRoot<Box> RootedBox;
typedef StackRoot<BoxedString> RootedBoxedString;

//
// the above types can be used whenever we want to explicitly root a Box subclass within some lexical scope.
//
// {
//     RootedBoxedString sub("hello world");
//     for (auto c : sub->s) {
//       doSomethingThatCouldTriggerACollection();
//     }
//     callWithString(sub); // pass RootedBoxedString to a function taking BoxedString*
//     // sub will be rooted conservatively until here
// }
//
}

#endif
