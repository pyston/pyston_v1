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

#ifndef PYSTON_CAPI_TYPEOBJECT_H
#define PYSTON_CAPI_TYPEOBJECT_H

#include "runtime/types.h"

namespace pyston {

/*
 * Type objects refer to Python's new-style classes that inherit from
 * `object`. This, classes declared as:
 *
 * class Foo(object):
 *  ...
 */

// Returns if a slot was updated
bool update_slot(BoxedClass* self, llvm::StringRef attr) noexcept;

void add_operators(BoxedClass* self) noexcept;
void fixup_slot_dispatchers(BoxedClass* self) noexcept;
void commonClassSetup(BoxedClass* cls);

// We need to expose these due to our different file organization (they
// are defined as part of the CPython copied into typeobject.c, but used
// from Pyston code).
// We could probably unify things more but that's for later.
PyTypeObject* best_base(PyObject* bases) noexcept;
PyObject* mro_external(PyObject* self) noexcept;
int type_set_bases(PyTypeObject* type, PyObject* value, void* context) noexcept;

PyObject* slot_tp_richcompare(PyObject* self, PyObject* other, int op) noexcept;
PyObject* slot_tp_iternext(PyObject* self) noexcept;
PyObject* slot_tp_new(PyTypeObject* self, PyObject* args, PyObject* kwds) noexcept;
PyObject* slot_mp_subscript(PyObject* self, PyObject* arg1) noexcept;
int slot_sq_contains(PyObject* self, PyObject* value) noexcept;
Py_ssize_t slot_sq_length(PyObject* self) noexcept;
PyObject* slot_tp_getattr_hook(PyObject* self, PyObject* name) noexcept;

class GetattrRewriteArgs;
Box* slotTpGetattrHookInternal(Box* self, BoxedString* attr, GetattrRewriteArgs* rewrite_args);
}

#endif
