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

#include "runtime/util.h"

#include "codegen/codegen.h"
#include "core/cfg.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"

namespace pyston {

void parseSlice(BoxedSlice* slice, int size, i64* out_start, i64* out_stop, i64* out_step, i64* out_length) {
    int ret = PySlice_GetIndicesEx((PySliceObject*)slice, size, out_start, out_stop, out_step, out_length);
    if (ret == -1)
        throwCAPIException();
}

bool isSliceIndex(Box* b) noexcept {
    return b->cls == none_cls || b->cls == int_cls || PyIndex_Check(b);
}

void adjustNegativeIndicesOnObject(Box* obj, i64* start_out, i64* stop_out) noexcept {
    i64 start = *start_out;
    i64 stop = *stop_out;
    PySequenceMethods* m;

    // Logic from PySequence_GetSlice:
    m = obj->cls->tp_as_sequence;
    if (m && m->sq_slice) {
        if (start < 0 || stop < 0) {
            if (m->sq_length) {
                Py_ssize_t l = (*m->sq_length)(obj);
                if (l >= 0) {
                    if (start < 0)
                        start += l;
                    if (stop < 0)
                        stop += l;
                }
            }
        }
    }

    *start_out = start;
    *stop_out = stop;
}

void boundSliceWithLength(i64* start_out, i64* stop_out, i64 start, i64 stop, i64 size) {
    if (start < 0)
        start = 0;
    else if (start > size)
        start = size;

    if (stop < start)
        stop = start;
    else if (stop > size)
        stop = size;

    assert(0 <= start && start <= stop && stop <= size);

    *start_out = start;
    *stop_out = stop;
}

Box* boxStringOrNone(const char* s) {
    if (s == NULL) {
        return None;
    } else {
        return boxString(s);
    }
}

Box* noneIfNull(Box* b) {
    if (b == NULL) {
        return None;
    } else {
        return b;
    }
}

template <ExceptionStyle S> Box* coerceUnicodeToStr(Box* unicode) noexcept(S == CAPI) {
    if (!isSubclass(unicode->cls, unicode_cls))
        return unicode;

    Box* r = PyUnicode_AsASCIIString(unicode);
    if (!r) {
        if (S == CAPI) {
            PyErr_SetString(TypeError, "Cannot use non-ascii unicode strings as attribute names or keywords");
            return NULL;
        } else {
            PyErr_Clear();
            raiseExcHelper(TypeError, "Cannot use non-ascii unicode strings as attribute names or keywords");
        }
    }

    return r;
}

// force instantiation:
template Box* coerceUnicodeToStr<CXX>(Box* unicode);
template Box* coerceUnicodeToStr<CAPI>(Box* unicode);

Box* boxStringFromCharPtr(const char* s) {
    return boxString(s);
}

extern "C" void dump(void* p) {
    dumpEx(p, 0);
}

extern "C" void dumpEx(void* p, int levels) {
    printf("\n");
    printf("Raw address: %p\n", p);

    bool is_gc = gc::isValidGCMemory(p);
    if (!is_gc) {
        printf("non-gc memory\n");
        return;
    }

    if (gc::isNonheapRoot(p)) {
        printf("Non-heap GC object\n");

        printf("Assuming it's a class object...\n");
        PyTypeObject* type = (PyTypeObject*)(p);
        printf("tp_name: %s\n", type->tp_name);
        return;
    }

    gc::GCAllocation* al = gc::GCAllocation::fromUserData(p);
    if (al->kind_id == gc::GCKind::UNTRACKED) {
        printf("gc-untracked object\n");
        return;
    }

    if (al->kind_id == gc::GCKind::PRECISE) {
        printf("pyston precise object\n");
        return;
    }

    if (al->kind_id == gc::GCKind::RUNTIME) {
        printf("pyston runtime object\n");
        return;
    }

    if (al->kind_id == gc::GCKind::CONSERVATIVE) {
        printf("conservatively-scanned object object\n");
        return;
    }

    if (al->kind_id == gc::GCKind::PYTHON) {
        printf("Python object\n");
        Box* b = (Box*)p;

        printf("Class: %s", getFullTypeName(b).c_str());
        if (b->cls->cls != type_cls) {
            printf(" (metaclass: %s)\n", getFullTypeName(b->cls).c_str());
        } else {
            printf("\n");
        }

        if (b->cls == bool_cls) {
            printf("The %s object\n", b == True ? "True" : "False");
        }

        if (PyType_Check(b)) {
            auto cls = static_cast<BoxedClass*>(b);
            printf("Type name: %s\n", getFullNameOfClass(cls).c_str());

            printf("MRO:");

            if (cls->tp_mro && cls->tp_mro->cls == tuple_cls) {
                bool first = true;
                for (auto b : *static_cast<BoxedTuple*>(cls->tp_mro)) {
                    if (!first)
                        printf(" ->");
                    first = false;
                    printf(" %s", getFullNameOfClass(static_cast<BoxedClass*>(b)).c_str());
                }
            }
            printf("\n");
        }

        if (PyString_Check(b)) {
            printf("String value: %s\n", static_cast<BoxedString*>(b)->data());
        }

        if (PyTuple_Check(b)) {
            BoxedTuple* t = static_cast<BoxedTuple*>(b);
            printf("%ld elements\n", t->size());

            if (levels > 0) {
                int i = 0;
                for (auto e : *t) {
                    printf("\nElement %d:", i);
                    i++;
                    dumpEx(e, levels - 1);
                }
            }
        }

        if (PyDict_Check(b)) {
            BoxedDict* d = static_cast<BoxedDict*>(b);
            printf("%d elements\n", d->d.size());

            if (levels > 0) {
                int i = 0;
                for (auto t : *d) {
                    printf("\nKey:");
                    dumpEx(t.first, levels - 1);
                    printf("Value:");
                    dumpEx(t.second, levels - 1);
                }
            }
        }

        if (PyInt_Check(b)) {
            printf("Int value: %ld\n", static_cast<BoxedInt*>(b)->n);
        }

        if (PyLong_Check(b)) {
            PyObject* str = longRepr(static_cast<BoxedLong*>(b));
            printf("Long value: %s\n", static_cast<BoxedString*>(str)->c_str());
        }

        if (PyList_Check(b)) {
            auto l = static_cast<BoxedList*>(b);
            printf("%ld elements\n", l->size);

            if (levels > 0) {
                int i = 0;
                for (int i = 0; i < l->size; i++) {
                    printf("\nElement %d:", i);
                    dumpEx(l->elts->elts[i], levels - 1);
                }
            }
        }

        if (isSubclass(b->cls, function_cls)) {
            BoxedFunction* f = static_cast<BoxedFunction*>(b);

            FunctionMetadata* md = f->md;
            if (md->source) {
                printf("User-defined function '%s'\n", md->source->getName()->c_str());
                printf("Defined at %s:%d\n", md->source->getFn()->c_str(), md->source->body[0]->lineno);

                if (md->source->cfg && levels > 0) {
                    md->source->cfg->print();
                }
            } else {
                printf("A builtin function\n");
            }

            printf("Has %ld function versions\n", md->versions.size());
            for (CompiledFunction* cf : md->versions) {
                bool got_name;
                if (cf->exception_style == CXX)
                    printf("CXX style: ");
                else
                    printf("CAPI style: ");
                std::string name = g.func_addr_registry.getFuncNameAtAddress(cf->code, true, &got_name);
                if (got_name)
                    printf("%s\n", name.c_str());
                else
                    printf("%p\n", cf->code);
            }
        }

        if (PyModule_Check(b)) {
            printf("The '%s' module\n", static_cast<BoxedModule*>(b)->name().c_str());
        }

        /*
        if (b->cls->instancesHaveHCAttrs()) {
            HCAttrs* attrs = b->getHCAttrsPtr();
            printf("Has %ld attrs\n", attrs->hcls->attr_offsets.size());
            for (const auto& p : attrs->hcls->attr_offsets) {
                printf("Index %d: %s: %p\n", p.second, p.first.c_str(), attrs->attr_list->attrs[p.second]);
            }
        }
        */

        return;
    }

    RELEASE_ASSERT(0, "%d", (int)al->kind_id);
}
}
