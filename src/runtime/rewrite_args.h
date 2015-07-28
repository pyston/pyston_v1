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

#ifndef PYSTON_RUNTIME_REWRITEARGS_H
#define PYSTON_RUNTIME_REWRITEARGS_H

#include "asm_writing/rewriter.h"

namespace pyston {

struct GetattrRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* obj;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    bool obj_hcls_guarded;
    bool obj_shape_guarded; // "shape" as in whether there are hcls attrs and where they live

    GetattrRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter),
          obj(obj),
          destination(destination),
          out_success(false),
          out_rtn(NULL),
          obj_hcls_guarded(false),
          obj_shape_guarded(false) {}
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

struct LenRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* obj;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    LenRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter), obj(obj), destination(destination), out_success(false), out_rtn(NULL) {}
};

struct CallRewriteArgs {
    Rewriter* rewriter;
    RewriterVar* obj;
    RewriterVar* arg1, *arg2, *arg3, *args;
    bool func_guarded;
    bool args_guarded;
    Location destination;

    bool out_success;
    RewriterVar* out_rtn;

    CallRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter),
          obj(obj),
          arg1(NULL),
          arg2(NULL),
          arg3(NULL),
          args(NULL),
          func_guarded(false),
          args_guarded(false),
          destination(destination),
          out_success(false),
          out_rtn(NULL) {}
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

// Passes the output arguments back through oarg.  Passes the rewrite success by setting rewrite_success.
// Directly modifies rewrite_args args in place, but only if rewrite_success got set.
// oargs needs to be pre-allocated by the caller, since it's assumed that they will want to use alloca.
// The caller is responsible for guarding for paramspec, argspec, param_names, and defaults.
// TODO Fix this function's signature.  should we pass back out through args?  the common case is that they
// match anyway.  Or maybe it should call a callback function, which could save on the common case.
void rearrangeArguments(ParamReceiveSpec paramspec, const ParamNames* param_names, const char* func_name,
                        Box** defaults, CallRewriteArgs* rewrite_args, bool& rewrite_success, ArgPassSpec argspec,
                        Box* arg1, Box* arg2, Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names,
                        Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** oargs);

// new_args should be allocated by the caller if at least three args get passed in.
// rewrite_args will get modified in place.
ArgPassSpec bindObjIntoArgs(Box* bind_obj, RewriterVar* r_bind_obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec,
                            Box*& arg1, Box*& arg2, Box*& arg3, Box** args, Box** new_args);
} // namespace pyston

#endif
