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

#include "core/options.h"

namespace pyston {

int GLOBAL_VERBOSITY = 0;

int PYSTON_VERSION_MAJOR = 0;
int PYSTON_VERSION_MINOR = 3;

int PYTHON_VERSION_MAJOR = DEFAULT_PYTHON_MAJOR_VERSION;
int PYTHON_VERSION_MINOR = DEFAULT_PYTHON_MINOR_VERSION;
int PYTHON_VERSION_MICRO = DEFAULT_PYTHON_MICRO_VERSION;

int PYTHON_VERSION_HEX = version_hex(PYTHON_VERSION_MAJOR, PYTHON_VERSION_MINOR, PYTHON_VERSION_MICRO);

int MAX_OPT_ITERATIONS = 1;

bool CONTINUE_AFTER_FATAL = false;
bool FORCE_INTERPRETER = false;
bool FORCE_OPTIMIZE = false;
bool SHOW_DISASM = false;
bool PROFILE = false;
bool DUMPJIT = false;
bool TRAP = false;
bool USE_STRIPPED_STDLIB = true; // always true
bool ENABLE_INTERPRETER = true;
bool ENABLE_PYPA_PARSER = true;
bool USE_REGALLOC_BASIC = true;
bool PAUSE_AT_ABORT = false;
bool ENABLE_TRACEBACKS = true;

int OSR_THRESHOLD_INTERPRETER = 500;
int REOPT_THRESHOLD_INTERPRETER = 200;
int OSR_THRESHOLD_BASELINE = 10000;
int REOPT_THRESHOLD_BASELINE = 250;
int OSR_THRESHOLD_T2 = 10000;
int REOPT_THRESHOLD_T2 = 10000;
int SPECULATION_THRESHOLD = 100;

int MAX_OBJECT_CACHE_ENTRIES = 500;

static bool _GLOBAL_ENABLE = 1;
bool ENABLE_ICS = 1 && _GLOBAL_ENABLE;
bool ENABLE_ICGENERICS = 1 && ENABLE_ICS;
bool ENABLE_ICGETITEMS = 1 && ENABLE_ICS;
bool ENABLE_ICSETITEMS = 1 && ENABLE_ICS;
bool ENABLE_ICDELITEMS = 1 && ENABLE_ICS;
bool ENABLE_ICCALLSITES = 1 && ENABLE_ICS;
bool ENABLE_ICSETATTRS = 1 && ENABLE_ICS;
bool ENABLE_ICGETATTRS = 1 && ENABLE_ICS;
bool ENALBE_ICDELATTRS = 1 && ENABLE_ICS;
bool ENABLE_ICGETGLOBALS = 1 && ENABLE_ICS;
bool ENABLE_ICBINEXPS = 1 && ENABLE_ICS;
bool ENABLE_ICNONZEROS = 1 && ENABLE_ICS;
bool ENABLE_SPECULATION = 1 && _GLOBAL_ENABLE;
bool ENABLE_OSR = 1 && _GLOBAL_ENABLE;
bool ENABLE_LLVMOPTS = 1 && _GLOBAL_ENABLE;
bool ENABLE_INLINING = 1 && _GLOBAL_ENABLE;
bool ENABLE_REOPT = 1 && _GLOBAL_ENABLE;
bool ENABLE_PYSTON_PASSES = 1 && _GLOBAL_ENABLE;
bool ENABLE_TYPE_FEEDBACK = 1 && _GLOBAL_ENABLE;
bool ENABLE_RUNTIME_ICS = 1 && _GLOBAL_ENABLE;
bool ENABLE_JIT_OBJECT_CACHE = 1 && _GLOBAL_ENABLE;

bool ENABLE_FRAME_INTROSPECTION = 1;
bool BOOLS_AS_I64 = ENABLE_FRAME_INTROSPECTION;

extern "C" {
int Py_FrozenFlag = 1;
int Py_IgnoreEnvironmentFlag = 0;
int Py_InspectFlag = 0;
int Py_NoSiteFlag = 0;
int Py_OptimizeFlag = 0;
int Py_VerboseFlag = 0;
}
}
