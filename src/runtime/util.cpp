// Copyright (c) 2014 Dropbox, Inc.
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
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

void parseSlice(BoxedSlice* slice, int size, i64* out_start, i64* out_stop, i64* out_step) {
    BoxedSlice* sslice = static_cast<BoxedSlice*>(slice);

    Box* start = sslice->start;
    assert(start);
    Box* stop = sslice->stop;
    assert(stop);
    Box* step = sslice->step;
    assert(step);

    RELEASE_ASSERT(start->cls == int_cls || start->cls == none_cls, "");
    RELEASE_ASSERT(stop->cls == int_cls || stop->cls == none_cls, "");
    RELEASE_ASSERT(step->cls == int_cls || step->cls == none_cls, "");

    int64_t istart;
    int64_t istop;
    int64_t istep = 1;

    if (step->cls == int_cls) {
        istep = static_cast<BoxedInt*>(step)->n;
    }

    if (start->cls == int_cls) {
        istart = static_cast<BoxedInt*>(start)->n;
        if (istart < 0)
            istart = size + istart;
    } else {
        if (istep > 0)
            istart = 0;
        else
            istart = size - 1;
    }
    if (stop->cls == int_cls) {
        istop = static_cast<BoxedInt*>(stop)->n;
        if (istop < 0)
            istop = size + istop;
    } else {
        if (istep > 0)
            istop = size;
        else
            istop = -1;
    }

    if (istep == 0) {
        fprintf(stderr, "ValueError: slice step cannot be zero\n");
        raiseExcHelper(ValueError, "");
    }

    if (istep > 0) {
        if (istart < 0)
            istart = 0;
        if (istop > size)
            istop = size;
    } else {
        if (istart >= size)
            istart = size - 1;
        if (istop < 0)
            istop = -1;
    }

    *out_start = istart;
    *out_stop = istop;
    *out_step = istep;
}
}
