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

#ifndef PYSTON_CODEGEN_IRGEN_HOOKS_H
#define PYSTON_CODEGEN_IRGEN_HOOKS_H

#include <string>

namespace pyston {

struct CompiledFunction;
class CLFunction;
class OSRExit;

CompiledFunction* compilePartialFuncInternal(OSRExit* exit);
void* compilePartialFunc(OSRExit*);
extern "C" CompiledFunction* reoptCompiledFuncInternal(CompiledFunction*);
extern "C" char* reoptCompiledFunc(CompiledFunction*);

class AST_Module;
class BoxedModule;
void compileAndRunModule(AST_Module* m, BoxedModule* bm);

// will we always want to generate unique function names? (ie will this function always be reasonable?)
CompiledFunction* cfForMachineFunctionName(const std::string&);
}

#endif
