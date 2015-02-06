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

void _doFree(GCAllocation* al);

// these template functions are for both large and huge sections
template <class ListT> inline void unlinkNode(ListT* node) {
    *node->prev = node->next;
    if (node->next)
        node->next->prev = node->prev;
}

template <class ListT, typename Free>
inline void sweepHeap(ListT* head, std::function<void(GCAllocation*)> __free, Free free_func) {
    auto cur = head;
    while (cur) {
        GCAllocation* al = cur->data;
        if (isMarked(al)) {
            clearMark(al);
            cur = cur->next;
        } else {
            __free(al);

            unlinkNode(cur);

            auto to_free = cur;
            cur = cur->next;
            free_func(to_free);
        }
    }
}

static unsigned bytesAllocatedSinceCollection;
static __thread unsigned thread_bytesAllocatedSinceCollection;
#define ALLOCBYTES_PER_COLLECTION 10000000

void registerGCManagedBytes(size_t bytes) {
    thread_bytesAllocatedSinceCollection += bytes;
    if (unlikely(thread_bytesAllocatedSinceCollection > ALLOCBYTES_PER_COLLECTION / 4)) {
        bytesAllocatedSinceCollection += thread_bytesAllocatedSinceCollection;
        thread_bytesAllocatedSinceCollection = 0;

        if (bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION) {
            if (!gcIsEnabled())
                return;

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

GCAllocation* SmallArena::realloc(GCAllocation* al, size_t bytes) {
    Block* b = Block::forPointer(al);

    size_t size = b->size;

    if (size >= bytes && size < bytes * 2)
        return al;

    GCAllocation* rtn = heap->alloc(bytes);

#ifndef NVALGRIND
    VALGRIND_DISABLE_ERROR_REPORTING;
    memcpy(rtn, al, std::min(bytes, size));
    VALGRIND_ENABLE_ERROR_REPORTING;
#else
    memcpy(rtn, al, std::min(bytes, size));
#endif

    _free(al, b);
    return rtn;
}

GCAllocation* SmallArena::allocationFrom(void* ptr) {
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

SmallArena::Block** SmallArena::freeChain(Block** head) {
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


void SmallArena::freeUnmarked() {
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
}


#define LARGE_BLOCK_NUM_CHUNKS ((BLOCK_SIZE >> CHUNK_BITS) - 1)

#define LARGE_BLOCK_FOR_OBJ(obj) ((LargeBlock*)((int64_t)(obj) & ~(int64_t)(BLOCK_SIZE - 1)))
#define LARGE_CHUNK_INDEX(obj, section) (((char*)(obj) - (char*)(section)) >> CHUNK_BITS)

int64_t los_memory_usage = 0;

static int64_t large_object_count = 0;
static int large_block_count = 0;

void LargeArena::add_free_chunk(LargeFreeChunk* free_chunks, size_t size) {
    size_t num_chunks = size >> CHUNK_BITS;

    free_chunks->size = size;

    if (num_chunks >= NUM_FREE_LISTS)
        num_chunks = 0;
    free_chunks->next_size = free_lists[num_chunks];
    free_lists[num_chunks] = free_chunks;
}

LargeArena::LargeFreeChunk* LargeArena::get_from_size_list(LargeFreeChunk** list, size_t size) {
    LargeFreeChunk* free_chunks = NULL;
    LargeBlock* section;
    size_t i, num_chunks, start_index;

    assert((size & (CHUNK_SIZE - 1)) == 0);

    while (*list) {
        free_chunks = *list;
        if (free_chunks->size >= size)
            break;
        list = &(*list)->next_size;
    }

    if (!*list)
        return NULL;

    *list = free_chunks->next_size;

    if (free_chunks->size > size)
        add_free_chunk((LargeFreeChunk*)((char*)free_chunks + size), free_chunks->size - size);

    num_chunks = size >> CHUNK_BITS;

    section = LARGE_BLOCK_FOR_OBJ(free_chunks);

    start_index = LARGE_CHUNK_INDEX(free_chunks, section);
    for (i = start_index; i < start_index + num_chunks; ++i) {
        assert(section->free_chunk_map[i]);
        section->free_chunk_map[i] = 0;
    }

    section->num_free_chunks -= size >> CHUNK_BITS;
    assert(section->num_free_chunks >= 0);

    return free_chunks;
}

LargeArena::LargeObj* LargeArena::_allocInternal(size_t size) {
    LargeBlock* section;
    LargeFreeChunk* free_chunks;
    size_t num_chunks;

    size += CHUNK_SIZE - 1;
    size &= ~(CHUNK_SIZE - 1);

    num_chunks = size >> CHUNK_BITS;

    assert(size > 0 && size - sizeof(LargeObj) <= ALLOC_SIZE_LIMIT);
    assert(num_chunks > 0);

retry:
    if (num_chunks >= NUM_FREE_LISTS) {
        free_chunks = get_from_size_list(&free_lists[0], size);
    } else {
        size_t i;
        for (i = num_chunks; i < NUM_FREE_LISTS; ++i) {
            free_chunks = get_from_size_list(&free_lists[i], size);
            if (free_chunks)
                break;
        }
        if (!free_chunks)
            free_chunks = get_from_size_list(&free_lists[0], size);
    }

    if (free_chunks)
        return (LargeObj*)free_chunks;

    section = (LargeBlock*)doMmap(BLOCK_SIZE);

    if (!section)
        return NULL;

    free_chunks = (LargeFreeChunk*)((char*)section + CHUNK_SIZE);
    free_chunks->size = BLOCK_SIZE - CHUNK_SIZE;
    free_chunks->next_size = free_lists[0];
    free_lists[0] = free_chunks;

    section->num_free_chunks = LARGE_BLOCK_NUM_CHUNKS;

    section->free_chunk_map = (unsigned char*)section + sizeof(LargeBlock);
    assert(sizeof(LargeBlock) + LARGE_BLOCK_NUM_CHUNKS + 1 <= CHUNK_SIZE);
    section->free_chunk_map[0] = 0;
    memset(section->free_chunk_map + 1, 1, LARGE_BLOCK_NUM_CHUNKS);

    section->next = blocks;
    blocks = section;

    ++large_block_count;

    goto retry;
}

void LargeArena::_freeInternal(LargeObj* obj, size_t size) {
    LargeBlock* section = LARGE_BLOCK_FOR_OBJ(obj);
    size_t num_chunks, i, start_index;

    size += CHUNK_SIZE - 1;
    size &= ~(CHUNK_SIZE - 1);

    num_chunks = size >> CHUNK_BITS;

    assert(size > 0 && size - sizeof(LargeObj) <= ALLOC_SIZE_LIMIT);
    assert(num_chunks > 0);

    section->num_free_chunks += num_chunks;
    assert(section->num_free_chunks <= LARGE_BLOCK_NUM_CHUNKS);

    /*
     * We could free the LOS section here if it's empty, but we
     * can't unless we also remove its free chunks from the fast
     * free lists.  Instead, we do it in los_sweep().
     */

    start_index = LARGE_CHUNK_INDEX(obj, section);
    for (i = start_index; i < start_index + num_chunks; ++i) {
        assert(!section->free_chunk_map[i]);
        section->free_chunk_map[i] = 1;
    }

    add_free_chunk((LargeFreeChunk*)obj, size);
}

void LargeArena::_free(LargeObj* obj) {
    unlinkNode(obj);
    _freeInternal(obj, obj->size);
}

void LargeArena::freeUnmarked() {
    sweepHeap(head, _doFree, [this](LargeObj* ptr) { _freeInternal(ptr, ptr->size); });
}

GCAllocation* LargeArena::alloc(size_t size) {
    registerGCManagedBytes(size);

    LOCK_REGION(heap->lock);

    // printf ("allocLarge %zu\n", size);

    LargeObj* obj = _allocInternal(size + sizeof(GCAllocation) + sizeof(LargeObj));

    obj->size = size;

    obj->next = head;
    if (obj->next)
        obj->next->prev = &obj->next;
    obj->prev = &head;
    head = obj;
    large_object_count++;

    return obj->data;
}

GCAllocation* LargeArena::realloc(GCAllocation* al, size_t bytes) {
    LargeObj* obj = (LargeObj*)((char*)al - offsetof(LargeObj, data));
    int size = obj->size;
    if (size >= bytes && size < bytes * 2)
        return al;

    GCAllocation* rtn = heap->alloc(bytes);
    memcpy(rtn, al, std::min(bytes, obj->size));

    _free(obj);
    return rtn;
}

void LargeArena::free(GCAllocation* al) {
    LargeObj* obj = (LargeObj*)((char*)al - offsetof(LargeObj, data));
    _free(obj);
}

GCAllocation* LargeArena::allocationFrom(void* ptr) {
    LargeObj* obj = NULL;

    for (obj = head; obj; obj = obj->next) {
        char* end = (char*)&obj->data + obj->size;

        if (ptr >= obj->data && ptr < end) {
            return &obj->data[0];
        }
    }
    return NULL;
}

void HugeArena::freeUnmarked() {
    sweepHeap(head, _doFree, [this](HugeObj* ptr) { _freeHugeObj(ptr); });
}

GCAllocation* HugeArena::alloc(size_t size) {
    registerGCManagedBytes(size);

    LOCK_REGION(heap->lock);

    size_t total_size = size + sizeof(HugeObj);
    total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    HugeObj* rtn = (HugeObj*)doMmap(total_size);
    rtn->obj_size = size;

    rtn->next = head;
    if (rtn->next)
        rtn->next->prev = &rtn->next;
    rtn->prev = &head;
    head = rtn;
    return rtn->data;
}

GCAllocation* HugeArena::realloc(GCAllocation* al, size_t bytes) {
    HugeObj* lobj = HugeObj::fromAllocation(al);

    int capacity = lobj->capacity();
    if (capacity >= bytes && capacity < bytes * 2)
        return al;

    GCAllocation* rtn = heap->alloc(bytes);
    memcpy(rtn, al, std::min(bytes, lobj->obj_size));

    _freeHugeObj(lobj);
    return rtn;
}

void HugeArena::_freeHugeObj(HugeObj* lobj) {
    unlinkNode(lobj);
    int r = munmap(lobj, lobj->mmap_size());
    assert(r == 0);
}


void HugeArena::free(GCAllocation* al) {
    HugeObj* lobj = HugeObj::fromAllocation(al);
    _freeHugeObj(lobj);
}

GCAllocation* HugeArena::allocationFrom(void* ptr) {
    HugeObj* cur = head;
    while (cur) {
        if (ptr >= cur && ptr < &cur->data[cur->obj_size])
            return &cur->data[0];
        cur = cur->next;
    }
    return NULL;
}

SmallArena::Block* SmallArena::alloc_block(uint64_t size, Block** prev) {
    Block* rtn = (Block*)doMmap(sizeof(Block));
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

void SmallArena::insertIntoLL(Block** next_pointer, Block* next) {
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

void SmallArena::removeFromLL(Block* b) {
    unlinkNode(b);
    b->next = NULL;
    b->prev = NULL;
}

SmallArena::ThreadBlockCache::~ThreadBlockCache() {
    LOCK_REGION(heap->lock);

    for (int i = 0; i < NUM_BUCKETS; i++) {
        while (Block* b = cache_free_heads[i]) {
            small->removeFromLL(b);
            small->insertIntoLL(&small->heads[i], b);
        }

        while (Block* b = cache_full_heads[i]) {
            small->removeFromLL(b);
            small->insertIntoLL(&small->full_heads[i], b);
        }
    }
}

GCAllocation* SmallArena::allocFromBlock(Block* b) {
    int idx = b->isfree.scanForNext(b->next_to_check);
    if (idx == -1)
        return NULL;

    void* rtn = &b->atoms[idx];
    return reinterpret_cast<GCAllocation*>(rtn);
}

SmallArena::Block* SmallArena::claimBlock(size_t rounded_size, Block** free_head) {
    Block* free_block = *free_head;
    if (free_block) {
        removeFromLL(free_block);
        return free_block;
    }

    return alloc_block(rounded_size, NULL);
}

GCAllocation* SmallArena::_alloc(size_t rounded_size, int bucket_idx) {
    registerGCManagedBytes(rounded_size);

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

        LOCK_REGION(heap->lock);

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

void SmallArena::_free(GCAllocation* alloc, Block* b) {
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

void _doFree(GCAllocation* al) {
    if (VERBOSITY() >= 2)
        printf("Freeing %p\n", al->user_data);

    if (al->kind_id == GCKind::PYTHON) {
        Box* b = (Box*)al->user_data;

        ASSERT(b->cls->tp_dealloc == NULL, "%s", getTypeName(b));
        if (b->cls->simple_destructor)
            b->cls->simple_destructor(b);
    }
}

void Heap::destroyContents(GCAllocation* al) {
    _doFree(al);
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
void SmallArena::getChainStatistics(HeapStatistics* stats, Block** head) {
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
void SmallArena::getStatistics(HeapStatistics* stats) {
    thread_caches.forEachValue([this, stats](ThreadBlockCache* cache) {
        for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
            Block* h = cache->cache_free_heads[bidx];

            getChainStatistics(stats, &cache->cache_free_heads[bidx]);
            getChainStatistics(stats, &cache->cache_full_heads[bidx]);
        }
    });

    for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
        getChainStatistics(stats, &heads[bidx]);
        getChainStatistics(stats, &full_heads[bidx]);
    }
}

void LargeArena::getStatistics(HeapStatistics* stats) {
    LargeObj* cur = head;
    while (cur) {
        GCAllocation* al = cur->data;
        addStatistic(stats, al, cur->size);

        cur = cur->next;
    }
}

void HugeArena::getStatistics(HeapStatistics* stats) {
    HugeObj* cur = head;
    while (cur) {
        GCAllocation* al = cur->data;
        addStatistic(stats, al, cur->capacity());

        cur = cur->next;
    }
}

void Heap::dumpHeapStatistics() {
    threading::GLPromoteRegion _lock;

    HeapStatistics stats;

    small_arena.getStatistics(&stats);
    large_arena.getStatistics(&stats);
    huge_arena.getStatistics(&stats);

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
