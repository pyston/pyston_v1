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

#ifndef PYSTON_CODEGEN_IRGEN_FUTURE_H
#define PYSTON_CODEGEN_IRGEN_FUTURE_H

#include <map>

#include "core/ast.h"
#include "core/options.h"
#include "core/types.h"

namespace pyston {

#define FF_ABSOLUTE_IMPORT 0x01
#define FF_DIVISION 0x02
#define FF_GENERATOR 0x04
#define FF_UNICODE_LITERALS 0x08
#define FF_PRINT_FUNCTIONS 0x10
#define FF_NESTED_SCOPES 0x20
#define FF_WITH_STATEMENT 0x40

struct FutureOption {
    int optional_version_hex;
    int mandatory_version_hex;
    int ff_mask;
};

typedef int FutureFlags;

const std::map<std::string, FutureOption> future_options
    = { { "absolute_import", { version_hex(2, 5, 0), version_hex(3, 0, 0), FF_ABSOLUTE_IMPORT } },
        { "division", { version_hex(2, 2, 0), version_hex(3, 0, 0), FF_DIVISION } },
        { "generators", { version_hex(2, 2, 0), version_hex(3, 0, 0), FF_GENERATOR } },
        { "unicode_literals", { version_hex(2, 6, 0), version_hex(3, 0, 0), FF_UNICODE_LITERALS } },
        { "print_functions", { version_hex(2, 6, 0), version_hex(3, 0, 0), FF_PRINT_FUNCTIONS } },
        { "nested_scopes", { version_hex(2, 1, 0), version_hex(2, 2, 0), FF_NESTED_SCOPES } },
        { "with_statement", { version_hex(2, 5, 0), version_hex(3, 6, 0), FF_WITH_STATEMENT } } };

// Loop through import statements to find __future__ imports throwing errors for
// bad __future__ imports. Returns the futures that are turned on. This is used
// for irgeneration; the parser still has to handle some futures on its own,
// when they are relevant for the parser.
FutureFlags getFutureFlags(AST_Module* m, const char* file);
}

#endif
