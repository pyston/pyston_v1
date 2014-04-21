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

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <stdint.h>
#include <sys/mman.h>

#include "valgrind.h"

#include "gc/gc_alloc.h"

#include "core/common.h"

namespace pyston {
namespace gc {

//extern unsigned numAllocs;
//#define ALLOCS_PER_COLLECTION 1000
extern unsigned bytesAllocatedSinceCollection;
#define ALLOCBYTES_PER_COLLECTION 2000000

void _collectIfNeeded(size_t bytes) {
    if (bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION) {
        bytesAllocatedSinceCollection = 0;
        runCollection();
    }
    bytesAllocatedSinceCollection += bytes;
}


Heap global_heap;

#define PAGE_SIZE 4096
class Arena {
    private:
        void* start;
        void* cur;

    public:
        constexpr Arena(void* start) : start(start), cur(start) {
        }

        void* doMmap(size_t size) {
            assert(size % PAGE_SIZE == 0);
            //printf("mmap %ld\n", size);

            void* mrtn = mmap(cur, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            assert((uintptr_t)mrtn != -1 && "failed to allocate memory from OS");
            ASSERT(mrtn == cur, "%p %p\n", mrtn, cur);
            cur = (uint8_t*)cur + size;
            return mrtn;
        }

        bool contains(void* addr) {
            return start <= addr && addr < cur;
        }
};

Arena small_arena((void*)0x1270000000L);
Arena large_arena((void*)0x2270000000L);

struct LargeObj {
    LargeObj *next, **prev;
    size_t obj_size;
    char data[0];

    int mmap_size() {
        size_t total_size = obj_size + sizeof(LargeObj);
        total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE-1);
        return total_size;
    }

    int capacity() {
        return mmap_size() - sizeof(LargeObj);
    }

    static LargeObj* fromPointer(void* ptr) {
        char* rtn = (char*)ptr + ((char*)NULL - ((LargeObj*)(NULL))->data);
        assert((uintptr_t)rtn % PAGE_SIZE == 0);
        return reinterpret_cast<LargeObj*>(rtn);
    }
};

void* Heap::allocLarge(size_t size) {
    _collectIfNeeded(size);

    size_t total_size = size + sizeof(LargeObj);
    total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE-1);
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
    // TODO use mmap

    Block* rtn = (Block*)small_arena.doMmap(sizeof(Block));
    assert(rtn);
    rtn->size = size;
    rtn->prev = prev;
    rtn->next = NULL;

#ifdef VALGRIND
    VALGRIND_CREATE_MEMPOOL(rtn, 0, true);
#endif

    // Don't think I need to do this:
    memset(rtn->isfree, 0, sizeof(Block::isfree));

    int num_objects = rtn->numObjects();
    int num_lost = rtn->minObjIndex();
    int atoms_per_object = rtn->atomsPerObj();
    for (int i = num_lost * atoms_per_object; i < num_objects * atoms_per_object; i += atoms_per_object) {
        int idx = i / 64;
        int bit = i % 64;
        rtn->isfree[idx] ^= (1L << bit);
        //printf("%d %d\n", idx, bit);
    }

    //printf("%d %d %d\n", num_objects, num_lost, atoms_per_object);
    //for (int i =0; i < BITFIELD_ELTS; i++) {
        //printf("%d: %lx\n", i, rtn->isfree[i]);
    //}
    return rtn;
}

void* Heap::allocSmall(size_t rounded_size, Block** prev, Block** full_head) {
    _collectIfNeeded(rounded_size);

    Block *cur = *prev;
    assert(!cur || prev == cur->prev);
    int scanned = 0;

    //printf("alloc(%ld)\n", rounded_size);

    //Block **full_prev = full_head;
    while (true) {
        //printf("cur = %p, prev = %p\n", cur, prev);
        if (cur == NULL) {
            Block *next = alloc_block(rounded_size, &cur->next);
            //printf("allocated new block %p\n", next);
            *prev = next;
            next->prev = prev;
            prev = &cur->next;

            next->next = *full_head;
            *full_head = NULL;
            prev = full_head;

            cur = next;
        }

        int i = 0;
        uint64_t mask = 0;
        for (; i < BITFIELD_ELTS; i++) {
            mask = cur->isfree[i];
            if (mask != 0L) {
                break;
            }
        }

        if (i == BITFIELD_ELTS) {
            scanned++;
            //printf("moving on\n");

            Block *t = *prev = cur->next;
            cur->next = NULL;
            if (t) t->prev = prev;

            cur->prev = full_head;
            cur->next = *full_head;
            *full_head = cur;

            cur = t;

            scanned++;
            continue;
        }

        //printf("scanned %d\n", scanned);
        int first = __builtin_ctzll(mask);
        assert(first < 64);
        //printf("mask: %lx, first: %d\n", mask, first);
        cur->isfree[i] ^= (1L << first);

        int idx = first + i * 64;

        //printf("Using index %d\n", idx);

        void* rtn = &cur->atoms[idx];

#ifndef NDEBUG
        Block *b = Block::forPointer(rtn);
        assert(b == cur);
        int offset = (char*)rtn - (char*)b;
        assert(offset % rounded_size == 0);
#endif

#ifdef VALGRIND
        VALGRIND_MEMPOOL_ALLOC(cur, rtn, rounded_size);
#endif

        return rtn;
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

#ifdef VALGRIND
    VALGRIND_MEMPOOL_FREE(b, ptr);
#endif
}

static void _freeLargeObj(LargeObj *lobj) {
    *lobj->prev = lobj->next;
    if (lobj->next)
        lobj->next->prev = lobj->prev;

    int r = munmap(lobj, lobj->mmap_size());
    assert(r == 0);
}

void Heap::free(void* ptr) {
    if (large_arena.contains(ptr)) {
        LargeObj *lobj = LargeObj::fromPointer(ptr);
        _freeLargeObj(lobj);
        return;
    }

    assert(small_arena.contains(ptr));
    Block *b = Block::forPointer(ptr);
    _freeFrom(ptr, b);
}

void* Heap::realloc(void* ptr, size_t bytes) {
    if (large_arena.contains(ptr)) {
        LargeObj *lobj = LargeObj::fromPointer(ptr);

        int capacity = lobj->capacity();
        if (capacity >= bytes && capacity < bytes * 2)
            return ptr;

        void* rtn = alloc(bytes);
        memcpy(rtn, ptr, std::min(bytes, lobj->obj_size));

        _freeLargeObj(lobj);
        return rtn;
    }

    assert(small_arena.contains(ptr));
    Block *b = Block::forPointer(ptr);

    size_t size = b->size;

    if (size >= bytes && size < bytes * 2)
        return ptr;

    void* rtn = alloc(bytes);

    memcpy(rtn, ptr, std::min(bytes, size));

    _freeFrom(ptr, b);
    return rtn;
}

void* Heap::getAllocationFromInteriorPointer(void* ptr) {
    if (large_arena.contains(ptr)) {
        LargeObj *cur = large_head;
        while (cur) {
            if (ptr >= cur && ptr < &cur->data[cur->obj_size])
                return &cur->data[0];
            cur = cur->next;
        }
        return NULL;
    }

    if (!small_arena.contains(ptr))
        return NULL;

    Block *b = Block::forPointer(ptr);
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

static long freeChain(Block* head) {
    long bytes_freed = 0;
    while (head) {
        int num_objects = head->numObjects();
        int first_obj = head->minObjIndex();
        int atoms_per_obj = head->atomsPerObj();

        for (int obj_idx = first_obj; obj_idx < num_objects; obj_idx++) {
            int atom_idx = obj_idx * atoms_per_obj;
            int bitmap_idx = atom_idx / 64;
            int bitmap_bit = atom_idx % 64;
            uint64_t mask = 1L << bitmap_bit;

            if (head->isfree[bitmap_idx] & mask)
                continue;

            void *p = &head->atoms[atom_idx];
            GCObjectHeader* header = headerFromObject(p);

            if (isMarked(header)) {
                clearMark(header);
            } else {
                if (VERBOSITY() >= 2) printf("Freeing %p\n", p);
                //assert(p != (void*)0x127000d960); // the main module
                bytes_freed += head->size;
                head->isfree[bitmap_idx] |= mask;
            }
        }

        head = head->next;
    }
    return bytes_freed;
}

void Heap::freeUnmarked() {
    long bytes_freed = 0;
    for (int bidx = 0; bidx < NUM_BUCKETS; bidx++) {
        bytes_freed += freeChain(heads[bidx]);
        bytes_freed += freeChain(full_heads[bidx]);
    }

    LargeObj *cur = large_head;
    while (cur) {
        void *p = cur->data;
        GCObjectHeader* header = headerFromObject(p);
        if (isMarked(header)) {
            clearMark(header);
        } else {
            if (VERBOSITY() >= 2) printf("Freeing %p\n", p);
            bytes_freed += cur->mmap_size();

            *cur->prev = cur->next;
            if (cur->next) cur->next->prev = cur->prev;

            LargeObj *to_free = cur;
            cur = cur->next;
            _freeLargeObj(to_free);
            continue;
        }

        cur = cur->next;
    }

    if (VERBOSITY("gc") >= 2) if (bytes_freed) printf("Freed %ld bytes\n", bytes_freed);
}

}
}
