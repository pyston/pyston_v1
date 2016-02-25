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

#include <unordered_map>
#include <vector>
#include <ctime>

#include <Python.h>
#include <structmember.h>
#include <frameobject.h>

static PyObject* trace_func;

/*
CC=/home/kmod/pyston_deps/llvm-trunk-build/Release/bin/clang++ CFLAGS='-Wno-write-strings -std=c++11' python setup.py build && time LD_PRELOAD='/home/kmod/pyston_deps/gcc-4.8.2-install/lib64/libstdc++.so' python test.py ../../minibenchmarks/raytrace.py
*/

namespace std {
template <typename T1, typename T2> struct hash<pair<T1, T2> > {
    size_t operator()(const pair<T1, T2> p) const { return hash<T1>()(p.first) ^ (hash<T2>()(p.second) << 1); }
};
}
//struct Position : public std::pair<const char*, int> {
    //constexpr Position (const char* c, const char*
//};
typedef std::pair<const char*, int> Position;

static std::unordered_map<Position, double> times;
static double* next_time = NULL;
static double prev_time;
static double start_time;

static std::vector<Position> call_stack;

double floattime() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (double)t.tv_sec + t.tv_usec*0.000001;
}

static double time_to_log = 0.0;
// Run calibrate.py and divide the time accounted to calibrate.py:2 by the loop count (10M)
#define CALIBRATION_CONSTANT 0.0000001

// Some benchmarks remove their warmup time from their results, so make sure to match that:
#define WARMUP_TIME 0

static PyObject *
trace(PyObject *self, PyObject *args)
{
    double curtime = floattime();
#if WARMUP_TIME > 0
    if (curtime < start_time + WARMUP_TIME) {
        Py_INCREF(trace_func);
        return trace_func;
    }
#endif

    bool log = false;
    if (next_time) {
        double f = (curtime - prev_time);
        f -= CALIBRATION_CONSTANT;
        *next_time += f;

        time_to_log -= f;
        if (time_to_log < 0) {
            time_to_log = 0.1;
            log = true;
        }
    }

    PyObject* _frame;
    char* event;
    PyObject* arg;
    if (!PyArg_ParseTuple(args, "OsO", &_frame, &event, &arg))
        return NULL;

    assert(PyFrame_Check(_frame));
    PyFrameObject* frame = (PyFrameObject*)_frame;

    PyObject* fn = frame->f_code->co_filename;
    char* fn_str = PyString_AsString(fn);
    if (log)
        printf("'%s': %s:%d (%p)\n", event, fn_str, frame->f_lineno, frame->f_back);

    if (strcmp(event, "call") == 0) {
        Position p(fn_str, frame->f_lineno);
        call_stack.push_back(p);
        next_time = &times[p];
    } else if (strcmp(event, "line") == 0 || strcmp(event, "exception") == 0) {
        Position p(fn_str, frame->f_lineno);
        next_time = &times[p];
    } else if (strcmp(event, "return") == 0) {
        Position p = call_stack.back();
        call_stack.pop_back();
        next_time = &times[p];
    } else {
        printf("unknown event: %s\n", event);
        PyErr_SetString(PyExc_RuntimeError, "unknown event!");
        Py_FatalError("Unknown event!");
        return NULL;
    }

    prev_time = floattime();
    Py_INCREF(trace_func);
    return trace_func;
}

static PyObject *
get_times(PyObject* self, PyObject* args)
{
    if (!PyArg_ParseTuple(args, ""))
        return NULL;

    PyObject* d = PyDict_New();
    for (auto& p : times) {
        // TODO reference handling
        PyObject* fn = PyString_FromString(p.first.first);
        PyObject* lineno = PyInt_FromLong(p.first.second);
        PyObject* tuple = PyTuple_Pack(2, fn, lineno);
        PyObject* val = PyFloat_FromDouble(p.second);

        PyDict_SetItem(d, tuple, val);
    }

    return d;
}

static PyMethodDef MeasureLocMethods[] = {
    {"trace", trace, METH_VARARGS, "Tracer."},
    {"get_times", get_times, METH_VARARGS, "Get logged times."},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

PyMODINIT_FUNC
initmeasure_loc_ext(void)
{
    start_time = floattime();
    PyObject *m;

    m = Py_InitModule("measure_loc_ext", MeasureLocMethods);
    if (m == NULL)
        return;
    trace_func = PyObject_GetAttrString(m, "trace");
    if (trace_func == NULL)
        return;
}
