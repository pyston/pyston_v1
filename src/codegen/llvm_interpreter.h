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

#ifndef PYSTON_CODEGEN_LLVMINTERPRETER_H
#define PYSTON_CODEGEN_LLVMINTERPRETER_H

namespace llvm {
class Function;
}

namespace pyston {

class Box;
class GCVisitor;
class LineInfo;

Box* interpretFunction(llvm::Function* f, int nargs, Box* closure, Box* arg1, Box* arg2, Box* arg3, Box** args);

void gatherInterpreterRoots(GCVisitor* visitor);
const LineInfo* getLineInfoForInterpretedFrame(void* frame_ptr);
}

#endif
