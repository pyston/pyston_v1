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

#include <algorithm>
#include <cstdarg>
#include <dlfcn.h>

#include "llvm/DebugInfo/DIContext.h"

#include "codegen/unwinding.h"
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

static std::vector<const LineInfo*> last_tb;

void raiseRaw(const ExcInfo& e) __attribute__((__noreturn__));
void raiseRaw(const ExcInfo& e) {
    // Should set these to None before getting here:
    assert(e.type);
    assert(e.value);
    assert(e.traceback);

    // Using libgcc:
    throw e;

    // Using libunwind
    // unwindExc(exc_obj);
}

void raiseExc(Box* exc_obj) {
    auto entries = getTracebackEntries();
    last_tb = std::move(entries);

    raiseRaw(ExcInfo(exc_obj->cls, exc_obj, None));
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, int col_offset, const std::string& file, const std::string& func) {
    Box* exc = exceptionNew2(SyntaxError, boxStrConstant(msg));

    auto entries = getTracebackEntries();
    last_tb = std::move(entries);
    // TODO: leaks this!
    last_tb.push_back(new LineInfo(lineno, col_offset, file, func));

    raiseRaw(ExcInfo(exc->cls, exc, None));
}

static void _printTraceback(const std::vector<const LineInfo*>& tb) {
    fprintf(stderr, "Traceback (most recent call last):\n");

    for (auto line : tb) {
        fprintf(stderr, "  File \"%s\", line %d, in %s:\n", line->file.c_str(), line->line, line->func.c_str());

        if (line->line < 0)
            continue;

        FILE* f = fopen(line->file.c_str(), "r");
        if (f) {
            assert(line->line < 10000000 && "Refusing to try to seek that many lines forward");
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

void printLastTraceback() {
    _printTraceback(last_tb);
}

void _printStacktrace() {
    _printTraceback(getTracebackEntries());
}

// where should this go...
extern "C" void abort() {
    static void (*libc_abort)() = (void (*)())dlsym(RTLD_NEXT, "abort");

    // In case something calls abort down the line:
    static bool recursive = false;
    if (!recursive) {
        recursive = true;

        fprintf(stderr, "Someone called abort!\n");

        _printStacktrace();
    }

    libc_abort();
    __builtin_unreachable();
}

extern "C" void exit(int code) {
    static void (*libc_exit)(int) = (void (*)(int))dlsym(RTLD_NEXT, "exit");

    if (code == 0) {
        libc_exit(0);
        __builtin_unreachable();
    }

    fprintf(stderr, "Someone called exit with code=%d!\n", code);

    // In case something calls exit down the line:
    static bool recursive = false;
    if (!recursive) {
        recursive = true;

        _printStacktrace();
    }

    libc_exit(code);
    __builtin_unreachable();
}

void raise0() {
    ExcInfo* exc_info = getFrameExcInfo();
    assert(exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (exc_info->type == None)
        raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not NoneType");

    raiseRaw(*exc_info);
}

#ifndef NDEBUG
ExcInfo::ExcInfo(Box* type, Box* value, Box* traceback) : type(type), value(value), traceback(traceback) {
    if (this->type && this->type != None)
        RELEASE_ASSERT(isSubclass(this->type->cls, type_cls), "throwing old-style objects not supported yet (%s)",
                       getTypeName(this->type));
}
#endif

bool ExcInfo::matches(BoxedClass* cls) const {
    assert(this->type);
    RELEASE_ASSERT(isSubclass(this->type->cls, type_cls), "throwing old-style objects not supported yet (%s)",
                   getTypeName(this->type));
    return isSubclass(static_cast<BoxedClass*>(this->type), cls);
}

void raise3(Box* arg0, Box* arg1, Box* arg2) {
    // TODO switch this to PyErr_Normalize

    if (isSubclass(arg0->cls, type_cls)) {
        BoxedClass* c = static_cast<BoxedClass*>(arg0);
        if (isSubclass(c, BaseException)) {
            Box* exc_obj;
            if (arg1 != None)
                exc_obj = exceptionNew2(c, arg1);
            else
                exc_obj = exceptionNew1(c);

            raiseRaw(ExcInfo(c, exc_obj, arg2));
        }
    }

    if (isSubclass(arg0->cls, BaseException)) {
        if (arg1 != None)
            raiseExcHelper(TypeError, "instance exception may not have a separate value");
        raiseRaw(ExcInfo(arg0->cls, arg0, arg2));
    }

    raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not %s",
                   getTypeName(arg0));
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
    std::string name = getTypeName(b);

    BoxedString* r = strOrNull(b);
    if (!r)
        return name;

    assert(r->cls == str_cls);
    const std::string* msg = &r->s;
    if (msg->size())
        return name + ": " + *msg;
    return name;
}
}
