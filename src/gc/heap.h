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

#include <cstdint>

#include "core/common.h"

namespace pyston {
namespace gc {

#define BLOCK_SIZE 4096
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
            Block *next, **prev;
            uint64_t size;
            uint64_t isfree[BITFIELD_ELTS];
        };
        Atoms atoms[ATOMS_PER_BLOCK];
    };

    inline int minObjIndex() {
        return (BLOCK_HEADER_SIZE + size - 1) / size;
    }

    inline int numObjects() {
        return BLOCK_SIZE / size;
    }

    inline int atomsPerObj() {
        return size / ATOM_SIZE;
    }

    static Block* forPointer(void* ptr) {
        return (Block*)((uintptr_t)ptr & ~(BLOCK_SIZE-1));
    }
};
static_assert(sizeof(Block) == BLOCK_SIZE, "bad size");

constexpr const size_t sizes[] = {
    16, 32, 48, 64,
    80, 96, 112, 128,
    160, 192, 224, 256,
    320, 384, 448, 512,
    640, 768, 896, 1024,
    1280, 1536, 1792, 2048,
    //2560, 3072, 3584, // 4096,
};
#define NUM_BUCKETS (sizeof(sizes) / sizeof(sizes[0]))

class LargeObj;
class Heap {
    private:
        Block* heads[NUM_BUCKETS];
        Block* full_heads[NUM_BUCKETS];
        LargeObj *large_head = NULL;

        void* allocSmall(size_t rounded_size, Block **head, Block **full_head);
        void* allocLarge(size_t bytes);

    public:
        void* realloc(void* ptr, size_t bytes);

        void* alloc(size_t bytes) {
            //assert(bytes >= 16);
            if (bytes <= 16)
                return allocSmall(16, &heads[0], &full_heads[0]);
            if (bytes <= 32)
                return allocSmall(32, &heads[1], &full_heads[1]);

            if (bytes > sizes[NUM_BUCKETS-1]) {
                return allocLarge(bytes);
            }

            for (int i = 2; i < NUM_BUCKETS; i++) {
                if (sizes[i] >= bytes) {
                    return allocSmall(sizes[i], &heads[i], &full_heads[i]);
                }
            }

            //unreachable
            abort();
        }

        void free(void* ptr);

        void* getAllocationFromInteriorPointer(void* ptr);
        void freeUnmarked();
};

extern Heap global_heap;

} // namespace gc
} // namespace pyston

#endif
