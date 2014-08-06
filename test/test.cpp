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
