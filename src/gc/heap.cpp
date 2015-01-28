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

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <sys/mman.h>

#include "core/common.h"
#include "core/util.h"
#include "gc/gc_alloc.h"
#include "runtime/types.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
namespace gc {

static unsigned bytesAllocatedSinceCollection;
static __thread unsigned thread_bytesAllocatedSinceCollection;
#define ALLOCBYTES_PER_COLLECTION 10000000

void _collectIfNeeded(size_t bytes) {
    thread_bytesAllocatedSinceCollection += bytes;
    if (unlikely(thread_bytesAllocatedSinceCollection > ALLOCBYTES_PER_COLLECTION / 4)) {
        bytesAllocatedSinceCollection += thread_bytesAllocatedSinceCollection;
        thread_bytesAllocatedSinceCollection = 0;

        if (bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION) {
            // bytesAllocatedSinceCollection = 0;
            // threading::GLPromoteRegion _lock;
            // runCollection();

            threading::GLPromoteRegion _lock;
            if (bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION) {
                runCollection();
                bytesAllocatedSinceCollection = 0;
            }
        }
    }
}


Heap global_heap;

#define PAGE_SIZE 4096
class Arena {
private:
    void* start;
    void* cur;

public:
    constexpr Arena(void* start) : start(start), cur(start) {}

    void* doMmap(size_t size) {
        assert(size % PAGE_SIZE == 0);
        // printf("mmap %ld\n", size);

        void* mrtn = mmap(cur, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert((uintptr_t)mrtn != -1 && "failed to allocate memory from OS");
        ASSERT(mrtn == cur, "%p %p\n", mrtn, cur);
        cur = (uint8_t*)cur + size;
        return mrtn;
    }

    bool contains(void* addr) { return start <= addr && addr < cur; }
};

static Arena small_arena((void*)0x1270000000L);
static Arena large_arena((void*)0x2270000000L);

struct LargeObj {
    LargeObj* next, **prev;
    size_t obj_size;
    GCAllocation data[0];

    int mmap_size() {
        size_t total_size = obj_size + sizeof(LargeObj);
        total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        return total_size;
    }

    int capacity() { return mmap_size() - sizeof(LargeObj); }

    static LargeObj* fromAllocation(GCAllocation* alloc) {
        char* rtn = (char*)alloc - offsetof(LargeObj, data);
        assert((uintptr_t)rtn % PAGE_SIZE == 0);
        return reinterpret_cast<LargeObj*>(rtn);
    }
};

GCAllocation* Heap::allocLarge(size_t size) {
    _collectIfNeeded(size);

    LOCK_REGION(lock);

    size_t total_size = size + sizeof(LargeObj);
    total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    LargeObj* rtn = (LargeObj*)large_arena.doMmap(total_size);
    rtn->obj_size = size;

    rtn->next = large_head;
    if (rtn->next)
        rtn->next->prev = &rtn->next;
    rtn->prev = &large_head;
    large_head = rtn;

    return rtn->data;
}

static Block* alloc_block(uint64_t size, Block** prev) {
    Block* rtn = (Block*)small_arena.doMmap(sizeof(Block));
    assert(rtn);
    rtn->size = size;
    rtn->num_obj = BLOCK_SIZE / size;
    rtn->min_obj_index = (BLOCK_HEADER_SIZE + size - 1) / size;
    rtn->atoms_per_obj = size / ATOM_SIZE;
    rtn->prev = prev;
    rtn->next = NULL;

#ifndef NVALGRIND
// Not sure if this mempool stuff is better than the malloc-like interface:
// VALGRIND_CREATE_MEMPOOL(rtn, 0, true);
#endif

    // printf("Allocated new block %p\n", rtn);

    // Don't think I need to do this:
    rtn->isfree.setAllZero();
    rtn->next_to_check.reset();

    int num_objects = rtn->numObjects();
    int num_lost = rtn->minObjIndex();
    int atoms_per_object = rtn->atomsPerObj();
    for (int i = num_lost * atoms_per_object; i < num_objects * atoms_per_object; i += atoms_per_object) {
        rtn->isfree.set(i);
        // printf("%d %d\n", idx, bit);
    }

    // printf("%d %d %d\n", num_objects, num_lost, atoms_per_object);
    // for (int i =0; i < BITFIELD_ELTS; i++) {
    // printf("%d: %lx\n", i, rtn->isfree[i]);
    //}
    return rtn;
}

static void insertIntoLL(Block** next_pointer, Block* next) {
    assert(next_pointer);
    assert(next);
    assert(!next->next);
    assert(!next->prev);

    next->next = *next_pointer;
    if (next->next)
        next->next->prev = &next->next;
    *next_pointer = next;
    next->prev = next_pointer;
}

static void removeFromLL(Block* b) {
    if (b->next)
        b->next->prev = b->prev;
    *b->prev = b->next;

    b->next = NULL;
    b->prev = NULL;
}

Heap::ThreadBlockCache::~ThreadBlockCache() {
    LOCK_REGION(heap->lock);

    for (int i = 0; i < NUM_BUCKETS; i++) {
        while (Block* b = cache_free_heads[i]) {
            removeFromLL(b);
            insertIntoLL(&heap->heads[i], b);
        }

        while (Block* b = cache_full_heads[i]) {
            removeFromLL(b);
            insertIntoLL(&heap->full_heads[i], b);
        }
    }
}

static GCAllocation* allocFromBlock(Block* b) {
    int idx = b->isfree.scanForNext(b->next_to_check);
    if (idx == -1)
        return NULL;

    void* rtn = &b->atoms[idx];
    return reinterpret_cast<GCAllocation*>(rtn);
}

static Block* claimBlock(size_t rounded_size, Block** free_head) {
    Block* free_block = *free_head;
    if (free_block) {
        removeFromLL(free_block);
        return free_block;
    }

    return alloc_block(rounded_size, NULL);
}

GCAllocation* Heap::allocSmall(size_t rounded_size, int bucket_idx) {
    _collectIfNeeded(rounded_size);

    Block** free_head = &heads[bucket_idx];
    Block** full_head = &full_heads[bucket_idx];

    ThreadBlockCache* cache = thread_caches.get();

    Block** cache_head = &cache->cache_free_heads[bucket_idx];

    // static __thread int gc_allocs = 0;
    // if (++gc_allocs == 128) {
    // static StatCounter sc_total("gc_allocs");
    // sc_total.log(128);
    // gc_allocs = 0;
    //}

    while (true) {
        while (Block* cache_block = *cache_head) {
            GCAllocation* rtn = allocFromBlock(cache_block);
            if (rtn)
                return rtn;

            removeFromLL(cache_block);
            insertIntoLL(&cache->cache_full_heads[bucket_idx], cache_block);
        }

        // Not very useful to count the cache misses if we don't count the total attempts:
        // static StatCounter sc_fallback("gc_allocs_cachemiss");
        // sc_fallback.log();

        LOCK_REGION(lock);

        assert(*cache_head == NULL);

        // should probably be called allocBlock:
        Block* myblock = claimBlock(rounded_size, &heads[bucket_idx]);
        assert(myblock);
        assert(!myblock->next);
        assert(!myblock->prev);

        // printf("%d claimed new block %p with %d objects\n", threading::gettid(), myblock, myblock->numObjects());

        insertIntoLL(cache_head, myblock);
    }
}

void _freeFrom(GCAllocation* alloc, Block* b) {
    assert(b == Block::forPointer(alloc));

    size_t size = b->size;
    int offset = (char*)alloc - (char*)b;
    assert(offset % size == 0);
    int atom_idx = offset / ATOM_SIZE;

    assert(!b->isfree.isSet(atom_idx));
    b->isfree.toggle(atom_idx);

#ifndef NVALGRIND
// VALGRIND_MEMPOOL_FREE(b, ptr);
#endif
}

static void _freeLargeObj(LargeObj* lobj) {
    *lobj->prev = lobj->next;
    if (lobj->next)
        lobj->next->prev = lobj->prev;

    int r = munmap(lobj, lobj->mmap_size());
    assert(r == 0);
}

static void _doFree(GCAllocation* al) {
    if (VERBOSITY() >= 2)
        printf("Freeing %p\n", al->user_data);

    if (al->kind_id == GCKind::PYTHON) {
        Box* b = (Box*)al->user_data;
        ASSERT(b->cls->tp_dealloc == NULL, "%s", getTypeName(b)->c_str());
    }
}

void Heap::free(GCAllocation* al) {
    _doFree(al);

    if (large_arena.contains(al)) {
        LargeObj* lobj = LargeObj::fromAllocation(al);
        _freeLargeObj(lobj);
        return;
    }

    assert(small_arena.contains(al));
    Block* b = Block::forPointer(al);
    _freeFrom(al, b);
}

GCAllocation* Heap::realloc(GCAllocation* al, size_t bytes) {
    if (large_arena.contains(al)) {
        LargeObj* lobj = LargeObj::fromAllocation(al);

        int capacity = lobj->capacity();
        if (capacity >= bytes && capacity < bytes * 2)
            return al;

        GCAllocation* rtn = alloc(bytes);
        memcpy(rtn, al, std::min(bytes, lobj->obj_size));

        _freeLargeObj(lobj);
        return rtn;
    }

    assert(small_arena.contains(al));
    Block* b = Block::forPointer(al);

    size_t size = b->size;

    if (size >= bytes && size < bytes * 2)
        return al;

    GCAllocation* rtn = alloc(bytes);

#ifndef NVALGRIND
    VALGRIND_DISABLE_ERROR_REPORTING;
    memcpy(rtn, al, std::min(bytes, size));
    VALGRIND_ENABLE_ERROR_REPORTING;
#else
    memcpy(rtn, al, std::min(bytes, size));
#endif

    _freeFrom(al, b);
    return rtn;
}

GCAllocation* Heap::getAllocationFromInteriorPointer(void* ptr) {
    if (large_arena.contains(ptr)) {
        LargeObj* cur = large_head;
        while (cur) {
            if (ptr >= cur && ptr < &cur->data[cur->obj_size])
                return &cur->data[0];
            cur = cur->next;
        }
        return NULL;
    }

    if (!small_arena.contains(ptr))
        return NULL;

    Block* b = Block::forPointer(ptr);
    size_t size = b->size;
    int offset = (char*)ptr - (char*)b;
    int obj_idx = offset / size;

    if (obj_idx < b->minObjIndex() || obj_idx >= b->numObjects())
        return NULL;

    int atom_idx = obj_idx * b->atomsPerObj();

    if (b->isfree.isSet(atom_idx))
        return NULL;

    return reinterpret_cast<GCAllocation*>(&b->atoms[atom_idx]);
}

static Block** freeChain(Block** head) {
    while (Block* b = *head) {
        int num_objects = b->numObjects();
        int first_obj = b->minObjIndex();
        int atoms_per_obj = b->atomsPerObj();

        for (int obj_idx = first_obj; obj_idx < num_objects; obj_idx++) {
            int atom_idx = obj_idx * atoms_per_obj;

            if (b->isfree.isSet(atom_idx))
                continue;

            void* p = &b->atoms[atom_idx];
            GCAllocation* al = reinterpret_cast<GCAllocation*>(p);

            if (isMarked(al)) {
                clearMark(al);
            } else {
                _doFree(al);

                // assert(p != (void*)0x127000d960); // the main module
                b->isfree.set(atom_idx);
            }
        }

        head = &b->next;
    }
    return head;
}

void Heap::freeUnmarked() {
    thread_caches.forEachValue([this](ThreadBlockCache* cache) {
        for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
            Block* h = cache->cache_free_heads[bidx];
            // Try to limit the amount of unused memory a thread can hold onto;
            // currently pretty dumb, just limit the number of blocks in the free-list
            // to 50.  (blocks in the full list don't need to be limited, since we're sure
            // that the thread had just actively used those.)
            // Eventually may want to come up with some scrounging system.
            // TODO does this thread locality even help at all?
            for (int i = 0; i < 50; i++) {
                if (h)
                    h = h->next;
                else
                    break;
            }
            if (h) {
                removeFromLL(h);
                insertIntoLL(&heads[bidx], h);
            }

            Block** chain_end = freeChain(&cache->cache_free_heads[bidx]);
            freeChain(&cache->cache_full_heads[bidx]);

            while (Block* b = cache->cache_full_heads[bidx]) {
                removeFromLL(b);
                insertIntoLL(chain_end, b);
            }
        }
    });

    for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
        Block** chain_end = freeChain(&heads[bidx]);
        freeChain(&full_heads[bidx]);

        while (Block* b = full_heads[bidx]) {
            removeFromLL(b);
            insertIntoLL(chain_end, b);
        }
    }

    LargeObj* cur = large_head;
    while (cur) {
        GCAllocation* al = cur->data;
        if (isMarked(al)) {
            clearMark(al);
        } else {
            _doFree(al);

            *cur->prev = cur->next;
            if (cur->next)
                cur->next->prev = cur->prev;

            LargeObj* to_free = cur;
            cur = cur->next;
            _freeLargeObj(to_free);
            continue;
        }

        cur = cur->next;
    }
}

void dumpHeapStatistics() {
    global_heap.dumpHeapStatistics();
}

struct HeapStatistics {
    struct TypeStats {
        int64_t nallocs;
        int64_t nbytes;
        TypeStats() : nallocs(0), nbytes(0) {}

        void print(const char* name) const {
            if (nbytes > (1 << 20))
                printf("%s: %ld allocations for %.1f MB\n", name, nallocs, nbytes * 1.0 / (1 << 20));
            else if (nbytes > (1 << 10))
                printf("%s: %ld allocations for %.1f KB\n", name, nallocs, nbytes * 1.0 / (1 << 10));
            else
                printf("%s: %ld allocations for %ld bytes\n", name, nallocs, nbytes);
        }
    };
    std::unordered_map<BoxedClass*, TypeStats> by_cls;
    TypeStats conservative, untracked;
    TypeStats total;
};

void addStatistic(HeapStatistics* stats, GCAllocation* al, int nbytes) {
    stats->total.nallocs++;
    stats->total.nbytes += nbytes;

    if (al->kind_id == GCKind::PYTHON) {
        Box* b = (Box*)al->user_data;
        auto& t = stats->by_cls[b->cls];

        t.nallocs++;
        t.nbytes += nbytes;
    } else if (al->kind_id == GCKind::CONSERVATIVE) {
        stats->conservative.nallocs++;
        stats->conservative.nbytes += nbytes;
    } else if (al->kind_id == GCKind::UNTRACKED) {
        stats->untracked.nallocs++;
        stats->untracked.nbytes += nbytes;
    } else {
        RELEASE_ASSERT(0, "%d", (int)al->kind_id);
    }
}

// TODO: copy-pasted from freeChain
void getChainStatistics(HeapStatistics* stats, Block** head) {
    while (Block* b = *head) {
        int num_objects = b->numObjects();
        int first_obj = b->minObjIndex();
        int atoms_per_obj = b->atomsPerObj();

        for (int obj_idx = first_obj; obj_idx < num_objects; obj_idx++) {
            int atom_idx = obj_idx * atoms_per_obj;

            if (b->isfree.isSet(atom_idx))
                continue;

            void* p = &b->atoms[atom_idx];
            GCAllocation* al = reinterpret_cast<GCAllocation*>(p);

            addStatistic(stats, al, b->size);
        }

        head = &b->next;
    }
}

// TODO: copy-pasted from freeUnmarked()
void Heap::dumpHeapStatistics() {
    threading::GLPromoteRegion _lock;

    HeapStatistics stats;

    thread_caches.forEachValue([this, &stats](ThreadBlockCache* cache) {
        for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
            Block* h = cache->cache_free_heads[bidx];

            getChainStatistics(&stats, &cache->cache_free_heads[bidx]);
            getChainStatistics(&stats, &cache->cache_full_heads[bidx]);
        }
    });

    for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
        getChainStatistics(&stats, &heads[bidx]);
        getChainStatistics(&stats, &full_heads[bidx]);
    }

    LargeObj* cur = large_head;
    while (cur) {
        GCAllocation* al = cur->data;
        addStatistic(&stats, al, cur->capacity());

        cur = cur->next;
    }

    stats.conservative.print("conservative");
    stats.untracked.print("untracked");
    for (const auto& p : stats.by_cls) {
        p.second.print(getFullNameOfClass(p.first).c_str());
    }
    stats.total.print("Total");
    printf("\n");
}

} // namespace gc
} // namespace pyston
