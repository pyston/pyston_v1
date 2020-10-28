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
extern "C" void raise0(ExcInfo* frame_exc_info) __attribute__((__noreturn__));
extern "C" void raise0_capi(ExcInfo* frame_exc_info) noexcept;
extern "C" void raise3(Box*, Box*, Box*) __attribute__((__noreturn__));
extern "C" void raise3_capi(Box*, Box*, Box*) noexcept;
extern "C" void rawReraise(Box*, Box*, Box*) __attribute__((__noreturn__));
void raiseExc(STOLEN(Box*) exc_obj) __attribute__((__noreturn__));
void _printStacktrace();

// Note -- most of these functions are marked 'noinline' because they inspect the return-address
// to see if they are getting called from jitted code.  If we inline them into a function that
// got called from jitted code, they might incorrectly think that they are a rewritable entrypoint.

extern "C" Box* deopt(Box* value) __attribute__((noinline));

// helper function for raising from the runtime:
void raiseExcHelper(BoxedClass*, const char* fmt, ...) __attribute__((__noreturn__))
__attribute__((format(printf, 2, 3)));
void raiseExcHelper(BoxedClass*, Box* arg) __attribute__((__noreturn__));

// TODO sort this
extern "C" void printHelper(Box* dest, Box* var, bool nl);
extern "C" void my_assert(bool b);
extern "C" Box* getattr(Box* obj, BoxedString* attr) __attribute__((noinline));
extern "C" Box* getattr_capi(Box* obj, BoxedString* attr) noexcept __attribute__((noinline));
extern "C" Box* getattrMaybeNonstring(Box* obj, Box* attr);
extern "C" void setattr(Box* obj, BoxedString* attr, STOLEN(Box*) attr_val) __attribute__((noinline));
extern "C" void delattr(Box* obj, BoxedString* attr) __attribute__((noinline));
extern "C" void delattrGeneric(Box* obj, BoxedString* attr, DelattrRewriteArgs* rewrite_args);
extern "C" bool nonzero(Box* obj) __attribute__((noinline));
extern "C" Box* runtimeCall(Box*, ArgPassSpec, Box*, Box*, Box*, Box**, BoxedTuple*) __attribute__((noinline));
extern "C" Box* runtimeCallCapi(Box*, ArgPassSpec, Box*, Box*, Box*, Box**, BoxedTuple*) noexcept
    __attribute__((noinline));
extern "C" Box* callattr(Box*, BoxedString*, CallattrFlags, Box*, Box*, Box*, Box**, BoxedTuple*)
    __attribute__((noinline));
extern "C" Box* callattrCapi(Box*, BoxedString*, CallattrFlags, Box*, Box*, Box*, Box**, BoxedTuple*) noexcept
    __attribute__((noinline));
extern "C" BoxedString* str(Box* obj) __attribute__((noinline));
extern "C" BoxedString* repr(Box* obj) __attribute__((noinline));
extern "C" bool exceptionMatches(Box* obj, Box* cls) __attribute__((noinline));
extern "C" BoxedInt* hash(Box* obj) __attribute__((noinline));
extern "C" int64_t hashUnboxed(Box* obj) __attribute__((noinline));
extern "C" Box* abs_(Box* obj);
// extern "C" Box* chr(Box* arg);
extern "C" Box* compare(Box*, Box*, int) __attribute__((noinline));
extern "C" BoxedInt* len(Box* obj) __attribute__((noinline));
// extern "C" Box* trap();
extern "C" i64 unboxedLen(Box* obj) __attribute__((noinline));
extern "C" Box* binop(Box* lhs, Box* rhs, int op_type) __attribute__((noinline));
extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type) __attribute__((noinline));
extern "C" Box* getitem(Box* value, Box* slice) __attribute__((noinline));
extern "C" Box* getitem_capi(Box* value, Box* slice) noexcept __attribute__((noinline));
extern "C" void setitem(Box* target, Box* slice, Box* value) __attribute__((noinline));
extern "C" void delitem(Box* target, Box* slice) __attribute__((noinline));
extern "C" PyObject* apply_slice(PyObject* u, PyObject* v, PyObject* w) noexcept;
extern "C" PyObject* applySlice(PyObject* u, PyObject* v, PyObject* w);
extern "C" int assign_slice(PyObject* u, PyObject* v, PyObject* w, PyObject* x) noexcept;
extern "C" void assignSlice(PyObject* u, PyObject* v, PyObject* w, PyObject* x);
extern "C" Box* getclsattr(Box* obj, BoxedString* attr) __attribute__((noinline));
extern "C" Box* getclsattrMaybeNonstring(Box* obj, Box* attr) __attribute__((noinline));
extern "C" Box* unaryop(Box* operand, int op_type) __attribute__((noinline));
extern "C" Box* importFrom(Box* obj, BoxedString* attr) __attribute__((noinline));
extern "C" Box* importStar(Box* from_module, Box* to_globals) __attribute__((noinline));
extern "C" Box** unpackIntoArray(Box* obj, int64_t expected_size, Box** out_keep_alive);
extern "C" void assertNameDefined(bool b, const char* name, BoxedClass* exc_cls, bool local_var_msg);
extern "C" void assertFailDerefNameDefined(const char* name);
extern "C" void assertFail(Box* assertion_type, Box* msg);
extern "C" void dictSetInternal(Box* d, STOLEN(Box*) k, STOLEN(Box* v));

inline bool isSubclass(BoxedClass* child, BoxedClass* parent) {
    return child == parent || PyType_IsSubtype(child, parent);
}

extern "C" BoxedClosure* createClosure(BoxedClosure* parent_closure, size_t size);

Box* getiter(Box* o);
extern "C" Box* getPystonIter(Box* o);
extern "C" Box* getiterHelper(Box* o) __attribute__((noinline));
extern "C" Box* createBoxedIterWrapperIfNeeded(Box* o) __attribute__((noinline));

struct SetattrRewriteArgs;
template <Rewritable rewritable>
void setattrGeneric(Box* obj, BoxedString* attr, STOLEN(Box*) val, SetattrRewriteArgs* rewrite_args);

struct BinopRewriteArgs;
template <Rewritable rewritable, bool inplace>
Box* binopInternal(Box* lhs, Box* rhs, int op_type, BinopRewriteArgs* rewrite_args);

struct CallRewriteArgs;
template <ExceptionStyle S, Rewritable rewritable>
Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, BoxedTuple* keyword_names) noexcept(S == CAPI);

struct GetitemRewriteArgs;
template <ExceptionStyle S, Rewritable rewritable = REWRITABLE>
Box* getitemInternal(Box* target, Box* slice, GetitemRewriteArgs* rewrite_args) noexcept(S == CAPI);
template <ExceptionStyle S> inline Box* getitemInternal(Box* target, Box* slice) noexcept(S == CAPI) {
    return getitemInternal<S, NOT_REWRITABLE>(target, slice, NULL);
}

template <ExceptionStyle S, Rewritable rewritable>
BoxedInt* lenInternal(Box* obj, UnaryopRewriteArgs* rewrite_args) noexcept(S == CAPI);
Box* lenCallInternal(BoxedFunctionBase* f, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                     Box* arg3, Box** args, BoxedTuple* keyword_names);

template <ExceptionStyle S, Rewritable rewritable = REWRITABLE>
Box* callFunc(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
              Box* arg3, Box** args, BoxedTuple* keyword_names) noexcept(S == CAPI);

enum LookupScope {
    CLASS_ONLY = 1,
    INST_ONLY = 2,
    CLASS_OR_INST = 3,
};
struct CallattrRewriteArgs;
template <ExceptionStyle S, Rewritable rewritable>
Box* callattrInternal(Box* obj, BoxedString* attr, LookupScope, CallattrRewriteArgs* rewrite_args, ArgPassSpec argspec,
                      Box* arg1, Box* arg2, Box* arg3, Box** args, BoxedTuple* keyword_names) noexcept(S == CAPI);
extern "C" void delattr_internal(Box* obj, BoxedString* attr, bool allow_custom, DelattrRewriteArgs* rewrite_args);
struct CompareRewriteArgs;
template <Rewritable rewritable>
Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args);

// This is the equivalent of PyObject_GetAttr. Unlike getattrInternalGeneric, it checks for custom __getattr__ or
// __getattribute__ methods.
template <ExceptionStyle S, Rewritable rewritable = REWRITABLE>
Box* getattrInternal(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args) noexcept(S == CAPI);
template <ExceptionStyle S> inline Box* getattrInternal(Box* obj, BoxedString* attr) noexcept(S == CAPI) {
    return getattrInternal<S, NOT_REWRITABLE>(obj, attr, NULL);
}

// This is the equivalent of PyObject_GenericGetAttr, which performs the default lookup rules for getattr() (check for
// data descriptor, check for instance attribute, check for non-data descriptor). It does not check for __getattr__ or
// __getattribute__.
template <bool IsType, Rewritable rewritable>
Box* getattrInternalGeneric(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args, bool cls_only, bool for_call,
                            BORROWED(Box**) bind_obj_out, RewriterVar** r_bind_obj_out);

extern "C" PyObject* type_getattro(PyObject* o, PyObject* name) noexcept;

// This is the equivalent of _PyType_Lookup(), which calls Box::getattr() on each item in the object's MRO in the
// appropriate order. It does not do any descriptor logic.
template <Rewritable rewritable = REWRITABLE>
BORROWED(Box*) typeLookup(BoxedClass* cls, BoxedString* attr, GetattrRewriteArgs* rewrite_args);
inline BORROWED(Box*) typeLookup(BoxedClass* cls, BoxedString* attr) {
    return typeLookup<NOT_REWRITABLE>(cls, attr, NULL);
}

extern "C" void raiseAttributeErrorStr(const char* typeName, llvm::StringRef attr) __attribute__((__noreturn__));
extern "C" void raiseAttributeError(Box* obj, llvm::StringRef attr) __attribute__((__noreturn__));
extern "C" void raiseAttributeErrorStrCapi(const char* typeName, llvm::StringRef attr) noexcept;
extern "C" void raiseAttributeErrorCapi(Box* obj, llvm::StringRef attr) noexcept;
extern "C" void raiseNotIterableError(const char* typeName) __attribute__((__noreturn__));
extern "C" void raiseIndexErrorStr(const char* typeName) __attribute__((__noreturn__));
extern "C" void raiseIndexErrorStrCapi(const char* typeName) noexcept;

Box* typeCall(Box*, BoxedTuple*, BoxedDict*);
Box* type_new(BoxedClass* metatype, Box* args, Box* kwds) noexcept;
Box* typeNewGeneric(Box* cls, Box* arg1, Box* arg2, Box** _args);

// These process a potential descriptor, differing in their behavior if the object was not a descriptor.
// the OrNull variant returns NULL to signify it wasn't a descriptor, and the processDescriptor version
// returns obj.
Box* processDescriptor(Box* obj, Box* inst, Box* owner);
Box* processDescriptorOrNull(Box* obj, Box* inst, Box* owner);

template <ExceptionStyle S, Rewritable rewritable>
Box* callCLFunc(BoxedCode* code, CallRewriteArgs* rewrite_args, int num_output_args, BoxedClosure* closure,
                BoxedGenerator* generator, Box* globals, Box* oarg1, Box* oarg2, Box* oarg3,
                Box** oargs) noexcept(S == CAPI);

static const char* objectNewParameterTypeErrorMsg() {
    if (PY_VERSION_HEX >= version_hex(2, 7, 4)) {
        return "object() takes no parameters";
    } else {
        return "object.__new__() takes no parameters";
    }
}

// use this like:
// if (!PyBool_Check(v))
//     return setDescrTypeError<S>(v, "bool", "__repr__");
// ...
template <ExceptionStyle S>
Box* __attribute__((warn_unused_result))
setDescrTypeError(Box* got, const char* expected_cls_name, const char* descr_name) {
    if (S == CXX)
        raiseExcHelper((PyTypeObject*)PyExc_TypeError, "descriptor '%s' requires a '%s' object but received a '%s'",
                       descr_name, expected_cls_name, getTypeName(got));
    PyErr_Format(PyExc_TypeError, "descriptor '%s' requires a '%s' object but received a '%s'", descr_name,
                 expected_cls_name, getTypeName(got));
    return NULL;
}

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
extern "C" Box* getGlobal(Box* globals, BoxedString* name) __attribute__((noinline));
extern "C" void setGlobal(Box* globals, BoxedString* name, STOLEN(Box*) value) __attribute__((noinline));
extern "C" void delGlobal(Box* globals, BoxedString* name) __attribute__((noinline));

extern "C" void boxedLocalsSet(Box* boxedLocals, BoxedString* attr, Box* val);
extern "C" Box* boxedLocalsGet(Box* boxedLocals, BoxedString* attr, Box* globals);
extern "C" void boxedLocalsDel(Box* boxedLocals, BoxedString* attr);

extern "C" void checkRefs(Box* b);   // asserts that b has >= 0 refs
extern "C" Box* assertAlive(Box* b); // asserts that b has > 0 refs, and returns b
extern "C" void xdecrefAndRethrow(void* cxa_ptr, int num_decref, ...);
}
#endif
