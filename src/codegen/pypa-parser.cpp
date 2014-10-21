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

#include "codegen/pypa-parser.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pypa/ast/visitor.hh>
#include <pypa/parser/parser.hh>
#include <sys/stat.h>

#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"

namespace pypa {
bool string_to_double(String const& s, double& result);
}

namespace pyston {

void location(AST* t, pypa::Ast& a) {
    t->lineno = a.line;
    t->col_offset = a.column;
}

AST_expr* readItem(pypa::AstExpression& e);
AST_stmt* readItem(pypa::AstStatement& s);
AST_ExceptHandler* readItem(pypa::AstExcept&);
AST_ExceptHandler* readItem(pypa::AstExceptPtr);

template <typename T> auto readItem(std::shared_ptr<T>& t) -> decltype(readItem(*t)) {
    if (t)
        return readItem(*t);
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
    return AST_TYPE::Unreachable;
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
    return AST_TYPE::Unreachable;
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
    return AST_TYPE::Unreachable;
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
    return AST_TYPE::Unreachable;
}

std::string readName(pypa::AstExpression& e) {
    assert(e.type == pypa::AstType::Name);
    return static_cast<pypa::AstName&>(e).id;
}

std::string readName(pypa::AstExpr& n) {
    if (!n) {
        return std::string();
    }
    return readName(*n);
}

AST_keyword* readItem(pypa::AstKeyword& k) {
    AST_keyword* ptr = new AST_keyword();
    location(ptr, k);
    ptr->arg = readName(k.name);
    ptr->value = readItem(k.value);
    return ptr;
}

void readVector(std::vector<AST_keyword*>& t, pypa::AstExprList& items) {
    for (auto& item : items) {
        assert(item->type == pypa::AstType::Keyword);
        t.push_back(readItem(static_cast<pypa::AstKeyword&>(*item)));
    }
}

template <typename T, typename U> void readVector(std::vector<T*>& t, std::vector<U>& u) {
    for (auto& item : u) {
        if (!item) {
            t.push_back(nullptr);
        } else {
            t.push_back(readItem(*item));
        }
    }
}

void readVector(std::vector<AST_expr*>& t, pypa::AstExpression& u) {
    if (u.type == pypa::AstType::Tuple) {
        pypa::AstTuple& e = static_cast<pypa::AstTuple&>(u);
        for (auto& item : e.elements) {
            assert(item);
            t.push_back(readItem(*item));
        }
    } else {
        t.push_back(readItem(u));
    }
}

void readVector(std::vector<AST_stmt*>& t, pypa::AstStatement& u) {
    if (u.type == pypa::AstType::Suite) {
        pypa::AstSuite& e = static_cast<pypa::AstSuite&>(u);
        for (auto& item : e.items) {
            assert(item);
            t.push_back(readItem(*item));
        }
    } else {
        t.push_back(readItem(u));
    }
}

AST_comprehension* readItem(pypa::AstComprehension& c) {
    AST_comprehension* ptr = new AST_comprehension();
    ptr->target = readItem(c.target);
    ptr->iter = readItem(c.iter);
    readVector(ptr->ifs, c.ifs);
    return ptr;
}

AST_comprehension* readItem(pypa::AstComprPtr c) {
    if (c)
        return readItem(*c);
    return nullptr;
}

void readVector(std::vector<AST_comprehension*>& t, pypa::AstExprList& u) {
    for (auto& e : u) {
        assert(e && e->type == pypa::AstType::Comprehension);
        t.push_back(readItem(static_cast<pypa::AstComprehension&>(*e)));
    }
}

void readVector(std::vector<AST_stmt*>& t, pypa::AstStmt u) {
    if (u) {
        readVector(t, *u);
    }
}

AST_ExceptHandler* readItem(pypa::AstExcept& e) {
    AST_ExceptHandler* ptr = new AST_ExceptHandler();
    location(ptr, e);
    readVector(ptr->body, e.body);
    ptr->name = readItem(e.name);
    ptr->type = readItem(e.type);
    return ptr;
}

AST_ExceptHandler* readItem(pypa::AstExceptPtr ptr) {
    assert(ptr);
    return readItem(*ptr);
}

AST_alias* readItem(pypa::AstAlias& a) {
    return new AST_alias(readName(a.name), readName(a.as_name));
}

AST_arguments* readItem(pypa::AstArguments& a) {
    AST_arguments* ptr = new AST_arguments();
    location(ptr, a);
    readVector(ptr->defaults, a.defaults);
    ptr->defaults.erase(std::remove(ptr->defaults.begin(), ptr->defaults.end(), nullptr), ptr->defaults.end());
    readVector(ptr->args, a.arguments);
    ptr->kwarg = readName(a.kwargs);
    ptr->vararg = readName(a.args);
    return ptr;
}

struct expr_dispatcher {
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
        fprintf(stderr, "Unhandled ast expression type caught: %d @%s\n", a.type, __PRETTY_FUNCTION__);
        return nullptr;
    }

    ResultPtr read(pypa::AstAttribute& a) {
        AST_Attribute* ptr = new AST_Attribute();
        location(ptr, a);
        ptr->value = readItem(a.value);
        ptr->attr = readName(a.attribute);
        ptr->ctx_type = readItem(a.context);
        return ptr;
    }

    ResultPtr read(pypa::AstBoolOp& b) {
        AST_BoolOp* ptr = new AST_BoolOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        readVector(ptr->values, b.values);
        return ptr;
    }

    ResultPtr read(pypa::AstBinOp& b) {
        AST_BinOp* ptr = new AST_BinOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        ptr->left = readItem(b.left);
        ptr->right = readItem(b.right);
        return ptr;
    }

    ResultPtr read(pypa::AstCall& c) {
        AST_Call* ptr = new AST_Call();
        location(ptr, c);
        readVector(ptr->args, c.arglist.arguments);
        readVector(ptr->keywords, c.arglist.keywords);
        ptr->func = readItem(c.function);
        ptr->starargs = readItem(c.arglist.args);
        ptr->kwargs = readItem(c.arglist.kwargs);
        return ptr;
    }

    ResultPtr read(pypa::AstCompare& c) {
        AST_Compare* ptr = new AST_Compare();
        location(ptr, c);
        ptr->left = readItem(c.left);
        ptr->ops.reserve(c.operators.size());
        for (auto op : c.operators) {
            ptr->ops.push_back(readItem(op));
        }
        readVector(ptr->comparators, c.comparators);
        return ptr;
    }

    ResultPtr read(pypa::AstComplex& c) {
        AST_Num* ptr = new AST_Num();
        ptr->num_type = AST_Num::COMPLEX;
        pypa::string_to_double(c.imag, ptr->n_float);
        return ptr;
    }

    ResultPtr read(pypa::AstComprehension& c) {
        assert("This should not be called" && false);
        return nullptr;
    }

    ResultPtr read(pypa::AstDict& d) {
        AST_Dict* ptr = new AST_Dict();
        location(ptr, d);
        readVector(ptr->keys, d.keys);
        readVector(ptr->values, d.values);
        return ptr;
    }

    ResultPtr read(pypa::AstDictComp& d) {
        AST_DictComp* ptr = new AST_DictComp();
        location(ptr, d);
        ptr->key = readItem(d.key);
        ptr->value = readItem(d.value);
        readVector(ptr->generators, d.generators);
        return ptr;
    }

    ResultPtr read(pypa::AstEllipsis& e) {
        AST_Ellipsis* ptr = new AST_Ellipsis();
        location(ptr, e);
        return ptr;
    }

    ResultPtr read(pypa::AstExtSlice& e) {
        AST_ExtSlice* ptr = new AST_ExtSlice();
        location(ptr, e);
        readVector(ptr->dims, e.dims);
        return ptr;
    }

    ResultPtr read(pypa::AstIfExpr& i) {
        AST_IfExp* ptr = new AST_IfExp();
        location(ptr, i);
        ptr->body = readItem(i.body);
        ptr->test = readItem(i.test);
        ptr->orelse = readItem(i.orelse);
        return ptr;
    }

    ResultPtr read(pypa::AstGenerator& g) {
        AST_GeneratorExp* ptr = new AST_GeneratorExp();
        location(ptr, g);
        ptr->elt = readItem(g.element);
        readVector(ptr->generators, g.generators);
        return ptr;
    }

    ResultPtr read(pypa::AstIndex& i) {
        AST_Index* ptr = new AST_Index();
        location(ptr, i);
        ptr->value = readItem(i.value);
        return ptr;
    }

    ResultPtr read(pypa::AstLambda& l) {
        AST_Lambda* ptr = new AST_Lambda();
        location(ptr, l);
        ptr->args = readItem(l.arguments);
        ptr->body = readItem(l.body);
        return ptr;
    }

    ResultPtr read(pypa::AstList& l) {
        AST_List* ptr = new AST_List();
        location(ptr, l);
        readVector(ptr->elts, l.elements);
        ptr->ctx_type = readItem(l.context);
        return ptr;
    }

    ResultPtr read(pypa::AstListComp& l) {
        AST_ListComp* ptr = new AST_ListComp();
        location(ptr, l);
        readVector(ptr->generators, l.generators);
        ptr->elt = readItem(l.element);
        return ptr;
    }

    ResultPtr read(pypa::AstName& a) {
        AST_Name* ptr = new AST_Name();
        location(ptr, a);
        ptr->ctx_type = readItem(a.context);
        ptr->id = a.id;
        return ptr;
    }

    ResultPtr read(pypa::AstNone& n) {
        AST_Name* ptr = new AST_Name();
        location(ptr, n);
        ptr->ctx_type = AST_TYPE::Load;
        ptr->id = "None";
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
        ptr->value = readItem(r.value);
        return ptr;
    }

    ResultPtr read(pypa::AstSet& s) {
        AST_Set* ptr = new AST_Set();
        location(ptr, s);
        readVector(ptr->elts, s.elements);
        return ptr;
    }

    ResultPtr read(pypa::AstSlice& s) {
        AST_Slice* ptr = new AST_Slice();
        location(ptr, s);
        ptr->lower = readItem(s.lower);
        ptr->upper = readItem(s.upper);
        ptr->step = readItem(s.step);
        return ptr;
    }

    ResultPtr read(pypa::AstStr& s) {
        AST_Str* ptr = new AST_Str();
        location(ptr, s);
        ptr->s = s.value;
        return ptr;
    }

    ResultPtr read(pypa::AstSubscript& s) {
        AST_Subscript* ptr = new AST_Subscript();
        location(ptr, s);
        ptr->value = readItem(s.value);
        ptr->ctx_type = readItem(s.context);
        ptr->slice = readItem(s.slice);
        return ptr;
    }

    ResultPtr read(pypa::AstTuple& t) {
        AST_Tuple* ptr = new AST_Tuple();
        location(ptr, t);
        readVector(ptr->elts, t.elements);
        ptr->ctx_type = readItem(t.context);
        return ptr;
    }

    ResultPtr read(pypa::AstUnaryOp& b) {
        AST_UnaryOp* ptr = new AST_UnaryOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        ptr->operand = readItem(b.operand);
        return ptr;
    }

    ResultPtr read(pypa::AstYieldExpr& e) {
        AST_Yield* ptr = new AST_Yield();
        location(ptr, e);
        ptr->value = readItem(e.args);
        return ptr;
    }
};

struct stmt_dispatcher {
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
        readVector(ptr->targets, a.targets);
        ptr->value = readItem(a.value);
        return ptr;
    }

    ResultPtr read(pypa::AstAssert& a) {
        AST_Assert* ptr = new AST_Assert();
        location(ptr, a);
        ptr->msg = readItem(a.expression);
        ptr->test = readItem(a.test);
        return ptr;
    }

    ResultPtr read(pypa::AstAugAssign& a) {
        AST_AugAssign* ptr = new AST_AugAssign();
        location(ptr, a);
        ptr->op_type = readItem(a.op);
        ptr->target = readItem(a.target);
        ptr->value = readItem(a.value);
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
            readVector(ptr->bases, *c.bases);
        readVector(ptr->decorator_list, c.decorators);
        readVector(ptr->body, c.body);
        ptr->name = readName(c.name);
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
        readVector(ptr->targets, *d.targets);
        return ptr;
    }

    ResultPtr read(pypa::AstExpressionStatement& e) {
        AST_Expr* ptr = new AST_Expr();
        location(ptr, e);
        ptr->value = readItem(e.expr);
        return ptr;
    }

    ResultPtr read(pypa::AstFor& f) {
        AST_For* ptr = new AST_For();
        location(ptr, f);
        ptr->target = readItem(f.target);
        if (f.iter)
            ptr->iter = readItem(f.iter);
        if (f.body)
            readVector(ptr->body, *f.body);
        if (f.orelse)
            readVector(ptr->orelse, *f.orelse);
        return ptr;
    }

    ResultPtr read(pypa::AstFunctionDef& f) {
        AST_FunctionDef* ptr = new AST_FunctionDef();
        location(ptr, f);
        readVector(ptr->decorator_list, f.decorators);
        ptr->name = readName(f.name);
        ptr->args = readItem(f.args);
        readVector(ptr->body, f.body);
        return ptr;
    }

    ResultPtr read(pypa::AstGlobal& g) {
        AST_Global* ptr = new AST_Global();
        location(ptr, g);
        ptr->names.resize(g.names.size());
        for (size_t i = 0; i < g.names.size(); ++i) {
            ptr->names[i] = readName(*g.names[i]);
        }
        return ptr;
    }

    ResultPtr read(pypa::AstIf& i) {
        AST_If* ptr = new AST_If();
        location(ptr, i);
        readVector(ptr->body, i.body);
        ptr->test = readItem(i.test);
        assert(ptr->test != 0);
        readVector(ptr->orelse, i.orelse);
        return ptr;
    }

    ResultPtr read(pypa::AstImport& i) {
        AST_Import* ptr = new AST_Import();
        location(ptr, i);
        if (i.names->type == pypa::AstType::Tuple) {
            for (auto& name : static_cast<pypa::AstTuple&>(*i.names).elements) {
                assert(name->type == pypa::AstType::Alias);
                ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*name)));
            }
        } else {
            assert(i.names->type == pypa::AstType::Alias);
            ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*i.names)));
        }
        return ptr;
    }

    ResultPtr read(pypa::AstImportFrom& i) {
        AST_ImportFrom* ptr = new AST_ImportFrom();
        location(ptr, i);
        ptr->module = readName(i.module);
        if (i.names->type == pypa::AstType::Tuple) {
            for (auto& name : static_cast<pypa::AstTuple&>(*i.names).elements) {
                assert(name->type == pypa::AstType::Alias);
                ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*name)));
            }
        } else {
            assert(i.names->type == pypa::AstType::Alias);
            ptr->names.push_back(readItem(static_cast<pypa::AstAlias&>(*i.names)));
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
        ptr->dest = readItem(p.destination);
        ptr->nl = p.newline;
        readVector(ptr->values, p.values);
        return ptr;
    }

    ResultPtr read(pypa::AstRaise& r) {
        AST_Raise* ptr = new AST_Raise();
        location(ptr, r);
        ptr->arg0 = readItem(r.arg0);
        ptr->arg1 = readItem(r.arg1);
        ptr->arg2 = readItem(r.arg2);
        return ptr;
    }

    ResultPtr read(pypa::AstSuite& s) { return nullptr; }

    ResultPtr read(pypa::AstReturn& r) {
        AST_Return* ptr = new AST_Return();
        location(ptr, r);
        ptr->value = readItem(r.value);
        return ptr;
    }

    ResultPtr read(pypa::AstTryExcept& t) {
        AST_TryExcept* ptr = new AST_TryExcept();
        location(ptr, t);
        readVector(ptr->body, t.body);
        readVector(ptr->orelse, t.orelse);
        readVector(ptr->handlers, t.handlers);
        return ptr;
    }

    ResultPtr read(pypa::AstTryFinally& t) {
        AST_TryFinally* ptr = new AST_TryFinally();
        location(ptr, t);
        readVector(ptr->body, t.body);
        readVector(ptr->finalbody, t.final_body);
        return ptr;
    }

    ResultPtr read(pypa::AstWith& w) {
        AST_With* ptr = new AST_With();
        location(ptr, w);
        ptr->optional_vars = readItem(w.optional);
        ptr->context_expr = readItem(w.context);
        readVector(ptr->body, w.body);
        return ptr;
    }

    ResultPtr read(pypa::AstWhile& w) {
        AST_While* ptr = new AST_While();
        location(ptr, w);
        ptr->test = readItem(w.test);
        readVector(ptr->body, w.body);
        readVector(ptr->orelse, w.orelse);
        return ptr;
    }

    ResultPtr read(pypa::AstYield& w) {
        AST_Expr* ptr = new AST_Expr();
        location(ptr, w);
        ptr->value = readItem(w.yield);
        return ptr;
    }

    ResultPtr read(pypa::AstDocString& d) {
        AST_Expr* ptr = new AST_Expr();
        location(ptr, d);
        AST_Str* str = new AST_Str();
        ptr->value = str;
        str->str_type = AST_Str::STR;
        str->s = d.doc;
        return ptr;
    }
};

AST_expr* readItem(pypa::AstExpression& e) {
    return pypa::visit<AST_expr*>(expr_dispatcher(), e);
}

AST_stmt* readItem(pypa::AstStatement& s) {
    return pypa::visit<AST_stmt*>(stmt_dispatcher(), s);
}

AST_Module* readModule(pypa::AstModule& t) {
    if (VERBOSITY("PYPA parsing") >= 2) {
        printf("PYPA reading module\n");
    }
    AST_Module* mod = new AST_Module();
    readVector(mod->body, t.body->items);
    return mod;
}

void pypaErrorHandler(pypa::Error e) {
    //    raiseSyntaxError
    //    void raiseSyntaxError(const char* msg, int lineno, int col_offset, const
    //    std::string& file, const std::string& func);
    if (e.type != pypa::ErrorType::SyntaxWarning) {
        raiseSyntaxError(e.message.c_str(), e.cur.line, e.cur.column, e.file_name, std::string());
    }
}

AST_Module* pypa_parse(char const* file_path) {
    pypa::Lexer lexer(file_path);
    pypa::SymbolTablePtr symbols;
    pypa::AstModulePtr module;
    pypa::ParserOptions options;

    options.printerrors = false;
    options.python3allowed = false;
    options.python3only = false;
    options.error_handler = pypaErrorHandler;

    if (pypa::parse(lexer, module, symbols, options) && module) {
        return readModule(*module);
    }
    return nullptr;
}
}
