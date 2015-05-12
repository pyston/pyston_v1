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

    GetattrRewriteArgs(Rewriter* rewriter, RewriterVar* obj, Location destination)
        : rewriter(rewriter),
          obj(obj),
          destination(destination),
          out_success(false),
          out_rtn(NULL),
          obj_hcls_guarded(false) {}
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

} // namespace pyston

#endif
