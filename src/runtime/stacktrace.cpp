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

#include "codegen/unwinding.h"
#include "core/options.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/traceback.h"
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
    raiseRaw(ExcInfo(exc_obj->cls, exc_obj, getTraceback()));
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, int col_offset, const std::string& file, const std::string& func) {
    Box* exc = runtimeCall(SyntaxError, ArgPassSpec(1), boxStrConstant(msg), NULL, NULL, NULL, NULL);

    auto tb = getTraceback();
    std::vector<const LineInfo*> entries = tb->lines;
    entries.push_back(new LineInfo(lineno, col_offset, file, func));
    raiseRaw(ExcInfo(exc->cls, exc, new BoxedTraceback(std::move(entries))));
}

void raiseSyntaxErrorHelper(const std::string& file, const std::string& func, AST* node_at, const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);

    char buf[1024];
    vsnprintf(buf, sizeof(buf), msg, ap);


    // TODO I'm not sure that it's safe to raise an exception here, since I think
    // there will be things that end up not getting cleaned up.
    // Then again, there are a huge number of things that don't get cleaned up even
    // if an exception doesn't get thrown...

    // TODO output is still a little wrong, should be, for example
    //
    //  File "../test/tests/future_non_existent.py", line 1
    //    from __future__ import rvalue_references # should cause syntax error
    //
    // but instead it is
    //
    // Traceback (most recent call last):
    //  File "../test/tests/future_non_existent.py", line -1, in :
    //    from __future__ import rvalue_references # should cause syntax error
    raiseSyntaxError(buf, node_at->lineno, node_at->col_offset, file, "");
}

void _printStacktrace() {
    printTraceback(getTraceback());
}

// where should this go...
extern "C" void abort() {
    static void (*libc_abort)() = (void (*)())dlsym(RTLD_NEXT, "abort");

    // In case something calls abort down the line:
    static bool recursive = false;
    // If object_cls is NULL, then we somehow died early on, and won't be able to display a traceback.
    if (!recursive && object_cls) {
        recursive = true;

        fprintf(stderr, "Someone called abort!\n");

        // If we call abort(), things may be seriously wrong.  Set an alarm() to
        // try to handle cases that we would just hang.
        // (Ex if we abort() from a static constructor, and _printStackTrace uses
        // that object, _printStackTrace will hang waiting for the first construction
        // to finish.)
        alarm(1);
        try {
            _printStacktrace();
        } catch (ExcInfo) {
            fprintf(stderr, "error printing stack trace during abort()");
        }

        // Cancel the alarm.
        // This is helpful for when running in a debugger, since the debugger will catch the
        // abort and let you investigate, but the alarm will still come back to kill the program.
        alarm(0);
    }

    if (PAUSE_AT_ABORT) {
        printf("PID %d about to call libc abort; pausing for a debugger...\n", getpid());
        while (true) {
            sleep(1);
        }
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

extern "C" void raise0() {
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

void ExcInfo::printExcAndTraceback() const {
    PyErr_Display(type, value, traceback);
}

bool ExcInfo::matches(BoxedClass* cls) const {
    assert(this->type);
    RELEASE_ASSERT(isSubclass(this->type->cls, type_cls), "throwing old-style objects not supported yet (%s)",
                   getTypeName(this->type));
    return isSubclass(static_cast<BoxedClass*>(this->type), cls);
}

// takes the three arguments of a `raise' and produces the ExcInfo to throw
ExcInfo excInfoForRaise(Box* exc_cls, Box* exc_val, Box* exc_tb) {
    assert(exc_cls && exc_val && exc_tb); // use None for default behavior, not nullptr
    // TODO switch this to PyErr_Normalize

    if (exc_tb == None)
        exc_tb = getTraceback();

    if (isSubclass(exc_cls->cls, type_cls)) {
        BoxedClass* c = static_cast<BoxedClass*>(exc_cls);
        if (isSubclass(c, BaseException)) {
            Box* exc_obj;

            if (isSubclass(exc_val->cls, BaseException)) {
                exc_obj = exc_val;
                c = exc_obj->cls;
            } else if (exc_val != None) {
                exc_obj = runtimeCall(c, ArgPassSpec(1), exc_val, NULL, NULL, NULL, NULL);
            } else {
                exc_obj = runtimeCall(c, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
            }

            return ExcInfo(c, exc_obj, exc_tb);
        }
    }

    if (isSubclass(exc_cls->cls, BaseException)) {
        if (exc_val != None)
            raiseExcHelper(TypeError, "instance exception may not have a separate value");
        return ExcInfo(exc_cls->cls, exc_cls, exc_tb);
    }

    raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not %s",
                   getTypeName(exc_cls));
}

extern "C" void raise3(Box* arg0, Box* arg1, Box* arg2) {
    raiseRaw(excInfoForRaise(arg0, arg1, arg2));
}

void raiseExcHelper(BoxedClass* cls, Box* arg) {
    Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    raiseExc(exc_obj);
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
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), message, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    } else {
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    }
}
}
