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

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#ifndef LIBUNWIND_PYSTON_PATCH_VERSION
#error "Please use a patched version of libunwind; see docs/INSTALLING.md"
#elif LIBUNWIND_PYSTON_PATCH_VERSION != 0x01
#error "Please repatch your version of libunwind; see docs/INSTALLING.md"
#endif


#include "core/options.h"

#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

// from http://www.nongnu.org/libunwind/man/libunwind(3).html
void showBacktrace() {
    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t ip, sp;

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);

    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        unw_get_reg(&cursor, UNW_REG_SP, &sp);
        printf("ip = %lx, sp = %lx\n", (long)ip, (long)sp);

        std::string py_info = getPythonFuncAt((void*)ip, (void*)sp);
        if (py_info.size()) {
            printf("Which is: %s\n", py_info.c_str());
        }
    }
}

void raiseExc() {
    if (VERBOSITY())
        showBacktrace();
    // if (VERBOSITY()) raise(SIGTRAP);
    if (VERBOSITY())
        abort();
    exit(1);
}
}
