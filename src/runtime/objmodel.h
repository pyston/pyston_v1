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

#include "core/options.h"
#include "core/types.h"

namespace pyston {

class Box;
class BoxedClass;
class BoxedInt;
class BoxedList;
class BoxedString;
class BoxedGenerator;

// user-level raise functions that implement python-level semantics
extern "C" void raise0() __attribute__((__noreturn__));
extern "C" void raise3(Box*, Box*, Box*) __attribute__((__noreturn__));
void raiseExc(Box* exc_obj) __attribute__((__noreturn__));

// helper function for raising from the runtime:
void raiseExcHelper(BoxedClass*, const char* fmt, ...) __attribute__((__noreturn__));

BoxedModule* getCurrentModule();

// TODO sort this
extern "C" bool softspace(Box* b, bool newval);
extern "C" void my_assert(bool b);
extern "C" Box* getattr(Box* obj, const char* attr);
extern "C" void setattr(Box* obj, const char* attr, Box* attr_val);
extern "C" void delattr(Box* obj, const char* attr);
extern "C" bool nonzero(Box* obj);
extern "C" Box* runtimeCall(Box*, ArgPassSpec, Box*, Box*, Box*, Box**, const std::vector<const std::string*>*);
extern "C" Box* callattr(Box*, const std::string*, bool, ArgPassSpec, Box*, Box*, Box*, Box**,
                         const std::vector<const std::string*>*);
extern "C" BoxedString* str(Box* obj);
extern "C" BoxedString* repr(Box* obj);
extern "C" BoxedString* reprOrNull(Box* obj); // similar to repr, but returns NULL on exception
extern "C" BoxedString* strOrNull(Box* obj);  // similar to str, but returns NULL on exception
extern "C" bool isinstance(Box* obj, Box* cls, int64_t flags);
extern "C" BoxedInt* hash(Box* obj);
extern "C" Box* abs_(Box* obj);
Box* open(Box* arg1, Box* arg2);
// extern "C" Box* chr(Box* arg);
extern "C" Box* compare(Box*, Box*, int);
extern "C" BoxedInt* len(Box* obj);
// extern "C" Box* trap();
extern "C" i64 unboxedLen(Box* obj);
extern "C" Box* binop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* getGlobal(BoxedModule* m, std::string* name);
extern "C" void delGlobal(BoxedModule* m, std::string* name);
extern "C" Box* getitem(Box* value, Box* slice);
extern "C" void setitem(Box* target, Box* slice, Box* value);
extern "C" void delitem(Box* target, Box* slice);
extern "C" Box* getclsattr(Box* obj, const char* attr);
extern "C" Box* unaryop(Box* operand, int op_type);
extern "C" Box* importFrom(Box* obj, const std::string* attr);
extern "C" Box* importStar(Box* from_module, BoxedModule* to_module);
extern "C" Box** unpackIntoArray(Box* obj, int64_t expected_size);
extern "C" void assertNameDefined(bool b, const char* name, BoxedClass* exc_cls, bool local_var_msg);
extern "C" void assertFail(BoxedModule* inModule, Box* msg);
extern "C" bool isSubclass(BoxedClass* child, BoxedClass* parent);
extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure);
extern "C" Box* getiter(Box* o);

class SetattrRewriteArgs;
void setattrInternal(Box* obj, const std::string& attr, Box* val, SetattrRewriteArgs* rewrite_args);

class BinopRewriteArgs;
extern "C" Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs* rewrite_args);

class CallRewriteArgs;
Box* typeCallInternal(BoxedFunction* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                      Box* arg3, Box** args, const std::vector<const std::string*>* keyword_names);

enum LookupScope {
    CLASS_ONLY = 1,
    INST_ONLY = 2,
    CLASS_OR_INST = 3,
};
extern "C" Box* callattrInternal(Box* obj, const std::string* attr, LookupScope, CallRewriteArgs* rewrite_args,
                                 ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                                 const std::vector<const std::string*>* keyword_names);
extern "C" void delattr_internal(Box* obj, const std::string& attr, bool allow_custom,
                                 DelattrRewriteArgs* rewrite_args);
struct CompareRewriteArgs;
Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args);
Box* getattrInternal(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args);
Box* getattrInternalGeneral(Box* obj, const std::string& attr, GetattrRewriteArgs* rewrite_args, bool cls_only,
                            bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out);

Box* typeLookup(BoxedClass* cls, const std::string& attr, GetattrRewriteArgs* rewrite_args);

extern "C" void raiseAttributeErrorStr(const char* typeName, const char* attr) __attribute__((__noreturn__));
extern "C" void raiseAttributeError(Box* obj, const char* attr) __attribute__((__noreturn__));
extern "C" void raiseNotIterableError(const char* typeName) __attribute__((__noreturn__));

Box* typeCall(Box*, BoxedList*);
Box* typeNew(Box* cls, Box* arg1, Box* arg2, Box** _args);
bool isUserDefined(BoxedClass* cls);

Box* processDescriptor(Box* obj, Box* inst, Box* owner);

Box* callCLFunc(CLFunction* f, CallRewriteArgs* rewrite_args, int num_output_args, BoxedClosure* closure,
                BoxedGenerator* generator, Box* oarg1, Box* oarg2, Box* oarg3, Box** oargs);

static const char* objectNewParameterTypeErrorMsg() {
    if (PYTHON_VERSION_HEX >= version_hex(2, 7, 4)) {
        return "object() takes no parameters";
    } else {
        return "object.__new__() takes no parameters";
    }
}
}
#endif
