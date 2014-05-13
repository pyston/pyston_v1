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

#ifndef PYSTON_CORE_OPTIONS_H
#define PYSTON_CORE_OPTIONS_H

namespace pyston {

extern "C" {

extern int GLOBAL_VERBOSITY;
#define VERBOSITY(x) GLOBAL_VERBOSITY
// Version number we're targeting:
extern int PYTHON_VERSION_MAJOR, PYTHON_VERSION_MINOR, PYTHON_VERSION_MICRO;

extern int MAX_OPT_ITERATIONS;

extern bool SHOW_DISASM, FORCE_OPTIMIZE, BENCH, PROFILE, DUMPJIT, TRAP, USE_STRIPPED_STDLIB, ENABLE_INTERPRETER;

extern bool ENABLE_ICS, ENABLE_ICGENERICS, ENABLE_ICGETITEMS, ENABLE_ICSETITEMS, ENABLE_ICDELITEMS, ENABLE_ICBINEXPS,
    ENABLE_ICNONZEROS, ENABLE_ICCALLSITES, ENABLE_ICSETATTRS, ENABLE_ICGETATTRS, ENABLE_ICGETGLOBALS,
    ENABLE_SPECULATION, ENABLE_OSR, ENABLE_LLVMOPTS, ENABLE_INLINING, ENABLE_REOPT, ENABLE_PYSTON_PASSES,
    ENABLE_TYPE_FEEDBACK;
}
}

#endif
