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

#ifndef PYSTON_RUNTIME_OBJMODEL_H
#define PYSTON_RUNTIME_OBJMODEL_H

#include <string>
#include <stdint.h>

#include "core/types.h"

namespace pyston {

class Box;
class BoxedClass;
class BoxedInt;
class BoxedList;
class BoxedString;

void raiseExc(Box*) __attribute__((__noreturn__));
void raiseExcHelper(BoxedClass*, const char* fmt, ...) __attribute__((__noreturn__));

extern "C" const std::string* getTypeName(Box* o);
extern "C" const std::string* getNameOfClass(BoxedClass* cls);

// TODO sort this
extern "C" void my_assert(bool b);
extern "C" Box* getattr(Box* obj, const char* attr);
extern "C" void setattr(Box* obj, const char* attr, Box* attr_val);
extern "C" bool nonzero(Box* obj);
extern "C" Box* runtimeCall(Box*, int64_t, Box*, Box*, Box*, Box**);
extern "C" Box* callattr(Box*, std::string*, bool, int64_t, Box*, Box*, Box*, Box**);
extern "C" BoxedString* str(Box* obj);
extern "C" Box* repr(Box* obj);
extern "C" bool isinstance(Box* obj, Box* cls, int64_t flags);
extern "C" BoxedInt* hash(Box* obj);
// extern "C" Box* abs_(Box* obj);
extern "C" Box* open1(Box* arg);
extern "C" Box* open2(Box* arg1, Box* arg2);
// extern "C" Box* chr(Box* arg);
extern "C" Box* compare(Box*, Box*, int);
extern "C" BoxedInt* len(Box* obj);
extern "C" void print(Box* obj);
extern "C" void dump(Box* obj);
// extern "C" Box* trap();
extern "C" i64 unboxedLen(Box* obj);
extern "C" Box* binop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* getGlobal(BoxedModule* m, std::string* name);
extern "C" Box* getitem(Box* value, Box* slice);
extern "C" void setitem(Box* target, Box* slice, Box* value);
extern "C" Box* getclsattr(Box* obj, const char* attr);
extern "C" Box* unaryop(Box* operand, int op_type);
extern "C" Box* import(const std::string* name);
extern "C" void checkUnpackingLength(i64 expected, i64 given);
extern "C" void assertNameDefined(bool b, const char* name);
extern "C" void assertFail(BoxedModule* inModule, Box* msg);

struct CompareRewriteArgs;
Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args);
Box* getattr_internal(Box* obj, const std::string& attr, bool check_cls, bool allow_custom,
                      GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2);

extern "C" void raiseAttributeErrorStr(const char* typeName, const char* attr) __attribute__((__noreturn__));
extern "C" void raiseAttributeError(Box* obj, const char* attr) __attribute__((__noreturn__));
extern "C" void raiseNotIterableError(const char* typeName) __attribute__((__noreturn__));

Box* typeCall(Box*, BoxedList*);
Box* typeNew(Box*, Box*);
bool isUserDefined(BoxedClass* cls);
}
#endif
