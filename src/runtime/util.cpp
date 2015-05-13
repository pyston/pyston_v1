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

#include "runtime/util.h"

#include "core/options.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

void parseSlice(BoxedSlice* slice, int size, i64* out_start, i64* out_stop, i64* out_step, i64* out_length) {
    int ret = PySlice_GetIndicesEx((PySliceObject*)slice, size, out_start, out_stop, out_step, out_length);
    if (ret == -1)
        throwCAPIException();
}

void sliceIndex(Box* b, int64_t* out) {
    if (b->cls == none_cls) {
        // Leave default value in case of None (useful for slices like [2:])
    } else if (b->cls == int_cls) {
        *out = static_cast<BoxedInt*>(b)->n;
    } else if (PyIndex_Check(b)) {
        int64_t x = PyNumber_AsSsize_t(b, NULL);
        if (!(x == -1 && PyErr_Occurred()))
            *out = x;
    } else {
        raiseExcHelper(TypeError, "slice indices must be integers or "
                                  "None or have an __index__ method");
    }
}

void boundSliceWithLength(i64* start_out, i64* stop_out, i64 start, i64 stop, i64 size) {
    // Logic from PySequence_GetSlice:
    if (start < 0)
        start += size;
    if (stop < 0)
        stop += size;

    if (start < 0)
        start = 0;
    else if (start > size)
        start = size;

    if (stop < start)
        stop = start;
    else if (stop > size)
        stop = size;

    assert(0 <= start && start <= stop && stop <= size);

    *start_out = start;
    *stop_out = stop;
}
}
