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

// Hopefully soon we will be able to switch to CPython's getargs.c, instead of having
// to reimplement it.  For now, it's easier to create simple versions of the functions
// instead of trying to support all of the internals of getargs.c

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "codegen/compvars.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {


#define FLAG_COMPAT 1
#define FLAG_SIZE_T 2

// This function is named after the CPython one:
static int vgetargs1(PyObject* _tuple, const char* fmt, va_list* ap, int flags) {
    RELEASE_ASSERT(_tuple->cls == tuple_cls, "");
    BoxedTuple* tuple = static_cast<BoxedTuple*>(_tuple);

    bool now_optional = false;
    int arg_idx = 0;

    int tuple_size = tuple->elts.size();

    while (char c = *fmt) {
        fmt++;

        if (c == ':') {
            break;
        } else if (c == '|') {
            now_optional = true;
            continue;
        } else {
            if (arg_idx >= tuple_size) {
                RELEASE_ASSERT(now_optional, "");
                break;
            }

            PyObject* arg = tuple->elts[arg_idx];
            arg_idx++;

            switch (c) {
                case 'i': { // signed int
                    int* p = (int*)va_arg(*ap, int*);
                    RELEASE_ASSERT(arg->cls == int_cls, "%s", getTypeName(arg)->c_str());
                    int64_t n = static_cast<BoxedInt*>(arg)->n;
                    RELEASE_ASSERT(n >= INT_MIN, "");
                    RELEASE_ASSERT(n <= INT_MAX, "");
                    *p = n;
                    break;
                }
                case 'n': { // ssize_t
                    Py_ssize_t* p = (Py_ssize_t*)va_arg(*ap, Py_ssize_t*);
                    // could also be a long:
                    RELEASE_ASSERT(arg->cls == int_cls, "%s", getTypeName(arg)->c_str());
                    int64_t n = static_cast<BoxedInt*>(arg)->n;
                    *p = n;
                    break;
                }
                case 's': {
                    if (*fmt == '*') {
                        Py_buffer* p = (Py_buffer*)va_arg(*ap, Py_buffer*);

                        RELEASE_ASSERT(arg->cls == str_cls, "");
                        PyBuffer_FillInfo(p, arg, PyString_AS_STRING(arg), PyString_GET_SIZE(arg), 1, 0);
                        fmt++;
                    } else if (*fmt == ':') {
                        break;
                    } else {
                        RELEASE_ASSERT(0, "");
                    }
                    break;
                }
                case 'O': {
                    if (fmt && *fmt == '!') {
                        fmt++;

                        PyObject* _cls = (PyObject*)va_arg(*ap, PyObject*);
                        PyObject** p = (PyObject**)va_arg(*ap, PyObject**);

                        RELEASE_ASSERT(_cls->cls == type_cls, "%s", getTypeName(_cls)->c_str());
                        PyTypeObject* cls = static_cast<PyTypeObject*>(_cls);

                        if (!isSubclass(arg->cls, cls)) {
                            // should raise a TypeError
                            abort();
                        }

                        *p = arg;
                    } else {
                        PyObject** p = (PyObject**)va_arg(*ap, PyObject**);
                        *p = arg;
                    }
                    break;
                }
                default:
                    RELEASE_ASSERT(0, "Unhandled format character: '%c'", c);
            }
        }
    }
    return 1;
}

extern "C" int PyArg_VaParse(PyObject* _tuple, const char* fmt, va_list ap) {
    va_list lva;
    __va_copy(lva, ap);
    return vgetargs1(_tuple, fmt, &lva, 0);
}

extern "C" int PyArg_ParseTuple(PyObject* _tuple, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    int r = vgetargs1(_tuple, fmt, &ap, 0);

    va_end(ap);

    return r;
}

extern "C" int _PyArg_ParseTuple_SizeT(PyObject* args, char* format, ...) {
    int retval;
    va_list va;

    va_start(va, format);
    retval = vgetargs1(args, format, &va, FLAG_SIZE_T);
    va_end(va);
    return retval;
}

extern "C" int PyArg_ParseTupleAndKeywords(PyObject* args, PyObject* kwargs, const char* format, char** kwlist, ...) {
    assert(kwargs->cls == dict_cls);
    RELEASE_ASSERT(static_cast<BoxedDict*>(kwargs)->d.size() == 0, "");

    va_list ap;
    va_start(ap, kwlist);

    int r = vgetargs1(args, format, &ap, 0);

    va_end(ap);

    return r;
}

extern "C" int _PyArg_ParseTupleAndKeywords_SizeT(PyObject* args, PyObject* keywords, const char* format, char** kwlist,
                                                  ...) {
    if ((args == NULL || !PyTuple_Check(args)) || (keywords != NULL && !PyDict_Check(keywords)) || format == NULL
        || kwlist == NULL) {
        PyErr_BadInternalCall();
        return 0;
    }

    assert(keywords->cls == dict_cls);
    RELEASE_ASSERT(static_cast<BoxedDict*>(keywords)->d.size() == 0, "");

    va_list ap;
    va_start(ap, kwlist);

    int r = vgetargs1(args, format, &ap, FLAG_SIZE_T);

    va_end(ap);

    return r;
}

extern "C" int PyArg_UnpackTuple(PyObject* args, const char* name, Py_ssize_t min, Py_ssize_t max, ...) {
    RELEASE_ASSERT(args->cls == tuple_cls, "");
    BoxedTuple* t = static_cast<BoxedTuple*>(args);

    RELEASE_ASSERT(min <= t->elts.size() && t->elts.size() <= max, "");

    va_list ap;
    va_start(ap, max);

    for (auto e : t->elts) {
        PyObject** p = (PyObject**)va_arg(ap, PyObject**);
        *p = e;
    }

    va_end(ap);

    return true;
}

} // namespace pyston
