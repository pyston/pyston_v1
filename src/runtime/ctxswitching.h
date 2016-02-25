// Copyright (c) 2014-2016 Dropbox, Inc.
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

#ifndef PYSTON_RUNTIME_CTXSWITCHING_H
#define PYSTON_RUNTIME_CTXSWITCHING_H

#include <cstdint>

namespace pyston {

struct Context {
    int64_t r12, r13, r14, r15, rbx, rbp, rip;
};

static_assert(sizeof(Context) == 8 * 7, "");

extern "C" Context* makeContext(void* stack_top, void (*start_func)(intptr_t));
extern "C" void swapContext(Context** old_context, Context* new_context, intptr_t arg);
}

#endif
