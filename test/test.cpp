#include <vector>
#include <cstdio>
#include "stdint.h"

void set64(int64_t* ptr) {
    *ptr = 0x1234;
}

void set64full(int64_t* ptr) {
    *ptr = 0x1234567890;
}

void set32(int64_t* ptr) {
    *(int32_t*)ptr = 0x1234;
}
