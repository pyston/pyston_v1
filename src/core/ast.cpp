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

#include "core/ast.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "Python.h"

#include "core/cfg.h"
#include "runtime/types.h"

namespace pyston {

#ifdef DEBUG_LINE_NUMBERS
int AST::next_lineno = 100000;

AST::AST(AST_TYPE::AST_TYPE type) : type(type), lineno(++next_lineno) {
    // if (lineno == 100644)
    // raise(SIGTRAP);
}

#endif

llvm::StringRef getOpSymbol(int op_type) {
    switch (op_type) {
        case AST_TYPE::Add:
            return "+";
        case AST_TYPE::BitAnd:
            return "&";
        case AST_TYPE::BitOr:
            return "|";
        case AST_TYPE::BitXor:
            return "^";
        case AST_TYPE::Div:
        case AST_TYPE::TrueDiv:
            return "/";
        case AST_TYPE::DivMod:
            return "divmod()";
        case AST_TYPE::Eq:
            return "==";
        case AST_TYPE::FloorDiv:
            return "//";
        case AST_TYPE::LShift:
            return "<<";
        case AST_TYPE::Lt:
            return "<";
        case AST_TYPE::LtE:
            return "<=";
        case AST_TYPE::Gt:
            return ">";
        case AST_TYPE::GtE:
            return ">=";
        case AST_TYPE::In:
            return "in";
        case AST_TYPE::Invert:
            return "~";
        case AST_TYPE::Is:
            return "is";
        case AST_TYPE::IsNot:
            return "is not";
        case AST_TYPE::Mod:
            return "%";
        case AST_TYPE::Mult:
            return "*";
        case AST_TYPE::Not:
            return "not";
        case AST_TYPE::NotEq:
            return "!=";
        case AST_TYPE::NotIn:
            return "not in";
        case AST_TYPE::Pow:
            return "**";
        case AST_TYPE::RShift:
            return ">>";
        case AST_TYPE::Sub:
            return "-";
        case AST_TYPE::UAdd:
            return "+";
        case AST_TYPE::USub:
            return "-";
        default:
            fprintf(stderr, "Unknown op type (" __FILE__ ":" STRINGIFY(__LINE__) "): %d\n", op_type);
            abort();
    }
}

std::string getInplaceOpSymbol(int op_type) {
    return std::string(getOpSymbol(op_type)) + '=';
}

BoxedString* getOpName(int op_type) {
    assert(op_type != AST_TYPE::Is);
    assert(op_type != AST_TYPE::IsNot);

    static BoxedString* strAdd, *strBitAnd, *strBitOr, *strBitXor, *strDiv, *strTrueDiv, *strDivMod, *strEq,
        *strFloorDiv, *strLShift, *strLt, *strLtE, *strGt, *strGtE, *strIn, *strInvert, *strMod, *strMult, *strNot,
        *strNotEq, *strPow, *strRShift, *strSub, *strUAdd, *strUSub;

    static bool initialized = false;
    if (!initialized) {
        strAdd = internStringImmortal("__add__");
        strBitAnd = internStringImmortal("__and__");
        strBitOr = internStringImmortal("__or__");
        strBitXor = internStringImmortal("__xor__");
        strDiv = internStringImmortal("__div__");
        strTrueDiv = internStringImmortal("__truediv__");
        strDivMod = internStringImmortal("__divmod__");
        strEq = internStringImmortal("__eq__");
        strFloorDiv = internStringImmortal("__floordiv__");
        strLShift = internStringImmortal("__lshift__");
        strLt = internStringImmortal("__lt__");
        strLtE = internStringImmortal("__le__");
        strGt = internStringImmortal("__gt__");
        strGtE = internStringImmortal("__ge__");
        strIn = internStringImmortal("__contains__");
        strInvert = internStringImmortal("__invert__");
        strMod = internStringImmortal("__mod__");
        strMult = internStringImmortal("__mul__");
        strNot = internStringImmortal("__nonzero__");
        strNotEq = internStringImmortal("__ne__");
        strPow = internStringImmortal("__pow__");
        strRShift = internStringImmortal("__rshift__");
        strSub = internStringImmortal("__sub__");
        strUAdd = internStringImmortal("__pos__");
        strUSub = internStringImmortal("__neg__");

        initialized = true;
    }

    switch (op_type) {
        case AST_TYPE::Add:
            return strAdd;
        case AST_TYPE::BitAnd:
            return strBitAnd;
        case AST_TYPE::BitOr:
            return strBitOr;
        case AST_TYPE::BitXor:
            return strBitXor;
        case AST_TYPE::Div:
            return strDiv;
        case AST_TYPE::TrueDiv:
            return strTrueDiv;
        case AST_TYPE::DivMod:
            return strDivMod;
        case AST_TYPE::Eq:
            return strEq;
        case AST_TYPE::FloorDiv:
            return strFloorDiv;
        case AST_TYPE::LShift:
            return strLShift;
        case AST_TYPE::Lt:
            return strLt;
        case AST_TYPE::LtE:
            return strLtE;
        case AST_TYPE::Gt:
            return strGt;
        case AST_TYPE::GtE:
            return strGtE;
        case AST_TYPE::In:
            return strIn;
        case AST_TYPE::Invert:
            return strInvert;
        case AST_TYPE::Mod:
            return strMod;
        case AST_TYPE::Mult:
            return strMult;
        case AST_TYPE::Not:
            return strNot;
        case AST_TYPE::NotEq:
            return strNotEq;
        case AST_TYPE::Pow:
            return strPow;
        case AST_TYPE::RShift:
            return strRShift;
        case AST_TYPE::Sub:
            return strSub;
        case AST_TYPE::UAdd:
            return strUAdd;
        case AST_TYPE::USub:
            return strUSub;
        default:
            fprintf(stderr, "Unknown op type (" __FILE__ ":" STRINGIFY(__LINE__) "): %d\n", op_type);
            abort();
    }
}

BoxedString* getInplaceOpName(int op_type) {
    BoxedString* normal_name = getOpName(op_type);
    // TODO inefficient
    return internStringImmortal(("__i" + normal_name->s().substr(2).str()).c_str());
}

// Maybe better name is "swapped" -- it's what the runtime will try if the normal op
// name fails, it will switch the order of the lhs/rhs and call the reverse op.
// Calling it "reverse" because that's what I'm assuming the 'r' stands for in ex __radd__
int getReverseCmpOp(int op_type, bool& success) {
    success = true;
    if (op_type == AST_TYPE::Lt)
        return AST_TYPE::Gt;
    if (op_type == AST_TYPE::LtE)
        return AST_TYPE::GtE;
    if (op_type == AST_TYPE::Gt)
        return AST_TYPE::Lt;
    if (op_type == AST_TYPE::GtE)
        return AST_TYPE::LtE;
    if (op_type == AST_TYPE::NotEq)
        return AST_TYPE::NotEq;
    if (op_type == AST_TYPE::Eq)
        return AST_TYPE::Eq;
    success = false;
    return op_type;
}

BoxedString* getReverseOpName(int op_type) {
    bool reversed = false;
    op_type = getReverseCmpOp(op_type, reversed);
    if (reversed)
        return getOpName(op_type);
    BoxedString* normal_name = getOpName(op_type);
    // TODO inefficient
    return internStringImmortal(("__r" + normal_name->s().substr(2).str()).c_str());
}

template <class T> static void visitVector(const std::vector<T*>& vec, ASTVisitor* v) {
    for (int i = 0; i < vec.size(); i++) {
        vec[i]->accept(v);
    }
}

void AST_alias::accept(ASTVisitor* v) {
    bool skip = v->visit_alias(this);
    if (skip)
        return;
}

void AST_arguments::accept(ASTVisitor* v) {
    bool skip = v->visit_arguments(this);
    if (skip)
        return;

    visitVector(defaults, v);
    visitVector(args, v);
}

void AST_Assert::accept(ASTVisitor* v) {
    bool skip = v->visit_assert(this);
    if (skip)
        return;

    test->accept(v);
    if (msg)
        msg->accept(v);
}

void AST_Assert::accept_stmt(StmtVisitor* v) {
    v->visit_assert(this);
}

void AST_Assign::accept(ASTVisitor* v) {
    bool skip = v->visit_assign(this);
    if (skip)
        return;

    value->accept(v);
    for (int i = 0; i < targets.size(); i++) {
        // Targets are assigned to left-to-right, so this is valid:
        // x = x.a = object()
        // but this is not:
        // x.a = x = object()
        targets[i]->accept(v);
    }
}

void AST_Assign::accept_stmt(StmtVisitor* v) {
    v->visit_assign(this);
}

void AST_AugAssign::accept(ASTVisitor* v) {
    bool skip = v->visit_augassign(this);
    if (skip)
        return;

    value->accept(v);
    target->accept(v);
}

void AST_AugAssign::accept_stmt(StmtVisitor* v) {
    v->visit_augassign(this);
}

void AST_AugBinOp::accept(ASTVisitor* v) {
    bool skip = v->visit_augbinop(this);
    if (skip)
        return;

    left->accept(v);
    right->accept(v);
}

void* AST_AugBinOp::accept_expr(ExprVisitor* v) {
    return v->visit_augbinop(this);
}

void AST_Attribute::accept(ASTVisitor* v) {
    bool skip = v->visit_attribute(this);
    if (skip)
        return;

    value->accept(v);
}

void* AST_Attribute::accept_expr(ExprVisitor* v) {
    return v->visit_attribute(this);
}

void AST_BinOp::accept(ASTVisitor* v) {
    bool skip = v->visit_binop(this);
    if (skip)
        return;

    left->accept(v);
    right->accept(v);
}

void* AST_BinOp::accept_expr(ExprVisitor* v) {
    return v->visit_binop(this);
}

void AST_BoolOp::accept(ASTVisitor* v) {
    bool skip = v->visit_boolop(this);
    if (skip)
        return;

    visitVector(values, v);
}

void* AST_BoolOp::accept_expr(ExprVisitor* v) {
    return v->visit_boolop(this);
}

void AST_Break::accept(ASTVisitor* v) {
    bool skip = v->visit_break(this);
    if (skip)
        return;
}

void AST_Break::accept_stmt(StmtVisitor* v) {
    v->visit_break(this);
}

void AST_Call::accept(ASTVisitor* v) {
    bool skip = v->visit_call(this);
    if (skip)
        return;

    func->accept(v);
    visitVector(args, v);
    visitVector(keywords, v);
    if (starargs)
        starargs->accept(v);
    if (kwargs)
        kwargs->accept(v);
}

void* AST_Call::accept_expr(ExprVisitor* v) {
    return v->visit_call(this);
}

void AST_Compare::accept(ASTVisitor* v) {
    bool skip = v->visit_compare(this);
    if (skip)
        return;

    left->accept(v);
    visitVector(comparators, v);
}

void* AST_Compare::accept_expr(ExprVisitor* v) {
    return v->visit_compare(this);
}

void AST_comprehension::accept(ASTVisitor* v) {
    bool skip = v->visit_comprehension(this);
    if (skip)
        return;

    target->accept(v);
    iter->accept(v);
    for (auto if_ : ifs) {
        if_->accept(v);
    }
}

void AST_ClassDef::accept(ASTVisitor* v) {
    bool skip = v->visit_classdef(this);
    if (skip)
        return;

    visitVector(this->bases, v);
    visitVector(this->decorator_list, v);
    visitVector(this->body, v);
}

void AST_ClassDef::accept_stmt(StmtVisitor* v) {
    v->visit_classdef(this);
}

void AST_Continue::accept(ASTVisitor* v) {
    bool skip = v->visit_continue(this);
    if (skip)
        return;
}

void AST_Continue::accept_stmt(StmtVisitor* v) {
    v->visit_continue(this);
}

void AST_Delete::accept(ASTVisitor* v) {
    bool skip = v->visit_delete(this);
    if (skip)
        return;

    visitVector(this->targets, v);
}

void AST_Delete::accept_stmt(StmtVisitor* v) {
    v->visit_delete(this);
}

void AST_Dict::accept(ASTVisitor* v) {
    bool skip = v->visit_dict(this);
    if (skip)
        return;

    for (int i = 0; i < keys.size(); i++) {
        keys[i]->accept(v);
        values[i]->accept(v);
    }
}

void* AST_Dict::accept_expr(ExprVisitor* v) {
    return v->visit_dict(this);
}

void AST_DictComp::accept(ASTVisitor* v) {
    bool skip = v->visit_dictcomp(this);
    if (skip)
        return;

    for (auto c : generators) {
        c->accept(v);
    }

    value->accept(v);
    key->accept(v);
}

void* AST_DictComp::accept_expr(ExprVisitor* v) {
    return v->visit_dictcomp(this);
}

void AST_Ellipsis::accept(ASTVisitor* v) {
    bool skip = v->visit_ellipsis(this);
    if (skip)
        return;
}

void* AST_Ellipsis::accept_expr(ExprVisitor* v) {
    return v->visit_ellipsis(this);
}

void AST_ExceptHandler::accept(ASTVisitor* v) {
    bool skip = v->visit_excepthandler(this);
    if (skip)
        return;

    if (type)
        type->accept(v);
    if (name)
        name->accept(v);
    visitVector(body, v);
}

void AST_Exec::accept(ASTVisitor* v) {
    bool skip = v->visit_exec(this);
    if (skip)
        return;

    if (body)
        body->accept(v);
    if (globals)
        globals->accept(v);
    if (locals)
        locals->accept(v);
}

void AST_Exec::accept_stmt(StmtVisitor* v) {
    v->visit_exec(this);
}

void AST_Expr::accept(ASTVisitor* v) {
    bool skip = v->visit_expr(this);
    if (skip)
        return;

    value->accept(v);
}

void AST_Expr::accept_stmt(StmtVisitor* v) {
    v->visit_expr(this);
}


void AST_ExtSlice::accept(ASTVisitor* v) {
    bool skip = v->visit_extslice(this);
    if (skip)
        return;
    visitVector(dims, v);
}

void* AST_ExtSlice::accept_expr(ExprVisitor* v) {
    return v->visit_extslice(this);
}

void AST_For::accept(ASTVisitor* v) {
    bool skip = v->visit_for(this);
    if (skip)
        return;

    iter->accept(v);
    target->accept(v);
    visitVector(body, v);
    visitVector(orelse, v);
}

void AST_For::accept_stmt(StmtVisitor* v) {
    v->visit_for(this);
}

void AST_FunctionDef::accept(ASTVisitor* v) {
    bool skip = v->visit_functiondef(this);
    if (skip)
        return;

    visitVector(decorator_list, v);
    args->accept(v);
    visitVector(body, v);
}

void AST_FunctionDef::accept_stmt(StmtVisitor* v) {
    v->visit_functiondef(this);
}

void AST_GeneratorExp::accept(ASTVisitor* v) {
    bool skip = v->visit_generatorexp(this);
    if (skip)
        return;

    for (auto c : generators) {
        c->accept(v);
    }

    elt->accept(v);
}

void* AST_GeneratorExp::accept_expr(ExprVisitor* v) {
    return v->visit_generatorexp(this);
}

void AST_Global::accept(ASTVisitor* v) {
    bool skip = v->visit_global(this);
    if (skip)
        return;
}

void AST_Global::accept_stmt(StmtVisitor* v) {
    v->visit_global(this);
}

void AST_If::accept(ASTVisitor* v) {
    bool skip = v->visit_if(this);
    if (skip)
        return;

    test->accept(v);
    visitVector(body, v);
    visitVector(orelse, v);
}

void AST_If::accept_stmt(StmtVisitor* v) {
    v->visit_if(this);
}

void AST_IfExp::accept(ASTVisitor* v) {
    bool skip = v->visit_ifexp(this);
    if (skip)
        return;

    this->test->accept(v);
    this->body->accept(v);
    this->orelse->accept(v);
}

void* AST_IfExp::accept_expr(ExprVisitor* v) {
    return v->visit_ifexp(this);
}

void AST_Import::accept(ASTVisitor* v) {
    bool skip = v->visit_import(this);
    if (skip)
        return;

    visitVector(names, v);
}

void AST_Import::accept_stmt(StmtVisitor* v) {
    v->visit_import(this);
}

void AST_ImportFrom::accept(ASTVisitor* v) {
    bool skip = v->visit_importfrom(this);
    if (skip)
        return;

    visitVector(names, v);
}

void AST_ImportFrom::accept_stmt(StmtVisitor* v) {
    v->visit_importfrom(this);
}

void AST_Index::accept(ASTVisitor* v) {
    bool skip = v->visit_index(this);
    if (skip)
        return;

    this->value->accept(v);
}

void* AST_Index::accept_expr(ExprVisitor* v) {
    return v->visit_index(this);
}

void AST_Invoke::accept(ASTVisitor* v) {
    bool skip = v->visit_invoke(this);
    if (skip)
        return;

    this->stmt->accept(v);
}

void AST_Invoke::accept_stmt(StmtVisitor* v) {
    return v->visit_invoke(this);
}

void AST_keyword::accept(ASTVisitor* v) {
    bool skip = v->visit_keyword(this);
    if (skip)
        return;

    value->accept(v);
}

void AST_Lambda::accept(ASTVisitor* v) {
    bool skip = v->visit_lambda(this);
    if (skip)
        return;

    args->accept(v);
    body->accept(v);
}

void* AST_Lambda::accept_expr(ExprVisitor* v) {
    return v->visit_lambda(this);
}

void AST_LangPrimitive::accept(ASTVisitor* v) {
    bool skip = v->visit_langprimitive(this);
    if (skip)
        return;

    visitVector(args, v);
}

void* AST_LangPrimitive::accept_expr(ExprVisitor* v) {
    return v->visit_langprimitive(this);
}

void AST_List::accept(ASTVisitor* v) {
    bool skip = v->visit_list(this);
    if (skip)
        return;

    visitVector(elts, v);
}

void* AST_List::accept_expr(ExprVisitor* v) {
    return v->visit_list(this);
}

void AST_ListComp::accept(ASTVisitor* v) {
    bool skip = v->visit_listcomp(this);
    if (skip)
        return;

    for (auto c : generators) {
        c->accept(v);
    }

    elt->accept(v);
}

void* AST_ListComp::accept_expr(ExprVisitor* v) {
    return v->visit_listcomp(this);
}

void AST_Module::accept(ASTVisitor* v) {
    bool skip = v->visit_module(this);
    if (skip)
        return;

    visitVector(body, v);
}

void AST_Expression::accept(ASTVisitor* v) {
    bool skip = v->visit_expression(this);
    if (skip)
        return;

    body->accept(v);
}

void AST_Suite::accept(ASTVisitor* v) {
    bool skip = v->visit_suite(this);
    if (skip)
        return;

    visitVector(body, v);
}

void AST_Name::accept(ASTVisitor* v) {
    bool skip = v->visit_name(this);
}

void* AST_Name::accept_expr(ExprVisitor* v) {
    return v->visit_name(this);
}

void AST_Num::accept(ASTVisitor* v) {
    bool skip = v->visit_num(this);
}

void* AST_Num::accept_expr(ExprVisitor* v) {
    return v->visit_num(this);
}

void AST_Pass::accept(ASTVisitor* v) {
    bool skip = v->visit_pass(this);
}

void AST_Pass::accept_stmt(StmtVisitor* v) {
    v->visit_pass(this);
}

void AST_Print::accept(ASTVisitor* v) {
    bool skip = v->visit_print(this);
    if (skip)
        return;

    if (dest)
        dest->accept(v);
    visitVector(values, v);
}

void AST_Print::accept_stmt(StmtVisitor* v) {
    v->visit_print(this);
}

void AST_Raise::accept(ASTVisitor* v) {
    bool skip = v->visit_raise(this);
    if (skip)
        return;

    if (arg0)
        arg0->accept(v);
    if (arg1)
        arg1->accept(v);
    if (arg2)
        arg2->accept(v);
}

void AST_Raise::accept_stmt(StmtVisitor* v) {
    v->visit_raise(this);
}

void AST_Repr::accept(ASTVisitor* v) {
    bool skip = v->visit_repr(this);
    if (skip)
        return;

    value->accept(v);
}

void* AST_Repr::accept_expr(ExprVisitor* v) {
    return v->visit_repr(this);
}

void AST_Return::accept(ASTVisitor* v) {
    bool skip = v->visit_return(this);
    if (skip)
        return;

    if (value)
        value->accept(v);
}

void AST_Return::accept_stmt(StmtVisitor* v) {
    v->visit_return(this);
}

void AST_Set::accept(ASTVisitor* v) {
    bool skip = v->visit_set(this);
    if (skip)
        return;

    visitVector(elts, v);
}

void* AST_Set::accept_expr(ExprVisitor* v) {
    return v->visit_set(this);
}

void AST_SetComp::accept(ASTVisitor* v) {
    bool skip = v->visit_setcomp(this);
    if (skip)
        return;

    for (auto c : generators) {
        c->accept(v);
    }

    elt->accept(v);
}

void* AST_SetComp::accept_expr(ExprVisitor* v) {
    return v->visit_setcomp(this);
}

void AST_Slice::accept(ASTVisitor* v) {
    bool skip = v->visit_slice(this);
    if (skip)
        return;

    if (lower)
        lower->accept(v);
    if (upper)
        upper->accept(v);
    if (step)
        step->accept(v);
}

void* AST_Slice::accept_expr(ExprVisitor* v) {
    return v->visit_slice(this);
}

void AST_Str::accept(ASTVisitor* v) {
    bool skip = v->visit_str(this);
    if (skip)
        return;
}

void* AST_Str::accept_expr(ExprVisitor* v) {
    return v->visit_str(this);
}

void AST_Subscript::accept(ASTVisitor* v) {
    bool skip = v->visit_subscript(this);
    if (skip)
        return;

    this->value->accept(v);
    this->slice->accept(v);
}

void* AST_Subscript::accept_expr(ExprVisitor* v) {
    return v->visit_subscript(this);
}

void AST_TryExcept::accept(ASTVisitor* v) {
    bool skip = v->visit_tryexcept(this);
    if (skip)
        return;

    visitVector(body, v);
    visitVector(orelse, v);
    visitVector(handlers, v);
}

void AST_TryExcept::accept_stmt(StmtVisitor* v) {
    v->visit_tryexcept(this);
}

void AST_TryFinally::accept(ASTVisitor* v) {
    bool skip = v->visit_tryfinally(this);
    if (skip)
        return;

    visitVector(body, v);
    visitVector(finalbody, v);
}

void AST_TryFinally::accept_stmt(StmtVisitor* v) {
    v->visit_tryfinally(this);
}

void AST_Tuple::accept(ASTVisitor* v) {
    bool skip = v->visit_tuple(this);
    if (skip)
        return;

    visitVector(elts, v);
}

void* AST_Tuple::accept_expr(ExprVisitor* v) {
    return v->visit_tuple(this);
}

void AST_UnaryOp::accept(ASTVisitor* v) {
    bool skip = v->visit_unaryop(this);
    if (skip)
        return;

    operand->accept(v);
}

void* AST_UnaryOp::accept_expr(ExprVisitor* v) {
    return v->visit_unaryop(this);
}

void AST_While::accept(ASTVisitor* v) {
    bool skip = v->visit_while(this);
    if (skip)
        return;

    test->accept(v);
    visitVector(body, v);
    visitVector(orelse, v);
}

void AST_While::accept_stmt(StmtVisitor* v) {
    v->visit_while(this);
}

void AST_With::accept(ASTVisitor* v) {
    bool skip = v->visit_with(this);
    if (skip)
        return;

    context_expr->accept(v);
    if (optional_vars)
        optional_vars->accept(v);
    visitVector(body, v);
}

void AST_With::accept_stmt(StmtVisitor* v) {
    v->visit_with(this);
}

void AST_Yield::accept(ASTVisitor* v) {
    bool skip = v->visit_yield(this);
    if (skip)
        return;

    if (value)
        value->accept(v);
}

void* AST_Yield::accept_expr(ExprVisitor* v) {
    return v->visit_yield(this);
}

void AST_Branch::accept(ASTVisitor* v) {
    bool skip = v->visit_branch(this);
    if (skip)
        return;

    test->accept(v);
}

void AST_Branch::accept_stmt(StmtVisitor* v) {
    v->visit_branch(this);
}

void AST_Jump::accept(ASTVisitor* v) {
    bool skip = v->visit_jump(this);
    if (skip)
        return;
}

void AST_Jump::accept_stmt(StmtVisitor* v) {
    v->visit_jump(this);
}

void AST_ClsAttribute::accept(ASTVisitor* v) {
    bool skip = v->visit_clsattribute(this);
    if (skip)
        return;

    value->accept(v);
}

void* AST_ClsAttribute::accept_expr(ExprVisitor* v) {
    return v->visit_clsattribute(this);
}

void AST_MakeFunction::accept(ASTVisitor* v) {
    bool skip = v->visit_makefunction(this);
    if (skip)
        return;

    function_def->accept(v);
}

void* AST_MakeFunction::accept_expr(ExprVisitor* v) {
    return v->visit_makefunction(this);
}

void AST_MakeClass::accept(ASTVisitor* v) {
    bool skip = v->visit_makeclass(this);
    if (skip)
        return;

    class_def->accept(v);
}

void* AST_MakeClass::accept_expr(ExprVisitor* v) {
    return v->visit_makeclass(this);
}

void print_ast(AST* ast) {
    PrintVisitor v;
    ast->accept(&v);
}

void PrintVisitor::printIndent() {
    for (int i = 0; i < indent; i++) {
        putchar(' ');
    }
}

bool PrintVisitor::visit_alias(AST_alias* node) {
    printf("%s", node->name.c_str());
    if (node->asname.s().size())
        printf(" as %s", node->asname.c_str());
    return true;
}

bool PrintVisitor::visit_arguments(AST_arguments* node) {
    int nargs = node->args.size();
    int ndefault = node->defaults.size();
    for (int i = 0; i < nargs; i++) {
        if (i > 0)
            printf(", ");

        node->args[i]->accept(this);
        if (i >= nargs - ndefault) {
            printf("=");
            node->defaults[i - (nargs - ndefault)]->accept(this);
        }
    }
    return true;
}

bool PrintVisitor::visit_assert(AST_Assert* node) {
    printf("assert ");
    node->test->accept(this);
    if (node->msg) {
        printf(", ");
        node->msg->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_assign(AST_Assign* node) {
    for (int i = 0; i < node->targets.size(); i++) {
        node->targets[i]->accept(this);
        printf(" = ");
    }
    node->value->accept(this);
    return true;
}

static void printOp(AST_TYPE::AST_TYPE op_type) {
    switch (op_type) {
        case AST_TYPE::Add:
            putchar('+');
            break;
        case AST_TYPE::BitAnd:
            putchar('&');
            break;
        case AST_TYPE::BitOr:
            putchar('|');
            break;
        case AST_TYPE::BitXor:
            putchar('^');
            break;
        case AST_TYPE::Div:
            putchar('/');
            break;
        case AST_TYPE::LShift:
            printf("<<");
            break;
        case AST_TYPE::RShift:
            printf(">>");
            break;
        case AST_TYPE::Pow:
            printf("**");
            break;
        case AST_TYPE::Mod:
            putchar('%');
            break;
        case AST_TYPE::Mult:
            putchar('*');
            break;
        case AST_TYPE::Sub:
            putchar('-');
            break;
        default:
            printf("<%d>", op_type);
            break;
    }
}

bool PrintVisitor::visit_augassign(AST_AugAssign* node) {
    node->target->accept(this);
    printOp(node->op_type);
    putchar('=');
    node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_augbinop(AST_AugBinOp* node) {
    node->left->accept(this);
    printf("=");
    printOp(node->op_type);
    node->right->accept(this);
    return true;
}

bool PrintVisitor::visit_attribute(AST_Attribute* node) {
    node->value->accept(this);
    putchar('.');
    printf("%s", node->attr.c_str());
    return true;
}

bool PrintVisitor::visit_binop(AST_BinOp* node) {
    node->left->accept(this);
    printOp(node->op_type);
    node->right->accept(this);
    return true;
}

bool PrintVisitor::visit_boolop(AST_BoolOp* node) {
    for (int i = 0; i < node->values.size(); i++) {
        node->values[i]->accept(this);

        if (i == node->values.size() - 1)
            continue;
        switch (node->op_type) {
            case AST_TYPE::And:
                printf(" and ");
                break;
            case AST_TYPE::Or:
                printf(" or ");
                break;
            default:
                ASSERT(0, "%d", node->op_type);
                break;
        }
    }
    return true;
}

bool PrintVisitor::visit_break(AST_Break* node) {
    printf("break");
    return true;
}

bool PrintVisitor::visit_call(AST_Call* node) {
    node->func->accept(this);
    printf("(");

    bool prevarg = false;
    for (int i = 0; i < node->args.size(); i++) {
        if (prevarg)
            printf(", ");
        node->args[i]->accept(this);
        prevarg = true;
    }
    for (int i = 0; i < node->keywords.size(); i++) {
        if (prevarg)
            printf(", ");
        node->keywords[i]->accept(this);
        prevarg = true;
    }
    if (node->starargs) {
        if (prevarg)
            printf(", ");
        node->starargs->accept(this);
        prevarg = true;
    }
    if (node->kwargs) {
        if (prevarg)
            printf(", ");
        node->kwargs->accept(this);
        prevarg = true;
    }
    printf(")");
    return true;
}

bool PrintVisitor::visit_compare(AST_Compare* node) {
    node->left->accept(this);

    for (int i = 0; i < node->ops.size(); i++) {
        std::string symbol = getOpSymbol(node->ops[i]);
        printf(" %s ", symbol.c_str());

        node->comparators[i]->accept(this);
    }

    return true;
}

bool PrintVisitor::visit_comprehension(AST_comprehension* node) {
    printf("for ");
    node->target->accept(this);
    printf(" in ");
    node->iter->accept(this);

    for (AST_expr* i : node->ifs) {
        printf(" if ");
        i->accept(this);
    }

    return true;
}

bool PrintVisitor::visit_classdef(AST_ClassDef* node) {
    for (int i = 0, n = node->decorator_list.size(); i < n; i++) {
        printf("@");
        node->decorator_list[i]->accept(this);
        printf("\n");
        printIndent();
    }
    printf("class %s(", node->name.c_str());
    for (int i = 0, n = node->bases.size(); i < n; i++) {
        if (i)
            printf(", ");
        node->bases[i]->accept(this);
    }
    printf(")");

    indent += 4;
    for (int i = 0, n = node->body.size(); i < n; i++) {
        printf("\n");
        printIndent();
        node->body[i]->accept(this);
    }
    indent -= 4;

    return true;
}

bool PrintVisitor::visit_continue(AST_Continue* node) {
    printf("continue");
    return true;
}

bool PrintVisitor::visit_delete(AST_Delete* node) {
    printf("del ");
    for (int i = 0; i < node->targets.size(); i++) {
        if (i > 0)
            printf(", ");
        node->targets[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_dict(AST_Dict* node) {
    printf("{");
    for (int i = 0; i < node->keys.size(); i++) {
        if (i > 0)
            printf(", ");
        node->keys[i]->accept(this);
        printf(":");
        node->values[i]->accept(this);
    }
    printf("}");
    return true;
}

bool PrintVisitor::visit_dictcomp(AST_DictComp* node) {
    printf("{");
    node->key->accept(this);
    printf(":");
    node->value->accept(this);
    for (auto c : node->generators) {
        printf(" ");
        c->accept(this);
    }
    printf("}");
    return true;
}

bool PrintVisitor::visit_ellipsis(AST_Ellipsis*) {
    printf("...");
    return true;
}

bool PrintVisitor::visit_excepthandler(AST_ExceptHandler* node) {
    printf("except");
    if (node->type) {
        printf(" ");
        node->type->accept(this);
    }
    if (node->name) {
        printf(" as ");
        node->name->accept(this);
    }
    printf(":\n");

    indent += 4;
    for (AST* subnode : node->body) {
        printIndent();
        subnode->accept(this);
        printf("\n");
    }
    indent -= 4;
    return true;
}

bool PrintVisitor::visit_exec(AST_Exec* node) {
    printf("exec ");

    node->body->accept(this);
    if (node->globals) {
        printf(" in ");
        node->globals->accept(this);

        if (node->locals) {
            printf(", ");
            node->locals->accept(this);
        }
    }
    printf("\n");
    return true;
}

bool PrintVisitor::visit_expr(AST_Expr* node) {
    return false;
}

bool PrintVisitor::visit_extslice(AST_ExtSlice* node) {
    for (int i = 0; i < node->dims.size(); ++i) {
        if (i > 0)
            printf(", ");
        node->dims[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_for(AST_For* node) {
    printf("<for loop>\n");
    return true;
}

bool PrintVisitor::visit_functiondef(AST_FunctionDef* node) {
    for (auto d : node->decorator_list) {
        printf("@");
        d->accept(this);
        printf("\n");
        printIndent();
    }

    printf("def %s(", node->name.c_str());
    node->args->accept(this);
    printf(")");

    indent += 4;
    for (int i = 0; i < node->body.size(); i++) {
        printf("\n");
        printIndent();
        node->body[i]->accept(this);
    }
    indent -= 4;
    return true;
}

bool PrintVisitor::visit_generatorexp(AST_GeneratorExp* node) {
    printf("[");
    node->elt->accept(this);
    for (auto c : node->generators) {
        printf(" ");
        c->accept(this);
    }
    printf("]");
    return true;
}

bool PrintVisitor::visit_global(AST_Global* node) {
    printf("global ");
    for (int i = 0; i < node->names.size(); i++) {
        if (i > 0)
            printf(", ");
        printf("%s", node->names[i].c_str());
    }
    return true;
}

bool PrintVisitor::visit_if(AST_If* node) {
    printf("if ");
    node->test->accept(this);
    printf(":\n");

    indent += 4;
    for (int i = 0; i < node->body.size(); i++) {
        printIndent();
        node->body[i]->accept(this);
        printf("\n");
    }
    indent -= 4;

    if (node->orelse.size()) {
        printIndent();
        bool elif = false;

        if (node->orelse.size() == 1 && node->orelse[0]->type == AST_TYPE::If)
            elif = true;

        if (elif) {
            printf("el");
        } else {
            printf("else:\n");
            indent += 4;
        }
        for (int i = 0; i < node->orelse.size(); i++) {
            if (i)
                printf("\n");
            printIndent();
            node->orelse[i]->accept(this);
        }
        if (!elif)
            indent -= 4;
    }
    return true;
}

bool PrintVisitor::visit_ifexp(AST_IfExp* node) {
    node->body->accept(this);
    printf(" if ");
    node->test->accept(this);
    printf(" else ");
    node->orelse->accept(this);
    return true;
}

bool PrintVisitor::visit_import(AST_Import* node) {
    printf("import ");
    for (int i = 0; i < node->names.size(); i++) {
        if (i > 0)
            printf(", ");
        node->names[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_importfrom(AST_ImportFrom* node) {
    printf("from %s import ", node->module.c_str());
    for (int i = 0; i < node->names.size(); i++) {
        if (i > 0)
            printf(", ");
        node->names[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_index(AST_Index* node) {
    return false;
}

bool PrintVisitor::visit_invoke(AST_Invoke* node) {
    printf("invoke %d %d: ", node->normal_dest->idx, node->exc_dest->idx);
    node->stmt->accept(this);
    return true;
}

bool PrintVisitor::visit_lambda(AST_Lambda* node) {
    printf("lambda ");
    node->args->accept(this);
    printf(": ");
    node->body->accept(this);
    return true;
}

bool PrintVisitor::visit_langprimitive(AST_LangPrimitive* node) {
    printf(":");
    switch (node->opcode) {
        case AST_LangPrimitive::CHECK_EXC_MATCH:
            printf("CHECK_EXC_MATCH");
            break;
        case AST_LangPrimitive::LANDINGPAD:
            printf("LANDINGPAD");
            break;
        case AST_LangPrimitive::LOCALS:
            printf("LOCALS");
            break;
        case AST_LangPrimitive::GET_ITER:
            printf("GET_ITER");
            break;
        case AST_LangPrimitive::IMPORT_FROM:
            printf("IMPORT_FROM");
            break;
        case AST_LangPrimitive::IMPORT_NAME:
            printf("IMPORT_NAME");
            break;
        case AST_LangPrimitive::IMPORT_STAR:
            printf("IMPORT_STAR");
            break;
        case AST_LangPrimitive::NONE:
            printf("NONE");
            break;
        case AST_LangPrimitive::NONZERO:
            printf("NONZERO");
            break;
        case AST_LangPrimitive::SET_EXC_INFO:
            printf("SET_EXC_INFO");
            break;
        case AST_LangPrimitive::UNCACHE_EXC_INFO:
            printf("UNCACHE_EXC_INFO");
            break;
        case AST_LangPrimitive::HASNEXT:
            printf("HASNEXT");
            break;
        default:
            RELEASE_ASSERT(0, "%d", node->opcode);
    }
    printf("(");
    for (int i = 0, n = node->args.size(); i < n; ++i) {
        if (i > 0)
            printf(", ");
        node->args[i]->accept(this);
    }
    printf(")");
    return true;
}

bool PrintVisitor::visit_list(AST_List* node) {
    printf("[");
    for (int i = 0, n = node->elts.size(); i < n; ++i) {
        if (i > 0)
            printf(", ");
        node->elts[i]->accept(this);
    }
    printf("]");
    return true;
}

bool PrintVisitor::visit_listcomp(AST_ListComp* node) {
    printf("[");
    node->elt->accept(this);
    for (auto c : node->generators) {
        printf(" ");
        c->accept(this);
    }
    printf("]");
    return true;
}

bool PrintVisitor::visit_keyword(AST_keyword* node) {
    printf("%s=", node->arg.c_str());
    node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_module(AST_Module* node) {
    // printf("<module>\n");
    for (int i = 0; i < node->body.size(); i++) {
        node->body[i]->accept(this);
        printf("\n");
    }
    return true;
}

bool PrintVisitor::visit_expression(AST_Expression* node) {
    node->body->accept(this);
    printf("\n");
    return true;
}

bool PrintVisitor::visit_suite(AST_Suite* node) {
    for (int i = 0; i < node->body.size(); i++) {
        printIndent();
        node->body[i]->accept(this);
        printf("\n");
    }
    return true;
}

bool PrintVisitor::visit_name(AST_Name* node) {
    printf("%s", node->id.c_str());
    // printf("%s(%d)", node->id.c_str(), node->ctx_type);
    return false;
}

bool PrintVisitor::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT) {
        printf("%ld", node->n_int);
    } else if (node->num_type == AST_Num::LONG) {
        printf("%sL", node->n_long.c_str());
    } else if (node->num_type == AST_Num::FLOAT) {
        printf("%f", node->n_float);
    } else if (node->num_type == AST_Num::COMPLEX) {
        printf("%fj", node->n_float);
    } else {
        RELEASE_ASSERT(0, "");
    }
    return false;
}

bool PrintVisitor::visit_pass(AST_Pass* node) {
    printf("pass");
    return true;
}

bool PrintVisitor::visit_print(AST_Print* node) {
    printf("print ");
    if (node->dest) {
        printf(">>");
        node->dest->accept(this);
        printf(", ");
    }
    for (int i = 0; i < node->values.size(); i++) {
        if (i > 0)
            printf(", ");
        node->values[i]->accept(this);
    }
    if (!node->nl)
        printf(",");
    return true;
}

bool PrintVisitor::visit_raise(AST_Raise* node) {
    printf("raise");
    if (node->arg0) {
        printf(" ");
        node->arg0->accept(this);
    }
    if (node->arg1) {
        printf(", ");
        node->arg1->accept(this);
    }
    if (node->arg2) {
        printf(", ");
        node->arg2->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_repr(AST_Repr* node) {
    printf("`");
    node->value->accept(this);
    printf("`");
    return true;
}

bool PrintVisitor::visit_return(AST_Return* node) {
    printf("return ");
    return false;
}

bool PrintVisitor::visit_set(AST_Set* node) {
    // An empty set literal is not writeable in Python (it's a dictionary),
    // but we sometimes generate it (ex in set comprehension lowering).
    // Just to make it clear when printing, print empty set literals as "SET{}".
    if (!node->elts.size())
        printf("SET");

    printf("{");

    bool first = true;
    for (auto e : node->elts) {
        if (!first)
            printf(", ");
        first = false;

        e->accept(this);
    }

    printf("}");
    return true;
}

bool PrintVisitor::visit_setcomp(AST_SetComp* node) {
    printf("{");
    node->elt->accept(this);
    for (auto c : node->generators) {
        printf(" ");
        c->accept(this);
    }
    printf("}");
    return true;
}

bool PrintVisitor::visit_slice(AST_Slice* node) {
    printf("<slice>(");
    if (node->lower)
        node->lower->accept(this);
    if (node->upper || node->step)
        putchar(':');
    if (node->upper)
        node->upper->accept(this);
    if (node->step) {
        putchar(':');
        node->step->accept(this);
    }
    printf(")");
    return true;
}

bool PrintVisitor::visit_str(AST_Str* node) {
    if (node->str_type == AST_Str::STR) {
        printf("\"%s\"", node->str_data.c_str());
    } else if (node->str_type == AST_Str::UNICODE) {
        printf("<unicode value>");
    } else {
        RELEASE_ASSERT(0, "%d", node->str_type);
    }
    return false;
}

bool PrintVisitor::visit_subscript(AST_Subscript* node) {
    node->value->accept(this);
    printf("[");
    node->slice->accept(this);
    printf("]");
    return true;
}

bool PrintVisitor::visit_tryexcept(AST_TryExcept* node) {
    printf("try:\n");
    indent += 4;
    for (AST* subnode : node->body) {
        printIndent();
        subnode->accept(this);
        printf("\n");
    }
    indent -= 4;
    for (AST_ExceptHandler* handler : node->handlers) {
        printIndent();
        handler->accept(this);
    }

    if (node->orelse.size()) {
        printIndent();
        printf("else:\n");
        indent += 4;
        for (AST* subnode : node->orelse) {
            printIndent();
            subnode->accept(this);
            printf("\n");
        }
        indent -= 4;
    }
    return true;
}

bool PrintVisitor::visit_tryfinally(AST_TryFinally* node) {
    if (node->body.size() == 1 && node->body[0]->type == AST_TYPE::TryExcept) {
        node->body[0]->accept(this);
        printIndent();
        printf("finally:\n");

        indent += 4;
        for (AST* subnode : node->finalbody) {
            printIndent();
            subnode->accept(this);
            printf("\n");
        }
        indent -= 4;
    } else {
        printf("try:\n");
        indent += 4;
        for (AST* subnode : node->body) {
            printIndent();
            subnode->accept(this);
            printf("\n");
        }
        indent -= 4;

        printIndent();
        printf("finally:\n");
        indent += 4;
        for (AST* subnode : node->finalbody) {
            printIndent();
            subnode->accept(this);
            printf("\n");
        }
        indent -= 4;
    }
    return true;
}

bool PrintVisitor::visit_tuple(AST_Tuple* node) {
    printf("(");
    int n = node->elts.size();
    for (int i = 0; i < n; i++) {
        if (i)
            printf(", ");
        node->elts[i]->accept(this);
    }
    if (n == 1)
        printf(",");
    printf(")");
    return true;
}

bool PrintVisitor::visit_unaryop(AST_UnaryOp* node) {
    switch (node->op_type) {
        case AST_TYPE::Invert:
            printf("~");
            break;
        case AST_TYPE::Not:
            printf("not ");
            break;
        case AST_TYPE::UAdd:
            printf("+");
            break;
        case AST_TYPE::USub:
            printf("-");
            break;
        default:
            RELEASE_ASSERT(0, "%s", getOpName(node->op_type)->c_str());
            break;
    }
    printf("(");
    node->operand->accept(this);
    printf(")");
    return true;
}

bool PrintVisitor::visit_while(AST_While* node) {
    printf("while ");
    node->test->accept(this);
    printf("\n");

    indent += 4;
    for (int i = 0; i < node->body.size(); i++) {
        printIndent();
        node->body[i]->accept(this);
        printf("\n");
    }
    indent -= 4;

    if (node->orelse.size()) {
        printIndent();
        printf("else\n");
        indent += 4;
        for (int i = 0; i < node->orelse.size(); i++) {
            printIndent();
            node->orelse[i]->accept(this);
            printf("\n");
        }
        indent -= 4;
    }
    return true;
}

bool PrintVisitor::visit_with(AST_With* node) {
    printf("with ");
    node->context_expr->accept(this);
    if (node->optional_vars) {
        printf(" as ");
        node->optional_vars->accept(this);
        printf(":\n");
    }

    indent += 4;
    for (int i = 0; i < node->body.size(); i++) {
        if (i > 0)
            printf("\n");
        printIndent();
        node->body[i]->accept(this);
    }
    indent -= 4;

    return true;
}

bool PrintVisitor::visit_yield(AST_Yield* node) {
    printf("yield ");
    if (node->value)
        node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_branch(AST_Branch* node) {
    printf("if ");
    node->test->accept(this);
    printf(" goto %d else goto %d", node->iftrue->idx, node->iffalse->idx);
    return true;
}

bool PrintVisitor::visit_jump(AST_Jump* node) {
    printf("goto %d", node->target->idx);
    return true;
}

bool PrintVisitor::visit_clsattribute(AST_ClsAttribute* node) {
    // printf("getclsattr(");
    // node->value->accept(this);
    // printf(", '%s')", node->attr.c_str());
    node->value->accept(this);
    printf(":%s", node->attr.c_str());
    return true;
}

bool PrintVisitor::visit_makefunction(AST_MakeFunction* node) {
    printf("make_");
    return false;
}

bool PrintVisitor::visit_makeclass(AST_MakeClass* node) {
    printf("make_");
    return false;
}

class FlattenVisitor : public ASTVisitor {
private:
    std::vector<AST*>* output;
    bool expand_scopes;

public:
    FlattenVisitor(std::vector<AST*>* output, bool expand_scopes) : output(output), expand_scopes(expand_scopes) {
        assert(expand_scopes && "not sure if this works properly");
    }

    virtual bool visit_alias(AST_alias* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_arguments(AST_arguments* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_assert(AST_Assert* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_assign(AST_Assign* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_augassign(AST_AugAssign* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_augbinop(AST_AugBinOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_attribute(AST_Attribute* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_binop(AST_BinOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_boolop(AST_BoolOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_break(AST_Break* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_call(AST_Call* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_classdef(AST_ClassDef* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_compare(AST_Compare* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_comprehension(AST_comprehension* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_continue(AST_Continue* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_delete(AST_Delete* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_dict(AST_Dict* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_dictcomp(AST_DictComp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_excepthandler(AST_ExceptHandler* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_exec(AST_Exec* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_expr(AST_Expr* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_extslice(AST_ExtSlice* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_for(AST_For* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_functiondef(AST_FunctionDef* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_generatorexp(AST_GeneratorExp* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_global(AST_Global* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_if(AST_If* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_ifexp(AST_IfExp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_import(AST_Import* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_importfrom(AST_ImportFrom* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_index(AST_Index* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_invoke(AST_Invoke* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_keyword(AST_keyword* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_lambda(AST_Lambda* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_langprimitive(AST_LangPrimitive* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_list(AST_List* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_listcomp(AST_ListComp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_module(AST_Module* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_name(AST_Name* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_num(AST_Num* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_pass(AST_Pass* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_print(AST_Print* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_raise(AST_Raise* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_repr(AST_Repr* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_return(AST_Return* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_set(AST_Set* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_setcomp(AST_SetComp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_slice(AST_Slice* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_str(AST_Str* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_subscript(AST_Subscript* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_tryexcept(AST_TryExcept* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_tryfinally(AST_TryFinally* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_tuple(AST_Tuple* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_unaryop(AST_UnaryOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_while(AST_While* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_with(AST_With* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_yield(AST_Yield* node) {
        output->push_back(node);
        return false;
    }

    virtual bool visit_branch(AST_Branch* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_jump(AST_Jump* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_clsattribute(AST_ClsAttribute* node) {
        output->push_back(node);
        return false;
    }

    virtual bool visit_makeclass(AST_MakeClass* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_makefunction(AST_MakeFunction* node) {
        output->push_back(node);
        return false;
    }
};

void flatten(const std::vector<AST_stmt*>& roots, std::vector<AST*>& output, bool expand_scopes) {
    FlattenVisitor visitor(&output, expand_scopes);

    for (int i = 0; i < roots.size(); i++) {
        roots[i]->accept(&visitor);
    }
}

void flatten(AST_expr* root, std::vector<AST*>& output, bool expand_scopes) {
    FlattenVisitor visitor(&output, expand_scopes);

    root->accept(&visitor);
}
}
