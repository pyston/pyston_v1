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

#include "codegen/parser.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdint.h>
#include <sys/stat.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/pypa-parser.h"
#include "codegen/serialize_ast.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "core/util.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

bool DEBUG_PARSING = false;

class BufferedReader {
private:
    static const int BUFSIZE = 1024;
    char buf[BUFSIZE];
    int start, end;

    // exactly one of these should be set and valid:
    FILE* fp;
    std::vector<char> data;

    InternedStringPool* intern_pool;

    void ensure(int num) {
        if (unlikely(fp) && end - start < num) {
            fill();
        }
    }

public:
    void fill() {
        if (unlikely(fp)) {
            memmove(buf, buf + start, end - start);
            end -= start;
            start = 0;
            end += fread(buf + end, 1, BUFSIZE - end, fp);
            if (VERBOSITY("parsing") >= 3)
                printf("filled, now at %d-%d\n", start, end);
        }
    }

    BufferedReader(FILE* fp) : start(0), end(0), fp(fp), data(), intern_pool(NULL) {}
    BufferedReader(std::vector<char> data, int start_offset = 0)
        : start(start_offset), end(data.size()), fp(NULL), data(std::move(data)), intern_pool(NULL) {}

    int bytesBuffered() { return (end - start); }

    uint8_t readByte() {
        ensure(1);
        RELEASE_ASSERT(end > start, "premature eof");

        if (unlikely(fp)) {
            return buf[start++];
        } else {
            return data[start++];
        }
    }
    uint16_t readShort() { return (readByte() << 8) | (readByte()); }
    uint32_t readUInt() { return (readShort() << 16) | (readShort()); }
    uint64_t readULL() { return ((uint64_t)readUInt() << 32) | (readUInt()); }
    double readDouble() {
        union {
            uint64_t raw;
            double d;
        };
        raw = readULL();
        return d;
    }

    std::unique_ptr<InternedStringPool> createInternedPool();
    InternedString readAndInternString();
    void readAndInternStringVector(std::vector<InternedString>& v);
};

std::unique_ptr<InternedStringPool> BufferedReader::createInternedPool() {
    assert(this->intern_pool == NULL);
    this->intern_pool = new InternedStringPool();
    return std::unique_ptr<InternedStringPool>(this->intern_pool);
}

AST* readASTMisc(BufferedReader* reader);
AST_expr* readASTExpr(BufferedReader* reader);
AST_stmt* readASTStmt(BufferedReader* reader);

static std::string readString(BufferedReader* reader) {
    int strlen = reader->readUInt();
    llvm::SmallString<32> chars;
    for (int i = 0; i < strlen; i++) {
        chars.push_back(reader->readByte());
    }
    return chars.str().str();
}

InternedString BufferedReader::readAndInternString() {
    int strlen = readUInt();
    llvm::SmallString<32> chars;
    for (int i = 0; i < strlen; i++) {
        chars.push_back(readByte());
    }
    return intern_pool->get(chars.str());
}

void BufferedReader::readAndInternStringVector(std::vector<InternedString>& v) {
    int num_elts = readShort();
    if (VERBOSITY("parsing") >= 3)
        printf("%d elts to read\n", num_elts);
    for (int i = 0; i < num_elts; i++) {
        v.push_back(readAndInternString());
    }
}

static void readStringVector(std::vector<std::string>& vec, BufferedReader* reader) {
    int num_elts = reader->readShort();
    if (VERBOSITY("parsing") >= 3)
        printf("%d elts to read\n", num_elts);
    for (int i = 0; i < num_elts; i++) {
        vec.push_back(readString(reader));
    }
}

static void readStmtVector(std::vector<AST_stmt*>& vec, BufferedReader* reader) {
    int num_elts = reader->readShort();
    if (VERBOSITY("parsing") >= 3)
        printf("%d elts to read\n", num_elts);
    for (int i = 0; i < num_elts; i++) {
        vec.push_back(readASTStmt(reader));
    }
}

static void readExprVector(std::vector<AST_expr*>& vec, BufferedReader* reader) {
    int num_elts = reader->readShort();
    if (VERBOSITY("parsing") >= 3)
        printf("%d elts to read\n", num_elts);
    for (int i = 0; i < num_elts; i++) {
        vec.push_back(readASTExpr(reader));
    }
}

template <class T> static void readMiscVector(std::vector<T*>& vec, BufferedReader* reader) {
    int num_elts = reader->readShort();
    if (VERBOSITY("parsing") >= 3)
        printf("%d elts to read\n", num_elts);
    for (int i = 0; i < num_elts; i++) {
        AST* read = readASTMisc(reader);
        assert(read->type == T::TYPE);
        vec.push_back(static_cast<T*>(read));
    }
}

static int readColOffset(BufferedReader* reader) {
    int rtn = reader->readULL();
    // offsets out of this range are almost certainly parse bugs:
    ASSERT(rtn >= -1 && rtn < 100000, "%d", rtn);
    return rtn;
}

AST_alias* read_alias(BufferedReader* reader) {
    InternedString asname = reader->readAndInternString();
    InternedString name = reader->readAndInternString();

    AST_alias* rtn = new AST_alias(name, asname);
    rtn->col_offset = -1;
    rtn->lineno = -1;

    return rtn;
}

AST_arguments* read_arguments(BufferedReader* reader) {
    if (VERBOSITY("parsing") >= 3)
        printf("reading arguments\n");

    AST_arguments* rtn = new AST_arguments();

    readExprVector(rtn->args, reader);
    rtn->col_offset = -1;
    readExprVector(rtn->defaults, reader);
    rtn->kwarg = reader->readAndInternString();
    rtn->lineno = -1;
    rtn->vararg = reader->readAndInternString();
    return rtn;
}

AST_Assert* read_assert(BufferedReader* reader) {
    AST_Assert* rtn = new AST_Assert();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->msg = readASTExpr(reader);
    rtn->test = readASTExpr(reader);
    return rtn;
}

AST_Assign* read_assign(BufferedReader* reader) {
    AST_Assign* rtn = new AST_Assign();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    readExprVector(rtn->targets, reader);
    rtn->value = readASTExpr(reader);
    return rtn;
}

AST_AugAssign* read_augassign(BufferedReader* reader) {
    AST_AugAssign* rtn = new AST_AugAssign();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->op_type = (AST_TYPE::AST_TYPE)reader->readByte();
    rtn->target = readASTExpr(reader);
    rtn->value = readASTExpr(reader);
    return rtn;
}

AST_Attribute* read_attribute(BufferedReader* reader) {
    AST_Attribute* rtn = new AST_Attribute();

    rtn->attr = reader->readAndInternString();
    rtn->col_offset = readColOffset(reader);
    rtn->ctx_type = (AST_TYPE::AST_TYPE)reader->readByte();
    rtn->lineno = reader->readULL();
    rtn->value = readASTExpr(reader);
    return rtn;
}

AST_expr* read_binop(BufferedReader* reader) {
    AST_BinOp* rtn = new AST_BinOp();

    rtn->col_offset = readColOffset(reader);
    rtn->left = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->op_type = (AST_TYPE::AST_TYPE)reader->readByte();
    rtn->right = readASTExpr(reader);

    return rtn;
}

AST_expr* read_boolop(BufferedReader* reader) {
    AST_BoolOp* rtn = new AST_BoolOp();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->op_type = (AST_TYPE::AST_TYPE)reader->readByte();
    readExprVector(rtn->values, reader);

    return rtn;
}

AST_Break* read_break(BufferedReader* reader) {
    AST_Break* rtn = new AST_Break();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();

    return rtn;
}

AST_Call* read_call(BufferedReader* reader) {
    AST_Call* rtn = new AST_Call();

    readExprVector(rtn->args, reader);
    rtn->col_offset = readColOffset(reader);
    rtn->func = readASTExpr(reader);

    readMiscVector(rtn->keywords, reader);

    rtn->kwargs = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->starargs = readASTExpr(reader);
    return rtn;
}

AST_expr* read_compare(BufferedReader* reader) {
    AST_Compare* rtn = new AST_Compare();

    rtn->col_offset = readColOffset(reader);
    readExprVector(rtn->comparators, reader);
    rtn->left = readASTExpr(reader);
    rtn->lineno = reader->readULL();

    int num_ops = reader->readShort();
    assert(num_ops == rtn->comparators.size());
    for (int i = 0; i < num_ops; i++) {
        rtn->ops.push_back((AST_TYPE::AST_TYPE)reader->readByte());
    }

    return rtn;
}

AST_comprehension* read_comprehension(BufferedReader* reader) {
    AST_comprehension* rtn = new AST_comprehension();

    readExprVector(rtn->ifs, reader);
    rtn->iter = readASTExpr(reader);
    rtn->target = readASTExpr(reader);

    rtn->col_offset = -1;
    rtn->lineno = -1;

    return rtn;
}

AST_ClassDef* read_classdef(BufferedReader* reader) {
    AST_ClassDef* rtn = new AST_ClassDef();

    readExprVector(rtn->bases, reader);
    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    readExprVector(rtn->decorator_list, reader);
    rtn->lineno = reader->readULL();
    rtn->name = reader->readAndInternString();

    return rtn;
}

AST_Continue* read_continue(BufferedReader* reader) {
    AST_Continue* rtn = new AST_Continue();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();

    return rtn;
}

AST_Delete* read_delete(BufferedReader* reader) {
    AST_Delete* rtn = new AST_Delete();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    readExprVector(rtn->targets, reader);

    return rtn;
}

AST_Dict* read_dict(BufferedReader* reader) {
    AST_Dict* rtn = new AST_Dict();

    rtn->col_offset = readColOffset(reader);
    readExprVector(rtn->keys, reader);
    rtn->lineno = reader->readULL();
    readExprVector(rtn->values, reader);

    assert(rtn->keys.size() == rtn->values.size());
    return rtn;
}

AST_DictComp* read_dictcomp(BufferedReader* reader) {
    AST_DictComp* rtn = new AST_DictComp();
    rtn->col_offset = readColOffset(reader);
    readMiscVector(rtn->generators, reader);
    rtn->key = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->value = readASTExpr(reader);
    return rtn;
}


AST_ExceptHandler* read_excepthandler(BufferedReader* reader) {
    AST_ExceptHandler* rtn = new AST_ExceptHandler();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->name = readASTExpr(reader);
    rtn->type = readASTExpr(reader);

    return rtn;
}

AST_Exec* read_exec(BufferedReader* reader) {
    AST_Exec* rtn = new AST_Exec();

    rtn->body = readASTExpr(reader);
    rtn->col_offset = readColOffset(reader);
    rtn->globals = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->locals = readASTExpr(reader);

    return rtn;
}

AST_Expr* read_expr(BufferedReader* reader) {
    AST_Expr* rtn = new AST_Expr();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->value = readASTExpr(reader);
    return rtn;
}

AST_ExtSlice* read_extslice(BufferedReader* reader) {
    AST_ExtSlice* rtn = new AST_ExtSlice();

    rtn->col_offset = -1;
    rtn->lineno = -1;
    readExprVector(rtn->dims, reader);
    return rtn;
}

AST_For* read_for(BufferedReader* reader) {
    AST_For* rtn = new AST_For();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    rtn->iter = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    readStmtVector(rtn->orelse, reader);
    rtn->target = readASTExpr(reader);
    return rtn;
}

AST_FunctionDef* read_functiondef(BufferedReader* reader) {
    if (VERBOSITY("parsing") >= 3)
        printf("reading functiondef\n");
    AST_FunctionDef* rtn = new AST_FunctionDef();

    rtn->args = ast_cast<AST_arguments>(readASTMisc(reader));
    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    readExprVector(rtn->decorator_list, reader);
    rtn->lineno = reader->readULL();
    rtn->name = reader->readAndInternString();
    return rtn;
}

AST_GeneratorExp* read_generatorexp(BufferedReader* reader) {
    AST_GeneratorExp* rtn = new AST_GeneratorExp();

    rtn->col_offset = readColOffset(reader);
    rtn->elt = readASTExpr(reader);
    readMiscVector(rtn->generators, reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_Global* read_global(BufferedReader* reader) {
    AST_Global* rtn = new AST_Global();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    reader->readAndInternStringVector(rtn->names);
    return rtn;
}

AST_If* read_if(BufferedReader* reader) {
    AST_If* rtn = new AST_If();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    readStmtVector(rtn->orelse, reader);
    rtn->test = readASTExpr(reader);
    return rtn;
}

AST_IfExp* read_ifexp(BufferedReader* reader) {
    AST_IfExp* rtn = new AST_IfExp();

    rtn->body = readASTExpr(reader);
    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->orelse = readASTExpr(reader);
    rtn->test = readASTExpr(reader);
    return rtn;
}

AST_Import* read_import(BufferedReader* reader) {
    AST_Import* rtn = new AST_Import();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    readMiscVector(rtn->names, reader);
    return rtn;
}

AST_ImportFrom* read_importfrom(BufferedReader* reader) {
    AST_ImportFrom* rtn = new AST_ImportFrom();

    rtn->col_offset = readColOffset(reader);
    rtn->level = reader->readULL();
    rtn->lineno = reader->readULL();
    rtn->module = reader->readAndInternString();
    readMiscVector(rtn->names, reader);
    return rtn;
}

AST_Index* read_index(BufferedReader* reader) {
    AST_Index* rtn = new AST_Index();

    rtn->col_offset = -1;
    rtn->lineno = -1;
    rtn->value = readASTExpr(reader);
    assert(rtn->value);
    return rtn;
}

AST_keyword* read_keyword(BufferedReader* reader) {
    AST_keyword* rtn = new AST_keyword();

    rtn->arg = reader->readAndInternString();
    rtn->col_offset = -1;
    rtn->lineno = -1;
    rtn->value = readASTExpr(reader);
    return rtn;
}

AST_Lambda* read_lambda(BufferedReader* reader) {
    AST_Lambda* rtn = new AST_Lambda();

    rtn->args = ast_cast<AST_arguments>(readASTMisc(reader));
    rtn->body = readASTExpr(reader);
    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_List* read_list(BufferedReader* reader) {
    AST_List* rtn = new AST_List();

    rtn->col_offset = readColOffset(reader);
    rtn->ctx_type = (AST_TYPE::AST_TYPE)reader->readByte();
    readExprVector(rtn->elts, reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_ListComp* read_listcomp(BufferedReader* reader) {
    AST_ListComp* rtn = new AST_ListComp();

    rtn->col_offset = readColOffset(reader);
    rtn->elt = readASTExpr(reader);
    readMiscVector(rtn->generators, reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_Module* read_module(BufferedReader* reader) {
    if (VERBOSITY("parsing") >= 3)
        printf("reading module\n");

    AST_Module* rtn = new AST_Module(reader->createInternedPool());

    readStmtVector(rtn->body, reader);
    rtn->col_offset = -1;
    rtn->lineno = -1;
    return rtn;
}

AST_Name* read_name(BufferedReader* reader) {
    auto col_offset = readColOffset(reader);
    auto ctx_type = (AST_TYPE::AST_TYPE)reader->readByte();
    auto id = reader->readAndInternString();
    auto lineno = reader->readULL();

    return new AST_Name(std::move(id), ctx_type, lineno, col_offset);
}

AST_Num* read_num(BufferedReader* reader) {
    AST_Num* rtn = new AST_Num();

    rtn->num_type = (AST_Num::NumType)reader->readByte();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();

    if (rtn->num_type == AST_Num::INT) {
        rtn->n_int = reader->readULL(); // automatic conversion to signed
    } else if (rtn->num_type == AST_Num::LONG) {
        rtn->n_long = readString(reader);
    } else if (rtn->num_type == AST_Num::FLOAT) {
        rtn->n_float = reader->readDouble();
    } else if (rtn->num_type == AST_Num::COMPLEX) {
        rtn->n_float = reader->readDouble();
    } else {
        RELEASE_ASSERT(0, "%d", rtn->num_type);
    }
    return rtn;
}

AST_Repr* read_repr(BufferedReader* reader) {
    AST_Repr* rtn = new AST_Repr();
    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->value = readASTExpr(reader);

    return rtn;
}

AST_Pass* read_pass(BufferedReader* reader) {
    AST_Pass* rtn = new AST_Pass();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_Print* read_print(BufferedReader* reader) {
    AST_Print* rtn = new AST_Print();

    rtn->col_offset = readColOffset(reader);
    rtn->dest = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->nl = reader->readByte();
    readExprVector(rtn->values, reader);
    return rtn;
}

AST_Raise* read_raise(BufferedReader* reader) {
    AST_Raise* rtn = new AST_Raise();

    // "arg0" "arg1" "arg2" are called "type", "inst", and "tback" in the python ast,
    // so that's the order we have to read them:
    rtn->col_offset = readColOffset(reader);
    rtn->arg1 /*inst*/ = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->arg2 /*tback*/ = readASTExpr(reader);
    rtn->arg0 /*type*/ = readASTExpr(reader);
    return rtn;
}

AST_Return* read_return(BufferedReader* reader) {
    AST_Return* rtn = new AST_Return();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->value = readASTExpr(reader);
    return rtn;
}

AST_Set* read_set(BufferedReader* reader) {
    AST_Set* rtn = new AST_Set();

    rtn->col_offset = readColOffset(reader);
    readExprVector(rtn->elts, reader);
    rtn->lineno = reader->readULL();

    return rtn;
}

AST_SetComp* read_setcomp(BufferedReader* reader) {
    AST_SetComp* rtn = new AST_SetComp();

    rtn->col_offset = readColOffset(reader);
    rtn->elt = readASTExpr(reader);
    readMiscVector(rtn->generators, reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_Slice* read_slice(BufferedReader* reader) {
    AST_Slice* rtn = new AST_Slice();

    rtn->col_offset = -1;
    rtn->lineno = -1;
    rtn->lower = readASTExpr(reader);
    rtn->step = readASTExpr(reader);
    rtn->upper = readASTExpr(reader);

    return rtn;
}

AST_Str* read_str(BufferedReader* reader) {
    AST_Str* rtn = new AST_Str();

    rtn->str_type = (AST_Str::StrType)reader->readByte();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();

    if (rtn->str_type == AST_Str::STR) {
        rtn->str_data = readString(reader);
    } else if (rtn->str_type == AST_Str::UNICODE) {
        rtn->str_data = readString(reader);
    } else {
        RELEASE_ASSERT(0, "%d", rtn->str_type);
    }

    return rtn;
}

AST_Subscript* read_subscript(BufferedReader* reader) {
    AST_Subscript* rtn = new AST_Subscript();

    rtn->col_offset = readColOffset(reader);
    rtn->ctx_type = (AST_TYPE::AST_TYPE)reader->readByte();
    rtn->lineno = reader->readULL();
    rtn->slice = readASTExpr(reader);
    rtn->value = readASTExpr(reader);

    return rtn;
}

AST_TryExcept* read_tryexcept(BufferedReader* reader) {
    AST_TryExcept* rtn = new AST_TryExcept();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    readMiscVector(rtn->handlers, reader);
    rtn->lineno = reader->readULL();
    readStmtVector(rtn->orelse, reader);
    return rtn;
}

AST_TryFinally* read_tryfinally(BufferedReader* reader) {
    AST_TryFinally* rtn = new AST_TryFinally();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    readStmtVector(rtn->finalbody, reader);
    rtn->lineno = reader->readULL();
    return rtn;
}

AST_Tuple* read_tuple(BufferedReader* reader) {
    AST_Tuple* rtn = new AST_Tuple();

    rtn->col_offset = readColOffset(reader);
    rtn->ctx_type = (AST_TYPE::AST_TYPE)reader->readByte();
    readExprVector(rtn->elts, reader);
    rtn->lineno = reader->readULL();

    return rtn;
}

AST_UnaryOp* read_unaryop(BufferedReader* reader) {
    AST_UnaryOp* rtn = new AST_UnaryOp();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->op_type = (AST_TYPE::AST_TYPE)reader->readByte();
    rtn->operand = readASTExpr(reader);

    return rtn;
}

AST_While* read_while(BufferedReader* reader) {
    AST_While* rtn = new AST_While();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    readStmtVector(rtn->orelse, reader);
    rtn->test = readASTExpr(reader);

    return rtn;
}

AST_With* read_with(BufferedReader* reader) {
    AST_With* rtn = new AST_With();

    readStmtVector(rtn->body, reader);
    rtn->col_offset = readColOffset(reader);
    rtn->context_expr = readASTExpr(reader);
    rtn->lineno = reader->readULL();
    rtn->optional_vars = readASTExpr(reader);

    return rtn;
}

AST_Yield* read_yield(BufferedReader* reader) {
    AST_Yield* rtn = new AST_Yield();

    rtn->col_offset = readColOffset(reader);
    rtn->lineno = reader->readULL();
    rtn->value = readASTExpr(reader);

    return rtn;
}

AST_expr* readASTExpr(BufferedReader* reader) {
    uint8_t type = reader->readByte();
    if (VERBOSITY("parsing") >= 3)
        printf("type = %d\n", type);
    if (type == 0)
        return NULL;

    uint8_t checkbyte = reader->readByte();
    assert(checkbyte == 0xae);

    switch (type) {
        case AST_TYPE::Attribute:
            return read_attribute(reader);
        case AST_TYPE::BinOp:
            return read_binop(reader);
        case AST_TYPE::BoolOp:
            return read_boolop(reader);
        case AST_TYPE::Call:
            return read_call(reader);
        case AST_TYPE::Compare:
            return read_compare(reader);
        case AST_TYPE::Dict:
            return read_dict(reader);
        case AST_TYPE::DictComp:
            return read_dictcomp(reader);
        case AST_TYPE::ExtSlice:
            return read_extslice(reader);
        case AST_TYPE::GeneratorExp:
            return read_generatorexp(reader);
        case AST_TYPE::IfExp:
            return read_ifexp(reader);
        case AST_TYPE::Index:
            return read_index(reader);
        case AST_TYPE::Lambda:
            return read_lambda(reader);
        case AST_TYPE::List:
            return read_list(reader);
        case AST_TYPE::ListComp:
            return read_listcomp(reader);
        case AST_TYPE::Name:
            return read_name(reader);
        case AST_TYPE::Num:
            return read_num(reader);
        case AST_TYPE::Repr:
            return read_repr(reader);
        case AST_TYPE::Set:
            return read_set(reader);
        case AST_TYPE::SetComp:
            return read_setcomp(reader);
        case AST_TYPE::Slice:
            return read_slice(reader);
        case AST_TYPE::Str:
            return read_str(reader);
        case AST_TYPE::Subscript:
            return read_subscript(reader);
        case AST_TYPE::Tuple:
            return read_tuple(reader);
        case AST_TYPE::UnaryOp:
            return read_unaryop(reader);
        case AST_TYPE::Yield:
            return read_yield(reader);
        default:
            fprintf(stderr, "Unknown expr node type (parser.cpp:" STRINGIFY(__LINE__) "): %d\n", type);
            abort();
            break;
    }
}

AST_stmt* readASTStmt(BufferedReader* reader) {
    uint8_t type = reader->readByte();
    if (VERBOSITY("parsing") >= 3)
        printf("type = %d\n", type);
    if (type == 0)
        return NULL;

    uint8_t checkbyte = reader->readByte();
    assert(checkbyte == 0xae);

    switch (type) {
        case AST_TYPE::Assert:
            return read_assert(reader);
        case AST_TYPE::Assign:
            return read_assign(reader);
        case AST_TYPE::AugAssign:
            return read_augassign(reader);
        case AST_TYPE::Break:
            return read_break(reader);
        case AST_TYPE::ClassDef:
            return read_classdef(reader);
        case AST_TYPE::Continue:
            return read_continue(reader);
        case AST_TYPE::Delete:
            return read_delete(reader);
        case AST_TYPE::Exec:
            return read_exec(reader);
        case AST_TYPE::Expr:
            return read_expr(reader);
        case AST_TYPE::For:
            return read_for(reader);
        case AST_TYPE::FunctionDef:
            return read_functiondef(reader);
        case AST_TYPE::Global:
            return read_global(reader);
        case AST_TYPE::If:
            return read_if(reader);
        case AST_TYPE::Import:
            return read_import(reader);
        case AST_TYPE::ImportFrom:
            return read_importfrom(reader);
        case AST_TYPE::Pass:
            return read_pass(reader);
        case AST_TYPE::Print:
            return read_print(reader);
        case AST_TYPE::Raise:
            return read_raise(reader);
        case AST_TYPE::Return:
            return read_return(reader);
        case AST_TYPE::TryExcept:
            return read_tryexcept(reader);
        case AST_TYPE::TryFinally:
            return read_tryfinally(reader);
        case AST_TYPE::While:
            return read_while(reader);
        case AST_TYPE::With:
            return read_with(reader);
        default:
            fprintf(stderr, "Unknown stmt node type (parser.cpp:" STRINGIFY(__LINE__) "): %d\n", type);
            exit(1);
            break;
    }
}

AST* readASTMisc(BufferedReader* reader) {
    uint8_t type = reader->readByte();
    if (VERBOSITY("parsing") >= 3)
        printf("type = %d\n", type);
    if (type == 0)
        return NULL;

    uint8_t checkbyte = reader->readByte();
    assert(checkbyte == 0xae);

    switch (type) {
        case AST_TYPE::alias:
            return read_alias(reader);
        case AST_TYPE::arguments:
            return read_arguments(reader);
        case AST_TYPE::comprehension:
            return read_comprehension(reader);
        case AST_TYPE::ExceptHandler:
            return read_excepthandler(reader);
        case AST_TYPE::keyword:
            return read_keyword(reader);
        case AST_TYPE::Module:
            return read_module(reader);
        default:
            fprintf(stderr, "Unknown node type (parser.cpp:" STRINGIFY(__LINE__) "): %d\n", type);
            exit(1);
            break;
    }
}

static std::string getParserCommandLine(const char* fn) {
    llvm::SmallString<128> parse_ast_fn;
    // TODO supposed to pass argv0, main_addr to this function:
    parse_ast_fn = llvm::sys::fs::getMainExecutable(NULL, NULL);
    assert(parse_ast_fn.size() && "could not find the path to the pyston src dir");

    // Start by removing the binary name, because the "pyston" binary will break the logic below
    llvm::sys::path::remove_filename(parse_ast_fn);

    llvm::sys::path::append(parse_ast_fn, "src/codegen/parse_ast.py");

    // We may be running in an environment where "python" resolves to pyston (ex in
    // a virtualenv), so try to hard code the path to CPython.
    // This should probably be a configure-time check?
    return std::string("/usr/bin/python -S ") + parse_ast_fn.str().str() + " " + fn;
}

AST_Module* parse_string(const char* code) {
    int size = strlen(code);
    char buf[] = "pystontmp_XXXXXX";
    char* tmpdir = mkdtemp(buf);
    assert(tmpdir);
    std::string tmp = std::string(tmpdir) + "/in.py";
    if (VERBOSITY() >= 3) {
        printf("writing %d bytes to %s\n", size, tmp.c_str());
    }

    FILE* f = fopen(tmp.c_str(), "w");
    fwrite(code, 1, size, f);
    fputc('\n', f);
    fclose(f);

    AST_Module* m = parse_file(tmp.c_str());
    removeDirectoryIfExists(tmpdir);

    return m;
}

AST_Module* parse_file(const char* fn) {
    Timer _t("parsing");

    if (ENABLE_PYPA_PARSER) {
        AST_Module* rtn = pypa_parse(fn);
        RELEASE_ASSERT(rtn, "unknown parse error (possibly: '%s'?)", strerror(errno));
        return rtn;
    }

    FILE* fp = popen(getParserCommandLine(fn).c_str(), "r");

    BufferedReader* reader = new BufferedReader(fp);
    AST* rtn = readASTMisc(reader);
    reader->fill();
    ASSERT(reader->bytesBuffered() == 0, "%d", reader->bytesBuffered());
    delete reader;

    int code = pclose(fp);
    assert(code == 0);

    assert(rtn->type == AST_TYPE::Module);

    long us = _t.end();
    static StatCounter us_parsing("us_parsing");
    us_parsing.log(us);

    return ast_cast<AST_Module>(rtn);
}

const char* getMagic() {
    if (ENABLE_PYPA_PARSER)
        return "a\ncM";
    else
        return "a\ncm";
}

#define MAGIC_STRING_LENGTH 4
#define LENGTH_LENGTH sizeof(int)
#define CHECKSUM_LENGTH 1

// Does at least one of: returns a valid file_data vector, or fills in 'module'
static std::vector<char> _reparse(const char* fn, const std::string& cache_fn, AST_Module*& module) {
    FILE* cache_fp = fopen(cache_fn.c_str(), "w");

    if (DEBUG_PARSING) {
        fprintf(stderr, "_reparse('%s', '%s'), pypa=%d\n", fn, cache_fn.c_str(), ENABLE_PYPA_PARSER);
        fprintf(stderr, "writing magic string: %d %d %d %d\n", getMagic()[0], getMagic()[1], getMagic()[2],
                getMagic()[3]);
    }

    std::vector<char> file_data;
    if (cache_fp)
        fwrite(getMagic(), 1, MAGIC_STRING_LENGTH, cache_fp);
    file_data.insert(file_data.end(), getMagic(), getMagic() + MAGIC_STRING_LENGTH);

    int checksum_start = file_data.size();

    int bytes_written = -1;
    static_assert(sizeof(bytes_written) == LENGTH_LENGTH, "");
    if (cache_fp)
        fwrite(&bytes_written, 1, LENGTH_LENGTH, cache_fp);
    file_data.insert(file_data.end(), (char*)&bytes_written, (char*)&bytes_written + LENGTH_LENGTH);
    bytes_written = 0;

    uint8_t checksum = -1;
    static_assert(sizeof(checksum) == CHECKSUM_LENGTH, "");
    if (cache_fp)
        fwrite(&checksum, 1, CHECKSUM_LENGTH, cache_fp);
    file_data.insert(file_data.end(), (char*)&checksum, (char*)&checksum + CHECKSUM_LENGTH);
    checksum = 0;

    if (ENABLE_PYPA_PARSER) {
        module = pypa_parse(fn);
        RELEASE_ASSERT(module, "unknown parse error");

        if (!cache_fp)
            return std::vector<char>();

        auto p = serializeAST(module, cache_fp);
        checksum = p.second;
        bytes_written += p.first;
    } else {
        FILE* parser = popen(getParserCommandLine(fn).c_str(), "r");
        char buf[80];
        while (true) {
            int nread = fread(buf, 1, 80, parser);
            if (nread == 0)
                break;
            bytes_written += nread;

            if (cache_fp)
                fwrite(buf, 1, nread, cache_fp);
            file_data.insert(file_data.end(), buf, buf + nread);

            for (int i = 0; i < nread; i++) {
                checksum ^= buf[i];
            }
        }
        int code = pclose(parser);
        assert(code == 0);
    }

    fseek(cache_fp, checksum_start, SEEK_SET);
    if (cache_fp)
        fwrite(&bytes_written, 1, LENGTH_LENGTH, cache_fp);
    memcpy(&file_data[checksum_start], &bytes_written, LENGTH_LENGTH);
    if (cache_fp)
        fwrite(&checksum, 1, CHECKSUM_LENGTH, cache_fp);
    memcpy(&file_data[checksum_start + LENGTH_LENGTH], &checksum, CHECKSUM_LENGTH);

    if (cache_fp)
        fclose(cache_fp);
    return std::move(file_data);
}

// Parsing the file is somewhat expensive since we have to shell out to cpython;
// it's not a huge deal right now, but this caching version can significantly cut down
// on the startup time (40ms -> 10ms).
AST_Module* caching_parse_file(const char* fn) {
    std::ostringstream oss;
    if (DEBUG_PARSING) {
        oss << "caching_parse_file() on " << fn << '\n';
        fprintf(stderr, "caching_parse_file('%s'), pypa=%d\n", fn, ENABLE_PYPA_PARSER);
    }

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_caching_parse_file");
    static StatCounter us_parsing("us_parsing");
    Timer _t("parsing");
    _t.setExitCallback([](uint64_t t) { us_parsing.log(t); });

    int code;
    std::string cache_fn = std::string(fn) + "c";

    struct stat source_stat, cache_stat;
    code = stat(fn, &source_stat);
    assert(code == 0);
    code = stat(cache_fn.c_str(), &cache_stat);
    std::vector<char> file_data;
    if (code == 0 && (cache_stat.st_mtime > source_stat.st_mtime
                      || (cache_stat.st_mtime == source_stat.st_mtime
                          && cache_stat.st_mtim.tv_nsec > source_stat.st_mtim.tv_nsec))) {
        oss << "reading pyc file\n";
        char buf[1024];
        FILE* cache_fp = fopen(cache_fn.c_str(), "r");
        if (cache_fp) {
            while (true) {
                int read = fread(buf, 1, 1024, cache_fp);
                file_data.insert(file_data.end(), buf, buf + read);

                if (read == 0) {
                    if (ferror(cache_fp)) {
                        oss << "encountered io error reading from the file\n";
                        AST_Module* mod = 0;
                        file_data = _reparse(fn, cache_fn, mod);
                        if (mod)
                            return mod;
                        assert(file_data.size());
                    }
                    break;
                }
            }
            fclose(cache_fp);
        }
    }

    static const int MAX_TRIES = 5;

    int tries = 0;
    while (true) {
        oss << "try number " << tries << '\n';

        bool good = true;

        if (file_data.size() < MAGIC_STRING_LENGTH + LENGTH_LENGTH + CHECKSUM_LENGTH) {
            oss << "file not long enough to include header\n";
            good = false;
        }

        if (good) {
            if (strncmp(&file_data[0], getMagic(), MAGIC_STRING_LENGTH) != 0) {
                oss << "magic string did not match\n";
                if (VERBOSITY() || tries == MAX_TRIES) {
                    fprintf(stderr, "Warning: corrupt or non-Pyston .pyc file found; ignoring\n");
                    fprintf(stderr, "%d %d %d %d\n", file_data[0], file_data[1], file_data[2], file_data[3]);
                    fprintf(stderr, "%d %d %d %d\n", getMagic()[0], getMagic()[1], getMagic()[2], getMagic()[3]);
                }
                good = false;
            }
        }

        if (good) {
            int length;
            static_assert(sizeof(length) == LENGTH_LENGTH, "");
            length = *reinterpret_cast<int*>(&file_data[MAGIC_STRING_LENGTH]);

            int expected_total_length = MAGIC_STRING_LENGTH + LENGTH_LENGTH + CHECKSUM_LENGTH + length;

            if (expected_total_length != file_data.size()) {
                oss << "length did not match\n";
                if (VERBOSITY() || tries == MAX_TRIES) {
                    fprintf(stderr, "Warning: truncated .pyc file found; ignoring\n");
                }
                good = false;
            } else {
                RELEASE_ASSERT(length > 0 && length < 10 * 1048576, "invalid file length: %d (file size is %ld)",
                               length, file_data.size());
            }
        }

        if (good) {
            uint8_t checksum;
            static_assert(sizeof(checksum) == CHECKSUM_LENGTH, "");
            checksum = *reinterpret_cast<uint8_t*>(&file_data[MAGIC_STRING_LENGTH + LENGTH_LENGTH]);

            for (int i = MAGIC_STRING_LENGTH + LENGTH_LENGTH + CHECKSUM_LENGTH; i < file_data.size(); i++) {
                checksum ^= file_data[i];
            }

            if (checksum != 0) {
                oss << "checksum did not match\n";
                if (VERBOSITY() || tries == MAX_TRIES)
                    fprintf(stderr, "pyc checksum failed!\n");
                good = false;
            }
        }

        if (good) {
            std::unique_ptr<BufferedReader> reader(
                new BufferedReader(file_data, MAGIC_STRING_LENGTH + LENGTH_LENGTH + CHECKSUM_LENGTH));
            AST* rtn = readASTMisc(reader.get());
            reader->fill();

            if (rtn && reader->bytesBuffered() == 0) {
                assert(rtn->type == AST_TYPE::Module);
                return ast_cast<AST_Module>(rtn);
            }

            oss << "returned NULL module\n";
            if (tries == MAX_TRIES)
                fprintf(stderr, "Returned NULL module?\n");
            good = false;
        }

        assert(!good);
        tries++;
        if (tries > MAX_TRIES) {
            fprintf(stderr, "\n%s\n", oss.str().c_str());
        }
        RELEASE_ASSERT(tries <= MAX_TRIES, "repeatedly failing to parse file");
        if (tries == MAX_TRIES)
            DEBUG_PARSING = true;

        if (!good) {
            file_data.clear();

            AST_Module* mod = 0;
            file_data = _reparse(fn, cache_fn, mod);
            if (mod)
                return mod;
            assert(file_data.size());
        }
    }
}
}
