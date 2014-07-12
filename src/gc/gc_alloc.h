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

#include "gc/collector.h"
#include "gc/heap.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

namespace pyston {
namespace gc {

inline void* gc_alloc(size_t bytes) __attribute__((visibility("default")));
inline void* gc_alloc(size_t bytes) {

#ifndef NVALGRIND
// Adding a redzone will confuse the allocator, so disable it for now.
#define REDZONE_SIZE 0
// This can also be set to "RUNNING_ON_VALGRIND", which will only turn on redzones when
// valgrind is actively running, but I think it's better to just always turn them on.
// They're broken and have 0 size anyway.
#define ENABLE_REDZONES 1
    void* r;
    if (ENABLE_REDZONES) {
        void* base = global_heap.alloc(bytes + REDZONE_SIZE * 2);
        r = ((char*)base) + REDZONE_SIZE;
        // printf("alloc base = %p\n", base);
    } else {
        r = global_heap.alloc(bytes);
    }
    VALGRIND_MALLOCLIKE_BLOCK(r, bytes, REDZONE_SIZE, false);
#else
    void* r = global_heap.alloc(bytes);
#endif

#ifndef NDEBUG
// I think I have a suspicion: the gc will see the constant and treat it as a
// root.  So instead, shift to hide the pointer
// if ((((intptr_t)r) >> 4) == (0x127001424L)) {
// if ((((intptr_t)r) >> 4) == (0x127000718L)) {
// raise(SIGTRAP);
//}

// if (VERBOSITY()) printf("Allocated %ld bytes at [%p, %p)\n", bytes, r, (char*)r + bytes);
#endif
    // printf("Allocated %p\n", r);


    return r;
}

inline void* gc_realloc(void* ptr, size_t bytes) __attribute__((visibility("default")));
inline void* gc_realloc(void* ptr, size_t bytes) {
#ifndef NVALGRIND
    void* rtn;
    if (ENABLE_REDZONES) {
        void* base = (char*)ptr - REDZONE_SIZE;
        void* rtn_base = global_heap.realloc(base, bytes + 2 * REDZONE_SIZE);
        rtn = (char*)rtn_base + REDZONE_SIZE;
    } else {
        rtn = global_heap.realloc(ptr, bytes);
    }

    VALGRIND_FREELIKE_BLOCK(ptr, REDZONE_SIZE);
    VALGRIND_MALLOCLIKE_BLOCK(rtn, bytes, REDZONE_SIZE, true);
    return rtn;
#else
    return global_heap.realloc(ptr, bytes);
#endif
}

inline void gc_free(void* ptr) __attribute__((visibility("default")));
inline void gc_free(void* ptr) {
#ifndef NVALGRIND
    if (ENABLE_REDZONES) {
        void* base = (char*)ptr - REDZONE_SIZE;
        global_heap.free(base);
    } else {
        global_heap.free(ptr);
    }
    VALGRIND_FREELIKE_BLOCK(ptr, REDZONE_SIZE);
#else
    global_heap.free(ptr);
#endif
}
}
}

#endif
