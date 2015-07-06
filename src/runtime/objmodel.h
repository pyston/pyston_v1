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
class BoxedTuple;

// user-level raise functions that implement python-level semantics
ExcInfo excInfoForRaise(Box*, Box*, Box*);
extern "C" void raise0() __attribute__((__noreturn__));
extern "C" void raise3(Box*, Box*, Box*) __attribute__((__noreturn__));
void raiseExc(Box* exc_obj) __attribute__((__noreturn__));
void _printStacktrace();

extern "C" Box* deopt(AST_expr* expr, Box* value);

// helper function for raising from the runtime:
void raiseExcHelper(BoxedClass*, const char* fmt, ...) __attribute__((__noreturn__));
void raiseExcHelper(BoxedClass*, Box* arg) __attribute__((__noreturn__));

BoxedModule* getCurrentModule();

// TODO sort this
extern "C" bool softspace(Box* b, bool newval);
extern "C" void printHelper(Box* dest, Box* var, bool nl);
extern "C" void my_assert(bool b);
extern "C" Box* getattr(Box* obj, BoxedString* attr);
extern "C" Box* getattrMaybeNonstring(Box* obj, Box* attr);
extern "C" void setattr(Box* obj, BoxedString* attr, Box* attr_val);
extern "C" void setattrMaybeNonstring(Box* obj, Box* attr, Box* attr_val);
extern "C" void delattr(Box* obj, BoxedString* attr);
extern "C" void delattrMaybeNonstring(Box* obj, Box* attr);
extern "C" void delattrGeneric(Box* obj, BoxedString* attr, DelattrRewriteArgs* rewrite_args);
extern "C" bool nonzero(Box* obj);
extern "C" Box* runtimeCall(Box*, ArgPassSpec, Box*, Box*, Box*, Box**, const std::vector<BoxedString*>*);
extern "C" Box* callattr(Box*, BoxedString*, CallattrFlags, Box*, Box*, Box*, Box**, const std::vector<BoxedString*>*);
extern "C" BoxedString* str(Box* obj);
extern "C" BoxedString* repr(Box* obj);
extern "C" BoxedString* reprOrNull(Box* obj); // similar to repr, but returns NULL on exception
extern "C" BoxedString* strOrNull(Box* obj);  // similar to str, but returns NULL on exception
extern "C" Box* strOrUnicode(Box* obj);
extern "C" bool exceptionMatches(Box* obj, Box* cls);
extern "C" BoxedInt* hash(Box* obj);
extern "C" int64_t hashUnboxed(Box* obj);
extern "C" Box* abs_(Box* obj);
// extern "C" Box* chr(Box* arg);
extern "C" Box* compare(Box*, Box*, int);
extern "C" BoxedInt* len(Box* obj);
// extern "C" Box* trap();
extern "C" i64 unboxedLen(Box* obj);
extern "C" Box* binop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type);
extern "C" Box* getitem(Box* value, Box* slice);
extern "C" void setitem(Box* target, Box* slice, Box* value);
extern "C" void delitem(Box* target, Box* slice);
extern "C" Box* getclsattr(Box* obj, BoxedString* attr);
extern "C" Box* getclsattrMaybeNonstring(Box* obj, Box* attr);
extern "C" Box* unaryop(Box* operand, int op_type);
extern "C" Box* importFrom(Box* obj, BoxedString* attr);
extern "C" Box* importStar(Box* from_module, Box* to_globals);
extern "C" Box** unpackIntoArray(Box* obj, int64_t expected_size);
extern "C" void assertNameDefined(bool b, const char* name, BoxedClass* exc_cls, bool local_var_msg);
extern "C" void assertFailDerefNameDefined(const char* name);
extern "C" void assertFail(Box* assertion_type, Box* msg);
extern "C" bool isSubclass(BoxedClass* child, BoxedClass* parent);
extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure, size_t size);

Box* getiter(Box* o);
extern "C" Box* getPystonIter(Box* o);
extern "C" Box* getiterHelper(Box* o);
extern "C" Box* createBoxedIterWrapperIfNeeded(Box* o);
extern "C" bool hasnext(Box* o);

extern "C" void dump(void* p);
extern "C" void dumpEx(void* p, int levels = 0);

struct SetattrRewriteArgs;
void setattrGeneric(Box* obj, BoxedString* attr, Box* val, SetattrRewriteArgs* rewrite_args);

struct BinopRewriteArgs;
extern "C" Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs* rewrite_args);

struct CallRewriteArgs;
Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, const std::vector<BoxedString*>* keyword_names);

Box* lenCallInternal(BoxedFunctionBase* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                     Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names);

Box* callFunc(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
              Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names);

enum LookupScope {
    CLASS_ONLY = 1,
    INST_ONLY = 2,
    CLASS_OR_INST = 3,
};
extern "C" Box* callattrInternal(Box* obj, BoxedString* attr, LookupScope, CallRewriteArgs* rewrite_args,
                                 ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                                 const std::vector<BoxedString*>* keyword_names);
extern "C" void delattr_internal(Box* obj, llvm::StringRef attr, bool allow_custom, DelattrRewriteArgs* rewrite_args);
struct CompareRewriteArgs;
Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args);

// This is the equivalent of PyObject_GetAttr. Unlike getattrInternalGeneric, it checks for custom __getattr__ or
// __getattribute__ methods.
Box* getattrInternal(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args);

// This is the equivalent of PyObject_GenericGetAttr, which performs the default lookup rules for getattr() (check for
// data descriptor, check for instance attribute, check for non-data descriptor). It does not check for __getattr__ or
// __getattribute__.
Box* getattrInternalGeneric(Box* obj, llvm::StringRef attr, GetattrRewriteArgs* rewrite_args, bool cls_only,
                            bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out);

// This is the equivalent of _PyType_Lookup(), which calls Box::getattr() on each item in the object's MRO in the
// appropriate order. It does not do any descriptor logic.
Box* typeLookup(BoxedClass* cls, llvm::StringRef attr, GetattrRewriteArgs* rewrite_args);

extern "C" void raiseAttributeErrorStr(const char* typeName, llvm::StringRef attr) __attribute__((__noreturn__));
extern "C" void raiseAttributeError(Box* obj, llvm::StringRef attr) __attribute__((__noreturn__));
extern "C" void raiseNotIterableError(const char* typeName) __attribute__((__noreturn__));
extern "C" void raiseIndexErrorStr(const char* typeName) __attribute__((__noreturn__));

Box* typeCall(Box*, BoxedTuple*, BoxedDict*);
Box* typeNew(Box* cls, Box* arg1, Box* arg2, Box** _args);
bool isUserDefined(BoxedClass* cls);

// These process a potential descriptor, differing in their behavior if the object was not a descriptor.
// the OrNull variant returns NULL to signify it wasn't a descriptor, and the processDescriptor version
// returns obj.
Box* processDescriptor(Box* obj, Box* inst, Box* owner);
Box* processDescriptorOrNull(Box* obj, Box* inst, Box* owner);

Box* callCLFunc(CLFunction* f, CallRewriteArgs* rewrite_args, int num_output_args, BoxedClosure* closure,
                BoxedGenerator* generator, Box* globals, Box* oarg1, Box* oarg2, Box* oarg3, Box** oargs);

static const char* objectNewParameterTypeErrorMsg() {
    if (PYTHON_VERSION_HEX >= version_hex(2, 7, 4)) {
        return "object() takes no parameters";
    } else {
        return "object.__new__() takes no parameters";
    }
}

// This function will ascii-encode any unicode objects it gets passed, or return the argument
// unmodified if it wasn't a unicode object.
// This is intended for functions that deal with attribute or variable names, which we internally
// assume will always be strings, but CPython lets be unicode.
// If we used an encoding like utf8 instead of ascii, we would allow collisions between unicode
// strings and a string that happens to be its encoding.  It seems safer to just encode as ascii,
// which will throw an exception if you try to pass something that might run into this risk.
// (We wrap the unicode error and throw a TypeError)
Box* coerceUnicodeToStr(Box* unicode);

inline std::tuple<Box*, Box*, Box*, Box**> getTupleFromArgsArray(Box** args, int num_args) {
    Box* arg1 = num_args >= 1 ? args[0] : nullptr;
    Box* arg2 = num_args >= 2 ? args[1] : nullptr;
    Box* arg3 = num_args >= 3 ? args[2] : nullptr;
    Box** argtuple = num_args >= 4 ? &args[3] : nullptr;
    return std::make_tuple(arg1, arg2, arg3, argtuple);
}

// The `globals` argument can be either a BoxedModule or a BoxedDict

// Corresponds to a name lookup with GLOBAL scope.  Checks the passed globals object, then the builtins,
// and if not found raises an exception.
extern "C" Box* getGlobal(Box* globals, BoxedString* name);
// Checks for the name just in the passed globals object, and returns NULL if it is not found.
// This includes if the globals object defined a custom __getattr__ method that threw an AttributeError.
Box* getFromGlobals(Box* globals, BoxedString* name);
void setGlobal(Box* globals, BoxedString* name, Box* value);
extern "C" void delGlobal(Box* globals, BoxedString* name);

extern "C" void boxedLocalsSet(Box* boxedLocals, BoxedString* attr, Box* val);
extern "C" Box* boxedLocalsGet(Box* boxedLocals, BoxedString* attr, Box* globals);
extern "C" void boxedLocalsDel(Box* boxedLocals, BoxedString* attr);
}
#endif
