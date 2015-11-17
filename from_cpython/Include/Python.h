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

#ifndef PYSTON_EXTINCLUDE_PYTHON_H
#define PYSTON_EXTINCLUDE_PYTHON_H

// Cython depends on having this define set:
#define Py_PYTHON_H

#define PYSTON_VERSION "0.5"

// XXX: testing
#ifndef NDEBUG
#define Py_REF_DEBUG
#define WITH_PYMALLOC
#define PYMALLOC_DEBUG
#endif

// These include orders come from CPython:
#include "patchlevel.h"
#include "pyconfig.h"

#include <limits.h>

#include <stdio.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_STDDEF_H
#include <stddef.h>
#endif

#include <assert.h>

#include "pyport.h"

#include "pymath.h"
#include "pymem.h"

#include "object.h"
#include "objimpl.h"

#include "pydebug.h"

#include "unicodeobject.h"
#include "intobject.h"
#include "boolobject.h"
#include "longobject.h"
#include "floatobject.h"
#ifndef WITHOUT_COMPLEX
#include "complexobject.h"
#endif
#include "memoryobject.h"
#include "stringobject.h"
#include "bufferobject.h"
#include "bytesobject.h"
#include "bytearrayobject.h"
#include "tupleobject.h"
#include "listobject.h"
#include "dictobject.h"
#include "setobject.h"
#include "methodobject.h"
#include "moduleobject.h"
#include "funcobject.h"
#include "classobject.h"
#include "cobject.h"
#include "fileobject.h"
#include "pycapsule.h"
#include "traceback.h"
#include "sliceobject.h"
#include "iterobject.h"
#include "genobject.h"
#include "descrobject.h"
#include "warnings.h"
#include "weakrefobject.h"

// Pyston additions:
// These new APIS give access to our fast hidden-class-based attributes implementation.
// Ideally in the future this will just be "storage strategy" of dicts and all Python
// dicts will benefit from it, but for now classes have to explicitly opt-in to having
// these kinds of attrs.
struct _hcattrs {
    char _data[16];
};
#ifndef _PYSTON_API
typedef struct _hcattrs PyHcAttrs;
#else
namespace pyston {
class HCAttrs;
}
typedef int PyHcAttrs;
#endif
PyAPI_FUNC(void) PyObject_InitHcAttrs(PyHcAttrs*) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject*) PyObject_GetAttrWrapper(PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(void) PyType_RequestHcAttrs(PyTypeObject*, int offset) PYSTON_NOEXCEPT;
// Sets a descriptor on the type so that the attrs are available via __dict__
PyAPI_FUNC(void) PyType_GiveHcAttrsDictDescr(PyTypeObject*) PYSTON_NOEXCEPT;
// These functions directly manipulate the hcattrs storage, bypassing any getattro
// or descriptor logic.  This is the equivallent of callling PyDict_GetItemString
// on an instance's dict.
// These functions try to mimic the Dict versions as much as possible, so for example
// the PyObject_GetHcAttrString function does not set an exception.
PyAPI_FUNC(PyObject*) PyObject_GetHcAttrString(PyObject*, const char*) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyObject_SetHcAttrString(PyObject*, const char*, PyObject*) PYSTON_NOEXCEPT;
PyAPI_FUNC(int) PyObject_DelHcAttrString(PyObject*, const char*) PYSTON_NOEXCEPT;

// Workaround: call this instead of setting tp_dict.
PyAPI_FUNC(void) PyType_SetDict(PyTypeObject*, PyObject*) PYSTON_NOEXCEPT;

// Pyston addition: register an object as a "static constant".  Current purpose is that this will
// get decref'd when the interpreter shuts down.
// PyType_Ready calls this automatically.
PyAPI_FUNC(void) PyGC_RegisterStaticConstant(PyObject*) PYSTON_NOEXCEPT;

#include "codecs.h"
#include "pyerrors.h"

#include "pystate.h"

#include "pyarena.h"
#include "modsupport.h"
#include "pythonrun.h"
#include "ceval.h"
#include "sysmodule.h"
#include "intrcheck.h"
#include "import.h"

#include "abstract.h"

#include "compile.h"

#include "pyctype.h"
#include "pystrtod.h"
#include "pystrcmp.h"
#include "dtoa.h"

// directly from CPython:
/* Argument must be a char or an int in [-128, 127] or [0, 255]. */
#define Py_CHARMASK(c)		((unsigned char)((c) & 0xff))

#include "pyfpe.h"

#include "code.h"

#define Py_single_input 256
#define Py_file_input 257
#define Py_eval_input 258

#ifdef __cplusplus
extern "C" {
#endif

PyObject* PyModule_GetDict(PyObject*) PYSTON_NOEXCEPT;

// Pyston change : expose these type objects
extern PyTypeObject Pattern_Type;
extern PyTypeObject Match_Type;
extern PyTypeObject Scanner_Type;

extern PyTypeObject* Itertool_SafeDealloc_Types[];

// In CPython this is in frameobject.h:
PyAPI_FUNC(int) PyFrame_ClearFreeList(void) PYSTON_NOEXCEPT;

#define PyDoc_VAR(name) static char name[]
#define PyDoc_STRVAR(name, str) PyDoc_VAR(name) = PyDoc_STR(str)
#define PyDoc_STR(str) str

#ifdef __cplusplus
#define PyMODINIT_FUNC extern "C" void
#else
#define PyMODINIT_FUNC void
#endif

#define PYTHON_API_VERSION 1013
#define PYTHON_API_STRING "1013"

#ifdef __cplusplus
}
#endif
#endif
