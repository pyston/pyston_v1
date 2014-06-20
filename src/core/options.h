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
extern int PYTHON_VERSION_MAJOR, PYTHON_VERSION_MINOR, PYTHON_VERSION_MICRO, PYTHON_VERSION_HEX;

inline int version_hex(int major, int minor, int micro, int level = 0, int serial = 0) {
    return (major << 24) | (minor << 16) | (micro << 8) | (level << 4) | (serial << 0);
}

extern int MAX_OPT_ITERATIONS;

extern bool SHOW_DISASM, FORCE_OPTIMIZE, BENCH, PROFILE, DUMPJIT, TRAP, USE_STRIPPED_STDLIB, ENABLE_INTERPRETER,
    ENABLE_PYPA_PARSER;

extern bool ENABLE_ICS, ENABLE_ICGENERICS, ENABLE_ICGETITEMS, ENABLE_ICSETITEMS, ENABLE_ICDELITEMS, ENABLE_ICBINEXPS,
    ENABLE_ICNONZEROS, ENABLE_ICCALLSITES, ENABLE_ICSETATTRS, ENABLE_ICGETATTRS, ENALBE_ICDELATTRS, ENABLE_ICGETGLOBALS,
    ENABLE_SPECULATION, ENABLE_OSR, ENABLE_LLVMOPTS, ENABLE_INLINING, ENABLE_REOPT, ENABLE_PYSTON_PASSES,
    ENABLE_TYPE_FEEDBACK, ENABLE_FRAME_INTROSPECTION;

// Due to a temporary LLVM limitation, represent bools as i64's instead of i1's.
extern bool BOOLS_AS_I64;
}
}

#endif
