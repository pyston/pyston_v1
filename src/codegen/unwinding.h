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
Box* getGlobals();     // returns either the module or a globals dict
Box* getGlobalsDict(); // always returns a dict-like object

BoxedTraceback* getTraceback();

struct ExecutionPoint {
    CompiledFunction* cf;
    AST_stmt* current_stmt;
};
ExecutionPoint getExecutionPoint();

// Adds stack locals and closure locals into the locals dict, and returns it.
Box* fastLocalsToBoxedLocals();

class PythonFrameIteratorImpl;
class PythonFrameIterator {
private:
    std::unique_ptr<PythonFrameIteratorImpl> impl;

public:
    CompiledFunction* getCF();
    FrameInfo* getFrameInfo();
    bool exists() { return impl.get() != NULL; }
    std::unique_ptr<ExecutionPoint> getExecutionPoint();
    Box* fastLocalsToBoxedLocals();

    // Gets the "current version" of this frame: if the frame has executed since
    // the iterator was obtained, the methods may return old values. This returns
    // an updated copy that returns the updated values.
    // The "current version" will live at the same stack location, but any other
    // similarities need to be verified by the caller.
    // This function can only be called from the thread that created this object.
    PythonFrameIterator getCurrentVersion();

    PythonFrameIterator(PythonFrameIterator&& rhs);
    void operator=(PythonFrameIterator&& rhs);
    PythonFrameIterator(std::unique_ptr<PythonFrameIteratorImpl>&& impl);
    ~PythonFrameIterator();
};

PythonFrameIterator getPythonFrame(int depth);

// Fetches a writeable pointer to the frame-local excinfo object,
// calculating it if necessary (from previous frames).
ExcInfo* getFrameExcInfo();

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
