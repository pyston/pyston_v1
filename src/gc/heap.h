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

#ifndef PYSTON_GC_HEAP_H
#define PYSTON_GC_HEAP_H

#include <cstddef>
#include <cstdint>

#include "core/common.h"
#include "core/threading.h"

namespace pyston {
namespace gc {

typedef uint8_t kindid_t;
struct GCAllocation {
    unsigned int gc_flags : 8;
    GCKind kind_id : 8;
    unsigned int _reserved1 : 16;
    unsigned int kind_data : 32;

    char user_data[0];

    static GCAllocation* fromUserData(void* user_data) {
        char* d = reinterpret_cast<char*>(user_data);
        return reinterpret_cast<GCAllocation*>(d - offsetof(GCAllocation, user_data));
    }
};
static_assert(sizeof(GCAllocation) <= sizeof(void*),
              "we should try to make sure the gc header is word-sized or smaller");

#define MARK_BIT 0x1

inline void setMark(GCAllocation* header) {
    header->gc_flags |= MARK_BIT;
}

inline void clearMark(GCAllocation* header) {
    header->gc_flags &= ~MARK_BIT;
}

inline bool isMarked(GCAllocation* header) {
    return (header->gc_flags & MARK_BIT) != 0;
}

#undef MARK_BIT



#define BLOCK_SIZE (4 * 4096)
#define ATOM_SIZE 16
static_assert(BLOCK_SIZE % ATOM_SIZE == 0, "");
#define ATOMS_PER_BLOCK (BLOCK_SIZE / ATOM_SIZE)
static_assert(ATOMS_PER_BLOCK % 64 == 0, "");
#define BITFIELD_SIZE (ATOMS_PER_BLOCK / 8)
#define BITFIELD_ELTS (BITFIELD_SIZE / 8)

#define BLOCK_HEADER_SIZE (BITFIELD_SIZE + 2 * sizeof(void*) + sizeof(uint64_t))
#define BLOCK_HEADER_ATOMS ((BLOCK_HEADER_SIZE + ATOM_SIZE - 1) / ATOM_SIZE)

struct Atoms {
    char _data[ATOM_SIZE];
};

struct Block {
    union {
        struct {
            Block* next, **prev;
            uint64_t size;
            uint64_t isfree[BITFIELD_ELTS];
        };
        Atoms atoms[ATOMS_PER_BLOCK];
    };

    inline int minObjIndex() { return (BLOCK_HEADER_SIZE + size - 1) / size; }

    inline int numObjects() { return BLOCK_SIZE / size; }

    inline int atomsPerObj() { return size / ATOM_SIZE; }

    static Block* forPointer(void* ptr) { return (Block*)((uintptr_t)ptr & ~(BLOCK_SIZE - 1)); }
};
static_assert(sizeof(Block) == BLOCK_SIZE, "bad size");

constexpr const size_t sizes[] = {
    16,  32,  48,  64,  80,  96,  112, 128,  160,  192,  224,  256,
    320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048,
    // 2560, 3072, 3584, // 4096,
};
#define NUM_BUCKETS (sizeof(sizes) / sizeof(sizes[0]))

class LargeObj;
class Heap {
private:
    Block* heads[NUM_BUCKETS];
    Block* full_heads[NUM_BUCKETS];
    LargeObj* large_head = NULL;

    GCAllocation* __attribute__((__malloc__)) allocSmall(size_t rounded_size, int bucket_idx);
    GCAllocation* __attribute__((__malloc__)) allocLarge(size_t bytes);

    // DS_DEFINE_MUTEX(lock);
    DS_DEFINE_SPINLOCK(lock);

    struct ThreadBlockCache {
        Heap* heap;
        Block* cache_free_heads[NUM_BUCKETS];
        Block* cache_full_heads[NUM_BUCKETS];

        ThreadBlockCache(Heap* heap) : heap(heap) {
            memset(cache_free_heads, 0, sizeof(cache_free_heads));
            memset(cache_full_heads, 0, sizeof(cache_full_heads));
        }
        ~ThreadBlockCache();
    };
    friend class ThreadBlockCache;
    // TODO only use thread caches if we're in GRWL mode?
    threading::PerThreadSet<ThreadBlockCache, Heap*> thread_caches;

public:
    Heap() : thread_caches(this) {}

    GCAllocation* realloc(GCAllocation* alloc, size_t bytes);

    GCAllocation* __attribute__((__malloc__)) alloc(size_t bytes) {
        GCAllocation* rtn;
        // assert(bytes >= 16);
        if (bytes <= 16)
            rtn = allocSmall(16, 0);
        else if (bytes <= 32)
            rtn = allocSmall(32, 1);
        else if (bytes > sizes[NUM_BUCKETS - 1])
            rtn = allocLarge(bytes);
        else {
            rtn = NULL;
            for (int i = 2; i < NUM_BUCKETS; i++) {
                if (sizes[i] >= bytes) {
                    rtn = allocSmall(sizes[i], i);
                    break;
                }
            }
        }

        return rtn;
    }

    void free(GCAllocation* alloc);

    // not thread safe:
    GCAllocation* getAllocationFromInteriorPointer(void* ptr);
    // not thread safe:
    void freeUnmarked();

    void dumpHeapStatistics();
};

extern Heap global_heap;
void dumpHeapStatistics();

} // namespace gc
} // namespace pyston

#endif
