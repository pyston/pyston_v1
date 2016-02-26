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

// Forward-declare libunwind's typedef'd unw_cursor_t:
struct unw_cursor;
typedef struct unw_cursor unw_cursor_t;

namespace pyston {

class Box;
class BoxedDict;
class BoxedModule;
class BoxedTraceback;
struct FrameInfo;

void registerDynamicEhFrame(uint64_t code_addr, size_t code_size, uint64_t eh_frame_addr, size_t eh_frame_size);

void setupUnwinding();
BoxedModule* getCurrentModule();
Box* getGlobals();     // returns either the module or a globals dict
Box* getGlobalsDict(); // always returns a dict-like object
CompiledFunction* getCFForAddress(uint64_t addr);

Box* getTraceback();

class PythonUnwindSession;
PythonUnwindSession* beginPythonUnwindSession();
PythonUnwindSession* getActivePythonUnwindSession();
void endPythonUnwindSession(PythonUnwindSession* unwind_session);
void* getPythonUnwindSessionExceptionStorage(PythonUnwindSession* unwind_session);
void unwindingThroughFrame(PythonUnwindSession* unwind_session, unw_cursor_t* cursor);

// TODO move these to exceptions.h
void logException(ExcInfo* exc_info);
void startReraise();
bool exceptionAtLineCheck();
void exceptionAtLine(LineInfo line_info, Box** traceback);
void caughtCxxException(LineInfo line_info, ExcInfo* exc_info);
extern "C" void caughtCapiException(AST_stmt* current_stmt, void* source_info);
extern "C" void reraiseCapiExcAsCxx() __attribute__((noreturn));


FunctionMetadata* getTopPythonFunction();

// debugging/stat helper, returns python filename:linenumber, or "unknown:-1" if it fails
std::string getCurrentPythonLine();

// doesn't really belong in unwinding.h, since it's stats related, but it needs to unwind to get the current line...
void logByCurrentPythonLine(const std::string& stat_name);

// Adds stack locals and closure locals into the locals dict, and returns it.
Box* fastLocalsToBoxedLocals();

class PythonFrameIteratorImpl;
class PythonFrameIterator {
private:
    std::unique_ptr<PythonFrameIteratorImpl> impl;

public:
    CompiledFunction* getCF();
    FunctionMetadata* getMD();
    FrameInfo* getFrameInfo();
    bool exists() { return impl.get() != NULL; }
    AST_stmt* getCurrentStatement();
    Box* getGlobalsDict();

    PythonFrameIterator(PythonFrameIterator&& rhs);
    void operator=(PythonFrameIterator&& rhs);
    PythonFrameIterator(std::unique_ptr<PythonFrameIteratorImpl> impl);
    ~PythonFrameIterator();
};

FrameInfo* getPythonFrameInfo(int depth);

// Fetches a writeable pointer to the frame-local excinfo object,
// calculating it if necessary (from previous frames).
ExcInfo* getFrameExcInfo();
// A similar function, but that takes a pointer to the most-recent ExcInfo.
// This is faster in the case that the frame-level excinfo is already up-to-date,
// but just as slow if it's not.
void updateFrameExcInfoIfNeeded(ExcInfo* latest);

struct FrameStackState {
    // This includes all # variables (but not the ! ones).
    // Therefore, it's not the same as the BoxedLocals.
    // This also means that it contains
    // CREATED_CLOSURE_NAME, PASSED_CLOSURE_NAME, and GENERATOR_NAME.
    BoxedDict* locals;

    // The frame_info is a pointer to the frame_info on the stack, so it is invalid
    // after the frame ends.
    FrameInfo* frame_info;

    FrameStackState() {}
    FrameStackState(BoxedDict* locals, FrameInfo* frame_info) : locals(locals), frame_info(frame_info) {}
};

// Returns all the stack locals, including hidden ones.
FrameStackState getFrameStackState();

struct DeoptState {
    FrameStackState frame_state;
    CompiledFunction* cf;
    AST_stmt* current_stmt;
};
DeoptState getDeoptState();
}

#endif
