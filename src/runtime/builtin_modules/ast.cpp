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

#include <algorithm>
#include <cmath>
#include <langinfo.h>
#include <sstream>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include "codegen/unwinding.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/file.h"
#include "runtime/inline/boxing.h"
#include "runtime/int.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

static BoxedClass* AST_cls;

class BoxedAST : public Box {
public:
    AST* ast;

    BoxedAST() {}
};

static std::unordered_map<int, BoxedClass*> type_to_cls;

Box* boxAst(AST* ast) {
    assert(ast);
    BoxedClass* cls = type_to_cls[ast->type];
    assert(cls);

    BoxedAST* rtn = new (cls) BoxedAST();
    assert(rtn->cls == cls);
    rtn->ast = ast;
    return rtn;
}

AST* unboxAst(Box* b) {
    assert(isSubclass(b->cls, AST_cls));
    AST* rtn = static_cast<BoxedAST*>(b)->ast;
    assert(rtn);
    return rtn;
}

extern "C" int PyAST_Check(PyObject* o) noexcept {
    return isSubclass(o->cls, AST_cls);
}

void setupAST() {
    BoxedModule* ast_module = createModule("_ast", "__builtin__");

    ast_module->giveAttr("PyCF_ONLY_AST", boxInt(PyCF_ONLY_AST));

// ::create takes care of registering the class as a GC root.
#define MAKE_CLS(name, base_cls)                                                                                       \
    BoxedClass* name##_cls = BoxedHeapClass::create(type_cls, base_cls, /* gchandler = */ NULL, 0, 0,                  \
                                                    sizeof(BoxedAST), false, STRINGIFY(name));                         \
    ast_module->giveAttr(STRINGIFY(name), name##_cls);                                                                 \
    type_to_cls[AST_TYPE::name] = name##_cls;                                                                          \
    name##_cls->giveAttr("__module__", boxString("_ast"));                                                             \
    name##_cls->freeze()

    AST_cls
        = BoxedHeapClass::create(type_cls, object_cls, /* gchandler = */ NULL, 0, 0, sizeof(BoxedAST), false, "AST");
    // ::create takes care of registering the class as a GC root.
    AST_cls->giveAttr("__module__", boxString("_ast"));
    AST_cls->freeze();

    // TODO(kmod) you can call the class constructors, such as "ast.AST()", so we need new/init
    // TODO(kmod) there is more inheritance than "they all inherit from AST"

    MAKE_CLS(alias, AST_cls);
    MAKE_CLS(arguments, AST_cls);
    MAKE_CLS(Assert, AST_cls);
    MAKE_CLS(Assign, AST_cls);
    MAKE_CLS(Attribute, AST_cls);
    MAKE_CLS(AugAssign, AST_cls);
    MAKE_CLS(BinOp, AST_cls);
    MAKE_CLS(BoolOp, AST_cls);
    MAKE_CLS(Call, AST_cls);
    MAKE_CLS(ClassDef, AST_cls);
    MAKE_CLS(Compare, AST_cls);
    MAKE_CLS(comprehension, AST_cls);
    MAKE_CLS(Delete, AST_cls);
    MAKE_CLS(Dict, AST_cls);
    MAKE_CLS(Exec, AST_cls);
    MAKE_CLS(ExceptHandler, AST_cls);
    MAKE_CLS(ExtSlice, AST_cls);
    MAKE_CLS(Expr, AST_cls);
    MAKE_CLS(For, AST_cls);
    MAKE_CLS(FunctionDef, AST_cls);
    MAKE_CLS(GeneratorExp, AST_cls);
    MAKE_CLS(Global, AST_cls);
    MAKE_CLS(If, AST_cls);
    MAKE_CLS(IfExp, AST_cls);
    MAKE_CLS(Import, AST_cls);
    MAKE_CLS(ImportFrom, AST_cls);
    MAKE_CLS(Index, AST_cls);
    MAKE_CLS(keyword, AST_cls);
    MAKE_CLS(Lambda, AST_cls);
    MAKE_CLS(List, AST_cls);
    MAKE_CLS(ListComp, AST_cls);
    MAKE_CLS(Module, AST_cls);
    MAKE_CLS(Num, AST_cls);
    MAKE_CLS(Name, AST_cls);
    MAKE_CLS(Pass, AST_cls);
    MAKE_CLS(Pow, AST_cls);
    MAKE_CLS(Print, AST_cls);
    MAKE_CLS(Raise, AST_cls);
    MAKE_CLS(Repr, AST_cls);
    MAKE_CLS(Return, AST_cls);
    MAKE_CLS(Slice, AST_cls);
    MAKE_CLS(Str, AST_cls);
    MAKE_CLS(Subscript, AST_cls);
    MAKE_CLS(TryExcept, AST_cls);
    MAKE_CLS(TryFinally, AST_cls);
    MAKE_CLS(Tuple, AST_cls);
    MAKE_CLS(UnaryOp, AST_cls);
    MAKE_CLS(With, AST_cls);
    MAKE_CLS(While, AST_cls);
    MAKE_CLS(Yield, AST_cls);
    MAKE_CLS(Store, AST_cls);
    MAKE_CLS(Load, AST_cls);
    MAKE_CLS(Param, AST_cls);
    MAKE_CLS(Not, AST_cls);
    MAKE_CLS(In, AST_cls);
    MAKE_CLS(Is, AST_cls);
    MAKE_CLS(IsNot, AST_cls);
    MAKE_CLS(Or, AST_cls);
    MAKE_CLS(And, AST_cls);
    MAKE_CLS(Eq, AST_cls);
    MAKE_CLS(NotEq, AST_cls);
    MAKE_CLS(NotIn, AST_cls);
    MAKE_CLS(GtE, AST_cls);
    MAKE_CLS(Gt, AST_cls);
    MAKE_CLS(Mod, AST_cls);
    MAKE_CLS(Add, AST_cls);
    MAKE_CLS(Continue, AST_cls);
    MAKE_CLS(Lt, AST_cls);
    MAKE_CLS(LtE, AST_cls);
    MAKE_CLS(Break, AST_cls);
    MAKE_CLS(Sub, AST_cls);
    MAKE_CLS(Del, AST_cls);
    MAKE_CLS(Mult, AST_cls);
    MAKE_CLS(Div, AST_cls);
    MAKE_CLS(USub, AST_cls);
    MAKE_CLS(BitAnd, AST_cls);
    MAKE_CLS(BitOr, AST_cls);
    MAKE_CLS(BitXor, AST_cls);
    MAKE_CLS(RShift, AST_cls);
    MAKE_CLS(LShift, AST_cls);
    MAKE_CLS(Invert, AST_cls);
    MAKE_CLS(UAdd, AST_cls);
    MAKE_CLS(FloorDiv, AST_cls);
    MAKE_CLS(DictComp, AST_cls);
    MAKE_CLS(Set, AST_cls);
    MAKE_CLS(Ellipsis, AST_cls);
    MAKE_CLS(Expression, AST_cls);
    MAKE_CLS(SetComp, AST_cls);
    MAKE_CLS(Suite, AST_cls);


#undef MAKE_CLS

    // Uncommenting this makes `import ast` work, which may or may not be desired.
    // For now it seems like making the import fail is better than having the module not work properly.
    // ast_module->giveAttr("__version__", boxInt(82160));
}
}
