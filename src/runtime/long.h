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

#ifndef PYSTON_RUNTIME_LONG_H
#define PYSTON_RUNTIME_LONG_H

#include <gmp.h>

#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

void setupLong();

extern BoxedClass* long_cls;
extern const ObjectFlavor long_flavor;

class BoxedLong : public Box {
public:
    mpz_t n;

    BoxedLong(BoxedClass* cls) __attribute__((visibility("default"))) : Box(&long_flavor, cls) {}
};

extern "C" Box* createLong(const std::string* s);
}

#endif
