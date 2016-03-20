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

#ifndef PYSTON_CODEGEN_PYPAPARSER_H
#define PYSTON_CODEGEN_PYPAPARSER_H

#include <cstdio>

#include "core/types.h"

namespace pyston {
class AST_Module;
AST_Module* pypa_parse(char const* file_path, FutureFlags future_flags);
AST_Module* pypa_parse_string(char const* str, FutureFlags future_flags);
}

#endif // PYSTON_CODEGEN_PYPAPARSER_H
