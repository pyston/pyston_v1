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

#ifndef PYSTON_CORE_BST_H
#define PYSTON_CORE_BST_H

#include <cassert>
#include <cstdlib>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/scoping_analysis.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

namespace BST_TYPE {
// These are in a pretty random order (started off alphabetical but then I had to add more).
// These can be changed freely as long as parse_ast.py is also updated
#define FOREACH_TYPE(X)                                                                                                \
    X(alias, 1)                                                                                                        \
    X(arguments, 2)                                                                                                    \
    X(Assert, 3)                                                                                                       \
    X(Assign, 4)                                                                                                       \
    X(Attribute, 5)                                                                                                    \
    X(AugAssign, 6)                                                                                                    \
    X(BinOp, 7)                                                                                                        \
    X(BoolOp, 8)                                                                                                       \
    X(Call, 9)                                                                                                         \
    X(ClassDef, 10)                                                                                                    \
    X(Compare, 11)                                                                                                     \
    X(comprehension, 12)                                                                                               \
    X(Delete, 13)                                                                                                      \
    X(Dict, 14)                                                                                                        \
    X(Exec, 16)                                                                                                        \
    X(ExceptHandler, 17)                                                                                               \
    X(ExtSlice, 18)                                                                                                    \
    X(Expr, 19)                                                                                                        \
    X(For, 20)                                                                                                         \
    X(FunctionDef, 21)                                                                                                 \
    X(GeneratorExp, 22)                                                                                                \
    X(Global, 23)                                                                                                      \
    X(If, 24)                                                                                                          \
    X(IfExp, 25)                                                                                                       \
    X(Import, 26)                                                                                                      \
    X(ImportFrom, 27)                                                                                                  \
    X(Index, 28)                                                                                                       \
    X(keyword, 29)                                                                                                     \
    X(Lambda, 30)                                                                                                      \
    X(List, 31)                                                                                                        \
    X(ListComp, 32)                                                                                                    \
    X(Module, 33)                                                                                                      \
    X(Num, 34)                                                                                                         \
    X(Name, 35)                                                                                                        \
    X(Pass, 37)                                                                                                        \
    X(Pow, 38)                                                                                                         \
    X(Print, 39)                                                                                                       \
    X(Raise, 40)                                                                                                       \
    X(Repr, 41)                                                                                                        \
    X(Return, 42)                                                                                                      \
    X(Slice, 44)                                                                                                       \
    X(Str, 45)                                                                                                         \
    X(Subscript, 46)                                                                                                   \
    X(TryExcept, 47)                                                                                                   \
    X(TryFinally, 48)                                                                                                  \
    X(Tuple, 49)                                                                                                       \
    X(UnaryOp, 50)                                                                                                     \
    X(With, 51)                                                                                                        \
    X(While, 52)                                                                                                       \
    X(Yield, 53)                                                                                                       \
    X(Store, 54)                                                                                                       \
    X(Load, 55)                                                                                                        \
    X(Param, 56)                                                                                                       \
    X(Not, 57)                                                                                                         \
    X(In, 58)                                                                                                          \
    X(Is, 59)                                                                                                          \
    X(IsNot, 60)                                                                                                       \
    X(Or, 61)                                                                                                          \
    X(And, 62)                                                                                                         \
    X(Eq, 63)                                                                                                          \
    X(NotEq, 64)                                                                                                       \
    X(NotIn, 65)                                                                                                       \
    X(GtE, 66)                                                                                                         \
    X(Gt, 67)                                                                                                          \
    X(Mod, 68)                                                                                                         \
    X(Add, 69)                                                                                                         \
    X(Continue, 70)                                                                                                    \
    X(Lt, 71)                                                                                                          \
    X(LtE, 72)                                                                                                         \
    X(Break, 73)                                                                                                       \
    X(Sub, 74)                                                                                                         \
    X(Del, 75)                                                                                                         \
    X(Mult, 76)                                                                                                        \
    X(Div, 77)                                                                                                         \
    X(USub, 78)                                                                                                        \
    X(BitAnd, 79)                                                                                                      \
    X(BitOr, 80)                                                                                                       \
    X(BitXor, 81)                                                                                                      \
    X(RShift, 82)                                                                                                      \
    X(LShift, 83)                                                                                                      \
    X(Invert, 84)                                                                                                      \
    X(UAdd, 85)                                                                                                        \
    X(FloorDiv, 86)                                                                                                    \
    X(DictComp, 15)                                                                                                    \
    X(Set, 43)                                                                                                         \
    X(Ellipsis, 87)                                                                                                    \
    /* like Module, but used for eval. */                                                                              \
    X(Expression, 88)                                                                                                  \
    X(SetComp, 89)                                                                                                     \
    X(Suite, 90)                                                                                                       \
                                                                                                                       \
    /* Pseudo-nodes that are specific to this compiler: */                                                             \
    X(Branch, 200)                                                                                                     \
    X(Jump, 201)                                                                                                       \
    X(ClsAttribute, 202)                                                                                               \
    X(AugBinOp, 203)                                                                                                   \
    X(Invoke, 204)                                                                                                     \
    X(LangPrimitive, 205)                                                                                              \
    /* wraps a ClassDef to make it an expr */                                                                          \
    X(MakeClass, 206)                                                                                                  \
    /* wraps a FunctionDef to make it an expr */                                                                       \
    X(MakeFunction, 207)                                                                                               \
                                                                                                                       \
    /* These aren't real BST types, but since we use BST types to represent binexp types */                            \
    /* and divmod+truediv are essentially types of binops, we add them here (at least for now): */                     \
    X(DivMod, 250)                                                                                                     \
    X(TrueDiv, 251)

#define GENERATE_ENUM(ENUM, N) ENUM = N,
#define GENERATE_STRING(STRING, N) m[N] = #STRING;

enum BST_TYPE : unsigned char { FOREACH_TYPE(GENERATE_ENUM) };

static const char* stringify(int n) {
    static std::map<int, const char*> m;
    FOREACH_TYPE(GENERATE_STRING)
    return m[n];
}

#undef FOREACH_TYPE
#undef GENERATE_ENUM
#undef GENERATE_STRING
};

class BSTVisitor;
class ExprVisitor;
class StmtVisitor;
class SliceVisitor;
class BST_keyword;

class BST {
public:
    virtual ~BST() {}

    const BST_TYPE::BST_TYPE type;
    uint32_t lineno;

    virtual void accept(BSTVisitor* v) = 0;

// #define DEBUG_LINE_NUMBERS 1
#ifdef DEBUG_LINE_NUMBERS
private:
    // Initialize lineno to something unique, so that if we see something ridiculous
    // appear in the traceback, we can isolate the allocation which created it.
    static int next_lineno;

public:
    BST(BST_TYPE::BST_TYPE type);
#else
    BST(BST_TYPE::BST_TYPE type) : type(type), lineno(0) {}
#endif
    BST(BST_TYPE::BST_TYPE type, uint32_t lineno) : type(type), lineno(lineno) {}
};

class BST_expr : public BST {
public:
    virtual void* accept_expr(ExprVisitor* v) = 0;

    BST_expr(BST_TYPE::BST_TYPE type) : BST(type) {}
    BST_expr(BST_TYPE::BST_TYPE type, uint32_t lineno) : BST(type, lineno) {}
};

class BST_stmt : public BST {
public:
    virtual void accept_stmt(StmtVisitor* v) = 0;

    int cxx_exception_count = 0;

    BST_stmt(BST_TYPE::BST_TYPE type) : BST(type) {}
};

class BST_slice : public BST {
public:
    virtual void* accept_slice(SliceVisitor* s) = 0;
    BST_slice(BST_TYPE::BST_TYPE type) : BST(type) {}
    BST_slice(BST_TYPE::BST_TYPE type, uint32_t lineno) : BST(type, lineno) {}
};

class BST_Name;

class BST_arguments : public BST {
public:
    // no lineno attributes
    std::vector<BST_expr*> defaults;

    virtual void accept(BSTVisitor* v);

    BST_arguments() : BST(BST_TYPE::arguments) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::arguments;
};

class BST_Assert : public BST_stmt {
public:
    BST_expr* msg, *test;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Assert() : BST_stmt(BST_TYPE::Assert) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Assert;
};

class BST_Assign : public BST_stmt {
public:
    BST_expr* target;
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Assign() : BST_stmt(BST_TYPE::Assign) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Assign;
};

class BST_AugBinOp : public BST_expr {
public:
    AST_TYPE::AST_TYPE op_type;
    BST_expr* left, *right;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_AugBinOp() : BST_expr(BST_TYPE::AugBinOp) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::AugBinOp;
};

class BST_Attribute : public BST_expr {
public:
    BST_expr* value;
    AST_TYPE::AST_TYPE ctx_type;
    InternedString attr;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Attribute() : BST_expr(BST_TYPE::Attribute) {}

    BST_Attribute(BST_expr* value, AST_TYPE::AST_TYPE ctx_type, InternedString attr)
        : BST_expr(BST_TYPE::Attribute), value(value), ctx_type(ctx_type), attr(attr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Attribute;
};

class BST_BinOp : public BST_expr {
public:
    AST_TYPE::AST_TYPE op_type;
    BST_expr* left, *right;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_BinOp() : BST_expr(BST_TYPE::BinOp) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::BinOp;
};

class BST_Call : public BST_expr {
public:
    BST_expr* starargs, *kwargs, *func;
    std::vector<BST_expr*> args;
    std::vector<BST_keyword*> keywords;

    // used during execution stores all keyword names
    std::unique_ptr<std::vector<BoxedString*>> keywords_names;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Call() : BST_expr(BST_TYPE::Call) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Call;
};

class BST_Compare : public BST_expr {
public:
    AST_TYPE::AST_TYPE op;
    BST_expr* comparator;
    BST_expr* left;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Compare() : BST_expr(BST_TYPE::Compare) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Compare;
};

class BST_ClassDef : public BST_stmt {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    std::vector<BST_expr*> bases, decorator_list;
    InternedString name;

    BoxedCode* code;

    BST_ClassDef() : BST_stmt(BST_TYPE::ClassDef) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ClassDef;
};

class BST_Dict : public BST_expr {
public:
    std::vector<BST_expr*> keys, values;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Dict() : BST_expr(BST_TYPE::Dict) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Dict;
};

class BST_Delete : public BST_stmt {
public:
    BST_expr* target;
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Delete() : BST_stmt(BST_TYPE::Delete) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Delete;
};

class BST_Ellipsis : public BST_slice {
public:
    virtual void accept(BSTVisitor* v);
    virtual void* accept_slice(SliceVisitor* v);

    BST_Ellipsis() : BST_slice(BST_TYPE::Ellipsis) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Ellipsis;
};

class BST_Expr : public BST_stmt {
public:
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Expr() : BST_stmt(BST_TYPE::Expr) {}
    BST_Expr(BST_expr* value) : BST_stmt(BST_TYPE::Expr), value(value) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Expr;
};

class BST_Exec : public BST_stmt {
public:
    BST_expr* body;
    BST_expr* globals;
    BST_expr* locals;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Exec() : BST_stmt(BST_TYPE::Exec) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Exec;
};

class BST_ExtSlice : public BST_slice {
public:
    std::vector<BST_slice*> dims;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_slice(SliceVisitor* v);

    BST_ExtSlice() : BST_slice(BST_TYPE::ExtSlice) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ExtSlice;
};

class BST_FunctionDef : public BST_stmt {
public:
    std::vector<BST_expr*> decorator_list;
    InternedString name; // if the name is not set this is a lambda
    BST_arguments* args;

    BoxedCode* code;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_FunctionDef() : BST_stmt(BST_TYPE::FunctionDef) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::FunctionDef;
};

class BST_Index : public BST_slice {
public:
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_slice(SliceVisitor* v);

    BST_Index() : BST_slice(BST_TYPE::Index) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Index;
};

class BST_keyword : public BST {
public:
    // no lineno attributes
    BST_expr* value;
    InternedString arg;

    virtual void accept(BSTVisitor* v);

    BST_keyword() : BST(BST_TYPE::keyword) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::keyword;
};

class BST_List : public BST_expr {
public:
    std::vector<BST_expr*> elts;
    AST_TYPE::AST_TYPE ctx_type;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_List() : BST_expr(BST_TYPE::List) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::List;
};

class BST_Name : public BST_expr {
public:
    AST_TYPE::AST_TYPE ctx_type;
    InternedString id;

    // The resolved scope of this name.  Kind of hacky to be storing it in the BST node;
    // in CPython it ends up getting "cached" by being translated into one of a number of
    // different bytecodes.
    ScopeInfo::VarScopeType lookup_type;

    // These are only valid for lookup_type == FAST or CLOSURE
    // The interpreter and baseline JIT store variables with FAST and CLOSURE scopes in an array (vregs) this specifies
    // the zero based index of this variable inside the vregs array. If uninitialized it's value is -1.
    int vreg;
    bool is_kill = false;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Name(InternedString id, AST_TYPE::AST_TYPE ctx_type, int lineno)
        : BST_expr(BST_TYPE::Name, lineno),
          ctx_type(ctx_type),
          id(id),
          lookup_type(ScopeInfo::VarScopeType::UNKNOWN),
          vreg(-1) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Name;
};

class BST_Num : public BST_expr {
public:
    AST_Num::NumType num_type;

    // TODO: these should just be Boxed objects now
    union {
        int64_t n_int;
        double n_float;
    };
    std::string n_long;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Num() : BST_expr(BST_TYPE::Num) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Num;
};

class BST_Repr : public BST_expr {
public:
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Repr() : BST_expr(BST_TYPE::Repr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Repr;
};

class BST_Print : public BST_stmt {
public:
    BST_expr* dest;
    bool nl;
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Print() : BST_stmt(BST_TYPE::Print), value(NULL) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Print;
};

class BST_Raise : public BST_stmt {
public:
    // In the python ast module, these are called "type", "inst", and "tback", respectively.
    // Renaming to arg{0..2} since I find that confusing, since they are filled in
    // sequentially rather than semantically.
    // Ie "raise Exception()" will have type==Exception(), inst==None, tback==None
    BST_expr* arg0, *arg1, *arg2;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Raise() : BST_stmt(BST_TYPE::Raise), arg0(NULL), arg1(NULL), arg2(NULL) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Raise;
};

class BST_Return : public BST_stmt {
public:
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Return() : BST_stmt(BST_TYPE::Return) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Return;
};

class BST_Set : public BST_expr {
public:
    std::vector<BST_expr*> elts;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Set() : BST_expr(BST_TYPE::Set) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Set;
};

class BST_Slice : public BST_slice {
public:
    BST_expr* lower, *upper, *step;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_slice(SliceVisitor* v);

    BST_Slice() : BST_slice(BST_TYPE::Slice) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Slice;
};

class BST_Str : public BST_expr {
public:
    AST_Str::StrType str_type;

    // The meaning of str_data depends on str_type.  For STR, it's just the bytes value.
    // For UNICODE, it's the utf-8 encoded value.
    std::string str_data;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Str() : BST_expr(BST_TYPE::Str), str_type(AST_Str::UNSET) {}
    BST_Str(std::string s) : BST_expr(BST_TYPE::Str), str_type(AST_Str::STR), str_data(std::move(s)) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Str;
};

class BST_Subscript : public BST_expr {
public:
    BST_expr* value;
    BST_slice* slice;
    AST_TYPE::AST_TYPE ctx_type;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Subscript() : BST_expr(BST_TYPE::Subscript) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Subscript;
};

class BST_Tuple : public BST_expr {
public:
    std::vector<BST_expr*> elts;
    AST_TYPE::AST_TYPE ctx_type;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Tuple() : BST_expr(BST_TYPE::Tuple) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Tuple;
};

class BST_UnaryOp : public BST_expr {
public:
    BST_expr* operand;
    AST_TYPE::AST_TYPE op_type;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_UnaryOp() : BST_expr(BST_TYPE::UnaryOp) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::UnaryOp;
};

class BST_Yield : public BST_expr {
public:
    BST_expr* value;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_Yield() : BST_expr(BST_TYPE::Yield) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Yield;
};

class BST_MakeFunction : public BST_expr {
public:
    BST_FunctionDef* function_def;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_MakeFunction(BST_FunctionDef* fd) : BST_expr(BST_TYPE::MakeFunction, fd->lineno), function_def(fd) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::MakeFunction;
};

class BST_MakeClass : public BST_expr {
public:
    BST_ClassDef* class_def;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_MakeClass(BST_ClassDef* cd) : BST_expr(BST_TYPE::MakeClass, cd->lineno), class_def(cd) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::MakeClass;
};


// BST pseudo-nodes that will get added during CFG-construction.  These don't exist in the input BST, but adding them in
// lets us avoid creating a completely new IR for this phase

class CFGBlock;

class BST_Branch : public BST_stmt {
public:
    BST_expr* test;
    CFGBlock* iftrue, *iffalse;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Branch() : BST_stmt(BST_TYPE::Branch) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Branch;
};

class BST_Jump : public BST_stmt {
public:
    CFGBlock* target;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Jump() : BST_stmt(BST_TYPE::Jump) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Jump;
};

class BST_ClsAttribute : public BST_expr {
public:
    BST_expr* value;
    InternedString attr;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_ClsAttribute() : BST_expr(BST_TYPE::ClsAttribute) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ClsAttribute;
};

class BST_Invoke : public BST_stmt {
public:
    BST_stmt* stmt;

    CFGBlock* normal_dest, *exc_dest;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Invoke(BST_stmt* stmt) : BST_stmt(BST_TYPE::Invoke), stmt(stmt) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Invoke;
};

// "LangPrimitive" represents operations that "primitive" to the language,
// but aren't directly *exactly* representable as normal Python.
// ClsAttribute would fall into this category.
// These are basically bytecodes, framed as pseudo-BST-nodes.
class BST_LangPrimitive : public BST_expr {
public:
    enum Opcodes : unsigned char {
        LANDINGPAD, // grabs the info about the last raised exception
        LOCALS,
        GET_ITER,
        IMPORT_FROM,
        IMPORT_NAME,
        IMPORT_STAR,
        NONE,
        NONZERO, // determines whether something is "true" for purposes of `if' and so forth
        CHECK_EXC_MATCH,
        SET_EXC_INFO,
        UNCACHE_EXC_INFO,
        HASNEXT,
        PRINT_EXPR,
    } opcode;
    std::vector<BST_expr*> args;

    virtual void accept(BSTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    BST_LangPrimitive(Opcodes opcode) : BST_expr(BST_TYPE::LangPrimitive), opcode(opcode) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::LangPrimitive;
};

template <typename T> T* bst_cast(BST* node) {
    ASSERT(!node || node->type == T::TYPE, "%d", node ? node->type : 0);
    return static_cast<T*>(node);
}



class BSTVisitor {
protected:
public:
    virtual ~BSTVisitor() {}

    virtual bool visit_arguments(BST_arguments* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_assert(BST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_assign(BST_Assign* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_augbinop(BST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_attribute(BST_Attribute* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_binop(BST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_call(BST_Call* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_clsattribute(BST_ClsAttribute* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_compare(BST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_classdef(BST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_delete(BST_Delete* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_dict(BST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_ellipsis(BST_Ellipsis* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_exec(BST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_expr(BST_Expr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_extslice(BST_ExtSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_functiondef(BST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_index(BST_Index* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_invoke(BST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_keyword(BST_keyword* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_langprimitive(BST_LangPrimitive* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_list(BST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_name(BST_Name* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_num(BST_Num* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_print(BST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_raise(BST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_repr(BST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_return(BST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_set(BST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_slice(BST_Slice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_str(BST_Str* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_subscript(BST_Subscript* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_tuple(BST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_unaryop(BST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_yield(BST_Yield* node) { RELEASE_ASSERT(0, ""); }

    virtual bool visit_makeclass(BST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makefunction(BST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_branch(BST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_jump(BST_Jump* node) { RELEASE_ASSERT(0, ""); }
};

class NoopBSTVisitor : public BSTVisitor {
protected:
public:
    virtual ~NoopBSTVisitor() {}

    virtual bool visit_arguments(BST_arguments* node) { return false; }
    virtual bool visit_assert(BST_Assert* node) { return false; }
    virtual bool visit_assign(BST_Assign* node) { return false; }
    virtual bool visit_augbinop(BST_AugBinOp* node) { return false; }
    virtual bool visit_attribute(BST_Attribute* node) { return false; }
    virtual bool visit_binop(BST_BinOp* node) { return false; }
    virtual bool visit_call(BST_Call* node) { return false; }
    virtual bool visit_clsattribute(BST_ClsAttribute* node) { return false; }
    virtual bool visit_compare(BST_Compare* node) { return false; }
    virtual bool visit_classdef(BST_ClassDef* node) { return false; }
    virtual bool visit_delete(BST_Delete* node) { return false; }
    virtual bool visit_dict(BST_Dict* node) { return false; }
    virtual bool visit_ellipsis(BST_Ellipsis* node) { return false; }
    virtual bool visit_exec(BST_Exec* node) { return false; }
    virtual bool visit_expr(BST_Expr* node) { return false; }
    virtual bool visit_extslice(BST_ExtSlice* node) { return false; }
    virtual bool visit_functiondef(BST_FunctionDef* node) { return false; }
    virtual bool visit_index(BST_Index* node) { return false; }
    virtual bool visit_invoke(BST_Invoke* node) { return false; }
    virtual bool visit_keyword(BST_keyword* node) { return false; }
    virtual bool visit_langprimitive(BST_LangPrimitive* node) { return false; }
    virtual bool visit_list(BST_List* node) { return false; }
    virtual bool visit_name(BST_Name* node) { return false; }
    virtual bool visit_num(BST_Num* node) { return false; }
    virtual bool visit_print(BST_Print* node) { return false; }
    virtual bool visit_raise(BST_Raise* node) { return false; }
    virtual bool visit_repr(BST_Repr* node) { return false; }
    virtual bool visit_return(BST_Return* node) { return false; }
    virtual bool visit_set(BST_Set* node) { return false; }
    virtual bool visit_slice(BST_Slice* node) { return false; }
    virtual bool visit_str(BST_Str* node) { return false; }
    virtual bool visit_subscript(BST_Subscript* node) { return false; }
    virtual bool visit_tuple(BST_Tuple* node) { return false; }
    virtual bool visit_unaryop(BST_UnaryOp* node) { return false; }
    virtual bool visit_yield(BST_Yield* node) { return false; }

    virtual bool visit_branch(BST_Branch* node) { return false; }
    virtual bool visit_jump(BST_Jump* node) { return false; }
    virtual bool visit_makeclass(BST_MakeClass* node) { return false; }
    virtual bool visit_makefunction(BST_MakeFunction* node) { return false; }
};

class ExprVisitor {
protected:
public:
    virtual ~ExprVisitor() {}

    virtual void* visit_augbinop(BST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_attribute(BST_Attribute* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_binop(BST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_call(BST_Call* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_clsattribute(BST_ClsAttribute* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_compare(BST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_dict(BST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_langprimitive(BST_LangPrimitive* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_list(BST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_name(BST_Name* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_num(BST_Num* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_repr(BST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_set(BST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_str(BST_Str* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_subscript(BST_Subscript* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_tuple(BST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_unaryop(BST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_yield(BST_Yield* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_makeclass(BST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_makefunction(BST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
};

class StmtVisitor {
protected:
public:
    virtual ~StmtVisitor() {}

    virtual void visit_assert(BST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_assign(BST_Assign* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_classdef(BST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_delete(BST_Delete* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_exec(BST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_expr(BST_Expr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_functiondef(BST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_invoke(BST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_print(BST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_raise(BST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_return(BST_Return* node) { RELEASE_ASSERT(0, ""); }

    virtual void visit_branch(BST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_jump(BST_Jump* node) { RELEASE_ASSERT(0, ""); }
};

class SliceVisitor {
public:
    virtual ~SliceVisitor() {}
    virtual void* visit_ellipsis(BST_Ellipsis* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_extslice(BST_ExtSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_index(BST_Index* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_slice(BST_Slice* node) { RELEASE_ASSERT(0, ""); }
};

void print_bst(BST* bst);
class PrintVisitor : public BSTVisitor {
private:
    llvm::raw_ostream& stream;
    int indent;
    void printIndent();
    void printOp(AST_TYPE::AST_TYPE op_type);

public:
    PrintVisitor(int indent = 0, llvm::raw_ostream& stream = llvm::outs()) : stream(stream), indent(indent) {}
    virtual ~PrintVisitor() {}
    void flush() { stream.flush(); }

    virtual bool visit_arguments(BST_arguments* node);
    virtual bool visit_assert(BST_Assert* node);
    virtual bool visit_assign(BST_Assign* node);
    virtual bool visit_augbinop(BST_AugBinOp* node);
    virtual bool visit_attribute(BST_Attribute* node);
    virtual bool visit_binop(BST_BinOp* node);
    virtual bool visit_call(BST_Call* node);
    virtual bool visit_compare(BST_Compare* node);
    virtual bool visit_classdef(BST_ClassDef* node);
    virtual bool visit_clsattribute(BST_ClsAttribute* node);
    virtual bool visit_delete(BST_Delete* node);
    virtual bool visit_dict(BST_Dict* node);
    virtual bool visit_ellipsis(BST_Ellipsis* node);
    virtual bool visit_exec(BST_Exec* node);
    virtual bool visit_expr(BST_Expr* node);
    virtual bool visit_extslice(BST_ExtSlice* node);
    virtual bool visit_functiondef(BST_FunctionDef* node);
    virtual bool visit_index(BST_Index* node);
    virtual bool visit_invoke(BST_Invoke* node);
    virtual bool visit_keyword(BST_keyword* node);
    virtual bool visit_langprimitive(BST_LangPrimitive* node);
    virtual bool visit_list(BST_List* node);
    virtual bool visit_name(BST_Name* node);
    virtual bool visit_num(BST_Num* node);
    virtual bool visit_print(BST_Print* node);
    virtual bool visit_raise(BST_Raise* node);
    virtual bool visit_repr(BST_Repr* node);
    virtual bool visit_return(BST_Return* node);
    virtual bool visit_set(BST_Set* node);
    virtual bool visit_slice(BST_Slice* node);
    virtual bool visit_str(BST_Str* node);
    virtual bool visit_subscript(BST_Subscript* node);
    virtual bool visit_tuple(BST_Tuple* node);
    virtual bool visit_unaryop(BST_UnaryOp* node);
    virtual bool visit_yield(BST_Yield* node);

    virtual bool visit_branch(BST_Branch* node);
    virtual bool visit_jump(BST_Jump* node);
    virtual bool visit_makefunction(BST_MakeFunction* node);
    virtual bool visit_makeclass(BST_MakeClass* node);
};

// Given an BST node, return a vector of the node plus all its descendents.
// This is useful for analyses that care more about the constituent nodes than the
// exact tree structure; ex, finding all "global" directives.
void flatten(llvm::ArrayRef<BST_stmt*> roots, std::vector<BST*>& output, bool expand_scopes);
void flatten(BST_expr* root, std::vector<BST*>& output, bool expand_scopes);
};

#endif
