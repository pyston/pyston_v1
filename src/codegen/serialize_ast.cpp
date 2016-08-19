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

#include "codegen/serialize_ast.h"

#include "llvm/Support/SwapByteOrder.h"

#include "core/ast.h"

namespace pyston {
namespace {

class SerializeASTVisitor : public ASTVisitor {
private:
    FILE* file;
    uint8_t checksum;

public:
    static std::pair<unsigned int, uint8_t> write(AST_Module* module, FILE* file) {
        SerializeASTVisitor visitor(file);
        unsigned long start_pos = ftell(file);
        visitor.writeASTMisc(module);
        return std::make_pair(ftell(file) - start_pos, visitor.checksum);
    }

private:
    SerializeASTVisitor(FILE* file) : file(file), checksum(0) {}
    virtual ~SerializeASTVisitor() {}

    void writeByte(uint64_t v) {
        assert(v < 256);

        uint8_t b = (uint8_t)v;
        fwrite(&b, 1, 1, file);
        checksum ^= b;
    }

    void writeShort(uint64_t v) {
        RELEASE_ASSERT(v < (1 << 16), "");
        // I guess we use big-endian:
        for (int i = 1; i >= 0; i--) {
            writeByte((v >> (i * 8)) & 0xff);
        }
    }

    void writeUInt(uint64_t v) {
        RELEASE_ASSERT(v < (1L << 32), "");
        for (int i = 3; i >= 0; i--) {
            writeByte((v >> (i * 8)) & 0xff);
        }
    }

    void writeULL(uint64_t v) {
        for (int i = 7; i >= 0; i--) {
            writeByte((v >> (i * 8)) & 0xff);
        }
    }

    void writeDouble(double v) {
        union {
            double v;
            uint64_t u;
        } u{.v = v };
        writeULL(u.u);
    }

    void writeString(llvm::StringRef v) {
        writeUInt(v.size());
        fwrite(v.data(), 1, v.size(), file);
        for (int i = 0; i < v.size(); i++) {
            checksum ^= v[i];
        }
    }

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

    void writeSlice(AST_slice* e) {
        if (!e) {
            writeByte(0x00);
        } else {
            writeByte(e->type);
            writeByte(0xae); // check byte
            e->accept(this);
        }
    }
    void writeSliceVector(const std::vector<AST_slice*>& vec) {
        writeShort(vec.size());
        for (auto* e : vec) {
            writeSlice(e);
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
        writeExpr(node->kwarg);
        writeExpr(node->vararg);
        return true;
    }
    virtual bool visit_assert(AST_Assert* node) {
        writeLineno(node->lineno);
        writeExpr(node->msg);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_assign(AST_Assign* node) {
        writeLineno(node->lineno);
        writeExprVector(node->targets);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_augassign(AST_AugAssign* node) {
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExpr(node->target);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_attribute(AST_Attribute* node) {
        writeString(node->attr);
        writeByte(node->ctx_type);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_binop(AST_BinOp* node) {
        writeExpr(node->left);
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExpr(node->right);
        return true;
    }
    virtual bool visit_boolop(AST_BoolOp* node) {
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExprVector(node->values);
        return true;
    }
    virtual bool visit_break(AST_Break* node) {
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_call(AST_Call* node) {
        writeExprVector(node->args);
        writeExpr(node->func);
        writeMiscVector(node->keywords);
        writeExpr(node->kwargs);
        writeLineno(node->lineno);
        writeExpr(node->starargs);
        return true;
    }
    virtual bool visit_compare(AST_Compare* node) {
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
        writeExprVector(node->decorator_list);
        writeLineno(node->lineno);
        writeString(node->name);
        return true;
    }
    virtual bool visit_continue(AST_Continue* node) {
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_delete(AST_Delete* node) {
        writeLineno(node->lineno);
        writeExprVector(node->targets);
        return true;
    }
    virtual bool visit_dict(AST_Dict* node) {
        writeExprVector(node->keys);
        writeLineno(node->lineno);
        writeExprVector(node->values);
        return true;
    }
    virtual bool visit_dictcomp(AST_DictComp* node) {
        writeMiscVector(node->generators);
        writeExpr(node->key);
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_ellipsis(AST_Ellipsis* node) { return true; }
    virtual bool visit_excepthandler(AST_ExceptHandler* node) {
        writeStmtVector(node->body);
        writeLineno(node->lineno);
        writeExpr(node->name);
        writeExpr(node->type);
        return true;
    }
    virtual bool visit_exec(AST_Exec* node) {
        writeExpr(node->body);
        writeExpr(node->globals);
        writeLineno(node->lineno);
        writeExpr(node->locals);
        return true;
    }
    virtual bool visit_expr(AST_Expr* node) {
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_extslice(AST_ExtSlice* node) {
        writeSliceVector(node->dims);
        return true;
    }
    virtual bool visit_for(AST_For* node) {
        writeStmtVector(node->body);
        writeExpr(node->iter);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        writeExpr(node->target);
        return true;
    }
    virtual bool visit_functiondef(AST_FunctionDef* node) {
        writeASTMisc(node->args);
        writeStmtVector(node->body);
        writeExprVector(node->decorator_list);
        writeLineno(node->lineno);
        writeString(node->name);
        return true;
    }
    virtual bool visit_generatorexp(AST_GeneratorExp* node) {
        writeExpr(node->elt);
        writeMiscVector(node->generators);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_global(AST_Global* node) {
        writeLineno(node->lineno);
        writeStringVector(node->names);
        return true;
    }
    virtual bool visit_if(AST_If* node) {
        writeStmtVector(node->body);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_ifexp(AST_IfExp* node) {
        writeExpr(node->body);
        writeLineno(node->lineno);
        writeExpr(node->orelse);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_import(AST_Import* node) {
        writeLineno(node->lineno);
        writeMiscVector(node->names);
        return true;
    }
    virtual bool visit_importfrom(AST_ImportFrom* node) {
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
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_list(AST_List* node) {
        writeByte(node->ctx_type);
        writeExprVector(node->elts);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_listcomp(AST_ListComp* node) {
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
        writeByte(node->ctx_type);
        writeString(node->id);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_num(AST_Num* node) {
        writeByte(node->num_type);
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
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_print(AST_Print* node) {
        writeExpr(node->dest);
        writeLineno(node->lineno);
        writeByte(node->nl);
        writeExprVector(node->values);
        return true;
    }
    virtual bool visit_raise(AST_Raise* node) {
        // "arg0" "arg1" "arg2" are called "type", "inst", and "tback" in the python ast,
        // so that's the order we have to write them:
        writeExpr(node->arg1 /*inst*/);
        writeLineno(node->lineno);
        writeExpr(node->arg2 /*tback*/);
        writeExpr(node->arg0 /*type*/);
        return true;
    }
    virtual bool visit_repr(AST_Repr* node) {
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_return(AST_Return* node) {
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_set(AST_Set* node) {
        writeExprVector(node->elts);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_setcomp(AST_SetComp* node) {
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
        writeByte(node->ctx_type);
        writeLineno(node->lineno);
        writeSlice(node->slice);
        writeExpr(node->value);
        return true;
    }
    virtual bool visit_tryexcept(AST_TryExcept* node) {
        writeStmtVector(node->body);
        writeMiscVector(node->handlers);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        return true;
    }
    virtual bool visit_tryfinally(AST_TryFinally* node) {
        writeStmtVector(node->body);
        writeStmtVector(node->finalbody);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_tuple(AST_Tuple* node) {
        writeByte(node->ctx_type);
        writeExprVector(node->elts);
        writeLineno(node->lineno);
        return true;
    }
    virtual bool visit_unaryop(AST_UnaryOp* node) {
        writeLineno(node->lineno);
        writeByte(node->op_type);
        writeExpr(node->operand);
        return true;
    }
    virtual bool visit_while(AST_While* node) {
        writeStmtVector(node->body);
        writeLineno(node->lineno);
        writeStmtVector(node->orelse);
        writeExpr(node->test);
        return true;
    }
    virtual bool visit_with(AST_With* node) {
        writeStmtVector(node->body);
        writeExpr(node->context_expr);
        writeLineno(node->lineno);
        writeExpr(node->optional_vars);
        return true;
    }
    virtual bool visit_yield(AST_Yield* node) {
        writeLineno(node->lineno);
        writeExpr(node->value);
        return true;
    }
};
}

std::pair<unsigned long, uint8_t> serializeAST(AST_Module* module, FILE* file) {
    return SerializeASTVisitor::write(module, file);
}
}
