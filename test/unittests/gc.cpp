#include <memory>
#include <vector>
#include <unordered_set>

#include "gtest/gtest.h"

#include "core/types.h"
#include "gc/gc_alloc.h"
#include "runtime/types.h"
#include "unittests.h"

using namespace pyston;
using namespace pyston::gc;

struct S {
    GCObjectHeader header;
    int data[0];
};

void testAlloc(int B) {
    std::unique_ptr<int> masks(new int[B/4]);
    masks.get()[0] = 0;
    for (int j = 1; j < B/4; j++) {
        masks.get()[j] = (masks.get()[j-1] * 1103515245 + 12345) % (1 << 31);
    }

    for (int l = 0; l < 10; l++) {
        std::vector<S*, StlCompatAllocator<S*>> allocd;
        std::unordered_set<S*, std::hash<S*>, std::equal_to<S*>, StlCompatAllocator<S*>> seen;

        int N = l * 1000;
        if (B > 1024)
            N /= 10;
        for (int i = 0; i < N; i++) {
            S* t = static_cast<S*>(gc_alloc(B));
            t->header.kind_id = untracked_kind.kind_id;

            ASSERT_TRUE(t != NULL);
            ASSERT_EQ(0, seen.count(t));

            for (int j = 0; j < (B - sizeof(S))/4; j++) {
                t->data[j] = i ^ masks.get()[j];
            }

            allocd.push_back(t);
            seen.insert(t);
        }

        for (int i = 0; i < N; i++) {
            for (int j = 0; j < (B - sizeof(S))/4; j++) {
                ASSERT_EQ(i ^ masks.get()[j], allocd[i]->data[j]);
            }
            gc_free(allocd[i]);
        }
    }
}

TEST(alloc, alloc16) { testAlloc(16); }
TEST(alloc, alloc24) { testAlloc(24); }
TEST(alloc, alloc32) { testAlloc(32); }
TEST(alloc, alloc48) { testAlloc(48); }
TEST(alloc, alloc64) { testAlloc(64); }
TEST(alloc, alloc128) { testAlloc(128); }
TEST(alloc, alloc258) { testAlloc(258); }
TEST(alloc, alloc3584) { testAlloc(3584); }

TEST(alloc, largeallocs) {
    int s1 = 1 << 20;
    S* d1 = (S*)gc_alloc(s1);
    d1->header.kind_id = untracked_kind.kind_id;
    memset(d1->data, 1, s1 - sizeof(S));

    int s2 = 2 << 20;
    S* d2 = (S*)gc_alloc(s2);
    d2->header.kind_id = untracked_kind.kind_id;
    memset(d2->data, 2, s2 - sizeof(S));

    int s3 = 4 << 20;
    S* d3 = (S*)gc_alloc(s3);
    d3->header.kind_id = untracked_kind.kind_id;
    memset(d3->data, 3, s3 - sizeof(S));

    for (int i = sizeof(S); i < s1; i++) {
        ASSERT_EQ(1, *(i + (char*)d1));
    }

    for (int i = sizeof(S); i < s2; i++) {
        ASSERT_EQ(2, *(i + (char*)d2));
    }

    for (int i = sizeof(S); i < s3; i++) {
        ASSERT_EQ(3, *(i + (char*)d3));
    }
}

TEST(alloc, freeing) {
    // Not sure this is enough to crash if it doesn't get freed:
    for (int i = 0; i < 100000; i++) {
        void* a = gc_alloc(1024);
        gc_free(a);
    }
}

TEST(alloc, freeingLarge) {
    for (int i = 0; i < 200; i++) {
        void* a = gc_alloc(1<<26);
        gc_free(a);
    }
}

