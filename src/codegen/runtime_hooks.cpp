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

#include "codegen/runtime_hooks.h"

#include <cstdio>
#include <unordered_map>

#include "llvm/Analysis/Passes.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/IR/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"

#include "codegen/codegen.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/util.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/gc_runtime.h"
#include "runtime/inline/boxing.h"
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

extern "C" void* __cxa_begin_catch(void*);
extern "C" void __cxa_end_catch();

namespace pyston {

static llvm::Function* lookupFunction(const std::string& name) {
    llvm::Function* r = g.stdlib_module->getFunction(name);
    ASSERT(r, "Couldn't find '%s'", name.c_str());
    return r;
}

static llvm::Value* getFunc(void* func, const char* name) {
    llvm::Function* f = lookupFunction(name);
    ASSERT(f, "%s", name);
    g.func_addr_registry.registerFunction(name, func, 0, f);
    return embedConstantPtr(func, f->getType());
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::ArrayRef<llvm::Type*> arg_types,
                            bool varargs = false) {
    llvm::FunctionType* ft = llvm::FunctionType::get(rtn_type, arg_types, varargs);
    return embedConstantPtr(func, ft->getPointerTo());
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, bool varargs = false) {
    return addFunc(func, rtn_type, llvm::ArrayRef<llvm::Type*>(), varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, bool varargs = false) {
    llvm::Type* array[] = { arg1 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2,
                            bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                            bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                            llvm::Type* arg4, bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                            llvm::Type* arg4, llvm::Type* arg5, bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4, arg5 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                            llvm::Type* arg4, llvm::Type* arg5, llvm::Type* arg6, bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4, arg5, arg6 };
    return addFunc(func, rtn_type, array, varargs);
}

static llvm::Value* addFunc(void* func, llvm::Type* rtn_type, llvm::Type* arg1, llvm::Type* arg2, llvm::Type* arg3,
                            llvm::Type* arg4, llvm::Type* arg5, llvm::Type* arg6, llvm::Type* arg7,
                            bool varargs = false) {
    llvm::Type* array[] = { arg1, arg2, arg3, arg4, arg5, arg6, arg7 };
    return addFunc(func, rtn_type, array, varargs);
}

void initGlobalFuncs(GlobalState& g) {
    g.llvm_opaque_type = llvm::StructType::create(g.context, "opaque");

    g.llvm_clfunction_type_ptr = lookupFunction("boxCLFunction")->arg_begin()->getType();
    g.llvm_module_type_ptr = g.stdlib_module->getTypeByName("class.pyston::BoxedModule")->getPointerTo();
    assert(g.llvm_module_type_ptr);
    g.llvm_bool_type_ptr = lookupFunction("boxBool")->getReturnType();

    g.llvm_value_type_ptr = lookupFunction("getattr")->getReturnType();
    g.llvm_value_type = g.llvm_value_type_ptr->getSequentialElementType();
    g.llvm_value_type_ptr_ptr = g.llvm_value_type_ptr->getPointerTo();
    // g.llvm_class_type_ptr = llvm::cast<llvm::StructType>(g.llvm_value_type)->getElementType(0);
    // g.llvm_class_type = g.llvm_class_type_ptr->getSequentialElementType();
    g.llvm_class_type = g.stdlib_module->getTypeByName("class.pyston::BoxedClass");
    assert(g.llvm_class_type);
    g.llvm_class_type_ptr = g.llvm_class_type->getPointerTo();

    g.llvm_flavor_type = g.stdlib_module->getTypeByName("class.pyston::ObjectFlavor");
    assert(g.llvm_flavor_type);
    g.llvm_flavor_type_ptr = g.llvm_flavor_type->getPointerTo();

    g.llvm_str_type_ptr = lookupFunction("boxStringPtr")->arg_begin()->getType();

    // The LLVM vector type for the arguments that we pass to runtimeCall and related functions.
    // It will be a pointer to a type named something like class.std::vector or
    // class.std::vector.##. We can figure out exactly what it is by looking at the last
    // argument of runtimeCall.
    g.vector_ptr = (--lookupFunction("runtimeCall")->getArgumentList().end())->getType();

    g.llvm_closure_type_ptr = g.stdlib_module->getTypeByName("class.pyston::BoxedClosure")->getPointerTo();
    assert(g.llvm_closure_type_ptr);

#define GET(N) g.funcs.N = getFunc((void*)N, STRINGIFY(N))

    g.funcs.printf = addFunc((void*)printf, g.i8_ptr, true);
    g.funcs.my_assert = getFunc((void*)my_assert, "my_assert");
    g.funcs.malloc = addFunc((void*)malloc, g.i8_ptr, g.i64);
    g.funcs.free = addFunc((void*)free, g.void_, g.i8_ptr);

    g.funcs.allowGLReadPreemption = addFunc((void*)threading::allowGLReadPreemption, g.void_);

    GET(boxCLFunction);
    GET(unboxCLFunction);
    GET(createUserClass);
    GET(boxInt);
    GET(unboxInt);
    GET(boxFloat);
    GET(unboxFloat);
    GET(boxStringPtr);
    GET(boxInstanceMethod);
    GET(boxBool);
    GET(unboxBool);
    GET(createTuple);
    GET(createList);
    GET(createDict);
    GET(createSlice);
    GET(createClosure);

    GET(getattr);
    GET(setattr);
    GET(getitem);
    GET(setitem);
    GET(delitem);
    GET(getGlobal);
    GET(binop);
    GET(compare);
    GET(augbinop);
    GET(nonzero);
    GET(print);
    GET(unboxedLen);
    GET(getclsattr);
    GET(unaryop);
    GET(import);
    GET(importFrom);
    GET(repr);
    GET(isinstance);

    GET(checkUnpackingLength);
    GET(raiseAttributeError);
    GET(raiseAttributeErrorStr);
    GET(raiseNotIterableError);
    GET(assertNameDefined);
    GET(assertFail);

    GET(printFloat);
    GET(listAppendInternal);

    g.funcs.runtimeCall = getFunc((void*)runtimeCall, "runtimeCall");
    g.funcs.runtimeCall0 = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32);
    g.funcs.runtimeCall1
        = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32, g.llvm_value_type_ptr);
    g.funcs.runtimeCall2 = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32,
                                   g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.runtimeCall3 = addFunc((void*)runtimeCall, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.i32,
                                   g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr);

    g.funcs.callattr = getFunc((void*)callattr, "callattr");
    g.funcs.callattr0
        = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_str_type_ptr, g.i1, g.i32);
    g.funcs.callattr1 = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_str_type_ptr,
                                g.i1, g.i32, g.llvm_value_type_ptr);
    g.funcs.callattr2 = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_str_type_ptr,
                                g.i1, g.i32, g.llvm_value_type_ptr, g.llvm_value_type_ptr);
    g.funcs.callattr3 = addFunc((void*)callattr, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_str_type_ptr,
                                g.i1, g.i32, g.llvm_value_type_ptr, g.llvm_value_type_ptr, g.llvm_value_type_ptr);

    g.funcs.reoptCompiledFunc = addFunc((void*)reoptCompiledFunc, g.i8_ptr, g.i8_ptr);
    g.funcs.compilePartialFunc = addFunc((void*)compilePartialFunc, g.i8_ptr, g.i8_ptr);

    GET(__cxa_begin_catch);
    g.funcs.__cxa_end_catch = addFunc((void*)__cxa_end_catch, g.void_);
    GET(raise0);
    GET(raise1);

    g.funcs.div_i64_i64 = getFunc((void*)div_i64_i64, "div_i64_i64");
    g.funcs.mod_i64_i64 = getFunc((void*)mod_i64_i64, "mod_i64_i64");
    g.funcs.pow_i64_i64 = getFunc((void*)pow_i64_i64, "pow_i64_i64");

    GET(div_float_float);
    GET(mod_float_float);
    GET(pow_float_float);
}
}
