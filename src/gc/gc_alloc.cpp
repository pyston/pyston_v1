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

extern "C" void* gc_compat_malloc(size_t sz) {
    return gc_alloc(sz, GCKind::CONSERVATIVE);
}

extern "C" void* gc_compat_realloc(void* ptr, size_t sz) {
    if (ptr == NULL)
        return gc_alloc(sz, GCKind::CONSERVATIVE);
    return gc_realloc(ptr, sz);
}

extern "C" void gc_compat_free(void* ptr) {
    gc_free(ptr);
}

int nallocs = 0;
bool recursive = false;

// We may need to hook malloc as well:
#if 0
extern "C" void* malloc(size_t sz) {
    static void *(*libc_malloc)(size_t) = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    nallocs++;
    void* r = libc_malloc(sz);;
    if (!recursive && nallocs > 4000000) {
        recursive = true;
        printf("malloc'd: %p\n", r);
        raise(SIGTRAP);
        recursive = false;
    }
    return r;
}

extern "C" void free(void* p) {
    static void (*libc_free)(void*) = (void (*)(void*))dlsym(RTLD_NEXT, "free");
    if (!recursive && nallocs > 4000000) {
        recursive = true;
        printf("free: %p\n", p);
        raise(SIGTRAP);
        recursive = false;
    }
    nallocs--;
    libc_free(p);
}
#endif

} // namespace gc
} // namespace pyston
