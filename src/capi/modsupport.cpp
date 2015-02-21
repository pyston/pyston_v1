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

// Hopefully soon we will be able to switch to CPython's modsupport.c, instead of having
// to reimplement it.  For now, it's easier to create simple versions of the functions
// instead of trying to support all of the internals of modsupport.c

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

#define FLAG_SIZE_T 1

static int countformat(const char* format, int endchar) noexcept {
    int count = 0;
    int level = 0;
    while (level > 0 || *format != endchar) {
        switch (*format) {
            case '\0':
                /* Premature end */
                PyErr_SetString(PyExc_SystemError, "unmatched paren in format");
                return -1;
            case '(':
            case '[':
            case '{':
                if (level == 0)
                    count++;
                level++;
                break;
            case ')':
            case ']':
            case '}':
                level--;
                break;
            case '#':
            case '&':
            case ',':
            case ':':
            case ' ':
            case '\t':
                break;
            default:
                if (level == 0)
                    count++;
        }
        format++;
    }
    return count;
}

static PyObject* do_mktuple(const char**, va_list*, int, int, int) noexcept;
// static PyObject *do_mklist(const char**, va_list *, int, int, int) noexcept;
// static PyObject *do_mkdict(const char**, va_list *, int, int, int) noexcept;
static PyObject* do_mkvalue(const char**, va_list*, int) noexcept;

static PyObject* do_mkvalue(const char** p_format, va_list* p_va, int flags) noexcept {
    for (;;) {
        switch (*(*p_format)++) {
            case '(':
                return do_mktuple(p_format, p_va, ')', countformat(*p_format, ')'), flags);

#if 0
            case '[':
                return do_mklist(p_format, p_va, ']', countformat(*p_format, ']'), flags);

            case '{':
                return do_mkdict(p_format, p_va, '}', countformat(*p_format, '}'), flags);
#endif

            case 'b':
            case 'B':
            case 'h':
            case 'i':
                return PyInt_FromLong((long)va_arg(*p_va, int));

            case 'H':
                return PyInt_FromLong((long)va_arg(*p_va, unsigned int));

            case 'n':
#if SIZEOF_SIZE_T != SIZEOF_LONG
                return PyInt_FromSsize_t(va_arg(*p_va, Py_ssize_t));
#endif
            /* Fall through from 'n' to 'l' if Py_ssize_t is long */
            case 'l':
                return PyInt_FromLong(va_arg(*p_va, long));

            case 'N':
            case 'S':
            case 'O':
                if (**p_format == '&') {
                    typedef PyObject* (*converter)(void*);
                    converter func = va_arg(*p_va, converter);
                    void* arg = va_arg(*p_va, void*);
                    ++*p_format;
                    return (*func)(arg);
                } else {
                    PyObject* v;
                    v = va_arg(*p_va, PyObject*);
                    if (v != NULL) {
                        if (*(*p_format - 1) != 'N')
                            Py_INCREF(v);
                    } else if (!PyErr_Occurred())
                        /* If a NULL was passed
                         * because a call that should
                         * have constructed a value
                         * failed, that's OK, and we
                         * pass the error on; but if
                         * no error occurred it's not
                         * clear that the caller knew
                         * what she was doing. */
                        PyErr_SetString(PyExc_SystemError, "NULL object passed to Py_BuildValue");
                    return v;
                }

            case 's':
            case 'z': {
                PyObject* v;
                char* str = va_arg(*p_va, char*);
                Py_ssize_t n;
                if (**p_format == '#') {
                    ++*p_format;
                    if (flags & FLAG_SIZE_T)
                        n = va_arg(*p_va, Py_ssize_t);
                    else
                        n = va_arg(*p_va, int);
                } else
                    n = -1;
                if (str == NULL) {
                    v = Py_None;
                    Py_INCREF(v);
                } else {
                    if (n < 0) {
                        size_t m = strlen(str);
                        if (m > PY_SSIZE_T_MAX) {
                            PyErr_SetString(PyExc_OverflowError, "string too long for Python string");
                            return NULL;
                        }
                        n = (Py_ssize_t)m;
                    }
                    v = PyString_FromStringAndSize(str, n);
                }
                return v;
            }

            default:
                RELEASE_ASSERT(0, "%c", *((*p_format) - 1));
        }
    }
    abort();
}

static PyObject* do_mktuple(const char** p_format, va_list* p_va, int endchar, int n, int flags) noexcept {
    PyObject* v;
    int i;
    int itemfailed = 0;
    if (n < 0)
        return NULL;
    if ((v = PyTuple_New(n)) == NULL)
        return NULL;
    /* Note that we can't bail immediately on error as this will leak
       refcounts on any 'N' arguments. */
    for (i = 0; i < n; i++) {
        PyObject* w = do_mkvalue(p_format, p_va, flags);
        if (w == NULL) {
            itemfailed = 1;
            Py_INCREF(Py_None);
            w = Py_None;
        }
        PyTuple_SET_ITEM(v, i, w);
    }
    if (itemfailed) {
        /* do_mkvalue() should have already set an error */
        Py_DECREF(v);
        return NULL;
    }
    if (**p_format != endchar) {
        Py_DECREF(v);
        PyErr_SetString(PyExc_SystemError, "Unmatched paren in format");
        return NULL;
    }
    if (endchar)
        ++*p_format;
    return v;
}

static PyObject* va_build_value(const char* fmt, va_list va, int flags) noexcept {
    int n = countformat(fmt, '\0');

    if (n < 0)
        return NULL;

    if (n == 0)
        return None;

    va_list lva;
    __va_copy(lva, va);

    if (n == 1)
        return do_mkvalue(&fmt, &lva, flags);

    return do_mktuple(&fmt, &lva, '\0', n, flags);
}

extern "C" PyObject* Py_VaBuildValue(const char* format, va_list va) noexcept {
    return va_build_value(format, va, 0);
}

extern "C" PyObject* _Py_VaBuildValue_SizeT(const char* format, va_list va) noexcept {
    return va_build_value(format, va, FLAG_SIZE_T);
}

extern "C" PyObject* _Py_BuildValue_SizeT(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);

    Box* r = va_build_value(fmt, ap, FLAG_SIZE_T);

    va_end(ap);
    return r;
}

extern "C" PyObject* Py_BuildValue(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);

    Box* r = va_build_value(fmt, ap, 0);

    va_end(ap);
    return r;
}

extern "C" PyObject* Py_InitModule4(const char* name, PyMethodDef* methods, const char* doc, PyObject* self,
                                    int apiver) noexcept {
    BoxedModule* module = createModule(name, "__builtin__");

    Box* passthrough = static_cast<Box*>(self);
    if (!passthrough)
        passthrough = None;

    while (methods && methods->ml_name) {
        if (VERBOSITY())
            printf("Loading method %s\n", methods->ml_name);

        assert((methods->ml_flags & (~(METH_VARARGS | METH_KEYWORDS | METH_NOARGS | METH_O))) == 0);
        module->giveAttr(methods->ml_name,
                         new BoxedCApiFunction(methods->ml_flags, passthrough, methods->ml_name, methods->ml_meth));

        methods++;
    }

    if (doc) {
        module->setattr("__doc__", boxStrConstant(doc), NULL);
    }

    return module;
}

extern "C" PyObject* PyModule_GetDict(PyObject* _m) noexcept {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    assert(m->cls == module_cls);

    return makeAttrWrapper(m);
}

extern "C" int PyModule_AddObject(PyObject* _m, const char* name, PyObject* value) noexcept {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    assert(m->cls == module_cls);

    m->setattr(name, value, NULL);
    return 0;
}

extern "C" int PyModule_AddStringConstant(PyObject* m, const char* name, const char* value) noexcept {
    PyObject* o = PyString_FromString(value);
    if (!o)
        return -1;
    if (PyModule_AddObject(m, name, o) == 0)
        return 0;
    Py_DECREF(o);
    return -1;
}

extern "C" int PyModule_AddIntConstant(PyObject* _m, const char* name, long value) noexcept {
    return PyModule_AddObject(_m, name, boxInt(value));
}



} // namespace pyston
