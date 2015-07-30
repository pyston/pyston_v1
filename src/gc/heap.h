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

#ifndef PYSTON_GC_HEAP_H
#define PYSTON_GC_HEAP_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <sys/mman.h>

#include "core/common.h"
#include "core/threading.h"
#include "core/types.h"

namespace pyston {

namespace gc {
extern "C" void* gc_alloc(size_t bytes, GCKind kind_id) __attribute__((visibility("default")));
extern "C" void* gc_realloc(void* ptr, size_t bytes) __attribute__((visibility("default")));
extern "C" void gc_free(void* ptr) __attribute__((visibility("default")));
}

template <class T> class StlCompatAllocator {
public:
    typedef size_t size_type;
    typedef T value_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef std::ptrdiff_t difference_type;

    StlCompatAllocator() {}
    template <class U> StlCompatAllocator(const StlCompatAllocator<U>& other) {}

    template <class U> struct rebind { typedef StlCompatAllocator<U> other; };

    pointer allocate(size_t n) {
        size_t to_allocate = n * sizeof(value_type);
        // assert(to_allocate < (1<<16));

        return reinterpret_cast<pointer>(gc_alloc(to_allocate, gc::GCKind::CONSERVATIVE));
    }

    void deallocate(pointer p, size_t n) {
        if (p != NULL)
            gc::gc_free(p);
    }

    // I would never be able to come up with this on my own:
    // http://en.cppreference.com/w/cpp/memory/allocator/construct
    template <class U, class... Args> void construct(U* p, Args&&... args) {
        ::new ((void*)p) U(std::forward<Args>(args)...);
    }

    template <class U> void destroy(U* p) { p->~U(); }

    bool operator==(const StlCompatAllocator<T>& rhs) const { return true; }
    bool operator!=(const StlCompatAllocator<T>& rhs) const { return false; }
};

template <typename K, typename V, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>>
class conservative_unordered_map
    : public std::unordered_map<K, V, Hash, KeyEqual, StlCompatAllocator<std::pair<const K, V>>> {};


namespace gc {

extern unsigned bytesAllocatedSinceCollection;
#define ALLOCBYTES_PER_COLLECTION 10000000
void _bytesAllocatedTripped();

// Notify the gc of n bytes as being under GC management.
// This is called internally for anything allocated through gc_alloc,
// but it can also be called by clients to say that they have memory that
// is ultimately GC managed but did not get allocated through gc_alloc,
// such as memory that will get freed by a gc destructor.
inline void registerGCManagedBytes(size_t bytes) {
    bytesAllocatedSinceCollection += bytes;
    if (unlikely(bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION)) {
        _bytesAllocatedTripped();
    }
}


class Heap;
struct HeapStatistics;

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
// reserved bit - along with MARK_BIT, encodes the states of finalization order
#define ORDERING_EXTRA_BIT 0x2
#define FINALIZER_HAS_RUN_BIT 0x4

#define ORDERING_BITS (MARK_BIT | ORDERING_EXTRA_BIT)

enum FinalizationState {
    UNREACHABLE = 0x0,
    TEMPORARY = ORDERING_EXTRA_BIT,

    // Note that these two states have MARK_BIT set.
    ALIVE = MARK_BIT,
    REACHABLE_FROM_FINALIZER = ORDERING_BITS,
};

inline bool isMarked(GCAllocation* header) {
    return (header->gc_flags & MARK_BIT) != 0;
}

inline void setMark(GCAllocation* header) {
    assert(!isMarked(header));
    header->gc_flags |= MARK_BIT;
}

inline void clearMark(GCAllocation* header) {
    assert(isMarked(header));
    header->gc_flags &= ~MARK_BIT;
}

inline bool hasFinalized(GCAllocation* header) {
    return (header->gc_flags & FINALIZER_HAS_RUN_BIT) != 0;
}

inline void setFinalized(GCAllocation* header) {
    assert(!hasFinalized(header));
    header->gc_flags |= FINALIZER_HAS_RUN_BIT;
}

inline FinalizationState orderingState(GCAllocation* header) {
    int state = header->gc_flags & ORDERING_BITS;
    assert(state <= static_cast<int>(FinalizationState::REACHABLE_FROM_FINALIZER));
    return static_cast<FinalizationState>(state);
}

inline void setOrderingState(GCAllocation* header, FinalizationState state) {
    header->gc_flags = (header->gc_flags & ~ORDERING_BITS) | static_cast<int>(state);
}

inline void clearOrderingState(GCAllocation* header) {
    header->gc_flags &= ~ORDERING_EXTRA_BIT;
}

#undef MARK_BIT
#undef ORDERING_EXTRA_BIT
#undef FINALIZER_HAS_RUN_BIT
#undef ORDERING_BITS

bool hasOrderedFinalizer(BoxedClass* cls);
void finalize(Box* b);
bool isWeaklyReferenced(Box* b);

#define PAGE_SIZE 4096

template <uintptr_t arena_start, uintptr_t arena_size, uintptr_t initial_mapsize, uintptr_t increment> class Arena {
private:
    void* cur;
    void* frontier;
    void* arena_end;

protected:
    Arena() : cur((void*)arena_start), frontier((void*)arena_start), arena_end((void*)(arena_start + arena_size)) {
        if (initial_mapsize)
            extendMapping(initial_mapsize);
    }

    // extends the mapping for this arena
    void extendMapping(size_t size) {
        assert(size % PAGE_SIZE == 0);

        assert(((uint8_t*)frontier + size) < arena_end && "arena full");

        void* mrtn = mmap(frontier, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        RELEASE_ASSERT((uintptr_t)mrtn != -1, "failed to allocate memory from OS");
        ASSERT(mrtn == frontier, "%p %p\n", mrtn, cur);

        frontier = (uint8_t*)frontier + size;
    }

    void* allocFromArena(size_t size) {
        if (((char*)cur + size) >= (char*)frontier) {
            // grow the arena by a multiple of increment such that we can service the allocation request
            size_t grow_size = (size + increment - 1) & ~(increment - 1);
            extendMapping(grow_size);
        }

        void* rtn = cur;
        cur = (uint8_t*)cur + size;
        return rtn;
    }

public:
    bool contains(void* addr) { return (void*)arena_start <= addr && addr < cur; }
};

constexpr uintptr_t ARENA_SIZE = 0x1000000000L;
constexpr uintptr_t SMALL_ARENA_START = 0x1270000000L;
constexpr uintptr_t LARGE_ARENA_START = 0x2270000000L;
constexpr uintptr_t HUGE_ARENA_START = 0x3270000000L;


//
// The SmallArena allocates objects <= 3584 bytes.
//
// it uses segregated-fit allocation, and each block contains a free
// bitmap for objects of a given size (constant for the block)
//
static const size_t sizes[] = {
    16,  32,  48,  64,  80,  96,  112, 128, 160,  192, 224,
    256, 320, 384, 448, 512, 640, 768, 896, 1024, 1280 //, 1536, 1792, 2048, 2560, 3072, 3584, // 4096,
};
static constexpr size_t NUM_BUCKETS = sizeof(sizes) / sizeof(sizes[0]);


class SmallArena : public Arena<SMALL_ARENA_START, ARENA_SIZE, 64 * 1024 * 1024, 16 * 1024 * 1024> {
public:
    SmallArena(Heap* heap) : Arena(), heap(heap), thread_caches(heap, this) {
#ifndef NDEBUG
        // Various things will crash if we instantiate multiple Heaps/Arenas
        static bool already_created = false;
        assert(!already_created);
        already_created = true;
#endif
    }

    GCAllocation* __attribute__((__malloc__)) alloc(size_t bytes) {
        registerGCManagedBytes(bytes);
        if (bytes <= 16)
            return _alloc(16, 0);
        else if (bytes <= 32)
            return _alloc(32, 1);
        else {
            for (int i = 2; i < NUM_BUCKETS; i++) {
                if (sizes[i] >= bytes) {
                    return _alloc(sizes[i], i);
                }
            }
            return NULL;
        }
    }

    GCAllocation* realloc(GCAllocation* alloc, size_t bytes);
    void free(GCAllocation* al);

    GCAllocation* allocationFrom(void* ptr);
    void freeUnmarked(std::vector<Box*>& weakly_referenced);

    void getStatistics(HeapStatistics* stats);

    void prepareForCollection() {}
    void cleanupAfterCollection() {}

#ifndef NDEBUG
    void assertConsistent();
#else
    void assertConsistent() {}
#endif

private:
    template <int N> class Bitmap {
        static_assert(N % 64 == 0, "");

    private:
        uint64_t data[N / 64];

    public:
        void setAllZero() { memset(data, 0, sizeof(data)); }

        struct Scanner {
        private:
            int64_t next_to_check;
            friend class Bitmap<N>;

        public:
            void reset() { next_to_check = 0; }
        };

        bool isSet(int idx) { return (data[idx / 64] >> (idx % 64)) & 1; }

        void set(int idx) { data[idx / 64] |= 1UL << (idx % 64); }

        void toggle(int idx) { data[idx / 64] ^= 1UL << (idx % 64); }

        void clear(int idx) { data[idx / 64] &= ~(1UL << (idx % 64)); }

        int scanForNext(Scanner& sc) {
            uint64_t mask = data[sc.next_to_check];

            if (unlikely(mask == 0L)) {
                while (true) {
                    sc.next_to_check++;
                    if (sc.next_to_check == N / 64) {
                        sc.next_to_check = 0;
                        return -1;
                    }
                    mask = data[sc.next_to_check];
                    if (likely(mask != 0L)) {
                        break;
                    }
                }
            }

            int i = sc.next_to_check;

            int first = __builtin_ctzll(mask);
            assert(first < 64);
            assert(data[i] & (1L << first));
            data[i] ^= (1L << first);

            int idx = first + i * 64;
            return idx;
        }
    };


    static constexpr size_t BLOCK_SIZE = 4 * 4096;

#define ATOM_SIZE 16
    static_assert(BLOCK_SIZE % ATOM_SIZE == 0, "");
#define ATOMS_PER_BLOCK (BLOCK_SIZE / ATOM_SIZE)
    static_assert(ATOMS_PER_BLOCK % 64 == 0, "");
#define BITFIELD_SIZE (ATOMS_PER_BLOCK / 8)
#define BITFIELD_ELTS (BITFIELD_SIZE / 8)

#define BLOCK_HEADER_SIZE (BITFIELD_SIZE + 4 * sizeof(void*))
#define BLOCK_HEADER_ATOMS ((BLOCK_HEADER_SIZE + ATOM_SIZE - 1) / ATOM_SIZE)

    struct Atoms {
        char _data[ATOM_SIZE];
    };

public:
    struct Block {
        union {
            struct {
                Block* next, **prev;
                uint32_t size;
                uint16_t num_obj;
                uint8_t min_obj_index;
                uint8_t atoms_per_obj;
                Bitmap<ATOMS_PER_BLOCK> isfree;
                Bitmap<ATOMS_PER_BLOCK>::Scanner next_to_check;
                void* _header_end[0];
            };
            Atoms atoms[ATOMS_PER_BLOCK];
        };

        inline int minObjIndex() const { return min_obj_index; }

        inline int numObjects() const { return num_obj; }

        inline int atomsPerObj() const { return atoms_per_obj; }

        static Block* forPointer(void* ptr) { return (Block*)((uintptr_t)ptr & ~(BLOCK_SIZE - 1)); }
    };
    static_assert(sizeof(Block) == BLOCK_SIZE, "bad size");
    static_assert(offsetof(Block, _header_end) >= BLOCK_HEADER_SIZE, "bad header size");
    static_assert(offsetof(Block, _header_end) <= BLOCK_HEADER_SIZE, "bad header size");

private:
    struct ThreadBlockCache {
        Heap* heap;
        SmallArena* small;
        Block* cache_free_heads[NUM_BUCKETS];
        Block* cache_full_heads[NUM_BUCKETS];

        ThreadBlockCache(Heap* heap, SmallArena* small) : heap(heap), small(small) {
            memset(cache_free_heads, 0, sizeof(cache_free_heads));
            memset(cache_full_heads, 0, sizeof(cache_full_heads));
        }
        ~ThreadBlockCache();
    };


    Block* heads[NUM_BUCKETS];
    Block* full_heads[NUM_BUCKETS];

    friend struct ThreadBlockCache;

    Heap* heap;
    // TODO only use thread caches if we're in GRWL mode?
    threading::PerThreadSet<ThreadBlockCache, Heap*, SmallArena*> thread_caches;

    Block* _allocBlock(uint64_t size, Block** prev);
    GCAllocation* _allocFromBlock(Block* b);
    Block* _claimBlock(size_t rounded_size, Block** free_head);
    Block** _freeChain(Block** head, std::vector<Box*>& weakly_referenced);
    void _getChainStatistics(HeapStatistics* stats, Block** head);

    GCAllocation* __attribute__((__malloc__)) _alloc(size_t bytes, int bucket_idx);
};

struct ObjLookupCache {
    void* data;
    size_t size;

    ObjLookupCache(void* data, size_t size) : data(data), size(size) {}
};


//
// The LargeArena allocates objects where 3584 < size <1024*1024-CHUNK_SIZE-sizeof(LargeObject) bytes.
//
// it maintains a set of size-segregated free lists, and a special
// free list for larger objects.  If the free list specific to a given
// size has no entries, we search the large free list.
//
// Blocks of 1meg are mmap'ed individually, and carved up as needed.
//
class LargeArena : public Arena<LARGE_ARENA_START, ARENA_SIZE, 32 * 1024 * 1024, 16 * 1024 * 1024> {
private:
    struct LargeBlock {
        LargeBlock* next;
        size_t num_free_chunks;
        unsigned char* free_chunk_map;
    };

    struct LargeFreeChunk {
        LargeFreeChunk* next_size;
        size_t size;
    };

    struct LargeObj {
        LargeObj* next, **prev;
        size_t size;
        GCAllocation data[0];

        static LargeObj* fromAllocation(GCAllocation* alloc) {
            char* rtn = (char*)alloc - offsetof(LargeObj, data);
            return reinterpret_cast<LargeObj*>(rtn);
        }
    };

    /*
     * This shouldn't be much smaller or larger than the largest small size bucket.
     * Must be at least sizeof (LargeBlock).
     */
    static constexpr size_t CHUNK_SIZE = 4096;
    static constexpr int CHUNK_BITS = 12;

    static_assert(CHUNK_SIZE > sizeof(LargeBlock), "bad large block size");

    static constexpr int BLOCK_SIZE = 1024 * 1024;

    static constexpr int NUM_FREE_LISTS = 32;

    std::vector<ObjLookupCache> lookup; // used during gc's to speed up finding large object GCAllocations
    Heap* heap;
    LargeObj* head;
    LargeBlock* blocks;
    LargeFreeChunk* free_lists[NUM_FREE_LISTS]; /* 0 is for larger sizes */

    void add_free_chunk(LargeFreeChunk* free_chunks, size_t size);
    LargeFreeChunk* get_from_size_list(LargeFreeChunk** list, size_t size);
    LargeObj* _alloc(size_t size);
    void _freeLargeObj(LargeObj* obj);

public:
    LargeArena(Heap* heap) : heap(heap), head(NULL), blocks(NULL) {}

    /* Largest object that can be allocated in a large block. */
    static constexpr size_t ALLOC_SIZE_LIMIT = BLOCK_SIZE - CHUNK_SIZE - sizeof(LargeObj);

    GCAllocation* __attribute__((__malloc__)) alloc(size_t bytes);
    GCAllocation* realloc(GCAllocation* alloc, size_t bytes);
    void free(GCAllocation* alloc);

    GCAllocation* allocationFrom(void* ptr);
    void freeUnmarked(std::vector<Box*>& weakly_referenced);

    void getStatistics(HeapStatistics* stats);

    void prepareForCollection();
    void cleanupAfterCollection();
};

// The HugeArena allocates objects where size > 1024*1024 bytes.
//
// Objects are allocated with individual mmap() calls, and kept in a
// linked list.  They are not reused.
class HugeArena : public Arena<HUGE_ARENA_START, ARENA_SIZE, 0, PAGE_SIZE> {
public:
    HugeArena(Heap* heap) : heap(heap) {}

    GCAllocation* __attribute__((__malloc__)) alloc(size_t bytes);
    GCAllocation* realloc(GCAllocation* alloc, size_t bytes);
    void free(GCAllocation* alloc);

    GCAllocation* allocationFrom(void* ptr);
    void freeUnmarked(std::vector<Box*>& weakly_referenced);

    void getStatistics(HeapStatistics* stats);

    void prepareForCollection();
    void cleanupAfterCollection();

private:
    struct HugeObj {
        HugeObj* next, **prev;
        size_t size;
        GCAllocation data[0];

        int mmap_size() {
            size_t total_size = size + sizeof(HugeObj);
            total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            return total_size;
        }

        int capacity() { return mmap_size() - sizeof(HugeObj); }

        static HugeObj* fromAllocation(GCAllocation* alloc) {
            char* rtn = (char*)alloc - offsetof(HugeObj, data);
            assert((uintptr_t)rtn % PAGE_SIZE == 0);
            return reinterpret_cast<HugeObj*>(rtn);
        }
    };

    void _freeHugeObj(HugeObj* lobj);

    HugeObj* head;
    std::vector<ObjLookupCache> lookup; // used during gc's to speed up finding large object GCAllocations

    Heap* heap;
};


class Heap {
private:
    SmallArena small_arena;
    LargeArena large_arena;
    HugeArena huge_arena;

    friend class SmallArena;
    friend class LargeArena;
    friend class HugeArena;

    // DS_DEFINE_MUTEX(lock);
    DS_DEFINE_SPINLOCK(lock);

public:
    Heap() : small_arena(this), large_arena(this), huge_arena(this) {}

    GCAllocation* realloc(GCAllocation* alloc, size_t bytes) {

        // TODO(toshok): there is duplicate code in each of the
        // ::realloc methods to test whether the allocation can be
        // reused.  Would be nice to factor it all out here into this
        // method.

        GCAllocation* rtn;
        if (large_arena.contains(alloc)) {
            rtn = large_arena.realloc(alloc, bytes);
        } else if (huge_arena.contains(alloc)) {
            rtn = huge_arena.realloc(alloc, bytes);
        } else {
            assert(small_arena.contains(alloc));
            rtn = small_arena.realloc(alloc, bytes);
        }

        return rtn;
    }

    GCAllocation* __attribute__((__malloc__)) alloc(size_t bytes) {
        if (bytes > LargeArena::ALLOC_SIZE_LIMIT)
            return huge_arena.alloc(bytes);
        else if (bytes > sizes[NUM_BUCKETS - 1])
            return large_arena.alloc(bytes);
        else
            return small_arena.alloc(bytes);
    }

    void destructContents(GCAllocation* alloc);

    void free(GCAllocation* alloc) {
        destructContents(alloc);

        if (large_arena.contains(alloc)) {
            large_arena.free(alloc);
            return;
        }

        if (huge_arena.contains(alloc)) {
            huge_arena.free(alloc);
            return;
        }

        assert(small_arena.contains(alloc));
        small_arena.free(alloc);
    }

    // not thread safe:
    GCAllocation* getAllocationFromInteriorPointer(void* ptr) {
        if (large_arena.contains(ptr)) {
            return large_arena.allocationFrom(ptr);
        } else if (huge_arena.contains(ptr)) {
            return huge_arena.allocationFrom(ptr);
        } else if (small_arena.contains(ptr)) {
            return small_arena.allocationFrom(ptr);
        }

        return NULL;
    }

    // not thread safe:
    void freeUnmarked(std::vector<Box*>& weakly_referenced) {
        small_arena.freeUnmarked(weakly_referenced);
        large_arena.freeUnmarked(weakly_referenced);
        huge_arena.freeUnmarked(weakly_referenced);
    }

    void prepareForCollection() {
        small_arena.prepareForCollection();
        large_arena.prepareForCollection();
        huge_arena.prepareForCollection();
    }

    void cleanupAfterCollection() {
        small_arena.cleanupAfterCollection();
        large_arena.cleanupAfterCollection();
        huge_arena.cleanupAfterCollection();
    }

    void dumpHeapStatistics(int level);
};

extern Heap global_heap;
void dumpHeapStatistics(int level);

} // namespace gc
} // namespace pyston

#endif
