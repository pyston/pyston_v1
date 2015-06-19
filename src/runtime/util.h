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

#ifndef PYSTON_RUNTIME_UTIL_H
#define PYSTON_RUNTIME_UTIL_H

#include "core/types.h"

namespace pyston {

class BoxedSlice;

void parseSlice(BoxedSlice* slice, int size, i64* out_start, i64* out_stop, i64* out_end, i64* out_length = nullptr);

template <typename T> void copySlice(T* __restrict__ dst, const T* __restrict__ src, i64 start, i64 step, i64 length) {
    assert(dst != src);
    if (step == 1) {
        memcpy(dst, &src[start], length * sizeof(T));
    } else {
        for (i64 curr = start, i = 0; i < length; curr += step, ++i)
            dst[i] = src[curr];
    }
}
}
#endif
