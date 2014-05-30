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

#include <cstdio>
#include <cstdlib>

#include "core/common.h"
#include "core/options.h"
#include "core/types.h"
#include "gc/gc_alloc.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

#define USE_CUSTOM_ALLOC

namespace pyston {

//#define DEBUG_GC
#ifdef DEBUG_GC
typedef std::unordered_set<void*> AliveSet;
static AliveSet* getAlive() {
    static AliveSet* alive = new AliveSet();
    return alive;
}
#endif

void* rt_alloc(size_t size) {
#ifdef USE_CUSTOM_ALLOC
    void* ptr = gc::gc_alloc(size);
#else
    void* ptr = malloc(size);
#endif

#ifndef NDEBUG
// nallocs++;
#endif
#ifdef DEBUG_GC
    getAlive()->insert(ptr);
#endif
    return ptr;
}

void* rt_realloc(void* ptr, size_t new_size) {
#ifdef USE_CUSTOM_ALLOC
    void* rtn = gc::gc_realloc(ptr, new_size);
#else
    void* rtn = realloc(ptr, new_size);
#endif

#ifdef DEBUG_GC
    getAlive()->erase(ptr);
    getAlive()->insert(rtn);
#endif
    return rtn;
}

void rt_free(void* ptr) {
#ifndef NDEBUG
// nallocs--;
#endif
#ifdef DEBUG_GC
    getAlive()->erase(ptr);
#endif

#ifdef USE_CUSTOM_ALLOC
    gc::gc_free(ptr);
#else
    free(ptr);
#endif

    // assert(nallocs >= 0);
}

void gc_teardown() {
    /*
    if (nallocs != 0) {
        printf("Error: %d alloc's not freed\n", nallocs);
#ifdef DEBUG_GC
        AliveSet *alive = getAlive();
        assert(nallocs == alive->size());
        for (void* p : alive) {
            printf("%p\n", p);
        }
#endif
        // This will scan through the heap and alert us about things that
        // aren't marked (which should be all alive objects):
        gc::global_heap.freeUnmarked();

        abort();
    }
    */
}
}
