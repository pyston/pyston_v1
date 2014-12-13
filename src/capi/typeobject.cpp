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

static int check_num_args(PyObject* ob, int n) {
    if (!PyTuple_CheckExact(ob)) {
        PyErr_SetString(PyExc_SystemError, "PyArg_UnpackTuple() argument list is not a tuple");
        return 0;
    }
    if (n == PyTuple_GET_SIZE(ob))
        return 1;
    PyErr_Format(PyExc_TypeError, "expected %d arguments, got %zd", n, PyTuple_GET_SIZE(ob));
    return 0;
}

static PyObject* wrap_call(PyObject* self, PyObject* args, void* wrapped, PyObject* kwds) {
    ternaryfunc func = (ternaryfunc)wrapped;

    return (*func)(self, args, kwds);
}

static PyObject* wrap_unaryfunc(PyObject* self, PyObject* args, void* wrapped) {
    unaryfunc func = (unaryfunc)wrapped;

    if (!check_num_args(args, 0))
        return NULL;
    return (*func)(self);
}

PyObject* slot_tp_new(PyTypeObject* self, PyObject* args, PyObject* kwds) noexcept {
    try {
        // TODO: runtime ICs?
        Box* new_attr = typeLookup(self, _new_str, NULL);
        assert(new_attr);
        new_attr = processDescriptor(new_attr, None, self);

        return runtimeCall(new_attr, ArgPassSpec(1, 0, true, true), self, args, kwds, NULL, NULL);
    } catch (Box* e) {
        abort();
    }
}

PyObject* slot_tp_call(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    try {
        Py_FatalError("this function is untested");

        // TODO: runtime ICs?
        return runtimeCall(self, ArgPassSpec(0, 0, true, true), args, kwds, NULL, NULL, NULL);
    } catch (Box* e) {
        abort();
    }
}

PyObject* slot_tp_repr(PyObject* self) noexcept {
    try {
        return repr(self);
    } catch (Box* e) {
        abort();
    }
}

typedef wrapper_def slotdef;

static void** slotptr(BoxedClass* type, int offset) {
    // We use the index into PyHeapTypeObject as the canonical way to represent offsets, even though we are not
    // (currently) using that object representation

    // copied from CPython:
    /* Note: this depends on the order of the members of PyHeapTypeObject! */
    assert(offset >= 0);
    assert((size_t)offset < offsetof(PyHeapTypeObject, as_buffer));
    char* ptr;
    if ((size_t)offset >= offsetof(PyHeapTypeObject, as_sequence)) {
        ptr = (char*)type->tp_as_sequence;
        offset -= offsetof(PyHeapTypeObject, as_sequence);
    } else if ((size_t)offset >= offsetof(PyHeapTypeObject, as_mapping)) {
        ptr = (char*)type->tp_as_mapping;
        offset -= offsetof(PyHeapTypeObject, as_mapping);
    } else if ((size_t)offset >= offsetof(PyHeapTypeObject, as_number)) {
        ptr = (char*)type->tp_as_number;
        offset -= offsetof(PyHeapTypeObject, as_number);
    } else {
        ptr = (char*)type;
    }
    if (ptr != NULL)
        ptr += offset;
    return (void**)ptr;
}

static void update_one_slot(BoxedClass* self, const slotdef& p) {
    // TODO: CPython version is significantly more sophisticated
    void** ptr = slotptr(self, p.offset);
    assert(ptr && "it is ok for this to be NULL (CPython handles that case) but I don't think it should happen?");

    if (typeLookup(self, p.name, NULL))
        *ptr = p.function;
    else
        *ptr = NULL;
}

// Copied from CPython:
#define TPSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), 0 }
#define FLSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC, FLAGS)                                                              \
    { NAME, offsetof(PyTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), FLAGS }
#define ETSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    { NAME, offsetof(PyHeapTypeObject, SLOT), (void*)(FUNCTION), WRAPPER, PyDoc_STR(DOC), 0 }
#define SQSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC) ETSLOT(NAME, as_sequence.SLOT, FUNCTION, WRAPPER, DOC)
#define MPSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC) ETSLOT(NAME, as_mapping.SLOT, FUNCTION, WRAPPER, DOC)
#define NBSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC) ETSLOT(NAME, as_number.SLOT, FUNCTION, WRAPPER, DOC)
#define UNSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, WRAPPER, "x." NAME "() <==> " DOC)
#define IBSLOT(NAME, SLOT, FUNCTION, WRAPPER, DOC)                                                                     \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, WRAPPER, "x." NAME "(y) <==> x" DOC "y")
#define BINSLOT(NAME, SLOT, FUNCTION, DOC)                                                                             \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_l, "x." NAME "(y) <==> x" DOC "y")
#define RBINSLOT(NAME, SLOT, FUNCTION, DOC)                                                                            \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_r, "x." NAME "(y) <==> y" DOC "x")
#define BINSLOTNOTINFIX(NAME, SLOT, FUNCTION, DOC)                                                                     \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_l, "x." NAME "(y) <==> " DOC)
#define RBINSLOTNOTINFIX(NAME, SLOT, FUNCTION, DOC)                                                                    \
    ETSLOT(NAME, as_number.SLOT, FUNCTION, wrap_binaryfunc_r, "x." NAME "(y) <==> " DOC)

static slotdef slotdefs[] = {
    TPSLOT("__repr__", tp_repr, slot_tp_repr, wrap_unaryfunc, "x.__repr__() <==> repr(x)"),
    FLSLOT("__call__", tp_call, slot_tp_call, (wrapperfunc)wrap_call, "x.__call__(...) <==> x(...)",
           PyWrapperFlag_KEYWORDS),
    TPSLOT("__new__", tp_new, slot_tp_new, NULL, ""),

#if 0
    SQSLOT("__len__", sq_length, slot_sq_length, wrap_lenfunc, "x.__len__() <==> len(x)"),
    /* Heap types defining __add__/__mul__ have sq_concat/sq_repeat == NULL.
       The logic in abstract.c always falls back to nb_add/nb_multiply in
       this case.  Defining both the nb_* and the sq_* slots to call the
       user-defined methods has unexpected side-effects, as shown by
       test_descr.notimplemented() */
    SQSLOT("__add__", sq_concat, NULL, wrap_binaryfunc, "x.__add__(y) <==> x+y"),
    SQSLOT("__mul__", sq_repeat, NULL, wrap_indexargfunc, "x.__mul__(n) <==> x*n"),
    SQSLOT("__rmul__", sq_repeat, NULL, wrap_indexargfunc, "x.__rmul__(n) <==> n*x"),
    SQSLOT("__getitem__", sq_item, slot_sq_item, wrap_sq_item, "x.__getitem__(y) <==> x[y]"),
    SQSLOT("__getslice__", sq_slice, slot_sq_slice, wrap_ssizessizeargfunc, "x.__getslice__(i, j) <==> x[i:j]\n\
           \n\
           Use of negative indices is not supported."),
    SQSLOT("__setitem__", sq_ass_item, slot_sq_ass_item, wrap_sq_setitem, "x.__setitem__(i, y) <==> x[i]=y"),
    SQSLOT("__delitem__", sq_ass_item, slot_sq_ass_item, wrap_sq_delitem, "x.__delitem__(y) <==> del x[y]"),
    SQSLOT("__setslice__", sq_ass_slice, slot_sq_ass_slice, wrap_ssizessizeobjargproc,
           "x.__setslice__(i, j, y) <==> x[i:j]=y\n\
           \n\
           Use  of negative indices is not supported."),
    SQSLOT("__delslice__", sq_ass_slice, slot_sq_ass_slice, wrap_delslice, "x.__delslice__(i, j) <==> del x[i:j]\n\
           \n\
           Use of negative indices is not supported."),
    SQSLOT("__contains__", sq_contains, slot_sq_contains, wrap_objobjproc, "x.__contains__(y) <==> y in x"),
    SQSLOT("__iadd__", sq_inplace_concat, NULL, wrap_binaryfunc, "x.__iadd__(y) <==> x+=y"),
    SQSLOT("__imul__", sq_inplace_repeat, NULL, wrap_indexargfunc, "x.__imul__(y) <==> x*=y"),
#endif
};

static void init_slotdefs() {
    static bool initialized = false;
    if (initialized)
        return;

    for (int i = 0; i < sizeof(slotdefs) / sizeof(slotdefs[0]); i++) {
        if (i > 0) {
            ASSERT(slotdefs[i].offset >= slotdefs[i - 1].offset, "%d %s", i, slotdefs[i - 1].name);
            // CPython interns the name here
        }
    }

    initialized = true;
}

bool update_slot(BoxedClass* self, const std::string& attr) {
    bool updated = false;
    for (const slotdef& p : slotdefs) {
        if (p.name == attr) {
            // TODO update subclasses;
            update_one_slot(self, p);
            updated = true;
        }
    }
    return updated;
}

void fixup_slot_dispatchers(BoxedClass* self) {
    init_slotdefs();

    for (const slotdef& p : slotdefs) {
        update_one_slot(self, p);
    }

    // TODO: CPython handles this by having the __name__ attribute wrap (via a getset object)
    // the tp_name field, whereas we're (needlessly?) doing the opposite.
    if (!self->tp_name) {
        Box* b = self->getattr("__name__");
        assert(b);
        assert(b->cls == str_cls);
        self->tp_name = static_cast<BoxedString*>(b)->s.c_str();
    }
}

static PyObject* tp_new_wrapper(PyTypeObject* self, BoxedTuple* args, Box* kwds) {
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

static void add_tp_new_wrapper(BoxedClass* type) {
    if (type->getattr("__new__"))
        return;

    type->giveAttr("__new__",
                   new BoxedCApiFunction(METH_VARARGS | METH_KEYWORDS, type, "__new__", (PyCFunction)tp_new_wrapper));
}

static void add_operators(BoxedClass* cls) {
    init_slotdefs();

    for (const slotdef& p : slotdefs) {
        if (!p.wrapper)
            continue;

        void** ptr = slotptr(cls, p.offset);

        if (!ptr || !*ptr)
            continue;
        if (cls->getattr(p.name))
            continue;
        // TODO PyObject_HashNotImplemented

        cls->giveAttr(p.name, new BoxedWrapperDescriptor(&p, cls));
    }

    if (cls->tp_new)
        add_tp_new_wrapper(cls);
}

extern "C" int PyType_IsSubtype(PyTypeObject* a, PyTypeObject* b) {
    return isSubclass(a, b);
}

extern "C" int PyType_Ready(PyTypeObject* cls) {
    gc::registerNonheapRootObject(cls);

    // unhandled fields:
    RELEASE_ASSERT(cls->tp_print == NULL, "");
    RELEASE_ASSERT(cls->tp_getattr == NULL, "");
    RELEASE_ASSERT(cls->tp_setattr == NULL, "");
    RELEASE_ASSERT(cls->tp_compare == NULL, "");
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

    RELEASE_ASSERT(cls->tp_richcompare == NULL, "");
    RELEASE_ASSERT(cls->tp_iter == NULL, "");
    RELEASE_ASSERT(cls->tp_iternext == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
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

// I think it is safe to ignore these for for now:
// RELEASE_ASSERT(cls->tp_weaklistoffset == 0, "");
// RELEASE_ASSERT(cls->tp_traverse == NULL, "");
// RELEASE_ASSERT(cls->tp_clear == NULL, "");

#define INITIALIZE(a) new (&(a)) decltype(a)
    INITIALIZE(cls->attrs);
    INITIALIZE(cls->dependent_icgetattrs);
#undef INITIALIZE

    BoxedClass* base = cls->base = object_cls;
    if (!cls->cls)
        cls->cls = cls->base->cls;

    assert(cls->tp_dict == NULL);
    cls->tp_dict = makeAttrWrapper(cls);

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

    try {
        add_operators(cls);
    } catch (Box* b) {
        abort();
    }

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
