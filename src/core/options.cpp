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

#include "core/options.h"

namespace pyston {

int GLOBAL_VERBOSITY = 1;

int PYTHON_VERSION_MAJOR = DEFAULT_PYTHON_MAJOR_VERSION;
int PYTHON_VERSION_MINOR = DEFAULT_PYTHON_MINOR_VERSION;
int PYTHON_VERSION_MICRO = DEFAULT_PYTHON_MICRO_VERSION;

int MAX_OPT_ITERATIONS = 1;

bool FORCE_OPTIMIZE = false;
bool SHOW_DISASM = false;
bool BENCH = false;
bool PROFILE = false;
bool DUMPJIT = false;
bool TRAP = false;
bool USE_STRIPPED_STDLIB = false;
bool ENABLE_INTERPRETER = true;

static bool _GLOBAL_ENABLE = 1;
bool ENABLE_ICS = 1 && _GLOBAL_ENABLE;
bool ENABLE_ICGENERICS = 1 && ENABLE_ICS;
bool ENABLE_ICGETITEMS = 1 && ENABLE_ICS;
bool ENABLE_ICSETITEMS = 1 && ENABLE_ICS;
bool ENABLE_ICDELITEMS = 1 && ENABLE_ICS;
bool ENABLE_ICCALLSITES = 1 && ENABLE_ICS;
bool ENABLE_ICSETATTRS = 1 && ENABLE_ICS;
bool ENABLE_ICGETATTRS = 1 && ENABLE_ICS;
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
}
