#include <vector>
#include <cstdio>
#include "stdint.h"

struct ExcInfo {
    int64_t a, b, c;
};

void bench0() {
    int64_t t = 0;
    for (int i = 0; i < 1000000; i++) {
        try {
            throw 0;
        } catch (int x) {
        }
    }
    printf("%ld\n", t);
}

void bench1() {
    int64_t t = 1;
    for (int i = 0; i < 1000000; i++) {
        try {
            throw ExcInfo({.a=t, .b=t, .c=t});
        } catch (ExcInfo e) {
            t += e.a + e.b + e.c;
        }
    }
    printf("b1 %ld\n", t);
}

static __thread ExcInfo curexc;
struct ExceptionOccurred {
};

void bench2() {
    int64_t t = 1;
    for (int i = 0; i < 1000000; i++) {
        try {
            curexc.a = t;
            curexc.b = t;
            curexc.c = t;
            throw ExceptionOccurred();
        } catch (ExceptionOccurred) {
            t += curexc.a + curexc.b + curexc.c;
        }
    }
    printf("b2 %ld\n", t);
}

void rbench1() {
    int64_t t = 1;
    for (int i = 0; i < 1000000; i++) {
        try {
            try {
                throw ExcInfo({.a=t, .b=t, .c=t});
            } catch (ExcInfo e) {
                throw e;
            }
        } catch (ExcInfo e) {
            t += e.a + e.b + e.c;
        }
    }
    printf("rb1 %ld\n", t);
}

void rbench2() {
    int64_t t = 1;
    for (int i = 0; i < 1000000; i++) {
        try {
            try {
                curexc.a = t;
                curexc.b = t;
                curexc.c = t;
                throw ExceptionOccurred();
            } catch (ExceptionOccurred x) {
                throw x;
            }
        } catch (ExceptionOccurred) {
            t += curexc.a + curexc.b + curexc.c;
        }
    }
    printf("rb2 %ld\n", t);
}

int main() {
    bench1();
}
