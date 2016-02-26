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

#include "codegen/cpython_ast.h"

#include "llvm/ADT/STLExtras.h"

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

#if 0
#Helpful script:
with open("Include/Python-ast.h") as f:
    lines = f.readlines()

#type, lines = "stmt", lines[72 : 181]
#type, lines = "expr", lines[196 : 309]
type, lines = "slice", lines[319:332]

names = []
while lines:
    l = lines.pop(0).strip()
    if l.startswith("struct {"):
        continue
    elif l.startswith("}"):
        name = l[2:-1]
        print "case %s_kind: {" % name
        print "auto r = new AST_%s();" % name
        print "auto v = %s->v.%s;" % (type, name)
        for n in names:
            print "r->%s = convert(v.%s);" % (n, n)
        print "return r;"
        print "}"
        names = []
        continue
    elif l.endswith(';'):
        n = l.split()[1][:-1]
        if n.startswith('*'):
            n = n[1:]
        names.append(n)
#endif

class Converter {
private:
    InternedStringPool* pool = NULL;
    int loop_depth = 0;
    int in_finally = 0;
    llvm::StringRef fn;

public:
    Converter(llvm::StringRef fn) : fn(fn) {}

    template <typename T, typename P> std::vector<P> convert(asdl_seq* seq) {
        std::vector<P> rtn;
        if (!seq)
            return rtn;
        for (int i = 0; i < seq->size; i++) {
            rtn.push_back(convert((T)seq->elements[i]));
        }
        return rtn;
    }

    template <typename T, typename P> std::vector<P> convert(asdl_int_seq* seq) {
        std::vector<P> rtn;
        if (!seq)
            return rtn;
        for (int i = 0; i < seq->size; i++) {
            rtn.push_back(convert((T)seq->elements[i]));
        }
        return rtn;
    }

    template <typename T, typename P> void convertAll(asdl_seq* seq, std::vector<P>& vec) { vec = convert<T, P>(seq); }

    InternedString convert(identifier ident) {
        assert(pool);
        if (!ident)
            return pool->get("");
        return pool->get(static_cast<BoxedString*>(ident)->s());
    }

    AST_arguments* convert(arguments_ty ident) {
        auto r = new AST_arguments();

        convertAll<expr_ty>(ident->args, r->args);
        convertAll<expr_ty>(ident->defaults, r->defaults);
        r->vararg = convert(ident->vararg);
        r->kwarg = convert(ident->kwarg);
        return r;
    }

#define CASE(N)                                                                                                        \
    case N:                                                                                                            \
        return AST_TYPE::N

    AST_TYPE::AST_TYPE convert(expr_context_ty context) {
        switch (context) {
            CASE(Load);
            CASE(Store);
            CASE(Del);
            CASE(Param);
            default:
                RELEASE_ASSERT(0, "unhandled context type: %d", context);
        }
    }

    AST_TYPE::AST_TYPE convert(operator_ty op) {
        switch (op) {
            CASE(Add);
            CASE(Sub);
            CASE(Mult);
            CASE(Div);
            CASE(Mod);
            CASE(Pow);
            CASE(LShift);
            CASE(RShift);
            CASE(BitOr);
            CASE(BitXor);
            CASE(BitAnd);
            CASE(FloorDiv);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }

    AST_TYPE::AST_TYPE convert(boolop_ty op) {
        switch (op) {
            CASE(Add);
            CASE(Or);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }

    AST_TYPE::AST_TYPE convert(unaryop_ty op) {
        switch (op) {
            CASE(Invert);
            CASE(Not);
            CASE(UAdd);
            CASE(USub);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }

    AST_TYPE::AST_TYPE convert(cmpop_ty op) {
        switch (op) {
            CASE(Eq);
            CASE(NotEq);
            CASE(Lt);
            CASE(LtE);
            CASE(Gt);
            CASE(GtE);
            CASE(Is);
            CASE(IsNot);
            CASE(In);
            CASE(NotIn);
        }
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid operator: %d", op);
    }
#undef CASE

    AST_keyword* convert(keyword_ty keyword) {
        auto r = new AST_keyword();
        r->arg = convert(keyword->arg);
        r->value = convert(keyword->value);
        return r;
    }

    AST_comprehension* convert(comprehension_ty comprehension) {
        auto r = new AST_comprehension();
        r->target = convert(comprehension->target);
        r->iter = convert(comprehension->iter);
        r->ifs = convert<expr_ty, AST_expr*>(comprehension->ifs);
        return r;
    }

    AST_slice* convert(slice_ty slice) {
        switch (slice->kind) {
            case Slice_kind: {
                auto r = new AST_Slice();
                auto v = slice->v.Slice;
                r->lower = convert(v.lower);
                r->upper = convert(v.upper);
                r->step = convert(v.step);
                return r;
            }
            case ExtSlice_kind: {
                auto r = new AST_ExtSlice();
                auto v = slice->v.ExtSlice;
                r->dims = convert<slice_ty, AST_slice*>(v.dims);
                return r;
            }
            case Index_kind: {
                auto r = new AST_Index();
                auto v = slice->v.Index;
                r->value = convert(v.value);
                return r;
            }
            case Ellipsis_kind:
                return new AST_Ellipsis();
        }
        RELEASE_ASSERT(0, "invalid slice type: %d", slice->kind);
    }

    AST_expr* _convert(expr_ty expr) {
        switch (expr->kind) {
            case BoolOp_kind: {
                auto r = new AST_BoolOp();
                auto v = expr->v.BoolOp;
                r->op_type = convert(v.op);
                r->values = convert<expr_ty, AST_expr*>(v.values);
                return r;
            }
            case BinOp_kind: {
                auto r = new AST_BinOp();
                auto v = expr->v.BinOp;
                r->left = convert(v.left);
                r->op_type = convert(v.op);
                r->right = convert(v.right);
                return r;
            }
            case UnaryOp_kind: {
                auto r = new AST_UnaryOp();
                auto v = expr->v.UnaryOp;
                r->op_type = convert(v.op);
                r->operand = convert(v.operand);
                return r;
            }
            case Lambda_kind: {
                auto r = new AST_Lambda();
                auto v = expr->v.Lambda;
                r->args = convert(v.args);
                r->body = convert(v.body);
                return r;
            }
            case IfExp_kind: {
                auto r = new AST_IfExp();
                auto v = expr->v.IfExp;
                r->test = convert(v.test);
                r->body = convert(v.body);
                r->orelse = convert(v.orelse);
                return r;
            }
            case Dict_kind: {
                auto r = new AST_Dict();
                auto v = expr->v.Dict;
                r->keys = convert<expr_ty, AST_expr*>(v.keys);
                r->values = convert<expr_ty, AST_expr*>(v.values);
                return r;
            }
            case Set_kind: {
                auto r = new AST_Set();
                auto v = expr->v.Set;
                r->elts = convert<expr_ty, AST_expr*>(v.elts);
                return r;
            }
            case ListComp_kind: {
                auto r = new AST_ListComp();
                auto v = expr->v.ListComp;
                r->elt = convert(v.elt);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case SetComp_kind: {
                auto r = new AST_SetComp();
                auto v = expr->v.SetComp;
                r->elt = convert(v.elt);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case DictComp_kind: {
                auto r = new AST_DictComp();
                auto v = expr->v.DictComp;
                r->key = convert(v.key);
                r->value = convert(v.value);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case GeneratorExp_kind: {
                auto r = new AST_GeneratorExp();
                auto v = expr->v.GeneratorExp;
                r->elt = convert(v.elt);
                r->generators = convert<comprehension_ty, AST_comprehension*>(v.generators);
                return r;
            }
            case Yield_kind: {
                auto r = new AST_Yield();
                auto v = expr->v.Yield;
                r->value = convert(v.value);
                return r;
            }
            case Compare_kind: {
                auto r = new AST_Compare();
                auto v = expr->v.Compare;
                r->left = convert(v.left);
                r->ops = convert<cmpop_ty, AST_TYPE::AST_TYPE>(v.ops);
                r->comparators = convert<expr_ty, AST_expr*>(v.comparators);
                return r;
            }
            case Call_kind: {
                auto r = new AST_Call();
                auto v = expr->v.Call;
                r->func = convert(v.func);
                r->args = convert<expr_ty, AST_expr*>(v.args);
                r->keywords = convert<keyword_ty, AST_keyword*>(v.keywords);
                r->starargs = convert(v.starargs);
                r->kwargs = convert(v.kwargs);
                return r;
            }
            case Repr_kind: {
                auto r = new AST_Repr();
                auto v = expr->v.Repr;
                r->value = convert(v.value);
                return r;
            }
            case Attribute_kind: {
                auto r = new AST_Attribute();
                auto v = expr->v.Attribute;
                r->value = convert(v.value);
                r->attr = convert(v.attr);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Subscript_kind: {
                auto r = new AST_Subscript();
                auto v = expr->v.Subscript;
                r->value = convert(v.value);
                r->slice = convert(v.slice);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Name_kind: {
                auto v = expr->v.Name;
                auto r = new AST_Name(convert(v.id), convert(v.ctx), 0);
                return r;
            }
            case List_kind: {
                auto r = new AST_List();
                auto v = expr->v.List;
                r->elts = convert<expr_ty, AST_expr*>(v.elts);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Tuple_kind: {
                auto r = new AST_Tuple();
                auto v = expr->v.Tuple;
                r->elts = convert<expr_ty, AST_expr*>(v.elts);
                r->ctx_type = convert(v.ctx);
                return r;
            }
            case Num_kind: {
                PyObject* o = expr->v.Num.n;
                if (o->cls == int_cls) {
                    auto r = new AST_Num();
                    r->num_type = AST_Num::INT;
                    r->n_int = unboxInt(o);
                    return r;
                }
                if (o->cls == float_cls) {
                    auto r = new AST_Num();
                    r->num_type = AST_Num::FLOAT;
                    r->n_float = unboxFloat(o);
                    return r;
                }
                if (o->cls == long_cls) {
                    auto r = new AST_Num();
                    r->num_type = AST_Num::LONG;
                    // XXX This is pretty silly:
                    auto s = _PyLong_Format(o, 10, 0, 0);
                    RELEASE_ASSERT(s, "");
                    r->n_long = PyString_AsString(s);
                    return r;
                }
                if (o->cls == complex_cls) {
                    auto r = new AST_Num();
                    r->num_type = AST_Num::COMPLEX;

                    double real = PyComplex_RealAsDouble(o);
                    double imag = PyComplex_ImagAsDouble(o);
                    RELEASE_ASSERT(real != -1.0 || !PyErr_Occurred(), "");
                    RELEASE_ASSERT(imag != -1.0 || !PyErr_Occurred(), "");

                    r->n_float = imag;

                    if (real == 0.0)
                        return r;

                    // TODO very silly:
                    auto freal = new AST_Num();
                    freal->n_float = real;
                    freal->num_type = AST_Num::FLOAT;

                    auto binop = new AST_BinOp();
                    binop->op_type = AST_TYPE::Add;
                    binop->left = freal;
                    binop->right = r;

                    return binop;
                }
                RELEASE_ASSERT(0, "unhandled num type: %s\n", o->cls->tp_name);
            }
            case Str_kind: {
                PyObject* o = expr->v.Str.s;
                if (o->cls == unicode_cls) {
                    o = PyUnicode_AsUTF8String(o);
                    RELEASE_ASSERT(o, "");

                    auto r = new AST_Str();
                    r->str_data = static_cast<BoxedString*>(o)->s();
                    r->str_type = AST_Str::UNICODE;
                    return r;
                }

                if (o->cls == str_cls) {
                    return new AST_Str(static_cast<BoxedString*>(o)->s());
                }
                RELEASE_ASSERT(0, "unhandled str type: %s\n", o->cls->tp_name);
            }
            default:
                RELEASE_ASSERT(0, "unhandled kind: %d\n", expr->kind);
        };
        Py_FatalError("unimplemented");
    }

    AST_expr* convert(expr_ty expr) {
        if (!expr)
            return NULL;

        auto r = _convert(expr);
        r->lineno = expr->lineno;
        r->col_offset = expr->col_offset;
        return r;
    }

    AST_ExceptHandler* convert(excepthandler_ty eh) {
        assert(eh->kind == ExceptHandler_kind);

        auto r = new AST_ExceptHandler();
        auto v = eh->v.ExceptHandler;
        r->type = convert(v.type);
        r->name = convert(v.name);
        r->body = convert<stmt_ty, AST_stmt*>(v.body);
        return r;
    }

    AST_alias* convert(alias_ty alias) { return new AST_alias(convert(alias->name), convert(alias->asname)); }

    AST_stmt* _convert(stmt_ty stmt) {
        switch (stmt->kind) {
            case FunctionDef_kind: {
                auto r = new AST_FunctionDef();
                auto v = stmt->v.FunctionDef;
                r->name = convert(v.name);
                r->args = convert(v.args);
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                r->decorator_list = convert<expr_ty, AST_expr*>(v.decorator_list);
                return r;
            }
            case ClassDef_kind: {
                auto r = new AST_ClassDef();
                auto v = stmt->v.ClassDef;
                r->name = convert(v.name);
                r->bases = convert<expr_ty, AST_expr*>(v.bases);
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                r->decorator_list = convert<expr_ty, AST_expr*>(v.decorator_list);
                return r;
            }
            case Return_kind: {
                auto r = new AST_Return();
                auto v = stmt->v.Return;
                r->value = convert(v.value);
                return r;
            }
            case Delete_kind: {
                auto r = new AST_Delete();
                auto v = stmt->v.Delete;
                r->targets = convert<expr_ty, AST_expr*>(v.targets);
                return r;
            }
            case Assign_kind: {
                auto r = new AST_Assign();
                auto v = stmt->v.Assign;
                r->targets = convert<expr_ty, AST_expr*>(v.targets);
                r->value = convert(v.value);
                return r;
            }
            case AugAssign_kind: {
                auto r = new AST_AugAssign();
                auto v = stmt->v.AugAssign;
                r->target = convert(v.target);
                r->op_type = convert(v.op);
                r->value = convert(v.value);
                return r;
            }
            case Print_kind: {
                auto r = new AST_Print();
                auto v = stmt->v.Print;
                r->dest = convert(v.dest);
                r->values = convert<expr_ty, AST_expr*>(v.values);
                r->nl = v.nl;
                return r;
            }
            case For_kind: {
                auto r = new AST_For();
                auto v = stmt->v.For;
                r->target = convert(v.target);
                r->iter = convert(v.iter);
                auto fin = in_finally;
                in_finally = 0;
                loop_depth++;
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                loop_depth--;
                in_finally = fin;
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case While_kind: {
                auto r = new AST_While();
                auto v = stmt->v.While;
                r->test = convert(v.test);
                auto fin = in_finally;
                in_finally = 0;
                loop_depth++;
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                loop_depth--;
                in_finally = fin;
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case If_kind: {
                auto r = new AST_If();
                auto v = stmt->v.If;
                r->test = convert(v.test);
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case With_kind: {
                auto r = new AST_With();
                auto v = stmt->v.With;
                r->context_expr = convert(v.context_expr);
                r->optional_vars = convert(v.optional_vars);
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                return r;
            }
            case Raise_kind: {
                auto r = new AST_Raise();
                auto v = stmt->v.Raise;
                r->arg0 = convert(v.type);
                r->arg1 = convert(v.inst);
                r->arg2 = convert(v.tback);
                return r;
            }
            case TryExcept_kind: {
                auto r = new AST_TryExcept();
                auto v = stmt->v.TryExcept;
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                r->handlers = convert<excepthandler_ty, AST_ExceptHandler*>(v.handlers);
                r->orelse = convert<stmt_ty, AST_stmt*>(v.orelse);
                return r;
            }
            case TryFinally_kind: {
                auto r = new AST_TryFinally();
                auto v = stmt->v.TryFinally;
                r->body = convert<stmt_ty, AST_stmt*>(v.body);
                in_finally++;
                r->finalbody = convert<stmt_ty, AST_stmt*>(v.finalbody);
                in_finally--;
                return r;
            }
            case Assert_kind: {
                auto r = new AST_Assert();
                auto v = stmt->v.Assert;
                r->test = convert(v.test);
                r->msg = convert(v.msg);
                return r;
            }
            case Import_kind: {
                auto r = new AST_Import();
                auto v = stmt->v.Import;
                r->names = convert<alias_ty, AST_alias*>(v.names);
                return r;
            }
            case ImportFrom_kind: {
                auto r = new AST_ImportFrom();
                auto v = stmt->v.ImportFrom;
                r->module = convert(v.module);
                r->names = convert<alias_ty, AST_alias*>(v.names);
                r->level = v.level;
                return r;
            }
            case Exec_kind: {
                auto r = new AST_Exec();
                auto v = stmt->v.Exec;
                r->body = convert(v.body);
                r->globals = convert(v.globals);
                r->locals = convert(v.locals);
                return r;
            }
            case Global_kind: {
                auto r = new AST_Global();
                auto v = stmt->v.Global;
                r->names = convert<identifier, InternedString>(v.names);
                return r;
            }
            case Expr_kind: {
                auto r = new AST_Expr();
                auto v = stmt->v.Expr;
                r->value = convert(v.value);
                return r;
            }
            case Pass_kind:
                return new AST_Pass();
            case Break_kind:
                // This is not really the right place to be handling this, but this whole thing is temporary anyway.
                if (loop_depth == 0)
                    raiseSyntaxError("'break' outside loop", stmt->lineno, stmt->col_offset, fn, "", true);
                return new AST_Break();
            case Continue_kind:
                if (loop_depth == 0)
                    raiseSyntaxError("'continue' not properly in loop", stmt->lineno, stmt->col_offset, fn, "", true);
                if (in_finally)
                    raiseSyntaxError("'continue' not supported inside 'finally' clause", stmt->lineno, stmt->col_offset,
                                     fn, "", true);
                return new AST_Continue();
        };
        // GCC wants this:
        RELEASE_ASSERT(0, "invalid statement type: %d", stmt->kind);
    }

    AST_stmt* convert(stmt_ty stmt) {
        auto r = _convert(stmt);
        r->lineno = stmt->lineno;
        r->col_offset = stmt->col_offset;
        return r;
    }

    AST* convert(mod_ty mod) {
        switch (mod->kind) {
            case Module_kind: {
                AST_Module* rtn = new AST_Module(llvm::make_unique<InternedStringPool>());
                assert(!this->pool);
                this->pool = rtn->interned_strings.get();
                convertAll<stmt_ty>(mod->v.Module.body, rtn->body);
                return rtn;
            }
            case Interactive_kind: {
                AST_Module* rtn = new AST_Module(llvm::make_unique<InternedStringPool>());
                assert(!this->pool);
                this->pool = rtn->interned_strings.get();
                convertAll<stmt_ty>(mod->v.Interactive.body, rtn->body);
                makeModuleInteractive(rtn);
                return rtn;
            }
            case Expression_kind: {
                AST_Expression* rtn = new AST_Expression(llvm::make_unique<InternedStringPool>());
                this->pool = rtn->interned_strings.get();
                rtn->body = this->convert(mod->v.Expression.body);
                return rtn;
            }
            default:
                RELEASE_ASSERT(0, "unhandled kind: %d\n", mod->kind);
        }
    }
};

AST* cpythonToPystonAST(mod_ty mod, llvm::StringRef fn) {
    Converter c(fn);
    return c.convert(mod);
}
}
