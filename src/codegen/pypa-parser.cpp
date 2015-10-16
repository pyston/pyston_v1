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

#include "codegen/pypa-parser.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pypa/ast/visitor.hh>
#include <pypa/parser/parser.hh>
#include <sys/stat.h>

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SwapByteOrder.h"

#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

/* For cStringIO: */
#include "Python.h"
#include "cStringIO.h"

namespace pypa {
bool string_to_double(String const& s, double& result);
}

namespace pyston {

void location(AST* t, pypa::Ast& a) {
    t->lineno = a.line;
    assert(a.column < 100000);
    t->col_offset = a.column;
}

AST_expr* readItem(pypa::AstExpression& e, InternedStringPool& interned_strings);
AST_slice* readItem(pypa::AstSliceType& e, InternedStringPool& interned_strings);
AST_stmt* readItem(pypa::AstStatement& s, InternedStringPool& interned_strings);
AST_ExceptHandler* readItem(pypa::AstExcept&, InternedStringPool& interned_strings);
AST_ExceptHandler* readItem(pypa::AstExceptPtr, InternedStringPool& interned_strings);

template <typename T>
auto readItem(std::shared_ptr<T>& t, InternedStringPool& interned_strings) -> decltype(readItem(*t, interned_strings)) {
    if (t)
        return readItem(*t, interned_strings);
    return nullptr;
}

AST_TYPE::AST_TYPE readItem(pypa::AstBoolOpType type) {
    switch (type) {
        case pypa::AstBoolOpType::And:
            return AST_TYPE::And;
        case pypa::AstBoolOpType::Or:
            return AST_TYPE::Or;
        default:
            break;
    }
    assert("Unknown AstBoolOpType" && false);
    abort();
}

AST_TYPE::AST_TYPE readItem(pypa::AstBinOpType type) {
    switch (type) {
        case pypa::AstBinOpType::Add:
            return AST_TYPE::Add;
        case pypa::AstBinOpType::BitAnd:
            return AST_TYPE::BitAnd;
        case pypa::AstBinOpType::BitOr:
            return AST_TYPE::BitOr;
        case pypa::AstBinOpType::BitXor:
            return AST_TYPE::BitXor;
        case pypa::AstBinOpType::Div:
            return AST_TYPE::Div;
        case pypa::AstBinOpType::FloorDiv:
            return AST_TYPE::FloorDiv;
        case pypa::AstBinOpType::LeftShift:
            return AST_TYPE::LShift;
        case pypa::AstBinOpType::Mod:
            return AST_TYPE::Mod;
        case pypa::AstBinOpType::Mult:
            return AST_TYPE::Mult;
        case pypa::AstBinOpType::Power:
            return AST_TYPE::Pow;
        case pypa::AstBinOpType::RightShift:
            return AST_TYPE::RShift;
        case pypa::AstBinOpType::Sub:
            return AST_TYPE::Sub;
        default:
            break;
    }
    assert("Unknown AstBinOpType" && false);
    abort();
}

AST_TYPE::AST_TYPE readItem(pypa::AstUnaryOpType type) {
    switch (type) {
        case pypa::AstUnaryOpType::Add:
            return AST_TYPE::UAdd;
        case pypa::AstUnaryOpType::Invert:
            return AST_TYPE::Invert;
        case pypa::AstUnaryOpType::Not:
            return AST_TYPE::Not;
        case pypa::AstUnaryOpType::Sub:
            return AST_TYPE::USub;
        default:
            break;
    }
    assert("Unknown AstUnaryOpType" && false);
    abort();
}


AST_TYPE::AST_TYPE readItem(pypa::AstContext ctx) {
    switch (ctx) {
        case pypa::AstContext::Load:
            return AST_TYPE::Load;
        case pypa::AstContext::Store:
            return AST_TYPE::Store;
        case pypa::AstContext::AugLoad:
            return AST_TYPE::Load;
        case pypa::AstContext::AugStore:
            return AST_TYPE::Store;
        case pypa::AstContext::Param:
            return AST_TYPE::Param;
        case pypa::AstContext::Del:
            return AST_TYPE::Del;
        default:
            break;
    }
    assert("Unknown AstContext" && false);
    return AST_TYPE::Load;
}

AST_TYPE::AST_TYPE readItem(pypa::AstCompareOpType type) {
    switch (type) {
        case pypa::AstCompareOpType::Equals:
            return AST_TYPE::Eq;
        case pypa::AstCompareOpType::In:
            return AST_TYPE::In;
        case pypa::AstCompareOpType::Is:
            return AST_TYPE::Is;
        case pypa::AstCompareOpType::IsNot:
            return AST_TYPE::IsNot;
        case pypa::AstCompareOpType::Less:
            return AST_TYPE::Lt;
        case pypa::AstCompareOpType::LessEqual:
            return AST_TYPE::LtE;
        case pypa::AstCompareOpType::More:
            return AST_TYPE::Gt;
        case pypa::AstCompareOpType::MoreEqual:
            return AST_TYPE::GtE;
        case pypa::AstCompareOpType::NotEqual:
            return AST_TYPE::NotEq;
        case pypa::AstCompareOpType::NotIn:
            return AST_TYPE::NotIn;
        default:
            break;
    }
    assert("Unknown AstCompareOpType" && false);
    abort();
}

InternedString readName(pypa::AstExpression& e, InternedStringPool& interned_strings) {
    assert(e.type == pypa::AstType::Name);
    return interned_strings.get(static_cast<pypa::AstName&>(e).id);
}

InternedString readName(pypa::AstExpr& n, InternedStringPool& interned_strings) {
    if (!n) {
        return interned_strings.get(std::string());
    }
    return readName(*n, interned_strings);
}

AST_keyword* readItem(pypa::AstKeyword& k, InternedStringPool& interned_strings) {
    AST_keyword* ptr = new AST_keyword();
    location(ptr, k);
    ptr->arg = readName(k.name, interned_strings);
    ptr->value = readItem(k.value, interned_strings);
    return ptr;
}

void readVector(std::vector<AST_keyword*>& t, pypa::AstExprList& items, InternedStringPool& interned_strings) {
    for (auto& item : items) {
        assert(item->type == pypa::AstType::Keyword);
        t.push_back(readItem(static_cast<pypa::AstKeyword&>(*item), interned_strings));
    }
}

template <typename T, typename U>
void readVector(std::vector<T*>& t, std::vector<U>& u, InternedStringPool& interned_strings) {
    for (auto& item : u) {
        if (!item) {
            t.push_back(nullptr);
        } else {
            t.push_back(readItem(*item, interned_strings));
        }
    }
}

void readVector(std::vector<AST_expr*>& t, pypa::AstExpression& u, InternedStringPool& interned_strings) {
    if (u.type == pypa::AstType::Tuple) {
        pypa::AstTuple& e = static_cast<pypa::AstTuple&>(u);
        for (auto& item : e.elements) {
            assert(item);
            t.push_back(readItem(*item, interned_strings));
        }
    } else {
        t.push_back(readItem(u, interned_strings));
    }
}

void readVector(std::vector<AST_slice*>& t, pypa::AstSliceType& u, InternedStringPool& interned_strings) {
    t.push_back(readItem(u, interned_strings));
}

void readVector(std::vector<AST_stmt*>& t, pypa::AstStatement& u, InternedStringPool& interned_strings) {
    if (u.type == pypa::AstType::Suite) {
        pypa::AstSuite& e = static_cast<pypa::AstSuite&>(u);
        for (auto& item : e.items) {
            assert(item);
            t.push_back(readItem(*item, interned_strings));
        }
    } else {
        t.push_back(readItem(u, interned_strings));
    }
}

AST_comprehension* readItem(pypa::AstComprehension& c, InternedStringPool& interned_strings) {
    AST_comprehension* ptr = new AST_comprehension();
    ptr->target = readItem(c.target, interned_strings);
    ptr->iter = readItem(c.iter, interned_strings);
    readVector(ptr->ifs, c.ifs, interned_strings);
    return ptr;
}

AST_comprehension* readItem(pypa::AstComprPtr c, InternedStringPool& interned_strings) {
    if (c)
        return readItem(*c, interned_strings);
    return nullptr;
}

void readVector(std::vector<AST_comprehension*>& t, pypa::AstExprList& u, InternedStringPool& interned_strings) {
    for (auto& e : u) {
        assert(e && e->type == pypa::AstType::Comprehension);
        t.push_back(readItem(static_cast<pypa::AstComprehension&>(*e), interned_strings));
    }
}

void readVector(std::vector<AST_slice*>& t, pypa::AstSliceTypePtr u, InternedStringPool& interned_strings) {
    if (u) {
        readVector(t, *u, interned_strings);
    }
}

void readVector(std::vector<AST_stmt*>& t, pypa::AstStmt u, InternedStringPool& interned_strings) {
    if (u) {
        readVector(t, *u, interned_strings);
    }
}

AST_ExceptHandler* readItem(pypa::AstExcept& e, InternedStringPool& interned_strings) {
    AST_ExceptHandler* ptr = new AST_ExceptHandler();
    location(ptr, e);
    readVector(ptr->body, e.body, interned_strings);
    ptr->name = readItem(e.name, interned_strings);
    ptr->type = readItem(e.type, interned_strings);
    return ptr;
}

AST_ExceptHandler* readItem(pypa::AstExceptPtr ptr, InternedStringPool& interned_strings) {
    assert(ptr);
    return readItem(*ptr, interned_strings);
}

AST_alias* readItem(pypa::AstAlias& a, InternedStringPool& interned_strings) {
    return new AST_alias(readName(a.name, interned_strings), readName(a.as_name, interned_strings));
}

AST_arguments* readItem(pypa::AstArguments& a, InternedStringPool& interned_strings) {
    AST_arguments* ptr = new AST_arguments();
    location(ptr, a);
    readVector(ptr->defaults, a.defaults, interned_strings);
    ptr->defaults.erase(std::remove(ptr->defaults.begin(), ptr->defaults.end(), nullptr), ptr->defaults.end());
    readVector(ptr->args, a.arguments, interned_strings);
    ptr->kwarg = readName(a.kwargs, interned_strings);
    ptr->vararg = readName(a.args, interned_strings);
    return ptr;
}

struct slice_dispatcher {
    InternedStringPool& interned_strings;
    slice_dispatcher(InternedStringPool& interned_strings) : interned_strings(interned_strings) {}

    typedef AST_slice* ResultPtr;
    template <typename T> ResultPtr operator()(std::shared_ptr<T> t) {
        if (t)
            return (*this)(*t);
        return nullptr;
    }

    template <typename T> ResultPtr operator()(T& t) {
        pypa::Ast& a = t;
        return read(t);
    }

    template <typename T> ResultPtr read(T& item) {
        pypa::Ast& a = item;
        RELEASE_ASSERT(0, "Unhandled ast slice type caught: %d @%s\n", a.type, __PRETTY_FUNCTION__);
    }

    ResultPtr read(pypa::AstEllipsis& e) {
        AST_Ellipsis* ptr = new AST_Ellipsis();
        location(ptr, e);
        return ptr;
    }

    ResultPtr read(pypa::AstExtSlice& e) {
        AST_ExtSlice* ptr = new AST_ExtSlice();
        location(ptr, e);
        readVector(ptr->dims, e.dims, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstIndex& i) {
        AST_Index* ptr = new AST_Index();
        location(ptr, i);
        ptr->value = readItem(i.value, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstSlice& s) {
        AST_Slice* ptr = new AST_Slice();
        location(ptr, s);
        ptr->lower = readItem(s.lower, interned_strings);
        ptr->upper = readItem(s.upper, interned_strings);
        ptr->step = readItem(s.step, interned_strings);
        return ptr;
    }
};

struct expr_dispatcher {
    InternedStringPool& interned_strings;
    expr_dispatcher(InternedStringPool& interned_strings) : interned_strings(interned_strings) {}

    typedef AST_expr* ResultPtr;
    template <typename T> ResultPtr operator()(std::shared_ptr<T> t) {
        if (t)
            return (*this)(*t);
        return nullptr;
    }

    template <typename T> ResultPtr operator()(T& t) {
        pypa::Ast& a = t;
        return read(t);
    }

    template <typename T> ResultPtr read(T& item) {
        pypa::Ast& a = item;
        RELEASE_ASSERT(0, "Unhandled ast expression type caught: %d @%s\n", a.type, __PRETTY_FUNCTION__);
    }

    ResultPtr read(pypa::AstAttribute& a) {
        AST_Attribute* ptr = new AST_Attribute();
        location(ptr, a);
        ptr->value = readItem(a.value, interned_strings);
        ptr->attr = readName(a.attribute, interned_strings);
        ptr->ctx_type = readItem(a.context);
        return ptr;
    }

    ResultPtr read(pypa::AstBoolOp& b) {
        AST_BoolOp* ptr = new AST_BoolOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        readVector(ptr->values, b.values, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstBinOp& b) {
        AST_BinOp* ptr = new AST_BinOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        ptr->left = readItem(b.left, interned_strings);
        ptr->right = readItem(b.right, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstCall& c) {
        AST_Call* ptr = new AST_Call();
        location(ptr, c);
        readVector(ptr->args, c.arglist.arguments, interned_strings);
        readVector(ptr->keywords, c.arglist.keywords, interned_strings);
        ptr->func = readItem(c.function, interned_strings);
        ptr->starargs = readItem(c.arglist.args, interned_strings);
        ptr->kwargs = readItem(c.arglist.kwargs, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstCompare& c) {
        AST_Compare* ptr = new AST_Compare();
        location(ptr, c);
        ptr->left = readItem(c.left, interned_strings);
        ptr->ops.reserve(c.operators.size());
        for (auto op : c.operators) {
            ptr->ops.push_back(readItem(op));
        }
        readVector(ptr->comparators, c.comparators, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstComplex& c) {
        AST_Num* imag = new AST_Num();
        location(imag, c);
        imag->num_type = AST_Num::COMPLEX;
        sscanf(c.imag.c_str(), "%lf", &imag->n_float);
        if (!c.real)
            return imag;

        AST_BinOp* binop = new AST_BinOp();
        location(binop, c);
        binop->op_type = AST_TYPE::Add;
        binop->left = readItem(c.real, interned_strings);
        binop->right = imag;
        return binop;
    }

    ResultPtr read(pypa::AstComprehension& c) {
        assert("This should not be called" && false);
        return nullptr;
    }

    ResultPtr read(pypa::AstDict& d) {
        AST_Dict* ptr = new AST_Dict();
        location(ptr, d);
        readVector(ptr->keys, d.keys, interned_strings);
        readVector(ptr->values, d.values, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstDictComp& d) {
        AST_DictComp* ptr = new AST_DictComp();
        location(ptr, d);
        ptr->key = readItem(d.key, interned_strings);
        ptr->value = readItem(d.value, interned_strings);
        readVector(ptr->generators, d.generators, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstIfExpr& i) {
        AST_IfExp* ptr = new AST_IfExp();
        location(ptr, i);
        ptr->body = readItem(i.body, interned_strings);
        ptr->test = readItem(i.test, interned_strings);
        ptr->orelse = readItem(i.orelse, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstGenerator& g) {
        AST_GeneratorExp* ptr = new AST_GeneratorExp();
        location(ptr, g);
        ptr->elt = readItem(g.element, interned_strings);
        readVector(ptr->generators, g.generators, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstLambda& l) {
        AST_Lambda* ptr = new AST_Lambda();
        location(ptr, l);
        ptr->args = readItem(l.arguments, interned_strings);
        ptr->body = readItem(l.body, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstList& l) {
        AST_List* ptr = new AST_List();
        location(ptr, l);
        readVector(ptr->elts, l.elements, interned_strings);
        ptr->ctx_type = readItem(l.context);
        return ptr;
    }

    ResultPtr read(pypa::AstListComp& l) {
        AST_ListComp* ptr = new AST_ListComp();
        location(ptr, l);
        readVector(ptr->generators, l.generators, interned_strings);
        ptr->elt = readItem(l.element, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstName& a) {
        AST_Name* ptr = new AST_Name(interned_strings.get(a.id), readItem(a.context), a.line, a.column);
        return ptr;
    }

    ResultPtr read(pypa::AstNone& n) {
        AST_Name* ptr = new AST_Name(interned_strings.get("None"), AST_TYPE::Load, n.line, n.column);
        return ptr;
    }

    ResultPtr read(pypa::AstNumber& c) {
        AST_Num* ptr = new AST_Num();
        location(ptr, c);
        switch (c.num_type) {
            case pypa::AstNumber::Float:
                ptr->num_type = AST_Num::FLOAT;
                ptr->n_float = c.floating;
                break;
            case pypa::AstNumber::Long:
                ptr->num_type = AST_Num::LONG;
                ptr->n_long = c.str;
                break;
            default:
                ptr->num_type = AST_Num::INT;
                ptr->n_int = c.integer;
                break;
        }
        return ptr;
    }

    ResultPtr read(pypa::AstRepr& r) {
        AST_Repr* ptr = new AST_Repr();
        location(ptr, r);
        ptr->value = readItem(r.value, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstSet& s) {
        AST_Set* ptr = new AST_Set();
        location(ptr, s);
        readVector(ptr->elts, s.elements, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstSetComp& l) {
        AST_SetComp* ptr = new AST_SetComp();
        location(ptr, l);
        readVector(ptr->generators, l.generators, interned_strings);
        ptr->elt = readItem(l.element, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstStr& s) {
        AST_Str* ptr = new AST_Str();
        location(ptr, s);
        ptr->str_type = s.unicode ? AST_Str::UNICODE : AST_Str::STR;
        ptr->str_data = s.value;
        return ptr;
    }

    ResultPtr read(pypa::AstSubscript& s) {
        AST_Subscript* ptr = new AST_Subscript();
        location(ptr, s);
        ptr->value = readItem(s.value, interned_strings);
        ptr->ctx_type = readItem(s.context);
        ptr->slice = readItem(s.slice, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstTuple& t) {
        AST_Tuple* ptr = new AST_Tuple();
        location(ptr, t);
        readVector(ptr->elts, t.elements, interned_strings);
        ptr->ctx_type = readItem(t.context);
        return ptr;
    }

    ResultPtr read(pypa::AstUnaryOp& b) {
        AST_UnaryOp* ptr = new AST_UnaryOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        ptr->operand = readItem(b.operand, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstYieldExpr& e) {
        AST_Yield* ptr = new AST_Yield();
        location(ptr, e);
        ptr->value = readItem(e.args, interned_strings);
        return ptr;
    }
};

struct stmt_dispatcher {
    InternedStringPool& interned_strings;
    stmt_dispatcher(InternedStringPool& interned_strings) : interned_strings(interned_strings) {}

    typedef AST_stmt* ResultPtr;
    template <typename T> ResultPtr operator()(std::shared_ptr<T> t) {
        if (t)
            return (*this)(*t);
        return nullptr;
    }

    template <typename T> ResultPtr operator()(T& t) {
        pypa::Ast& a = t;
        return read(t);
    }

    template <typename T> ResultPtr read(T& item) {
        pypa::Ast& a = item;
        fprintf(stderr, "Unhandled ast statement type caught: %d @%s\n", a.type, __PRETTY_FUNCTION__);
        return nullptr;
    }

    ResultPtr read(pypa::AstAssign& a) {
        AST_Assign* ptr = new AST_Assign();
        location(ptr, a);
        readVector(ptr->targets, a.targets, interned_strings);
        ptr->value = readItem(a.value, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstAssert& a) {
        AST_Assert* ptr = new AST_Assert();
        location(ptr, a);
        ptr->msg = readItem(a.expression, interned_strings);
        ptr->test = readItem(a.test, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstAugAssign& a) {
        AST_AugAssign* ptr = new AST_AugAssign();
        location(ptr, a);
        ptr->op_type = readItem(a.op);
        ptr->target = readItem(a.target, interned_strings);
        ptr->value = readItem(a.value, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstBreak& b) {
        AST_Break* ptr = new AST_Break();
        location(ptr, b);
        return ptr;
    }

    ResultPtr read(pypa::AstClassDef& c) {
        AST_ClassDef* ptr = new AST_ClassDef();
        location(ptr, c);
        if (c.bases)
            readVector(ptr->bases, *c.bases, interned_strings);
        readVector(ptr->decorator_list, c.decorators, interned_strings);
        readVector(ptr->body, c.body, interned_strings);
        ptr->name = readName(c.name, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstContinue& c) {
        AST_Continue* ptr = new AST_Continue();
        location(ptr, c);
        return ptr;
    }

    ResultPtr read(pypa::AstDelete& d) {
        AST_Delete* ptr = new AST_Delete();
        location(ptr, d);
        readVector(ptr->targets, *d.targets, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstExec& e) {
        AST_Exec* ptr = new AST_Exec();
        location(ptr, e);
        ptr->body = readItem(e.body, interned_strings);
        if (e.globals)
            ptr->globals = readItem(e.globals, interned_strings);
        else
            ptr->globals = NULL;
        if (e.locals)
            ptr->locals = readItem(e.locals, interned_strings);
        else
            ptr->locals = NULL;
        assert(ptr->globals || !ptr->locals);
        return ptr;
    }

    ResultPtr read(pypa::AstExpressionStatement& e) {
        AST_Expr* ptr = new AST_Expr();
        location(ptr, e);
        ptr->value = readItem(e.expr, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstFor& f) {
        AST_For* ptr = new AST_For();
        location(ptr, f);
        ptr->target = readItem(f.target, interned_strings);
        if (f.iter)
            ptr->iter = readItem(f.iter, interned_strings);
        if (f.body)
            readVector(ptr->body, *f.body, interned_strings);
        if (f.orelse)
            readVector(ptr->orelse, *f.orelse, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstFunctionDef& f) {
        AST_FunctionDef* ptr = new AST_FunctionDef();
        location(ptr, f);
        readVector(ptr->decorator_list, f.decorators, interned_strings);
        ptr->name = readName(f.name, interned_strings);
        ptr->args = readItem(f.args, interned_strings);
        readVector(ptr->body, f.body, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstGlobal& g) {
        AST_Global* ptr = new AST_Global();
        location(ptr, g);
        ptr->names.resize(g.names.size());
        for (size_t i = 0; i < g.names.size(); ++i) {
            ptr->names[i] = readName(*g.names[i], interned_strings);
        }
        return ptr;
    }

    ResultPtr read(pypa::AstIf& i) {
        AST_If* ptr = new AST_If();
        location(ptr, i);
        readVector(ptr->body, i.body, interned_strings);
        ptr->test = readItem(i.test, interned_strings);
        assert(ptr->test != 0);
        readVector(ptr->orelse, i.orelse, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstImport& i) {
        AST_Import* ptr = new AST_Import();
        location(ptr, i);
        if (i.names->type == pypa::AstType::Tuple) {
            for (auto& name : static_cast<pypa::AstTuple&>(*i.names).elements) {
                assert(name->type == pypa::AstType::Alias);
                ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*name), interned_strings));
            }
        } else {
            assert(i.names->type == pypa::AstType::Alias);
            ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*i.names), interned_strings));
        }
        return ptr;
    }

    ResultPtr read(pypa::AstImportFrom& i) {
        AST_ImportFrom* ptr = new AST_ImportFrom();
        location(ptr, i);
        ptr->module = readName(i.module, interned_strings);
        if (i.names->type == pypa::AstType::Tuple) {
            for (auto& name : static_cast<pypa::AstTuple&>(*i.names).elements) {
                assert(name->type == pypa::AstType::Alias);
                ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*name), interned_strings));
            }
        } else {
            assert(i.names->type == pypa::AstType::Alias);
            ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*i.names), interned_strings));
        }
        ptr->level = i.level;
        return ptr;
    }

    ResultPtr read(pypa::AstPass& p) {
        AST_Pass* ptr = new AST_Pass();
        location(ptr, p);
        return ptr;
    }

    ResultPtr read(pypa::AstPrint& p) {
        AST_Print* ptr = new AST_Print();
        location(ptr, p);
        ptr->dest = readItem(p.destination, interned_strings);
        ptr->nl = p.newline;
        readVector(ptr->values, p.values, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstRaise& r) {
        AST_Raise* ptr = new AST_Raise();
        location(ptr, r);
        ptr->arg0 = readItem(r.arg0, interned_strings);
        ptr->arg1 = readItem(r.arg1, interned_strings);
        ptr->arg2 = readItem(r.arg2, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstSuite& s) { return nullptr; }

    ResultPtr read(pypa::AstReturn& r) {
        AST_Return* ptr = new AST_Return();
        location(ptr, r);
        ptr->value = readItem(r.value, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstTryExcept& t) {
        AST_TryExcept* ptr = new AST_TryExcept();
        location(ptr, t);
        readVector(ptr->body, t.body, interned_strings);
        readVector(ptr->orelse, t.orelse, interned_strings);
        readVector(ptr->handlers, t.handlers, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstTryFinally& t) {
        AST_TryFinally* ptr = new AST_TryFinally();
        location(ptr, t);
        readVector(ptr->body, t.body, interned_strings);
        readVector(ptr->finalbody, t.final_body, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstWith& w) {
        AST_With* ptr = new AST_With();
        location(ptr, w);
        ptr->optional_vars = readItem(w.optional, interned_strings);
        ptr->context_expr = readItem(w.context, interned_strings);
        readVector(ptr->body, w.body, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstWhile& w) {
        AST_While* ptr = new AST_While();
        location(ptr, w);
        ptr->test = readItem(w.test, interned_strings);
        readVector(ptr->body, w.body, interned_strings);
        readVector(ptr->orelse, w.orelse, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstYield& w) {
        AST_Expr* ptr = new AST_Expr();
        location(ptr, w);
        ptr->value = readItem(w.yield, interned_strings);
        return ptr;
    }

    ResultPtr read(pypa::AstDocString& d) {
        AST_Expr* ptr = new AST_Expr();
        location(ptr, d);
        AST_Str* str = new AST_Str();
        location(str, d);
        ptr->value = str;
        str->str_type = d.unicode ? AST_Str::UNICODE : AST_Str::STR;
        str->str_data = d.doc;
        return ptr;
    }
};

AST_expr* readItem(pypa::AstExpression& e, InternedStringPool& interned_strings) {
    return pypa::visit<AST_expr*>(expr_dispatcher(interned_strings), e);
}

AST_slice* readItem(pypa::AstSliceType& e, InternedStringPool& interned_strings) {
    return pypa::visit<AST_slice*>(slice_dispatcher(interned_strings), e);
}

AST_stmt* readItem(pypa::AstStatement& s, InternedStringPool& interned_strings) {
    return pypa::visit<AST_stmt*>(stmt_dispatcher(interned_strings), s);
}

AST_Module* readModule(pypa::AstModule& t) {
    if (VERBOSITY("PYPA parsing") >= 2) {
        printf("PYPA reading module\n");
    }
    AST_Module* mod = new AST_Module(llvm::make_unique<InternedStringPool>());
    readVector(mod->body, t.body->items, *mod->interned_strings);
    return mod;
}

void pypaErrorHandler(pypa::Error e) {
    if (e.type != pypa::ErrorType::SyntaxWarning) {
        raiseSyntaxError(e.message.c_str(), e.cur.line, e.cur.column, e.file_name, std::string());
    }
}

static PyObject* decode_utf8(const char** sPtr, const char* end, const char* encoding) noexcept {
#ifndef Py_USING_UNICODE
    Py_FatalError("decode_utf8 should not be called in this build.");
    return NULL;
#else
    PyObject* u, *v;
    const char* s, *t;
    t = s = (const char*)*sPtr;
    /* while (s < end && *s != '\\') s++; */ /* inefficient for u".." */
    while (s < end && (*s & 0x80))
        s++;
    *sPtr = s;
    u = PyUnicode_DecodeUTF8(t, s - t, NULL);
    if (u == NULL)
        return NULL;
    v = PyUnicode_AsEncodedString(u, encoding, NULL);
    Py_DECREF(u);
    return v;
#endif
}

#ifdef Py_USING_UNICODE
static PyObject* decode_unicode(const char* s, size_t len, int rawmode, const char* encoding) noexcept {
    PyObject* v;
    PyObject* u = NULL;
    char* buf;
    char* p;
    const char* end;
    if (encoding != NULL && strcmp(encoding, "iso-8859-1")) {
        /* check for integer overflow */
        if (len > PY_SIZE_MAX / 6)
            return NULL;
        /* "<C3><A4>" (2 bytes) may become "\U000000E4" (10 bytes), or 1:5
           "\ä" (3 bytes) may become "\u005c\U000000E4" (16 bytes), or ~1:6 */
        u = PyString_FromStringAndSize((char*)NULL, len * 6);
        if (u == NULL)
            return NULL;
        p = buf = PyString_AsString(u);
        end = s + len;
        while (s < end) {
            if (*s == '\\') {
                *p++ = *s++;
                if (*s & 0x80) {
                    strcpy(p, "u005c");
                    p += 5;
                }
            }
            if (*s & 0x80) { /* XXX inefficient */
                PyObject* w;
                char* r;
                Py_ssize_t rn, i;
                w = decode_utf8(&s, end, "utf-32-be");
                if (w == NULL) {
                    Py_DECREF(u);
                    return NULL;
                }
                r = PyString_AsString(w);
                rn = PyString_Size(w);
                assert(rn % 4 == 0);
                for (i = 0; i < rn; i += 4) {
                    sprintf(p, "\\U%02x%02x%02x%02x", r[i + 0] & 0xFF, r[i + 1] & 0xFF, r[i + 2] & 0xFF,
                            r[i + 3] & 0xFF);
                    p += 10;
                }
                Py_DECREF(w);
            } else {
                *p++ = *s++;
            }
        }
        len = p - buf;
        s = buf;
    }
    if (rawmode)
        v = PyUnicode_DecodeRawUnicodeEscape(s, len, NULL);
    else
        v = PyUnicode_DecodeUnicodeEscape(s, len, NULL);
    Py_XDECREF(u);
    return v;
}
#endif

pypa::String pypaEscapeDecoder(const pypa::String& s, const pypa::String& encoding, bool unicode, bool raw_prefix,
                               bool& error) {
    try {
        error = false;
        if (unicode) {
            PyObject* str = decode_unicode(s.c_str(), s.size(), raw_prefix, encoding.c_str());
            if (!str)
                throwCAPIException();
            BoxedString* str_utf8 = (BoxedString*)PyUnicode_AsUTF8String(str);
            assert(str_utf8->cls == str_cls);
            checkAndThrowCAPIException();
            return str_utf8->s().str();
        }

        bool need_encoding = encoding != "utf-8" && encoding != "iso-8859-1";
        if (raw_prefix || s.find('\\') == pypa::String::npos) {
            if (need_encoding) {
                PyObject* u = PyUnicode_DecodeUTF8(s.c_str(), s.size(), NULL);
                if (!u)
                    throwCAPIException();
                BoxedString* str = (BoxedString*)PyUnicode_AsEncodedString(u, encoding.c_str(), NULL);
                assert(str->cls == str_cls);
                return str->s().str();
            } else {
                return s;
            }
        }

        BoxedString* decoded = (BoxedString*)PyString_DecodeEscape(s.c_str(), s.size(), NULL, false,
                                                                   need_encoding ? encoding.c_str() : NULL);
        if (!decoded)
            throwCAPIException();
        assert(decoded->cls == str_cls);
        return decoded->s().str();
    } catch (ExcInfo e) {
        error = true;
        BoxedString* error_message = str(e.value);
        if (error_message && error_message->cls == str_cls)
            return std::string(error_message->s());
        return "Encountered an unknown error inside pypaEscapeDecoder";
    }
}

class PystonReader : public pypa::Reader {
public:
    PystonReader();
    ~PystonReader() override;
    bool set_encoding(const std::string& coding) override;
    std::string get_line() override;
    unsigned get_line_number() const override { return line_number; }

    virtual char next() = 0;
    virtual PyObject* open_python_file() noexcept = 0;

    bool eof() const override { return is_eof; }
    void set_eof() { is_eof = true; }

private:
    bool is_eof;
    PyObject* readline;
    unsigned line_number;
};

PystonReader::PystonReader() : is_eof(false), readline(nullptr), line_number(0) {
}

PystonReader::~PystonReader() {
    if (readline)
        gc::deregisterPermanentRoot(readline);
    readline = nullptr;
}

bool PystonReader::set_encoding(const std::string& coding) {
    PyObject* stream = open_python_file();
    if (stream == NULL)
        return false;

    PyObject* reader = PyCodec_StreamReader(coding.c_str(), stream, NULL);
    if (reader == NULL)
        return false;

    readline = PyObject_GetAttrString(reader, "readline");
    if (readline == NULL)
        return false;

    gc::registerPermanentRoot(readline);
    return true;
}

std::string PystonReader::get_line() {
    if (eof())
        return std::string();

    if (!readline) {
        std::string line;
        char c;
        do {
            c = next();
            if (eof())
                break;
            line.push_back(c);
        } while (c != '\n' && c != '\x0c');

        // check for UTF8 BOM
        if (line_number == 0 && line[0] == '\xEF' && line[1] == '\xBB' && line[2] == '\xBF') {
            set_encoding("utf-8");
            line.erase(0, 3);
        }
        ++line_number;
        return line;
    }

    BoxedString* line = (BoxedString*)runtimeCall(readline, ArgPassSpec(0), 0, 0, 0, 0, 0);
    if (line->cls == unicode_cls) {
        line = (BoxedString*)PyUnicode_AsUTF8String(line);
        if (line == NULL) {
            is_eof = true;
            return std::string();
        }
    }
    assert(line->cls == str_cls);
    if (!line->size())
        is_eof = true;
    ++line_number;
    return line->s();
}

class PystonFileReader : public PystonReader {
public:
    PystonFileReader(FILE* file, std::string file_path);
    ~PystonFileReader() override;

    PyObject* open_python_file() noexcept override;

    std::string get_filename() const override { return file_path; }

    static std::unique_ptr<PystonFileReader> create(const char* path);

private:
    char next() override;

    FILE* file;
    std::string file_path;
};

PystonFileReader::PystonFileReader(FILE* file, std::string file_path) : file(file), file_path(std::move(file_path)) {
}

PystonFileReader::~PystonFileReader() {
    if (file)
        fclose(file);
    file = nullptr;
    file_path.clear();
    set_eof();
}

std::unique_ptr<PystonFileReader> PystonFileReader::create(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f)
        return nullptr;
    return llvm::make_unique<PystonFileReader>(f, path);
}

PyObject* PystonFileReader::open_python_file() noexcept {
    return PyFile_FromFile(file, file_path.c_str(), "rb", NULL);
}

char PystonFileReader::next() {
    if (eof())
        return 0;

    int c = fgetc(file);
    if (c == EOF) {
        set_eof();
        return 0;
    }
    return c;
}

class PystonStringReader : public PystonReader {
public:
    PystonStringReader(const char* str) : str(str), position(0) {}
    ~PystonStringReader() override {}

    std::string get_filename() const override { return "<stdin>"; }

private:
    char next() override;
    PyObject* open_python_file() noexcept override;

    const char* str;
    int position;
};

static AST_Module* parse_with_reader(std::unique_ptr<pypa::Reader> reader, FutureFlags future_flags) {
    pypa::Lexer lexer(std::move(reader));
    pypa::SymbolTablePtr symbols;
    pypa::AstModulePtr module;
    pypa::ParserOptions options;

    options.perform_inline_optimizations = true;
    options.printerrors = false;
    options.python3allowed = false;
    options.python3only = false;
    options.handle_future_errors = false;
    options.error_handler = pypaErrorHandler;
    options.escape_handler = pypaEscapeDecoder;

    if (future_flags & CO_FUTURE_PRINT_FUNCTION) {
        future_flags &= ~CO_FUTURE_PRINT_FUNCTION;
        options.initial_future_features.print_function = true;
    }

    if (future_flags & CO_FUTURE_DIVISION) {
        future_flags &= ~CO_FUTURE_DIVISION;
        options.initial_future_features.division = true;
    }

    if (future_flags & CO_FUTURE_ABSOLUTE_IMPORT) {
        future_flags &= ~CO_FUTURE_ABSOLUTE_IMPORT;
        options.initial_future_features.absolute_imports = true;
    }

    if (future_flags & CO_FUTURE_WITH_STATEMENT) {
        future_flags &= ~CO_FUTURE_WITH_STATEMENT;
        options.initial_future_features.with_statement = true;
    }

    if (future_flags & CO_FUTURE_UNICODE_LITERALS) {
        future_flags &= ~CO_FUTURE_UNICODE_LITERALS;
        options.initial_future_features.unicode_literals = true;
    }

    // Strip out some flags:
    future_flags &= ~(CO_NESTED);
    RELEASE_ASSERT(!future_flags, "0x%x", future_flags);

    if (pypa::parse(lexer, module, symbols, options) && module) {
        return readModule(*module);
    }
    return nullptr;
}

char PystonStringReader::next() {
    char c = str[position];
    if (c)
        position++;
    else
        set_eof();
    return c;
}

PyObject* PystonStringReader::open_python_file() noexcept {
    PycString_IMPORT;
    PyObject* s = PyString_FromString(str + position);
    if (!s)
        return s;
    return PycStringIO->NewInput(s);
}

AST_Module* pypa_parse(char const* file_path, FutureFlags future_flags) {
    auto reader = PystonFileReader::create(file_path);
    if (!reader)
        return nullptr;

    return parse_with_reader(std::move(reader), future_flags);
}

AST_Module* pypa_parse_string(char const* str, FutureFlags future_flags) {
    auto reader = llvm::make_unique<PystonStringReader>(str);

    return parse_with_reader(std::move(reader), future_flags);
}
}
