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

#include "gc/root_finder.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <vector>

#include "codegen/codegen.h"
#include "codegen/llvm_interpreter.h"
#include "core/common.h"
#include "core/threading.h"
#include "gc/collector.h"
#include "gc/heap.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

extern "C" void __libc_start_main();

namespace pyston {
namespace gc {

void collectRoots(void* start, void* end, TraceStack* stack) {
    assert(start <= end);

    TraceStackGCVisitor(stack).visitPotentialRange((void**)start, (void**)end);
}


void collectOtherThreadsStacks(TraceStack* stack) {
    std::vector<threading::ThreadState> threads = threading::getAllThreadStates();

    for (threading::ThreadState& tstate : threads) {
        collectRoots(tstate.stack_start, tstate.stack_end, stack);
        collectRoots(&tstate.ucontext, (&tstate.ucontext) + 1, stack);
    }
}

static void collectLocalStack(TraceStack* stack) {
    // force callee-save registers onto the stack:
    // Actually, I feel like this is pretty brittle:
    // collectLocalStack itself is allowed to save the callee-save registers
    // on its own stack.
    jmp_buf registers __attribute__((aligned(sizeof(void*))));

    setjmp(registers);

    assert(sizeof(registers) % 8 == 0);
    // void* stack_bottom = __builtin_frame_address(0);
    collectRoots(&registers, (&registers) + 1, stack);

    void* stack_bottom = threading::getStackBottom();
#if STACK_GROWS_DOWN
    collectRoots(&registers, stack_bottom, stack);
#else
    collectRoots(stack_bottom, &registers + 1, stack);
#endif
}

void collectStackRoots(TraceStack* stack) {
    collectLocalStack(stack);
    collectOtherThreadsStacks(stack);

    TraceStackGCVisitor visitor(stack);
    gatherInterpreterRoots(&visitor);
}
}
}
