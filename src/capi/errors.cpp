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

extern "C" PyObject* PyErr_SetFromErrno(PyObject* exc) {
    return PyErr_SetFromErrnoWithFilenameObject(exc, NULL);
}
}
