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

#ifndef PYSTON_CODEGEN_ASTINTERPRETER_H
#define PYSTON_CODEGEN_ASTINTERPRETER_H

#include "codegen/unwinding.h"

namespace pyston {

namespace gc {
class GCVisitor;
}

class AST_expr;
class AST_stmt;
class AST_Jump;
class Box;
class BoxedClosure;
class BoxedDict;
struct FunctionMetadata;
struct LineInfo;

extern const void* interpreter_instr_addr;

struct ASTInterpreterJitInterface {
    // Special value which when returned from the bjit will trigger a OSR.
    static constexpr uint64_t osr_dummy_value = -1;

    static int getBoxedLocalsOffset();
    static int getCurrentBlockOffset();
    static int getCurrentInstOffset();
    static int getEdgeCountOffset();
    static int getGeneratorOffset();
    static int getGlobalsOffset();

    static void delNameHelper(void* _interpreter, InternedString name);
    static Box* derefHelper(void* interp, InternedString s);
    static Box* landingpadHelper(void* interp);
    static void pendingCallsCheckHelper();
    static void setExcInfoHelper(void* interp, STOLEN(Box*) type, STOLEN(Box*) value, STOLEN(Box*) traceback);
    static void setLocalClosureHelper(void* interp, long vreg, InternedString id, Box* v);
    static void uncacheExcInfoHelper(void* interp);
    static void raise0Helper(void* interp) __attribute__((noreturn));
};

class RewriterVar;
struct Value {
    union {
        bool b;
        int64_t n;
        double d;
        Box* o;
    };
    RewriterVar* var;

    operator RewriterVar*() { return var; }

    Value() : o(0), var(0) {}
    Value(bool b, RewriterVar* var) : b(b), var(var) {}
    Value(int64_t n, RewriterVar* var) : n(n), var(var) {}
    Value(double d, RewriterVar* var) : d(d), var(var) {}
    Value(Box* o, RewriterVar* var) : o(o), var(var) {}
};

Box* astInterpretFunction(FunctionMetadata* f, Box* closure, Box* generator, Box* globals, Box* arg1, Box* arg2,
                          Box* arg3, Box** args);
Box* astInterpretFunctionEval(FunctionMetadata* cf, Box* globals, Box* boxedLocals);
// this function is implemented in the src/codegen/ast_interpreter_exec.S assembler file
extern "C" Box* astInterpretDeopt(FunctionMetadata* cf, AST_expr* after_expr, AST_stmt* enclosing_stmt, Box* expr_val,
                                  FrameStackState frame_state);

struct FrameInfo;
FrameInfo* getFrameInfoForInterpretedFrame(void* frame_ptr);

// Executes the equivalent of CPython's PRINT_EXPR opcode (call sys.displayhook)
extern "C" void printExprHelper(Box* b);
}

#endif
