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

#ifndef PYSTON_CODEGEN_IRGEN_UTIL_H
#define PYSTON_CODEGEN_IRGEN_UTIL_H

#include <string>

namespace llvm {
class Constant;
class Function;
class Type;
}

namespace pyston {

llvm::Constant* getStringConstantPtr(const std::string& str);
llvm::Constant* getStringConstantPtr(const char* str);
llvm::Constant* embedConstantPtr(const void* addr, llvm::Type*);
llvm::Constant* getConstantInt(int val);
llvm::Constant* getConstantInt(int val, llvm::Type*);

void dumpPrettyIR(llvm::Function* f);
}

#endif
