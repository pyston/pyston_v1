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

#ifndef PYSTON_CORE_AST_H
#define PYSTON_CORE_AST_H

#include <cassert>
#include <cstdlib>
#include <stdint.h>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"

#include "analysis/scoping_analysis.h"
#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

namespace AST_TYPE {
// These are in a pretty random order (started off alphabetical but then I had to add more).
// These can be changed freely as long as parse_ast.py is also updated
enum AST_TYPE {
    alias = 1,
    arguments = 2,
    Assert = 3,
    Assign = 4,
    Attribute = 5,
    AugAssign = 6,
    BinOp = 7,
    BoolOp = 8,
    Call = 9,
    ClassDef = 10,
    Compare = 11,
    comprehension = 12,
    Delete = 13,
    Dict = 14,
    Exec = 16,
    ExceptHandler = 17,
    ExtSlice = 18,
    Expr = 19,
    For = 20,
    FunctionDef = 21,
    GeneratorExp = 22,
    Global = 23,
    If = 24,
    IfExp = 25,
    Import = 26,
    ImportFrom = 27,
    Index = 28,
    keyword = 29,
    Lambda = 30,
    List = 31,
    ListComp = 32,
    Module = 33,
    Num = 34,
    Name = 35,
    Pass = 37,
    Pow = 38,
    Print = 39,
    Raise = 40,
    Repr = 41,
    Return = 42,
    Slice = 44,
    Str = 45,
    Subscript = 46,
    TryExcept = 47,
    TryFinally = 48,
    Tuple = 49,
    UnaryOp = 50,
    With = 51,
    While = 52,
    Yield = 53,
    Store = 54,
    Load = 55,
    Param = 56,
    Not = 57,
    In = 58,
    Is = 59,
    IsNot = 60,
    Or = 61,
    And = 62,
    Eq = 63,
    NotEq = 64,
    NotIn = 65,
    GtE = 66,
    Gt = 67,
    Mod = 68,
    Add = 69,
    Continue = 70,
    Lt = 71,
    LtE = 72,
    Break = 73,
    Sub = 74,
    Del = 75,
    Mult = 76,
    Div = 77,
    USub = 78,
    BitAnd = 79,
    BitOr = 80,
    BitXor = 81,
    RShift = 82,
    LShift = 83,
    Invert = 84,
    UAdd = 85,
    FloorDiv = 86,
    DictComp = 15,
    Set = 43,
    Ellipsis = 87,
    Expression = 88, // like Module, but used for eval.
    SetComp = 89,
    Suite = 90,

    // Pseudo-nodes that are specific to this compiler:
    Branch = 200,
    Jump = 201,
    ClsAttribute = 202,
    AugBinOp = 203,
    Invoke = 204,
    LangPrimitive = 205,
    MakeClass = 206,    // wraps a ClassDef to make it an expr
    MakeFunction = 207, // wraps a FunctionDef to make it an expr

    // These aren't real AST types, but since we use AST types to represent binexp types
    // and divmod+truediv are essentially types of binops, we add them here (at least for now):
    DivMod = 250,
    TrueDiv = 251,
};
};

class ASTVisitor;
class ExprVisitor;
class StmtVisitor;
class AST_keyword;

class AST {
public:
    virtual ~AST() {}

    const AST_TYPE::AST_TYPE type;
    uint32_t lineno, col_offset;

    virtual void accept(ASTVisitor* v) = 0;

// #define DEBUG_LINE_NUMBERS 1
#ifdef DEBUG_LINE_NUMBERS
private:
    // Initialize lineno to something unique, so that if we see something ridiculous
    // appear in the traceback, we can isolate the allocation which created it.
    static int next_lineno;

public:
    AST(AST_TYPE::AST_TYPE type);
#else
    AST(AST_TYPE::AST_TYPE type) : type(type), lineno(0), col_offset(0) {}
#endif
    AST(AST_TYPE::AST_TYPE type, uint32_t lineno, uint32_t col_offset = 0)
        : type(type), lineno(lineno), col_offset(col_offset) {}
};

class AST_expr : public AST {
public:
    virtual void* accept_expr(ExprVisitor* v) = 0;

    AST_expr(AST_TYPE::AST_TYPE type) : AST(type) {}
    AST_expr(AST_TYPE::AST_TYPE type, uint32_t lineno, uint32_t col_offset = 0) : AST(type, lineno, col_offset) {}
};

class AST_stmt : public AST {
public:
    virtual void accept_stmt(StmtVisitor* v) = 0;

    AST_stmt(AST_TYPE::AST_TYPE type) : AST(type) {}
};



class AST_alias : public AST {
public:
    InternedString name, asname;

    virtual void accept(ASTVisitor* v);

    AST_alias(InternedString name, InternedString asname) : AST(AST_TYPE::alias), name(name), asname(asname) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::alias;
};

class AST_arguments : public AST {
public:
    // no lineno, col_offset attributes
    std::vector<AST_expr*> args, defaults;

    // These are represented as strings, not names; not sure why.
    // If they don't exist, the string is empty.
    InternedString kwarg, vararg;

    virtual void accept(ASTVisitor* v);

    AST_arguments() : AST(AST_TYPE::arguments) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::arguments;
};

class AST_Assert : public AST_stmt {
public:
    AST_expr* msg, *test;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Assert() : AST_stmt(AST_TYPE::Assert) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Assert;
};

class AST_Assign : public AST_stmt {
public:
    std::vector<AST_expr*> targets;
    AST_expr* value;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Assign() : AST_stmt(AST_TYPE::Assign) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Assign;
};

class AST_AugAssign : public AST_stmt {
public:
    AST_expr* value;
    AST_expr* target;
    AST_TYPE::AST_TYPE op_type;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_AugAssign() : AST_stmt(AST_TYPE::AugAssign) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::AugAssign;
};

class AST_AugBinOp : public AST_expr {
public:
    AST_TYPE::AST_TYPE op_type;
    AST_expr* left, *right;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_AugBinOp() : AST_expr(AST_TYPE::AugBinOp) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::AugBinOp;
};

class AST_Attribute : public AST_expr {
public:
    AST_expr* value;
    AST_TYPE::AST_TYPE ctx_type;
    InternedString attr;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Attribute() : AST_expr(AST_TYPE::Attribute) {}

    AST_Attribute(AST_expr* value, AST_TYPE::AST_TYPE ctx_type, InternedString attr)
        : AST_expr(AST_TYPE::Attribute), value(value), ctx_type(ctx_type), attr(attr) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Attribute;
};

class AST_BinOp : public AST_expr {
public:
    AST_TYPE::AST_TYPE op_type;
    AST_expr* left, *right;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_BinOp() : AST_expr(AST_TYPE::BinOp) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::BinOp;
};

class AST_BoolOp : public AST_expr {
public:
    AST_TYPE::AST_TYPE op_type;
    std::vector<AST_expr*> values;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_BoolOp() : AST_expr(AST_TYPE::BoolOp) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::BoolOp;
};

class AST_Break : public AST_stmt {
public:
    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Break() : AST_stmt(AST_TYPE::Break) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Break;
};

class AST_Call : public AST_expr {
public:
    AST_expr* starargs, *kwargs, *func;
    std::vector<AST_expr*> args;
    std::vector<AST_keyword*> keywords;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Call() : AST_expr(AST_TYPE::Call) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Call;
};

class AST_Compare : public AST_expr {
public:
    std::vector<AST_TYPE::AST_TYPE> ops;
    std::vector<AST_expr*> comparators;
    AST_expr* left;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Compare() : AST_expr(AST_TYPE::Compare) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Compare;
};

class AST_comprehension : public AST {
public:
    AST_expr* target;
    AST_expr* iter;
    std::vector<AST_expr*> ifs;

    virtual void accept(ASTVisitor* v);

    AST_comprehension() : AST(AST_TYPE::comprehension) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::comprehension;
};

class AST_ClassDef : public AST_stmt {
public:
    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    std::vector<AST_expr*> bases, decorator_list;
    std::vector<AST_stmt*> body;
    InternedString name;

    AST_ClassDef() : AST_stmt(AST_TYPE::ClassDef) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::ClassDef;
};

class AST_Continue : public AST_stmt {
public:
    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Continue() : AST_stmt(AST_TYPE::Continue) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Continue;
};

class AST_Dict : public AST_expr {
public:
    std::vector<AST_expr*> keys, values;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Dict() : AST_expr(AST_TYPE::Dict) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Dict;
};

class AST_DictComp : public AST_expr {
public:
    std::vector<AST_comprehension*> generators;
    AST_expr* key, *value;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_DictComp() : AST_expr(AST_TYPE::DictComp) {}

    const static AST_TYPE::AST_TYPE TYPE = AST_TYPE::DictComp;
};

class AST_Delete : public AST_stmt {
public:
    std::vector<AST_expr*> targets;
    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Delete() : AST_stmt(AST_TYPE::Delete) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Delete;
};

class AST_Ellipsis : public AST_expr {
public:
    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Ellipsis() : AST_expr(AST_TYPE::Ellipsis) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Ellipsis;
};

class AST_Expr : public AST_stmt {
public:
    AST_expr* value;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Expr() : AST_stmt(AST_TYPE::Expr) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Expr;
};

class AST_ExceptHandler : public AST {
public:
    std::vector<AST_stmt*> body;
    AST_expr* type; // can be NULL for a bare "except:" clause
    AST_expr* name; // can be NULL if the exception doesn't get a name

    virtual void accept(ASTVisitor* v);

    AST_ExceptHandler() : AST(AST_TYPE::ExceptHandler) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::ExceptHandler;
};

class AST_Exec : public AST_stmt {
public:
    AST_expr* body;
    AST_expr* globals;
    AST_expr* locals;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Exec() : AST_stmt(AST_TYPE::Exec) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Exec;
};

// (Alternative to AST_Module, used for, e.g., eval)
class AST_Expression : public AST {
public:
    std::unique_ptr<InternedStringPool> interned_strings;

    AST_expr* body;

    virtual void accept(ASTVisitor* v);

    AST_Expression(std::unique_ptr<InternedStringPool> interned_strings)
        : AST(AST_TYPE::Expression), interned_strings(std::move(interned_strings)) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Expression;
};

class AST_ExtSlice : public AST_expr {
public:
    std::vector<AST_expr*> dims;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_ExtSlice() : AST_expr(AST_TYPE::ExtSlice) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::ExtSlice;
};

class AST_For : public AST_stmt {
public:
    std::vector<AST_stmt*> body, orelse;
    AST_expr* target, *iter;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_For() : AST_stmt(AST_TYPE::For) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::For;
};

class AST_FunctionDef : public AST_stmt {
public:
    std::vector<AST_stmt*> body;
    std::vector<AST_expr*> decorator_list;
    InternedString name;
    AST_arguments* args;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_FunctionDef() : AST_stmt(AST_TYPE::FunctionDef) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::FunctionDef;
};

class AST_GeneratorExp : public AST_expr {
public:
    std::vector<AST_comprehension*> generators;
    AST_expr* elt;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_GeneratorExp() : AST_expr(AST_TYPE::GeneratorExp) {}

    const static AST_TYPE::AST_TYPE TYPE = AST_TYPE::GeneratorExp;
};

class AST_Global : public AST_stmt {
public:
    std::vector<InternedString> names;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Global() : AST_stmt(AST_TYPE::Global) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Global;
};

class AST_If : public AST_stmt {
public:
    std::vector<AST_stmt*> body, orelse;
    AST_expr* test;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_If() : AST_stmt(AST_TYPE::If) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::If;
};

class AST_IfExp : public AST_expr {
public:
    AST_expr* body, *test, *orelse;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_IfExp() : AST_expr(AST_TYPE::IfExp) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::IfExp;
};

class AST_Import : public AST_stmt {
public:
    std::vector<AST_alias*> names;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Import() : AST_stmt(AST_TYPE::Import) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Import;
};

class AST_ImportFrom : public AST_stmt {
public:
    InternedString module;
    std::vector<AST_alias*> names;
    int level;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_ImportFrom() : AST_stmt(AST_TYPE::ImportFrom) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::ImportFrom;
};

class AST_Index : public AST_expr {
public:
    AST_expr* value;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Index() : AST_expr(AST_TYPE::Index) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Index;
};

class AST_keyword : public AST {
public:
    // no lineno, col_offset attributes
    AST_expr* value;
    InternedString arg;

    virtual void accept(ASTVisitor* v);

    AST_keyword() : AST(AST_TYPE::keyword) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::keyword;
};

class AST_Lambda : public AST_expr {
public:
    AST_arguments* args;
    AST_expr* body;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Lambda() : AST_expr(AST_TYPE::Lambda) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Lambda;
};

class AST_List : public AST_expr {
public:
    std::vector<AST_expr*> elts;
    AST_TYPE::AST_TYPE ctx_type;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_List() : AST_expr(AST_TYPE::List) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::List;
};

class AST_ListComp : public AST_expr {
public:
    std::vector<AST_comprehension*> generators;
    AST_expr* elt;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_ListComp() : AST_expr(AST_TYPE::ListComp) {}

    const static AST_TYPE::AST_TYPE TYPE = AST_TYPE::ListComp;
};

class AST_Module : public AST {
public:
    std::unique_ptr<InternedStringPool> interned_strings;

    // no lineno, col_offset attributes
    std::vector<AST_stmt*> body;

    virtual void accept(ASTVisitor* v);

    AST_Module(std::unique_ptr<InternedStringPool> interned_strings)
        : AST(AST_TYPE::Module), interned_strings(std::move(interned_strings)) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Module;
};

class AST_Suite : public AST {
public:
    std::unique_ptr<InternedStringPool> interned_strings;

    std::vector<AST_stmt*> body;

    virtual void accept(ASTVisitor* v);

    AST_Suite(std::unique_ptr<InternedStringPool> interned_strings)
        : AST(AST_TYPE::Suite), interned_strings(std::move(interned_strings)) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Suite;
};

class AST_Name : public AST_expr {
public:
    AST_TYPE::AST_TYPE ctx_type;
    InternedString id;

    // The resolved scope of this name.  Kind of hacky to be storing it in the AST node;
    // in CPython it ends up getting "cached" by being translated into one of a number of
    // different bytecodes.
    ScopeInfo::VarScopeType lookup_type;

    // The interpreter and baseline JIT store variables with FAST and CLOSURE scopes in an array (vregs) this specifies
    // the zero based index of this variable inside the vregs array. If uninitialized it's value is -1.
    int vreg;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Name(InternedString id, AST_TYPE::AST_TYPE ctx_type, int lineno, int col_offset = 0)
        : AST_expr(AST_TYPE::Name, lineno, col_offset),
          ctx_type(ctx_type),
          id(id),
          lookup_type(ScopeInfo::VarScopeType::UNKNOWN),
          vreg(-1) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Name;
};

class AST_Num : public AST_expr {
public:
    enum NumType {
        // These values must correspond to the values in parse_ast.py
        INT = 0x10,
        FLOAT = 0x20,
        LONG = 0x30,

        // for COMPLEX, n_float is the imaginary part, real part is 0
        COMPLEX = 0x40,
    } num_type;

    union {
        int64_t n_int;
        double n_float;
    };
    std::string n_long;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Num() : AST_expr(AST_TYPE::Num) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Num;
};

class AST_Repr : public AST_expr {
public:
    AST_expr* value;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Repr() : AST_expr(AST_TYPE::Repr) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Repr;
};

class AST_Pass : public AST_stmt {
public:
    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Pass() : AST_stmt(AST_TYPE::Pass) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Pass;
};

class AST_Print : public AST_stmt {
public:
    AST_expr* dest;
    bool nl;
    std::vector<AST_expr*> values;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Print() : AST_stmt(AST_TYPE::Print) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Print;
};

class AST_Raise : public AST_stmt {
public:
    // In the python ast module, these are called "type", "inst", and "tback", respectively.
    // Renaming to arg{0..2} since I find that confusing, since they are filled in
    // sequentially rather than semantically.
    // Ie "raise Exception()" will have type==Exception(), inst==None, tback==None
    AST_expr* arg0, *arg1, *arg2;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Raise() : AST_stmt(AST_TYPE::Raise), arg0(NULL), arg1(NULL), arg2(NULL) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Raise;
};

class AST_Return : public AST_stmt {
public:
    AST_expr* value;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Return() : AST_stmt(AST_TYPE::Return) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Return;
};

class AST_Set : public AST_expr {
public:
    std::vector<AST_expr*> elts;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Set() : AST_expr(AST_TYPE::Set) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Set;
};

class AST_SetComp : public AST_expr {
public:
    std::vector<AST_comprehension*> generators;
    AST_expr* elt;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_SetComp() : AST_expr(AST_TYPE::SetComp) {}

    const static AST_TYPE::AST_TYPE TYPE = AST_TYPE::SetComp;
};

class AST_Slice : public AST_expr {
public:
    AST_expr* lower, *upper, *step;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Slice() : AST_expr(AST_TYPE::Slice) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Slice;
};

class AST_Str : public AST_expr {
public:
    enum StrType {
        UNSET = 0x00,
        STR = 0x10,
        UNICODE = 0x20,
    } str_type;

    // The meaning of str_data depends on str_type.  For STR, it's just the bytes value.
    // For UNICODE, it's the utf-8 encoded value.
    std::string str_data;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Str() : AST_expr(AST_TYPE::Str), str_type(UNSET) {}
    AST_Str(std::string s) : AST_expr(AST_TYPE::Str), str_type(STR), str_data(std::move(s)) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Str;
};

class AST_Subscript : public AST_expr {
public:
    AST_expr* value, *slice;
    AST_TYPE::AST_TYPE ctx_type;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Subscript() : AST_expr(AST_TYPE::Subscript) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Subscript;
};

class AST_TryExcept : public AST_stmt {
public:
    std::vector<AST_stmt*> body, orelse;
    std::vector<AST_ExceptHandler*> handlers;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_TryExcept() : AST_stmt(AST_TYPE::TryExcept) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::TryExcept;
};

class AST_TryFinally : public AST_stmt {
public:
    std::vector<AST_stmt*> body, finalbody;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_TryFinally() : AST_stmt(AST_TYPE::TryFinally) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::TryFinally;
};

class AST_Tuple : public AST_expr {
public:
    std::vector<AST_expr*> elts;
    AST_TYPE::AST_TYPE ctx_type;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Tuple() : AST_expr(AST_TYPE::Tuple) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Tuple;
};

class AST_UnaryOp : public AST_expr {
public:
    AST_expr* operand;
    AST_TYPE::AST_TYPE op_type;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_UnaryOp() : AST_expr(AST_TYPE::UnaryOp) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::UnaryOp;
};

class AST_While : public AST_stmt {
public:
    AST_expr* test;
    std::vector<AST_stmt*> body, orelse;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_While() : AST_stmt(AST_TYPE::While) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::While;
};

class AST_With : public AST_stmt {
public:
    AST_expr* optional_vars, *context_expr;
    std::vector<AST_stmt*> body;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_With() : AST_stmt(AST_TYPE::With) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::With;
};

class AST_Yield : public AST_expr {
public:
    AST_expr* value;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_Yield() : AST_expr(AST_TYPE::Yield) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Yield;
};

class AST_MakeFunction : public AST_expr {
public:
    AST_FunctionDef* function_def;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_MakeFunction(AST_FunctionDef* fd)
        : AST_expr(AST_TYPE::MakeFunction, fd->lineno, fd->col_offset), function_def(fd) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::MakeFunction;
};

class AST_MakeClass : public AST_expr {
public:
    AST_ClassDef* class_def;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_MakeClass(AST_ClassDef* cd) : AST_expr(AST_TYPE::MakeClass, cd->lineno, cd->col_offset), class_def(cd) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::MakeClass;
};


// AST pseudo-nodes that will get added during CFG-construction.  These don't exist in the input AST, but adding them in
// lets us avoid creating a completely new IR for this phase

class CFGBlock;

class AST_Branch : public AST_stmt {
public:
    AST_expr* test;
    CFGBlock* iftrue, *iffalse;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Branch() : AST_stmt(AST_TYPE::Branch) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Branch;
};

class AST_Jump : public AST_stmt {
public:
    CFGBlock* target;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Jump() : AST_stmt(AST_TYPE::Jump) {
        lineno = -1;
        col_offset = -1;
    }

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Jump;
};

class AST_ClsAttribute : public AST_expr {
public:
    AST_expr* value;
    InternedString attr;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_ClsAttribute() : AST_expr(AST_TYPE::ClsAttribute) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::ClsAttribute;
};

class AST_Invoke : public AST_stmt {
public:
    AST_stmt* stmt;

    CFGBlock* normal_dest, *exc_dest;

    virtual void accept(ASTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    AST_Invoke(AST_stmt* stmt) : AST_stmt(AST_TYPE::Invoke), stmt(stmt) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::Invoke;
};

// "LangPrimitive" represents operations that "primitive" to the language,
// but aren't directly *exactly* representable as normal Python.
// ClsAttribute would fall into this category.
// These are basically bytecodes, framed as pseudo-AST-nodes.
class AST_LangPrimitive : public AST_expr {
public:
    enum Opcodes {
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
    } opcode;
    std::vector<AST_expr*> args;

    virtual void accept(ASTVisitor* v);
    virtual void* accept_expr(ExprVisitor* v);

    AST_LangPrimitive(Opcodes opcode) : AST_expr(AST_TYPE::LangPrimitive), opcode(opcode) {}

    static const AST_TYPE::AST_TYPE TYPE = AST_TYPE::LangPrimitive;
};

template <typename T> T* ast_cast(AST* node) {
    assert(node->type == T::TYPE);
    return static_cast<T*>(node);
}



class ASTVisitor {
protected:
public:
    virtual ~ASTVisitor() {}

    virtual bool visit_alias(AST_alias* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_arguments(AST_arguments* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_assert(AST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_assign(AST_Assign* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_augassign(AST_AugAssign* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_augbinop(AST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_attribute(AST_Attribute* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_binop(AST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_boolop(AST_BoolOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_break(AST_Break* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_call(AST_Call* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_clsattribute(AST_ClsAttribute* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_compare(AST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_comprehension(AST_comprehension* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_classdef(AST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_continue(AST_Continue* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_delete(AST_Delete* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_dict(AST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_dictcomp(AST_DictComp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_ellipsis(AST_Ellipsis* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_excepthandler(AST_ExceptHandler* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_exec(AST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_expr(AST_Expr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_expression(AST_Expression* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_suite(AST_Suite* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_extslice(AST_ExtSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_for(AST_For* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_functiondef(AST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_generatorexp(AST_GeneratorExp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_global(AST_Global* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_if(AST_If* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_ifexp(AST_IfExp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_import(AST_Import* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importfrom(AST_ImportFrom* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_index(AST_Index* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_invoke(AST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_keyword(AST_keyword* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_lambda(AST_Lambda* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_langprimitive(AST_LangPrimitive* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_list(AST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_listcomp(AST_ListComp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_module(AST_Module* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_name(AST_Name* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_num(AST_Num* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_pass(AST_Pass* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_print(AST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_raise(AST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_repr(AST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_return(AST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_set(AST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_setcomp(AST_SetComp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_slice(AST_Slice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_str(AST_Str* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_subscript(AST_Subscript* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_tryexcept(AST_TryExcept* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_tryfinally(AST_TryFinally* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_tuple(AST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_unaryop(AST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_while(AST_While* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_with(AST_With* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_yield(AST_Yield* node) { RELEASE_ASSERT(0, ""); }

    virtual bool visit_makeclass(AST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makefunction(AST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_branch(AST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_jump(AST_Jump* node) { RELEASE_ASSERT(0, ""); }
};

class NoopASTVisitor : public ASTVisitor {
protected:
public:
    virtual ~NoopASTVisitor() {}

    virtual bool visit_alias(AST_alias* node) { return false; }
    virtual bool visit_arguments(AST_arguments* node) { return false; }
    virtual bool visit_assert(AST_Assert* node) { return false; }
    virtual bool visit_assign(AST_Assign* node) { return false; }
    virtual bool visit_augassign(AST_AugAssign* node) { return false; }
    virtual bool visit_augbinop(AST_AugBinOp* node) { return false; }
    virtual bool visit_attribute(AST_Attribute* node) { return false; }
    virtual bool visit_binop(AST_BinOp* node) { return false; }
    virtual bool visit_boolop(AST_BoolOp* node) { return false; }
    virtual bool visit_break(AST_Break* node) { return false; }
    virtual bool visit_call(AST_Call* node) { return false; }
    virtual bool visit_clsattribute(AST_ClsAttribute* node) { return false; }
    virtual bool visit_compare(AST_Compare* node) { return false; }
    virtual bool visit_comprehension(AST_comprehension* node) { return false; }
    virtual bool visit_classdef(AST_ClassDef* node) { return false; }
    virtual bool visit_continue(AST_Continue* node) { return false; }
    virtual bool visit_delete(AST_Delete* node) { return false; }
    virtual bool visit_dict(AST_Dict* node) { return false; }
    virtual bool visit_dictcomp(AST_DictComp* node) { return false; }
    virtual bool visit_ellipsis(AST_Ellipsis* node) { return false; }
    virtual bool visit_excepthandler(AST_ExceptHandler* node) { return false; }
    virtual bool visit_exec(AST_Exec* node) { return false; }
    virtual bool visit_expr(AST_Expr* node) { return false; }
    virtual bool visit_expr(AST_Expression* node) { return false; }
    virtual bool visit_suite(AST_Suite* node) { return false; }
    virtual bool visit_extslice(AST_ExtSlice* node) { return false; }
    virtual bool visit_for(AST_For* node) { return false; }
    virtual bool visit_functiondef(AST_FunctionDef* node) { return false; }
    virtual bool visit_generatorexp(AST_GeneratorExp* node) { return false; }
    virtual bool visit_global(AST_Global* node) { return false; }
    virtual bool visit_if(AST_If* node) { return false; }
    virtual bool visit_ifexp(AST_IfExp* node) { return false; }
    virtual bool visit_import(AST_Import* node) { return false; }
    virtual bool visit_importfrom(AST_ImportFrom* node) { return false; }
    virtual bool visit_index(AST_Index* node) { return false; }
    virtual bool visit_invoke(AST_Invoke* node) { return false; }
    virtual bool visit_keyword(AST_keyword* node) { return false; }
    virtual bool visit_lambda(AST_Lambda* node) { return false; }
    virtual bool visit_langprimitive(AST_LangPrimitive* node) { return false; }
    virtual bool visit_list(AST_List* node) { return false; }
    virtual bool visit_listcomp(AST_ListComp* node) { return false; }
    virtual bool visit_module(AST_Module* node) { return false; }
    virtual bool visit_name(AST_Name* node) { return false; }
    virtual bool visit_num(AST_Num* node) { return false; }
    virtual bool visit_pass(AST_Pass* node) { return false; }
    virtual bool visit_print(AST_Print* node) { return false; }
    virtual bool visit_raise(AST_Raise* node) { return false; }
    virtual bool visit_repr(AST_Repr* node) { return false; }
    virtual bool visit_return(AST_Return* node) { return false; }
    virtual bool visit_set(AST_Set* node) { return false; }
    virtual bool visit_setcomp(AST_SetComp* node) { return false; }
    virtual bool visit_slice(AST_Slice* node) { return false; }
    virtual bool visit_str(AST_Str* node) { return false; }
    virtual bool visit_subscript(AST_Subscript* node) { return false; }
    virtual bool visit_tryexcept(AST_TryExcept* node) { return false; }
    virtual bool visit_tryfinally(AST_TryFinally* node) { return false; }
    virtual bool visit_tuple(AST_Tuple* node) { return false; }
    virtual bool visit_unaryop(AST_UnaryOp* node) { return false; }
    virtual bool visit_while(AST_While* node) { return false; }
    virtual bool visit_with(AST_With* node) { return false; }
    virtual bool visit_yield(AST_Yield* node) { return false; }

    virtual bool visit_branch(AST_Branch* node) { return false; }
    virtual bool visit_jump(AST_Jump* node) { return false; }
    virtual bool visit_makeclass(AST_MakeClass* node) { return false; }
    virtual bool visit_makefunction(AST_MakeFunction* node) { return false; }
};

class ExprVisitor {
protected:
public:
    virtual ~ExprVisitor() {}

    virtual void* visit_augbinop(AST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_attribute(AST_Attribute* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_binop(AST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_boolop(AST_BoolOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_call(AST_Call* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_clsattribute(AST_ClsAttribute* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_compare(AST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_dict(AST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_dictcomp(AST_DictComp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_ellipsis(AST_Ellipsis* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_extslice(AST_ExtSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_generatorexp(AST_GeneratorExp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_ifexp(AST_IfExp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_index(AST_Index* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_lambda(AST_Lambda* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_langprimitive(AST_LangPrimitive* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_list(AST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_listcomp(AST_ListComp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_name(AST_Name* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_num(AST_Num* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_repr(AST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_set(AST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_setcomp(AST_SetComp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_slice(AST_Slice* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_str(AST_Str* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_subscript(AST_Subscript* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_tuple(AST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_unaryop(AST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_yield(AST_Yield* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_makeclass(AST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual void* visit_makefunction(AST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
};

class StmtVisitor {
protected:
public:
    virtual ~StmtVisitor() {}

    virtual void visit_assert(AST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_assign(AST_Assign* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_augassign(AST_AugAssign* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_break(AST_Break* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_classdef(AST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_delete(AST_Delete* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_continue(AST_Continue* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_exec(AST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_expr(AST_Expr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_for(AST_For* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_functiondef(AST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_global(AST_Global* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_if(AST_If* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_import(AST_Import* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importfrom(AST_ImportFrom* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_invoke(AST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_pass(AST_Pass* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_print(AST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_raise(AST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_return(AST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_tryexcept(AST_TryExcept* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_tryfinally(AST_TryFinally* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_while(AST_While* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_with(AST_With* node) { RELEASE_ASSERT(0, ""); }

    virtual void visit_branch(AST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_jump(AST_Jump* node) { RELEASE_ASSERT(0, ""); }
};

void print_ast(AST* ast);
class PrintVisitor : public ASTVisitor {
private:
    int indent;
    void printIndent();

public:
    PrintVisitor(int indent = 0) : indent(indent) {}
    virtual ~PrintVisitor() {}

    virtual bool visit_alias(AST_alias* node);
    virtual bool visit_arguments(AST_arguments* node);
    virtual bool visit_assert(AST_Assert* node);
    virtual bool visit_assign(AST_Assign* node);
    virtual bool visit_augassign(AST_AugAssign* node);
    virtual bool visit_augbinop(AST_AugBinOp* node);
    virtual bool visit_attribute(AST_Attribute* node);
    virtual bool visit_binop(AST_BinOp* node);
    virtual bool visit_boolop(AST_BoolOp* node);
    virtual bool visit_break(AST_Break* node);
    virtual bool visit_call(AST_Call* node);
    virtual bool visit_compare(AST_Compare* node);
    virtual bool visit_comprehension(AST_comprehension* node);
    virtual bool visit_classdef(AST_ClassDef* node);
    virtual bool visit_clsattribute(AST_ClsAttribute* node);
    virtual bool visit_continue(AST_Continue* node);
    virtual bool visit_delete(AST_Delete* node);
    virtual bool visit_dict(AST_Dict* node);
    virtual bool visit_dictcomp(AST_DictComp* node);
    virtual bool visit_ellipsis(AST_Ellipsis* node);
    virtual bool visit_excepthandler(AST_ExceptHandler* node);
    virtual bool visit_exec(AST_Exec* node);
    virtual bool visit_expr(AST_Expr* node);
    virtual bool visit_expression(AST_Expression* node);
    virtual bool visit_suite(AST_Suite* node);
    virtual bool visit_extslice(AST_ExtSlice* node);
    virtual bool visit_for(AST_For* node);
    virtual bool visit_functiondef(AST_FunctionDef* node);
    virtual bool visit_generatorexp(AST_GeneratorExp* node);
    virtual bool visit_global(AST_Global* node);
    virtual bool visit_if(AST_If* node);
    virtual bool visit_ifexp(AST_IfExp* node);
    virtual bool visit_import(AST_Import* node);
    virtual bool visit_importfrom(AST_ImportFrom* node);
    virtual bool visit_index(AST_Index* node);
    virtual bool visit_invoke(AST_Invoke* node);
    virtual bool visit_keyword(AST_keyword* node);
    virtual bool visit_lambda(AST_Lambda* node);
    virtual bool visit_langprimitive(AST_LangPrimitive* node);
    virtual bool visit_list(AST_List* node);
    virtual bool visit_listcomp(AST_ListComp* node);
    virtual bool visit_module(AST_Module* node);
    virtual bool visit_name(AST_Name* node);
    virtual bool visit_num(AST_Num* node);
    virtual bool visit_pass(AST_Pass* node);
    virtual bool visit_print(AST_Print* node);
    virtual bool visit_raise(AST_Raise* node);
    virtual bool visit_repr(AST_Repr* node);
    virtual bool visit_return(AST_Return* node);
    virtual bool visit_set(AST_Set* node);
    virtual bool visit_setcomp(AST_SetComp* node);
    virtual bool visit_slice(AST_Slice* node);
    virtual bool visit_str(AST_Str* node);
    virtual bool visit_subscript(AST_Subscript* node);
    virtual bool visit_tuple(AST_Tuple* node);
    virtual bool visit_tryexcept(AST_TryExcept* node);
    virtual bool visit_tryfinally(AST_TryFinally* node);
    virtual bool visit_unaryop(AST_UnaryOp* node);
    virtual bool visit_while(AST_While* node);
    virtual bool visit_with(AST_With* node);
    virtual bool visit_yield(AST_Yield* node);

    virtual bool visit_branch(AST_Branch* node);
    virtual bool visit_jump(AST_Jump* node);
    virtual bool visit_makefunction(AST_MakeFunction* node);
    virtual bool visit_makeclass(AST_MakeClass* node);
};

// Given an AST node, return a vector of the node plus all its descendents.
// This is useful for analyses that care more about the constituent nodes than the
// exact tree structure; ex, finding all "global" directives.
void flatten(const std::vector<AST_stmt*>& roots, std::vector<AST*>& output, bool expand_scopes);
void flatten(AST_expr* root, std::vector<AST*>& output, bool expand_scopes);
// Similar to the flatten() function, but filters for a specific type of ast nodes:
template <class T, class R> void findNodes(const R& roots, std::vector<T*>& output, bool expand_scopes) {
    std::vector<AST*> flattened;
    flatten(roots, flattened, expand_scopes);
    for (AST* n : flattened) {
        if (n->type == T::TYPE)
            output.push_back(reinterpret_cast<T*>(n));
    }
}

llvm::StringRef getOpSymbol(int op_type);
BoxedString* getOpName(int op_type);
int getReverseCmpOp(int op_type, bool& success);
BoxedString* getReverseOpName(int op_type);
BoxedString* getInplaceOpName(int op_type);
std::string getInplaceOpSymbol(int op_type);
};

#endif
