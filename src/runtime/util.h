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
#include "runtime/types.h"

namespace pyston {

class BoxedSlice;

void parseSlice(BoxedSlice* slice, int size, i64* out_start, i64* out_stop, i64* out_end, i64* out_length = nullptr);

// Analogue of _PyEval_SliceIndex
inline void sliceIndex(Box* b, int64_t* out) {
    if (b->cls == none_cls) {
        // Leave default value in case of None (useful for slices like [2:])
        return;
    }

    int ret = _PyEval_SliceIndex(b, out);
    if (ret <= 0)
        throwCAPIException();
}

bool isSliceIndex(Box* b);

void adjustNegativeIndicesOnObject(Box* obj, i64* start, i64* stop);

// Adjust the start and stop bounds of the sequence we are slicing to its size.
// Ensure stop >= start and remain within bounds.
void boundSliceWithLength(i64* start_out, i64* stop_out, i64 start, i64 stop, i64 size);

Box* noneIfNull(Box* b);
Box* boxStringOrNone(const char* s);
Box* boxStringFromCharPtr(const char* s);

// This function will ascii-encode any unicode objects it gets passed, or return the argument
// unmodified if it wasn't a unicode object.
// This is intended for functions that deal with attribute or variable names, which we internally
// assume will always be strings, but CPython lets be unicode.
// If we used an encoding like utf8 instead of ascii, we would allow collisions between unicode
// strings and a string that happens to be its encoding.  It seems safer to just encode as ascii,
// which will throw an exception if you try to pass something that might run into this risk.
// (We wrap the unicode error and throw a TypeError)
Box* coerceUnicodeToStr(Box* unicode);

extern "C" bool hasnext(Box* o);
extern "C" void dump(void* p);
extern "C" void dumpEx(void* p, int levels = 0);

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
