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

#include <algorithm>
#include <cstdarg>

#include "llvm/DebugInfo/DIContext.h"

#include "codegen/codegen.h"
#include "codegen/llvm_interpreter.h"
#include "core/options.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>

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

static std::vector<const LineInfo*> getTracebackEntries();
static gc::StaticRootHandle last_exc;
static std::vector<const LineInfo*> last_tb;

void raiseRaw(Box* exc_obj) __attribute__((__noreturn__));
void raiseRaw(Box* exc_obj) {
    // Using libgcc:
    throw exc_obj;

    // Using libunwind
    // unwindExc(exc_obj);
}

void raiseExc(Box* exc_obj) __attribute__((__noreturn__));
void raiseExc(Box* exc_obj) {
    auto entries = getTracebackEntries();
    last_tb = std::move(entries);
    last_exc = exc_obj;

    raiseRaw(exc_obj);
}

void printLastTraceback() {
    fprintf(stderr, "Traceback (most recent call last):\n");

    for (auto line : last_tb) {
        fprintf(stderr, "  File \"%s\", line %d, in %s:\n", line->file.c_str(), line->line, line->func.c_str());

        FILE* f = fopen(line->file.c_str(), "r");
        if (f) {
            for (int i = 1; i < line->line; i++) {
                char* buf = NULL;
                size_t size;
                size_t r = getline(&buf, &size, f);
                if (r != -1)
                    free(buf);
            }
            char* buf = NULL;
            size_t size;
            size_t r = getline(&buf, &size, f);
            if (r != -1) {
                while (buf[r - 1] == '\n' or buf[r - 1] == '\r')
                    r--;

                char* ptr = buf;
                while (*ptr == ' ' || *ptr == '\t') {
                    ptr++;
                    r--;
                }

                fprintf(stderr, "    %.*s\n", (int)r, ptr);
                free(buf);
            }
        }
    }
}

static std::vector<const LineInfo*> getTracebackEntries() {
    std::vector<const LineInfo*> entries;

    unw_cursor_t cursor;
    unw_context_t uc;
    unw_word_t ip, bp;

    unw_getcontext(&uc);
    unw_init_local(&cursor, &uc);

    int code;
    unw_proc_info_t pip;

    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);

        const LineInfo* line = getLineInfoFor((uint64_t)ip);
        if (line) {
            entries.push_back(line);
        } else {
            unw_get_reg(&cursor, UNW_TDEP_BP, &bp);

            unw_proc_info_t pip;
            code = unw_get_proc_info(&cursor, &pip);
            RELEASE_ASSERT(code == 0, "%d", code);

            if (pip.start_ip == (intptr_t)interpretFunction) {
                line = getLineInfoForInterpretedFrame((void*)bp);
                assert(line);
                entries.push_back(line);
            }
        }
    }
    std::reverse(entries.begin(), entries.end());

    return entries;
}

void raise0() {
    raiseRaw(last_exc);
}

void raise1(Box* b) {
    if (b->cls == type_cls) {
        BoxedClass* c = static_cast<BoxedClass*>(b);
        if (isSubclass(c, Exception)) {
            auto exc_obj = exceptionNew1(c);
            raiseExc(exc_obj);
        } else {
            raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not %s",
                           getTypeName(b)->c_str());
        }
    }

    // TODO: should only allow throwing of old-style classes or things derived
    // from BaseException:
    raiseExc(b);
}

void raiseExcHelper(BoxedClass* cls, const char* msg, ...) {
    if (msg != NULL) {
        va_list ap;
        va_start(ap, msg);

        // printf("Raising: ");
        // vprintf(msg, ap);
        // printf("\n");
        // va_start(ap, msg);

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

    Box* attr = b->getattr("message");
    if (attr == nullptr)
        return *name;

    BoxedString* r = strOrNull(attr);
    if (!r)
        return *name;

    assert(r->cls == str_cls);
    const std::string* msg = &r->s;
    if (msg->size())
        return *name + ": " + *msg;
    return *name;
}
}
