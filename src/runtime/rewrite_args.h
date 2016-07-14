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

#ifndef PYSTON_RUNTIME_REWRITEARGS_H
#define PYSTON_RUNTIME_REWRITEARGS_H

#include "asm_writing/rewriter.h"
#include "codegen/unwinding.h"

namespace pyston {

// We have have a couple different conventions for returning values from getattr-like functions.
// For normal code we have just two conventions:
// - the "normal" convention is that signalling the lack of an attribute is handled by throwing
//   an exception (via either CAPI or C++ means).  This is the only convention that CPython has.
// - our fast "no exception" convention which will return NULL and not throw an exception, not
//   even a CAPI exception.
//
// Each function has a specific convention (most are "normal" and a couple of the inner-most ones
// are "no-exception"), and the callers and callees can both know+adhere adhere to it.
//
// For the rewriter, there are a couple more cases, and we won't know which one we will have to
// use until we get to that particular rewrite.  So the function will use 'out_return_convention' to
// signal some information about the possible values out_rtn might represent. A future C++ exception can
// usually still happen if any of these are signalled.
// - There is always a valid attribute (HAS_RETURN).  out_rtn will be set and point to a non-null object.
// - There is never an attribute (NO_RETURN).  out_rtn is null.
// - There is a valid capi return (CAPI_RETURN).  out_rtn is set, and either points to a valid object,
//   or will be a null value and a C exception is set.
// - NOEXC_POSSIBLE.   out_rtn is set, and may point to a null value with no exception set.
//
// UNSPECIFIED is used as an invalid-default to make sure that we don't implicitly assume one
// of the cases when the callee didn't explicitly signal one.
//
enum class ReturnConvention {
    UNSPECIFIED,
    HAS_RETURN,
    NO_RETURN,
    CAPI_RETURN,
    NOEXC_POSSIBLE,
    MAYBE_EXC,
};

class _ReturnConventionBase {
private:
    bool out_success;
    RewriterVar* out_rtn;
    ReturnConvention out_return_convention;
#ifndef NDEBUG
    bool return_convention_checked;
#endif

public:
    _ReturnConventionBase()
        : out_success(false),
          out_rtn(NULL),
          out_return_convention(ReturnConvention::UNSPECIFIED)
#ifndef NDEBUG
          ,
          return_convention_checked(false)
#endif
    {
    }

#ifndef NDEBUG
    ~_ReturnConventionBase() {
        if (out_success && !isUnwinding())
            assert(return_convention_checked && "Didn't check the return convention of this rewrite...");
    }
#endif

    void setReturn(RewriterVar* out_rtn, ReturnConvention out_return_convention) {
        assert(!out_success);
        assert(out_return_convention != ReturnConvention::UNSPECIFIED);
        assert((out_rtn == NULL) == (out_return_convention == ReturnConvention::NO_RETURN));
        assert(out_return_convention != ReturnConvention::UNSPECIFIED);
        assert(!return_convention_checked);
        this->out_success = true;
        this->out_rtn = out_rtn;
        this->out_return_convention = out_return_convention;

// I'm mixed on how useful this is; I like the extra checking, but changing the generated
// assembly is risky:
#ifndef NDEBUG
        struct Checker {
            static void call(Box* b, ReturnConvention r) {
                if (r == ReturnConvention::HAS_RETURN) {
                    assert(b);
                    assert(!PyErr_Occurred());
                } else if (r == ReturnConvention::CAPI_RETURN) {
                    assert((bool)b ^ (bool)PyErr_Occurred());
                } else if (r == ReturnConvention::MAYBE_EXC) {
                    assert(b);
                } else {
                    assert(r == ReturnConvention::NOEXC_POSSIBLE);
                }
            }
        };
        if (out_rtn) {
            auto rewriter = out_rtn->getRewriter();
            rewriter->call(false, (void*)Checker::call, out_rtn, rewriter->loadConst((int)out_return_convention));
        }
#endif
    }

    // For convenience for use as rewrite_args->setReturn(other_rewrite_args->getReturn())
    void setReturn(std::pair<RewriterVar*, ReturnConvention> p) { setReturn(p.first, p.second); }

    void clearReturn() {
        assert(out_success);
        assert(return_convention_checked && "Didn't check the return convention of this rewrite...");
        out_success = false;
        out_rtn = NULL;
        out_return_convention = ReturnConvention::UNSPECIFIED;
#ifndef NDEBUG
        return_convention_checked = false;
#endif
    }

    RewriterVar* getReturn(ReturnConvention required_convention) {
        assert(isSuccessful());
        assert(this->out_return_convention == required_convention);
#ifndef NDEBUG
        return_convention_checked = true;
#endif
        return this->out_rtn;
    }

    void assertReturnConvention(ReturnConvention required_convention) {
        assert(isSuccessful());
        ASSERT(this->out_return_convention == required_convention, "user asked for convention %d but got %d",
               required_convention, this->out_return_convention);
#ifndef NDEBUG
        return_convention_checked = true;
#endif
    }

    std::pair<RewriterVar*, ReturnConvention> getReturn() {
        assert(isSuccessful());
#ifndef NDEBUG
        return_convention_checked = true;
#endif
        return std::make_pair(this->out_rtn, this->out_return_convention);
    }

    bool isSuccessful() {
        assert(out_success == (out_return_convention != ReturnConvention::UNSPECIFIED));
        return out_success;
    }
};

class GetattrRewriteArgs : public _ReturnConventionBase {
public:
    Rewriter* rewriter;
    RewriterVar* obj;
    Location destination;

public:
    bool obj_hcls_guarded;
    bool obj_shape_guarded; // "shape" as in whether there are hcls attrs and where they live

    GetattrRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter), obj(obj), destination(destination), obj_hcls_guarded(false), obj_shape_guarded(false) {}
};

struct SetattrRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* obj;
    RewriterVar* attrval;

    bool out_success;

    SetattrRewriteArgs(Rewriter* rewriter, RewriterVar* obj, RewriterVar* attrval)
        : rewriter(rewriter), obj(obj), attrval(attrval), out_success(false) {}
};

struct DelattrRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* obj;

    bool out_success;

    DelattrRewriteArgs(Rewriter* rewriter, RewriterVar* obj) : rewriter(rewriter), obj(obj), out_success(false) {}
};

struct UnaryopRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* obj;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    UnaryopRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter), obj(obj), destination(destination), out_success(false), out_rtn(NULL) {}
};

struct _CallRewriteArgsBase {
public:
    Rewriter* rewriter;
    RewriterVar* obj;
    RewriterVar* arg1, *arg2, *arg3, *args;
    bool func_guarded;
    bool args_guarded;
    Location destination;

    _CallRewriteArgsBase(const _CallRewriteArgsBase& copy_from) = default;
    _CallRewriteArgsBase(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter),
          obj(obj),
          arg1(NULL),
          arg2(NULL),
          arg3(NULL),
          args(NULL),
          func_guarded(false),
          args_guarded(false),
          destination(destination) {}
};

struct CallRewriteArgs : public _CallRewriteArgsBase {
public:
    bool out_success;
    RewriterVar* out_rtn;

    CallRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : _CallRewriteArgsBase(rewriter, obj, destination), out_success(false), out_rtn(NULL) {}

    CallRewriteArgs(_CallRewriteArgsBase* copy_from)
        : _CallRewriteArgsBase(*copy_from), out_success(false), out_rtn(NULL) {}
};

class CallattrRewriteArgs : public _CallRewriteArgsBase, public _ReturnConventionBase {
public:
    CallattrRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : _CallRewriteArgsBase(rewriter, obj, destination) {}

    CallattrRewriteArgs(_CallRewriteArgsBase* copy_from) : _CallRewriteArgsBase(*copy_from) {}
};

struct GetitemRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* target;
    RewriterVar* slice;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    GetitemRewriteArgs(Rewriter* rewriter, RewriterVar* target, RewriterVar* slice, Location destination)
        : rewriter(rewriter),
          target(target),
          slice(slice),
          destination(destination),
          out_success(false),
          out_rtn(NULL) {}
};

struct BinopRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* lhs;
    RewriterVar* rhs;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    BinopRewriteArgs(Rewriter* rewriter, RewriterVar* lhs, RewriterVar* rhs, Location destination)
        : rewriter(rewriter), lhs(lhs), rhs(rhs), destination(destination), out_success(false), out_rtn(NULL) {}
};

struct CompareRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* lhs;
    RewriterVar* rhs;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    CompareRewriteArgs(Rewriter* rewriter, RewriterVar* lhs, RewriterVar* rhs, Location destination)
        : rewriter(rewriter), lhs(lhs), rhs(rhs), destination(destination), out_success(false), out_rtn(NULL) {}
};

using FunctorPointer
    = llvm::function_ref<Box*(CallRewriteArgs* rewrite_args, Box* arg1, Box* arg2, Box* arg3, Box** args)>;

// rearrangeArgumentsAndCall maps from a given set of arguments (the structure specified by an ArgPassSpec) to the
// parameter form than the receiving function expects (given by the ParamReceiveSpec).  After it does this, it will
// call `continuation` and return the result.
//
// The caller is responsible for guarding for paramspec, argspec, param_names, and defaults.
//
// rearrangeArgumentsAndCall supports both CAPI- and CXX- exception styles for continuation, and will propagate them
// back to the caller.  For now, it can also throw its own exceptions such as "not enough arguments", and will throw
// them in the CXX style.
Box* rearrangeArgumentsAndCall(ParamReceiveSpec paramspec, const ParamNames* param_names, const char* func_name,
                               Box** defaults, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                               Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names,
                               FunctorPointer continuation);

// new_args should be allocated by the caller if at least three args get passed in.
// rewrite_args will get modified in place.
ArgPassSpec bindObjIntoArgs(Box* bind_obj, RewriterVar* r_bind_obj, _CallRewriteArgsBase* rewrite_args,
                            ArgPassSpec argspec, Box*& arg1, Box*& arg2, Box*& arg3, Box** args, Box** new_args);

} // namespace pyston

#endif
