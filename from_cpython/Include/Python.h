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

// Cython has some "not targeting CPython" support that is triggered by having PYPY_VERSION defined:
#define PYPY_VERSION "Pyston"

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

// CPython doesn't seem to include this but I'm not sure how they get the definition of 'bool':
#include <stdbool.h>

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
#include "descrobject.h"
#include "warnings.h"
#include "weakrefobject.h"

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

#include "pyctype.h"
#include "pystrtod.h"
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

// Pyston addition:
// Our goal is to not make exception modules declare their static memory.  But until we can identify
// that in an automated way, we have to modify extension modules to call this:
PyObject* PyGC_AddRoot(PyObject*) PYSTON_NOEXCEPT;
// PyGC_AddRoot returns its argument, with the intention that you do something like
//      static PyObject* obj = PyGC_AddRoot(foo());
// rather than
//      static PyObject* obj = foo();
//      PyGC_AddRoot(obj);
// to reduce any chances of compiler reorderings or a GC somehow happening between the assignment
// to the static slot and the call to PyGC_AddRoot.

#define PyDoc_VAR(name) static char name[]
#define PyDoc_STRVAR(name, str) PyDoc_VAR(name) = PyDoc_STR(str)
#define PyDoc_STR(str) str

// This is in Python-ast.h in CPython, which we don't yet have:
int PyAST_Check(PyObject* obj) PYSTON_NOEXCEPT;

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
