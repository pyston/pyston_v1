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

#include "gc/gc_alloc.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/mman.h>

#include "core/common.h"
#include "core/util.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
namespace gc {

#if STAT_TIMERS
uint64_t* gc_alloc_stattimer_counter = Stats::getStatCounter("us_timer_gc_alloc");
#endif

extern "C" void* gc_compat_malloc_untracked(size_t sz) noexcept {
    return gc_alloc(sz, GCKind::UNTRACKED);
}

extern "C" void* gc_compat_malloc(size_t sz) noexcept {
    return gc_alloc(sz, GCKind::CONSERVATIVE);
}

extern "C" void* gc_compat_realloc(void* ptr, size_t sz) noexcept {
    if (ptr == NULL)
        return gc_alloc(sz, GCKind::CONSERVATIVE);
    return gc_realloc(ptr, sz);
}

extern "C" void gc_compat_free(void* ptr) noexcept {
    if (ptr)
        gc_free(ptr);
}

// We may need to hook malloc as well.  For now, these definitions serve
// as a reference on how to do that, and also can help with debugging malloc
// usage issues.
#if 0
bool recursive = false;

extern "C" void* malloc(size_t sz) {
    static void *(*libc_malloc)(size_t) = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    void* r = libc_malloc(sz);
    if (!recursive) {
        recursive = true;
        printf("\nmalloc %p\n", r);
        recursive = false;
    }
    return r;
}

extern "C" void* relloc(void* p, size_t sz) {
    static void *(*libc_realloc)(void*, size_t) = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    void* r = libc_realloc(p, sz);
    if (!recursive) {
        recursive = true;
        printf("\nrealloc %p %p\n", p, r);
        recursive = false;
    }
    return r;
}

extern "C" void free(void* p) {
    static void (*libc_free)(void*) = (void (*)(void*))dlsym(RTLD_NEXT, "free");
    if (!recursive) {
        recursive = true;
        printf("\nfree %p\n", p);
        if (p == (void*)0x1c4c780)
            raise(SIGTRAP);
        recursive = false;
    }
    libc_free(p);
}
#endif

} // namespace gc
} // namespace pyston
