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

#ifndef PYSTON_GC_COLLECTOR_H
#define PYSTON_GC_COLLECTOR_H

#include <vector>

#include "core/types.h"

namespace pyston {
namespace gc {

// Mark this gc-allocated object as being a root, even if there are no visible references to it.
// (Note: this marks the gc allocation itself, not the pointer that points to one.  For that, use
// a GCRootHandle)
void registerPermanentRoot(void* root_obj);
// Register an object that was not allocated through this collector, as a root for this collector.
// The motivating usecase is statically-allocated PyTypeObject objects, which are full Python objects
// even if they are not heap allocated.
void registerNonheapRootObject(void* obj);

// If you want to have a static root "location" where multiple values could be stored, use this:
class GCRootHandle {
public:
    Box* value;

    GCRootHandle();
    ~GCRootHandle();

    void operator=(Box* b) { value = b; }

    operator Box*() { return value; }
};

void runCollection();

bool isValidGCObject(void* p);
}
}

#endif
