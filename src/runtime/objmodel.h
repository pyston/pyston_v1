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

#include <stdint.h>
#include <string>

#include "core/types.h"

namespace pyston {

class Box;
class BoxedClass;
class BoxedInt;
class BoxedList;
class BoxedString;

// user-level raise functions that implement python-level semantics
extern "C" void raise0() __attribute__((__noreturn__));
extern "C" void raise1(Box*) __attribute__((__noreturn__));
extern "C" void raise2(Box*, Box*) __attribute__((__noreturn__));
extern "C" void raise3(Box*, Box*, Box*) __attribute__((__noreturn__));
// helper function for raising from the runtime:
void raiseExcHelper(BoxedClass*, const char* fmt, ...) __attribute__((__noreturn__));

extern "C" const std::string* getTypeName(Box* o);
extern "C" const std::string* getNameOfClass(BoxedClass* cls);

// TODO sort this
extern "C" void my_assert(bool b);
extern "C" Box* importFrom(Box* obj, const std::string* attr);
extern "C" Box* getattr(Box* obj, const char* attr);
extern "C" void setattr(Box* obj, const char* attr, Box* attr_val);
extern "C" bool nonzero(Box* obj);
extern "C" Box* runtimeCall(Box*, ArgPassSpec, Box*, Box*, Box*, Box**, const std::vector<const std::string*>*);
extern "C" Box* callattr(Box*, std::string*, bool, ArgPassSpec, Box*, Box*, Box*, Box**,
                         const std::vector<const std::string*>*);
extern "C" BoxedString* str(Box* obj);
extern "C" Box* repr(Box* obj);
extern "C" BoxedString* reprOrNull(Box* obj); // similar to repr, but returns NULL on exception
extern "C" BoxedString* strOrNull(Box* obj);  // similar to str, but returns NULL on exception
extern "C" bool isinstance(Box* obj, Box* cls, int64_t flags);
extern "C" BoxedInt* hash(Box* obj);
// extern "C" Box* abs_(Box* obj);
Box* open(Box* arg1, Box* arg2);
// extern "C" Box* chr(Box* arg);
extern "C" Box* compare(Box*, Box*, int);
extern "C" BoxedInt* len(Box* obj);
extern "C" void print(Box* obj);
// extern "C" Box* trap();
extern "C" i64 unboxedLen(Box* obj);
extern "C" Box* binop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* getGlobal(BoxedModule* m, std::string* name);
extern "C" Box* getitem(Box* value, Box* slice);
extern "C" void setitem(Box* target, Box* slice, Box* value);
extern "C" void delitem(Box* target, Box* slice);
extern "C" Box* getclsattr(Box* obj, const char* attr);
extern "C" Box* unaryop(Box* operand, int op_type);
extern "C" Box* import(const std::string* name);
extern "C" void checkUnpackingLength(i64 expected, i64 given);
extern "C" void assertNameDefined(bool b, const char* name);
extern "C" void assertFail(BoxedModule* inModule, Box* msg);
extern "C" bool isSubclass(BoxedClass* child, BoxedClass* parent);
extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure);

class BinopRewriteArgs;
extern "C" Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs* rewrite_args);

Box* typeCallInternal(BoxedFunction* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                      Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names);

class CallRewriteArgs;
enum LookupScope {
    CLASS_ONLY = 1,
    INST_ONLY = 2,
    CLASS_OR_INST = 3,
};
extern "C" Box* callattrInternal(Box* obj, const std::string* attr, LookupScope, CallRewriteArgs* rewrite_args,
                                 ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                                 const std::vector<const std::string*>* keyword_names);

struct CompareRewriteArgs;
Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args);
Box* getattr_internal(Box* obj, const std::string& attr, bool check_cls, bool allow_custom,
                      GetattrRewriteArgs* rewrite_args, GetattrRewriteArgs2* rewrite_args2);

Box* typeLookup(BoxedClass* cls, const std::string& attr, GetattrRewriteArgs* rewrite_args,
                GetattrRewriteArgs2* rewrite_args2);

extern "C" void raiseAttributeErrorStr(const char* typeName, const char* attr) __attribute__((__noreturn__));
extern "C" void raiseAttributeError(Box* obj, const char* attr) __attribute__((__noreturn__));
extern "C" void raiseNotIterableError(const char* typeName) __attribute__((__noreturn__));

Box* typeCall(Box*, BoxedList*);
Box* typeNew(Box*, Box*);
bool isUserDefined(BoxedClass* cls);
}
#endif
