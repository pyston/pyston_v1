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

#include <algorithm>
#include <cstdarg>
#include <dlfcn.h>

#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/options.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

void raiseExc(STOLEN(Box*) exc_obj) {
    assert(!PyErr_Occurred());
    throw ExcInfo(incref(exc_obj->cls), exc_obj, NULL);
}

// Have a special helper function for syntax errors, since we want to include the location
// of the syntax error in the traceback, even though it is not part of the execution:
void raiseSyntaxError(const char* msg, int lineno, llvm::StringRef file, llvm::StringRef func, bool compiler_error) {
    if (compiler_error) {
        // This is how CPython's compiler_error() works:
        assert(file.data()[file.size()] == '\0');
        Box* loc = PyErr_ProgramText(file.data(), lineno);
        if (!loc) {
            Py_INCREF(Py_None);
            loc = Py_None;
        }
        AUTO_DECREF(loc);

        auto args = BoxedTuple::create({ autoDecref(boxString(file)), autoDecref(boxInt(lineno)), Py_None, loc });
        AUTO_DECREF(args);
        Box* exc = runtimeCall(SyntaxError, ArgPassSpec(2), autoDecref(boxString(msg)), args, NULL, NULL, NULL);
        assert(!PyErr_Occurred());
        throw ExcInfo(incref(exc->cls), exc, NULL);
    } else {
        PyErr_SetString(SyntaxError, msg);
        PyErr_SyntaxLocation(file.str().c_str(), lineno);
        throwCAPIException();
    }
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
    raiseSyntaxError(buf, node_at->lineno, file, "");
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
ExcInfo excInfoForRaise(STOLEN(Box*) type, STOLEN(Box*) value, STOLEN(Box*) tb) {
    assert(type && value && tb); // use None for default behavior, not nullptr
    // TODO switch this to PyErr_Normalize

    if (tb == Py_None) {
        Py_DECREF(tb);
        tb = NULL;
    } else if (tb != NULL && !PyTraceBack_Check(tb)) {
        Py_DECREF(type);
        Py_DECREF(value);
        Py_XDECREF(tb);
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
            Py_DECREF(type);
            Py_DECREF(value);
            Py_XDECREF(tb);
            raiseExcHelper(TypeError, "calling %s() should have returned an instance of "
                                      "BaseException, not '%s'",
                           ((PyTypeObject*)type)->tp_name, Py_TYPE(value)->tp_name);
        }
    } else if (PyExceptionInstance_Check(type)) {
        /* Raising an instance.  The value should be a dummy. */
        if (value != Py_None) {
            Py_DECREF(type);
            Py_DECREF(value);
            Py_XDECREF(tb);
            raiseExcHelper(TypeError, "instance exception may not have a separate value");
        } else {
            /* Normalize to raise <class>, <instance> */
            Py_DECREF(value);
            value = type;
            type = PyExceptionInstance_Class(type);
            Py_INCREF(type);
        }
    } else {
        Py_DECREF(type);
        Py_DECREF(value);
        Py_XDECREF(tb);
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        raiseExcHelper(TypeError, "exceptions must be old-style classes or "
                                  "derived from BaseException, not %s",
                       type->cls->tp_name);
    }

    assert(PyExceptionClass_Check(type));

    return ExcInfo(type, value, tb);
}

extern "C" void raise0(ExcInfo* frame_exc_info) {
    updateFrameExcInfoIfNeeded(frame_exc_info);
    assert(frame_exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (frame_exc_info->type == Py_None)
        raiseExcHelper(TypeError, "exceptions must be old-style classes or derived from BaseException, not NoneType");

    startReraise();
    assert(!PyErr_Occurred());

    Py_INCREF(frame_exc_info->type);
    Py_INCREF(frame_exc_info->value);
    Py_INCREF(frame_exc_info->traceback);
    throw * frame_exc_info;
}

extern "C" void raise0_capi(ExcInfo* frame_exc_info) noexcept {
    updateFrameExcInfoIfNeeded(frame_exc_info);
    assert(frame_exc_info->type);

    // TODO need to clean up when we call normalize, do_raise, etc
    if (frame_exc_info->type == Py_None) {
        PyErr_SetString(TypeError, "exceptions must be old-style classes or derived from BaseException, not NoneType");
        return;
    }

    startReraise();
    assert(!PyErr_Occurred());

    Py_INCREF(frame_exc_info->type);
    Py_INCREF(frame_exc_info->value);
    Py_INCREF(frame_exc_info->traceback);

    PyErr_Restore(frame_exc_info->type, frame_exc_info->value, frame_exc_info->traceback);
}

extern "C" void raise3(STOLEN(Box*) arg0, STOLEN(Box*) arg1, STOLEN(Box*) arg2) {
    bool reraise = arg2 != NULL && arg2 != Py_None;
    auto exc_info = excInfoForRaise(arg0, arg1, arg2);

    if (reraise)
        startReraise();

    if (PyErr_Occurred())
        PyErr_Clear();
    throw exc_info;
}

extern "C" void raise3_capi(STOLEN(Box*) arg0, STOLEN(Box*) arg1, STOLEN(Box*) arg2) noexcept {
    bool reraise = arg2 != NULL && arg2 != Py_None;

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
        Box* exc_obj = runtimeCall(cls, ArgPassSpec(1), autoDecref(message), NULL, NULL, NULL, NULL);
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

extern "C" void caughtCapiException() {
    PyThreadState* tstate = PyThreadState_GET();
    exceptionAtLine(&tstate->curexc_traceback);
}

extern "C" void reraiseCapiExcAsCxx() {
    ensureCAPIExceptionSet();
    // TODO: we are normalizing to many times?
    Box* type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    ExcInfo e = excInfoForRaise(type, value, tb);
    startReraise();
    throw e;
}

extern "C" void rawReraise(Box* type, Box* value, Box* tb) {
    startReraise();
    throw ExcInfo(type, value, tb);
}

void caughtCxxException(ExcInfo* exc_info) {
    static StatCounter frames_unwound("num_frames_unwound_python");
    frames_unwound.log();

    exceptionAtLine(&exc_info->traceback);
}

struct ExcState {
    bool is_reraise;
    constexpr ExcState() : is_reraise(false) {}
} static __thread exc_state;

bool& getIsReraiseFlag() {
    return exc_state.is_reraise;
}

void exceptionAtLine(Box** traceback) {
    if (!getIsReraiseFlag())
        PyTraceBack_Here_Tb((struct _frame*)getFrame((FrameInfo*)cur_thread_state.frame_info),
                            (PyTracebackObject**)traceback);
    else
        getIsReraiseFlag() = false;
}
}
