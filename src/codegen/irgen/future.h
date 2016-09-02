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

#ifndef PYSTON_CODEGEN_IRGEN_FUTURE_H
#define PYSTON_CODEGEN_IRGEN_FUTURE_H

#include "llvm/ADT/ArrayRef.h"

#include "core/types.h"

namespace pyston {

class AST_stmt;

// Loop through import statements to find __future__ imports throwing errors for
// bad __future__ imports. Returns the futures that are turned on. This is used
// for irgeneration; the parser still has to handle some futures on its own,
// when they are relevant for the parser.
FutureFlags getFutureFlags(llvm::ArrayRef<AST_stmt*> body, const char* file);
}

#endif
