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

#ifndef PYSTON_CODEGEN_IRGEN_UTIL_H
#define PYSTON_CODEGEN_IRGEN_UTIL_H

#include <string>

#include "llvm/ADT/StringRef.h"

namespace llvm {
class Constant;
class Function;
class Type;
}

namespace pyston {

llvm::Constant* getStringConstantPtr(const std::string& str);
llvm::Constant* getStringConstantPtr(const char* str);
llvm::Constant* embedRelocatablePtr(const void* addr, llvm::Type*, llvm::StringRef shared_name = llvm::StringRef());
llvm::Constant* embedConstantPtr(const void* addr, llvm::Type*);
llvm::Constant* getConstantInt(int64_t val);
llvm::Constant* getConstantDouble(double val);
llvm::Constant* getConstantInt(int64_t val, llvm::Type*);
llvm::Constant* getNullPtr(llvm::Type* t);

void clearRelocatableSymsMap();
const void* getValueOfRelocatableSym(const std::string& str);

void dumpPrettyIR(llvm::Function* f);
}

#endif
