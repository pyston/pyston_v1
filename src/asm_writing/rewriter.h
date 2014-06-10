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

#ifndef PYSTON_ASMWRITING_REWRITER_H
#define PYSTON_ASMWRITING_REWRITER_H

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "asm_writing/icinfo.h"
#include "core/ast.h"

namespace pyston {

class ICInfo;
class ICSlotInfo;
class ICInvalidator;
class Rewriter;

namespace assembler {
class Assembler;
}

// TODO Maybe should make these classes abstract and then pick between a debug implementation
// and a release one, instead of trying to make one class do both?

class RewriterVar {
private:
    Rewriter* rewriter;
    int argnum;
    int version;

public:
    RewriterVar() : rewriter(NULL), argnum(-100), version(-100) {}
    RewriterVar(Rewriter* rewriter, int argnum, int version);
    RewriterVar& operator=(const RewriterVar& rhs);

#ifndef NDEBUG
    void assertValid() const;
    void lock();
    void unlock();
#else
    inline void assertValid() const {}
    inline void lock() {}
    inline void unlock() {}
#endif
    int getArgnum();

    void addGuard(intptr_t val);
    void addGuardNotEq(intptr_t val);
    // More efficient than getAttr().addGuard(), but less efficient than addGuard() if the value is already available:
    void addAttrGuard(int offset, intptr_t val);

    RewriterVar getAttr(int offset, int dest);
    void incAttr(int offset);
    void setAttr(int offset, const RewriterVar& val, bool user_visible = true);
    RewriterVar move(int argnum);
    bool isInReg();
    void push();
    RewriterVar cmp(AST_TYPE::AST_TYPE cmp_type, const RewriterVar& val, int dest);
    RewriterVar toBool(int dest);
    RewriterVar add(int64_t amount);

    friend class Rewriter;
};

class Rewriter : public ICSlotRewrite::CommitHook {
private:
    std::unique_ptr<ICSlotRewrite> rewrite;
    assembler::Assembler* assembler;
    const int num_orig_args;
    const int num_temp_regs;

    void finishAssembly(int continue_offset);

    int alloca_bytes;
    int max_pushes;
    std::vector<int> pushes;
#ifndef NDEBUG
    std::unordered_map<int, int> versions;
    int next_version;
    bool changed_something;
    std::unordered_set<int> locked;
#endif
    int ndecisions;
    uint64_t decision_path;

    Rewriter(ICSlotRewrite* rewrite, int num_orig_args, int num_temp_regs);

    void addPush(int version);

public:
    static Rewriter* createRewriter(void* ic_rtn_addr, int num_orig_args, int num_temp_regs, const char* debug_name);

#ifndef NDEBUG
    int mutate(int argnum);
    void lock(int argnum);
    void unlock(int argnum);
    void checkVersion(int argnum, int version);
    void checkArgsValid();
#else
    inline int mutate(int argnum) { return 0; }
    inline void lock(int argnum) {}
    inline void unlock(int argnum) {}
    inline void checkVersion(int argnum, int version) {}
    inline void checkArgsValid() {}
#endif

    int getFuncStackSize() { return rewrite->getFuncStackSize(); }
    int getScratchRbpOffset() { return rewrite->getScratchRbpOffset(); }
    int getScratchBytes() { return rewrite->getScratchBytes(); }
    RewriterVar getRbp();
    RewriterVar getRsp();

    void addDecision(int way);

    RewriterVar alloca_(int bytes, int dest_argnum);
    RewriterVar getArg(int argnum);

    void trap();
    void nop();
    void annotate(int num);
    RewriterVar pop(int argnum);
    RewriterVar call(void* func_addr);
    RewriterVar loadConst(int argnum, intptr_t val);

    void addDependenceOn(ICInvalidator&);
    void commit();

    friend class RewriterVar;
};
}

#endif
