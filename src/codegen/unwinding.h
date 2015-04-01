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

#ifndef PYSTON_CODEGEN_UNWINDING_H
#define PYSTON_CODEGEN_UNWINDING_H

#include <unordered_map>

#include "codegen/codegen.h"

namespace pyston {

class Box;
class BoxedDict;
class BoxedModule;
class BoxedTraceback;
struct FrameInfo;

BoxedModule* getCurrentModule();

BoxedTraceback* getTraceback();

// Adds stack locals and closure locals into the locals dict, and returns it.
Box* fastLocalsToBoxedLocals(int framesToSkip = 0);

// Fetches a writeable pointer to the frame-local excinfo object,
// calculating it if necessary (from previous frames).
ExcInfo* getFrameExcInfo();

struct ExecutionPoint {
    CompiledFunction* cf;
    AST_stmt* current_stmt;
};
ExecutionPoint getExecutionPoint();
std::unique_ptr<ExecutionPoint> getExecutionPoint(int framesToSkip);

struct FrameStackState {
    // This includes all # variables (but not the ! ones).
    // Therefore, it's not the same as the BoxedLocals.
    // This also means that it contains
    // CREATED_CLOSURE_NAME, PASSED_CLOSURE_NAME, and GENERATOR_NAME.
    BoxedDict* locals;

    // The frame_info is a pointer to the frame_info on the stack, so it is invalid
    // after the frame ends.
    FrameInfo* frame_info;

    FrameStackState(BoxedDict* locals, FrameInfo* frame_info) : locals(locals), frame_info(frame_info) {}
};

// Returns all the stack locals, including hidden ones.
FrameStackState getFrameStackState();
}

#endif
