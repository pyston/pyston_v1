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

#include "core/options.h"

namespace pyston {

int GLOBAL_VERBOSITY = 0;

int PYSTON_VERSION_MAJOR = 0;
int PYSTON_VERSION_MINOR = 5;
int PYSTON_VERSION_MICRO = 0;

int MAX_OPT_ITERATIONS = 1;

bool LOG_IC_ASSEMBLY = 0;
bool LOG_BJIT_ASSEMBLY = 0;

bool FORCE_INTERPRETER = false;
bool FORCE_OPTIMIZE = false;
bool ENABLE_INTERPRETER = true;
bool ENABLE_BASELINEJIT = true;

bool CONTINUE_AFTER_FATAL = false;
bool SHOW_DISASM = false;
bool PROFILE = false;
bool DUMPJIT = false;
bool TRAP = false;
bool USE_STRIPPED_STDLIB = true; // always true
bool ENABLE_PYPA_PARSER = true;
bool ENABLE_CPYTHON_PARSER = true;
bool USE_REGALLOC_BASIC = false;
bool PAUSE_AT_ABORT = false;
bool ENABLE_TRACEBACKS = true;

// Forces the llvm jit to use capi exceptions whenever it can, as opposed to whenever it thinks
// it is faster.  The CALLS version is for calls that the llvm jit will make, and the THROWS version
// is for the exceptions it will throw.
bool FORCE_LLVM_CAPI_CALLS = false;
bool FORCE_LLVM_CAPI_THROWS = false;

int OSR_THRESHOLD_INTERPRETER = 25;
int REOPT_THRESHOLD_INTERPRETER = 25;
int OSR_THRESHOLD_BASELINE = 2500;
int REOPT_THRESHOLD_BASELINE = 1500;
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
bool ENABLE_PYSTON_PASSES = 0 && _GLOBAL_ENABLE;
bool ENABLE_TYPE_FEEDBACK = 1 && _GLOBAL_ENABLE;
bool ENABLE_RUNTIME_ICS = 1 && _GLOBAL_ENABLE;
bool ENABLE_JIT_OBJECT_CACHE = 1 && _GLOBAL_ENABLE;
bool LAZY_SCOPING_ANALYSIS = 1;

bool ENABLE_FRAME_INTROSPECTION = 1;

extern "C" {
int Py_FrozenFlag = 1;
int Py_IgnoreEnvironmentFlag = 0;
int Py_InteractiveFlag = 0;
int Py_InspectFlag = 0;
int Py_NoSiteFlag = 0;
int Py_OptimizeFlag = 0;
int Py_VerboseFlag = 0;
int Py_UnicodeFlag = 0;
}
}
