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

#include "codegen/serialize_ast.h"

#include "llvm/Support/SwapByteOrder.h"

#include "core/ast.h"

namespace pyston {
namespace {

class SerializeASTVisitor : public ASTVisitor {
private:
    FILE* file;

public:
    static unsigned int write(AST_Module* module, FILE* file) {
        SerializeASTVisitor visitor(file);
        unsigned long start_pos = ftell(file);
        visitor.writeASTMisc(module);
        return ftell(file) - start_pos;
    }

private:
    SerializeASTVisitor(FILE* file) : file(file) {}
    virtual ~SerializeASTVisitor() {}

    void writeByte(uint8_t v) { fwrite(&v, 1, sizeof(v), file); }

    void writeShort(uint16_t v) {
        v = llvm::sys::getSwappedBytes(v); // TODO: assumes little endian machine
        fwrite(&v, 1, sizeof(v), file);
    }

    void writeUInt(uint32_t v) {
        v = llvm::sys::getSwappedBytes(v); // TODO: assumes little endian machine
        fwrite(&v, 1, sizeof(v), file);
    }

    void writeULL(uint64_t v) {
        v = llvm::sys::getSwappedBytes(v); // TODO: assumes little endian machine
        fwrite(&v, 1, sizeof(v), file);
    }

    void writeDouble(double v) {
        union {
            double v;
            uint64_t u;
        } u{.v = v };
        writeULL(u.u);
    }

    void writeString(const std::string& v) {
        writeShort(v.size());
        fwrite(v.c_str(), 1, v.size(), file);
    }

    void writeString(const InternedString v) { writeString(v.str()); }

    void writeStringVector(const std::vector<InternedString>& vec) {
        writeShort(vec.size());
        for (auto&& e : vec) {
            writeString(e);
        }
    }

    void writeExpr(AST_expr* e) {
        if (!e) {
            writeByte(0x00);
        } else {
            writeByte(e->type);
            writeByte(0xae); // check byte
            e->accept(this);
        }
    }

    void writeExprVector(const std::vector<AST_expr*>& vec) {
        writeShort(vec.size());
        for (auto* e : vec) {
            writeExpr(e);
        }
    }

    void writeStmt(AST_stmt* e) {
        writeByte(e->type);
        writeByte(0xae); // check byte
        e->accept(this);
    }

    void writeStmtVector(const std::vector<AST_stmt*>& vec) {
        writeShort(vec.size());
        for (auto* e : vec) {
            writeStmt(e);
        }
    }

    void writeColOffset(uint32_t v) {
        assert(v < 100000 || v == -1);
        writeULL(v == -1 ? 0 : v);
    }

    void writeLineno(uint64_t v) { writeULL(v); }

    void writeASTMisc(AST* e) {
        writeByte(e->type);
        writeByte(0xae); // check byte
        switch (e->type) {
            case AST_TYPE::alias:
            case AST_TYPE::arguments:
            case AST_TYPE::comprehension:
            case AST_TYPE::ExceptHandler:
            case AST_TYPE::keyword:
            case AST_TYPE::Module:
                return e->accept(this);
            default:
                assert(0);
        }
    }

    template <class T> void writeMiscVector(std::vector<T*>& vec) {
        writeShort(vec.size());
        for (auto&& e : vec) {
            writeASTMisc(e);
        }
    }



    virtual bool visit_alias(AST_alias* node) {
        writeString(node->asname);
        writeString(node->name);
        return true;
    }
    virtual bool visit_arguments(AST_arguments* node) {
        writeExprVector(node->args);
        writeExprVector(node->defaults);
        writeString(node->kwarg);
        writeString(node->vararg);
        return true;
    }
    virtual bool visit_assert(AST_Assert* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->msg);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_assign(AST_Assign* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExprVector(node->targets);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_augassign(AST_AugAssign* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExpr(node->target);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_attribute(AST_Attribute* node) {
        writeString(node->attr);
        writeColOffset(node->col_offset);
        writeByte(node->ctx_type);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_binop(AST_BinOp* node) {
        writeColOffset(node->col_offset);
        writeExpr(node->left);
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExpr(node->right);
        return true;
    }
    virtual bool visit_boolop(AST_BoolOp* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExprVector(node->values);
        return true;
    }
    virtual bool visit_break(AST_Break* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_call(AST_Call* node) {
        writeExprVector(node->args);
        writeColOffset(node->col_offset);
        writeExpr(node->func);
        writeMiscVector(node->keywords);
        writeExpr(node->kwargs);
        writeLineno(node->lineno);
        writeExpr(node->starargs);
        return true;
    }
    virtual bool visit_compare(AST_Compare* node) {
        writeColOffset(node->col_offset);
        writeExprVector(node->comparators);
        writeExpr(node->left);
        writeLineno(node->lineno);

        writeShort(node->comparators.size());
        for (auto& e : node->ops) {
            writeByte(e);
        }
        return true;
    }
    virtual bool visit_comprehension(AST_comprehension* node) {
        writeExprVector(node->ifs);
        writeExpr(node->iter);
        writeExpr(node->target);
        return true;
    }
    virtual bool visit_classdef(AST_ClassDef* node) {
        writeExprVector(node->bases);
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeExprVector(node->decorator_list);
        writeLineno(node->lineno);
        writeString(node->name);
        return true;
    }
    virtual bool visit_continue(AST_Continue* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_delete(AST_Delete* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExprVector(node->targets);
        return true;
    }
    virtual bool visit_dict(AST_Dict* node) {
        writeColOffset(node->col_offset);
        writeExprVector(node->keys);
        writeLineno(node->lineno);
        writeExprVector(node->values);
        return true;
    }
    virtual bool visit_dictcomp(AST_DictComp* node) {
        writeColOffset(node->col_offset);
        writeMiscVector(node->generators);
        writeExpr(node->key);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_excepthandler(AST_ExceptHandler* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->name);
        writeExpr(node->type);
        return true;
    }
    virtual bool visit_exec(AST_Exec* node) {
        writeExpr(node->body);
        writeColOffset(node->col_offset);
        writeExpr(node->globals);
        writeLineno(node->lineno);
        writeExpr(node->locals);
        return true;
    }
    virtual bool visit_expr(AST_Expr* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_extslice(AST_ExtSlice* node) {
        writeExprVector(node->dims);
        return true;
    }
    virtual bool visit_for(AST_For* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeExpr(node->iter);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        writeExpr(node->target);
        return true;
    }
    virtual bool visit_functiondef(AST_FunctionDef* node) {
        writeASTMisc(node->args);
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeExprVector(node->decorator_list);
        writeLineno(node->lineno);
        writeString(node->name);
        return true;
    }
    virtual bool visit_generatorexp(AST_GeneratorExp* node) {
        writeColOffset(node->col_offset);
        writeExpr(node->elt);
        writeMiscVector(node->generators);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_global(AST_Global* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeStringVector(node->names);
        return true;
    }
    virtual bool visit_if(AST_If* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_ifexp(AST_IfExp* node) {
        writeExpr(node->body);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->orelse);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_import(AST_Import* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeMiscVector(node->names);
        return true;
    }
    virtual bool visit_importfrom(AST_ImportFrom* node) {
        writeColOffset(node->col_offset);
        writeULL(node->level);
        writeLineno(node->lineno);
        writeString(node->module);
        writeMiscVector(node->names);
        return true;
    }
    virtual bool visit_index(AST_Index* node) {
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_keyword(AST_keyword* node) {
        writeString(node->arg);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_lambda(AST_Lambda* node) {
        writeASTMisc(node->args);
        writeExpr(node->body);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_list(AST_List* node) {
        writeColOffset(node->col_offset);
        writeByte(node->ctx_type);
        writeExprVector(node->elts);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_listcomp(AST_ListComp* node) {
        writeColOffset(node->col_offset);
        writeExpr(node->elt);
        writeMiscVector(node->generators);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_module(AST_Module* node) {
        writeStmtVector(node->body);
        return true;
    }
    virtual bool visit_name(AST_Name* node) {
        writeColOffset(node->col_offset);
        writeByte(node->ctx_type);
        writeString(node->id);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_num(AST_Num* node) {
        writeByte(node->num_type);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        if (node->num_type == AST_Num::INT) {
            writeULL(node->n_int);
        } else if (node->num_type == AST_Num::LONG) {
            writeString(node->n_long);
        } else if (node->num_type == AST_Num::FLOAT) {
            writeDouble(node->n_float);
        } else if (node->num_type == AST_Num::COMPLEX) {
            writeDouble(node->n_float);
        } else {
            RELEASE_ASSERT(0, "%d", node->num_type);
        }
        return true;
    }
    virtual bool visit_pass(AST_Pass* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_print(AST_Print* node) {
        writeColOffset(node->col_offset);
        writeExpr(node->dest);
        writeLineno(node->lineno);
        writeByte(node->nl);
        writeExprVector(node->values);
        return true;
    }
    virtual bool visit_raise(AST_Raise* node) {
        // "arg0" "arg1" "arg2" are called "type", "inst", and "tback" in the python ast,
        // so that's the order we have to write them:
        writeColOffset(node->col_offset);
        writeExpr(node->arg1 /*inst*/);
        writeLineno(node->lineno);
        writeExpr(node->arg2 /*tback*/);
        writeExpr(node->arg0 /*type*/);
        return true;
    }
    virtual bool visit_repr(AST_Repr* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_return(AST_Return* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_set(AST_Set* node) {
        writeColOffset(node->col_offset);
        writeExprVector(node->elts);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_setcomp(AST_SetComp* node) {
        writeColOffset(node->col_offset);
        writeExpr(node->elt);
        writeMiscVector(node->generators);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_slice(AST_Slice* node) {
        writeExpr(node->lower);
        writeExpr(node->step);
        writeExpr(node->upper);
        return true;
    }
    virtual bool visit_str(AST_Str* node) {
        writeByte(node->str_type);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        if (node->str_type == AST_Str::STR) {
            writeString(node->str_data);
        } else if (node->str_type == AST_Str::UNICODE) {
            writeString(node->str_data);
        } else {
            RELEASE_ASSERT(0, "%d", node->str_type);
        }
        return true;
    }
    virtual bool visit_subscript(AST_Subscript* node) {
        writeColOffset(node->col_offset);
        writeByte(node->ctx_type);
        writeLineno(node->lineno);
        writeExpr(node->slice);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_tryexcept(AST_TryExcept* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeMiscVector(node->handlers);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        return true;
    }
    virtual bool visit_tryfinally(AST_TryFinally* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeStmtVector(node->finalbody);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_tuple(AST_Tuple* node) {
        writeColOffset(node->col_offset);
        writeByte(node->ctx_type);
        writeExprVector(node->elts);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_unaryop(AST_UnaryOp* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExpr(node->operand);
        return true;
    }
    virtual bool visit_while(AST_While* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_with(AST_With* node) {
        writeStmtVector(node->body);
        writeColOffset(node->col_offset);
        writeExpr(node->context_expr);
        writeLineno(node->lineno);
        writeExpr(node->optional_vars);
        return true;
    }
    virtual bool visit_yield(AST_Yield* node) {
        writeColOffset(node->col_offset);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
};
}

unsigned long serializeAST(AST_Module* module, FILE* file) {
    return SerializeASTVisitor::write(module, file);
}
}
