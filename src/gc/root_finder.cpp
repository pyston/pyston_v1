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

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include <cstring>
#include <setjmp.h>
#include <cstdio>
#include <cstdlib>
#include <cassert>
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

    void** cur = (void**)start;
    while (cur < end) {
        void* p = global_heap.getAllocationFromInteriorPointer(*cur);
        if (p)
            stack->push(p);
        cur++;
    }
}


static void _unwindStack(unw_cursor_t* cursor, TraceStack* stack) {
    TraceStackGCVisitor visitor(stack);

    unw_word_t ip, sp, bp;
#ifndef NVALGRIND
    if (RUNNING_ON_VALGRIND) {
        memset(&ip, 0, sizeof(ip));
        memset(&sp, 0, sizeof(sp));
        memset(&bp, 0, sizeof(bp));
    }
#endif


    int code;
    while (true) {
        int code = unw_step(cursor);
        // Negative codes are errors, zero means that there isn't a new frame.
        RELEASE_ASSERT(code >= 0 && "something broke unwinding!", "%d '%s'", code, unw_strerror(code));
        RELEASE_ASSERT(code != 0, "didn't get to the top of the stack!");

        unw_get_reg(cursor, UNW_REG_IP, &ip);
        unw_get_reg(cursor, UNW_REG_SP, &sp);
        unw_get_reg(cursor, UNW_TDEP_BP, &bp);

        void* cur_sp = (void*)sp;
        void* cur_bp = (void*)bp;

        // std::string name = g.func_addr_registry.getFuncNameAtAddress((void*)ip, true);

        unw_proc_info_t pip;
        unw_get_proc_info(cursor, &pip);

        // if (VERBOSITY()) printf("ip = 0x%lx (start_ip = 0x%lx), stack = [%p, %p)\n", (long) ip, pip.start_ip, cur_sp,
        // cur_bp);

        if (pip.start_ip == (uintptr_t)&__libc_start_main) {
            break;
        }

        if (pip.start_ip == (intptr_t)interpretFunction) {
            // TODO Do we still need to crawl the interpreter itself?
            gatherInterpreterRootsForFrame(&visitor, cur_bp);
        }

        collectRoots(cur_sp, (char*)cur_bp, stack);

        if (pip.start_ip == threading::call_frame_base) {
            break;
        }

        if (cur_bp == NULL) {
            // TODO I think this indicates an unwind mistake by libunwind?  Not sure.
            // But if it returns cur_bp=NULL, this is probably just a thread where libunwind
            // didn't reconstruct the call stack exactly the way we thought.
            // TODO we probably don't need to do any unwinding here at all; we can just track
            // the stack min and max for every thread.
            break;
        }
    }
}

void collectOtherThreadsStacks(TraceStack* stack) {
    std::vector<threading::ThreadState> threads = threading::getAllThreadStates();

    // unw_addr_space_t as = getOtherAddrSpace();

    for (threading::ThreadState& tstate : threads) {
        unw_cursor_t cursor;
        // int code = unw_init_remote(&cursor, as, &tstate);
        int code = unw_init_local(&cursor, (ucontext_t*)&tstate.ucontext);
        assert(code == 0);

        // printf("Collecting thread %d\n", tstate.tid);

        collectRoots(&tstate.ucontext, (&tstate.ucontext) + 1, stack);

        _unwindStack(&cursor, stack);
    }
}

static void collectLocalStack(TraceStack* stack) {
    unw_cursor_t cursor;
    unw_context_t uc;

    // force callee-save registers onto the stack:
    // Actually, I feel like this is pretty brittle:
    // collectLocalStack itself is allowed to save the callee-save registers
    // on its own stack.
    jmp_buf registers __attribute__((aligned(sizeof(void*))));

#ifndef NVALGRIND
    if (RUNNING_ON_VALGRIND) {
        memset(&registers, 0, sizeof(registers));
        memset(&cursor, 0, sizeof(cursor));
        memset(&uc, 0, sizeof(uc));
    }
#endif

    setjmp(registers);

    assert(sizeof(registers) % 8 == 0);
    // void* stack_bottom = __builtin_frame_address(0);
    collectRoots(&registers, &registers + 1, stack);

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);

    _unwindStack(&cursor, stack);
}

void collectStackRoots(TraceStack* stack) {
    collectLocalStack(stack);
    collectOtherThreadsStacks(stack);
}
}
}
