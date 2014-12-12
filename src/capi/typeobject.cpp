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


#include "capi/typeobject.h"

#include "capi/types.h"
#include "runtime/objmodel.h"

namespace pyston {

// FIXME duplicated with objmodel.cpp
static const std::string _new_str("__new__");

extern "C" void conservativeGCHandler(GCVisitor* v, Box* b) {
    v->visitPotentialRange((void* const*)b, (void* const*)((char*)b + b->cls->tp_basicsize));
}

PyObject* Py_CallPythonNew(PyTypeObject* self, PyObject* args, PyObject* kwds) {
    try {
        Py_FatalError("this function is untested");

        // TODO: runtime ICs?
        Box* new_attr = typeLookup(self, _new_str, NULL);
        assert(new_attr);
        new_attr = processDescriptor(new_attr, None, self);

        return runtimeCall(new_attr, ArgPassSpec(1, 0, true, true), self, args, kwds, NULL, NULL);
    } catch (Box* e) {
        abort();
    }
}

PyObject* Py_CallPythonCall(PyObject* self, PyObject* args, PyObject* kwds) {
    try {
        Py_FatalError("this function is untested");

        // TODO: runtime ICs?
        return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwds, NULL, NULL, NULL);
    } catch (Box* e) {
        abort();
    }
}


bool update_slot(BoxedClass* self, const std::string& attr) {
    if (attr == "__new__") {
        self->tp_new = &Py_CallPythonNew;
        // TODO update subclasses
        return true;
    }

    if (attr == "__call__") {
        self->tp_call = &Py_CallPythonCall;
        // TODO update subclasses
        return true;
    }

    return false;
}

void fixup_slot_dispatchers(BoxedClass* self) {
    // This will probably share a lot in common with Py_TypeReady:
    if (!self->tp_new) {
        self->tp_new = &Py_CallPythonNew;
    } else if (self->tp_new != Py_CallPythonNew) {
        ASSERT(0, "need to set __new__?");
    }

    if (!self->tp_call) {
        self->tp_call = &Py_CallPythonCall;
    } else if (self->tp_call != Py_CallPythonCall) {
        ASSERT(0, "need to set __call__?");
    }
}

wrapper_def call_wrapper = { "__call__", offsetof(PyTypeObject, tp_call), PyWrapperFlag_KEYWORDS };

PyObject* tp_new_wrapper(PyTypeObject* self, BoxedTuple* args, Box* kwds) {
    RELEASE_ASSERT(isSubclass(self->cls, type_cls), "");

    // ASSERT(self->tp_new != Py_CallPythonNew, "going to get in an infinite loop");

    RELEASE_ASSERT(args->cls == tuple_cls, "");
    RELEASE_ASSERT(kwds->cls == dict_cls, "");
    RELEASE_ASSERT(args->elts.size() >= 1, "");

    BoxedClass* subtype = static_cast<BoxedClass*>(args->elts[0]);
    RELEASE_ASSERT(isSubclass(subtype->cls, type_cls), "");
    RELEASE_ASSERT(isSubclass(subtype, self), "");

    BoxedTuple* new_args = new BoxedTuple(BoxedTuple::GCVector(args->elts.begin() + 1, args->elts.end()));

    return self->tp_new(subtype, new_args, kwds);
}

static void add_operators(PyTypeObject* cls) {
    if (cls->tp_new) {
        cls->giveAttr("__new__",
                      new BoxedCApiFunction(METH_VARARGS | METH_KEYWORDS, cls, "__new__", (PyCFunction)tp_new_wrapper));
    }

    if (cls->tp_call) {
        cls->giveAttr("__call__", new BoxedWrapperDescriptor(&call_wrapper, cls));
    }
}

extern "C" int PyType_IsSubtype(PyTypeObject*, PyTypeObject*) {
    Py_FatalError("unimplemented");
}

extern "C" int PyType_Ready(PyTypeObject* cls) {
    gc::registerNonheapRootObject(cls);

    // unhandled fields:
    RELEASE_ASSERT(cls->tp_print == NULL, "");
    RELEASE_ASSERT(cls->tp_getattr == NULL, "");
    RELEASE_ASSERT(cls->tp_setattr == NULL, "");
    RELEASE_ASSERT(cls->tp_compare == NULL, "");
    RELEASE_ASSERT(cls->tp_repr == NULL, "");
    RELEASE_ASSERT(cls->tp_as_number == NULL, "");
    RELEASE_ASSERT(cls->tp_as_sequence == NULL, "");
    RELEASE_ASSERT(cls->tp_as_mapping == NULL, "");
    RELEASE_ASSERT(cls->tp_hash == NULL, "");
    RELEASE_ASSERT(cls->tp_str == NULL, "");
    RELEASE_ASSERT(cls->tp_getattro == NULL || cls->tp_getattro == PyObject_GenericGetAttr, "");
    RELEASE_ASSERT(cls->tp_setattro == NULL, "");
    RELEASE_ASSERT(cls->tp_as_buffer == NULL, "");

    int ALLOWABLE_FLAGS = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC;
    RELEASE_ASSERT((cls->tp_flags & ~ALLOWABLE_FLAGS) == 0, "");
    // RELEASE_ASSERT(cls->tp_traverse == NULL, "");
    // RELEASE_ASSERT(cls->tp_clear == NULL, "");

    RELEASE_ASSERT(cls->tp_richcompare == NULL, "");
    RELEASE_ASSERT(cls->tp_iter == NULL, "");
    RELEASE_ASSERT(cls->tp_iternext == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_dict == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_get == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_set == NULL, "");
    RELEASE_ASSERT(cls->tp_init == NULL, "");
    RELEASE_ASSERT(cls->tp_alloc == NULL, "");
    RELEASE_ASSERT(cls->tp_free == NULL || cls->tp_free == PyObject_Del, "");
    RELEASE_ASSERT(cls->tp_is_gc == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_mro == NULL, "");
    RELEASE_ASSERT(cls->tp_cache == NULL, "");
    RELEASE_ASSERT(cls->tp_subclasses == NULL, "");
    RELEASE_ASSERT(cls->tp_weaklist == NULL, "");
    RELEASE_ASSERT(cls->tp_del == NULL, "");
    RELEASE_ASSERT(cls->tp_version_tag == 0, "");

// I think it is safe to ignore tp_weaklistoffset for now:
// RELEASE_ASSERT(cls->tp_weaklistoffset == 0, "");

#define INITIALIZE(a) new (&(a)) decltype(a)
    INITIALIZE(cls->attrs);
    INITIALIZE(cls->dependent_icgetattrs);
#undef INITIALIZE

    BoxedClass* base = cls->base = object_cls;
    if (!cls->cls)
        cls->cls = cls->base->cls;

    assert(cls->tp_name);
    cls->giveAttr("__name__", boxStrConstant(cls->tp_name));
    // tp_name
    // tp_basicsize, tp_itemsize
    // tp_doc

    if (!cls->tp_new && base != object_cls)
        cls->tp_new = base->tp_new;

    if (!cls->tp_alloc) {
        cls->tp_alloc = reinterpret_cast<decltype(cls->tp_alloc)>(PyType_GenericAlloc);
    }

    add_operators(cls);

    for (PyMethodDef* method = cls->tp_methods; method && method->ml_name; ++method) {
        cls->giveAttr(method->ml_name, new BoxedMethodDescriptor(method, cls));
    }

    for (PyMemberDef* member = cls->tp_members; member && member->name; ++member) {
        cls->giveAttr(member->name, new BoxedMemberDescriptor(member));
    }

    if (cls->tp_getset) {
        if (VERBOSITY())
            printf("warning: ignoring tp_getset for now\n");
    }

    cls->gc_visit = &conservativeGCHandler;

    // TODO not sure how we can handle extension types that manually
    // specify a dict...
    RELEASE_ASSERT(cls->tp_dictoffset == 0, "");
    // this should get automatically initialized to 0 on this path:
    assert(cls->attrs_offset == 0);

    return 0;
}

} // namespace pyston
