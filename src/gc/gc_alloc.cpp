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

// We may need to hook malloc as well:
/*
extern "C" void* malloc(size_t sz) {
    abort();
}

extern "C" void free(void* p) {
    abort();
}
*/

} // namespace gc
} // namespace pyston
