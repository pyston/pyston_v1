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
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "core/common.h"
#include "core/types.h"
#include "core/util.h"
#include "gc/collector.h"
#include "runtime/capi.h"
#include "runtime/dict.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" PyObject* PyErr_SetFromErrnoWithFilenameObject(PyObject* exc, PyObject* filenameObject) {
    PyObject* v;
    // Pyston change: made const
    const char* s;
    int i = errno;
#ifdef PLAN9
    char errbuf[ERRMAX];
#endif
#ifdef MS_WINDOWS
    char* s_buf = NULL;
    char s_small_buf[28]; /* Room for "Windows Error 0xFFFFFFFF" */
#endif
#ifdef EINTR
    if (i == EINTR && PyErr_CheckSignals())
        return NULL;
#endif
#ifdef PLAN9
    rerrstr(errbuf, sizeof errbuf);
    s = errbuf;
#else
    if (i == 0)
        s = "Error"; /* Sometimes errno didn't get set */
    else
#ifndef MS_WINDOWS
        s = strerror(i);
#else
    {
        /* Note that the Win32 errors do not lineup with the
           errno error.  So if the error is in the MSVC error
           table, we use it, otherwise we assume it really _is_
           a Win32 error code
        */
        if (i > 0 && i < _sys_nerr) {
            s = _sys_errlist[i];
        } else {
            int len = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                        | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    NULL, /* no message source */
                                    i, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                    /* Default language */
                                    (LPTSTR)&s_buf, 0, /* size not used */
                                    NULL);             /* no args */
            if (len == 0) {
                /* Only ever seen this in out-of-mem
                   situations */
                sprintf(s_small_buf, "Windows Error 0x%X", i);
                s = s_small_buf;
                s_buf = NULL;
            } else {
                s = s_buf;
                /* remove trailing cr/lf and dots */
                while (len > 0 && (s[len - 1] <= ' ' || s[len - 1] == '.'))
                    s[--len] = '\0';
            }
        }
    }
#endif /* Unix/Windows */
#endif /* PLAN 9*/
    if (filenameObject != NULL)
        v = Py_BuildValue("(isO)", i, s, filenameObject);
    else
        v = Py_BuildValue("(is)", i, s);
    if (v != NULL) {
        PyErr_SetObject(exc, v);
        Py_DECREF(v);
    }
#ifdef MS_WINDOWS
    LocalFree(s_buf);
#endif
    return NULL;
}

extern "C" PyObject* PyErr_SetFromErrnoWithFilename(PyObject* exc, const char* filename) {
    PyObject* name = filename ? PyString_FromString(filename) : NULL;
    PyObject* result = PyErr_SetFromErrnoWithFilenameObject(exc, name);
    Py_XDECREF(name);
    return result;
}

#ifdef MS_WINDOWS
extern "C" PyObject* PyErr_SetFromErrnoWithUnicodeFilename(PyObject* exc, const Py_UNICODE* filename) {
    PyObject* name = filename ? PyUnicode_FromUnicode(filename, wcslen(filename)) : NULL;
    PyObject* result = PyErr_SetFromErrnoWithFilenameObject(exc, name);
    Py_XDECREF(name);
    return result;
}
#endif /* MS_WINDOWS */

extern "C" void PyErr_Fetch(PyObject** p_type, PyObject** p_value, PyObject** p_traceback) {
    PyThreadState* tstate = PyThreadState_GET();

    *p_type = tstate->curexc_type;
    *p_value = tstate->curexc_value;
    *p_traceback = tstate->curexc_traceback;

    tstate->curexc_type = NULL;
    tstate->curexc_value = NULL;
    tstate->curexc_traceback = NULL;
}

extern "C" PyObject* PyErr_SetFromErrno(PyObject* exc) {
    return PyErr_SetFromErrnoWithFilenameObject(exc, NULL);
}

/* Call when an exception has occurred but there is no way for Python
   to handle it.  Examples: exception in __del__ or during GC. */
extern "C" void PyErr_WriteUnraisable(PyObject* obj) {
    PyObject* f, *t, *v, *tb;
    PyErr_Fetch(&t, &v, &tb);
    f = PySys_GetObject("stderr");
    if (f != NULL) {
        PyFile_WriteString("Exception ", f);
        if (t) {
            PyObject* moduleName;
            const char* className;
            assert(PyExceptionClass_Check(t));
            className = PyExceptionClass_Name(t);
            if (className != NULL) {
                const char* dot = strrchr(className, '.');
                if (dot != NULL)
                    className = dot + 1;
            }

            moduleName = PyObject_GetAttrString(t, "__module__");
            if (moduleName == NULL)
                PyFile_WriteString("<unknown>", f);
            else {
                char* modstr = PyString_AsString(moduleName);
                if (modstr && strcmp(modstr, "exceptions") != 0) {
                    PyFile_WriteString(modstr, f);
                    PyFile_WriteString(".", f);
                }
            }
            if (className == NULL)
                PyFile_WriteString("<unknown>", f);
            else
                PyFile_WriteString(className, f);
            if (v && v != Py_None) {
                PyFile_WriteString(": ", f);
                PyFile_WriteObject(v, f, 0);
            }
            Py_XDECREF(moduleName);
        }
        PyFile_WriteString(" in ", f);
        PyFile_WriteObject(obj, f, 0);
        PyFile_WriteString(" ignored\n", f);
        PyErr_Clear(); /* Just in case */
    }
    Py_XDECREF(t);
    Py_XDECREF(v);
    Py_XDECREF(tb);
}

extern "C" void PyErr_Display(PyObject* exception, PyObject* value, PyObject* tb) {
    Py_FatalError("unimplemented");
}

static void handle_system_exit(void) noexcept {
    Py_FatalError("unimplemented");
}

extern "C" void PyErr_PrintEx(int set_sys_last_vars) {
    PyObject* exception, *v, *tb, *hook;

    if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
        handle_system_exit();
    }
    PyErr_Fetch(&exception, &v, &tb);
    if (exception == NULL)
        return;
    PyErr_NormalizeException(&exception, &v, &tb);
    if (exception == NULL)
        return;
    /* Now we know v != NULL too */
    if (set_sys_last_vars) {
        PySys_SetObject("last_type", exception);
        PySys_SetObject("last_value", v);
        PySys_SetObject("last_traceback", tb);
    }
    hook = PySys_GetObject("excepthook");
    if (hook && hook != Py_None) {
        PyObject* args = PyTuple_Pack(3, exception, v, tb ? tb : Py_None);
        PyObject* result = PyEval_CallObject(hook, args);
        if (result == NULL) {
            PyObject* exception2, *v2, *tb2;
            if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
                handle_system_exit();
            }
            PyErr_Fetch(&exception2, &v2, &tb2);
            PyErr_NormalizeException(&exception2, &v2, &tb2);
            /* It should not be possible for exception2 or v2
               to be NULL. However PyErr_Display() can't
               tolerate NULLs, so just be safe. */
            if (exception2 == NULL) {
                exception2 = Py_None;
                Py_INCREF(exception2);
            }
            if (v2 == NULL) {
                v2 = Py_None;
                Py_INCREF(v2);
            }
            if (Py_FlushLine())
                PyErr_Clear();
            fflush(stdout);
            PySys_WriteStderr("Error in sys.excepthook:\n");
            PyErr_Display(exception2, v2, tb2);
            PySys_WriteStderr("\nOriginal exception was:\n");
            PyErr_Display(exception, v, tb);
            Py_DECREF(exception2);
            Py_DECREF(v2);
            Py_XDECREF(tb2);
        }
        Py_XDECREF(result);
        Py_XDECREF(args);
    } else {
        PySys_WriteStderr("sys.excepthook is missing\n");
        PyErr_Display(exception, v, tb);
    }
    Py_XDECREF(exception);
    Py_XDECREF(v);
    Py_XDECREF(tb);
}


extern "C" void PyErr_Print() {
    PyErr_PrintEx(1);
}
}
