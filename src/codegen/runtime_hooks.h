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

#ifndef PYSTON_CODEGEN_RUNTIMEHOOKS_H
#define PYSTON_CODEGEN_RUNTIMEHOOKS_H

namespace pyston {

struct GlobalFuncs {
    llvm::Value* printf, *my_assert, *malloc, *free;

    llvm::Value* boxInt, *unboxInt, *boxFloat, *unboxFloat, *boxStringPtr, *boxCLFunction, *unboxCLFunction,
        *boxInstanceMethod, *boxBool, *unboxBool, *createTuple, *createDict, *createList, *createSlice,
        *createUserClass;
    llvm::Value* getattr, *setattr, *print, *nonzero, *binop, *compare, *augbinop, *unboxedLen, *getitem, *getclsattr,
        *getGlobal, *setitem, *unaryop, *import, *repr, *isinstance;
    llvm::Value* checkUnpackingLength, *raiseAttributeError, *raiseAttributeErrorStr, *raiseNotIterableError,
        *assertNameDefined, *assertFail;
    llvm::Value* printFloat, *listAppendInternal;
    llvm::Value* dump;
    llvm::Value* runtimeCall0, *runtimeCall1, *runtimeCall2, *runtimeCall3, *runtimeCall;
    llvm::Value* callattr0, *callattr1, *callattr2, *callattr3, *callattr;
    llvm::Value* reoptCompiledFunc, *compilePartialFunc;

    llvm::Value* __cxa_begin_catch, *__cxa_end_catch, *__cxa_allocate_exception, *__cxa_throw;

    llvm::Value* div_i64_i64, *mod_i64_i64, *pow_i64_i64;
    llvm::Value* div_float_float, *mod_float_float, *pow_float_float;
};
}

#endif
