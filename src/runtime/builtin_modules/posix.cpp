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

#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/types.h"
#include "gc/collector.h"
#include "runtime/inline/boxing.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

BoxedModule* posix_module;

namespace posix {

static Box* urandom(Box* _n) {
    RELEASE_ASSERT(_n->cls == int_cls, "");

    int64_t n = static_cast<BoxedInt*>(_n)->n;
    RELEASE_ASSERT(n < INT_MAX, "");

    int fd = ::open("/dev/urandom", O_RDONLY);
    RELEASE_ASSERT(fd > 0, "");

    BoxedString* r = static_cast<BoxedString*>(PyString_FromStringAndSize(NULL, n));
    RELEASE_ASSERT(r, "");
    char* buf = PyString_AsString(r);

    int total_read = 0;
    while (total_read < n) {
        int this_read = read(fd, buf, n - total_read);
        assert(this_read > 0);
        total_read += this_read;
    }
    ::close(fd);

    return r;
}

static Box* posix_getuid() {
    return boxInt(getuid());
}

extern "C" {
extern char** environ;
}

static Box* convertEnviron() {
    assert(environ);

    BoxedDict* d = new BoxedDict();
    // Ported from CPython:
    for (char** e = environ; *e != NULL; e++) {
        char* p = strchr(*e, '=');
        if (p == NULL)
            continue;
        Box* k = PyString_FromStringAndSize(*e, (int)(p - *e));
        Box* v = PyString_FromString(p + 1);

        Box*& cur_v = d->d[k];
        if (cur_v == NULL)
            cur_v = v;
    }
    return d;
}

} // namespace posix

using namespace posix;

void setupPosix() {
    posix_module = createModule("posix", "__builtin__");

    posix_module->giveAttr("urandom", new BoxedFunction(boxRTFunction((void*)posix::urandom, STR, 1)));
    posix_module->giveAttr("getuid", new BoxedFunction(boxRTFunction((void*)posix::posix_getuid, BOXED_INT, 0)));

    posix_module->giveAttr("error", OSError);
    posix_module->giveAttr("environ", convertEnviron());
}
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-constant-out-of-range-compare"
#else
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
// Copied from CPython/Modules/posixmodule.c:
extern "C" {
PyObject* _PyInt_FromUid(uid_t uid) {
    if (uid <= LONG_MAX)
        return PyInt_FromLong(uid);
    return PyLong_FromUnsignedLong(uid);
}

PyObject* _PyInt_FromGid(gid_t gid) {
    if (gid <= LONG_MAX)
        return PyInt_FromLong(gid);
    return PyLong_FromUnsignedLong(gid);
}

int _Py_Uid_Converter(PyObject* obj, void* p) {
    int overflow;
    long result;
    if (PyFloat_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "integer argument expected, got float");
        return 0;
    }
    result = PyLong_AsLongAndOverflow(obj, &overflow);
    if (overflow < 0)
        goto OverflowDown;
    if (!overflow && result == -1) {
        /* error or -1 */
        if (PyErr_Occurred())
            return 0;
        *(uid_t*)p = (uid_t)-1;
    } else {
        /* unsigned uid_t */
        unsigned long uresult;
        if (overflow > 0) {
            uresult = PyLong_AsUnsignedLong(obj);
            if (PyErr_Occurred()) {
                if (PyErr_ExceptionMatches(PyExc_OverflowError))
                    goto OverflowUp;
                return 0;
            }
        } else {
            if (result < 0)
                goto OverflowDown;
            uresult = result;
        }
        if (sizeof(uid_t) < sizeof(long) && (unsigned long)(uid_t)uresult != uresult)
            goto OverflowUp;
        *(uid_t*)p = (uid_t)uresult;
    }
    return 1;

OverflowDown:
    PyErr_SetString(PyExc_OverflowError, "user id is less than minimum");
    return 0;

OverflowUp:
    PyErr_SetString(PyExc_OverflowError, "user id is greater than maximum");
    return 0;
}

int _Py_Gid_Converter(PyObject* obj, void* p) {
    int overflow;
    long result;
    if (PyFloat_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "integer argument expected, got float");
        return 0;
    }
    result = PyLong_AsLongAndOverflow(obj, &overflow);
    if (overflow < 0)
        goto OverflowDown;
    if (!overflow && result == -1) {
        /* error or -1 */
        if (PyErr_Occurred())
            return 0;
        *(gid_t*)p = (gid_t)-1;
    } else {
        /* unsigned gid_t */
        unsigned long uresult;
        if (overflow > 0) {
            uresult = PyLong_AsUnsignedLong(obj);
            if (PyErr_Occurred()) {
                if (PyErr_ExceptionMatches(PyExc_OverflowError))
                    goto OverflowUp;
                return 0;
            }
        } else {
            if (result < 0)
                goto OverflowDown;
            uresult = result;
        }
        if (sizeof(gid_t) < sizeof(long) && (unsigned long)(gid_t)uresult != uresult)
            goto OverflowUp;
        *(gid_t*)p = (gid_t)uresult;
    }
    return 1;

OverflowDown:
    PyErr_SetString(PyExc_OverflowError, "group id is less than minimum");
    return 0;

OverflowUp:
    PyErr_SetString(PyExc_OverflowError, "group id is greater than maximum");
    return 0;
}
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif
