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
struct FrameInfo;

class RegisterEHFrame {
private:
    void* dyn_info;

public:
    RegisterEHFrame() : dyn_info(NULL) {}
    ~RegisterEHFrame() { deregisterFrame(); }

    // updates the EH info at eh_frame_addr to reference the passed code addr and code size and registers it
    void updateAndRegisterFrameFromTemplate(uint64_t code_addr, size_t code_size, uint64_t eh_frame_addr,
                                            size_t eh_frame_size);

    void registerFrame(uint64_t code_addr, size_t code_size, uint64_t eh_frame_addr, size_t eh_frame_size);
    void deregisterFrame();
};

void* registerDynamicEhFrame(uint64_t code_addr, size_t code_size, uint64_t eh_frame_addr, size_t eh_frame_size);
void deregisterDynamicEhFrame(void* dyn_info);
uint64_t getCXXUnwindSymbolAddress(llvm::StringRef sym);

// use this instead of std::uncaught_exception.
// Highly discouraged except for asserting -- we could be processing
// a destructor with decref'd something and then we called into more
// Python code.  So it's impossible to tell for instance, if a destructor
// was called due to an exception or due to normal function termination,
// since the latter can still return isUnwinding==true if there is an
// exception up in the stack.
#ifndef NDEBUG
bool isUnwinding();
#endif

void setupUnwinding();
BORROWED(BoxedModule*) getCurrentModule();
BORROWED(Box*) getGlobals();     // returns either the module or a globals dict
BORROWED(Box*) getGlobalsDict(); // always returns a dict-like object
CompiledFunction* getCFForAddress(uint64_t addr);

class PythonUnwindSession;
PythonUnwindSession* beginPythonUnwindSession();
PythonUnwindSession* getActivePythonUnwindSession();
void endPythonUnwindSession(PythonUnwindSession* unwind_session);
void* getPythonUnwindSessionExceptionStorage(PythonUnwindSession* unwind_session);
void unwindingThroughFrame(PythonUnwindSession* unwind_session, unw_cursor_t* cursor);

// TODO move these to exceptions.h
void logException(ExcInfo* exc_info);
bool& getIsReraiseFlag();
inline void startReraise() {
    getIsReraiseFlag() = true;
}
void exceptionAtLine(Box** traceback);
void caughtCxxException(ExcInfo* exc_info);
extern "C" void caughtCapiException();
extern "C" void reraiseCapiExcAsCxx() __attribute__((noreturn));


BoxedCode* getTopPythonFunction();

// debugging/stat helper, returns python filename:linenumber, or "unknown:-1" if it fails
std::string getCurrentPythonLine();

// doesn't really belong in unwinding.h, since it's stats related, but it needs to unwind to get the current line...
void logByCurrentPythonLine(const std::string& stat_name);

// Adds stack locals and closure locals into the locals dict, and returns it.
BORROWED(Box*) fastLocalsToBoxedLocals();

class PythonFrameIteratorImpl;
class PythonFrameIterator {
private:
    std::unique_ptr<PythonFrameIteratorImpl> impl;

public:
    CompiledFunction* getCF();
    BoxedCode* getCode();
    FrameInfo* getFrameInfo();
    bool exists() { return impl.get() != NULL; }
    AST_stmt* getCurrentStatement();
    BORROWED(Box*) getGlobalsDict();

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

void addDecrefInfoEntry(uint64_t ip, std::vector<class Location> location);
void removeDecrefInfoEntry(uint64_t ip);

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
