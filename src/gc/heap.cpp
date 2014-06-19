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

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <sys/mman.h>

#include "core/common.h"
#include "core/util.h"
#include "gc/gc_alloc.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
namespace gc {

static unsigned bytesAllocatedSinceCollection;
static __thread unsigned thread_bytesAllocatedSinceCollection;
#define ALLOCBYTES_PER_COLLECTION 2000000

void _collectIfNeeded(size_t bytes) {
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

    thread_bytesAllocatedSinceCollection += bytes;
    if (thread_bytesAllocatedSinceCollection > ALLOCBYTES_PER_COLLECTION / 4) {
        bytesAllocatedSinceCollection += thread_bytesAllocatedSinceCollection;
        thread_bytesAllocatedSinceCollection = 0;
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

        void* mrtn = mmap(cur, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert((uintptr_t)mrtn != -1 && "failed to allocate memory from OS");
        ASSERT(mrtn == cur, "%p %p\n", mrtn, cur);
        cur = (uint8_t*)cur + size;
        return mrtn;
    }

    bool contains(void* addr) { return start <= addr && addr < cur; }
};

Arena small_arena((void*)0x1270000000L);
Arena large_arena((void*)0x2270000000L);

struct LargeObj {
    LargeObj* next, **prev;
    size_t obj_size;
    char data[0];

    int mmap_size() {
        size_t total_size = obj_size + sizeof(LargeObj);
        total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        return total_size;
    }

    int capacity() { return mmap_size() - sizeof(LargeObj); }

    static LargeObj* fromPointer(void* ptr) {
        char* rtn = (char*)ptr + ((char*)NULL - ((LargeObj*)(NULL))->data);
        assert((uintptr_t)rtn % PAGE_SIZE == 0);
        return reinterpret_cast<LargeObj*>(rtn);
    }
};

void* Heap::allocLarge(size_t size) {
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

    return &rtn->data;
}

static Block* alloc_block(uint64_t size, Block** prev) {
    Block* rtn = (Block*)small_arena.doMmap(sizeof(Block));
    assert(rtn);
    rtn->size = size;
    rtn->prev = prev;
    rtn->next = NULL;

#ifndef NVALGRIND
// Not sure if this mempool stuff is better than the malloc-like interface:
// VALGRIND_CREATE_MEMPOOL(rtn, 0, true);
#endif

    // printf("Allocated new block %p\n", rtn);

    // Don't think I need to do this:
    memset(rtn->isfree, 0, sizeof(Block::isfree));

    int num_objects = rtn->numObjects();
    int num_lost = rtn->minObjIndex();
    int atoms_per_object = rtn->atomsPerObj();
    for (int i = num_lost * atoms_per_object; i < num_objects * atoms_per_object; i += atoms_per_object) {
        int idx = i / 64;
        int bit = i % 64;
        rtn->isfree[idx] ^= (1L << bit);
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

static void* allocFromBlock(Block* b) {
    int i = 0;
    uint64_t mask = 0;
    for (; i < BITFIELD_ELTS; i++) {
        mask = b->isfree[i];
        if (mask != 0L) {
            break;
        }
    }

    if (i == BITFIELD_ELTS) {
        return NULL;
    }

    int first = __builtin_ctzll(mask);
    assert(first < 64);
    assert(b->isfree[i] & (1L << first));
    b->isfree[i] ^= (1L << first);
    // printf("Marking %d:%d: %p=%lx\n", i, first, &b->isfree[i], b->isfree[i]);

    int idx = first + i * 64;

    void* rtn = &b->atoms[idx];
    return rtn;
}

static Block* claimBlock(size_t rounded_size, Block** free_head) {
    Block* free_block = *free_head;
    if (free_block) {
        removeFromLL(free_block);
        return free_block;
    }

    return alloc_block(rounded_size, NULL);
}

void* Heap::allocSmall(size_t rounded_size, int bucket_idx) {
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
            void* rtn = allocFromBlock(cache_block);
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

void _freeFrom(void* ptr, Block* b) {
    assert(b == Block::forPointer(ptr));

    size_t size = b->size;
    int offset = (char*)ptr - (char*)b;
    assert(offset % size == 0);
    int atom_idx = offset / ATOM_SIZE;

    int bitmap_idx = atom_idx / 64;
    int bitmap_bit = atom_idx % 64;
    uint64_t mask = 1L << bitmap_bit;
    assert((b->isfree[bitmap_idx] & mask) == 0);
    b->isfree[bitmap_idx] ^= mask;

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

void Heap::free(void* ptr) {
    if (large_arena.contains(ptr)) {
        LargeObj* lobj = LargeObj::fromPointer(ptr);
        _freeLargeObj(lobj);
        return;
    }

    assert(small_arena.contains(ptr));
    Block* b = Block::forPointer(ptr);
    _freeFrom(ptr, b);
}

void* Heap::realloc(void* ptr, size_t bytes) {
    if (large_arena.contains(ptr)) {
        LargeObj* lobj = LargeObj::fromPointer(ptr);

        int capacity = lobj->capacity();
        if (capacity >= bytes && capacity < bytes * 2)
            return ptr;

        void* rtn = alloc(bytes);
        memcpy(rtn, ptr, std::min(bytes, lobj->obj_size));

        _freeLargeObj(lobj);
        return rtn;
    }

    assert(small_arena.contains(ptr));
    Block* b = Block::forPointer(ptr);

    size_t size = b->size;

    if (size >= bytes && size < bytes * 2)
        return ptr;

    void* rtn = alloc(bytes);

#ifndef NVALGRIND
    VALGRIND_DISABLE_ERROR_REPORTING;
    memcpy(rtn, ptr, std::min(bytes, size));
    VALGRIND_ENABLE_ERROR_REPORTING;
#else
    memcpy(rtn, ptr, std::min(bytes, size));
#endif

    _freeFrom(ptr, b);
    return rtn;
}

void* Heap::getAllocationFromInteriorPointer(void* ptr) {
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

    int atom_idx = obj_idx * (size / ATOM_SIZE);

    int bitmap_idx = atom_idx / 64;
    int bitmap_bit = atom_idx % 64;
    uint64_t mask = 1L << bitmap_bit;
    if (b->isfree[bitmap_idx] & mask)
        return NULL;

    return &b->atoms[atom_idx];
}

static Block** freeChain(Block** head) {
    while (Block* b = *head) {
        int num_objects = b->numObjects();
        int first_obj = b->minObjIndex();
        int atoms_per_obj = b->atomsPerObj();

        for (int obj_idx = first_obj; obj_idx < num_objects; obj_idx++) {
            int atom_idx = obj_idx * atoms_per_obj;
            int bitmap_idx = atom_idx / 64;
            int bitmap_bit = atom_idx % 64;
            uint64_t mask = 1L << bitmap_bit;

            if (b->isfree[bitmap_idx] & mask)
                continue;

            void* p = &b->atoms[atom_idx];
            GCObjectHeader* header = headerFromObject(p);

            if (isMarked(header)) {
                clearMark(header);
            } else {
                // assert(p != (void*)0x127000d960); // the main module
                b->isfree[bitmap_idx] |= mask;
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
        void* p = cur->data;
        GCObjectHeader* header = headerFromObject(p);
        if (isMarked(header)) {
            clearMark(header);
        } else {
            if (VERBOSITY() >= 2)
                printf("Freeing %p\n", p);

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
} // namespace gc
} // namespace pyston
