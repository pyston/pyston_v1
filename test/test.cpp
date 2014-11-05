#include <vector>
#include <cstdio>
#include "stdint.h"

extern void foo();

void bar(int* x) {
    if (*x & 0x08)
        foo();
}
