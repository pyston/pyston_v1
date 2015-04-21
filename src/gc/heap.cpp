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

namespace std {
template <> std::pair<pyston::Box**, std::ptrdiff_t> get_temporary_buffer<pyston::Box*>(std::ptrdiff_t count) noexcept {
    void* r = pyston::gc::gc_alloc(sizeof(pyston::Box*) * count, pyston::gc::GCKind::CONSERVATIVE);
    return std::make_pair((pyston::Box**)r, count);
}
template <> void return_temporary_buffer<pyston::Box*>(pyston::Box** p) {
    pyston::gc::gc_free(p);
}
}

namespace pyston {
namespace gc {

bool _doFree(GCAllocation* al, std::list<Box*, StlCompatAllocator<Box*>>* weakly_referenced);

// lots of linked lists around here, so let's just use template functions for operations on them.
template <class ListT> inline void nullNextPrev(ListT* node) {
    node->next = NULL;
    node->prev = NULL;
}

template <class ListT> inline void removeFromLL(ListT* node) {
    *node->prev = node->next;
    if (node->next)
        node->next->prev = node->prev;
}

template <class ListT> inline void removeFromLLAndNull(ListT* node) {
    *node->prev = node->next;
    if (node->next)
        node->next->prev = node->prev;
    nullNextPrev(node);
}

template <class ListT> inline void insertIntoLL(ListT** next_pointer, ListT* next) {
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

template <class ListT, typename Func> inline void forEach(ListT* list, Func func) {
    auto cur = list;
    while (cur) {
        func(cur);
        cur = cur->next;
    }
}

template <class ListT, typename Free>
inline void sweepList(ListT* head, std::list<Box*, StlCompatAllocator<Box*>>& weakly_referenced, Free free_func) {
    auto cur = head;
    while (cur) {
        GCAllocation* al = cur->data;
        if (isMarked(al)) {
            clearMark(al);
            cur = cur->next;
        } else {
            if (_doFree(al, &weakly_referenced)) {

                removeFromLL(cur);

                auto to_free = cur;
                cur = cur->next;
                free_func(to_free);
            }
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

bool _doFree(GCAllocation* al, std::list<Box*, StlCompatAllocator<Box*>>* weakly_referenced) {
    if (VERBOSITY() >= 4)
        printf("Freeing %p\n", al->user_data);

#ifndef NVALGRIND
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif
    GCKind alloc_kind = al->kind_id;
#ifndef NVALGRIND
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif

    if (alloc_kind == GCKind::PYTHON) {
#ifndef NVALGRIND
        VALGRIND_DISABLE_ERROR_REPORTING;
#endif
        Box* b = (Box*)al->user_data;
#ifndef NVALGRIND
        VALGRIND_ENABLE_ERROR_REPORTING;
#endif

        if (PyType_SUPPORTS_WEAKREFS(b->cls)) {
            PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(b);
            if (list && *list) {
                assert(weakly_referenced && "attempting to free a weakly referenced object manually");
                weakly_referenced->push_back(b);
                return false;
            }
        }

        ASSERT(b->cls->tp_dealloc == NULL, "%s", getTypeName(b));
        if (b->cls->simple_destructor)
            b->cls->simple_destructor(b);
    }
    return true;
}

void Heap::destructContents(GCAllocation* al) {
    _doFree(al, NULL);
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
    TypeStats conservative, untracked, hcls;
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
    } else if (al->kind_id == GCKind::HIDDEN_CLASS) {
        stats->hcls.nallocs++;
        stats->hcls.nbytes += nbytes;
    } else {
        RELEASE_ASSERT(0, "%d", (int)al->kind_id);
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
    stats.hcls.print("hcls");
    for (const auto& p : stats.by_cls) {
        p.second.print(getFullNameOfClass(p.first).c_str());
    }
    stats.total.print("Total");
    printf("\n");
}

void dumpHeapStatistics() {
    global_heap.dumpHeapStatistics();
}

//////
/// Small Arena

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

    free(al);
    return rtn;
}

void SmallArena::free(GCAllocation* alloc) {
    Block* b = Block::forPointer(alloc);
    size_t size = b->size;
    int offset = (char*)alloc - (char*)b;
    assert(offset % size == 0);
    int atom_idx = offset / ATOM_SIZE;

    assert(!b->isfree.isSet(atom_idx));
    b->isfree.set(atom_idx);

#ifndef NVALGRIND
// VALGRIND_MEMPOOL_FREE(b, ptr);
#endif
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

void SmallArena::freeUnmarked(std::list<Box*, StlCompatAllocator<Box*>>& weakly_referenced) {
    thread_caches.forEachValue([this, &weakly_referenced](ThreadBlockCache* cache) {
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
                removeFromLLAndNull(h);
                insertIntoLL(&heads[bidx], h);
            }

            Block** chain_end = _freeChain(&cache->cache_free_heads[bidx], weakly_referenced);
            _freeChain(&cache->cache_full_heads[bidx], weakly_referenced);

            while (Block* b = cache->cache_full_heads[bidx]) {
                removeFromLLAndNull(b);
                insertIntoLL(chain_end, b);
            }
        }
    });

    for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
        Block** chain_end = _freeChain(&heads[bidx], weakly_referenced);
        _freeChain(&full_heads[bidx], weakly_referenced);

        while (Block* b = full_heads[bidx]) {
            removeFromLLAndNull(b);
            insertIntoLL(chain_end, b);
        }
    }
}

// TODO: copy-pasted from freeUnmarked()
void SmallArena::getStatistics(HeapStatistics* stats) {
    thread_caches.forEachValue([this, stats](ThreadBlockCache* cache) {
        for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
            Block* h = cache->cache_free_heads[bidx];

            _getChainStatistics(stats, &cache->cache_free_heads[bidx]);
            _getChainStatistics(stats, &cache->cache_full_heads[bidx]);
        }
    });

    for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
        _getChainStatistics(stats, &heads[bidx]);
        _getChainStatistics(stats, &full_heads[bidx]);
    }
}


SmallArena::Block** SmallArena::_freeChain(Block** head, std::list<Box*, StlCompatAllocator<Box*>>& weakly_referenced) {
    while (Block* b = *head) {
        int num_objects = b->numObjects();
        int first_obj = b->minObjIndex();
        int atoms_per_obj = b->atomsPerObj();

        for (int atom_idx = first_obj * atoms_per_obj; atom_idx < num_objects * atoms_per_obj;
             atom_idx += atoms_per_obj) {

            if (b->isfree.isSet(atom_idx))
                continue;

            void* p = &b->atoms[atom_idx];
            GCAllocation* al = reinterpret_cast<GCAllocation*>(p);

            if (isMarked(al)) {
                clearMark(al);
            } else {
                if (_doFree(al, &weakly_referenced))
                    b->isfree.set(atom_idx);
            }
        }

        head = &b->next;
    }
    return head;
}


SmallArena::Block* SmallArena::_allocBlock(uint64_t size, Block** prev) {
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

SmallArena::ThreadBlockCache::~ThreadBlockCache() {
    LOCK_REGION(heap->lock);

    for (int i = 0; i < NUM_BUCKETS; i++) {
        while (Block* b = cache_free_heads[i]) {
            removeFromLLAndNull(b);
            insertIntoLL(&small->heads[i], b);
        }

        while (Block* b = cache_full_heads[i]) {
            removeFromLLAndNull(b);
            insertIntoLL(&small->full_heads[i], b);
        }
    }
}

GCAllocation* SmallArena::_allocFromBlock(Block* b) {
    int idx = b->isfree.scanForNext(b->next_to_check);
    if (idx == -1)
        return NULL;

    void* rtn = &b->atoms[idx];
    return reinterpret_cast<GCAllocation*>(rtn);
}

SmallArena::Block* SmallArena::_claimBlock(size_t rounded_size, Block** free_head) {
    Block* free_block = *free_head;
    if (free_block) {
        removeFromLLAndNull(free_block);
        return free_block;
    }

    return _allocBlock(rounded_size, NULL);
}

GCAllocation* SmallArena::_alloc(size_t rounded_size, int bucket_idx) {
    Block** free_head = &heads[bucket_idx];
    Block** full_head = &full_heads[bucket_idx];

    static __thread ThreadBlockCache* cache = NULL;
    if (!cache)
        cache = thread_caches.get();

    Block** cache_head = &cache->cache_free_heads[bucket_idx];

    // static __thread int gc_allocs = 0;
    // if (++gc_allocs == 128) {
    // static StatCounter sc_total("gc_allocs");
    // sc_total.log(128);
    // gc_allocs = 0;
    //}

    while (true) {
        while (Block* cache_block = *cache_head) {
            GCAllocation* rtn = _allocFromBlock(cache_block);
            if (rtn)
                return rtn;

            removeFromLLAndNull(cache_block);
            insertIntoLL(&cache->cache_full_heads[bucket_idx], cache_block);
        }

        // Not very useful to count the cache misses if we don't count the total attempts:
        // static StatCounter sc_fallback("gc_allocs_cachemiss");
        // sc_fallback.log();

        LOCK_REGION(heap->lock);

        assert(*cache_head == NULL);

        // should probably be called allocBlock:
        Block* myblock = _claimBlock(rounded_size, &heads[bucket_idx]);
        assert(myblock);
        assert(!myblock->next);
        assert(!myblock->prev);

        // printf("%d claimed new block %p with %d objects\n", threading::gettid(), myblock, myblock->numObjects());

        insertIntoLL(cache_head, myblock);
    }
}

// TODO: copy-pasted from _freeChain
void SmallArena::_getChainStatistics(HeapStatistics* stats, Block** head) {
    while (Block* b = *head) {
        int num_objects = b->numObjects();
        int first_obj = b->minObjIndex();
        int atoms_per_obj = b->atomsPerObj();

        for (int atom_idx = first_obj * atoms_per_obj; atom_idx < num_objects * atoms_per_obj;
             atom_idx += atoms_per_obj) {

            if (b->isfree.isSet(atom_idx))
                continue;

            void* p = &b->atoms[atom_idx];
            GCAllocation* al = reinterpret_cast<GCAllocation*>(p);

            addStatistic(stats, al, b->size);
        }

        head = &b->next;
    }
}

//////
/// Large Arena

#define LARGE_BLOCK_NUM_CHUNKS ((BLOCK_SIZE >> CHUNK_BITS) - 1)

#define LARGE_BLOCK_FOR_OBJ(obj) ((LargeBlock*)((int64_t)(obj) & ~(int64_t)(BLOCK_SIZE - 1)))
#define LARGE_CHUNK_INDEX(obj, section) (((char*)(obj) - (char*)(section)) >> CHUNK_BITS)

GCAllocation* LargeArena::alloc(size_t size) {
    registerGCManagedBytes(size);

    LOCK_REGION(heap->lock);

    // printf ("allocLarge %zu\n", size);

    LargeObj* obj = _alloc(size + sizeof(GCAllocation) + sizeof(LargeObj));

    obj->size = size;

    nullNextPrev(obj);
    insertIntoLL(&head, obj);

    return obj->data;
}

GCAllocation* LargeArena::realloc(GCAllocation* al, size_t bytes) {
    LargeObj* obj = LargeObj::fromAllocation(al);
    int size = obj->size;
    if (size >= bytes && size < bytes * 2)
        return al;

    GCAllocation* rtn = heap->alloc(bytes);
    memcpy(rtn, al, std::min(bytes, obj->size));

    _freeLargeObj(obj);
    return rtn;
}

void LargeArena::free(GCAllocation* al) {
    _freeLargeObj(LargeObj::fromAllocation(al));
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

void LargeArena::freeUnmarked(std::list<Box*, StlCompatAllocator<Box*>>& weakly_referenced) {
    sweepList(head, weakly_referenced, [this](LargeObj* ptr) { _freeLargeObj(ptr); });
}

void LargeArena::getStatistics(HeapStatistics* stats) {
    forEach(head, [stats](LargeObj* obj) { addStatistic(stats, obj->data, obj->size); });
}


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

    assert(section->num_free_chunks >= size >> CHUNK_BITS);
    section->num_free_chunks -= size >> CHUNK_BITS;

    return free_chunks;
}

LargeArena::LargeObj* LargeArena::_alloc(size_t size) {
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

    goto retry;
}

void LargeArena::_freeLargeObj(LargeObj* obj) {
    removeFromLL(obj);

    size_t size = obj->size;
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

//////
/// Huge Arena


GCAllocation* HugeArena::alloc(size_t size) {
    registerGCManagedBytes(size);

    LOCK_REGION(heap->lock);

    size_t total_size = size + sizeof(HugeObj);
    total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    HugeObj* rtn = (HugeObj*)doMmap(total_size);
    rtn->obj_size = size;

    nullNextPrev(rtn);
    insertIntoLL(&head, rtn);

    return rtn->data;
}

GCAllocation* HugeArena::realloc(GCAllocation* al, size_t bytes) {
    HugeObj* obj = HugeObj::fromAllocation(al);

    int capacity = obj->capacity();
    if (capacity >= bytes && capacity < bytes * 2)
        return al;

    GCAllocation* rtn = heap->alloc(bytes);
    memcpy(rtn, al, std::min(bytes, obj->obj_size));

    _freeHugeObj(obj);
    return rtn;
}

void HugeArena::free(GCAllocation* al) {
    _freeHugeObj(HugeObj::fromAllocation(al));
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

void HugeArena::freeUnmarked(std::list<Box*, StlCompatAllocator<Box*>>& weakly_referenced) {
    sweepList(head, weakly_referenced, [this](HugeObj* ptr) { _freeHugeObj(ptr); });
}

void HugeArena::getStatistics(HeapStatistics* stats) {
    forEach(head, [stats](HugeObj* obj) { addStatistic(stats, obj->data, obj->capacity()); });
}

void HugeArena::_freeHugeObj(HugeObj* lobj) {
    removeFromLL(lobj);
    int r = munmap(lobj, lobj->mmap_size());
    assert(r == 0);
}


} // namespace gc
} // namespace pyston
