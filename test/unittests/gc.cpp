#include <memory>
#include <vector>
#include <unordered_set>

#include "gtest/gtest.h"

#include "gc/gc_alloc.h"

using namespace pyston;
using namespace pyston::gc;

void testAlloc(int B) {
    struct S {
        int data[0];
    };

    std::unique_ptr<int> masks(new int[B/4]);
    masks.get()[0] = 0;
    for (int j = 1; j < B/4; j++) {
        masks.get()[j] = (masks.get()[j-1] * 1103515245 + 12345) % (1 << 31);
    }

    for (int l = 0; l < 10; l++) {
        std::vector<S*> allocd;
        std::unordered_set<S*> seen;

        const int N = l * 1000;
        for (int i = 0; i < N; i++) {
            S* t = static_cast<S*>(gc_alloc(B));

            ASSERT_TRUE(t != NULL);
            ASSERT_EQ(0, seen.count(t));

            for (int j = 0; j < B/4; j++) {
                t->data[j] = i ^ masks.get()[j];
            }

            allocd.push_back(t);
            seen.insert(t);
        }

        for (int i = 0; i < N; i++) {
            for (int j = 0; j < B/4; j++) {
                ASSERT_EQ(i ^ masks.get()[j], allocd[i]->data[j]);
            }
            gc_free(allocd[i]);
        }
    }
}

TEST(gc, alloc16) { testAlloc(16); }
TEST(gc, alloc24) { testAlloc(24); }
TEST(gc, alloc32) { testAlloc(32); }
TEST(gc, alloc48) { testAlloc(48); }
TEST(gc, alloc64) { testAlloc(64); }
TEST(gc, alloc128) { testAlloc(128); }
TEST(gc, alloc258) { testAlloc(258); }
TEST(gc, alloc3584) { testAlloc(3584); }

TEST(gc, largeallocs) {
    int s1 = 1 << 20;
    char* d1 = (char*)gc_alloc(s1);
    memset(d1, 1, s1);

    int s2 = 2 << 20;
    char* d2 = (char*)gc_alloc(s2);
    memset(d2, 2, s2);

    int s3 = 4 << 20;
    char* d3 = (char*)gc_alloc(s3);
    memset(d3, 3, s3);

    for (int i = 0; i < s1; i++) {
        ASSERT_EQ(1, d1[i]);
    }

    for (int i = 0; i < s2; i++) {
        ASSERT_EQ(2, d2[i]);
    }

    for (int i = 0; i < s3; i++) {
        ASSERT_EQ(3, d3[i]);
    }
}

TEST(gc, freeing) {
    // Not sure this is enough to crash if it doesn't get freed:
    for (int i = 0; i < 1000000; i++) {
        void* a = gc_alloc(1024);
        gc_free(a);
    }
}

TEST(gc, freeingLarge) {
    for (int i = 0; i < 100000; i++) {
        void* a = gc_alloc(1<<24);
        gc_free(a);
    }
}

