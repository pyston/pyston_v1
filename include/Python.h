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
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

//struct PyObject {
//};
//typedef struct PyObject PyObject;
typedef void PyObject;

#define Py_INCREF(x)
bool PyArg_ParseTuple(PyObject*, const char*, ...);
PyObject* Py_BuildValue(const char*, ...);

typedef PyObject *(*PyCFunction)(PyObject *, PyObject *);
struct PyMethodDef {
    const char  *ml_name;   /* The name of the built-in function/method */
    PyCFunction  ml_meth;   /* The C function that implements it */
    int          ml_flags;  /* Combination of METH_xxx flags, which mostly
                               describe the args expected by the C func */
    const char  *ml_doc;    /* The __doc__ attribute, or NULL */
};
typedef struct PyMethodDef PyMethodDef;

#define METH_VARARGS  0x0001

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

#endif
