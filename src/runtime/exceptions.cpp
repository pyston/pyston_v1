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
#include "runtime/objmodel.h"
#include "runtime/traceback.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

void raiseExc(Box* exc_obj) {
    assert(!PyErr_Occurred());
    throw ExcInfo(exc_obj->cls, exc_obj, None);
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, int col_offset, llvm::StringRef file, llvm::StringRef func,
                      bool compiler_error) {
    Box* exc;
    Box* tb = None;
    if (compiler_error) {
        // This is how CPython's compiler_error() works:
        assert(file.data()[file.size()] == '\0');
        Box* loc = PyErr_ProgramText(file.data(), lineno);
        if (!loc) {
            Py_INCREF(Py_None);
            loc = Py_None;
        }

        auto args = BoxedTuple::create({ boxString(file), boxInt(lineno), None, loc });
        exc = runtimeCall(SyntaxError, ArgPassSpec(2), boxString(msg), args, NULL, NULL, NULL);
    } else {
        // This is more like how the parser handles it:
        exc = runtimeCall(SyntaxError, ArgPassSpec(1), boxString(msg), NULL, NULL, NULL, NULL);
        tb = new BoxedTraceback(LineInfo(lineno, col_offset, boxString(file), boxString(func)), None);
    }

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

void ExcInfo::printExcAndTraceback() const {
    PyErr_Display(type, value, traceback);
}

bool ExcInfo::matches(BoxedClass* cls) const {
    assert(this->type);
    RELEASE_ASSERT(PyType_Check(this->type), "throwing old-style objects not supported yet (%s)",
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

extern "C" void raise0(ExcInfo* frame_exc_info) {
    updateFrameExcInfoIfNeeded(frame_exc_info);
    assert(frame_exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (frame_exc_info->type == None)
        raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not NoneType");

    startReraise();
    assert(!PyErr_Occurred());
    throw * frame_exc_info;
}

extern "C" void raise0_capi(ExcInfo* frame_exc_info) noexcept {
    updateFrameExcInfoIfNeeded(frame_exc_info);
    assert(frame_exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (frame_exc_info->type == None) {
        frame_exc_info->type = TypeError;
        frame_exc_info->value
            = boxString("exceptions must be old-style classes or derived from BaseException, not NoneType");
        frame_exc_info->traceback = None;
        PyErr_NormalizeException(&frame_exc_info->type, &frame_exc_info->value, &frame_exc_info->traceback);
    }

    startReraise();
    PyErr_Restore(frame_exc_info->type, frame_exc_info->value, frame_exc_info->traceback);
}

extern "C" void raise3(Box* arg0, Box* arg1, Box* arg2) {
    bool reraise = arg2 != NULL && arg2 != None;
    auto exc_info = excInfoForRaise(arg0, arg1, arg2);

    if (reraise)
        startReraise();

    assert(!PyErr_Occurred());
    throw exc_info;
}

extern "C" void raise3_capi(Box* arg0, Box* arg1, Box* arg2) noexcept {
    bool reraise = arg2 != NULL && arg2 != None;

    ExcInfo exc_info(NULL, NULL, NULL);
    try {
        exc_info = excInfoForRaise(arg0, arg1, arg2);
    } catch (ExcInfo e) {
        exc_info = e;
    }

    if (reraise)
        startReraise();

    PyErr_Restore(exc_info.type, exc_info.value, exc_info.traceback);
}

void raiseExcHelper(BoxedClass* cls, Box* arg) {
    assert(!PyErr_Occurred());
    Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), arg, NULL, NULL, NULL, NULL);
    raiseExc(exc_obj);
}

void raiseExcHelper(BoxedClass* cls, const char* msg, ...) {
    assert(!PyErr_Occurred());
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


void logException(ExcInfo* exc_info) {
#if STAT_EXCEPTIONS
    static StatCounter num_exceptions("num_exceptions");
    num_exceptions.log();

    std::string stat_name;
    if (PyType_Check(exc_info->type))
        stat_name = "num_exceptions_" + std::string(static_cast<BoxedClass*>(exc_info->type)->tp_name);
    else
        stat_name = "num_exceptions_" + std::string(exc_info->value->cls->tp_name);
    Stats::log(Stats::getStatCounter(stat_name));
#if STAT_EXCEPTIONS_LOCATION
    logByCurrentPythonLine(stat_name);
#endif
#endif
}

extern "C" void caughtCapiException(AST_stmt* stmt, void* _source_info) {
    SourceInfo* source = static_cast<SourceInfo*>(_source_info);
    PyThreadState* tstate = PyThreadState_GET();

    exceptionAtLine(LineInfo(stmt->lineno, stmt->col_offset, source->getFn(), source->getName()),
                    &tstate->curexc_traceback);
}

extern "C" void reraiseCapiExcAsCxx() {
    ensureCAPIExceptionSet();
    // TODO: we are normalizing to many times?
    ExcInfo e = excInfoForRaise(cur_thread_state.curexc_type, cur_thread_state.curexc_value,
                                cur_thread_state.curexc_traceback);
    PyErr_Clear();
    startReraise();
    throw e;
}

void caughtCxxException(LineInfo line_info, ExcInfo* exc_info) {
    static StatCounter frames_unwound("num_frames_unwound_python");
    frames_unwound.log();

    exceptionAtLine(line_info, &exc_info->traceback);
}



struct ExcState {
    bool is_reraise;
    constexpr ExcState() : is_reraise(false) {}
} static __thread exc_state;

bool exceptionAtLineCheck() {
    if (exc_state.is_reraise) {
        exc_state.is_reraise = false;
        return false;
    }
    return true;
}

void exceptionAtLine(LineInfo line_info, Box** traceback) {
    if (exceptionAtLineCheck())
        BoxedTraceback::here(line_info, traceback);
}

void startReraise() {
    assert(!exc_state.is_reraise);
    exc_state.is_reraise = true;
}
}
