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

} // namespace pyston
