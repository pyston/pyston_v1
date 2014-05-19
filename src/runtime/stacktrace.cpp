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


#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

#include "core/options.h"

#include <stdarg.h>

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

// Currently-unused libunwind-based unwinding:
void unwindExc(Box* exc_obj) __attribute__((noreturn));
void unwindExc(Box* exc_obj) {
    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t ip, sp;

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);

    int code;
    unw_proc_info_t pip;

    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        unw_get_reg(&cursor, UNW_REG_SP, &sp);
        printf("ip = %lx, sp = %lx\n", (long)ip, (long)sp);

        code = unw_get_proc_info(&cursor, &pip);
        RELEASE_ASSERT(code == 0, "");

        // printf("%lx %lx %lx %lx %lx %lx %d %d %p\n", pip.start_ip, pip.end_ip, pip.lsda, pip.handler, pip.gp,
        // pip.flags, pip.format, pip.unwind_info_size, pip.unwind_info);

        assert((pip.lsda == 0) == (pip.handler == 0));
        assert(pip.flags == 0);

        if (pip.handler == 0) {
            if (VERBOSITY())
                printf("Skipping frame without handler\n");

            continue;
        }

        printf("%lx %lx %lx\n", pip.lsda, pip.handler, pip.flags);
        // assert(pip.handler == (uintptr_t)__gxx_personality_v0 || pip.handler == (uintptr_t)__py_personality_v0);

        // auto handler_fn = (int (*)(int, int, uint64_t, void*, void*))pip.handler;
        ////handler_fn(1, 1 /* _UA_SEARCH_PHASE */, 0 /* exc_class */, NULL, NULL);
        // handler_fn(2, 2 /* _UA_SEARCH_PHASE */, 0 /* exc_class */, NULL, NULL);
        unw_set_reg(&cursor, UNW_REG_IP, 1);

        // TODO testing:
        // unw_resume(&cursor);
    }

    abort();
}

void raiseExc(Box* exc_obj) {
    // Using libgcc:
    throw exc_obj;

    // Using libunwind
    // unwindExc(exc_obj);

    abort();
}

void raiseExcHelper(BoxedClass* cls, const char* msg, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, msg);

        char buf[1024];
        vsnprintf(buf, sizeof(buf), msg, ap);

        va_end(ap);

        BoxedString* message = boxStrConstant(buf);
        Box* exc_obj = exceptionNew2(cls, message);
        raiseExc(exc_obj);
    } else {
        Box* exc_obj = exceptionNew1(cls);
        raiseExc(exc_obj);
    }
}

std::string formatException(Box* b) {
    const std::string* name = getTypeName(b);
    HCBox* hcb = static_cast<HCBox*>(b);

    Box* attr = hcb->peekattr("message");
    if (attr == nullptr)
        return *name;

    Box* r;
    try {
        r = str(attr);
    }
    catch (Box* b) {
        return *name;
    }

    assert(r->cls == str_cls);
    const std::string* msg = &static_cast<BoxedString*>(r)->s;
    return *name + ": " + *msg;
}
}
