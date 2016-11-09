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

#ifndef PYSTON_CORE_BST_H
#define PYSTON_CORE_BST_H

#include <cassert>
#include <cstdlib>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/scoping_analysis.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

namespace BST_TYPE {

#define FOREACH_TYPE(X)                                                                                                \
    X(Assert, 1)                                                                                                       \
    X(AugBinOp, 2)                                                                                                     \
    X(BinOp, 3)                                                                                                        \
    X(Branch, 4)                                                                                                       \
    X(CallAttr, 5)                                                                                                     \
    X(CallClsAttr, 6)                                                                                                  \
    X(CallFunc, 7)                                                                                                     \
    X(CheckExcMatch, 8)                                                                                                \
    X(ClassDef, 9)                                                                                                     \
    X(Compare, 10)                                                                                                     \
    X(CopyVReg, 11)                                                                                                    \
    X(DeleteAttr, 12)                                                                                                  \
    X(DeleteName, 13)                                                                                                  \
    X(DeleteSub, 14)                                                                                                   \
    X(DeleteSubSlice, 15)                                                                                              \
    X(Dict, 16)                                                                                                        \
    X(Exec, 17)                                                                                                        \
    X(FunctionDef, 18)                                                                                                 \
    X(GetIter, 19)                                                                                                     \
    X(HasNext, 20)                                                                                                     \
    X(ImportFrom, 21)                                                                                                  \
    X(ImportName, 22)                                                                                                  \
    X(ImportStar, 23)                                                                                                  \
    X(Invoke, 24)                                                                                                      \
    X(Jump, 25)                                                                                                        \
    X(Landingpad, 26)                                                                                                  \
    X(List, 27)                                                                                                        \
    X(LoadAttr, 28)                                                                                                    \
    X(LoadName, 29)                                                                                                    \
    X(LoadSub, 30)                                                                                                     \
    X(LoadSubSlice, 31)                                                                                                \
    X(Locals, 32)                                                                                                      \
    X(MakeClass, 33)                                                                                                   \
    X(MakeFunction, 34)                                                                                                \
    X(MakeSlice, 35)                                                                                                   \
    X(Nonzero, 36)                                                                                                     \
    X(Print, 37)                                                                                                       \
    X(PrintExpr, 38)                                                                                                   \
    X(Raise, 39)                                                                                                       \
    X(Repr, 40)                                                                                                        \
    X(Return, 41)                                                                                                      \
    X(Set, 42)                                                                                                         \
    X(SetExcInfo, 43)                                                                                                  \
    X(StoreAttr, 44)                                                                                                   \
    X(StoreName, 45)                                                                                                   \
    X(StoreSub, 46)                                                                                                    \
    X(StoreSubSlice, 47)                                                                                               \
    X(Tuple, 48)                                                                                                       \
    X(UnaryOp, 49)                                                                                                     \
    X(UncacheExcInfo, 50)                                                                                              \
    X(UnpackIntoArray, 51)                                                                                             \
    X(Yield, 52)

#define GENERATE_ENUM(ENUM, N) ENUM = N,
#define GENERATE_STRING(STRING, N) m[N] = #STRING;

enum BST_TYPE : unsigned char { FOREACH_TYPE(GENERATE_ENUM) };

static const char* stringify(int n) {
    static std::map<int, const char*> m;
    FOREACH_TYPE(GENERATE_STRING)
    return m[n];
}

#undef GENERATE_ENUM
#undef GENERATE_STRING
};

class BSTVisitor;
class ExprVisitor;
class StmtVisitor;

// Most nodes got a destination vreg and one or more source vregs. Currently all of them are 32bit long. Some nodes
// support a variable size of operands (e.g. the tuple node) but the size can't change after creating the node.
// In general all instructions except CopyVReg kill the source operand vregs except if the source is a constant. If one
// needs the preserve the source vreg on needs to create a new temporary using the CopyVReg opcode.

// There is a special vreg number: VREG_UNDEFINED
static constexpr int VREG_UNDEFINED = std::numeric_limits<int>::min();
// - when it's set as an operand vreg: it means that this is a not-set optional argument.
//   e.g. for a slice which only has lower set: upper would be VREG_UNDEFINED
// - if it's the destination it's means the result value should get immediately killed
//   e.g. "invoke 15 16: %undef = %11(%14)"
//    this is a call whose result gets ignored
//
// all other negative vreg numbers are indices into a constant table (after adding 1 and making them positive).
// Constants can be all str and numeric types, None and Ellipis. Every constant will only get stored once in the table.
// e.g. (4, 2, 'lala') generates: "%undef = (%-1|4|, %-2|2|, %-3|'lala'|)"
//  this creates a tuple whose elements are the constant idx -1, -2 and -3.
//  in order to make it easier for a human to understand we print the actual value of the constant between | characters.


#define PACKED __attribute__((packed)) __attribute__((__aligned__(1)))

class BST_stmt {
public:
    const BST_TYPE::BST_TYPE type;
    uint32_t lineno;

    void accept(BSTVisitor* v);
    void accept_stmt(StmtVisitor* v);

    inline int size_in_bytes() const __attribute__((always_inline));
    inline bool has_dest_vreg() const __attribute__((always_inline));
    bool is_terminator() const __attribute__((always_inline)) {
        return type == BST_TYPE::Branch || type == BST_TYPE::Jump || type == BST_TYPE::Invoke
               || type == BST_TYPE::Return || type == BST_TYPE::Raise || type == BST_TYPE::Assert;
    }


// #define DEBUG_LINE_NUMBERS 1
#ifdef DEBUG_LINE_NUMBERS
private:
    // Initialize lineno to something unique, so that if we see something ridiculous
    // appear in the traceback, we can isolate the allocation which created it.
    static int next_lineno;

public:
    BST_stmt(BST_TYPE::BST_TYPE type);
#else
    BST_stmt(BST_TYPE::BST_TYPE type) : type(type), lineno(0) {}
#endif
    BST_stmt(BST_TYPE::BST_TYPE type, uint32_t lineno) : type(type), lineno(lineno) {}
} PACKED;

// base class of all nodes which have a single destination vreg
class BST_stmt_with_dest : public BST_stmt {
public:
    int vreg_dst = VREG_UNDEFINED;
    BST_stmt_with_dest(BST_TYPE::BST_TYPE type) : BST_stmt(type) {}
    BST_stmt_with_dest(BST_TYPE::BST_TYPE type, int lineno) : BST_stmt(type, lineno) {}
} PACKED;

#define BSTNODE(opcode)                                                                                                \
    void accept(BSTVisitor* v);                                                                                        \
    void accept_stmt(StmtVisitor* v);                                                                                  \
    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::opcode;

#define BSTFIXEDVREGS(opcode, base_class)                                                                              \
    BSTNODE(opcode)                                                                                                    \
    BST_##opcode() : base_class(BST_TYPE::opcode) {}                                                                   \
    int size_in_bytes() const { return sizeof(*this); }

#define BSTVARVREGS(opcode, base_class, num_elts, vreg_dst)                                                            \
public:                                                                                                                \
    BSTNODE(opcode)                                                                                                    \
    static BST_##opcode* create(int num_elts) { return new (num_elts) BST_##opcode(num_elts); }                        \
    static void operator delete(void* ptr) { ::operator delete[](ptr); }                                               \
    int size_in_bytes() const { return offsetof(BST_##opcode, vreg_dst) + num_elts * sizeof(int); }                    \
                                                                                                                       \
private:                                                                                                               \
    static void* operator new(size_t, int num_elts) {                                                                  \
        return ::new char[offsetof(BST_##opcode, vreg_dst) + num_elts * sizeof(int)];                                  \
    }                                                                                                                  \
    BST_##opcode(int num_elts) : base_class(BST_TYPE::opcode), num_elts(num_elts) {                                    \
        for (int i = 0; i < num_elts; ++i) {                                                                           \
            vreg_dst[i] = VREG_UNDEFINED;                                                                              \
        }                                                                                                              \
    }

#define BSTVARVREGS2(opcode, base_class, num_elts, num_elts2, vreg_dst)                                                \
public:                                                                                                                \
    BSTNODE(opcode)                                                                                                    \
    static BST_##opcode* create(int num_elts, int num_elts2) {                                                         \
        return new (num_elts + num_elts2) BST_##opcode(num_elts, num_elts2);                                           \
    }                                                                                                                  \
    static void operator delete(void* ptr) { ::operator delete[](ptr); }                                               \
    int size_in_bytes() const { return offsetof(BST_##opcode, vreg_dst) + (num_elts + num_elts2) * sizeof(int); }      \
                                                                                                                       \
private:                                                                                                               \
    static void* operator new(size_t, int num_elts_total) {                                                            \
        return ::new char[offsetof(BST_##opcode, vreg_dst) + num_elts_total * sizeof(int)];                            \
    }                                                                                                                  \
    BST_##opcode(int num_elts, int num_elts2)                                                                          \
        : base_class(BST_TYPE::opcode), num_elts(num_elts), num_elts2(num_elts2) {                                     \
        for (int i = 0; i < num_elts + num_elts2; ++i) {                                                               \
            vreg_dst[i] = VREG_UNDEFINED;                                                                              \
        }                                                                                                              \
    }

#define BSTVARVREGS2CALL(opcode, num_elts, num_elts2, vreg_dst)                                                        \
public:                                                                                                                \
    BSTNODE(opcode)                                                                                                    \
    static BST_##opcode* create(int num_elts, int num_elts2) {                                                         \
        return new (num_elts + num_elts2) BST_##opcode(num_elts, num_elts2);                                           \
    }                                                                                                                  \
    static void operator delete(void* ptr) { ::operator delete[](ptr); }                                               \
    int size_in_bytes() const { return offsetof(BST_##opcode, vreg_dst) + (num_elts + num_elts2) * sizeof(int); }      \
                                                                                                                       \
private:                                                                                                               \
    static void* operator new(size_t, int num_elts_total) {                                                            \
        return ::new char[offsetof(BST_##opcode, vreg_dst) + num_elts_total * sizeof(int)];                            \
    }                                                                                                                  \
    BST_##opcode(int num_elts, int num_elts2) : BST_Call(BST_TYPE::opcode, num_elts, num_elts2) {                      \
        for (int i = 0; i < num_elts + num_elts2; ++i) {                                                               \
            vreg_dst[i] = VREG_UNDEFINED;                                                                              \
        }                                                                                                              \
    }

class BST_Assert : public BST_stmt {
public:
    int vreg_msg = VREG_UNDEFINED;

    BSTFIXEDVREGS(Assert, BST_stmt)
} PACKED;

class BST_UnpackIntoArray : public BST_stmt {
public:
    int vreg_src = VREG_UNDEFINED;
    const int num_elts;
    int vreg_dst[1];

    BSTVARVREGS(UnpackIntoArray, BST_stmt, num_elts, vreg_dst)
} PACKED;

// This is a special instruction which copies a vreg without destroying the source.
// All other instructions always kill the operands (except if they are a constant) so if one needs the operand to stay
// alive one has to create a copy usning CopyVReg first.
class BST_CopyVReg : public BST_stmt_with_dest {
public:
    int vreg_src = VREG_UNDEFINED; // this vreg will not get killed!

    BSTFIXEDVREGS(CopyVReg, BST_stmt_with_dest)
} PACKED;


class BST_StoreName : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;

    int index_id = VREG_UNDEFINED;
    ScopeInfo::VarScopeType lookup_type;
    int vreg = VREG_UNDEFINED;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    BSTFIXEDVREGS(StoreName, BST_stmt)
} PACKED;

class BST_StoreAttr : public BST_stmt {
public:
    int index_attr = VREG_UNDEFINED;
    int vreg_target = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(StoreAttr, BST_stmt)
} PACKED;

class BST_StoreSub : public BST_stmt {
public:
    int vreg_target = VREG_UNDEFINED;
    int vreg_slice = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(StoreSub, BST_stmt)
} PACKED;

class BST_StoreSubSlice : public BST_stmt {
public:
    int vreg_target = VREG_UNDEFINED;
    int vreg_lower = VREG_UNDEFINED, vreg_upper = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(StoreSubSlice, BST_stmt)
} PACKED;

class BST_LoadName : public BST_stmt_with_dest {
public:
    int index_id = VREG_UNDEFINED;
    ScopeInfo::VarScopeType lookup_type;
    // LoadName does not kill this vreg
    int vreg = VREG_UNDEFINED;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    BSTFIXEDVREGS(LoadName, BST_stmt_with_dest)
} PACKED;

class BST_LoadAttr : public BST_stmt_with_dest {
public:
    int index_attr = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;
    bool clsonly = false;

    BSTFIXEDVREGS(LoadAttr, BST_stmt_with_dest)
} PACKED;

class BST_LoadSub : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_slice = VREG_UNDEFINED;

    BSTFIXEDVREGS(LoadSub, BST_stmt_with_dest)
} PACKED;

class BST_LoadSubSlice : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_lower = VREG_UNDEFINED, vreg_upper = VREG_UNDEFINED;

    BSTFIXEDVREGS(LoadSubSlice, BST_stmt_with_dest)
} PACKED;

class BST_AugBinOp : public BST_stmt_with_dest {
public:
    AST_TYPE::AST_TYPE op_type;
    int vreg_left = VREG_UNDEFINED, vreg_right = VREG_UNDEFINED;

    BSTFIXEDVREGS(AugBinOp, BST_stmt_with_dest)
} PACKED;

class BST_BinOp : public BST_stmt_with_dest {
public:
    AST_TYPE::AST_TYPE op_type;
    int vreg_left = VREG_UNDEFINED, vreg_right = VREG_UNDEFINED;

    BSTFIXEDVREGS(BinOp, BST_stmt_with_dest)
} PACKED;

class BST_Call : public BST_stmt_with_dest {
public:
    int vreg_starargs = VREG_UNDEFINED, vreg_kwargs = VREG_UNDEFINED;
    const int num_args;
    const int num_keywords;

    int index_keyword_names = -1;

    BST_Call(BST_TYPE::BST_TYPE type, int num_args, int num_keywords)
        : BST_stmt_with_dest(type), num_args(num_args), num_keywords(num_keywords) {}
} PACKED;

class BST_CallFunc : public BST_Call {
public:
    int vreg_func = VREG_UNDEFINED;
    int elts[1];

    BSTVARVREGS2CALL(CallFunc, num_args, num_keywords, elts)
} PACKED;

class BST_CallAttr : public BST_Call {
public:
    int vreg_value = VREG_UNDEFINED;
    int index_attr = VREG_UNDEFINED;
    int elts[1];

    BSTVARVREGS2CALL(CallAttr, num_args, num_keywords, elts)
} PACKED;

class BST_CallClsAttr : public BST_Call {
public:
    int vreg_value = VREG_UNDEFINED;
    int index_attr = VREG_UNDEFINED;
    int elts[1];

    BSTVARVREGS2CALL(CallClsAttr, num_args, num_keywords, elts)
} PACKED;


class BST_Compare : public BST_stmt_with_dest {
public:
    AST_TYPE::AST_TYPE op;
    int vreg_comparator = VREG_UNDEFINED;
    int vreg_left = VREG_UNDEFINED;

    BSTFIXEDVREGS(Compare, BST_stmt_with_dest)
} PACKED;

class BST_ClassDef : public BST_stmt {
public:
    int index_name = VREG_UNDEFINED;
    int vreg_bases_tuple = VREG_UNDEFINED;
    const int num_decorator;
    int decorator[1];

    BSTVARVREGS(ClassDef, BST_stmt, num_decorator, decorator)
} PACKED;

class BST_Dict : public BST_stmt_with_dest {
public:
    BSTFIXEDVREGS(Dict, BST_stmt_with_dest)
} PACKED;

class BST_DeleteAttr : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;
    int index_attr = VREG_UNDEFINED;

    BSTFIXEDVREGS(DeleteAttr, BST_stmt)
} PACKED;

class BST_DeleteName : public BST_stmt {
public:
    int index_id = VREG_UNDEFINED;
    ScopeInfo::VarScopeType lookup_type;
    int vreg = VREG_UNDEFINED;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    BSTFIXEDVREGS(DeleteName, BST_stmt)
} PACKED;

class BST_DeleteSub : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_slice = VREG_UNDEFINED;

    BSTFIXEDVREGS(DeleteSub, BST_stmt)
} PACKED;

class BST_DeleteSubSlice : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_lower = VREG_UNDEFINED;
    int vreg_upper = VREG_UNDEFINED;

    BSTFIXEDVREGS(DeleteSubSlice, BST_stmt)
} PACKED;

class BST_Exec : public BST_stmt {
public:
    int vreg_body = VREG_UNDEFINED;
    int vreg_globals = VREG_UNDEFINED;
    int vreg_locals = VREG_UNDEFINED;

    BSTFIXEDVREGS(Exec, BST_stmt)
} PACKED;

class BST_FunctionDef : public BST_stmt {
public:
    int index_name = VREG_UNDEFINED; // if the name is not set this is a lambda

    const int num_decorator;
    const int num_defaults;

    int elts[1]; // decorators followed by defaults

    BSTVARVREGS2(FunctionDef, BST_stmt, num_decorator, num_defaults, elts)
} PACKED;

class BST_List : public BST_stmt_with_dest {
public:
    const int num_elts;
    int elts[1];

    BSTVARVREGS(List, BST_stmt_with_dest, num_elts, elts)
} PACKED;

class BST_Repr : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(Repr, BST_stmt_with_dest)
} PACKED;

class BST_Print : public BST_stmt {
public:
    int vreg_dest = VREG_UNDEFINED;
    bool nl;
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(Print, BST_stmt)
} PACKED;

class BST_Raise : public BST_stmt {
public:
    // In the python ast module, these are called "type", "inst", and "tback", respectively.
    // Renaming to arg{0..2} since I find that confusing, since they are filled in
    // sequentially rather than semantically.
    // Ie "raise Exception()" will have type==Exception(), inst==None, tback==None
    int vreg_arg0 = VREG_UNDEFINED, vreg_arg1 = VREG_UNDEFINED, vreg_arg2 = VREG_UNDEFINED;

    BSTFIXEDVREGS(Raise, BST_stmt)
} PACKED;

class BST_Return : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(Return, BST_stmt)
} PACKED;

class BST_Set : public BST_stmt_with_dest {
public:
    const int num_elts;
    int elts[1];

    BSTVARVREGS(Set, BST_stmt_with_dest, num_elts, elts)
} PACKED;

class BST_MakeSlice : public BST_stmt_with_dest {
public:
    int vreg_lower = VREG_UNDEFINED, vreg_upper = VREG_UNDEFINED, vreg_step = VREG_UNDEFINED;

    BSTFIXEDVREGS(MakeSlice, BST_stmt_with_dest)
} PACKED;

class BST_Tuple : public BST_stmt_with_dest {
public:
    const int num_elts;
    int elts[1];

    BSTVARVREGS(Tuple, BST_stmt_with_dest, num_elts, elts)
} PACKED;

class BST_UnaryOp : public BST_stmt_with_dest {
public:
    int vreg_operand = VREG_UNDEFINED;
    AST_TYPE::AST_TYPE op_type;

    BSTFIXEDVREGS(UnaryOp, BST_stmt_with_dest)
} PACKED;

class BST_Yield : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(Yield, BST_stmt_with_dest)
} PACKED;

class BST_MakeFunction : public BST_stmt_with_dest {
public:
    const int index_func_def;

    BST_MakeFunction(BST_FunctionDef* fd, int index_func_def)
        : BST_stmt_with_dest(BST_TYPE::MakeFunction, fd->lineno), index_func_def(index_func_def) {}
    int size_in_bytes() const { return sizeof(*this); }

    BSTNODE(MakeFunction)
} PACKED;

class BST_MakeClass : public BST_stmt_with_dest {
public:
    const int index_class_def;

    BST_MakeClass(BST_ClassDef* cd, int index_class_def)
        : BST_stmt_with_dest(BST_TYPE::MakeClass, cd->lineno), index_class_def(index_class_def) {}
    int size_in_bytes() const { return sizeof(*this); }

    BSTNODE(MakeClass)
} PACKED;

class CFGBlock;

class BST_Branch : public BST_stmt {
public:
    int vreg_test = VREG_UNDEFINED;
    CFGBlock* iftrue, *iffalse;

    BSTFIXEDVREGS(Branch, BST_stmt)
} PACKED;

class BST_Jump : public BST_stmt {
public:
    CFGBlock* target;

    BSTFIXEDVREGS(Jump, BST_stmt)
} PACKED;

// This instruction is special because it contains a pointer to another stmt
// After the final bytecode is constructed the stmt will always be exactly after the invoke
// TODO: we should never emit this instruction in the final bytecode but instead use a bit in the opcode to specify
// invokes and append the blocks directly after the stmt
class BST_Invoke : public BST_stmt {
public:
    BST_stmt* stmt;

    CFGBlock* normal_dest, *exc_dest;

    BST_Invoke(BST_stmt* stmt) : BST_stmt(BST_TYPE::Invoke), stmt(stmt) {}
    int size_in_bytes() const { return sizeof(*this); }

    BSTNODE(Invoke)
} PACKED;


// grabs the info about the last raised exception
class BST_Landingpad : public BST_stmt_with_dest {
public:
    BSTFIXEDVREGS(Landingpad, BST_stmt_with_dest)
} PACKED;

class BST_Locals : public BST_stmt_with_dest {
public:
    BSTFIXEDVREGS(Locals, BST_stmt_with_dest)
} PACKED;

class BST_GetIter : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(GetIter, BST_stmt_with_dest)
} PACKED;

class BST_ImportFrom : public BST_stmt_with_dest {
public:
    int vreg_module = VREG_UNDEFINED;
    int vreg_name = VREG_UNDEFINED;

    BSTFIXEDVREGS(ImportFrom, BST_stmt_with_dest)
} PACKED;

class BST_ImportName : public BST_stmt_with_dest {
public:
    int vreg_from = VREG_UNDEFINED;
    int level = VREG_UNDEFINED;
    int vreg_name = VREG_UNDEFINED;

    BSTFIXEDVREGS(ImportName, BST_stmt_with_dest)
} PACKED;

class BST_ImportStar : public BST_stmt_with_dest {
public:
    int vreg_name = VREG_UNDEFINED;

    BSTFIXEDVREGS(ImportStar, BST_stmt_with_dest)
} PACKED;

// determines whether something is "true" for purposes of `if' and so forth
class BST_Nonzero : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(Nonzero, BST_stmt_with_dest)
} PACKED;

class BST_CheckExcMatch : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_cls = VREG_UNDEFINED;

    BSTFIXEDVREGS(CheckExcMatch, BST_stmt_with_dest)
} PACKED;

class BST_SetExcInfo : public BST_stmt {
public:
    int vreg_type = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;
    int vreg_traceback = VREG_UNDEFINED;

    BSTFIXEDVREGS(SetExcInfo, BST_stmt)
} PACKED;

class BST_UncacheExcInfo : public BST_stmt {
public:
    BSTFIXEDVREGS(UncacheExcInfo, BST_stmt)
} PACKED;

class BST_HasNext : public BST_stmt_with_dest {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(HasNext, BST_stmt_with_dest)
} PACKED;

class BST_PrintExpr : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;

    BSTFIXEDVREGS(PrintExpr, BST_stmt)
} PACKED;


template <typename T> T* bst_cast(const BST_stmt* node) {
    ASSERT(!node || node->type == T::TYPE, "%d", node ? node->type : 0);
    return static_cast<T*>(node);
}

int BST_stmt::size_in_bytes() const {
    switch (type) {
#define DISPATCH_SIZE(x, y)                                                                                            \
    case BST_TYPE::x:                                                                                                  \
        return bst_cast<const BST_##x>(this)->size_in_bytes();
        FOREACH_TYPE(DISPATCH_SIZE)
    };
    assert(0);
    __builtin_unreachable();
#undef DISPATCH_SIZE
}

bool BST_stmt::has_dest_vreg() const {
    switch (type) {
#define DISPATCH_HAS_DEST(x, y)                                                                                        \
    case BST_TYPE::x:                                                                                                  \
        return std::is_base_of<BST_stmt_with_dest, BST_##x>();
        FOREACH_TYPE(DISPATCH_HAS_DEST)
    };
    assert(0);
    __builtin_unreachable();
#undef DISPATCH_HAS_DEST
}

// this is not a real bytecode it's only used to initalize arguments
class BST_Name {
public:
    InternedString id;

    // The resolved scope of this name.  Kind of hacky to be storing it in the BST node;
    // in CPython it ends up getting "cached" by being translated into one of a number of
    // different bytecodes.
    ScopeInfo::VarScopeType lookup_type;

    // These are only valid for lookup_type == FAST or CLOSURE
    // The interpreter and baseline JIT store variables with FAST and CLOSURE scopes in an array (vregs) this specifies
    // the zero based index of this variable inside the vregs array. If uninitialized it's value is VREG_UNDEFINED.
    int vreg;

    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    BST_Name(InternedString id, int lineno)
        : id(id), lookup_type(ScopeInfo::VarScopeType::UNKNOWN), vreg(VREG_UNDEFINED) {}
};

template <typename T> T* bst_cast(BST_stmt* node) {
    ASSERT(!node || node->type == T::TYPE, "%d", node ? node->type : 0);
    return static_cast<T*>(node);
}

class CodeConstants;
class BSTVisitor {
public:
    const CodeConstants& code_constants;
    BSTVisitor(const CodeConstants& code_constants) : code_constants(code_constants) {}
    virtual ~BSTVisitor() {}

    const CodeConstants& getCodeConstants() const { return code_constants; }

    // pseudo
    virtual bool visit_vreg(int* vreg, bool is_dst = false) { RELEASE_ASSERT(0, ""); }

    virtual bool visit_assert(BST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_augbinop(BST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_binop(BST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_branch(BST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_callattr(BST_CallAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_callclsattr(BST_CallClsAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_callfunc(BST_CallFunc* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_classdef(BST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_compare(BST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_copyvreg(BST_CopyVReg* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deleteattr(BST_DeleteAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deletename(BST_DeleteName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deletesub(BST_DeleteSub* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_dict(BST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_exec(BST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_functiondef(BST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_getiter(BST_GetIter* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_hasnext(BST_HasNext* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importfrom(BST_ImportFrom* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importname(BST_ImportName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importstar(BST_ImportStar* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_invoke(BST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_jump(BST_Jump* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_landingpad(BST_Landingpad* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_list(BST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadattr(BST_LoadAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadname(BST_LoadName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadsub(BST_LoadSub* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_locals(BST_Locals* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makeclass(BST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makefunction(BST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makeslice(BST_MakeSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_nonzero(BST_Nonzero* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_print(BST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_printexpr(BST_PrintExpr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_raise(BST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_repr(BST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_return(BST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_set(BST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_setexcinfo(BST_SetExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storeattr(BST_StoreAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storename(BST_StoreName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storesub(BST_StoreSub* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storesubslice(BST_StoreSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_tuple(BST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_unaryop(BST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_yield(BST_Yield* node) { RELEASE_ASSERT(0, ""); }
};

class NoopBSTVisitor : public BSTVisitor {
protected:
public:
    NoopBSTVisitor(const CodeConstants& code_constants) : BSTVisitor(code_constants) {}
    virtual ~NoopBSTVisitor() {}

    virtual bool visit_assert(BST_Assert* node) override { return false; }
    virtual bool visit_augbinop(BST_AugBinOp* node) override { return false; }
    virtual bool visit_binop(BST_BinOp* node) override { return false; }
    virtual bool visit_branch(BST_Branch* node) override { return false; }
    virtual bool visit_callattr(BST_CallAttr* node) override { return false; }
    virtual bool visit_callclsattr(BST_CallClsAttr* node) override { return false; }
    virtual bool visit_callfunc(BST_CallFunc* node) override { return false; }
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node) override { return false; }
    virtual bool visit_classdef(BST_ClassDef* node) override { return false; }
    virtual bool visit_compare(BST_Compare* node) override { return false; }
    virtual bool visit_copyvreg(BST_CopyVReg* node) override { return false; }
    virtual bool visit_deleteattr(BST_DeleteAttr* node) override { return false; }
    virtual bool visit_deletename(BST_DeleteName* node) override { return false; }
    virtual bool visit_deletesub(BST_DeleteSub* node) override { return false; }
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node) override { return false; }
    virtual bool visit_dict(BST_Dict* node) override { return false; }
    virtual bool visit_exec(BST_Exec* node) override { return false; }
    virtual bool visit_functiondef(BST_FunctionDef* node) override { return false; }
    virtual bool visit_getiter(BST_GetIter* node) override { return false; }
    virtual bool visit_hasnext(BST_HasNext* node) override { return false; }
    virtual bool visit_importfrom(BST_ImportFrom* node) override { return false; }
    virtual bool visit_importname(BST_ImportName* node) override { return false; }
    virtual bool visit_importstar(BST_ImportStar* node) override { return false; }
    virtual bool visit_invoke(BST_Invoke* node) override { return false; }
    virtual bool visit_jump(BST_Jump* node) override { return false; }
    virtual bool visit_landingpad(BST_Landingpad* node) override { return false; }
    virtual bool visit_list(BST_List* node) override { return false; }
    virtual bool visit_loadattr(BST_LoadAttr* node) override { return false; }
    virtual bool visit_loadname(BST_LoadName* node) override { return false; }
    virtual bool visit_loadsub(BST_LoadSub* node) override { return false; }
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node) override { return false; }
    virtual bool visit_locals(BST_Locals* node) override { return false; }
    virtual bool visit_makeclass(BST_MakeClass* node) override { return false; }
    virtual bool visit_makefunction(BST_MakeFunction* node) override { return false; }
    virtual bool visit_makeslice(BST_MakeSlice* node) override { return false; }
    virtual bool visit_nonzero(BST_Nonzero* node) override { return false; }
    virtual bool visit_print(BST_Print* node) override { return false; }
    virtual bool visit_printexpr(BST_PrintExpr* node) override { return false; }
    virtual bool visit_raise(BST_Raise* node) override { return false; }
    virtual bool visit_repr(BST_Repr* node) override { return false; }
    virtual bool visit_return(BST_Return* node) override { return false; }
    virtual bool visit_set(BST_Set* node) override { return false; }
    virtual bool visit_setexcinfo(BST_SetExcInfo* node) override { return false; }
    virtual bool visit_storeattr(BST_StoreAttr* node) override { return false; }
    virtual bool visit_storename(BST_StoreName* node) override { return false; }
    virtual bool visit_storesub(BST_StoreSub* node) override { return false; }
    virtual bool visit_storesubslice(BST_StoreSubSlice* node) override { return false; }
    virtual bool visit_tuple(BST_Tuple* node) override { return false; }
    virtual bool visit_unaryop(BST_UnaryOp* node) override { return false; }
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node) override { return false; }
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node) override { return false; }
    virtual bool visit_yield(BST_Yield* node) override { return false; }
};

class StmtVisitor {
protected:
public:
    const CodeConstants& code_constants;
    StmtVisitor(const CodeConstants& code_constants) : code_constants(code_constants) {}
    virtual ~StmtVisitor() {}

    const CodeConstants& getCodeConstants() const { return code_constants; }

    virtual void visit_assert(BST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_augbinop(BST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_binop(BST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_branch(BST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_callattr(BST_CallAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_callclsattr(BST_CallClsAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_callfunc(BST_CallFunc* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_checkexcmatch(BST_CheckExcMatch* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_classdef(BST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_compare(BST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_copyvreg(BST_CopyVReg* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deleteattr(BST_DeleteAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deletename(BST_DeleteName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deletesub(BST_DeleteSub* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deletesubslice(BST_DeleteSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_dict(BST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_exec(BST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_functiondef(BST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_getiter(BST_GetIter* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_hasnext(BST_HasNext* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importfrom(BST_ImportFrom* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importname(BST_ImportName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importstar(BST_ImportStar* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_invoke(BST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_jump(BST_Jump* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_landingpad(BST_Landingpad* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_list(BST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadattr(BST_LoadAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadname(BST_LoadName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadsub(BST_LoadSub* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadsubslice(BST_LoadSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_locals(BST_Locals* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_makeclass(BST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_makefunction(BST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_makeslice(BST_MakeSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_nonzero(BST_Nonzero* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_print(BST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_printexpr(BST_PrintExpr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_raise(BST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_repr(BST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_return(BST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_set(BST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_setexcinfo(BST_SetExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storeattr(BST_StoreAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storename(BST_StoreName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storesub(BST_StoreSub* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storesubslice(BST_StoreSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_tuple(BST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_unaryop(BST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_uncacheexcinfo(BST_UncacheExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_unpackintoarray(BST_UnpackIntoArray* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_yield(BST_Yield* node) { RELEASE_ASSERT(0, ""); }
};

void print_bst(BST_stmt* bst, const CodeConstants& code_constants);

class PrintVisitor : public BSTVisitor {
private:
    llvm::raw_ostream& stream;
    int indent;
    void printIndent();
    void printOp(AST_TYPE::AST_TYPE op_type);

public:
    PrintVisitor(const CodeConstants& code_constants, int indent, llvm::raw_ostream& stream)
        : BSTVisitor(code_constants), stream(stream), indent(indent) {}
    virtual ~PrintVisitor() {}
    void flush() { stream.flush(); }

    virtual bool visit_vreg(int* vreg, bool is_dst = false);

    virtual bool visit_assert(BST_Assert* node);
    virtual bool visit_augbinop(BST_AugBinOp* node);
    virtual bool visit_binop(BST_BinOp* node);
    virtual bool visit_branch(BST_Branch* node);
    virtual bool visit_callattr(BST_CallAttr* node);
    virtual bool visit_callclsattr(BST_CallClsAttr* node);
    virtual bool visit_callfunc(BST_CallFunc* node);
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node);
    virtual bool visit_classdef(BST_ClassDef* node);
    virtual bool visit_compare(BST_Compare* node);
    virtual bool visit_copyvreg(BST_CopyVReg* node);
    virtual bool visit_deleteattr(BST_DeleteAttr* node);
    virtual bool visit_deletename(BST_DeleteName* node);
    virtual bool visit_deletesub(BST_DeleteSub* node);
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node);
    virtual bool visit_dict(BST_Dict* node);
    virtual bool visit_exec(BST_Exec* node);
    virtual bool visit_functiondef(BST_FunctionDef* node);
    virtual bool visit_getiter(BST_GetIter* node);
    virtual bool visit_hasnext(BST_HasNext* node);
    virtual bool visit_importfrom(BST_ImportFrom* node);
    virtual bool visit_importname(BST_ImportName* node);
    virtual bool visit_importstar(BST_ImportStar* node);
    virtual bool visit_invoke(BST_Invoke* node);
    virtual bool visit_jump(BST_Jump* node);
    virtual bool visit_landingpad(BST_Landingpad* node);
    virtual bool visit_list(BST_List* node);
    virtual bool visit_loadattr(BST_LoadAttr* node);
    virtual bool visit_loadname(BST_LoadName* node);
    virtual bool visit_loadsub(BST_LoadSub* node);
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node);
    virtual bool visit_locals(BST_Locals* node);
    virtual bool visit_makeclass(BST_MakeClass* node);
    virtual bool visit_makefunction(BST_MakeFunction* node);
    virtual bool visit_makeslice(BST_MakeSlice* node);
    virtual bool visit_nonzero(BST_Nonzero* node);
    virtual bool visit_print(BST_Print* node);
    virtual bool visit_printexpr(BST_PrintExpr* node);
    virtual bool visit_raise(BST_Raise* node);
    virtual bool visit_repr(BST_Repr* node);
    virtual bool visit_return(BST_Return* node);
    virtual bool visit_set(BST_Set* node);
    virtual bool visit_setexcinfo(BST_SetExcInfo* node);
    virtual bool visit_storeattr(BST_StoreAttr* node);
    virtual bool visit_storename(BST_StoreName* node);
    virtual bool visit_storesub(BST_StoreSub* node);
    virtual bool visit_storesubslice(BST_StoreSubSlice* node);
    virtual bool visit_tuple(BST_Tuple* node);
    virtual bool visit_unaryop(BST_UnaryOp* node);
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node);
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node);
    virtual bool visit_yield(BST_Yield* node);
};
}

#endif
