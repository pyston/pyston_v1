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

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

// These include orders come from CPython:
#include "patchlevel.h"
#include "pyconfig.h"

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
#include "stringobject.h"
#include "listobject.h"
#include "dictobject.h"
#include "tupleobject.h"
#include "methodobject.h"
#include "pycapsule.h"
#include "iterobject.h"
#include "descrobject.h"
#include "warnings.h"

#include "pyerrors.h"

#include "modsupport.h"
#include "import.h"

#include "abstract.h"

#include "pyctype.h"

// directly from CPython:
/* Argument must be a char or an int in [-128, 127] or [0, 255]. */
#define Py_CHARMASK(c)		((unsigned char)((c) & 0xff))

#include "pyfpe.h"

#ifdef __cplusplus
extern "C" {
#endif

PyObject* Py_BuildValue(const char*, ...);


PyObject* PyString_FromString(const char*);
PyObject* PyInt_FromLong(long);
int PyDict_SetItem(PyObject* mp, PyObject* key, PyObject* item);
int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item);

PyObject* PyModule_GetDict(PyObject*);
PyObject* PyDict_New(void);

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

PyObject* Py_InitModule4(const char *arg0, PyMethodDef *arg1, const char *arg2, PyObject *arg3, int arg4);
#define Py_InitModule(name, methods) \
	Py_InitModule4(name, methods, (char *)NULL, (PyObject *)NULL, \
		       PYTHON_API_VERSION)

#define Py_InitModule3(name, methods, doc) \
	Py_InitModule4(name, methods, doc, (PyObject *)NULL, \
		       PYTHON_API_VERSION)

#ifdef __cplusplus
}
#endif
#endif
