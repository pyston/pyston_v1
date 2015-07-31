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
#include "core/ast.h"
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

void raiseExc(Box* exc_obj) {
    assert(!PyErr_Occurred());
    throw ExcInfo(exc_obj->cls, exc_obj, None);
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, int col_offset, llvm::StringRef file, llvm::StringRef func) {
    Box* exc = runtimeCall(SyntaxError, ArgPassSpec(1), boxString(msg), NULL, NULL, NULL, NULL);

    auto tb = new BoxedTraceback(LineInfo(lineno, col_offset, file, func), None);
    assert(!PyErr_Occurred());
    throw ExcInfo(exc->cls, exc, tb);
}

void raiseSyntaxErrorHelper(llvm::StringRef file, llvm::StringRef func, AST* node_at, const char* msg, ...) {
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
    static bool recursive = false;

    if (recursive) {
        fprintf(stderr, "_printStacktrace ran into an issue; refusing to try it again!\n");
        return;
    }

    recursive = true;
    printTraceback(getTraceback());
    recursive = false;
}

// where should this go...
extern "C" void abort() {
    static void (*libc_abort)() = (void (*)())dlsym(RTLD_NEXT, "abort");

    // In case displaying the traceback recursively calls abort:
    static bool recursive = false;

    if (!recursive) {
        recursive = true;
        Stats::dump();
        fprintf(stderr, "Someone called abort!\n");

        // If traceback_cls is NULL, then we somehow died early on, and won't be able to display a traceback.
        if (traceback_cls) {

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
            // This is helpful for when running in a debugger, since otherwise the debugger will catch the
            // abort and let you investigate, but the alarm will still come back to kill the program.
            alarm(0);
        }
    }

    if (PAUSE_AT_ABORT) {
        fprintf(stderr, "PID %d about to call libc abort; pausing for a debugger...\n", getpid());

        // Sometimes stderr isn't available (or doesn't immediately appear), so write out a file
        // just in case:
        FILE* f = fopen("pausing.txt", "w");
        if (f) {
            fprintf(f, "PID %d about to call libc abort; pausing for a debugger...\n", getpid());
            fclose(f);
        }

        while (true) {
            sleep(1);
        }
    }
    libc_abort();
    __builtin_unreachable();
}

#if 0
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
#endif

extern "C" void raise0() {
    ExcInfo* exc_info = getFrameExcInfo();
    assert(exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (exc_info->type == None)
        raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not NoneType");

    exc_info->reraise = true;
    assert(!PyErr_Occurred());
    throw * exc_info;
}

#ifndef NDEBUG
ExcInfo::ExcInfo(Box* type, Box* value, Box* traceback)
    : type(type), value(value), traceback(traceback), reraise(false) {
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
ExcInfo excInfoForRaise(Box* type, Box* value, Box* tb) {
    assert(type && value && tb); // use None for default behavior, not nullptr
    // TODO switch this to PyErr_Normalize

    if (tb == None) {
        tb = NULL;
    } else if (tb != NULL && !PyTraceBack_Check(tb)) {
        raiseExcHelper(TypeError, "raise: arg 3 must be a traceback or None");
    }


    /* Next, repeatedly, replace a tuple exception with its first item */
    while (PyTuple_Check(type) && PyTuple_Size(type) > 0) {
        PyObject* tmp = type;
        type = PyTuple_GET_ITEM(type, 0);
        Py_INCREF(type);
        Py_DECREF(tmp);
    }

    if (PyExceptionClass_Check(type)) {
        PyErr_NormalizeException(&type, &value, &tb);

        if (!PyExceptionInstance_Check(value)) {
            raiseExcHelper(TypeError, "calling %s() should have returned an instance of "
                                      "BaseException, not '%s'",
                           ((PyTypeObject*)type)->tp_name, Py_TYPE(value)->tp_name);
        }
    } else if (PyExceptionInstance_Check(type)) {
        /* Raising an instance.  The value should be a dummy. */
        if (value != Py_None) {
            raiseExcHelper(TypeError, "instance exception may not have a separate value");
        } else {
            /* Normalize to raise <class>, <instance> */
            Py_DECREF(value);
            value = type;
            type = PyExceptionInstance_Class(type);
            Py_INCREF(type);
        }
    } else {
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        raiseExcHelper(TypeError, "exceptions must be old-style classes or "
                                  "derived from BaseException, not %s",
                       type->cls->tp_name);
    }

    assert(PyExceptionClass_Check(type));

    if (tb == NULL) {
        tb = None;
    }

    return ExcInfo(type, value, tb);
}

extern "C" void raise3(Box* arg0, Box* arg1, Box* arg2) {
    bool reraise = arg2 != NULL && arg2 != None;
    auto exc_info = excInfoForRaise(arg0, arg1, arg2);

    exc_info.reraise = reraise;
    assert(!PyErr_Occurred());
    throw exc_info;
}

extern "C" void raise3_capi(Box* arg0, Box* arg1, Box* arg2) noexcept {
    bool reraise = arg2 != NULL && arg2 != None;

    ExcInfo exc_info(NULL, NULL, NULL);
    try {
        exc_info = excInfoForRaise(arg0, arg1, arg2);
        exc_info.reraise = reraise;
    } catch (ExcInfo e) {
        exc_info = e;
    }

    assert(!exc_info.reraise); // would get thrown away
    PyErr_Restore(exc_info.type, exc_info.value, exc_info.traceback);
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

        BoxedString* message = boxString(buf);
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), message, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    } else {
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(0), NULL, NULL, NULL, NULL, NULL);
        raiseExc(exc_obj);
    }
}
}
