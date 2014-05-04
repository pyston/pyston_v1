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

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <stdint.h>
#include <cassert>

#include "core/ast.h"
#include "core/cfg.h"

#define FUTURE_DIVISION 0

namespace pyston {

std::string getOpSymbol(int op_type) {
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
            return "/";
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
    return getOpSymbol(op_type) + '=';
}

std::string getOpName(int op_type) {
    assert(op_type != AST_TYPE::Is);
    assert(op_type != AST_TYPE::IsNot);

    switch (op_type) {
        case AST_TYPE::Add:
            return "__add__";
        case AST_TYPE::BitAnd:
            return "__and__";
        case AST_TYPE::BitOr:
            return "__or__";
        case AST_TYPE::BitXor:
            return "__xor__";
        case AST_TYPE::Div:
            if (FUTURE_DIVISION)
                return "__truediv__";
            else
                return "__div__";
        case AST_TYPE::Eq:
            return "__eq__";
        case AST_TYPE::FloorDiv:
            return "__floordiv__";
        case AST_TYPE::LShift:
            return "__lshift__";
        case AST_TYPE::Lt:
            return "__lt__";
        case AST_TYPE::LtE:
            return "__le__";
        case AST_TYPE::Gt:
            return "__gt__";
        case AST_TYPE::GtE:
            return "__ge__";
        case AST_TYPE::In:
            return "__contains__";
        case AST_TYPE::Invert:
            return "__invert__";
        case AST_TYPE::Mod:
            return "__mod__";
        case AST_TYPE::Mult:
            return "__mul__";
        case AST_TYPE::Not:
            return "__nonzero__";
        case AST_TYPE::NotEq:
            return "__ne__";
        case AST_TYPE::Pow:
            return "__pow__";
        case AST_TYPE::RShift:
            return "__rshift__";
        case AST_TYPE::Sub:
            return "__sub__";
        case AST_TYPE::UAdd:
            return "__pos__";
        case AST_TYPE::USub:
            return "__neg__";
        default:
            fprintf(stderr, "Unknown op type (" __FILE__ ":" STRINGIFY(__LINE__) "): %d\n", op_type);
            abort();
    }
}

std::string getInplaceOpName(int op_type) {
    std::string normal_name = getOpName(op_type);
    return "__i" + normal_name.substr(2);
}

// Maybe better name is "swapped" -- it's what the runtime will try if the normal op
// name fails, it will switch the order of the lhs/rhs and call the reverse op.
// Calling it "reverse" because that's what I'm assuming the 'r' stands for in ex __radd__
std::string getReverseOpName(int op_type) {
    if (op_type == AST_TYPE::Lt)
        return getOpName(AST_TYPE::GtE);
    if (op_type == AST_TYPE::LtE)
        return getOpName(AST_TYPE::Gt);
    if (op_type == AST_TYPE::Gt)
        return getOpName(AST_TYPE::LtE);
    if (op_type == AST_TYPE::GtE)
        return getOpName(AST_TYPE::Lt);
    if (op_type == AST_TYPE::NotEq)
        return getOpName(AST_TYPE::NotEq);
    if (op_type == AST_TYPE::Eq)
        return getOpName(AST_TYPE::Eq);

    std::string normal_name = getOpName(op_type);
    return "__r" + normal_name.substr(2);
}

template <class T>
static void visitVector(const std::vector<T*> &vec, ASTVisitor *v) {
    for (int i = 0; i < vec.size(); i++) {
        vec[i]->accept(v);
    }
}

void AST_alias::accept(ASTVisitor *v) {
    bool skip = v->visit_alias(this);
    if (skip) return;
}

void AST_arguments::accept(ASTVisitor *v) {
    bool skip = v->visit_arguments(this);
    if (skip) return;

    visitVector(defaults, v);
    visitVector(args, v);
    if (kwarg) kwarg->accept(v);
}

void AST_Assert::accept(ASTVisitor *v) {
    bool skip = v->visit_assert(this);
    if (skip) return;

    test->accept(v);
    if (msg) msg->accept(v);
}

void AST_Assert::accept_stmt(StmtVisitor *v) {
    v->visit_assert(this);
}

void AST_Assign::accept(ASTVisitor *v) {
    bool skip = v->visit_assign(this);
    if (skip) return;

    value->accept(v);
    for (int i = 0; i < targets.size(); i++) {
        // Targets are assigned to left-to-right, so this is valid:
        // x = x.a = object()
        // but this is not:
        // x.a = x = object()
        targets[i]->accept(v);
    }
}

void AST_Assign::accept_stmt(StmtVisitor *v) {
    v->visit_assign(this);
}

void AST_AugAssign::accept(ASTVisitor *v) {
    bool skip = v->visit_augassign(this);
    if (skip) return;

    value->accept(v);
    target->accept(v);
}

void AST_AugAssign::accept_stmt(StmtVisitor *v) {
    v->visit_augassign(this);
}

void AST_AugBinOp::accept(ASTVisitor *v) {
    bool skip = v->visit_augbinop(this);
    if (skip) return;

    left->accept(v);
    right->accept(v);
}

void* AST_AugBinOp::accept_expr(ExprVisitor *v) {
    return v->visit_augbinop(this);
}

void AST_Attribute::accept(ASTVisitor *v) {
    bool skip = v->visit_attribute(this);
    if (skip) return;

    value->accept(v);
}

void* AST_Attribute::accept_expr(ExprVisitor *v) {
    return v->visit_attribute(this);
}

void AST_BinOp::accept(ASTVisitor *v) {
    bool skip = v->visit_binop(this);
    if (skip) return;

    left->accept(v);
    right->accept(v);
}

void* AST_BinOp::accept_expr(ExprVisitor *v) {
    return v->visit_binop(this);
}

void AST_BoolOp::accept(ASTVisitor *v) {
    bool skip = v->visit_boolop(this);
    if (skip) return;

    visitVector(values, v);
}

void* AST_BoolOp::accept_expr(ExprVisitor *v) {
    return v->visit_boolop(this);
}

void AST_Break::accept(ASTVisitor *v) {
    bool skip = v->visit_break(this);
    if (skip) return;
}

void AST_Break::accept_stmt(StmtVisitor *v) {
    v->visit_break(this);
}

void AST_Call::accept(ASTVisitor *v) {
    bool skip = v->visit_call(this);
    if (skip) return;

    func->accept(v);
    visitVector(args, v);
    visitVector(keywords, v);
    if (starargs) starargs->accept(v);
    if (kwargs) kwargs->accept(v);
}

void* AST_Call::accept_expr(ExprVisitor *v) {
    return v->visit_call(this);
}

void AST_Compare::accept(ASTVisitor *v) {
    bool skip = v->visit_compare(this);
    if (skip) return;

    left->accept(v);
    visitVector(comparators, v);
}

void* AST_Compare::accept_expr(ExprVisitor *v) {
    return v->visit_compare(this);
}

void AST_comprehension::accept(ASTVisitor *v) {
    bool skip = v->visit_comprehension(this);
    if (skip) return;

    target->accept(v);
    iter->accept(v);
    for (auto if_ : ifs) {
        if_->accept(v);
    }
}

void AST_ClassDef::accept(ASTVisitor *v) {
    bool skip = v->visit_classdef(this);
    if (skip) return;

    visitVector(this->bases, v);
    visitVector(this->decorator_list, v);
    visitVector(this->body, v);
}

void AST_ClassDef::accept_stmt(StmtVisitor *v) {
    v->visit_classdef(this);
}

void AST_Continue::accept(ASTVisitor *v) {
    bool skip = v->visit_continue(this);
    if (skip) return;
}

void AST_Continue::accept_stmt(StmtVisitor *v) {
    v->visit_continue(this);
}

void AST_Delete::accept(ASTVisitor *v){
	bool skip = v->visit_delete(this);
	if (skip) return;

	visitVector(this->targets, v);
}

void AST_Delete::accept_stmt(StmtVisitor *v){
	v->visit_delete(this);
}

void AST_Dict::accept(ASTVisitor *v) {
    bool skip = v->visit_dict(this);
    if (skip) return;

    for (int i = 0; i < keys.size(); i++) {
        keys[i]->accept(v);
        values[i]->accept(v);
    }
}

void* AST_Dict::accept_expr(ExprVisitor *v) {
    return v->visit_dict(this);
}

void AST_Expr::accept(ASTVisitor *v) {
    bool skip = v->visit_expr(this);
    if (skip) return;

    value->accept(v);
}

void AST_Expr::accept_stmt(StmtVisitor *v) {
    v->visit_expr(this);
}

void AST_For::accept(ASTVisitor *v) {
    bool skip = v->visit_for(this);
    if (skip) return;

    iter->accept(v);
    target->accept(v);
    visitVector(body, v);
    visitVector(orelse, v);
}

void AST_For::accept_stmt(StmtVisitor *v) {
    v->visit_for(this);
}

void AST_FunctionDef::accept(ASTVisitor *v) {
    bool skip = v->visit_functiondef(this);
    if (skip) return;

    visitVector(decorator_list, v);
    args->accept(v);
    visitVector(body, v);
}

void AST_FunctionDef::accept_stmt(StmtVisitor *v) {
    v->visit_functiondef(this);
}

void AST_Global::accept(ASTVisitor *v) {
    bool skip = v->visit_global(this);
    if (skip) return;
}

void AST_Global::accept_stmt(StmtVisitor *v) {
    v->visit_global(this);
}

void AST_If::accept(ASTVisitor *v) {
    bool skip = v->visit_if(this);
    if (skip) return;

    test->accept(v);
    visitVector(body, v);
    visitVector(orelse, v);
}

void AST_If::accept_stmt(StmtVisitor *v) {
    v->visit_if(this);
}

void AST_IfExp::accept(ASTVisitor *v) {
    bool skip = v->visit_ifexp(this);
    if (skip) return;

    this->test->accept(v);
    this->body->accept(v);
    this->orelse->accept(v);
}

void* AST_IfExp::accept_expr(ExprVisitor *v) {
    return v->visit_ifexp(this);
}

void AST_Import::accept(ASTVisitor *v) {
    bool skip = v->visit_import(this);
    if (skip) return;

    visitVector(names, v);
}

void AST_Import::accept_stmt(StmtVisitor *v) {
    v->visit_import(this);
}

void AST_ImportFrom::accept(ASTVisitor *v) {
    bool skip = v->visit_importfrom(this);
    if (skip) return;

    visitVector(names, v);
}

void AST_ImportFrom::accept_stmt(StmtVisitor *v) {
    v->visit_importfrom(this);
}

void AST_Index::accept(ASTVisitor *v) {
    bool skip = v->visit_index(this);
    if (skip) return;

    this->value->accept(v);
}

void* AST_Index::accept_expr(ExprVisitor *v) {
    return v->visit_index(this);
}

void AST_keyword::accept(ASTVisitor *v) {
    bool skip = v->visit_keyword(this);
    if (skip) return;

    value->accept(v);
}

void AST_List::accept(ASTVisitor *v) {
    bool skip = v->visit_list(this);
    if (skip) return;

    visitVector(elts, v);
}

void* AST_List::accept_expr(ExprVisitor *v) {
    return v->visit_list(this);
}

void AST_ListComp::accept(ASTVisitor *v) {
    bool skip = v->visit_listcomp(this);
    if (skip) return;

    for (auto c : generators) {
        c->accept(v);
    }

    elt->accept(v);
}

void* AST_ListComp::accept_expr(ExprVisitor *v) {
    return v->visit_listcomp(this);
}

void AST_Module::accept(ASTVisitor *v) {
    bool skip = v->visit_module(this);
    if (skip) return;

    visitVector(body, v);
}

void AST_Name::accept(ASTVisitor *v) {
    bool skip = v->visit_name(this);
}

void* AST_Name::accept_expr(ExprVisitor *v) {
    return v->visit_name(this);
}

void AST_Num::accept(ASTVisitor *v) {
    bool skip = v->visit_num(this);
}

void* AST_Num::accept_expr(ExprVisitor *v) {
    return v->visit_num(this);
}

void AST_Pass::accept(ASTVisitor *v) {
    bool skip = v->visit_pass(this);
}

void AST_Pass::accept_stmt(StmtVisitor *v) {
    v->visit_pass(this);
}

void AST_Print::accept(ASTVisitor *v) {
    bool skip = v->visit_print(this);
    if (skip) return;

    if (dest) dest->accept(v);
    visitVector(values, v);
}

void AST_Print::accept_stmt(StmtVisitor *v) {
    v->visit_print(this);
}

void AST_Return::accept(ASTVisitor *v) {
    bool skip = v->visit_return(this);
    if (skip) return;

    if (value) value->accept(v);
}

void AST_Return::accept_stmt(StmtVisitor *v) {
    v->visit_return(this);
}

void AST_Slice::accept(ASTVisitor *v) {
    bool skip = v->visit_slice(this);
    if (skip) return;

    if (lower) lower->accept(v);
    if (upper) upper->accept(v);
    if (step) step->accept(v);
}

void* AST_Slice::accept_expr(ExprVisitor *v) {
    return v->visit_slice(this);
}

void AST_Str::accept(ASTVisitor *v) {
    bool skip = v->visit_str(this);
    if (skip) return;
}

void* AST_Str::accept_expr(ExprVisitor *v) {
    return v->visit_str(this);
}

void AST_Subscript::accept(ASTVisitor *v) {
    bool skip = v->visit_subscript(this);
    if (skip) return;

    this->value->accept(v);
    this->slice->accept(v);
}

void* AST_Subscript::accept_expr(ExprVisitor *v) {
    return v->visit_subscript(this);
}

void AST_Tuple::accept(ASTVisitor *v) {
    bool skip = v->visit_tuple(this);
    if (skip) return;

    visitVector(elts, v);
}

void* AST_Tuple::accept_expr(ExprVisitor *v) {
    return v->visit_tuple(this);
}

void AST_UnaryOp::accept(ASTVisitor *v) {
    bool skip = v->visit_unaryop(this);
    if (skip) return;

    operand->accept(v);
}

void* AST_UnaryOp::accept_expr(ExprVisitor *v) {
    return v->visit_unaryop(this);
}

void AST_While::accept(ASTVisitor *v) {
    bool skip = v->visit_while(this);
    if (skip) return;

    test->accept(v);
    visitVector(body, v);
    visitVector(orelse, v);
}

void AST_While::accept_stmt(StmtVisitor *v) {
    v->visit_while(this);
}

void AST_With::accept(ASTVisitor *v) {
    bool skip = v->visit_with(this);
    if (skip) return;

    context_expr->accept(v);
    if (optional_vars) optional_vars->accept(v);
    visitVector(body, v);
}

void AST_With::accept_stmt(StmtVisitor *v) {
    v->visit_with(this);
}


void AST_Branch::accept(ASTVisitor *v) {
    bool skip = v->visit_branch(this);
    if (skip) return;

    test->accept(v);
}

void AST_Branch::accept_stmt(StmtVisitor *v) {
    v->visit_branch(this);
}


void AST_Jump::accept(ASTVisitor *v) {
    bool skip = v->visit_jump(this);
    if (skip) return;
}

void AST_Jump::accept_stmt(StmtVisitor *v) {
    v->visit_jump(this);
}

void AST_ClsAttribute::accept(ASTVisitor *v) {
    bool skip = v->visit_clsattribute(this);
    if (skip) return;

    value->accept(v);
}

void* AST_ClsAttribute::accept_expr(ExprVisitor *v) {
    return v->visit_clsattribute(this);
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

bool PrintVisitor::visit_alias(AST_alias *node) {
    printf("%s", node->name.c_str());
    if (node->asname.size())
        printf(" as %s", node->asname.c_str());
    return true;
}

bool PrintVisitor::visit_arguments(AST_arguments *node) {
    int nargs = node->args.size();
    int ndefault = node->defaults.size();
    for (int i = 0; i < nargs; i++) {
        if (i > 0) printf(", ");

        node->args[i]->accept(this);
        if (i >= nargs - ndefault) {
            printf("=");
            node->defaults[i - (nargs - ndefault)]->accept(this);
        }
    }
    return true;
}

bool PrintVisitor::visit_assert(AST_Assert *node) {
    printf("assert ");
    node->test->accept(this);
    if (node->msg) {
        printf(", ");
        node->msg->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_assign(AST_Assign *node) {
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

bool PrintVisitor::visit_augassign(AST_AugAssign *node) {
    node->target->accept(this);
    printOp(node->op_type);
    putchar('=');
    node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_augbinop(AST_AugBinOp *node) {
    node->left->accept(this);
    printf("=");
    printOp(node->op_type);
    node->right->accept(this);
    return true;
}

bool PrintVisitor::visit_attribute(AST_Attribute *node) {
    node->value->accept(this);
    putchar('.');
    printf("%s", node->attr.c_str());
    return true;
}

bool PrintVisitor::visit_binop(AST_BinOp *node) {
    node->left->accept(this);
    printOp(node->op_type);
    node->right->accept(this);
    return true;
}

bool PrintVisitor::visit_boolop(AST_BoolOp *node) {
    for (int i = 0; i < node->values.size(); i++) {
        node->values[i]->accept(this);

        if (i == node->values.size() - 1) continue;
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

bool PrintVisitor::visit_break(AST_Break *node) {
    printf("break");
    return true;
}

bool PrintVisitor::visit_call(AST_Call *node) {
    node->func->accept(this);
    printf("(");

    bool prevarg = false;
    for (int i = 0; i < node->args.size(); i++) {
        if (prevarg) printf(", ");
        node->args[i]->accept(this);
        prevarg = true;
    }
    for (int i = 0; i < node->keywords.size(); i++) {
        if (prevarg) printf(", ");
        node->keywords[i]->accept(this);
        prevarg = true;
    }
    if (node->starargs) {
        if (prevarg) printf(", ");
        node->starargs->accept(this);
        prevarg = true;
    }
    if (node->kwargs) {
        if (prevarg) printf(", ");
        node->kwargs->accept(this);
        prevarg = true;
    }
    printf(")");
    return true;
}

bool PrintVisitor::visit_compare(AST_Compare *node) {
    node->left->accept(this);

    for (int i = 0; i < node->ops.size(); i++) {
        std::string symbol = getOpSymbol(node->ops[i]);
        printf(" %s ", symbol.c_str());

        node->comparators[i]->accept(this);
    }

    return true;
}

bool PrintVisitor::visit_comprehension(AST_comprehension *node) {
    printf("for ");
    node->target->accept(this);
    printf(" in ");
    node->iter->accept(this);

    for (AST_expr *i : node->ifs) {
        printf(" if ");
        i->accept(this);
    }

    return true;
}

bool PrintVisitor::visit_classdef(AST_ClassDef *node) {
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

bool PrintVisitor::visit_continue(AST_Continue *node) {
    printf("continue");
    return true;
}

bool PrintVisitor::visit_delete(AST_Delete *node) {
	printf("del ");
	for (int i = 0; i < node->targets.size(); i++) {
		if (i > 0) printf(", ");
		node->targets[i]->accept(this);
	}
	return true;
}
bool PrintVisitor::visit_dict(AST_Dict *node) {
    printf("{");
    for (int i = 0; i < node->keys.size(); i++) {
        if (i > 0) printf(", ");
        node->keys[i]->accept(this);
        printf(":");
        node->values[i]->accept(this);
    }
    printf("}");
    return true;
}

bool PrintVisitor::visit_expr(AST_Expr *node) {
    return false;
}

bool PrintVisitor::visit_for(AST_For *node) {
    printf("<for loop>\n");
    return true;
}

bool PrintVisitor::visit_functiondef(AST_FunctionDef *node) {
    assert(node->decorator_list.size() == 0);
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

bool PrintVisitor::visit_global(AST_Global *node) {
    printf("global ");
    for (int i = 0; i < node->names.size(); i++) {
        if (i > 0) printf(", ");
        printf("%s", node->names[i].c_str());
    }
    return true;
}

bool PrintVisitor::visit_if(AST_If *node) {
    printf("if ");
    node->test->accept(this);
    printf(":\n");

    indent += 4;
    for(int i = 0; i < node->body.size(); i++) {
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
        for(int i = 0; i < node->orelse.size(); i++) {
            if (i) printf("\n");
            printIndent();
            node->orelse[i]->accept(this);
        }
        if (!elif)
            indent -= 4;
    }
    return true;
}

bool PrintVisitor::visit_ifexp(AST_IfExp *node) {
    node->body->accept(this);
    printf(" if ");
    node->test->accept(this);
    printf(" else ");
    node->orelse->accept(this);
    return true;
}

bool PrintVisitor::visit_import(AST_Import *node) {
    printf("import ");
    for (int i = 0; i < node->names.size(); i++) {
        if (i > 0) printf(", ");
        node->names[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_importfrom(AST_ImportFrom *node) {
    printf("from %s import ", node->module.c_str());
    for (int i = 0; i < node->names.size(); i++) {
        if (i > 0) printf(", ");
        node->names[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_index(AST_Index *node) {
    return false;
}

bool PrintVisitor::visit_list(AST_List *node) {
    printf("[");
    for (int i = 0, n = node->elts.size(); i < n; ++i) {
        if (i > 0)
            printf(", ");
        node->elts[i]->accept(this);
    }
    printf("]");
    return true;
}

bool PrintVisitor::visit_listcomp(AST_ListComp *node) {
    printf("[");
    node->elt->accept(this);
    for (auto c : node->generators) {
        printf(" ");
        c->accept(this);
    }
    printf("]");
    return true;
}

bool PrintVisitor::visit_keyword(AST_keyword *node) {
    printf("%s=", node->arg.c_str());
    node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_module(AST_Module *node) {
    //printf("<module>\n");
    for (int i = 0; i < node->body.size(); i++) {
        node->body[i]->accept(this);
        printf("\n");
    }
    return true;
}

bool PrintVisitor::visit_name(AST_Name *node) {
    printf("%s", node->id.c_str());
    //printf("%s(%d)", node->id.c_str(), node->ctx_type);
    return false;
}

bool PrintVisitor::visit_num(AST_Num *node) {
    if (node->num_type == AST_Num::INT) {
        printf("%ld", node->n_int);
    } else if (node->num_type == AST_Num::FLOAT) {
        printf("%f", node->n_float);
    } else {
        RELEASE_ASSERT(0, "");
    }
    return false;
}

bool PrintVisitor::visit_pass(AST_Pass *node) {
    printf("pass");
    return true;
}

bool PrintVisitor::visit_print(AST_Print *node) {
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

bool PrintVisitor::visit_return(AST_Return *node) {
    printf("return ");
    return false;
}

bool PrintVisitor::visit_slice(AST_Slice *node) {
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
    return true;
}

bool PrintVisitor::visit_str(AST_Str *node) {
    printf("\"%s\"", node->s.c_str());
    return false;
}

bool PrintVisitor::visit_subscript(AST_Subscript *node) {
    node->value->accept(this);
    printf("[");
    node->slice->accept(this);
    printf("]");
    return true;
}

bool PrintVisitor::visit_tuple(AST_Tuple *node) {
    printf("(");
    int n = node->elts.size();
    for (int i = 0; i < n; i++) {
        if (i) printf(", ");
        node->elts[i]->accept(this);
    }
    if (n == 1)
        printf(",");
    printf(")");
    return true;
}

bool PrintVisitor::visit_unaryop(AST_UnaryOp *node) {
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
            RELEASE_ASSERT(0, "%s", getOpName(node->op_type).c_str());
            break;
    }
    node->operand->accept(this);
    return true;
}

bool PrintVisitor::visit_while(AST_While *node) {
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

bool PrintVisitor::visit_with(AST_With *node) {
    printf("with ");
    node->context_expr->accept(this);
    if (node->optional_vars) {
        printf(" as ");
        node->optional_vars->accept(this);
        printf(":\n");
    }

    indent += 4;
    for (int i = 0; i < node->body.size(); i++) {
        if (i > 0) printf("\n");
        printIndent();
        node->body[i]->accept(this);
    }
    indent -= 4;

    return true;
}

bool PrintVisitor::visit_branch(AST_Branch *node) {
    printf("if ");
    node->test->accept(this);
    printf(" goto %d else goto %d", node->iftrue->idx, node->iffalse->idx);
    return true;
}

bool PrintVisitor::visit_jump(AST_Jump *node) {
    printf("goto %d", node->target->idx);
    return true;
}

bool PrintVisitor::visit_clsattribute(AST_ClsAttribute *node) {
    //printf("getclsattr(");
    //node->value->accept(this);
    //printf(", '%s')", node->attr.c_str());
    node->value->accept(this);
    printf(":%s", node->attr.c_str());
    return true;
}

class FlattenVisitor : public ASTVisitor {
    private:
        std::vector<AST*> *output;
        bool expand_scopes;
    public:
        FlattenVisitor(std::vector<AST*> *output, bool expand_scopes) : output(output), expand_scopes(expand_scopes) {
        }

        virtual bool visit_alias(AST_alias *node) { output->push_back(node); return false; }
        virtual bool visit_arguments(AST_arguments *node) { output->push_back(node); return false; }
        virtual bool visit_assert(AST_Assert *node) { output->push_back(node); return false; }
        virtual bool visit_assign(AST_Assign *node) { output->push_back(node); return false; }
        virtual bool visit_augassign(AST_AugAssign *node) { output->push_back(node); return false; }
        virtual bool visit_augbinop(AST_AugBinOp *node) { output->push_back(node); return false; }
        virtual bool visit_attribute(AST_Attribute *node) { output->push_back(node); return false; }
        virtual bool visit_binop(AST_BinOp *node) { output->push_back(node); return false; }
        virtual bool visit_boolop(AST_BoolOp *node) { output->push_back(node); return false; }
        virtual bool visit_break(AST_Break *node) { output->push_back(node); return false; }
        virtual bool visit_call(AST_Call *node) { output->push_back(node); return false; }
        virtual bool visit_classdef(AST_ClassDef *node) { output->push_back(node); return !expand_scopes; }
        virtual bool visit_compare(AST_Compare *node) { output->push_back(node); return false; }
        virtual bool visit_comprehension(AST_comprehension *node) { output->push_back(node); return false; }
        virtual bool visit_continue(AST_Continue *node) { output->push_back(node); return false; }
    virtual bool visit_delete(AST_Delete *node){ output->push_back(node); return false; }
        virtual bool visit_dict(AST_Dict *node) { output->push_back(node); return false; }
        virtual bool visit_expr(AST_Expr *node) { output->push_back(node); return false; }
        virtual bool visit_for(AST_For *node) { output->push_back(node); return !expand_scopes; }
        virtual bool visit_functiondef(AST_FunctionDef *node) { output->push_back(node); return !expand_scopes; }
        virtual bool visit_global(AST_Global *node) { output->push_back(node); return false; }
        virtual bool visit_if(AST_If *node) { output->push_back(node); return false; }
        virtual bool visit_ifexp(AST_IfExp *node) { output->push_back(node); return false; }
        virtual bool visit_import(AST_Import *node) { output->push_back(node); return false; }
        virtual bool visit_importfrom(AST_ImportFrom *node) { output->push_back(node); return false; }
        virtual bool visit_index(AST_Index *node) { output->push_back(node); return false; }
        virtual bool visit_keyword(AST_keyword *node) { output->push_back(node); return false; }
        virtual bool visit_list(AST_List *node) { output->push_back(node); return false; }
        virtual bool visit_listcomp(AST_ListComp *node) { output->push_back(node); return false; }
        virtual bool visit_module(AST_Module *node) { output->push_back(node); return !expand_scopes; }
        virtual bool visit_name(AST_Name *node) { output->push_back(node); return false; }
        virtual bool visit_num(AST_Num *node) { output->push_back(node); return false; }
        virtual bool visit_pass(AST_Pass *node) { output->push_back(node); return false; }
        virtual bool visit_print(AST_Print *node) { output->push_back(node); return false; }
        virtual bool visit_return(AST_Return *node) { output->push_back(node); return false; }
        virtual bool visit_slice(AST_Slice *node) { output->push_back(node); return false; }
        virtual bool visit_str(AST_Str *node) { output->push_back(node); return false; }
        virtual bool visit_subscript(AST_Subscript *node) { output->push_back(node); return false; }
        virtual bool visit_tuple(AST_Tuple *node) { output->push_back(node); return false; }
        virtual bool visit_unaryop(AST_UnaryOp *node) { output->push_back(node); return false; }
        virtual bool visit_while(AST_While *node) { output->push_back(node); return false; }
        virtual bool visit_with(AST_With *node) { output->push_back(node); return false; }

        virtual bool visit_branch(AST_Branch *node) { output->push_back(node); return false; }
        virtual bool visit_jump(AST_Jump *node) { output->push_back(node); return false; }
        virtual bool visit_clsattribute(AST_ClsAttribute *node) { output->push_back(node); return false; }
};

void flatten(const std::vector<AST_stmt*> &roots, std::vector<AST*> &output, bool expand_scopes) {
    FlattenVisitor visitor(&output, expand_scopes);

    for (int i = 0; i < roots.size(); i++) {
        roots[i]->accept(&visitor);
    }
}

void flatten(AST_expr* root, std::vector<AST*> &output, bool expand_scopes) {
    FlattenVisitor visitor(&output, expand_scopes);

    root->accept(&visitor);
}

}
