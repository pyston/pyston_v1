#include <vector>
#include <cstdio>
#include "stdint.h"

void set64(int64_t* ptr) {
    *ptr = 0x1234;
}

void set64full(int64_t* ptr) {
    *ptr = 0x1234567890;
}

namespace pyston {
    class Box {};

int throw_catch(Box* b) {
    try {
        throw b;
    } catch (int e) {
        return e;
    }
}
}
