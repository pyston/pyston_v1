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

#ifndef PYSTON_CODEGEN_PARSER_H
#define PYSTON_CODEGEN_PARSER_H

#include "core/types.h"

namespace pyston {

class AST_Module;
class ASTAllocator;

std::pair<AST_Module*, std::unique_ptr<ASTAllocator>> parse_string(const char* code, FutureFlags inherited_flags);
std::pair<AST_Module*, std::unique_ptr<ASTAllocator>> parse_file(const char* fn, FutureFlags inherited_flags);
std::pair<AST_Module*, std::unique_ptr<ASTAllocator>> caching_parse_file(const char* fn, FutureFlags inherited_flags);
}

#endif
