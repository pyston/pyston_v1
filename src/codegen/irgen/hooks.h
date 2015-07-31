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

#ifndef PYSTON_CODEGEN_IRGEN_HOOKS_H
#define PYSTON_CODEGEN_IRGEN_HOOKS_H

#include <string>

#include "core/types.h"

namespace pyston {

struct CompiledFunction;
class CLFunction;
class OSRExit;
class Box;
class BoxedDict;

CompiledFunction* compilePartialFuncInternal(OSRExit* exit);
void* compilePartialFunc(OSRExit*);
extern "C" CompiledFunction* reoptCompiledFuncInternal(CompiledFunction*);
extern "C" char* reoptCompiledFunc(CompiledFunction*);

class AST_Module;
class BoxedModule;
void compileAndRunModule(AST_Module* m, BoxedModule* bm);

// will we always want to generate unique function names? (ie will this function always be reasonable?)
CompiledFunction* cfForMachineFunctionName(const std::string&);

extern "C" void capiExcCaughtInJit(AST_stmt* current_stmt, void* source_info);
// This is just meant for the use of the JIT (normal runtime code should call throwCAPIException)
extern "C" void reraiseJitCapiExc() __attribute__((noreturn));

extern "C" Box* exec(Box* boxedCode, Box* globals, Box* locals, FutureFlags caller_future_flags);
extern "C" Box* eval(Box* boxedCode, Box* globals, Box* locals);
extern "C" Box* compile(Box* source, Box* filename, Box* mode, Box** _args /* flags, dont_inherit */);
}

#endif
