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
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// A dictionary-like wrapper around the attributes array.
// Not sure if this will be enough to satisfy users who expect __dict__
// or PyModule_GetDict to return real dicts.
BoxedClass* attrwrapper_cls;
class AttrWrapper : public Box {
private:
    Box* b;

public:
    AttrWrapper(Box* b) : Box(attrwrapper_cls), b(b) {}

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        AttrWrapper* aw = (AttrWrapper*)b;
        v->visit(aw->b);
    }

    static Box* setitem(Box* _self, Box* _key, Box* value) {
        assert(_self->cls == attrwrapper_cls);
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        self->b->setattr(key->s, value, NULL);

        return None;
    }
};

extern "C" PyObject* PyModule_GetDict(PyObject* _m) {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    assert(m->cls == module_cls);

    return new AttrWrapper(m);
}

extern "C" PyObject* PyDict_New() {
    return new BoxedDict();
}

extern "C" PyObject* PyString_FromString(const char* s) {
    return boxStrConstant(s);
}

extern "C" PyObject* PyInt_FromLong(long n) {
    return boxInt(n);
}

extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) {
    Box* b = static_cast<Box*>(mp);
    Box* key = static_cast<Box*>(_key);
    Box* item = static_cast<Box*>(_item);

    static std::string setitem_str("__setitem__");
    Box* r = callattrInternal(b, &setitem_str, CLASS_ONLY, NULL, ArgPassSpec(2), key, item, NULL, NULL, NULL);

    RELEASE_ASSERT(r, "");
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) {
    return PyDict_SetItem(mp, boxStrConstant(key), item);
}


BoxedClass* capifunc_cls;
class BoxedCApiFunction : public Box {
private:
    Box* passthrough;
    const char* name;
    PyCFunction func;

public:
    BoxedCApiFunction(Box* passthrough, const char* name, PyCFunction func)
        : Box(capifunc_cls), passthrough(passthrough), name(name), func(func) {}

    static BoxedString* __repr__(BoxedCApiFunction* self) {
        assert(self->cls == capifunc_cls);
        return boxStrConstant(self->name);
    }

    static Box* __call__(BoxedCApiFunction* self, BoxedTuple* varargs) {
        assert(self->cls == capifunc_cls);
        assert(varargs->cls == tuple_cls);

        threading::GLPromoteRegion _gil_lock;

        Box* rtn = (Box*)self->func(self->passthrough, varargs);
        assert(rtn);
        return rtn;
    }
};

extern "C" PyObject* Py_InitModule4(const char* name, PyMethodDef* methods, const char* doc, PyObject* self,
                                    int apiver) {
    BoxedModule* module = createModule(name, "__builtin__");

    Box* passthrough = static_cast<Box*>(self);
    if (!passthrough)
        passthrough = None;

    while (methods->ml_name) {
        if (VERBOSITY())
            printf("Loading method %s\n", methods->ml_name);
        assert(methods->ml_flags == METH_VARARGS);

        module->giveAttr(methods->ml_name, new BoxedCApiFunction(passthrough, methods->ml_name, methods->ml_meth));

        methods++;
    }

    if (doc) {
        module->setattr("__doc__", boxStrConstant(doc), NULL);
    }

    return module;
}

extern "C" PyObject* Py_BuildValue(const char* arg0, ...) {
    assert(*arg0 == '\0');
    return None;
}

extern "C" bool PyArg_ParseTuple(PyObject* tuple, const char* fmt, ...) {
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

BoxedModule* importTestExtension() {
    const char* pathname = "../test/test_extension/test.so";
    void* handle = dlopen(pathname, RTLD_NOW);
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

    assert(init);
    (*init)();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStrConstant("test");
    Box* _m = sys_modules->d[s];
    RELEASE_ASSERT(_m, "module failed to initialize properly?");
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);
    m->setattr("__file__", boxStrConstant(pathname), NULL);
    m->fn = pathname;
    return m;
}

void setupCAPI() {
    capifunc_cls = new BoxedClass(object_cls, NULL, 0, sizeof(BoxedCApiFunction), false);
    capifunc_cls->giveAttr("__name__", boxStrConstant("capifunc"));

    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));
    capifunc_cls->giveAttr("__str__", capifunc_cls->getattr("__repr__"));

    capifunc_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, false)));

    capifunc_cls->freeze();

    attrwrapper_cls = new BoxedClass(object_cls, &AttrWrapper::gcHandler, 0, sizeof(AttrWrapper), false);
    attrwrapper_cls->giveAttr("__name__", boxStrConstant("attrwrapper"));
    attrwrapper_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::setitem, UNKNOWN, 3)));
    attrwrapper_cls->freeze();
}

void teardownCAPI() {
}
}
