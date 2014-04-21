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

#ifndef PYSTON_GC_GCALLOC_H
#define PYSTON_GC_GCALLOC_H

#include <cstdlib>

#include "gc/heap.h"
#include "gc/collector.h"

namespace pyston {
namespace gc {

inline void* gc_alloc(size_t bytes) __attribute__((visibility("default")));
inline void* gc_alloc(size_t bytes) {
    //if ((++numAllocs) >= ALLOCS_PER_COLLECTION) {
        //numAllocs = 0;
        //runCollection();
    //}

    void* r = global_heap.alloc(bytes);

#ifndef NDEBUG
    // I think I have a suspicion: the gc will see the constant and treat it as a
    // root.  So instead, shift to hide the pointer
    //if ((((intptr_t)r) >> 4) == (0x127001424L)) {
    //if ((((intptr_t)r) >> 4) == (0x127000718L)) {
        //raise(SIGTRAP);
    //}

    //if (VERBOSITY()) printf("Allocated: %p\n", r);
#endif

    return r;
}

inline void* gc_realloc(void* ptr, size_t bytes) __attribute__((visibility("default")));
inline void* gc_realloc(void* ptr, size_t bytes) {
    return global_heap.realloc(ptr, bytes);
}

inline void gc_free(void* ptr) __attribute__((visibility("default")));
inline void gc_free(void* ptr) {
    global_heap.free(ptr);
}

}
}

#endif
