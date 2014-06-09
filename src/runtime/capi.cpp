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

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "codegen/compvars.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

static BoxedModule* test_module = NULL;

extern "C" const ObjectFlavor capifunc_flavor(&boxGCHandler, NULL);
BoxedClass* capifunc_cls;
class BoxedCApiFunction : public Box {
private:
    const char* name;
    PyCFunction func;

public:
    BoxedCApiFunction(const char* name, PyCFunction func)
        : Box(&capifunc_flavor, capifunc_cls), name(name), func(func) {}

    static BoxedString* __repr__(BoxedCApiFunction* self) {
        assert(self->cls == capifunc_cls);
        return boxStrConstant(self->name);
    }

    static Box* __call__(BoxedCApiFunction* self, BoxedTuple* varargs) {
        assert(self->cls == capifunc_cls);
        assert(varargs->cls == tuple_cls);

        threading::GLPromoteRegion _gil_lock;

        Box* rtn = (Box*)self->func(test_module, varargs);
        assert(rtn);
        return rtn;
    }
};

extern "C" void* Py_InitModule4(const char* arg0, PyMethodDef* arg1, const char* arg2, PyObject* arg3, int arg4) {
    test_module = createModule("test", "../test/test_extension/test.so");

    while (arg1->ml_name) {
        if (VERBOSITY())
            printf("Loading method %s\n", arg1->ml_name);
        assert(arg1->ml_flags == METH_VARARGS);

        // test_module->giveAttr(arg1->ml_name, boxInt(1));
        test_module->giveAttr(arg1->ml_name, new BoxedCApiFunction(arg1->ml_name, arg1->ml_meth));

        arg1++;
    }
    return test_module;
}

extern "C" void* Py_BuildValue(const char* arg0) {
    assert(*arg0 == '\0');
    return None;
}

extern "C" bool PyArg_ParseTuple(void* tuple, const char* fmt, ...) {
    if (strcmp("", fmt) == 0)
        return true;

    assert(strcmp("O", fmt) == 0);

    BoxedTuple* varargs = (BoxedTuple*)tuple;
    assert(varargs->elts.size() == 1);

    va_list ap;
    va_start(ap, fmt);

    Box** arg0 = va_arg(ap, Box**);
    *arg0 = varargs->elts[0];

    va_end(ap);

    return true;
}

BoxedModule* getTestModule() {
    if (test_module)
        return test_module;

    void* handle = dlopen("../test/test_extension/test.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    assert(handle);

    void (*init)() = (void (*)())dlsym(handle, "inittest");

    char* error;
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    // dlclose(handle);

    assert(init);
    (*init)();
    assert(test_module);
    return test_module;
}

void setupCAPI() {
    capifunc_cls = new BoxedClass(object_cls, 0, sizeof(BoxedCApiFunction), false);
    capifunc_cls->giveAttr("__name__", boxStrConstant("capifunc"));

    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));
    capifunc_cls->giveAttr("__str__", capifunc_cls->getattr("__repr__"));

    capifunc_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, false)));

    capifunc_cls->freeze();
}

void teardownCAPI() {
}
}
