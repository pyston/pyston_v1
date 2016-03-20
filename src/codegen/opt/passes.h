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

#ifndef PYSTON_CODEGEN_OPT_PASSES_H
#define PYSTON_CODEGEN_OPT_PASSES_H

namespace llvm {
class BasicBlockPass;
class FunctionPass;
class ImmutablePass;
}

namespace pyston {
llvm::ImmutablePass* createPystonAAPass();
llvm::FunctionPass* createMallocsNonNullPass();
llvm::FunctionPass* createConstClassesPass();
llvm::FunctionPass* createDeadAllocsPass();
llvm::FunctionPass* createRemoveUnnecessaryBoxingPass();
llvm::BasicBlockPass* createRemoveDuplicateBoxingPass();
}

#endif
