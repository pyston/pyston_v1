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
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "core/common.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/dict.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

#ifdef MS_WINDOWS
extern "C" PyObject* PyErr_SetFromErrnoWithUnicodeFilename(PyObject* exc, const Py_UNICODE* filename) noexcept {
    PyObject* name = filename ? PyUnicode_FromUnicode(filename, wcslen(filename)) : NULL;
    PyObject* result = PyErr_SetFromErrnoWithFilenameObject(exc, name);
    Py_XDECREF(name);
    return result;
}
#endif /* MS_WINDOWS */

static int parse_syntax_error(PyObject* err, PyObject** message, const char** filename, int* lineno, int* offset,
                              const char** text) noexcept {
    long hold;
    PyObject* v;

    /* old style errors */
    if (PyTuple_Check(err))
        return PyArg_ParseTuple(err, "O(ziiz)", message, filename, lineno, offset, text);

    *message = NULL;

    /* new style errors.  `err' is an instance */
    *message = PyObject_GetAttrString(err, "msg");
    if (!*message)
        goto finally;

    v = PyObject_GetAttrString(err, "filename");
    if (!v)
        goto finally;
    if (v == Py_None) {
        Py_DECREF(v);
        *filename = NULL;
    } else {
        *filename = PyString_AsString(v);
        Py_DECREF(v);
        if (!*filename)
            goto finally;
    }

    v = PyObject_GetAttrString(err, "lineno");
    if (!v)
        goto finally;
    hold = PyInt_AsLong(v);
    Py_DECREF(v);
    if (hold < 0 && PyErr_Occurred())
        goto finally;
    *lineno = (int)hold;

    v = PyObject_GetAttrString(err, "offset");
    if (!v)
        goto finally;
    if (v == Py_None) {
        *offset = -1;
        Py_DECREF(v);
    } else {
        hold = PyInt_AsLong(v);
        Py_DECREF(v);
        if (hold < 0 && PyErr_Occurred())
            goto finally;
        *offset = (int)hold;
    }

    v = PyObject_GetAttrString(err, "text");
    if (!v)
        goto finally;
    if (v == Py_None) {
        Py_DECREF(v);
        *text = NULL;
    } else {
        *text = PyString_AsString(v);
        Py_DECREF(v);
        if (!*text)
            goto finally;
    }
    return 1;

finally:
    Py_XDECREF(*message);
    return 0;
}

static void print_error_text(PyObject* f, int offset, const char* text) noexcept {
    const char* nl;
    if (offset >= 0) {
        if (offset > 0 && offset == strlen(text) && text[offset - 1] == '\n')
            offset--;
        for (;;) {
            nl = strchr(text, '\n');
            if (nl == NULL || nl - text >= offset)
                break;
            offset -= (int)(nl + 1 - text);
            text = nl + 1;
        }
        while (*text == ' ' || *text == '\t') {
            text++;
            offset--;
        }
    }
    PyFile_WriteString("    ", f);
    PyFile_WriteString(text, f);
    if (*text == '\0' || text[strlen(text) - 1] != '\n')
        PyFile_WriteString("\n", f);
    if (offset == -1)
        return;
    PyFile_WriteString("    ", f);
    offset--;
    while (offset > 0) {
        PyFile_WriteString(" ", f);
        offset--;
    }
    PyFile_WriteString("^\n", f);
}

extern "C" void PyErr_Display(PyObject* exception, PyObject* value, PyObject* tb) noexcept {
    int err = 0;
    PyObject* f = PySys_GetObject("stderr");
    Py_INCREF(value);
    if (f == NULL || f == Py_None)
        fprintf(stderr, "lost sys.stderr\n");
    else {
        if (Py_FlushLine())
            PyErr_Clear();
        fflush(stdout);
        if (tb && tb != Py_None)
            err = PyTraceBack_Print(tb, f);
        if (err == 0 && PyObject_HasAttrString(value, "print_file_and_line")) {
            PyObject* message;
            const char* filename, *text;
            int lineno, offset;
            if (!parse_syntax_error(value, &message, &filename, &lineno, &offset, &text))
                PyErr_Clear();
            else {
                char buf[10];
                PyFile_WriteString("  File \"", f);
                if (filename == NULL)
                    PyFile_WriteString("<string>", f);
                else
                    PyFile_WriteString(filename, f);
                PyFile_WriteString("\", line ", f);
                PyOS_snprintf(buf, sizeof(buf), "%d", lineno);
                PyFile_WriteString(buf, f);
                PyFile_WriteString("\n", f);
                if (text != NULL)
                    print_error_text(f, offset, text);
                Py_DECREF(value);
                value = message;
                /* Can't be bothered to check all those
                   PyFile_WriteString() calls */
                if (PyErr_Occurred())
                    err = -1;
            }
        }
        if (err) {
            /* Don't do anything else */
        } else if (PyExceptionClass_Check(exception)) {
            PyObject* moduleName;
            const char* className = PyExceptionClass_Name(exception);
            if (className != NULL) {
                const char* dot = strrchr(className, '.');
                if (dot != NULL)
                    className = dot + 1;
            }

            moduleName = PyObject_GetAttrString(exception, "__module__");
            if (moduleName == NULL)
                err = PyFile_WriteString("<unknown>", f);
            else {
                char* modstr = PyString_AsString(moduleName);
                if (modstr && strcmp(modstr, "exceptions")) {
                    err = PyFile_WriteString(modstr, f);
                    err += PyFile_WriteString(".", f);
                }
                Py_DECREF(moduleName);
            }
            if (err == 0) {
                if (className == NULL)
                    err = PyFile_WriteString("<unknown>", f);
                else
                    err = PyFile_WriteString(className, f);
            }
        } else
            err = PyFile_WriteObject(exception, f, Py_PRINT_RAW);
        if (err == 0 && (value != Py_None)) {
            PyObject* s = PyObject_Str(value);
            /* only print colon if the str() of the
               object is not the empty string
            */
            if (s == NULL)
                err = -1;
            else if (!PyString_Check(s) || PyString_GET_SIZE(s) != 0)
                err = PyFile_WriteString(": ", f);
            if (err == 0)
                err = PyFile_WriteObject(s, f, Py_PRINT_RAW);
            Py_XDECREF(s);
        }
        /* try to write a newline in any case */
        err += PyFile_WriteString("\n", f);
    }
    Py_DECREF(value);
    /* If an error happened here, don't show it.
       XXX This is wrong, but too many callers rely on this behavior. */
    if (err != 0)
        PyErr_Clear();
}

static void handle_system_exit(void) noexcept {
    PyObject* exception, *value, *tb;
    int exitcode = 0;

    if (Py_InspectFlag)
        /* Don't exit if -i flag was given. This flag is set to 0
         * when entering interactive mode for inspecting. */
        return;

    PyErr_Fetch(&exception, &value, &tb);
    if (Py_FlushLine())
        PyErr_Clear();
    fflush(stdout);
    if (value == NULL || value == Py_None)
        goto done;
    if (PyExceptionInstance_Check(value)) {
        /* The error code should be in the `code' attribute. */
        PyObject* code = PyObject_GetAttrString(value, "code");
        if (code) {
            Py_DECREF(value);
            value = code;
            if (value == Py_None)
                goto done;
        }
        /* If we failed to dig out the 'code' attribute,
           just let the else clause below print the error. */
    }
    if (PyInt_Check(value))
        exitcode = (int)PyInt_AsLong(value);
    else {
        PyObject* sys_stderr = PySys_GetObject("stderr");
        if (sys_stderr != NULL && sys_stderr != Py_None) {
            PyFile_WriteObject(value, sys_stderr, Py_PRINT_RAW);
        } else {
            PyObject_Print(value, stderr, Py_PRINT_RAW);
            fflush(stderr);
        }
        PySys_WriteStderr("\n");
        exitcode = 1;
    }
done:
    /* Restore and clear the exception info, in order to properly decref
     * the exception, value, and traceback.      If we just exit instead,
     * these leak, which confuses PYTHONDUMPREFS output, and may prevent
     * some finalizers from running.
     */
    PyErr_Restore(exception, value, tb);
    PyErr_Clear();
    Py_Exit(exitcode);
    /* NOTREACHED */
}

extern "C" void PyErr_PrintEx(int set_sys_last_vars) noexcept {
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

extern "C" void PyErr_Print() noexcept {
    PyErr_PrintEx(1);
}
}
