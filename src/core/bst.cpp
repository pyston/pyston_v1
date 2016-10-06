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

#include "core/bst.h"

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
int BST_stmt::next_lineno = 100000;

BST_stmt::BST_stmt(BST_TYPE::BST_TYPE type) : type(type), lineno(++next_lineno) {
    // if (lineno == 100644)
    // raise(SIGTRAP);
}

#endif

template <class T> static void visitVector(const std::vector<T*>& vec, BSTVisitor* v) {
    for (int i = 0; i < vec.size(); i++) {
        vec[i]->accept(v);
    }
}

void BST_stmt::accept(BSTVisitor* v) {
    switch (type()) {
#define DISPATCH_ACCEPT(x, y)                                                                                          \
    case BST_TYPE::x:                                                                                                  \
        return bst_cast<BST_##x>(this)->accept(v);
        FOREACH_TYPE(DISPATCH_ACCEPT)
    };
}

void BST_stmt::accept_stmt(StmtVisitor* v) {
    switch (type()) {
#define DISPATCH_ACCEPT_STMT(x, y)                                                                                     \
    case BST_TYPE::x:                                                                                                  \
        return bst_cast<BST_##x>(this)->accept_stmt(v);
        FOREACH_TYPE(DISPATCH_ACCEPT_STMT)
    };
}


void BST_Assert::accept(BSTVisitor* v) {
    bool skip = v->visit_assert(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_msg);
}

void BST_Assert::accept_stmt(StmtVisitor* v) {
    v->visit_assert(this);
}

void BST_CopyVReg::accept(BSTVisitor* v) {
    bool skip = v->visit_copyvreg(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_src);
    v->visit_vreg(&vreg_dst, true);
}

void BST_CopyVReg::accept_stmt(StmtVisitor* v) {
    v->visit_copyvreg(this);
}

void BST_AugBinOp::accept(BSTVisitor* v) {
    bool skip = v->visit_augbinop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_right);
    v->visit_vreg(&vreg_dst, true);
}

void BST_AugBinOp::accept_stmt(StmtVisitor* v) {
    return v->visit_augbinop(this);
}

void BST_BinOp::accept(BSTVisitor* v) {
    bool skip = v->visit_binop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_right);
    v->visit_vreg(&vreg_dst, true);
}

void BST_BinOp::accept_stmt(StmtVisitor* v) {
    return v->visit_binop(this);
}

void BST_CallFunc::accept(BSTVisitor* v) {
    bool skip = v->visit_callfunc(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_func);
    for (int i = 0; i < num_args + num_keywords; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_starargs);
    v->visit_vreg(&vreg_kwargs);
    v->visit_vreg(&vreg_dst, true);
}

void BST_CallFunc::accept_stmt(StmtVisitor* v) {
    return v->visit_callfunc(this);
}

void BST_CallAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_callattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    for (int i = 0; i < num_args + num_keywords; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_starargs);
    v->visit_vreg(&vreg_kwargs);
    v->visit_vreg(&vreg_dst, true);
}

void BST_CallAttr::accept_stmt(StmtVisitor* v) {
    return v->visit_callattr(this);
}

void BST_CallClsAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_callclsattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    for (int i = 0; i < num_args + num_keywords; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_starargs);
    v->visit_vreg(&vreg_kwargs);
    v->visit_vreg(&vreg_dst, true);
}

void BST_CallClsAttr::accept_stmt(StmtVisitor* v) {
    return v->visit_callclsattr(this);
}

void BST_Compare::accept(BSTVisitor* v) {
    bool skip = v->visit_compare(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_comparator);
    v->visit_vreg(&vreg_dst, true);
}

void BST_Compare::accept_stmt(StmtVisitor* v) {
    return v->visit_compare(this);
}

void BST_ClassDef::accept(BSTVisitor* v) {
    bool skip = v->visit_classdef(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_bases_tuple);
    for (int i = 0; i < num_decorator; ++i) {
        v->visit_vreg(&decorator[i]);
    }

    // we dont't visit the body
}

void BST_ClassDef::accept_stmt(StmtVisitor* v) {
    v->visit_classdef(this);
}

void BST_DeleteAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_deleteattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
}

void BST_DeleteAttr::accept_stmt(StmtVisitor* v) {
    v->visit_deleteattr(this);
}

void BST_DeleteSub::accept(BSTVisitor* v) {
    bool skip = v->visit_deletesub(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_slice);
}

void BST_DeleteSub::accept_stmt(StmtVisitor* v) {
    v->visit_deletesub(this);
}

void BST_DeleteSubSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_deletesubslice(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
}

void BST_DeleteSubSlice::accept_stmt(StmtVisitor* v) {
    v->visit_deletesubslice(this);
}


void BST_DeleteName::accept(BSTVisitor* v) {
    bool skip = v->visit_deletename(this);
    if (skip)
        return;
    v->visit_vreg(&vreg);
}

void BST_DeleteName::accept_stmt(StmtVisitor* v) {
    v->visit_deletename(this);
}

void BST_Dict::accept(BSTVisitor* v) {
    bool skip = v->visit_dict(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Dict::accept_stmt(StmtVisitor* v) {
    return v->visit_dict(this);
}

void BST_Exec::accept(BSTVisitor* v) {
    bool skip = v->visit_exec(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_body);
    v->visit_vreg(&vreg_globals);
    v->visit_vreg(&vreg_locals);
}

void BST_Exec::accept_stmt(StmtVisitor* v) {
    v->visit_exec(this);
}

void BST_FunctionDef::accept(BSTVisitor* v) {
    bool skip = v->visit_functiondef(this);
    if (skip)
        return;

    for (int i = 0; i < num_decorator + num_defaults; ++i) {
        v->visit_vreg(&elts[i]);
    }
    // we dont't visit the body
}

void BST_FunctionDef::accept_stmt(StmtVisitor* v) {
    v->visit_functiondef(this);
}

void BST_Landingpad::accept(BSTVisitor* v) {
    bool skip = v->visit_landingpad(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Landingpad::accept_stmt(StmtVisitor* v) {
    return v->visit_landingpad(this);
}

void BST_Locals::accept(BSTVisitor* v) {
    bool skip = v->visit_locals(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Locals::accept_stmt(StmtVisitor* v) {
    return v->visit_locals(this);
}

void BST_GetIter::accept(BSTVisitor* v) {
    bool skip = v->visit_getiter(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_dst, true);
}

void BST_GetIter::accept_stmt(StmtVisitor* v) {
    return v->visit_getiter(this);
}

void BST_ImportFrom::accept(BSTVisitor* v) {
    bool skip = v->visit_importfrom(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_module);
    v->visit_vreg(&vreg_name);
    v->visit_vreg(&vreg_dst, true);
}

void BST_ImportFrom::accept_stmt(StmtVisitor* v) {
    return v->visit_importfrom(this);
}

void BST_ImportName::accept(BSTVisitor* v) {
    bool skip = v->visit_importname(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_from);
    v->visit_vreg(&vreg_name);
    v->visit_vreg(&vreg_dst, true);
}

void BST_ImportName::accept_stmt(StmtVisitor* v) {
    return v->visit_importname(this);
}

void BST_ImportStar::accept(BSTVisitor* v) {
    bool skip = v->visit_importstar(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_name);
    v->visit_vreg(&vreg_dst, true);
}

void BST_ImportStar::accept_stmt(StmtVisitor* v) {
    return v->visit_importstar(this);
}

void BST_Nonzero::accept(BSTVisitor* v) {
    bool skip = v->visit_nonzero(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_dst, true);
}

void BST_Nonzero::accept_stmt(StmtVisitor* v) {
    return v->visit_nonzero(this);
}

void BST_CheckExcMatch::accept(BSTVisitor* v) {
    bool skip = v->visit_checkexcmatch(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_cls);
    v->visit_vreg(&vreg_dst, true);
}

void BST_CheckExcMatch::accept_stmt(StmtVisitor* v) {
    return v->visit_checkexcmatch(this);
}

void BST_SetExcInfo::accept(BSTVisitor* v) {
    bool skip = v->visit_setexcinfo(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_type);
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_traceback);
}

void BST_SetExcInfo::accept_stmt(StmtVisitor* v) {
    return v->visit_setexcinfo(this);
}

void BST_UncacheExcInfo::accept(BSTVisitor* v) {
    bool skip = v->visit_uncacheexcinfo(this);
    if (skip)
        return;
}

void BST_UncacheExcInfo::accept_stmt(StmtVisitor* v) {
    return v->visit_uncacheexcinfo(this);
}

void BST_HasNext::accept(BSTVisitor* v) {
    bool skip = v->visit_hasnext(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_dst, true);
}

void BST_HasNext::accept_stmt(StmtVisitor* v) {
    return v->visit_hasnext(this);
}

void BST_PrintExpr::accept(BSTVisitor* v) {
    bool skip = v->visit_printexpr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
}

void BST_PrintExpr::accept_stmt(StmtVisitor* v) {
    return v->visit_printexpr(this);
}

void BST_List::accept(BSTVisitor* v) {
    bool skip = v->visit_list(this);
    if (skip)
        return;

    for (int i = 0; i < num_elts; ++i)
        v->visit_vreg(&elts[i]);
    v->visit_vreg(&vreg_dst, true);
}

void BST_List::accept_stmt(StmtVisitor* v) {
    return v->visit_list(this);
}

void BST_LoadName::accept(BSTVisitor* v) {
    bool skip = v->visit_loadname(this);
    if (skip)
        return;

    if (lookup_type == ScopeInfo::VarScopeType::FAST || lookup_type == ScopeInfo::VarScopeType::CLOSURE)
        v->visit_vreg(&vreg);
    v->visit_vreg(&vreg_dst, true);
}

void BST_LoadName::accept_stmt(StmtVisitor* v) {
    v->visit_loadname(this);
}

void BST_LoadAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_loadattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_dst, true);
}

void BST_LoadAttr::accept_stmt(StmtVisitor* v) {
    v->visit_loadattr(this);
}

void BST_LoadSub::accept(BSTVisitor* v) {
    bool skip = v->visit_loadsub(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_slice);
    v->visit_vreg(&vreg_dst, true);
}

void BST_LoadSub::accept_stmt(StmtVisitor* v) {
    v->visit_loadsub(this);
}

void BST_LoadSubSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_loadsubslice(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
    v->visit_vreg(&vreg_dst, true);
}

void BST_LoadSubSlice::accept_stmt(StmtVisitor* v) {
    v->visit_loadsubslice(this);
}

void BST_StoreName::accept(BSTVisitor* v) {
    bool skip = v->visit_storename(this);
    if (skip)
        return;

    if (lookup_type == ScopeInfo::VarScopeType::FAST || lookup_type == ScopeInfo::VarScopeType::CLOSURE)
        v->visit_vreg(&vreg, true);
    v->visit_vreg(&vreg_value);
}

void BST_StoreName::accept_stmt(StmtVisitor* v) {
    v->visit_storename(this);
}

void BST_StoreAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_storeattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_target);
}

void BST_StoreAttr::accept_stmt(StmtVisitor* v) {
    v->visit_storeattr(this);
}

void BST_StoreSub::accept(BSTVisitor* v) {
    bool skip = v->visit_storesub(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_target);
    v->visit_vreg(&vreg_slice);
    v->visit_vreg(&vreg_value);
}

void BST_StoreSub::accept_stmt(StmtVisitor* v) {
    v->visit_storesub(this);
}

void BST_StoreSubSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_storesubslice(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_target);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
    v->visit_vreg(&vreg_value);
}

void BST_StoreSubSlice::accept_stmt(StmtVisitor* v) {
    v->visit_storesubslice(this);
}

void BST_Print::accept(BSTVisitor* v) {
    bool skip = v->visit_print(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dest);
    v->visit_vreg(&vreg_value);
}

void BST_Print::accept_stmt(StmtVisitor* v) {
    v->visit_print(this);
}

void BST_Raise::accept(BSTVisitor* v) {
    bool skip = v->visit_raise(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_arg0);
    v->visit_vreg(&vreg_arg1);
    v->visit_vreg(&vreg_arg2);
}

void BST_Raise::accept_stmt(StmtVisitor* v) {
    v->visit_raise(this);
}

void BST_Repr::accept(BSTVisitor* v) {
    bool skip = v->visit_repr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_dst, true);
}

void BST_Repr::accept_stmt(StmtVisitor* v) {
    return v->visit_repr(this);
}

void BST_Return::accept(BSTVisitor* v) {
    bool skip = v->visit_return(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
}

void BST_Return::accept_stmt(StmtVisitor* v) {
    v->visit_return(this);
}

void BST_Set::accept(BSTVisitor* v) {
    bool skip = v->visit_set(this);
    if (skip)
        return;

    for (int i = 0; i < num_elts; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_dst, true);
}

void BST_Set::accept_stmt(StmtVisitor* v) {
    return v->visit_set(this);
}

void BST_Tuple::accept(BSTVisitor* v) {
    bool skip = v->visit_tuple(this);
    if (skip)
        return;

    for (int i = 0; i < num_elts; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_dst, true);
}

void BST_Tuple::accept_stmt(StmtVisitor* v) {
    return v->visit_tuple(this);
}

void BST_UnaryOp::accept(BSTVisitor* v) {
    bool skip = v->visit_unaryop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_operand);
    v->visit_vreg(&vreg_dst, true);
}

void BST_UnaryOp::accept_stmt(StmtVisitor* v) {
    return v->visit_unaryop(this);
}

void BST_UnpackIntoArray::accept(BSTVisitor* v) {
    bool skip = v->visit_unpackintoarray(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_src);
    for (int i = 0; i < num_elts; ++i) {
        v->visit_vreg(&vreg_dst[i], true);
    }
}

void BST_UnpackIntoArray::accept_stmt(StmtVisitor* v) {
    return v->visit_unpackintoarray(this);
}

void BST_Yield::accept(BSTVisitor* v) {
    bool skip = v->visit_yield(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_dst, true);
}

void BST_Yield::accept_stmt(StmtVisitor* v) {
    return v->visit_yield(this);
}

void BST_Branch::accept(BSTVisitor* v) {
    bool skip = v->visit_branch(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_test);
}

void BST_Branch::accept_stmt(StmtVisitor* v) {
    v->visit_branch(this);
}

void BST_Jump::accept(BSTVisitor* v) {
    bool skip = v->visit_jump(this);
    if (skip)
        return;
}

void BST_Jump::accept_stmt(StmtVisitor* v) {
    v->visit_jump(this);
}

void BST_MakeFunction::accept(BSTVisitor* v) {
    bool skip = v->visit_makefunction(this);
    if (skip)
        return;

    bst_cast<BST_FunctionDef>(v->getCodeConstants().getFuncOrClass(index_func_def).first)->accept(v);
    v->visit_vreg(&vreg_dst, true);
}

void BST_MakeFunction::accept_stmt(StmtVisitor* v) {
    return v->visit_makefunction(this);
}

void BST_MakeClass::accept(BSTVisitor* v) {
    bool skip = v->visit_makeclass(this);
    if (skip)
        return;

    bst_cast<BST_ClassDef>(v->getCodeConstants().getFuncOrClass(index_class_def).first)->accept(v);
    v->visit_vreg(&vreg_dst, true);
}

void BST_MakeClass::accept_stmt(StmtVisitor* v) {
    return v->visit_makeclass(this);
}

void BST_MakeSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_makeslice(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
    v->visit_vreg(&vreg_step);
    v->visit_vreg(&vreg_dst, true);
}

void BST_MakeSlice::accept_stmt(StmtVisitor* v) {
    return v->visit_makeslice(this);
}

void print_bst(BST_stmt* bst, const CodeConstants& code_constants) {
    PrintVisitor v(code_constants, 0, llvm::outs());
    bst->accept(&v);
    v.flush();
}

void PrintVisitor::printIndent() {
    for (int i = 0; i < indent; i++) {
        stream << ' ';
    }
}

extern "C" BoxedString* repr(Box* obj);
bool PrintVisitor::visit_vreg(int* vreg, bool is_dst) {
    if (*vreg != VREG_UNDEFINED) {
        stream << "%" << *vreg;
        if (*vreg < 0)
            stream << "|" << autoDecref(repr(code_constants.getConstant(*vreg)))->s() << "|";
    } else
        stream << "%undef";

    if (is_dst)
        stream << " = ";

    return true;
}

bool PrintVisitor::check_if_invoke(BST_stmt* node) {
    if (node->is_invoke())
        stream << "invoke " << node->get_normal_block()->idx << " " << node->get_exc_block()->idx << ": ";
    return false;
}

bool PrintVisitor::visit_assert(BST_Assert* node) {
    check_if_invoke(node);

    stream << "assert 0";
    if (node->vreg_msg != VREG_UNDEFINED) {
        stream << ", ";
        visit_vreg(&node->vreg_msg);
    }
    return true;
}

bool PrintVisitor::visit_copyvreg(BST_CopyVReg* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "nokill ";
    visit_vreg(&node->vreg_src);
    return true;
}

bool PrintVisitor::visit_augbinop(BST_AugBinOp* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_left);
    stream << " =" << getOpSymbol(node->op_type) << " ";
    visit_vreg(&node->vreg_right);
    return true;
}

bool PrintVisitor::visit_binop(BST_BinOp* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_left);
    stream << " " << getOpSymbol(node->op_type) << " ";
    visit_vreg(&node->vreg_right);
    return true;
}

bool PrintVisitor::visit_callfunc(BST_CallFunc* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_func);
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->elts[i]);
        prevarg = true;
    }
    if (node->vreg_starargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_starargs);
        prevarg = true;
    }
    if (node->vreg_kwargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_kwargs);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_callattr(BST_CallAttr* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << ".";
    stream << code_constants.getInternedString(node->index_attr).s();
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->elts[i]);
        prevarg = true;
    }
    if (node->vreg_starargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_starargs);
        prevarg = true;
    }
    if (node->vreg_kwargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_kwargs);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_callclsattr(BST_CallClsAttr* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << ":";
    stream << code_constants.getInternedString(node->index_attr).s();
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->elts[i]);
        prevarg = true;
    }
    if (node->vreg_starargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_starargs);
        prevarg = true;
    }
    if (node->vreg_kwargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_kwargs);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_compare(BST_Compare* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_left);
    stream << " " << getOpSymbol(node->op) << " ";
    visit_vreg(&node->vreg_comparator);

    return true;
}

bool PrintVisitor::visit_classdef(BST_ClassDef* node) {
    check_if_invoke(node);

    for (int i = 0, n = node->num_decorator; i < n; i++) {
        stream << "@";
        visit_vreg(&node->decorator[i]);
        stream << "\n";
        printIndent();
    }
    stream << "class " << code_constants.getInternedString(node->index_name).s() << "(";
    visit_vreg(&node->vreg_bases_tuple);
    stream << ")";

    indent += 4;
    stream << '\n';
    printIndent();
    stream << "...";
#if 0
    for (int i = 0, n = node->body.size(); i < n; i++) {
        stream << "\n";
        printIndent();
        node->body[i]->accept(this);
    }
#endif
    indent -= 4;

    return true;
}

bool PrintVisitor::visit_deletesub(BST_DeleteSub* node) {
    check_if_invoke(node);

    stream << "del ";
    visit_vreg(&node->vreg_value);
    stream << "[";
    visit_vreg(&node->vreg_slice);
    stream << "]";
    return true;
}
bool PrintVisitor::visit_deletesubslice(BST_DeleteSubSlice* node) {
    check_if_invoke(node);

    stream << "del ";
    visit_vreg(&node->vreg_value);
    stream << "[";
    if (node->vreg_lower != VREG_UNDEFINED)
        visit_vreg(&node->vreg_lower);
    if (node->vreg_upper != VREG_UNDEFINED) {
        stream << ":";
        visit_vreg(&node->vreg_upper);
    }
    stream << "]";
    return true;
}
bool PrintVisitor::visit_deleteattr(BST_DeleteAttr* node) {
    check_if_invoke(node);

    stream << "del ";
    visit_vreg(&node->vreg_value);
    stream << '.';
    stream << code_constants.getInternedString(node->index_attr).s();
    return true;
}
bool PrintVisitor::visit_deletename(BST_DeleteName* node) {
    check_if_invoke(node);

    stream << "del ";
    if (node->lookup_type == ScopeInfo::VarScopeType::FAST || node->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
        visit_vreg(&node->vreg);
        stream << " ";
    }
    stream << code_constants.getInternedString(node->index_id).s();
    return true;
}

bool PrintVisitor::visit_dict(BST_Dict* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "{}";
    return true;
}

bool PrintVisitor::visit_exec(BST_Exec* node) {
    check_if_invoke(node);

    stream << "exec ";

    visit_vreg(&node->vreg_body);
    if (node->vreg_globals != VREG_UNDEFINED) {
        stream << " in ";
        visit_vreg(&node->vreg_globals);

        if (node->vreg_locals != VREG_UNDEFINED) {
            stream << ", ";
            visit_vreg(&node->vreg_locals);
        }
    }
    stream << "\n";
    return true;
}

bool PrintVisitor::visit_functiondef(BST_FunctionDef* node) {
    check_if_invoke(node);

    for (int i = 0; i < node->num_decorator; ++i) {
        stream << "@";
        visit_vreg(&node->elts[i]);
        stream << "\n";
        printIndent();
    }

    stream << "def ";
    if (node->index_name != VREG_UNDEFINED)
        stream << code_constants.getInternedString(node->index_name).s();
    else
        stream << "<lambda>";
    stream << "(";

    for (int i = 0; i < node->num_defaults; ++i) {
        if (i > 0)
            stream << ", ";

        stream << "<default " << i << ">=";
        visit_vreg(&node->elts[node->num_decorator + i]);
    }

    stream << ")";

    indent += 4;
    stream << '\n';
    printIndent();
    stream << "...";
#if 0
    for (int i = 0; i < node->body.size(); i++) {
        stream << "\n";
        printIndent();
        node->body[i]->accept(this);
    }
#endif
    indent -= 4;
    return true;
}

bool PrintVisitor::visit_landingpad(BST_Landingpad* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":LANDINGPAD()";
    return true;
}
bool PrintVisitor::visit_locals(BST_Locals* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":LOCALS()";
    return true;
}
bool PrintVisitor::visit_getiter(BST_GetIter* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":GET_ITER(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_importfrom(BST_ImportFrom* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":IMPORT_FROM(";
    visit_vreg(&node->vreg_module);
    stream << ", ";
    visit_vreg(&node->vreg_name);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_importname(BST_ImportName* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":IMPORT_NAME(";
    visit_vreg(&node->vreg_from);
    stream << ", ";
    visit_vreg(&node->vreg_name);
    stream << ", " << node->level << ")";
    return true;
}
bool PrintVisitor::visit_importstar(BST_ImportStar* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":IMPORT_STAR(";
    visit_vreg(&node->vreg_name);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_nonzero(BST_Nonzero* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":NONZERO(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_checkexcmatch(BST_CheckExcMatch* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":CHECK_EXC_MATCH(";
    visit_vreg(&node->vreg_value);
    stream << ", ";
    visit_vreg(&node->vreg_cls);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_setexcinfo(BST_SetExcInfo* node) {
    check_if_invoke(node);

    stream << ":SET_EXC_INFO(";
    visit_vreg(&node->vreg_value);
    stream << ", ";
    visit_vreg(&node->vreg_type);
    stream << ", ";
    visit_vreg(&node->vreg_traceback);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_uncacheexcinfo(BST_UncacheExcInfo* node) {
    check_if_invoke(node);

    stream << ":UNCACHE_EXC_INFO()";
    return true;
}
bool PrintVisitor::visit_hasnext(BST_HasNext* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << ":HAS_NEXT(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_printexpr(BST_PrintExpr* node) {
    check_if_invoke(node);

    stream << ":PRINT_EXPR(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}

bool PrintVisitor::visit_list(BST_List* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "[";
    for (int i = 0, n = node->num_elts; i < n; ++i) {
        if (i > 0)
            stream << ", ";
        visit_vreg(&node->elts[i]);
    }
    stream << "]";
    return true;
}

bool PrintVisitor::visit_print(BST_Print* node) {
    check_if_invoke(node);

    stream << "print ";
    if (node->vreg_dest != VREG_UNDEFINED) {
        stream << ">>";
        visit_vreg(&node->vreg_dest);
        stream << ", ";
    }
    if (node->vreg_value != VREG_UNDEFINED)
        visit_vreg(&node->vreg_value);
    if (!node->nl)
        stream << ",";
    return true;
}

bool PrintVisitor::visit_raise(BST_Raise* node) {
    check_if_invoke(node);

    stream << "raise";
    if (node->vreg_arg0 != VREG_UNDEFINED) {
        stream << " ";
        visit_vreg(&node->vreg_arg0);
    }
    if (node->vreg_arg1 != VREG_UNDEFINED) {
        stream << ", ";
        visit_vreg(&node->vreg_arg1);
    }
    if (node->vreg_arg2 != VREG_UNDEFINED) {
        stream << ", ";
        visit_vreg(&node->vreg_arg2);
    }
    return true;
}

bool PrintVisitor::visit_repr(BST_Repr* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "`";
    visit_vreg(&node->vreg_value);
    stream << "`";
    return true;
}

bool PrintVisitor::visit_return(BST_Return* node) {
    check_if_invoke(node);

    stream << "return ";
    if (node->vreg_value != VREG_UNDEFINED)
        visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_set(BST_Set* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    // An empty set literal is not writeable in Python (it's a dictionary),
    // but we sometimes generate it (ex in set comprehension lowering).
    // Just to make it clear when printing, print empty set literals as "SET{}".
    if (!node->num_elts)
        stream << "SET";

    stream << "{";

    bool first = true;
    for (int i = 0; i < node->num_elts; ++i) {
        if (!first)
            stream << ", ";
        first = false;

        visit_vreg(&node->num_elts[&i]);
    }

    stream << "}";
    return true;
}

bool PrintVisitor::visit_makeslice(BST_MakeSlice* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "<slice>(";
    if (node->vreg_lower != VREG_UNDEFINED)
        visit_vreg(&node->vreg_lower);
    if (node->vreg_upper != VREG_UNDEFINED || node->vreg_step != VREG_UNDEFINED)
        stream << ':';
    if (node->vreg_upper != VREG_UNDEFINED)
        visit_vreg(&node->vreg_upper);
    if (node->vreg_step != VREG_UNDEFINED) {
        stream << ':';
        visit_vreg(&node->vreg_step);
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_loadname(BST_LoadName* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    if (node->lookup_type == ScopeInfo::VarScopeType::FAST || node->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
        visit_vreg(&node->vreg);
        stream << " ";
    }
    stream << code_constants.getInternedString(node->index_id).s();
    return true;
}

bool PrintVisitor::visit_loadattr(BST_LoadAttr* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << (node->clsonly ? ':' : '.') << code_constants.getInternedString(node->index_attr).s();
    return true;
}

bool PrintVisitor::visit_loadsub(BST_LoadSub* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << "[";
    visit_vreg(&node->vreg_slice);
    stream << "]";
    return true;
}

bool PrintVisitor::visit_loadsubslice(BST_LoadSubSlice* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << "[";
    if (node->vreg_lower != VREG_UNDEFINED)
        visit_vreg(&node->vreg_lower);
    if (node->vreg_upper != VREG_UNDEFINED) {
        stream << ":";
        visit_vreg(&node->vreg_upper);
    }
    stream << "]";
    return true;
}

bool PrintVisitor::visit_storename(BST_StoreName* node) {
    check_if_invoke(node);

    if (node->lookup_type == ScopeInfo::VarScopeType::FAST || node->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
        visit_vreg(&node->vreg);
        stream << " ";
    }
    stream << code_constants.getInternedString(node->index_id).s();
    stream << " = ";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_storeattr(BST_StoreAttr* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_target);
    stream << "." << code_constants.getInternedString(node->index_attr).s() << " = ";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_storesub(BST_StoreSub* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_target);
    stream << "[";
    visit_vreg(&node->vreg_slice);
    stream << "] = ";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_storesubslice(BST_StoreSubSlice* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_target);
    stream << "[";
    if (node->vreg_lower != VREG_UNDEFINED)
        visit_vreg(&node->vreg_lower);
    if (node->vreg_upper != VREG_UNDEFINED) {
        stream << ":";
        visit_vreg(&node->vreg_upper);
    }
    stream << "] = ";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_tuple(BST_Tuple* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "(";
    int n = node->num_elts;
    for (int i = 0; i < n; i++) {
        if (i)
            stream << ", ";
        visit_vreg(&node->elts[i]);
    }
    if (n == 1)
        stream << ",";
    stream << ")";
    return true;
}

bool PrintVisitor::visit_unaryop(BST_UnaryOp* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    switch (node->op_type) {
        case AST_TYPE::Invert:
            stream << "~";
            break;
        case AST_TYPE::Not:
            stream << "not ";
            break;
        case AST_TYPE::UAdd:
            stream << "+";
            break;
        case AST_TYPE::USub:
            stream << "-";
            break;
        default:
            RELEASE_ASSERT(0, "%s", getOpName(node->op_type)->c_str());
            break;
    }
    stream << "(";
    visit_vreg(&node->vreg_operand);
    stream << ")";
    return true;
}

bool PrintVisitor::visit_unpackintoarray(BST_UnpackIntoArray* node) {
    check_if_invoke(node);

    stream << "(";
    for (int i = 0; i < node->num_elts; ++i) {
        visit_vreg(&node->vreg_dst[i]);
        if (i + 1 < node->num_elts || i == 0)
            stream << ", ";
    }
    stream << ") = ";

    visit_vreg(&node->vreg_src);
    return true;
}

bool PrintVisitor::visit_yield(BST_Yield* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "yield ";
    if (node->vreg_value != VREG_UNDEFINED)
        visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_branch(BST_Branch* node) {
    check_if_invoke(node);

    stream << "if ";
    visit_vreg(&node->vreg_test);
    stream << " goto " << node->iftrue->idx << " else goto " << node->iffalse->idx;
    return true;
}

bool PrintVisitor::visit_jump(BST_Jump* node) {
    check_if_invoke(node);

    stream << "goto " << node->target->idx;
    return true;
}

bool PrintVisitor::visit_makefunction(BST_MakeFunction* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "make_";
    return false;
}

bool PrintVisitor::visit_makeclass(BST_MakeClass* node) {
    check_if_invoke(node);

    visit_vreg(&node->vreg_dst, true);
    stream << "make_";
    return false;
}
}
