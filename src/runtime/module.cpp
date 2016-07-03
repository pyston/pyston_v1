// Copyright (c) 2014-2016 Dropbox, Inc.
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
#include "core/types.h"
#include "runtime/types.h"

namespace pyston {

static int module_init(PyObject* m, PyObject* args, PyObject* kwds) {
    static char* kwlist[3] = { NULL, NULL, NULL };
    kwlist[0] = const_cast<char*>("name");
    kwlist[1] = const_cast<char*>("doc");
    PyObject* name = Py_None, * doc = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "S|O:module.__init__", kwlist, &name, &doc))
        return -1;

    try {
        Box* r = moduleInit(static_cast<BoxedModule*>(m), static_cast<BoxedString*>(name), doc == Py_None ? NULL : doc);
        Py_DECREF(r);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

void setupModule() {
    module_cls->tp_init = module_init;
}
}
