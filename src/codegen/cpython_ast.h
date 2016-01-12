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

#ifndef PYSTON_CODEGEN_CPYTHONAST_H
#define PYSTON_CODEGEN_CPYTHONAST_H

#include "Python.h"
#include "Python-ast.h"

#include "core/ast.h"

namespace pyston {

// Convert a CPython ast object to a Pyston ast object.
// This will also check for certain kinds of "syntax errors" (ex continue not in loop) and will
// throw them as C++ exceptions.
AST* cpythonToPystonAST(mod_ty mod, llvm::StringRef fn);
}

#endif
