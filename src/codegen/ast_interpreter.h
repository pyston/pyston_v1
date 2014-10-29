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

#ifndef PYSTON_CODEGEN_ASTINTERPRETER_H
#define PYSTON_CODEGEN_ASTINTERPRETER_H

#include <llvm/ADT/StringMap.h>

#include "codegen/compvars.h"
#include "core/ast.h"
#include "runtime/objmodel.h"

namespace pyston {

namespace gc {
class GCVisitor;
}

class Box;
class CLFunction;
struct LineInfo;

Box* astInterpretFunction(CompiledFunction* f, int nargs, Box* closure, Box* generator, Box* arg1, Box* arg2, Box* arg3,
                          Box** args);

const LineInfo* getLineInfoForInterpretedFrame(void* frame_ptr);
BoxedModule* getModuleForInterpretedFrame(void* frame_ptr);

union Value {
    bool b;
    int64_t n;
    double d;
    Box* o;

    Value(bool b) : b(b) {}
    Value(int64_t n = 0) : n(n) {}
    Value(double d) : d(d) {}
    Value(Box* o) : o(o) {}
};

class ASTInterpreter {
public:
    typedef llvm::StringMap<Box*> SymMap;

    ASTInterpreter(CompiledFunction* compiled_function);
    ~ASTInterpreter();

    void initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args);
    Value execute(CFGBlock* block = 0);

private:
    Box* createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body);
    Value doBinOp(Box* left, Box* right, int op, BinExpType exp_type);
    void doStore(AST_expr* node, Value value);
    void doStore(const std::string& name, Value value);
    void eraseDeadSymbols();

    Value visit_assert(AST_Assert* node);
    Value visit_assign(AST_Assign* node);
    Value visit_binop(AST_BinOp* node);
    Value visit_call(AST_Call* node);
    Value visit_classDef(AST_ClassDef* node);
    Value visit_compare(AST_Compare* node);
    Value visit_delete(AST_Delete* node);
    Value visit_functionDef(AST_FunctionDef* node);
    Value visit_global(AST_Global* node);
    Value visit_module(AST_Module* node);
    Value visit_print(AST_Print* node);
    Value visit_raise(AST_Raise* node);
    Value visit_return(AST_Return* node);
    Value visit_stmt(AST_stmt* node);
    Value visit_unaryop(AST_UnaryOp* node);

    Value visit_attribute(AST_Attribute* node);
    Value visit_dict(AST_Dict* node);
    Value visit_expr(AST_expr* node);
    Value visit_expr(AST_Expr* node);
    Value visit_index(AST_Index* node);
    Value visit_lambda(AST_Lambda* node);
    Value visit_list(AST_List* node);
    Value visit_name(AST_Name* node);
    Value visit_num(AST_Num* node);
    Value visit_repr(AST_Repr* node);
    Value visit_set(AST_Set* node);
    Value visit_slice(AST_Slice* node);
    Value visit_str(AST_Str* node);
    Value visit_subscript(AST_Subscript* node);
    Value visit_tuple(AST_Tuple* node);
    Value visit_yield(AST_Yield* node);


    // pseudo
    Value visit_augBinOp(AST_AugBinOp* node);
    Value visit_branch(AST_Branch* node);
    Value visit_clsAttribute(AST_ClsAttribute* node);
    Value visit_invoke(AST_Invoke* node);
    Value visit_jump(AST_Jump* node);
    Value visit_langPrimitive(AST_LangPrimitive* node);

public:
    SourceInfo* source_info;
    SymMap sym_table;
    CFGBlock* next_block, *current_block;
    AST* current_inst;
    Box* last_exception;
    BoxedClosure* passed_closure, *created_closure;
    BoxedGenerator* generator;
    ScopeInfo* scope_info;
    CompiledFunction* compiled_func;
    unsigned edgecount;
};
}

#endif
