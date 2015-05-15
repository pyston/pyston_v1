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

namespace pyston {

void parseSlice(BoxedSlice* slice, int size, i64* out_start, i64* out_stop, i64* out_step, i64* out_length) {
    int ret = PySlice_GetIndicesEx((PySliceObject*)slice, size, out_start, out_stop, out_step, out_length);
    if (ret == -1)
        throwCAPIException();
}

bool isSliceIndex(Box* b) {
    return b->cls == none_cls || b->cls == int_cls || PyIndex_Check(b);
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
