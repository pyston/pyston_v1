#include <vector>
#include <cstdio>
#include "stdint.h"

class C {
public:
    C(int i) {
        printf("ctor\n");
    }
    void* operator new(size_t bytes) {
        printf("operator new\n");
        return NULL;
    }
};

int f() {
    printf("f()");
    return 1;
}

extern "C" C* c() {
    return new C(f());
}

void moo(double);
float chrs[10];
void foo(int i) {
    moo(chrs[i]);
}

void moo2(long long);
int chrs2[10];
void foo2(int i) {
    moo2(chrs2[i]);
}

void moo3(long long);
short chrs3[10];
void foo3(int i) {
    moo3(chrs3[i]);
}

void moo4(bool);
bool chrs4[10];
void foo4(int i) {
    moo4(chrs4[i]);
}
