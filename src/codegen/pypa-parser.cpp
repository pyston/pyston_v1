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

#include <pypa/parser/parser.hh>
#include <pypa/ast/visitor.hh>
#include "codegen/pypa-parser.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <sys/stat.h>

#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"

namespace pyston {

AST_expr* readItem(pypa::AstExpression& e);
AST_stmt* readItem(pypa::AstStatement& s);

template <typename T> auto readItem(std::shared_ptr<T>& t) -> decltype(readItem(*t)) {
    if (t)
        return readItem(*t);
    return nullptr;
}

AST_TYPE::AST_TYPE readItem(pypa::AstBoolOpType type) {
    switch(type) {
    case pypa::AstBoolOpType::And:  return AST_TYPE::And;
    case pypa::AstBoolOpType::Or:   return AST_TYPE::Or;
    default: break;
    }
    assert("Unknown AstBoolOpType" && false);
    return AST_TYPE::Unreachable;
}

AST_TYPE::AST_TYPE readItem(pypa::AstBinOpType type) {
    switch(type) {
    case pypa::AstBinOpType::Add:           return AST_TYPE::Add;
    case pypa::AstBinOpType::BitAnd:        return AST_TYPE::BitAnd;
    case pypa::AstBinOpType::BitOr:         return AST_TYPE::BitOr;
    case pypa::AstBinOpType::BitXor:        return AST_TYPE::BitXor;
    case pypa::AstBinOpType::Div:           return AST_TYPE::Div;
    case pypa::AstBinOpType::FloorDiv:      return AST_TYPE::FloorDiv;
    case pypa::AstBinOpType::LeftShift:     return AST_TYPE::LShift;
    case pypa::AstBinOpType::Mod:           return AST_TYPE::Mod;
    case pypa::AstBinOpType::Mult:          return AST_TYPE::Mult;
    case pypa::AstBinOpType::Power:         return AST_TYPE::Pow;
    case pypa::AstBinOpType::RightShift:    return AST_TYPE::RShift;
    case pypa::AstBinOpType::Sub:           return AST_TYPE::Sub;
    default: break;
    }
    assert("Unknown AstBinOpType" && false);
    return AST_TYPE::Unreachable;
}

AST_TYPE::AST_TYPE readItem(pypa::AstUnaryOpType type) {
    switch(type) {
    case pypa::AstUnaryOpType::Add:     return AST_TYPE::UAdd;
    case pypa::AstUnaryOpType::Invert:  return AST_TYPE::Invert;
    case pypa::AstUnaryOpType::Not:     return AST_TYPE::Not;
    case pypa::AstUnaryOpType::Sub:     return AST_TYPE::USub;
    default: break;
    }
    assert("Unknown AstUnaryOpType" && false);
    return AST_TYPE::Unreachable;
}


AST_TYPE::AST_TYPE readItem(pypa::AstContext ctx) {
    switch (ctx) {
        case pypa::AstContext::Load:        return AST_TYPE::Load;
        case pypa::AstContext::Store:       return AST_TYPE::Store;
        case pypa::AstContext::AugLoad:     return AST_TYPE::Load;
        case pypa::AstContext::AugStore:    return AST_TYPE::Store;
        case pypa::AstContext::Param:       return AST_TYPE::Param;
        case pypa::AstContext::Del:         return AST_TYPE::Del;
        default: break;
    }
    assert("Unknown AstContext" && false);
    return AST_TYPE::Load;
}

AST_TYPE::AST_TYPE readItem(pypa::AstCompareOpType type) {
    switch(type) {
    case pypa::AstCompareOpType::Equals:    return AST_TYPE::Eq;
    case pypa::AstCompareOpType::In:        return AST_TYPE::In;
    case pypa::AstCompareOpType::Is:        return AST_TYPE::Is;
    case pypa::AstCompareOpType::IsNot:     return AST_TYPE::IsNot;
    case pypa::AstCompareOpType::Less:      return AST_TYPE::Lt;
    case pypa::AstCompareOpType::LessEqual: return AST_TYPE::LtE;
    case pypa::AstCompareOpType::More:      return AST_TYPE::Gt;
    case pypa::AstCompareOpType::MoreEqual: return AST_TYPE::GtE;
    case pypa::AstCompareOpType::NotEqual:  return AST_TYPE::NotEq;
    case pypa::AstCompareOpType::NotIn:     return AST_TYPE::NotIn;
    default: break;
    }
    assert("Unknown AstCompareOpType" && false);
    return AST_TYPE::Unreachable;
}

std::string readName(pypa::AstExpression& e) {
    assert(e.type == pypa::AstType::Name);
    return static_cast<pypa::AstName&>(e).id;
}

std::string readName(pypa::AstExpr& n) {
    assert(n);
    return readName(*n);
}

AST_keyword* readItem(pypa::AstKeyword& k) {
    AST_keyword* ptr = new AST_keyword();
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
        assert(item);
        t.push_back(readItem(*item));
    }
}

void readVector(std::vector<AST_expr*>& t, pypa::AstExpression& u) {
    if (u.type == pypa::AstType::Expressions) {
        pypa::AstExpressions& e = static_cast<pypa::AstExpressions&>(u);
        for (auto& item : e.items) {
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

void location(AST* t, pypa::Ast& a) {
    t->lineno = a.line;
    t->col_offset = a.column;
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

    ResultPtr read(pypa::AstUnaryOp & b) {
        AST_UnaryOp* ptr = new AST_UnaryOp();
        location(ptr, b);
        ptr->op_type = readItem(b.op);
        ptr->operand = readItem(b.operand);
        return ptr;
    }

    ResultPtr read(pypa::AstCompare & c) {
        AST_Compare * ptr = new AST_Compare();
        ptr->left = readItem(c.left) ;
        pypa::AstExpr r = c.right;
        ptr->ops.push_back(readItem(c.op));
        while(r && r->type == pypa::AstType::Compare) {
            pypa::AstCompare & c2 = static_cast<pypa::AstCompare&>(*r);
            ptr->comparators.push_back(readItem(c2.left));
            ptr->ops.push_back(readItem(c2.op));
            r = c2.right;
        }
        ptr->comparators.push_back(readItem(*r));
        return ptr;
    }


    ResultPtr read(pypa::AstExpressions& a) {
        assert(a.items.empty() || a.items.size() == 1);
        if (!a.items.empty()) {
            return readItem(a.items.front());
        }
        return nullptr;
    }

    ResultPtr read(pypa::AstStr& s) {
        AST_Str* ptr = new AST_Str();
        ptr->s = s.value;
        return ptr;
    }

    ResultPtr read(pypa::AstName& a) {
        AST_Name* ptr = new AST_Name();
        ptr->ctx_type = readItem(a.context);
        ptr->id = a.id;
        return ptr;
    }

    ResultPtr read(pypa::AstAttribute& a) {
        AST_Attribute* ptr = new AST_Attribute();
        ptr->value = readItem(a.value);
        ptr->attr = readName(a.attribute);
        ptr->ctx_type = readItem(a.context);
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

    ResultPtr read(pypa::AstNumber& c) {
        AST_Num* ptr = new AST_Num();
        ptr->num_type = c.num_type == pypa::AstNumber::Float ? AST_Num::FLOAT : AST_Num::INT;
        if(ptr->num_type == AST_Num::INT)
            ptr->n_int = c.integer;
        else
            ptr->n_float = c.floating;
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
        readVector(ptr->targets, *a.targets);
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

    ResultPtr read(pypa::AstBreak& b) {
        AST_Break* ptr = new AST_Break();
        location(ptr, b);
        return ptr;
    }

    ResultPtr read(pypa::AstFor & f) {
        AST_For * ptr = new AST_For();
        ptr->target = readItem(f.target);
        ptr->iter = readItem(f.iter);
        if(f.body) readVector(ptr->body, *f.body);
        if(f.orelse) readVector(ptr->orelse, *f.orelse);
        return ptr;
    }

    ResultPtr read(pypa::AstWith& w) {
        AST_With* ptr = new AST_With();
        location(ptr, w);
        ptr->optional_vars = readItem(w.optional);
        ptr->context_expr = readItem(w.context);
        if (w.body->type != pypa::AstType::Suite) {
            ptr->body.push_back(readItem(w.body));
        } else {
            readVector(ptr->body, static_cast<pypa::AstSuite&>(*w.body).items);
        }
        return ptr;
    }

    ResultPtr read(pypa::AstAugAssign & a) {
        AST_AugAssign * ptr = new AST_AugAssign();
        location(ptr, a);
        ptr->op_type = readItem(a.op);
        ptr->target = readItem(a.target);
        ptr->value = readItem(a.value);
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

AST_Module* pypa_parse(char const* file_path) {
    pypa::Lexer lexer(file_path);
    pypa::SymbolTablePtr symbols;
    pypa::AstModulePtr module;
    if (pypa::parse(lexer, module, symbols) && module) {
        return readModule(*module);
    }
    return nullptr;
}
}
