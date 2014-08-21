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

// Hopefully soon we will be able to switch to CPython's modsupport.c, instead of having
// to reimplement it.  For now, it's easier to create simple versions of the functions
// instead of trying to support all of the internals of modsupport.c

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/types.h"
#include "codegen/compvars.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

#define FLAG_SIZE_T 1

extern "C" PyObject* _Py_BuildValue_SizeT(const char* arg0, ...) {
    RELEASE_ASSERT(*arg0 == '\0', "");
    return None;
}

extern "C" PyObject* Py_BuildValue(const char* arg0, ...) {
    RELEASE_ASSERT(*arg0 == '\0', "");
    return None;
}

extern "C" PyObject* Py_InitModule4(const char* name, PyMethodDef* methods, const char* doc, PyObject* self,
                                    int apiver) {
    BoxedModule* module = createModule(name, "__builtin__");

    Box* passthrough = static_cast<Box*>(self);
    if (!passthrough)
        passthrough = None;

    while (methods->ml_name) {
        if (VERBOSITY())
            printf("Loading method %s\n", methods->ml_name);

        assert((methods->ml_flags & (~(METH_VARARGS | METH_KEYWORDS | METH_NOARGS))) == 0);
        module->giveAttr(methods->ml_name,
                         new BoxedCApiFunction(methods->ml_flags, passthrough, methods->ml_name, methods->ml_meth));

        methods++;
    }

    if (doc) {
        module->setattr("__doc__", boxStrConstant(doc), NULL);
    }

    return module;
}

extern "C" int PyModule_AddIntConstant(PyObject* _m, const char* name, long value) {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    assert(m->cls == module_cls);

    m->setattr(name, boxInt(value), NULL);
    return 0;
}



} // namespace pyston
