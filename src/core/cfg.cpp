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

#include "core/cfg.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "llvm/ADT/DenseSet.h"
#include "Python.h"

#include "analysis/scoping_analysis.h"
#include "codegen/unwinding.h"
#include "core/bst.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/complex.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
// This class helps keeping track of the memory locations of vregs inside the bytecode by storing them as offsets from
// the start of the bytecode if the address is inside the bytecode otherwise it is just stored as a regular pointer.
// We can't directly keep track of the locations with normal pointers because during bytecode emission the memory
// location grows several times which will move around the memory location.
// TODO: all vreg locations should be inside the bytecode but currently we still emit a few references to outside vregs
// for the BST_ClassDef and BST_FunctionDef nodes. We should try to remove them.
class TrackingVRegPtr {
private:
    union {
        long offset;
        int* ptr;
    };
    bool is_fixed_ptr;

public:
    static TrackingVRegPtr createTracking(int* vreg, BSTAllocator& alloc) {
        TrackingVRegPtr tmp;
        tmp.offset = alloc.getOffset(vreg);
        tmp.is_fixed_ptr = false;
        return tmp;
    }
    static TrackingVRegPtr createFixed(int* vreg) {
        TrackingVRegPtr tmp;
        tmp.ptr = vreg;
        tmp.is_fixed_ptr = true;
        return tmp;
    }
    static TrackingVRegPtr create(int* vreg, BSTAllocator& alloc) {
        if (alloc.isInside(vreg))
            return createTracking(vreg, alloc);
        return createFixed(vreg);
    }
    std::size_t getHash() const {
        if (is_fixed_ptr)
            return std::hash<int*>()(ptr);
        return std::hash<long>()(offset);
    }
    bool operator==(const TrackingVRegPtr& right) const {
        if (is_fixed_ptr != right.is_fixed_ptr)
            return false;
        if (is_fixed_ptr)
            return ptr == right.ptr;
        return offset == right.offset;
    }
    static TrackingVRegPtr getEmptyKey() {
        TrackingVRegPtr tmp;
        tmp.ptr = (int*)-1;
        tmp.is_fixed_ptr = true;
        return tmp;
    }
    static TrackingVRegPtr getTombstoneKey() {
        TrackingVRegPtr tmp;
        tmp.ptr = (int*)-2;
        tmp.is_fixed_ptr = true;
        return tmp;
    }
};
}
namespace llvm {
template <> struct DenseMapInfo<pyston::TrackingVRegPtr> {
    static inline pyston::TrackingVRegPtr getEmptyKey() { return pyston::TrackingVRegPtr::getEmptyKey(); }
    static inline pyston::TrackingVRegPtr getTombstoneKey() { return pyston::TrackingVRegPtr::getTombstoneKey(); }
    static unsigned getHashValue(const pyston::TrackingVRegPtr& val) { return val.getHash(); }
    static bool isEqual(const pyston::TrackingVRegPtr& lhs, const pyston::TrackingVRegPtr& rhs) { return lhs == rhs; }
};
}

namespace pyston {

template <typename Node> void fillScopingInfo(Node* node, InternedString id, ScopeInfo* scope_info) {
    node->lookup_type = scope_info->getScopeTypeOfName(id);

    if (node->lookup_type == ScopeInfo::VarScopeType::CLOSURE)
        node->closure_offset = scope_info->getClosureOffset(id);
    else if (node->lookup_type == ScopeInfo::VarScopeType::DEREF)
        node->deref_info = scope_info->getDerefInfo(id);

    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
}

void fillScopingInfo(BST_Name* node, ScopeInfo* scope_info) {
    node->lookup_type = scope_info->getScopeTypeOfName(node->id);

    if (node->lookup_type == ScopeInfo::VarScopeType::CLOSURE)
        node->closure_offset = scope_info->getClosureOffset(node->id);
    else if (node->lookup_type == ScopeInfo::VarScopeType::DEREF)
        assert(0 && "should not happen");

    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
}

ParamNames::ParamNames(AST_arguments* arguments, InternedStringPool& pool)
    : all_args_contains_names(1), takes_param_names(1), has_vararg_name(0), has_kwarg_name(0) {
    if (!arguments)
        return;

    for (int i = 0; i < arguments->args.size(); i++) {
        AST_expr* arg = arguments->args[i];
        if (arg->type == AST_TYPE::Name) {
            AST_Name* name = ast_cast<AST_Name>(arg);
            BST_Name* new_name = new BST_Name(name->id, name->lineno);
            all_args.emplace_back(new_name);
        } else {
            InternedString dot_arg_name = pool.get("." + std::to_string(i));
            auto new_name = new BST_Name(dot_arg_name, arg->lineno);
            new_name->lookup_type = ScopeInfo::VarScopeType::FAST;
            all_args.emplace_back(new_name);
        }
    }

    auto vararg_name = arguments->vararg;
    if (vararg_name) {
        has_vararg_name = 1;
        BST_Name* new_name = new BST_Name(vararg_name->id, vararg_name->lineno);
        all_args.emplace_back(new_name);
    }

    auto kwarg_name = arguments->kwarg;
    if (kwarg_name) {
        has_kwarg_name = 1;
        BST_Name* new_name = new BST_Name(kwarg_name->id, kwarg_name->lineno);
        all_args.emplace_back(new_name);
    }
}

ParamNames::ParamNames(const std::vector<const char*>& args, const char* vararg, const char* kwarg)
    : all_args_contains_names(0),
      takes_param_names(1),
      has_vararg_name(vararg && *vararg),
      has_kwarg_name(kwarg && *kwarg) {
    all_args.reserve(args.size() + has_vararg_name + has_kwarg_name);
    for (auto&& arg : args) {
        all_args.emplace_back(arg);
    }
    if (has_vararg_name)
        all_args.emplace_back(vararg);
    if (has_kwarg_name)
        all_args.emplace_back(kwarg);
}

ParamNames::~ParamNames() {
    if (all_args_contains_names) {
        for (auto&& e : all_args)
            delete e.name;
    }
}

std::vector<const char*> ParamNames::allArgsAsStr() const {
    std::vector<const char*> ret;
    ret.reserve(all_args.size());
    if (all_args_contains_names) {
        for (auto&& arg : all_args) {
            ret.push_back(arg.name->id.c_str());
        }
    } else {
        for (auto&& arg : all_args) {
            ret.push_back(arg.str);
        }
    }
    return ret;
}

// getLastLineno and getLastLinenoSub: gets the last line of a block.
// getLastLineno takes the block itself, and getLastLinenoSub takes an entry
// inside the block.  This is important because if there is a functiondef as the last
// statement in a block, we should not look inside it.
static int getLastLinenoSub(AST* ast) {
    if (ast->type == AST_TYPE::TryExcept) {
        auto te = ast_cast<AST_TryExcept>(ast);
        if (!te->orelse.empty())
            return getLastLinenoSub(te->orelse.back());
        return getLastLinenoSub(te->handlers.back()->body.back());
    }
    if (ast->type == AST_TYPE::For) {
        return getLastLinenoSub(ast_cast<AST_For>(ast)->body.back());
    }
    if (ast->type == AST_TYPE::While) {
        return getLastLinenoSub(ast_cast<AST_While>(ast)->body.back());
    }
    if (ast->type == AST_TYPE::TryFinally) {
        return getLastLinenoSub(ast_cast<AST_TryFinally>(ast)->finalbody.back());
    }
    if (ast->type == AST_TYPE::With) {
        return getLastLinenoSub(ast_cast<AST_With>(ast)->body.back());
    }
    if (ast->type == AST_TYPE::If) {
        auto if_ = ast_cast<AST_If>(ast);
        if (!if_->orelse.empty())
            return getLastLinenoSub(if_->orelse.back());
        return getLastLinenoSub(if_->body.back());
    }

    // TODO: this is not quite right if the last statement is multiline.  See exited_lineno_multiline.py
    return ast->lineno;
}

static int getLastLineno(llvm::ArrayRef<AST_stmt*> body, int default_lineno) {
    if (body.size() == 0)
        return default_lineno;
    return getLastLinenoSub(body.back());
}

llvm::SmallVector<CFGBlock*, 2> CFGBlock::successors() const {
    llvm::SmallVector<CFGBlock*, 2> successors;
    auto* last = getTerminator();
    if (last->type() == BST_TYPE::Jump) {
        successors.push_back(bst_cast<BST_Jump>(last)->target);
    } else if (last->type() == BST_TYPE::Branch) {
        assert(bst_cast<BST_Branch>(last)->iftrue != bst_cast<BST_Branch>(last)->iffalse);
        successors.push_back(bst_cast<BST_Branch>(last)->iftrue);
        successors.push_back(bst_cast<BST_Branch>(last)->iffalse);
    } else if (last->is_invoke()) {
        successors.push_back(last->get_normal_block());
        if (last->get_exc_block() != last->get_normal_block())
            successors.push_back(last->get_exc_block());
    }
    return successors;
}

void CFGBlock::connectTo(CFGBlock* successor, bool allow_backedge) {
    if (!allow_backedge) {
        assert(this->idx >= 0);
        ASSERT(successor->idx == -1 || successor->idx > this->idx, "edge from %d (%s) to %d (%s)", this->idx,
               this->info, successor->idx, successor->info);
    }
    successor->predecessors.push_back(this);
}

void CFGBlock::unconnectFrom(CFGBlock* successor) {
    successor->predecessors.erase(std::remove(successor->predecessors.begin(), successor->predecessors.end(), this),
                                  successor->predecessors.end());
}

void CFGBlock::print(const CodeConstants& code_constants, llvm::raw_ostream& stream) {
    stream << "Block " << idx;
    if (info)
        stream << " '" << info << "'";

    stream << "; Predecessors:";
    for (int j = 0; j < predecessors.size(); j++) {
        stream << " " << predecessors[j]->idx;
    }
    stream << " Successors:";
    for (CFGBlock* successor : successors()) {
        stream << " " << successor->idx;
    }
    stream << "\n";

    PrintVisitor pv(code_constants, 4, stream);
    for (BST_stmt* stmt : *this) {
        stream << "    ";
        stmt->accept(&pv);
        stream << "\n";
    }
}

static const std::string RETURN_NAME("#rtnval");

// The various reasons why a `finally' block (or similar, eg. a `with' exit block) might get entered.
// this has to go outside CFGVisitor b/c why_values can't go inside it.
enum Why : int8_t {
    FALLTHROUGH, // i.e. normal control flow
    CONTINUE,
    BREAK,
    RETURN,
    EXCEPTION,
};

static const Why why_values[] = { FALLTHROUGH, CONTINUE, BREAK, RETURN, EXCEPTION };

// A class that manages the computation of all CFGs in a module
class ModuleCFGProcessor {
public:
    ScopingAnalysis scoping;
    InternedStringPool& stringpool;
    FutureFlags future_flags;
    BoxedString* fn;
    BoxedModule* bm;

    ModuleCFGProcessor(AST* ast, bool globals_from_module, FutureFlags future_flags, BoxedString* fn, BoxedModule* bm)
        : scoping(ast, globals_from_module),
          stringpool(ast->getStringpool()),
          future_flags(future_flags),
          fn(fn),
          bm(bm) {}

    // orig_node is the node from the original ast, but 'ast' can be a desugared version.
    // For example if we convert a generator expression into a function, the new function
    // should get passed as 'ast', but the original generator expression should get
    // passed as 'orig_node' so that the scoping analysis can know what we're talking about.
    BoxedCode* runRecursively(llvm::ArrayRef<AST_stmt*> body, BoxedString* name, int lineno, AST_arguments* args,
                              AST* orig_node);
};

static std::pair<CFG*, CodeConstants> computeCFG(llvm::ArrayRef<AST_stmt*> body, AST_TYPE::AST_TYPE ast_type,
                                                 int lineno, AST_arguments* args, BoxedString* filename,
                                                 SourceInfo* source, const ParamNames& param_names, ScopeInfo* scoping,
                                                 ModuleCFGProcessor* cfgizer);

// This keeps track of the result of an instruction it's either a name, const or undefined.
struct TmpValue {
    union {
        InternedString is;
        int vreg_const;
    };
    int lineno = 0;
    enum { UNDEFINED, CONST, NAME } type = UNDEFINED;
    TmpValue() : type(UNDEFINED) {}
    explicit TmpValue(int vreg_const, int lineno) : vreg_const(vreg_const), lineno(lineno), type(CONST) {
        assert(vreg_const < 0);
    }
    TmpValue(InternedString is, int lineno) : is(is), lineno(lineno), type(NAME) {}

    bool isConst() const { return type == CONST; }
    bool isName() const { return type == NAME; }
    bool isUndefined() const { return type == UNDEFINED; }
};

// A class that crawls the AST of a single function and computes the CFG
class CFGVisitor : public ASTVisitor {
    // ---------- Types ----------
private:
    /* Explanation of ContInfo and ExcBlockInfo:
     *
     * While generating the CFG, we need to know what to do if we:
     * 1. hit a `continue'
     * 2. hit a `break'
     * 3. hit a `return'
     * 4. raise an exception
     *
     * We call these "continuations", because they're what we "continue on to" after these conditions occur.
     *
     * Various control flow constructs affect each of these:
     * - `for' and `while' affect (1-2).
     * - `try/except' affects (4).
     * - `try/finally' and `with' affect all four.
     *
     * Each of these take effect only within some chunk of code. So, notionally, we keep a stack for each of (1-4) whose
     * _top_ value says what to do if that condition occurs. The top of the continue-stack points to the block to jump
     * to if we hit a `continue', etc.
     *
     * For example, when we enter a loop, we push a pointer to the head of the loop onto the continue-stack, and a
     * pointer to the code after the loop onto the break-stack. When we visit a `break' in the loop body, we emit a jump
     * to the top of the break-stack, which is the end of the loop. After we finish visiting the loop body, we pop the
     * break- & continue-stacks, restoring our old state (maybe we were inside another loop, for example).
     *
     * It's more complicated in practice, because:
     *
     * 1. When we jump to a `finally' block, we must tell it *why* we jumped to it. After the `finally' block finishes,
     *    it uses this info to resume what we were doing before we entered it (returning, raising an exception, etc).
     *
     * 2. When we jump to a `except' block, we must record three pieces of information about the exception (its type,
     *    value, and traceback).
     *
     * So instead of four stacks of block pointers, instead we have two stacks:
     * - `continuations', a stack of ContInfos, for `continue', `break', and `return'
     * - `exc_handlers', a stack of ExcBlockInfos, for exceptions
     *
     * Read the comments in ContInfo & ExcBlockInfo for more information.
     */
    struct ContInfo {
        // where to jump to if a continue, break, or return happens respectively
        CFGBlock* continue_dest, *break_dest, *return_dest;
        // true if this continuation needs to know the reason why we entered it. `finally' blocks use this info to
        // determine how to resume execution after they finish.
        bool say_why;
        // bit-vector tracking all reasons Why we ever might enter this continuation. is only updated/used if `say_why'
        // is true. when we emit a jump to this continuation for reason w, we set the bit (did_why & (1 << w)). this is
        // used when emitting `finally' blocks to determine which continuation-cases to emit.
        int did_why;
        // name of the variable to store the reason Why we jumped in.
        InternedString why_name;

        ContInfo(CFGBlock* continue_dest, CFGBlock* break_dest, CFGBlock* return_dest, bool say_why,
                 InternedString why_name)
            : continue_dest(continue_dest),
              break_dest(break_dest),
              return_dest(return_dest),
              say_why(say_why),
              did_why(0),
              why_name(why_name) {}
    };

    struct ExcBlockInfo {
        // where to jump in case of an exception
        CFGBlock* exc_dest;
        // variable names to store the exception (type, value, traceback) in
        InternedString exc_type_name, exc_value_name, exc_traceback_name;

        // Similar to did_why: says whether the block might have been jumped-to
        bool maybe_taken;
    };

    // ---------- Member fields ----------
private:
    BoxedString* filename;
    SourceInfo* source;
    InternedStringPool& stringpool;
    ScopeInfo* scoping;
    // `root_type' is the type of the root of the AST tree that we are turning
    // into a CFG. Used when we find a "return" to check that we're inside a
    // function (otherwise we SyntaxError).
    AST_TYPE::AST_TYPE root_type;
    FutureFlags future_flags;
    CFG* cfg;
    ModuleCFGProcessor* cfgizer;

    CFGBlock* curblock;
    std::vector<ContInfo> continuations;
    std::vector<ExcBlockInfo> exc_handlers;

    // maps constants to their vreg number
    llvm::DenseMap<Box*, int> consts;
    CodeConstants code_constants;

    llvm::StringMap<BoxedString*> str_constants;
    llvm::StringMap<Box*> unicode_constants;
    // I'm not sure how well it works to use doubles as hashtable keys; thankfully
    // it's not a big deal if we get misses.
    std::unordered_map<int64_t, Box*> imaginary_constants;
    llvm::StringMap<Box*> long_constants;

    unsigned int next_var_index = 0;

    friend std::pair<CFG*, CodeConstants> computeCFG(llvm::ArrayRef<AST_stmt*> body, AST_TYPE::AST_TYPE ast_type,
                                                     int lineno, AST_arguments* args, BoxedString* filename,
                                                     SourceInfo* source, const ParamNames& param_names,
                                                     ScopeInfo* scoping, ModuleCFGProcessor* cfgizer);

public:
    CFGVisitor(BoxedString* filename, SourceInfo* source, InternedStringPool& stringpool, ScopeInfo* scoping,
               AST_TYPE::AST_TYPE root_type, FutureFlags future_flags, CFG* cfg, ModuleCFGProcessor* cfgizer)
        : filename(filename),
          source(source),
          stringpool(stringpool),
          scoping(scoping),
          root_type(root_type),
          future_flags(future_flags),
          cfg(cfg),
          cfgizer(cfgizer) {
        setInsertPoint(cfg->addDeferredBlock());
        curblock->info = "entry";
    }

    ~CFGVisitor() {
        // if we're being destroyed due to an exception, our internal invariants may be violated, but that's okay; the
        // CFG isn't going to get used anyway. (Maybe we should check that it won't be used somehow?)
        assert(continuations.size() == 0 || isUnwinding());
        assert(exc_handlers.size() == 0 || isUnwinding());
    }

    // ---------- private methods ----------
private:
    void setInsertPoint(CFGBlock* block) {
        curblock = block;
        if (block)
            cfg->placeBlock(block);
    }

    // Adds the specified instruction to the current block (invalidating pointers to previous returned
    // instuctions!) and returns a pointer to the newly created node. It automatically creates invokes if neccessary.
    // Because of the invalidation issue it is extremely important that all fields of a node get set/retrieved before a
    // new instruction get's added! The reason for this behaviour is that we directly alloc into an array which can
    // change address when we have to resize it.
    template <typename T, bool cant_throw = false, typename... Args> T* allocAndPush(Args&&... args) {
        assert(curblock->isPlaced());
        assert(curblock->allowed_to_add_stuff);
        if (exc_handlers.size() == 0 || cant_throw)
            return T::create(cfg->bytecode, std::forward<Args>(args)...);

        BST_TYPE::BST_TYPE type = T::TYPE;
        switch (type) {
            case BST_TYPE::Branch:
            case BST_TYPE::CopyVReg:
            case BST_TYPE::Dict:
            case BST_TYPE::Jump:
            case BST_TYPE::Landingpad:
            case BST_TYPE::Return:
            case BST_TYPE::SetExcInfo:
            case BST_TYPE::Tuple:
            case BST_TYPE::UncacheExcInfo:
                return T::create(cfg->bytecode, std::forward<Args>(args)...);
            default:
                break;
        };

        // We remapped asserts to just be assertion failures at this point.
        bool is_raise = (type == BST_TYPE::Raise || type == BST_TYPE::Assert);

        // If we invoke a raise statement, generate an invoke where both destinations
        // are the exception handler, since we know the non-exceptional path won't be taken.
        // TODO: would be much better (both more efficient and require less special casing)
        // if we just didn't generate this control flow as exceptions.

        CFGBlock* normal_dest = cfg->addDeferredBlock();
        // Add an extra exc_dest trampoline to prevent critical edges:
        CFGBlock* exc_dest;
        if (is_raise)
            exc_dest = normal_dest;
        else
            exc_dest = cfg->addDeferredBlock();

        auto rtn = T::create(cfg->bytecode, std::forward<Args>(args)...);
        auto rtn_offset = cfg->bytecode.getOffset(rtn);
        rtn->type_and_flags |= BST_stmt::invoke_flag;
        rtn = NULL; // we can not use rtn from here on because the next allocations invalidate the address.

        *cfg->bytecode.allocate<CFGBlock*>() = normal_dest;
        *cfg->bytecode.allocate<CFGBlock*>() = exc_dest;

        curblock->connectTo(normal_dest);
        if (!is_raise)
            curblock->connectTo(exc_dest);

        ExcBlockInfo& exc_info = exc_handlers.back();
        exc_info.maybe_taken = true;

        setInsertPoint(exc_dest);
        // TODO: need to clear some temporaries here
        auto* landingpad = new (cfg->bytecode) BST_Landingpad;
        TmpValue landingpad_name = createDstName(landingpad);

        auto* exc_unpack = BST_UnpackIntoArray::create(cfg->bytecode, 3);
        unmapExpr(landingpad_name, &exc_unpack->vreg_src);
        int* array = exc_unpack->vreg_dst;
        unmapExpr(TmpValue(exc_info.exc_type_name, 0), &array[0]);
        unmapExpr(TmpValue(exc_info.exc_value_name, 0), &array[1]);
        unmapExpr(TmpValue(exc_info.exc_traceback_name, 0), &array[2]);

        pushJump(exc_info.exc_dest);

        if (is_raise)
            curblock = NULL;
        else
            setInsertPoint(normal_dest);

        return (T*)&cfg->bytecode.getData()[rtn_offset];
    }

    template <typename T> InternedString internString(T&& s) { return stringpool.get(std::forward<T>(s)); }

    InternedString createUniqueName(llvm::Twine prefix) {
        std::string name = (prefix + llvm::Twine(next_var_index++)).str();
        return stringpool.get(std::move(name));
    }

    AST_Name* makeASTName(ASTAllocator& allocator, InternedString id, AST_TYPE::AST_TYPE ctx_type, int lineno,
                          int col_offset = 0) {
        AST_Name* name = new (allocator) AST_Name(id, ctx_type, lineno, col_offset);
        return name;
    }

    void pushLoopContinuation(CFGBlock* continue_dest, CFGBlock* break_dest) {
        assert(continue_dest
               != break_dest); // I guess this doesn't have to be true, but validates passing say_why=false
        continuations.emplace_back(continue_dest, break_dest, nullptr, false, internString(""));
    }

    void pushFinallyContinuation(CFGBlock* finally_block, InternedString why_name) {
        continuations.emplace_back(finally_block, finally_block, finally_block, true, why_name);
    }

    void popContinuation() { continuations.pop_back(); }

    void doReturn(TmpValue value) {
        assert(curblock);

        for (auto& cont : llvm::make_range(continuations.rbegin(), continuations.rend())) {
            if (cont.return_dest) {
                if (cont.say_why) {
                    pushAssign(cont.why_name, makeNum(Why::RETURN, value.lineno));
                    cont.did_why |= (1 << Why::RETURN);
                }

                pushAssign(internString(RETURN_NAME), value);
                pushJump(cont.return_dest);
                return;
            }
        }

        BST_Return* node = allocAndPush<BST_Return>();
        unmapExpr(value, &node->vreg_value);
        node->lineno = value.lineno;
        curblock = NULL;
    }

    void doContinue(AST* value) {
        assert(curblock);
        for (auto& cont : llvm::make_range(continuations.rbegin(), continuations.rend())) {
            if (cont.continue_dest) {
                if (cont.say_why) {
                    pushAssign(cont.why_name, makeNum(Why::CONTINUE, value->lineno));
                    cont.did_why |= (1 << Why::CONTINUE);
                }

                pushJump(cont.continue_dest, true);
                return;
            }
        }

        raiseSyntaxError("'continue' not properly in loop", value->lineno, value->col_offset, filename->s(), "", true);
    }

    void doBreak(AST* value) {
        assert(curblock);
        for (auto& cont : llvm::make_range(continuations.rbegin(), continuations.rend())) {
            if (cont.break_dest) {
                if (cont.say_why) {
                    pushAssign(cont.why_name, makeNum(Why::BREAK, value->lineno));
                    cont.did_why |= (1 << Why::BREAK);
                }

                pushJump(cont.break_dest, true);
                return;
            }
        }

        raiseSyntaxError("'break' outside loop", value->lineno, value->col_offset, filename->s(), "", true);
    }

    TmpValue callNonzero(TmpValue e) {
        BST_Nonzero* call = allocAndPush<BST_Nonzero>();
        call->lineno = e.lineno;
        unmapExpr(e, &call->vreg_value);
        return createDstName(call);
    }

    TmpValue createDstName(BST_stmt_with_dest* rtn) {
        TmpValue name(nodeName(), rtn->lineno);
        unmapExpr(name, &rtn->vreg_dst);
        return name;
    }

    int remapInternedString(InternedString id) {
        // all our string constants are interned so we can just use normal string handling
        return makeStr(id.s()).vreg_const;
    }

    TmpValue remapName(AST_Name* name) {
        if (!name)
            return TmpValue();

        // we treat None as a constant because it can never get modified
        if (name->id == "None")
            return makeNone(name->lineno);

        auto rtn = allocAndPush<BST_LoadName>();
        rtn->lineno = name->lineno;
        rtn->index_id = remapInternedString(name->id);
        rtn->lookup_type = name->lookup_type;
        fillScopingInfo(rtn, name->id, scoping);
        return createDstName(rtn);
    }

    int addConst(STOLEN(Box*) o) {
        // make sure all consts are unique
        auto it = consts.find(o);
        if (it != consts.end()) {
            Py_DECREF(o);
            return it->second;
        }
        int vreg = code_constants.createVRegEntryForConstant(o);
        consts[o] = vreg;
        return vreg;
    }

    static int64_t getDoubleBits(double d) {
        int64_t rtn;
        static_assert(sizeof(rtn) == sizeof(d), "");
        memcpy(&rtn, &d, sizeof(d));
        return rtn;
    }

    TmpValue remapNum(AST_Num* num) {
        Box* o = createConstObject(num);
        return TmpValue(addConst(o), num->lineno);
    }

    TmpValue makeStr(llvm::StringRef str, int lineno = 0) {
        AST_Str ast_str(str.str());
        Box* o = createConstObject(&ast_str);
        return TmpValue(addConst(o), lineno);
    }

    TmpValue remapStr(AST_Str* str) {
        Box* o = createConstObject(str);
        return TmpValue(addConst(o), str->lineno);
    }

    TmpValue makeNum(int64_t n, int lineno) {
        AST_Num ast_num;
        ast_num.num_type = AST_Num::INT;
        ast_num.n_int = n;
        Box* o = createConstObject(&ast_num);
        return TmpValue(addConst(o), lineno);
    }

    TmpValue makeNone(int lineno) {
        int vreg_const = addConst(incref(Py_None));
        return TmpValue(vreg_const, lineno);
    }

    TmpValue applyComprehensionCall(AST_ListComp* node, TmpValue name) {
        TmpValue elt = remapExpr(node->elt);
        return makeCallAttr(name, internString("append"), true, { elt });
    }

    TmpValue _dup(TmpValue val) {
        if (val.isName()) {
            BST_CopyVReg* assign = allocAndPush<BST_CopyVReg>();
            assign->lineno = val.lineno;

            auto id = TrackingVRegPtr::createTracking(&assign->vreg_src, cfg->bytecode);
            assert(!id_vreg.count(id));
            id_vreg[id] = val.is;

            return createDstName(assign);
        }
        return val;
    }

    template <typename CompType> TmpValue remapComprehension(CompType* node) {
        assert(curblock);

        auto* list = allocAndPush<BST_List>(0);
        list->lineno = node->lineno;
        TmpValue rtn_name = createDstName(list);
        struct BlockInfo {
            CFGBlock* exit_block;
            CFGBlock* test_block;
            InternedString is;
        };
        llvm::SmallVector<BlockInfo, 4> blocks;

        for (int i = 0, n = node->generators.size(); i < n; i++) {
            AST_comprehension* c = node->generators[i];
            bool is_innermost = (i == n - 1);

            TmpValue remapped_iter = remapExpr(c->iter);
            BST_GetIter* iter_call = allocAndPush<BST_GetIter>();
            unmapExpr(remapped_iter, &iter_call->vreg_value);
            iter_call->lineno = c->target->lineno; // Not sure if this should be c->target or c->iter
            TmpValue iter_name(nodeName("lc_iter", i), node->lineno);
            unmapExpr(iter_name, &iter_call->vreg_dst);

            CFGBlock* test_block = cfg->addDeferredBlock();
            test_block->info = "comprehension_test";
            // printf("Test block for comp %d is %d\n", i, test_block->idx);
            pushJump(test_block);

            setInsertPoint(test_block);
            auto dup_iter_name = _dup(iter_name);
            BST_HasNext* test_call = allocAndPush<BST_HasNext>();
            unmapExpr(dup_iter_name, &test_call->vreg_value);
            test_call->lineno = c->target->lineno;
            TmpValue tmp_test_name = createDstName(test_call);

            CFGBlock* body_block = cfg->addDeferredBlock();
            body_block->info = "comprehension_body";
            CFGBlock* exit_block = cfg->addDeferredBlock();
            exit_block->info = "comprehension_exit";
            blocks.emplace_back(BlockInfo{ exit_block, test_block, iter_name.is });
            // printf("Body block for comp %d is %d\n", i, body_block->idx);

            BST_Branch* br = allocAndPush<BST_Branch>();
            br->lineno = node->lineno;
            unmapExpr(tmp_test_name, &br->vreg_test);
            br->iftrue = (CFGBlock*)body_block;
            br->iffalse = (CFGBlock*)exit_block;
            curblock->connectTo(body_block);
            curblock->connectTo(exit_block);

            setInsertPoint(body_block);
            TmpValue next_name(nodeName(), node->lineno);
            pushAssign(next_name, makeCallAttr(_dup(iter_name), internString("next"), true));
            pushAssign(c->target, next_name);

            for (AST_expr* if_condition : c->ifs) {
                TmpValue remapped = callNonzero(remapExpr(if_condition));
                BST_Branch* br = allocAndPush<BST_Branch>();
                unmapExpr(remapped, &br->vreg_test);

                // Put this below the entire body?
                CFGBlock* body_tramp = cfg->addDeferredBlock();
                body_tramp->info = "comprehension_if_trampoline";
                // printf("body_tramp for %d is %d\n", i, body_tramp->idx);
                CFGBlock* body_continue = cfg->addDeferredBlock();
                body_continue->info = "comprehension_if_continue";
                // printf("body_continue for %d is %d\n", i, body_continue->idx);

                br->iffalse = (CFGBlock*)body_tramp;
                curblock->connectTo(body_tramp);
                br->iftrue = (CFGBlock*)body_continue;
                curblock->connectTo(body_continue);

                setInsertPoint(body_tramp);
                pushJump(test_block, true);

                setInsertPoint(body_continue);
            }

            if (is_innermost) {
                applyComprehensionCall(node, _dup(rtn_name));

                pushJump(test_block, true);
            } else {
                // continue onto the next comprehension and add to this body
            }
        }

        assert(blocks.size());
        for (int i = blocks.size() - 1; i >= 0; --i) {
            // every exit block except the first one jumps to the previous test block (because we add them in reverse)
            // the first one is the block where we continue execution
            setInsertPoint(blocks[i].exit_block);
            if (i > 0) {
                makeKill(blocks[i].is);
                pushJump(blocks[i - 1].test_block, true);
            }
        }

        return rtn_name;
    }

    void pushJump(CFGBlock* target, bool allow_backedge = false, int lineno = 0) {
        BST_Jump* rtn = allocAndPush<BST_Jump>();
        rtn->target = (CFGBlock*)target;
        rtn->lineno = lineno;

        curblock->connectTo(target, allow_backedge);
        curblock = nullptr;
    }

    // NB. can generate blocks, because callNonzero can
    BST_Branch* makeBranch(TmpValue test) {
        auto expr = callNonzero(test);
        BST_Branch* rtn = allocAndPush<BST_Branch>();
        unmapExpr(expr, &rtn->vreg_test);
        rtn->lineno = test.lineno;
        return rtn;
    }

    // NB. this can (but usually doesn't) generate new blocks, which is why we require `iftrue' and `iffalse' to be
    // deferred, to avoid heisenbugs. of course, this doesn't allow these branches to be backedges, but that hasn't yet
    // been necessary.
    void pushBranch(TmpValue test, CFGBlock* iftrue, CFGBlock* iffalse) {
        assert(iftrue->idx == -1 && iffalse->idx == -1);
        BST_Branch* branch = makeBranch(test);
        branch->iftrue = (CFGBlock*)iftrue;
        branch->iffalse = (CFGBlock*)iffalse;
        curblock->connectTo(iftrue);
        curblock->connectTo(iffalse);
        curblock = nullptr;
    }

    void pushReraise(int lineno, InternedString exc_type_name, InternedString exc_value_name,
                     InternedString exc_traceback_name) {
        auto raise = allocAndPush<BST_Raise>();
        raise->lineno = lineno;
        unmapExpr(TmpValue(exc_type_name, lineno), &raise->vreg_arg0);
        unmapExpr(TmpValue(exc_value_name, lineno), &raise->vreg_arg1);
        unmapExpr(TmpValue(exc_traceback_name, lineno), &raise->vreg_arg2);
        curblock = nullptr;
    }

    AST_expr* makeASTLoadAttribute(ASTAllocator& allocator, AST_expr* base, InternedString name, bool clsonly) {
        AST_expr* rtn;
        if (clsonly) {
            AST_ClsAttribute* attr = new (allocator) AST_ClsAttribute();
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        } else {
            AST_Attribute* attr = new (allocator) AST_Attribute();
            attr->ctx_type = AST_TYPE::Load;
            attr->value = base;
            attr->attr = name;
            rtn = attr;
        }
        rtn->col_offset = base->col_offset;
        rtn->lineno = base->lineno;
        return rtn;
    }

    TmpValue makeLoadAttribute(TmpValue base, InternedString attr, bool clsonly) {
        BST_LoadAttr* rtn = allocAndPush<BST_LoadAttr>();
        rtn->clsonly = clsonly;
        unmapExpr(base, &rtn->vreg_value);
        rtn->index_attr = remapInternedString(attr);
        rtn->lineno = base.lineno;
        return createDstName(rtn);
    }

    AST_Call* makeASTCall(ASTAllocator& allocator, AST_expr* func) {
        AST_Call* call = new (allocator) AST_Call();
        call->starargs = NULL;
        call->kwargs = NULL;
        call->func = func;
        call->col_offset = func->col_offset;
        call->lineno = func->lineno;
        return call;
    }

    AST_Call* makeASTCall(ASTAllocator& allocator, AST_expr* func, AST_expr* arg0) {
        auto call = makeASTCall(allocator, func);
        call->args.push_back(arg0);
        return call;
    }

    AST_Call* makeASTCall(ASTAllocator& allocator, AST_expr* func, AST_expr* arg0, AST_expr* arg1) {
        auto call = makeASTCall(allocator, func);
        call->args.push_back(arg0);
        call->args.push_back(arg1);
        return call;
    }

    TmpValue makeCall(TmpValue func, llvm::ArrayRef<TmpValue> args = {}) {
        BST_CallFunc* rtn = allocAndPush<BST_CallFunc>(args.size(), 0 /* num keywords */);
        unmapExpr(func, &rtn->vreg_func);
        for (int i = 0; i < args.size(); ++i) {
            unmapExpr(args[i], &rtn->elts[i]);
        }
        rtn->lineno = func.lineno;
        return createDstName(rtn);
    }

    TmpValue makeCallAttr(TmpValue target, InternedString attr, bool is_cls, llvm::ArrayRef<TmpValue> args = {}) {
        BST_Call* rtn = NULL;
        if (!is_cls) {
            BST_CallAttr* call = allocAndPush<BST_CallAttr>(args.size(), 0 /* num keywords */);
            call->index_attr = remapInternedString(attr);
            unmapExpr(target, &call->vreg_value);
            for (int i = 0; i < args.size(); ++i) {
                unmapExpr(args[i], &call->elts[i]);
            }
            rtn = call;
        } else {
            BST_CallClsAttr* call = allocAndPush<BST_CallClsAttr>(args.size(), 0 /* num keywords */);
            call->index_attr = remapInternedString(attr);
            unmapExpr(target, &call->vreg_value);
            for (int i = 0; i < args.size(); ++i) {
                unmapExpr(args[i], &call->elts[i]);
            }
            rtn = call;
        }
        rtn->lineno = target.lineno;
        return createDstName(rtn);
    }

    TmpValue makeCompare(AST_TYPE::AST_TYPE oper, TmpValue left, TmpValue right) {
        auto compare = allocAndPush<BST_Compare>();
        compare->op = oper;
        unmapExpr(left, &compare->vreg_left);
        unmapExpr(right, &compare->vreg_comparator);
        return createDstName(compare);
    }

    void pushAssign(AST_expr* target, TmpValue val) {
        if (target->type == AST_TYPE::Name) {
            pushStoreName(ast_cast<AST_Name>(target)->id, val);
        } else if (target->type == AST_TYPE::Subscript) {
            AST_Subscript* s = ast_cast<AST_Subscript>(target);
            assert(s->ctx_type == AST_TYPE::Store);

            auto remaped_value = remapExpr(s->value);
            if (isSlice(s->slice)) {
                auto* slice = ast_cast<AST_Slice>((AST_Slice*)s->slice);

                auto remaped_lower = remapExpr(slice->lower);
                auto remaped_upper = remapExpr(slice->upper);

                auto* s_target = allocAndPush<BST_StoreSubSlice>();
                s_target->lineno = val.lineno;
                unmapExpr(remaped_value, &s_target->vreg_target);
                unmapExpr(remaped_lower, &s_target->vreg_lower);
                unmapExpr(remaped_upper, &s_target->vreg_upper);
                unmapExpr(val, &s_target->vreg_value);
            } else {
                auto remaped_slice = remapSlice(s->slice);

                auto* s_target = allocAndPush<BST_StoreSub>();
                s_target->lineno = val.lineno;
                unmapExpr(remaped_value, &s_target->vreg_target);
                unmapExpr(remaped_slice, &s_target->vreg_slice);
                unmapExpr(val, &s_target->vreg_value);
            }

        } else if (target->type == AST_TYPE::Attribute) {
            AST_Attribute* a = ast_cast<AST_Attribute>(target);
            auto remapped_value = remapExpr(a->value);

            BST_StoreAttr* a_target = allocAndPush<BST_StoreAttr>();
            unmapExpr(val, &a_target->vreg_value);
            unmapExpr(remapped_value, &a_target->vreg_target);
            a_target->index_attr = remapInternedString(scoping->mangleName(a->attr));
            a_target->lineno = a->lineno;
        } else if (target->type == AST_TYPE::Tuple || target->type == AST_TYPE::List) {
            std::vector<AST_expr*>* elts;
            if (target->type == AST_TYPE::Tuple) {
                AST_Tuple* _t = ast_cast<AST_Tuple>(target);
                assert(_t->ctx_type == AST_TYPE::Store);
                elts = &_t->elts;
            } else {
                AST_List* _t = ast_cast<AST_List>(target);
                assert(_t->ctx_type == AST_TYPE::Store);
                elts = &_t->elts;
            }

            BST_UnpackIntoArray* unpack = allocAndPush<BST_UnpackIntoArray>(elts->size());
            unmapExpr(val, &unpack->vreg_src);
            unpack->lineno = val.lineno;

            llvm::SmallVector<TmpValue, 8> tmp_names;
            for (int i = 0; i < elts->size(); i++) {
                TmpValue tmp_name(nodeName("", i), (*elts)[i]->lineno);
                unmapExpr(tmp_name, &unpack->vreg_dst[i]);
                tmp_names.emplace_back(tmp_name);
            }

            for (int i = 0; i < elts->size(); i++) {
                pushAssign((*elts)[i], tmp_names[i]);
            }
        } else {
            RELEASE_ASSERT(0, "%d", target->type);
        }
    }

    void pushAssign(TmpValue dst, TmpValue val) {
        assert(dst.isName());
        InternedString id = dst.is;
        if (id.isCompilerCreatedName()) {
            if (val.isConst()) {
                BST_CopyVReg* assign = allocAndPush<BST_CopyVReg>();
                assign->lineno = val.lineno;
                unmapExpr(val, &assign->vreg_src);
                unmapExpr(dst, &assign->vreg_dst);
                return;
            }
        }

        pushStoreName(id, val);
    }

    void pushAssign(InternedString id, TmpValue val) { pushAssign(TmpValue(id, val.lineno), val); }

    AST_stmt* makeASTExpr(ASTAllocator& allocator, AST_expr* expr) {
        AST_Expr* stmt = new (allocator) AST_Expr();
        stmt->value = expr;
        stmt->lineno = expr->lineno;
        stmt->col_offset = expr->col_offset;
        return stmt;
    }

    InternedString nodeName() { return createUniqueName("#"); }

    InternedString nodeName(llvm::StringRef suffix) { return createUniqueName(llvm::Twine("#") + suffix + "_"); }

    InternedString nodeName(llvm::StringRef suffix, int idx) {
        return createUniqueName(llvm::Twine("#") + suffix + "_" + llvm::Twine(idx) + "_");
    }

    TmpValue remapAttribute(AST_Attribute* node) {
        auto remapped_value = remapExpr(node->value);
        BST_LoadAttr* rtn = allocAndPush<BST_LoadAttr>();
        rtn->lineno = node->lineno;
        rtn->index_attr = remapInternedString(scoping->mangleName(node->attr));
        unmapExpr(remapped_value, &rtn->vreg_value);
        return createDstName(rtn);
    }

    // This functions makes sure that AssignVRegsVisitor will fill in the correct vreg number in the supplied pointer to
    // a integer.
    // We needs this because the vregs get only assigned after the CFG is completely constructed.
    llvm::DenseMap<TrackingVRegPtr, InternedString> id_vreg;
    void unmapExpr(TmpValue val, int* vreg) {
        if (val.isConst()) {
            *vreg = val.vreg_const;
            return;
        } else if (val.isName()) {
            auto id = TrackingVRegPtr::create(vreg, cfg->bytecode);
            assert(!id_vreg.count(id));
            id_vreg[id] = val.is;
            return;
        } else if (val.isUndefined()) {
            *vreg = VREG_UNDEFINED;
            return;
        }

        assert(0);
    }

    TmpValue remapBinOp(AST_BinOp* node) {
        auto remapped_left = remapExpr(node->left);
        auto remapped_right = remapExpr(node->right);

        BST_BinOp* rtn = allocAndPush<BST_BinOp>();
        rtn->lineno = node->lineno;
        rtn->op_type = remapBinOpType(node->op_type);
        unmapExpr(remapped_left, &rtn->vreg_left);
        unmapExpr(remapped_right, &rtn->vreg_right);
        return createDstName(rtn);
    }

    TmpValue remapBoolOp(AST_BoolOp* node) {
        assert(curblock);

        InternedString name = nodeName();

        CFGBlock* starting_block = curblock;
        CFGBlock* exit_block = cfg->addDeferredBlock();

        for (int i = 0; i < node->values.size() - 1; i++) {
            TmpValue val = remapExpr(node->values[i]);
            pushAssign(name, _dup(val));

            auto remapped_br_test = callNonzero(val);
            BST_Branch* br = allocAndPush<BST_Branch>();
            unmapExpr(remapped_br_test, &br->vreg_test);
            br->lineno = val.lineno;

            CFGBlock* was_block = curblock;
            CFGBlock* next_block = cfg->addDeferredBlock();
            CFGBlock* crit_break_block = cfg->addDeferredBlock();
            was_block->connectTo(next_block);
            was_block->connectTo(crit_break_block);

            if (node->op_type == AST_TYPE::Or) {
                br->iftrue = (CFGBlock*)crit_break_block;
                br->iffalse = (CFGBlock*)next_block;
            } else if (node->op_type == AST_TYPE::And) {
                br->iffalse = (CFGBlock*)crit_break_block;
                br->iftrue = (CFGBlock*)next_block;
            } else {
                RELEASE_ASSERT(0, "");
            }

            setInsertPoint(crit_break_block);

            pushJump(exit_block);

            setInsertPoint(next_block);
        }

        TmpValue final_val = remapExpr(node->values[node->values.size() - 1]);
        pushAssign(name, final_val);
        pushJump(exit_block);

        setInsertPoint(exit_block);

        return TmpValue(name, node->lineno);
    }

    template <typename T>
    void remapCallHelper(T* dst_node, AST_Call* node, llvm::ArrayRef<TmpValue> remapped_args,
                         llvm::ArrayRef<TmpValue> remapped_keywords) {
        for (int i = 0; i < node->args.size(); ++i) {
            unmapExpr(remapped_args[i], &dst_node->elts[i]);
        }
        for (int i = 0; i < node->keywords.size(); ++i) {
            unmapExpr(remapped_keywords[i], &dst_node->elts[node->args.size() + i]);
        }
    }

    TmpValue remapCall(AST_Call* node) {
        BST_Call* rtn_shared = NULL;

        TmpValue remapped_func;
        if (node->func->type != AST_TYPE::Attribute && node->func->type != AST_TYPE::ClsAttribute)
            remapped_func = remapExpr(node->func);

        llvm::SmallVector<TmpValue, 8> remapped_args;
        for (int i = 0; i < node->args.size(); ++i) {
            remapped_args.emplace_back(remapExpr(node->args[i]));
        }
        llvm::SmallVector<TmpValue, 8> remapped_keywords;
        for (int i = 0; i < node->keywords.size(); ++i) {
            remapped_keywords.emplace_back(remapExpr(node->keywords[i]->value));
        }

        auto remapped_starargs = remapExpr(node->starargs);
        auto remapped_kwargs = remapExpr(node->kwargs);

        if (node->func->type == AST_TYPE::Attribute) {
            auto* attr = ast_cast<AST_Attribute>(node->func);
            auto remapped_value = remapExpr(attr->value);

            BST_CallAttr* rtn = allocAndPush<BST_CallAttr>(node->args.size(), node->keywords.size());
            rtn->index_attr = remapInternedString(scoping->mangleName(attr->attr));
            unmapExpr(remapped_value, &rtn->vreg_value);
            remapCallHelper(rtn, node, remapped_args, remapped_keywords);
            rtn_shared = rtn;
        } else if (node->func->type == AST_TYPE::ClsAttribute) {
            auto* attr = ast_cast<AST_ClsAttribute>(node->func);
            auto remapped_value = remapExpr(attr->value);

            BST_CallClsAttr* rtn = allocAndPush<BST_CallClsAttr>(node->args.size(), node->keywords.size());
            rtn->index_attr = remapInternedString(scoping->mangleName(attr->attr));
            unmapExpr(remapped_value, &rtn->vreg_value);
            remapCallHelper(rtn, node, remapped_args, remapped_keywords);
            rtn_shared = rtn;
        } else {
            BST_CallFunc* rtn = allocAndPush<BST_CallFunc>(node->args.size(), node->keywords.size());
            unmapExpr(remapped_func, &rtn->vreg_func);
            remapCallHelper(rtn, node, remapped_args, remapped_keywords);
            rtn_shared = rtn;
        }

        rtn_shared->lineno = node->lineno;

        if (node->keywords.size()) {
            llvm::SmallVector<BoxedString*, 8> keywords_names;
            for (auto kw : node->keywords) {
                keywords_names.push_back(kw->arg.getBox());
            }
            rtn_shared->index_keyword_names = code_constants.addKeywordNames(keywords_names);
        }

        unmapExpr(remapped_starargs, &rtn_shared->vreg_starargs);
        unmapExpr(remapped_kwargs, &rtn_shared->vreg_kwargs);
        return createDstName(rtn_shared);
    }

    TmpValue remapClsAttribute(AST_ClsAttribute* node) {
        auto remapped_value = remapExpr(node->value);
        BST_LoadAttr* rtn = allocAndPush<BST_LoadAttr>();
        rtn->clsonly = true;
        rtn->lineno = node->lineno;
        rtn->index_attr = remapInternedString(scoping->mangleName(node->attr));
        unmapExpr(remapped_value, &rtn->vreg_value);
        return createDstName(rtn);
    }

    TmpValue remapCompare(AST_Compare* node) {
        assert(curblock);

        // special case unchained comparisons to avoid generating a unnecessary complex cfg.
        if (node->ops.size() == 1) {
            auto remapped_left = remapExpr(node->left);
            assert(node->comparators.size() == 1);
            auto remapped_comp = remapExpr(node->comparators[0]);

            BST_Compare* rtn = allocAndPush<BST_Compare>();
            rtn->lineno = node->lineno;
            rtn->op = node->ops[0];
            unmapExpr(remapped_left, &rtn->vreg_left);
            unmapExpr(remapped_comp, &rtn->vreg_comparator);
            return createDstName(rtn);
        } else {
            TmpValue name(nodeName(), node->lineno);

            CFGBlock* exit_block = cfg->addDeferredBlock();
            TmpValue left = remapExpr(node->left);

            for (int i = 0; i < node->ops.size(); i++) {
                if (i > 0)
                    makeKill(name.is);

                TmpValue right = remapExpr(node->comparators[i]);
                auto remapped_comp = right;
                if (i < node->ops.size() - 1)
                    remapped_comp = _dup(right);

                BST_Compare* val = allocAndPush<BST_Compare>();
                val->lineno = node->lineno;
                unmapExpr(left, &val->vreg_left);
                unmapExpr(remapped_comp, &val->vreg_comparator);
                val->op = node->ops[i];
                unmapExpr(name, &val->vreg_dst);

                if (i == node->ops.size() - 1) {
                    continue;
                }

                auto remapped_br_test = callNonzero(_dup(name));
                BST_Branch* br = allocAndPush<BST_Branch>();
                unmapExpr(remapped_br_test, &br->vreg_test);

                CFGBlock* was_block = curblock;
                CFGBlock* next_block = cfg->addDeferredBlock();
                CFGBlock* crit_break_block = cfg->addDeferredBlock();
                was_block->connectTo(next_block);
                was_block->connectTo(crit_break_block);

                br->iffalse = (CFGBlock*)crit_break_block;
                br->iftrue = (CFGBlock*)next_block;

                setInsertPoint(crit_break_block);
                pushJump(exit_block);

                setInsertPoint(next_block);

                left = right;
            }

            pushJump(exit_block);
            setInsertPoint(exit_block);

            return name;
        }
    }

    TmpValue remapDict(AST_Dict* node) {
        BST_Dict* rtn = allocAndPush<BST_Dict>();
        rtn->lineno = node->lineno;

        TmpValue dict_name = createDstName(rtn);

        for (int i = 0; i < node->keys.size(); i++) {
            auto remapped_value = remapExpr(node->values[i]);
            auto remapped_target = _dup(dict_name);
            auto remapped_slice = remapExpr(node->keys[i]);

            BST_StoreSub* store = allocAndPush<BST_StoreSub>();
            store->lineno = node->values[i]->lineno;
            unmapExpr(remapped_value, &store->vreg_value);
            unmapExpr(remapped_target, &store->vreg_target);
            unmapExpr(remapped_slice, &store->vreg_slice);
        }

        return dict_name;
    }

    TmpValue remapEllipsis(AST_Ellipsis* node) {
        int vreg_const = addConst(incref(Ellipsis));
        return TmpValue(vreg_const, node->lineno);
    }

    TmpValue remapExtSlice(AST_ExtSlice* node) {
        llvm::SmallVector<TmpValue, 8> remmaped_elts;
        for (int i = 0; i < node->dims.size(); ++i) {
            remmaped_elts.emplace_back(remapSlice(node->dims[i]));
        }

        auto* rtn = allocAndPush<BST_Tuple>(node->dims.size());
        rtn->lineno = node->lineno;
        for (int i = 0; i < node->dims.size(); ++i) {
            unmapExpr(remmaped_elts[i], &rtn->elts[i]);
        }
        return createDstName(rtn);
    }

    // This is a helper function used for generator expressions and comprehensions.
    // TODO(rntz): use this to handle unscoped (i.e. list) comprehensions as well?
    void emitComprehensionLoops(ASTAllocator& allocator, int lineno, std::vector<AST_stmt*>* insert_point,
                                const std::vector<AST_comprehension*>& comprehensions, AST_expr* first_generator,
                                std::function<void(std::vector<AST_stmt*>*)> do_yield) {
        for (int i = 0; i < comprehensions.size(); i++) {
            AST_comprehension* c = comprehensions[i];

            AST_For* loop = new (allocator) AST_For();
            loop->target = c->target;
            loop->iter = (i == 0) ? first_generator : c->iter;
            loop->lineno = lineno;

            insert_point->push_back(loop);
            insert_point = &loop->body;

            for (AST_expr* if_condition : c->ifs) {
                AST_If* if_block = new (allocator) AST_If();
                if_block->lineno = if_condition->lineno;
                // Note: don't call callNonzero here, since we are generating
                // AST inside a new functiondef which will go through the CFG
                // process again.
                if_block->test = if_condition;

                insert_point->push_back(if_block);
                insert_point = &if_block->body;
            }
        }

        do_yield(insert_point);
    }

    TmpValue remapGeneratorExp(AST_GeneratorExp* node) {
        assert(node->generators.size());

        // We need to evaluate the first for-expression immediately, as the PEP dictates; so we pass it in as an
        // argument to the function we create. See
        // https://www.python.org/dev/peps/pep-0289/#early-binding-versus-late-binding
        TmpValue first = remapExpr(node->generators[0]->iter);
        InternedString arg_name = internString("#arg");

        ASTAllocator allocator;
        AST_arguments* genexp_args = new (allocator) AST_arguments();
        genexp_args->args.push_back(makeASTName(allocator, arg_name, AST_TYPE::Param, node->lineno));
        std::vector<AST_stmt*> new_body;
        emitComprehensionLoops(allocator, node->lineno, &new_body, node->generators,
                               makeASTName(allocator, arg_name, AST_TYPE::Load, node->lineno, /* col_offset */ 0),
                               [this, node, &allocator](std::vector<AST_stmt*>* insert_point) {
                                   auto y = new (allocator) AST_Yield();
                                   y->value = node->elt;
                                   y->lineno = node->lineno;
                                   insert_point->push_back(makeASTExpr(allocator, y));
                               });

        // I'm not sure this actually gets used
        static BoxedString* gen_name = getStaticString("<generator>");

        BoxedCode* code = cfgizer->runRecursively(new_body, gen_name, node->lineno, genexp_args, node);
        BST_MakeFunction* mkfunc = allocAndPush<BST_MakeFunction>(0, 0);
        mkfunc->lineno = node->lineno;
        mkfunc->vreg_code_obj = addConst(code);
        TmpValue func_var_name = createDstName(mkfunc);

        return makeCall(func_var_name, { first });
    }

    void emitComprehensionYield(ASTAllocator& allocator, AST_DictComp* node, InternedString dict_name,
                                std::vector<AST_stmt*>* insert_point) {
        // add entry to the dictionary
        AST_expr* setitem
            = makeASTLoadAttribute(allocator, makeASTName(allocator, dict_name, AST_TYPE::Load, node->lineno),
                                   internString("__setitem__"), true);
        insert_point->push_back(makeASTExpr(allocator, makeASTCall(allocator, setitem, node->key, node->value)));
    }

    void emitComprehensionYield(ASTAllocator& allocator, AST_SetComp* node, InternedString set_name,
                                std::vector<AST_stmt*>* insert_point) {
        // add entry to the dictionary
        AST_expr* add = makeASTLoadAttribute(allocator, makeASTName(allocator, set_name, AST_TYPE::Load, node->lineno),
                                             internString("add"), true);
        insert_point->push_back(makeASTExpr(allocator, makeASTCall(allocator, add, node->elt)));
    }

    template <typename ResultType, typename CompType> TmpValue remapScopedComprehension(CompType* node) {
        // See comment in remapGeneratorExp re early vs. late binding.
        TmpValue first = remapExpr(node->generators[0]->iter);
        InternedString arg_name = internString("#arg");

        ASTAllocator allocator;
        AST_arguments* args = new (allocator) AST_arguments();
        args->args.push_back(makeASTName(allocator, arg_name, AST_TYPE::Param, node->lineno));

        std::vector<AST_stmt*> new_body;

        InternedString rtn_name = internString("#comp_rtn");
        auto asgn = new (allocator) AST_Assign();
        asgn->targets.push_back(makeASTName(allocator, rtn_name, AST_TYPE::Store, node->lineno));
        asgn->value = new (allocator) ResultType();
        asgn->lineno = node->lineno;
        new_body.push_back(asgn);

        auto lambda = [&](std::vector<AST_stmt*>* insert_point) {
            emitComprehensionYield(allocator, node, rtn_name, insert_point);
        };
        AST_Name* first_name
            = makeASTName(allocator, internString("#arg"), AST_TYPE::Load, node->lineno, /* col_offset */ 0);
        emitComprehensionLoops(allocator, node->lineno, &new_body, node->generators, first_name, lambda);

        auto rtn = new (allocator) AST_Return();
        rtn->value = makeASTName(allocator, rtn_name, AST_TYPE::Load, node->lineno, /* col_offset */ 0);
        rtn->lineno = node->lineno;
        new_body.push_back(rtn);

        // I'm not sure this actually gets used
        static BoxedString* comp_name = getStaticString("<comperehension>");

        BoxedCode* code = cfgizer->runRecursively(new_body, comp_name, node->lineno, args, node);
        BST_MakeFunction* mkfunc = allocAndPush<BST_MakeFunction>(0, 0);
        mkfunc->lineno = node->lineno;
        mkfunc->vreg_code_obj = addConst(code);
        TmpValue func_var_name = createDstName(mkfunc);

        return makeCall(func_var_name, { first });
    }

    TmpValue remapIfExp(AST_IfExp* node) {
        assert(curblock);

        InternedString rtn_name = nodeName();
        CFGBlock* iftrue = cfg->addDeferredBlock();
        CFGBlock* iffalse = cfg->addDeferredBlock();
        CFGBlock* exit_block = cfg->addDeferredBlock();

        pushBranch(remapExpr(node->test), iftrue, iffalse);

        // if true block
        setInsertPoint(iftrue);
        iftrue->info = "iftrue";
        pushAssign(rtn_name, remapExpr(node->body));
        pushJump(exit_block);

        // if false block
        setInsertPoint(iffalse);
        iffalse->info = "iffalse";
        pushAssign(rtn_name, remapExpr(node->orelse));
        pushJump(exit_block);

        // exit block
        setInsertPoint(exit_block);

        return TmpValue(rtn_name, node->lineno);
    }

    TmpValue remapLambda(AST_Lambda* node) {
        ASTAllocator allocator;
        auto stmt = new (allocator) AST_Return;
        stmt->lineno = node->lineno;

        stmt->value = node->body; // don't remap now; will be CFG'ed later

        llvm::SmallVector<TmpValue, 8> remapped_defaults;
        for (int i = 0; i < node->args->defaults.size(); ++i) {
            remapped_defaults.emplace_back(remapExpr(node->args->defaults[i]));
        }

        auto mkfn = allocAndPush<BST_MakeFunction>(0 /* decorators */, node->args->defaults.size());
        mkfn->lineno = node->lineno;

        for (int i = 0; i < node->args->defaults.size(); ++i) {
            unmapExpr(remapped_defaults[i], &mkfn->elts[i]);
        }

        auto name = getStaticString("<lambda>");
        auto* code = cfgizer->runRecursively({ stmt }, name, mkfn->lineno, node->args, node);
        mkfn->vreg_code_obj = addConst(code);

        return createDstName(mkfn);
    }

    TmpValue remapLangPrimitive(AST_LangPrimitive* node) {
        // AST_LangPrimitive can be PRINT_EXPR
        assert(node->opcode == AST_LangPrimitive::PRINT_EXPR);
        assert(node->args.size() == 1);
        auto remapped_value = remapExpr(node->args[0]);
        BST_PrintExpr* rtn = allocAndPush<BST_PrintExpr>();
        rtn->lineno = node->lineno;
        unmapExpr(remapped_value, &rtn->vreg_value);
        return TmpValue();
    }

    TmpValue remapList(AST_List* node) {
        assert(node->ctx_type == AST_TYPE::Load);
        llvm::SmallVector<TmpValue, 8> remapped_elts;
        for (int i = 0; i < node->elts.size(); ++i) {
            remapped_elts.emplace_back(remapExpr(node->elts[i]));
        }
        BST_List* rtn = allocAndPush<BST_List>(node->elts.size());
        rtn->lineno = node->lineno;
        for (int i = 0; i < node->elts.size(); ++i) {
            unmapExpr(remapped_elts[i], &rtn->elts[i]);
        }
        return createDstName(rtn);
    }

    TmpValue remapRepr(AST_Repr* node) {
        auto remapped_value = remapExpr(node->value);
        BST_Repr* rtn = allocAndPush<BST_Repr>();
        rtn->lineno = node->lineno;
        unmapExpr(remapped_value, &rtn->vreg_value);
        return createDstName(rtn);
    }

    TmpValue remapSet(AST_Set* node) {
        llvm::SmallVector<TmpValue, 8> remapped_elts;
        for (int i = 0; i < node->elts.size(); ++i) {
            remapped_elts.emplace_back(remapExpr(node->elts[i]));
        }

        BST_Set* rtn = allocAndPush<BST_Set>(node->elts.size());
        rtn->lineno = node->lineno;
        for (int i = 0; i < node->elts.size(); ++i) {
            unmapExpr(remapped_elts[i], &rtn->elts[i]);
        }
        return createDstName(rtn);
    }

    bool isSlice(AST_slice* node) { return node->type == AST_TYPE::Slice && ast_cast<AST_Slice>(node)->step == NULL; }

    TmpValue remapSlice(AST_Slice* node) {
        auto remapped_lower = remapExpr(node->lower);
        auto remapped_upper = remapExpr(node->upper);
        auto remapped_step = remapExpr(node->step);

        BST_MakeSlice* rtn = allocAndPush<BST_MakeSlice>();
        rtn->lineno = node->lineno;
        unmapExpr(remapped_lower, &rtn->vreg_lower);
        unmapExpr(remapped_upper, &rtn->vreg_upper);
        unmapExpr(remapped_step, &rtn->vreg_step);
        return createDstName(rtn);
    }

    TmpValue remapSlice(AST_slice* node) {
        TmpValue rtn;
        switch (node->type) {
            case AST_TYPE::Ellipsis:
                rtn = remapEllipsis(ast_cast<AST_Ellipsis>(node));
                break;
            case AST_TYPE::ExtSlice:
                rtn = remapExtSlice(ast_cast<AST_ExtSlice>(node));
                break;
            case AST_TYPE::Index:
                rtn = remapExpr(ast_cast<AST_Index>(node)->value);
                break;
            case AST_TYPE::Slice:
                rtn = remapSlice(ast_cast<AST_Slice>(node));
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->type);
        }
        return rtn;
    }

    bool isConstObject(AST* node) {
        if (node->type == AST_TYPE::Str || node->type == AST_TYPE::Num)
            return true;
        else if (node->type == AST_TYPE::Tuple) {
            auto* tuple = ast_cast<AST_Tuple>(node);
            assert(tuple->ctx_type == AST_TYPE::Load);
            for (auto&& elt : tuple->elts) {
                if (!isConstObject(elt))
                    return false;
            }
            return true;
        } else if (node->type == AST_TYPE::Name && ast_cast<AST_Name>(node)->id.s() == "None")
            return true;
        return false;
    }

    Box* createConstObject(AST* node) {
        if (node->type == AST_TYPE::Str) {
            auto* str = ast_cast<AST_Str>(node);
            if (str->str_type == AST_Str::STR) {
                BoxedString*& o = str_constants[str->str_data];
                // we always intern the string
                if (!o)
                    o = internStringMortal(str->str_data);
                else
                    incref(o);
                return o;
            } else if (str->str_type == AST_Str::UNICODE) {
                Box*& r = unicode_constants[str->str_data];
                if (!r)
                    r = decodeUTF8StringPtr(str->str_data);
                else
                    incref(r);
                return r;
            } else
                RELEASE_ASSERT(0, "not implemented");
        } else if (node->type == AST_TYPE::Num) {
            auto* num = ast_cast<AST_Num>(node);
            if (num->num_type == AST_Num::INT)
                return incref(code_constants.getIntConstant(num->n_int));
            else if (num->num_type == AST_Num::FLOAT)
                return incref(code_constants.getFloatConstant(num->n_float));
            else if (num->num_type == AST_Num::LONG) {
                Box*& r = long_constants[num->n_long];
                if (!r)
                    r = createLong(num->n_long);
                else
                    incref(r);
                return r;
            } else if (num->num_type == AST_Num::COMPLEX) {
                Box*& r = imaginary_constants[getDoubleBits(num->n_float)];
                if (!r)
                    r = createPureImaginary(num->n_float);
                else
                    incref(r);
                return r;
            } else
                RELEASE_ASSERT(0, "not implemented");
        } else if (node->type == AST_TYPE::Tuple) {
            auto* tuple_node = ast_cast<AST_Tuple>(node);
            BoxedTuple* tuple = BoxedTuple::create(tuple_node->elts.size());
            for (int i = 0; i < tuple_node->elts.size(); ++i) {
                tuple->elts[i] = createConstObject(tuple_node->elts[i]);
            }
            return tuple;
        } else if (node->type == AST_TYPE::Name && ast_cast<AST_Name>(node)->id.s() == "None") {
            return incref(Py_None);
        }
        RELEASE_ASSERT(0, "not implemented");
    }

    TmpValue remapTuple(AST_Tuple* node) {
        assert(node->ctx_type == AST_TYPE::Load);

        // handle tuples where all elements are constants as a constant
        if (isConstObject(node)) {
            Box* tuple = createConstObject(node);
            return TmpValue(addConst(tuple), node->lineno);
        }

        llvm::SmallVector<TmpValue, 8> remapped_elts;
        for (int i = 0; i < node->elts.size(); ++i) {
            remapped_elts.emplace_back(remapExpr(node->elts[i]));
        }

        BST_Tuple* rtn = allocAndPush<BST_Tuple>(node->elts.size());
        rtn->lineno = node->lineno;
        for (int i = 0; i < node->elts.size(); ++i) {
            unmapExpr(remapped_elts[i], &rtn->elts[i]);
        }
        return createDstName(rtn);
    }


    TmpValue remapSubscript(AST_Subscript* node) {
        assert(node->ctx_type == AST_TYPE::AST_TYPE::Load);
        auto remapped_value = remapExpr(node->value);
        if (!isSlice(node->slice)) {
            auto remapped_slice = remapSlice(node->slice);
            BST_LoadSub* rtn = allocAndPush<BST_LoadSub>();
            rtn->lineno = node->lineno;
            unmapExpr(remapped_value, &rtn->vreg_value);
            unmapExpr(remapped_slice, &rtn->vreg_slice);
            return createDstName(rtn);
        } else {
            auto remapped_lower = remapExpr(ast_cast<AST_Slice>(node->slice)->lower);
            auto remapped_upper = remapExpr(ast_cast<AST_Slice>(node->slice)->upper);

            BST_LoadSubSlice* rtn = allocAndPush<BST_LoadSubSlice>();
            rtn->lineno = node->lineno;
            assert(node->ctx_type == AST_TYPE::AST_TYPE::Load);
            unmapExpr(remapped_value, &rtn->vreg_value);
            unmapExpr(remapped_lower, &rtn->vreg_lower);
            unmapExpr(remapped_upper, &rtn->vreg_upper);
            return createDstName(rtn);
        }
    }

    TmpValue remapUnaryOp(AST_UnaryOp* node) {
        auto remapped_operand = remapExpr(node->operand);
        BST_UnaryOp* rtn = allocAndPush<BST_UnaryOp>();
        rtn->lineno = node->lineno;
        rtn->op_type = node->op_type;
        unmapExpr(remapped_operand, &rtn->vreg_operand);
        return createDstName(rtn);
    }

    TmpValue remapYield(AST_Yield* node) {
        auto remapped_value = remapExpr(node->value);
        BST_Yield* rtn = allocAndPush<BST_Yield>();
        rtn->lineno = node->lineno;
        unmapExpr(remapped_value, &rtn->vreg_value);

        TmpValue val = createDstName(rtn);

        allocAndPush<BST_UncacheExcInfo>();

        if (root_type != AST_TYPE::FunctionDef && root_type != AST_TYPE::Lambda)
            raiseExcHelper(SyntaxError, "'yield' outside function");

        return val;
    }

    // Flattens a nested expression into a flat one, emitting instructions &
    // generating temporary variables as needed.
    //
    // If `wrap_with_assign` is true, it will always return a temporary
    // variable.
    TmpValue remapExpr(AST_expr* node) {
        if (node == NULL)
            return TmpValue();

        TmpValue rtn;
        switch (node->type) {
            case AST_TYPE::Attribute:
                rtn = remapAttribute(ast_cast<AST_Attribute>(node));
                break;
            case AST_TYPE::BinOp:
                rtn = remapBinOp(ast_cast<AST_BinOp>(node));
                break;
            case AST_TYPE::BoolOp:
                rtn = remapBoolOp(ast_cast<AST_BoolOp>(node));
                break;
            case AST_TYPE::Call:
                rtn = remapCall(ast_cast<AST_Call>(node));
                break;
            case AST_TYPE::ClsAttribute:
                rtn = remapClsAttribute(ast_cast<AST_ClsAttribute>(node));
                break;
            case AST_TYPE::Compare:
                rtn = remapCompare(ast_cast<AST_Compare>(node));
                break;
            case AST_TYPE::Dict:
                rtn = remapDict(ast_cast<AST_Dict>(node));
                break;
            case AST_TYPE::DictComp:
                rtn = remapScopedComprehension<AST_Dict>(ast_cast<AST_DictComp>(node));
                break;
            case AST_TYPE::GeneratorExp:
                rtn = remapGeneratorExp(ast_cast<AST_GeneratorExp>(node));
                break;
            case AST_TYPE::IfExp:
                rtn = remapIfExp(ast_cast<AST_IfExp>(node));
                break;
            case AST_TYPE::Lambda:
                rtn = remapLambda(ast_cast<AST_Lambda>(node));
                break;
            case AST_TYPE::LangPrimitive:
                rtn = remapLangPrimitive(ast_cast<AST_LangPrimitive>(node));
                break;
            case AST_TYPE::List:
                rtn = remapList(ast_cast<AST_List>(node));
                break;
            case AST_TYPE::ListComp:
                rtn = remapComprehension(ast_cast<AST_ListComp>(node));
                break;
            case AST_TYPE::Name:
                rtn = remapName(ast_cast<AST_Name>(node));
                break;
            case AST_TYPE::Num:
                rtn = remapNum(ast_cast<AST_Num>(node));
                break;
            case AST_TYPE::Repr:
                rtn = remapRepr(ast_cast<AST_Repr>(node));
                break;
            case AST_TYPE::Set:
                rtn = remapSet(ast_cast<AST_Set>(node));
                break;
            case AST_TYPE::SetComp:
                rtn = remapScopedComprehension<AST_Set>(ast_cast<AST_SetComp>(node));
                break;
            case AST_TYPE::Str:
                rtn = remapStr(ast_cast<AST_Str>(node));
                break;
            case AST_TYPE::Subscript:
                rtn = remapSubscript(ast_cast<AST_Subscript>(node));
                break;
            case AST_TYPE::Tuple:
                rtn = remapTuple(ast_cast<AST_Tuple>(node));
                break;
            case AST_TYPE::UnaryOp:
                rtn = remapUnaryOp(ast_cast<AST_UnaryOp>(node));
                break;
            case AST_TYPE::Yield:
                rtn = remapYield(ast_cast<AST_Yield>(node));
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->type);
        }

        return rtn;
    }

    // helper for visit_{tryfinally,with}
    CFGBlock* makeFinallyCont(Why reason, TmpValue whyexpr, CFGBlock* then_block) {
        CFGBlock* otherwise = cfg->addDeferredBlock();
        otherwise->info = "finally_otherwise";
        pushBranch(makeCompare(AST_TYPE::Eq, whyexpr, makeNum(reason, whyexpr.lineno)), then_block, otherwise);
        return otherwise;
    }

    // Helper for visit_with. Performs the appropriate exit from a with-block, according to the value of `why'.
    // NB. `exit_block' is only used if `why' is FALLTHROUGH.
    void exitFinally(AST* node, Why why, CFGBlock* exit_block = nullptr) {
        switch (why) {
            case Why::RETURN:
                doReturn(TmpValue(internString(RETURN_NAME), node->lineno));
                break;
            case Why::BREAK:
                doBreak(node);
                break;
            case Why::CONTINUE:
                doContinue(node);
                break;
            case Why::FALLTHROUGH:
                assert(exit_block);
                pushJump(exit_block);
                break;
            case Why::EXCEPTION:
                assert(why != Why::EXCEPTION); // not handled here
                break;
        }
        assert(curblock == nullptr);
    }

    // helper for visit_{with,tryfinally}. Generates a branch testing the value of `whyexpr' against `why', and
    // performing the appropriate exit from the with-block if they are equal.
    // NB. `exit_block' is only used if `why' is FALLTHROUGH.
    void exitFinallyIf(AST* node, Why why, TmpValue whyname, bool is_kill = false) {
        CFGBlock* do_exit = cfg->addDeferredBlock();
        do_exit->info = "with_exit_if";
        CFGBlock* otherwise = makeFinallyCont(why, is_kill ? whyname : _dup(whyname), do_exit);

        setInsertPoint(do_exit);
        exitFinally(node, why);

        setInsertPoint(otherwise);
    }

    // ---------- public methods ----------
public:
    void pushStoreName(InternedString name, TmpValue value) {
        BST_StoreName* store;
        // we assume currently that stores to non temp names can't throw
        if (name.s()[0] != '#') // this can't throw
            store = allocAndPush<BST_StoreName, true /* can't throw */>();
        else
            store = allocAndPush<BST_StoreName>();
        store->index_id = remapInternedString(name);
        unmapExpr(value, &store->vreg_value);
        store->lineno = value.lineno;
        fillScopingInfo(store, name, scoping);
    }

    bool visit_classdef(AST_ClassDef* node) override {
        // Decorators are evaluated before bases:
        llvm::SmallVector<TmpValue, 8> remapped_deco;
        for (int i = 0; i < node->decorator_list.size(); ++i) {
            remapped_deco.emplace_back(remapExpr(node->decorator_list[i]));
        }

        llvm::SmallVector<TmpValue, 8> remapped_bases;
        for (int i = 0; i < node->bases.size(); ++i) {
            remapped_bases.emplace_back(remapExpr(node->bases[i]));
        }

        auto* bases = allocAndPush<BST_Tuple>(node->bases.size());
        for (int i = 0; i < node->bases.size(); ++i) {
            unmapExpr(remapped_bases[i], &bases->elts[i]);
        }
        TmpValue bases_name = createDstName(bases);

        auto mkclass = allocAndPush<BST_MakeClass>(node->decorator_list.size());
        mkclass->lineno = node->lineno;
        mkclass->index_name = remapInternedString(node->name);

        for (int i = 0; i < node->decorator_list.size(); ++i) {
            unmapExpr(remapped_deco[i], &mkclass->decorator[i]);
        }

        unmapExpr(bases_name, &mkclass->vreg_bases_tuple);

        auto* code = cfgizer->runRecursively(node->body, node->name.getBox(), node->lineno, NULL, node);
        mkclass->vreg_code_obj = addConst(code);

        auto tmp = createDstName(mkclass);
        pushAssign(TmpValue(scoping->mangleName(node->name), node->lineno), tmp);

        return true;
    }

    bool visit_functiondef(AST_FunctionDef* node) override {
        // Decorators are evaluated before the defaults, so this *must* go before remapArguments().
        // TODO(rntz): do we have a test for this
        llvm::SmallVector<TmpValue, 8> remapped_deco;
        for (int i = 0; i < node->decorator_list.size(); ++i) {
            remapped_deco.emplace_back(remapExpr(node->decorator_list[i]));
        }
        llvm::SmallVector<TmpValue, 8> remapped_defaults;
        for (int i = 0; i < node->args->defaults.size(); ++i) {
            remapped_defaults.emplace_back(remapExpr(node->args->defaults[i]));
        }

        auto mkfunc = allocAndPush<BST_MakeFunction>(node->decorator_list.size(), node->args->defaults.size());
        mkfunc->lineno = node->lineno;
        mkfunc->index_name = remapInternedString(node->name);

        // Decorators are evaluated before the defaults, so this *must* go before remapArguments().
        // TODO(rntz): do we have a test for this
        for (int i = 0; i < node->decorator_list.size(); ++i) {
            unmapExpr(remapped_deco[i], &mkfunc->elts[i]);
        }
        for (int i = 0; i < node->args->defaults.size(); ++i) {
            unmapExpr(remapped_defaults[i], &mkfunc->elts[node->decorator_list.size() + i]);
        }

        auto* code = cfgizer->runRecursively(node->body, node->name.getBox(), node->lineno, node->args, node);
        mkfunc->vreg_code_obj = addConst(code);
        auto tmp = createDstName(mkfunc);
        pushAssign(TmpValue(scoping->mangleName(node->name), node->lineno), tmp);

        return true;
    }

    bool visit_global(AST_Global*) override {
        // nothing todo only the scoping analysis cares about this node
        return true;
    }

    static llvm::StringRef getTopModule(llvm::StringRef full_name) {
        size_t period_index = full_name.find('.');
        if (period_index == std::string::npos) {
            return full_name;
        } else {
            return full_name.substr(0, period_index);
        }
    }

    bool visit_import(AST_Import* node) override {
        for (AST_alias* a : node->names) {
            BST_ImportName* import = allocAndPush<BST_ImportName>();
            import->lineno = node->lineno;

            // level == 0 means only check sys path for imports, nothing package-relative,
            // level == -1 means check both sys path and relative for imports.
            // so if `from __future__ import absolute_import` was used in the file, set level to 0
            int level;
            if (!(future_flags & CO_FUTURE_ABSOLUTE_IMPORT))
                level = -1;
            else
                level = 0;
            import->level = level;

            unmapExpr(makeNone(node->lineno), &import->vreg_from);
            import->index_id = remapInternedString(a->name);

            TmpValue tmpname = createDstName(import);

            if (a->asname.s().size() == 0) {
                // No asname, so load the top-level module into the name
                // (e.g., for `import os.path`, loads the os module into `os`)
                pushAssign(TmpValue(internString(getTopModule(a->name.s())), node->lineno), tmpname);
            } else {
                // If there is an asname, get the bottom-level module by
                // getting the attributes and load it into asname.
                int l = 0;
                do {
                    int r = a->name.s().find('.', l);
                    if (r == std::string::npos) {
                        r = a->name.s().size();
                    }
                    if (l == 0) {
                        l = r + 1;
                        continue;
                    }

                    auto* store = allocAndPush<BST_LoadAttr>();
                    store->lineno = import->lineno;
                    store->index_attr
                        = remapInternedString(scoping->mangleName(internString(a->name.s().substr(l, r - l))));
                    unmapExpr(tmpname, &store->vreg_value);
                    unmapExpr(tmpname, &store->vreg_dst);

                    l = r + 1;
                } while (l < a->name.s().size());
                pushAssign(a->asname, tmpname);
            }
        }

        return true;
    }

    bool visit_importfrom(AST_ImportFrom* node) override {
        BST_ImportName* import = allocAndPush<BST_ImportName>();
        import->lineno = node->lineno;

        // level == 0 means only check sys path for imports, nothing package-relative,
        // level == -1 means check both sys path and relative for imports.
        // so if `from __future__ import absolute_import` was used in the file, set level to 0
        int level;
        if (node->level == 0 && !(future_flags & CO_FUTURE_ABSOLUTE_IMPORT))
            level = -1;
        else
            level = node->level;
        import->level = level;

        auto* tuple = BoxedTuple::create(node->names.size());
        for (int i = 0; i < node->names.size(); i++) {
            tuple->elts[i] = internStringMortal(node->names[i]->name.s());
        }
        import->vreg_from = addConst(tuple);
        import->index_id = remapInternedString(internString(node->module.s()));

        TmpValue tmp_module_name = createDstName(import);

        int i = 0;
        for (AST_alias* a : node->names) {
            i++;
            bool is_kill = (i == node->names.size());
            TmpValue remapped_tmp_module_name = is_kill ? tmp_module_name : _dup(tmp_module_name);
            if (a->name.s() == "*") {
                BST_ImportStar* import_star = allocAndPush<BST_ImportStar>();
                import_star->lineno = node->lineno;
                unmapExpr(remapped_tmp_module_name, &import_star->vreg_name);

                createDstName(import_star);
            } else {
                BST_ImportFrom* import_from = allocAndPush<BST_ImportFrom>();
                import_from->lineno = node->lineno;
                unmapExpr(remapped_tmp_module_name, &import_from->vreg_module);
                import_from->index_id = remapInternedString(a->name);

                TmpValue tmp_import_name = createDstName(import_from);
                pushAssign(a->asname.s().size() ? a->asname : a->name, tmp_import_name);
            }
        }

        return true;
    }

    bool visit_pass(AST_Pass* node) override { return true; }

    bool visit_assert(AST_Assert* node) override {
        assert(curblock);

        auto remapped_br_test = callNonzero(remapExpr(node->test));
        BST_Branch* br = allocAndPush<BST_Branch>();
        unmapExpr(remapped_br_test, &br->vreg_test);

        CFGBlock* iffalse = cfg->addDeferredBlock();
        iffalse->info = "assert_fail";
        curblock->connectTo(iffalse);
        CFGBlock* iftrue = cfg->addDeferredBlock();
        iftrue->info = "assert_pass";
        curblock->connectTo(iftrue);
        br->iftrue = iftrue;
        br->iffalse = iffalse;

        setInsertPoint(iffalse);

        TmpValue remapped_msg;
        if (node->msg)
            remapped_msg = remapExpr(node->msg);

        BST_Assert* remapped = allocAndPush<BST_Assert>();
        if (node->msg)
            unmapExpr(remapped_msg, &remapped->vreg_msg);
        else
            remapped->vreg_msg = VREG_UNDEFINED;
        remapped->lineno = node->lineno;

        setInsertPoint(iftrue);

        return true;
    }

    bool visit_assign(AST_Assign* node) override {
        TmpValue remapped_value = remapExpr(node->value);

        for (int i = 0; i < node->targets.size(); i++) {
            TmpValue val;
            if (i == node->targets.size() - 1)
                val = remapped_value;
            else
                val = _dup(remapped_value);
            pushAssign(node->targets[i], val);
        }
        return true;
    }

    bool visit_augassign(AST_AugAssign* node) override {
        // augassign is pretty tricky; "x" += "y" mostly textually maps to
        // "x" = "x" =+ "y" (using "=+" to represent an augbinop)
        // except that "x" only gets evaluated once.  So it's something like
        // "target", val = eval("x")
        // "target" = val =+ "y"
        // where "target" is handled specially, because it can't just be a name;
        // it has to be a name-only version of the target type (ex subscript, attribute).
        // So for "f().x += g()", it has to translate to
        // "c = f(); y = c.x; z = g(); c.x = y =+ z"
        //
        // Even if the target is a simple name, it can be complicated, because the
        // value can change the name.  For "x += f()", have to translate to
        // "y = x; z = f(); x = y =+ z"
        //
        // Finally, due to possibility of exceptions, we don't want to assign directly
        // to the final target at the same time as evaluating the augbinop



        // TODO bad that it's reusing the AST nodes?
        switch (node->target->type) {
            case AST_TYPE::Name: {
                AST_Name* n = ast_cast<AST_Name>(node->target);
                assert(n->ctx_type == AST_TYPE::Store);

                auto remapped_name = remapName(n);
                auto remapped_value = remapExpr(node->value);

                BST_AugBinOp* binop = allocAndPush<BST_AugBinOp>();
                binop->op_type = remapBinOpType(node->op_type);
                unmapExpr(remapped_name, &binop->vreg_left);
                unmapExpr(remapped_value, &binop->vreg_right);
                binop->lineno = node->lineno;
                TmpValue result_name = createDstName(binop);
                pushStoreName(n->id, result_name);

                return true;
            }
            case AST_TYPE::Subscript: {
                AST_Subscript* s = ast_cast<AST_Subscript>(node->target);
                assert(s->ctx_type == AST_TYPE::Store);

                TmpValue value_remapped = remapExpr(s->value);
                auto value_remapped_dup = _dup(value_remapped);

                if (isSlice(s->slice)) {
                    auto* slice = ast_cast<AST_Slice>(s->slice);

                    auto lower_remapped = remapExpr(slice->lower);
                    auto upper_remapped = remapExpr(slice->upper);

                    auto lower_remapped_dup = _dup(lower_remapped);
                    auto upper_remapped_dup = _dup(upper_remapped);

                    BST_LoadSubSlice* s_lhs = allocAndPush<BST_LoadSubSlice>();
                    unmapExpr(value_remapped_dup, &s_lhs->vreg_value);
                    unmapExpr(lower_remapped_dup, &s_lhs->vreg_lower);
                    unmapExpr(upper_remapped_dup, &s_lhs->vreg_upper);
                    s_lhs->lineno = s->lineno;
                    TmpValue name_lhs = createDstName(s_lhs);

                    auto remapped_value = remapExpr(node->value);
                    BST_AugBinOp* binop = allocAndPush<BST_AugBinOp>();
                    binop->op_type = remapBinOpType(node->op_type);
                    unmapExpr(name_lhs, &binop->vreg_left);
                    unmapExpr(remapped_value, &binop->vreg_right);
                    binop->lineno = node->lineno;
                    TmpValue node_name = createDstName(binop);

                    BST_StoreSubSlice* s_target = allocAndPush<BST_StoreSubSlice>();
                    s_target->lineno = s->lineno;
                    unmapExpr(node_name, &s_target->vreg_value);
                    unmapExpr(value_remapped, &s_target->vreg_target);
                    unmapExpr(lower_remapped, &s_target->vreg_lower);
                    unmapExpr(upper_remapped, &s_target->vreg_upper);
                } else {

                    auto slice_remapped = remapSlice(s->slice);
                    auto slice_remapped_dup = _dup(slice_remapped);

                    BST_LoadSub* s_lhs = allocAndPush<BST_LoadSub>();
                    unmapExpr(value_remapped_dup, &s_lhs->vreg_value);
                    unmapExpr(slice_remapped_dup, &s_lhs->vreg_slice);
                    s_lhs->lineno = s->lineno;
                    TmpValue name_lhs = createDstName(s_lhs);

                    auto remapped_value = remapExpr(node->value);
                    BST_AugBinOp* binop = allocAndPush<BST_AugBinOp>();
                    binop->op_type = remapBinOpType(node->op_type);
                    unmapExpr(name_lhs, &binop->vreg_left);
                    unmapExpr(remapped_value, &binop->vreg_right);
                    binop->lineno = node->lineno;
                    TmpValue node_name = createDstName(binop);

                    BST_StoreSub* s_target = allocAndPush<BST_StoreSub>();
                    s_target->lineno = s->lineno;
                    unmapExpr(node_name, &s_target->vreg_value);
                    unmapExpr(value_remapped, &s_target->vreg_target);
                    unmapExpr(slice_remapped, &s_target->vreg_slice);
                }

                return true;
            }
            case AST_TYPE::Attribute: {
                AST_Attribute* a = ast_cast<AST_Attribute>(node->target);
                assert(a->ctx_type == AST_TYPE::Store);
                auto value_remapped = remapExpr(a->value);
                auto value_remapped_dup = _dup(value_remapped);

                BST_LoadAttr* a_lhs = allocAndPush<BST_LoadAttr>();
                unmapExpr(value_remapped_dup, &a_lhs->vreg_value);
                auto index_attr = remapInternedString(scoping->mangleName(a->attr));
                a_lhs->index_attr = index_attr;
                a_lhs->lineno = a->lineno;
                TmpValue name_lhs = createDstName(a_lhs);

                auto remapped_value = remapExpr(node->value);
                BST_AugBinOp* binop = allocAndPush<BST_AugBinOp>();
                binop->op_type = remapBinOpType(node->op_type);
                unmapExpr(name_lhs, &binop->vreg_left);
                unmapExpr(remapped_value, &binop->vreg_right);
                binop->lineno = node->lineno;
                TmpValue node_name = createDstName(binop);

                BST_StoreAttr* a_target = allocAndPush<BST_StoreAttr>();
                unmapExpr(node_name, &a_target->vreg_value);
                unmapExpr(value_remapped, &a_target->vreg_target);
                a_target->index_attr = index_attr;
                a_target->lineno = a->lineno;

                return true;
            }
            default:
                RELEASE_ASSERT(0, "%d", node->target->type);
        }
        return true;
    }

    AST_TYPE::AST_TYPE remapBinOpType(AST_TYPE::AST_TYPE op_type) {
        if (op_type == AST_TYPE::Div && (future_flags & (CO_FUTURE_DIVISION))) {
            return AST_TYPE::TrueDiv;
        } else {
            return op_type;
        }
    }

    bool visit_delete(AST_Delete* node) override {
        for (auto t : node->targets) {
            switch (t->type) {
                case AST_TYPE::Subscript: {
                    AST_Subscript* s = static_cast<AST_Subscript*>(t);
                    auto remapped_value = remapExpr(s->value);
                    if (isSlice(s->slice)) {
                        auto* slice = ast_cast<AST_Slice>(s->slice);
                        auto remapped_lower = remapExpr(slice->lower);
                        auto remapped_upper = remapExpr(slice->upper);
                        auto* del = allocAndPush<BST_DeleteSubSlice>();
                        del->lineno = node->lineno;
                        unmapExpr(remapped_value, &del->vreg_value);
                        unmapExpr(remapped_lower, &del->vreg_lower);
                        unmapExpr(remapped_upper, &del->vreg_upper);
                    } else {
                        auto remapped_slice = remapSlice(s->slice);
                        auto* del = allocAndPush<BST_DeleteSub>();
                        del->lineno = node->lineno;
                        unmapExpr(remapped_value, &del->vreg_value);
                        unmapExpr(remapped_slice, &del->vreg_slice);
                    }
                    break;
                }
                case AST_TYPE::Attribute: {
                    AST_Attribute* astattr = static_cast<AST_Attribute*>(t);
                    auto remaped_value = remapExpr(astattr->value);
                    auto* del = allocAndPush<BST_DeleteAttr>();
                    del->lineno = node->lineno;
                    unmapExpr(remaped_value, &del->vreg_value);
                    del->index_attr = remapInternedString(scoping->mangleName(astattr->attr));
                    break;
                }
                case AST_TYPE::Name: {
                    AST_Name* s = static_cast<AST_Name*>(t);
                    auto* del = allocAndPush<BST_DeleteName>();
                    del->lineno = node->lineno;
                    del->index_id = remapInternedString(s->id);
                    fillScopingInfo(del, s->id, scoping);
                    break;
                }
                case AST_TYPE::List: {
                    AST_List* list = static_cast<AST_List*>(t);
                    ASTAllocator allocator;
                    AST_Delete* temp_ast_del = new (allocator) AST_Delete();
                    temp_ast_del->lineno = node->lineno;
                    temp_ast_del->col_offset = node->col_offset;

                    for (auto elt : list->elts) {
                        temp_ast_del->targets.push_back(elt);
                    }
                    visit_delete(temp_ast_del);
                    break;
                }
                case AST_TYPE::Tuple: {
                    AST_Tuple* tuple = static_cast<AST_Tuple*>(t);
                    ASTAllocator allocator;
                    AST_Delete* temp_ast_del = new (allocator) AST_Delete();
                    temp_ast_del->lineno = node->lineno;
                    temp_ast_del->col_offset = node->col_offset;

                    for (auto elt : tuple->elts) {
                        temp_ast_del->targets.push_back(elt);
                    }
                    visit_delete(temp_ast_del);
                    break;
                }
                default:
                    RELEASE_ASSERT(0, "Unsupported del target: %d", t->type);
            }
        }

        return true;
    }


    bool visit_expr(AST_Expr* node) override {
        remapExpr(node->value);
        return true;
    }

    bool visit_print(AST_Print* node) override {
        TmpValue dest = remapExpr(node->dest);

        int i = 0;
        for (auto v : node->values) {
            TmpValue remapped_dest = dest;
            if (i < node->values.size() - 1)
                remapped_dest = _dup(dest);

            auto remapped_value = remapExpr(v);

            BST_Print* remapped = allocAndPush<BST_Print>();
            remapped->lineno = node->lineno;

            unmapExpr(remapped_dest, &remapped->vreg_dest);
            if (i < node->values.size() - 1)
                remapped->nl = false;
            else
                remapped->nl = node->nl;
            unmapExpr(remapped_value, &remapped->vreg_value);

            i++;
        }

        if (node->values.size() == 0) {
            assert(node->nl);

            BST_Print* final = allocAndPush<BST_Print>();
            final->lineno = node->lineno;
            // TODO not good to reuse 'dest' like this
            unmapExpr(dest, &final->vreg_dest);
            final->nl = node->nl;
        }

        return true;
    }

    bool visit_return(AST_Return* node) override {
        // returns are allowed in functions (of course), and also in eval("...") strings - basically, eval strings get
        // an implicit `return'. root_type is AST_TYPE::Expression when we're compiling an eval string.
        assert(curblock);

        if (root_type != AST_TYPE::FunctionDef && root_type != AST_TYPE::Lambda && root_type != AST_TYPE::Expression) {
            raiseExcHelper(SyntaxError, "'return' outside function");
        }

        if (!curblock)
            return true;

        doReturn(node->value ? remapExpr(node->value) : makeNone(node->lineno));
        return true;
    }

    bool visit_if(AST_If* node) override {
        assert(curblock);

        auto remapped_br_test = callNonzero(remapExpr(node->test));
        BST_Branch* br = allocAndPush<BST_Branch>();
        br->lineno = node->lineno;
        unmapExpr(remapped_br_test, &br->vreg_test);

        CFGBlock* starting_block = curblock;
        CFGBlock* exit = cfg->addDeferredBlock();
        exit->info = "ifexit";

        CFGBlock* iffalse = cfg->addDeferredBlock();
        iffalse->info = "iffalse";
        br->iffalse = (CFGBlock*)iffalse;
        starting_block->connectTo(iffalse);

        CFGBlock* iftrue = cfg->addDeferredBlock();
        iftrue->info = "iftrue";
        br->iftrue = (CFGBlock*)iftrue;
        starting_block->connectTo(iftrue);

        setInsertPoint(iftrue);
        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock) {
            pushJump(exit);
        }

        setInsertPoint(iffalse);
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock) {
            pushJump(exit);
        }

        if (exit->predecessors.size() == 0) {
            curblock = NULL;
        } else {
            setInsertPoint(exit);
        }

        return true;
    }

    bool visit_break(AST_Break* node) override {
        assert(curblock);

        doBreak(node);
        assert(!curblock);
        return true;
    }

    bool visit_continue(AST_Continue* node) override {
        assert(curblock);

        doContinue(node);
        assert(!curblock);
        return true;
    }

    bool visit_exec(AST_Exec* node) override {
        auto remapped_body = remapExpr(node->body);
        auto remapped_globals = remapExpr(node->globals);
        auto remapped_locals = remapExpr(node->locals);

        BST_Exec* astexec = allocAndPush<BST_Exec>();
        astexec->lineno = node->lineno;
        unmapExpr(remapped_body, &astexec->vreg_body);
        unmapExpr(remapped_globals, &astexec->vreg_globals);
        unmapExpr(remapped_locals, &astexec->vreg_locals);
        return true;
    }

    bool visit_while(AST_While* node) override {
        assert(curblock);

        CFGBlock* test_block = cfg->addDeferredBlock();
        test_block->info = "while_test";
        pushJump(test_block);

        setInsertPoint(test_block);
        BST_Branch* br = makeBranch(remapExpr(node->test));
        CFGBlock* test_block_end = curblock;

        // We need a reference to this block early on so we can break to it,
        // but we don't want it to be placed until after the orelse.
        CFGBlock* end = cfg->addDeferredBlock();
        end->info = "while_exit";

        CFGBlock* body = cfg->addDeferredBlock();
        body->info = "while_body_start";
        br->iftrue = (CFGBlock*)body;
        test_block_end->connectTo(body);

        CFGBlock* orelse = cfg->addDeferredBlock();
        orelse->info = "while_orelse_start";
        br->iffalse = (CFGBlock*)orelse;
        test_block_end->connectTo(orelse);

        pushLoopContinuation(test_block, end);

        setInsertPoint(body);
        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock)
            pushJump(test_block, true);
        popContinuation();

        setInsertPoint(orelse);
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock)
            pushJump(end);

        if (end->predecessors.size() == 0) {
            delete end;
            curblock = NULL;
        } else {
            setInsertPoint(end);
        }

        return true;
    }

    BST_stmt* makeKill(InternedString name) {
        // There might be a better way to represent this, maybe with a dedicated AST_Kill bytecode?
        auto del = allocAndPush<BST_DeleteName, false /* can't throw */>();
        del->index_id = remapInternedString(name);
        del->lineno = 0;
        fillScopingInfo(del, name, scoping);
        return del;
    }

    bool visit_for(AST_For* node) override {
        assert(curblock);

        // TODO this is so complicated because I tried doing loop inversion;
        // is it really worth it?  It got so bad because all the edges became
        // critical edges and needed to be broken, otherwise it's not too different.

        TmpValue remapped_iter = remapExpr(node->iter);
        BST_GetIter* iter_call = allocAndPush<BST_GetIter>();
        unmapExpr(remapped_iter, &iter_call->vreg_value);
        iter_call->lineno = node->lineno;
        TmpValue itername(createUniqueName("#iter_"), node->lineno);
        unmapExpr(itername, &iter_call->vreg_dst);

        CFGBlock* test_block = cfg->addDeferredBlock();
        pushJump(test_block);
        setInsertPoint(test_block);

        auto itername_dup = _dup(itername);
        BST_HasNext* test_call = allocAndPush<BST_HasNext>();
        test_call->lineno = node->lineno;
        unmapExpr(itername_dup, &test_call->vreg_value);
        TmpValue tmp_has_call = createDstName(test_call);

        BST_Branch* test_br = makeBranch(tmp_has_call);

        CFGBlock* test_true = cfg->addDeferredBlock();
        CFGBlock* test_false = cfg->addDeferredBlock();
        test_br->iftrue = (CFGBlock*)test_true;
        test_br->iffalse = (CFGBlock*)test_false;
        curblock->connectTo(test_true);
        curblock->connectTo(test_false);

        CFGBlock* loop_block = cfg->addDeferredBlock();
        CFGBlock* break_block = cfg->addDeferredBlock();
        CFGBlock* end_block = cfg->addDeferredBlock();
        CFGBlock* else_block = cfg->addDeferredBlock();

        setInsertPoint(test_true);
        // TODO simplify the breaking of these crit edges?
        pushJump(loop_block);

        setInsertPoint(test_false);
        pushJump(else_block);

        pushLoopContinuation(test_block, break_block);

        setInsertPoint(loop_block);
        TmpValue next_name = makeCallAttr(_dup(itername), internString("next"), true);
        pushAssign(node->target, next_name);

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }
        popContinuation();

        if (curblock) {
            remapped_iter = _dup(itername);
            BST_HasNext* end_call = allocAndPush<BST_HasNext>();
            unmapExpr(remapped_iter, &end_call->vreg_value);
            end_call->lineno = node->lineno;
            TmpValue tmp_end_call = createDstName(end_call);

            BST_Branch* end_br = makeBranch(tmp_end_call);

            CFGBlock* end_true = cfg->addDeferredBlock();
            CFGBlock* end_false = cfg->addDeferredBlock();
            end_br->iftrue = (CFGBlock*)end_true;
            end_br->iffalse = (CFGBlock*)end_false;
            curblock->connectTo(end_true);
            curblock->connectTo(end_false);

            setInsertPoint(end_true);
            pushJump(loop_block, true, getLastLinenoSub(node->body.back()));

            setInsertPoint(end_false);
            pushJump(else_block);
        }

        setInsertPoint(else_block);

        makeKill(itername.is);
        for (int i = 0; i < node->orelse.size(); i++) {
            node->orelse[i]->accept(this);
            if (!curblock)
                break;
        }
        if (curblock)
            pushJump(end_block);

        if (break_block->predecessors.size() == 0) {
            delete break_block;
        } else {
            setInsertPoint(break_block);
            makeKill(itername.is);
            pushJump(end_block);
        }

        if (end_block->predecessors.size() == 0) {
            delete end_block;
            curblock = NULL;
        } else {
            setInsertPoint(end_block);
        }

        return true;
    }

    bool visit_raise(AST_Raise* node) override {
        assert(curblock);

        TmpValue remapped_arg0, remapped_arg1, remapped_arg2;
        if (node->arg0)
            remapped_arg0 = remapExpr(node->arg0);
        if (node->arg1)
            remapped_arg1 = remapExpr(node->arg1);
        if (node->arg2)
            remapped_arg2 = remapExpr(node->arg2);

        BST_Raise* remapped = allocAndPush<BST_Raise>();
        remapped->lineno = node->lineno;
        if (node->arg0)
            unmapExpr(remapped_arg0, &remapped->vreg_arg0);
        if (node->arg1)
            unmapExpr(remapped_arg1, &remapped->vreg_arg1);
        if (node->arg2)
            unmapExpr(remapped_arg2, &remapped->vreg_arg2);

        if (!curblock)
            return true;

        curblock = NULL;

        return true;
    }

    bool visit_tryexcept(AST_TryExcept* node) override {
        assert(curblock);
        assert(node->handlers.size() > 0);

        CFGBlock* exc_handler_block = cfg->addDeferredBlock();
        TmpValue exc_type_name(nodeName("type"), node->lineno);
        TmpValue exc_value_name(nodeName("value"), node->lineno);
        TmpValue exc_traceback_name(nodeName("traceback"), node->lineno);
        exc_handlers.push_back(
            { exc_handler_block, exc_type_name.is, exc_value_name.is, exc_traceback_name.is, false });

        for (AST_stmt* subnode : node->body) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        exc_handlers.pop_back();

        if (curblock) {
            for (AST_stmt* subnode : node->orelse) {
                subnode->accept(this);
                if (!curblock)
                    break;
            }
        }

        CFGBlock* join_block = cfg->addDeferredBlock();
        if (curblock)
            pushJump(join_block);

        if (exc_handler_block->predecessors.size() == 0) {
            delete exc_handler_block;
        } else {
            setInsertPoint(exc_handler_block);

            bool caught_all = false;
            for (AST_ExceptHandler* exc_handler : node->handlers) {
                assert(!caught_all && "bare except clause not the last one in the list?");

                CFGBlock* exc_next = nullptr;
                if (exc_handler->type) {
                    TmpValue handled_type = remapExpr(exc_handler->type);

                    auto remapped_exc_value_name = _dup(exc_value_name);

                    BST_CheckExcMatch* is_caught_here = allocAndPush<BST_CheckExcMatch>();
                    // TODO This is supposed to be exc_type_name (value doesn't matter for checking matches)
                    unmapExpr(remapped_exc_value_name, &is_caught_here->vreg_value);
                    unmapExpr(handled_type, &is_caught_here->vreg_cls);
                    is_caught_here->lineno = exc_handler->lineno;
                    TmpValue name_is_caught_here = createDstName(is_caught_here);

                    auto remapped_br_test = callNonzero(name_is_caught_here);
                    BST_Branch* br = allocAndPush<BST_Branch>();
                    unmapExpr(remapped_br_test, &br->vreg_test);
                    br->lineno = exc_handler->lineno;

                    CFGBlock* exc_handle = cfg->addDeferredBlock();
                    exc_next = cfg->addDeferredBlock();

                    br->iftrue = (CFGBlock*)exc_handle;
                    br->iffalse = (CFGBlock*)exc_next;
                    curblock->connectTo(exc_handle);
                    curblock->connectTo(exc_next);
                    setInsertPoint(exc_handle);
                } else {
                    caught_all = true;
                }

                if (exc_handler->name) {
                    pushAssign(exc_handler->name, _dup(exc_value_name));
                }

                BST_SetExcInfo* set_exc_info = allocAndPush<BST_SetExcInfo>();
                unmapExpr(exc_type_name, &set_exc_info->vreg_type);
                unmapExpr(exc_value_name, &set_exc_info->vreg_value);
                unmapExpr(exc_traceback_name, &set_exc_info->vreg_traceback);

                for (AST_stmt* subnode : exc_handler->body) {
                    subnode->accept(this);
                    if (!curblock)
                        break;
                }

                if (curblock) {
                    pushJump(join_block);
                }

                if (!exc_next) {
                    assert(caught_all);
                }
                setInsertPoint(exc_next);
            }

            if (!caught_all) {
                BST_Raise* raise = allocAndPush<BST_Raise>();
                unmapExpr(exc_type_name, &raise->vreg_arg0);
                unmapExpr(exc_value_name, &raise->vreg_arg1);
                unmapExpr(exc_traceback_name, &raise->vreg_arg2);

                // This is weird but I think it is right.
                // Even though the line number of the trackback will correctly point to the line that
                // raised, this matches CPython's behavior that the frame's line number points to
                // the last statement of the last except block.
                raise->lineno = getLastLinenoSub(node->handlers.back()->body.back());

                curblock = NULL;
            }
        }

        if (join_block->predecessors.size() == 0) {
            delete join_block;
            curblock = NULL;
        } else {
            setInsertPoint(join_block);
        }

        return true;
    }

    bool visit_tryfinally(AST_TryFinally* node) override {
        assert(curblock);

        CFGBlock* exc_handler_block = cfg->addDeferredBlock();
        InternedString exc_type_name = nodeName("type");
        InternedString exc_value_name = nodeName("value");
        InternedString exc_traceback_name = nodeName("traceback");
        TmpValue exc_why_name(nodeName("why"), node->lineno);
        exc_handlers.push_back({ exc_handler_block, exc_type_name, exc_value_name, exc_traceback_name, false });

        CFGBlock* finally_block = cfg->addDeferredBlock();
        pushFinallyContinuation(finally_block, exc_why_name.is);

        for (AST_stmt* subnode : node->body) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        bool maybe_exception = exc_handlers.back().maybe_taken;
        exc_handlers.pop_back();

        int did_why = continuations.back().did_why; // bad to just reach in like this
        popContinuation();                          // finally continuation

        if (curblock) {
            // assign the exc_*_name variables to tell irgen that they won't be undefined?
            // have an :UNDEF() langprimitive to not have to do any loading there?
            pushAssign(exc_why_name, makeNum(Why::FALLTHROUGH, node->lineno));
            pushJump(finally_block);
        }

        if (exc_handler_block->predecessors.size() == 0) {
            delete exc_handler_block;
        } else {
            setInsertPoint(exc_handler_block);
            pushAssign(exc_why_name, makeNum(Why::EXCEPTION, node->lineno));
            pushJump(finally_block);
        }

        setInsertPoint(finally_block);

        for (AST_stmt* subnode : node->finalbody) {
            subnode->accept(this);
            if (!curblock)
                break;
        }

        if (curblock) {
            if (did_why & (1 << Why::RETURN))
                exitFinallyIf(node, Why::RETURN, exc_why_name);
            if (did_why & (1 << Why::BREAK))
                exitFinallyIf(node, Why::BREAK, exc_why_name);
            if (did_why & (1 << Why::CONTINUE))
                exitFinallyIf(node, Why::CONTINUE, exc_why_name);
            if (maybe_exception) {
                CFGBlock* reraise = cfg->addDeferredBlock();
                CFGBlock* noexc = makeFinallyCont(Why::EXCEPTION, exc_why_name, reraise);

                setInsertPoint(reraise);
                pushReraise(getLastLinenoSub(node->finalbody.back()), exc_type_name, exc_value_name,
                            exc_traceback_name);

                setInsertPoint(noexc);
            }
        }

        return true;
    }

    bool visit_with(AST_With* node) override {
        // see https://www.python.org/dev/peps/pep-0343/
        // section "Specification: the 'with' Statement"
        // which contains pseudocode for what this implements:
        //
        // mgr = (EXPR)
        // exit = type(mgr).__exit__            # not calling it yet
        // value = type(mgr).__enter__(mgr)
        // exc = True
        // try:
        //     VAR = value
        //     BLOCK
        // except:
        //     exc = False
        //     if not exit(mgr, *sys.exc_info()):
        //         raise
        // finally:
        //     if exc:
        //         exit(mgr, None, None, None)
        //
        // Unfortunately, this pseudocode isn't *quite* correct. We don't actually call type(mgr).__exit__ and
        // type(mgr).__enter__; rather, we use Python's "special method lookup rules" to find the appropriate method.
        // See https://docs.python.org/2/reference/datamodel.html#new-style-special-lookup. This is one reason we can't
        // just translate this into AST_Try{Except,Finally} nodes and recursively visit those. (If there are other
        // reasons, I've forgotten them.)
        assert(curblock);
        TmpValue ctxmgrname(nodeName("ctxmgr"), node->lineno);
        TmpValue exitname(nodeName("exit"), node->lineno);
        TmpValue whyname(nodeName("why"), node->lineno);
        TmpValue exc_type_name(nodeName("exc_type"), node->lineno);
        TmpValue exc_value_name(nodeName("exc_value"), node->lineno);
        TmpValue exc_traceback_name(nodeName("exc_traceback"), node->lineno);
        CFGBlock* exit_block = cfg->addDeferredBlock();
        exit_block->info = "with_exit";

        pushAssign(ctxmgrname, remapExpr(node->context_expr));

        // TODO(rntz): for some reason, in the interpreter (but not the JIT), this is looking up __exit__ on the
        // instance rather than the class. See test/tests/with_ctxclass_instance_attrs.py.
        TmpValue exit = makeLoadAttribute(_dup(ctxmgrname), internString("__exit__"), true);
        pushAssign(exitname, exit);

        // Oddly, this acces to __enter__ doesn't suffer from the same bug. Perhaps it has something to do with
        // __enter__ being called immediately?
        TmpValue enter = makeCallAttr(ctxmgrname, internString("__enter__"), true);
        if (node->optional_vars)
            pushAssign(node->optional_vars, enter);

        // push continuations
        CFGBlock* finally_block = cfg->addDeferredBlock();
        finally_block->info = "with_finally";
        pushFinallyContinuation(finally_block, whyname.is);

        CFGBlock* exc_block = cfg->addDeferredBlock();
        exc_block->info = "with_exc";
        exc_handlers.push_back({ exc_block, exc_type_name.is, exc_value_name.is, exc_traceback_name.is, false });

        for (int i = 0; i < node->body.size(); i++) {
            node->body[i]->accept(this);
            if (!curblock)
                break;
        }

        exc_handlers.pop_back();
        int finally_did_why = continuations.back().did_why;
        popContinuation();

        if (curblock) {
            // The try-suite finished as normal; jump to the finally block.
            pushAssign(whyname, makeNum(Why::FALLTHROUGH, node->lineno));
            pushJump(finally_block);
        }

        // The exception-handling block
        if (exc_block->predecessors.size() == 0) {
            // TODO(rntz): test for this case
            delete exc_block;
        } else {
            setInsertPoint(exc_block);

            // call the context-manager's exit method
            TmpValue suppressname(nodeName("suppress"), node->lineno);
            pushAssign(suppressname,
                       makeCall(exitname, { _dup(exc_type_name), _dup(exc_value_name), _dup(exc_traceback_name) }));

            // if it returns true, suppress the error and go to our exit block
            CFGBlock* reraise_block = cfg->addDeferredBlock();
            reraise_block->info = "with_reraise";
            // break potential critical edge
            CFGBlock* exiter = cfg->addDeferredBlock();
            exiter->info = "with_exiter";
            pushBranch(suppressname, exiter, reraise_block);

            setInsertPoint(exiter);
            pushJump(exit_block);

            // otherwise, reraise the exception
            setInsertPoint(reraise_block);
            pushReraise(getLastLinenoSub(node->body.back()), exc_type_name.is, exc_value_name.is,
                        exc_traceback_name.is);
        }

        // The finally block
        if (finally_block->predecessors.size() == 0) {
            // TODO(rntz): test for this case, "with foo: raise bar"
            delete finally_block;
        } else {
            setInsertPoint(finally_block);
            // call the context-manager's exit method, ignoring result
            makeCall(exitname, { makeNone(exitname.lineno), makeNone(exitname.lineno), makeNone(exitname.lineno) });

            if (finally_did_why & (1 << Why::CONTINUE))
                exitFinallyIf(node, Why::CONTINUE, whyname, /* is_kill */ finally_did_why == (1 << Why::CONTINUE));
            if (finally_did_why & (1 << Why::BREAK))
                exitFinallyIf(node, Why::BREAK, whyname, /* is_kill */ !(finally_did_why & (1 << Why::RETURN)));
            if (finally_did_why & (1 << Why::RETURN))
                exitFinallyIf(node, Why::RETURN, whyname, /* is_kill */ true);
            exitFinally(node, Why::FALLTHROUGH, exit_block);
        }

        if (exit_block->predecessors.size() == 0) {
            // FIXME(rntz): does this ever happen?
            // make a test for it!
            delete exit_block;
        } else {
            setInsertPoint(exit_block);
        }

        return true;
    }
};

void CFG::print(const CodeConstants& code_constants, llvm::raw_ostream& stream) {
    stream << "CFG:\n";
    stream << blocks.size() << " blocks\n";
    for (int i = 0; i < blocks.size(); i++)
        blocks[i]->print(code_constants, stream);
    stream.flush();
}

class AssignVRegsVisitor : public NoopBSTVisitor {
public:
    CFGBlock* current_block;
    int next_vreg;
    llvm::DenseMap<InternedString, DefaultedInt<VREG_UNDEFINED>> sym_vreg_map;
    llvm::DenseMap<InternedString, std::unordered_set<CFGBlock*>> sym_blocks_map;
    llvm::DenseSet<InternedString>
        name_is_read; // this is used to find unused desination vregs which we can transform to VREG_UNDEFINED
    std::vector<InternedString> vreg_sym_map;
    llvm::DenseMap<TrackingVRegPtr, InternedString>& id_vreg;

    enum Step { TrackBlockUsage = 0, UserVisible, CrossBlock, SingleBlockUse } step;

    AssignVRegsVisitor(const CodeConstants& code_constants, llvm::DenseMap<TrackingVRegPtr, InternedString>& id_vreg)
        : NoopBSTVisitor(code_constants), current_block(0), next_vreg(0), id_vreg(id_vreg) {}

    bool visit_vreg(int* vreg, bool is_dst = false) override {
        auto tracking_id = TrackingVRegPtr::create(vreg, current_block->cfg->bytecode);
        if (is_dst) {
            assert(id_vreg.count(tracking_id));
            if (*vreg != VREG_UNDEFINED)
                return true;
            InternedString id = id_vreg[tracking_id];
            if (step == TrackBlockUsage) {
                sym_blocks_map[id].insert(current_block);
                return true;
            } else if (step == UserVisible) {
                return true;
            } else {
                bool is_block_local = isNameUsedInSingleBlock(id);
                if (step == CrossBlock && is_block_local)
                    return true;
                if (step == SingleBlockUse && !is_block_local)
                    return true;
            }
            if (step == SingleBlockUse && !name_is_read.count(id) && id.isCompilerCreatedName()) {
                *vreg = VREG_UNDEFINED;
                return true;
            }
            *vreg = assignVReg(id);
            return true;
        }

        if (!id_vreg.count(tracking_id)) {
            if (*vreg >= 0)
                *vreg = VREG_UNDEFINED;
            return true;
        }

        auto id = id_vreg[tracking_id];

        if (step == TrackBlockUsage) {
            name_is_read.insert(id);
            sym_blocks_map[id].insert(current_block);
            return true;
        } else if (step == UserVisible) {
            if (id.isCompilerCreatedName())
                return true;
        } else {
            bool is_block_local = isNameUsedInSingleBlock(id);
            if (step == CrossBlock && is_block_local)
                return true;
            if (step == SingleBlockUse && !is_block_local)
                return true;
        }
        *vreg = assignVReg(id);

        return true;
    }

    bool isNameUsedInSingleBlock(InternedString id) {
        assert(step != TrackBlockUsage);
        assert(sym_blocks_map.count(id));
        return sym_blocks_map[id].size() == 1;
    }

    template <typename T> bool visit_nameHelper(T* node, InternedString id) {
        if (node->vreg != VREG_UNDEFINED)
            return true;

        ASSERT(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN, "%s", id.c_str());

        if (node->lookup_type != ScopeInfo::VarScopeType::FAST && node->lookup_type != ScopeInfo::VarScopeType::CLOSURE)
            return true;

        if (step == TrackBlockUsage) {
            sym_blocks_map[id].insert(current_block);
            return true;
        } else if (step == UserVisible) {
            if (id.isCompilerCreatedName())
                return true;
        } else {
            bool is_block_local = node->lookup_type == ScopeInfo::VarScopeType::FAST && isNameUsedInSingleBlock(id);
            if (step == CrossBlock && is_block_local)
                return true;
            if (step == SingleBlockUse && !is_block_local)
                return true;
        }
        node->vreg = assignVReg(id);
        return true;
    }

    bool visit_loadname(BST_LoadName* node) override {
        visit_vreg(&node->vreg_dst, true);
        InternedString id = code_constants.getInternedString(node->index_id);
        return visit_nameHelper(node, id);
    }

    bool visit_storename(BST_StoreName* node) override {
        visit_vreg(&node->vreg_value);
        InternedString id = code_constants.getInternedString(node->index_id);
        return visit_nameHelper(node, id);
    }

    bool visit_deletename(BST_DeleteName* node) override {
        InternedString id = code_constants.getInternedString(node->index_id);
        return visit_nameHelper(node, id);
    }

    int assignVReg(InternedString id) {
        assert(id.s().size());

        auto it = sym_vreg_map.find(id);
        if (sym_vreg_map.end() == it) {
            sym_vreg_map[id] = next_vreg;

            if (!REUSE_VREGS || step == UserVisible || step == CrossBlock) {
                assert(next_vreg == vreg_sym_map.size());
                vreg_sym_map.push_back(id);
            }
            return next_vreg++;
        }
        return it->second;
    }
};

void VRegInfo::assignVRegs(const CodeConstants& code_constants, CFG* cfg, const ParamNames& param_names,
                           llvm::DenseMap<TrackingVRegPtr, InternedString>& id_vreg) {
    assert(!hasVRegsAssigned());

    // warning: don't rearrange the steps, they need to be run in this exact order!
    AssignVRegsVisitor visitor(code_constants, id_vreg);
    for (auto step : { AssignVRegsVisitor::TrackBlockUsage, AssignVRegsVisitor::UserVisible,
                       AssignVRegsVisitor::CrossBlock, AssignVRegsVisitor::SingleBlockUse }) {
        visitor.step = step;

        for (CFGBlock* b : cfg->blocks) {
            visitor.current_block = b;

#if REUSE_VREGS
            if (step == AssignVRegsVisitor::SingleBlockUse)
                visitor.next_vreg = num_vregs_cross_block;
#endif

            if (b == cfg->getStartingBlock()) {
                for (auto* name : param_names.allArgsAsName()) {
                    visitor.visit_nameHelper(name, name->id);
                }
            }

            for (BST_stmt* stmt : *b) {
                stmt->accept(&visitor);
            }

            if (step == AssignVRegsVisitor::SingleBlockUse)
                num_vregs = std::max(num_vregs, visitor.next_vreg);
        }

        if (step == AssignVRegsVisitor::UserVisible) {
            num_vregs_user_visible = visitor.sym_vreg_map.size();
#ifndef NDEBUG
            sym_vreg_map_user_visible = visitor.sym_vreg_map;
#endif
        } else if (step == AssignVRegsVisitor::CrossBlock)
            num_vregs = num_vregs_cross_block = visitor.next_vreg;
    }
#ifndef NDEBUG
    sym_vreg_map = std::move(visitor.sym_vreg_map);
#endif
    vreg_sym_map = std::move(visitor.vreg_sym_map);
    vreg_sym_map.shrink_to_fit();
    assert(hasVRegsAssigned());
#if REUSE_VREGS
    assert(vreg_sym_map.size() == num_vregs_cross_block);
#else
    assert(vreg_sym_map.size() == num_vregs);
#endif
}

// Prune unnecessary blocks from the CFG.
// Not strictly necessary, but makes the output easier to look at,
// and can make the analyses more efficient.
// The extra blocks would get merged by LLVM passes, so I'm not sure
// how much overall improvement there is.
// returns num of removed blocks
static int pruneUnnecessaryBlocks(CFG* rtn) {
    llvm::DenseMap<CFGBlock*, CFGBlock*> blocks_to_merge;
    // Must evaluate end() on every iteration because erase() will invalidate the end.
    for (auto it = rtn->blocks.begin(); it != rtn->blocks.end(); ++it) {
        CFGBlock* b = *it;

        auto b_successors = b->successors();
        if (b_successors.size() == 1) {
            CFGBlock* b2 = b_successors[0];
            if (b2->predecessors.size() != 1)
                continue;

            auto last_stmt = b->getTerminator();
            if (last_stmt->is_invoke()) {
                // TODO probably shouldn't be generating these anyway:
                assert(last_stmt->get_normal_block() == last_stmt->get_exc_block());
                continue;
            }

            assert(last_stmt->type() == BST_TYPE::Jump);

            if (VERBOSITY("cfg") >= 2) {
                // rtn->print();
                printf("Joining blocks %d and %d\n", b->idx, b2->idx);
            }

            b->unconnectFrom(b2);

            for (CFGBlock* b3 : b2->successors()) {
                b->connectTo(b3, true);
                b2->unconnectFrom(b3);
            }

            rtn->blocks.erase(std::remove(rtn->blocks.begin(), rtn->blocks.end(), b2), rtn->blocks.end());

            blocks_to_merge[b] = b2;
        }
    }

    if (!blocks_to_merge.empty()) {
        BSTAllocator final_bytecode;
        final_bytecode.reserve(rtn->bytecode.getSize());

        int offset = 0;
        for (CFGBlock* b : rtn->blocks) {
            int block_size = b->sizeInBytes();
            int new_offset_of_block_start = offset;
            bool should_merge_blocks = blocks_to_merge.count(b);
            if (should_merge_blocks) {
                // copy first block without the terminator
                block_size -= b->getTerminator()->size_in_bytes();
                memcpy(final_bytecode.allocate(block_size), b->body(), block_size);
                offset += block_size;
                // copy second block and delete it
                CFGBlock* second_block = blocks_to_merge[b];
                int second_block_size = second_block->sizeInBytes();
                memcpy(final_bytecode.allocate(second_block_size), second_block->body(), second_block_size);
                offset += second_block_size;
                delete second_block;
            } else {
                memcpy(final_bytecode.allocate(block_size), b->body(), block_size);
                offset += block_size;
            }
            // update block start offset
            b->offset_of_first_stmt = new_offset_of_block_start;
        }
        rtn->bytecode = std::move(final_bytecode);
    }

    return blocks_to_merge.size();
}

static std::pair<CFG*, CodeConstants> computeCFG(llvm::ArrayRef<AST_stmt*> body, AST_TYPE::AST_TYPE ast_type,
                                                 int lineno, AST_arguments* args, BoxedString* filename,
                                                 SourceInfo* source, const ParamNames& param_names, ScopeInfo* scoping,
                                                 ModuleCFGProcessor* cfgizer) {
    STAT_TIMER(t0, "us_timer_computecfg", 0);

    CFG* rtn = new CFG();

    assert((bool)args == (ast_type == AST_TYPE::FunctionDef || ast_type == AST_TYPE::Lambda));

    auto&& stringpool = cfgizer->stringpool;
    CFGVisitor visitor(filename, source, stringpool, scoping, ast_type, source->future_flags, rtn, cfgizer);

    bool skip_first = false;

    if (ast_type == AST_TYPE::ClassDef) {
        InternedString id = stringpool.get("__name__");
        // A classdef always starts with "__module__ = __name__"
        auto module_name_value = visitor.allocAndPush<BST_LoadName>();
        module_name_value->lineno = lineno;
        module_name_value->index_id = visitor.remapInternedString(id);
        fillScopingInfo(module_name_value, id, scoping);
        TmpValue module_name = visitor.createDstName(module_name_value);
        visitor.pushStoreName(stringpool.get("__module__"), module_name);

        // If the first statement is just a single string, transform it to an assignment to __doc__
        if (body.size() && body[0]->type == AST_TYPE::Expr) {
            AST_Expr* first_expr = ast_cast<AST_Expr>(body[0]);
            if (first_expr->value->type == AST_TYPE::Str) {
                visitor.pushStoreName(stringpool.get("__doc__"),
                                      visitor.remapStr(ast_cast<AST_Str>(first_expr->value)));
                skip_first = true;
            }
        }
    }

    if (ast_type == AST_TYPE::FunctionDef || ast_type == AST_TYPE::Lambda) {
        // Unpack tuple arguments
        // Tuple arguments get assigned names ".0", ".1" etc. So this
        // def f(a, (b,c), (d,e)):
        // would expand to:
        // def f(a, .1, .2):
        //     (b, c) = .1
        //     (d, e) = .2
        int counter = 0;
        for (AST_expr* arg_expr : args->args) {
            if (arg_expr->type == AST_TYPE::Tuple) {
                InternedString arg_name = stringpool.get("." + std::to_string(counter));
                assert(scoping->getScopeTypeOfName(arg_name) == ScopeInfo::VarScopeType::FAST);

                auto load = visitor.allocAndPush<BST_LoadName>();
                load->index_id = visitor.remapInternedString(arg_name);
                load->lineno = arg_expr->lineno;
                fillScopingInfo(load, arg_name, scoping);
                TmpValue val = visitor.createDstName(load);

                visitor.pushAssign(arg_expr, val);
            } else {
                assert(arg_expr->type == AST_TYPE::Name);
            }
            counter++;
        }
    }

    for (int i = (skip_first ? 1 : 0); i < body.size(); i++) {
        if (!visitor.curblock)
            break;
        ASSERT(body[i]->lineno > 0, "%d", body[i]->type);
        body[i]->accept(&visitor);
    }

    // The functions we create for classdefs are supposed to return a dictionary of their locals.
    // This is the place that we add all of that:
    if (ast_type == AST_TYPE::ClassDef) {
        BST_Locals* locals = visitor.allocAndPush<BST_Locals>();
        TmpValue name = visitor.createDstName(locals);

        BST_Return* rtn = visitor.allocAndPush<BST_Return>();
        rtn->lineno = getLastLineno(body, lineno);
        visitor.unmapExpr(name, &rtn->vreg_value);
    } else if (visitor.curblock) {
        // Put a fake "return" statement at the end of every function just to make sure they all have one;
        // we already have to support multiple return statements in a function, but this way we can avoid
        // having to support not having a return statement:
        BST_Return* return_stmt = visitor.allocAndPush<BST_Return>();
        return_stmt->lineno = getLastLineno(body, lineno);
        return_stmt->vreg_value = VREG_UNDEFINED;
    }

    if (VERBOSITY("cfg") >= 3) {
        printf("Before cfg checking and transformations:\n");
        rtn->print(visitor.code_constants);
    }

#ifndef NDEBUG
    ////
    // Check some properties expected by later stages:
    assert(rtn->getStartingBlock()->predecessors.size() == 0);

    for (CFGBlock* b : rtn->blocks) {
        ASSERT(b->idx != -1 && b->isPlaced(), "Forgot to place a block!");
        for (CFGBlock* b2 : b->predecessors) {
            ASSERT(b2->idx != -1 && b->isPlaced(), "Forgot to place a block!");
        }
        for (CFGBlock* b2 : b->successors()) {
            ASSERT(b2->idx != -1 && b->isPlaced(), "Forgot to place a block!");
        }

        ASSERT(b->body() != NULL, "%d", b->idx);
        ASSERT(b->successors().size() <= 2, "%d has too many successors!", b->idx);
        if (b->successors().size() == 0) {
            BST_stmt* terminator = b->getTerminator();
            assert(terminator->type() == BST_TYPE::Return || terminator->type() == BST_TYPE::Raise
                   || terminator->type() == BST_TYPE::Assert);
        }

        if (b->predecessors.size() == 0) {
            if (b != rtn->getStartingBlock()) {
                rtn->print(visitor.code_constants);
            }
            ASSERT(b == rtn->getStartingBlock(), "%d", b->idx);
        }
    }

    // We need to generate the CFG in a way that doesn't have any critical edges,
    // since the ir generation requires that.
    // We could do this with a separate critical-edge-breaking pass, but for now
    // the cfg-computing code directly avoids making critical edges.
    // Either way, double check to make sure that we don't have any:
    for (int i = 0; i < rtn->blocks.size(); i++) {
        auto successors = rtn->blocks[i]->successors();
        if (successors.size() >= 2) {
            for (CFGBlock* successor : successors) {
                // It's ok to have zero predecessors if you are the entry block
                ASSERT(successor->predecessors.size() < 2, "Critical edge from %d to %d!", i, successor->idx);
            }
        }
    }

    // The cfg blocks should be generated in roughly program order.
    // Specifically, this means every block should have one predecessor block that
    // has a lower index (except for block 0).
    // We use this during IR generation to ensure that at least one predecessor has always
    // been evaluated before the current block; this property also ensures that there are no
    // dead blocks.
    for (int i = 1; i < rtn->blocks.size(); i++) {
        bool good = false;
        for (int j = 0; j < rtn->blocks[i]->predecessors.size(); j++) {
            if (rtn->blocks[i]->predecessors[j]->idx < i)
                good = true;
        }
        if (!good) {
            printf("internal error: block %d doesn't have a previous predecessor\n", i);
            abort();
        }

        // Later phases also rely on the fact that the first predecessor has a lower index;
        // this can be worked around but it's easiest just to ensure this here.
        assert(rtn->blocks[i]->predecessors[0]->idx < i);
    }

    assert(rtn->getStartingBlock()->idx == 0);

// Uncomment this for some heavy checking to make sure that we don't forget
// to set lineno.  It will catch a lot of things that don't necessarily
// need to be fixed.
#if 0
    for (auto b : rtn->blocks) {
        for (auto bst : *b) {
            if (bst->type() == BST_TYPE::Jump)
                continue;
            if (bst->type() == BST_TYPE::Landingpad)
                continue;
            if (bst->type() == BST_TYPE::UncacheExcInfo)
                continue;
            if (bst->type() == BST_TYPE::SetExcInfo)
                continue;

            if (bst->type() == BST_TYPE::DeleteName) {
                auto* del = bst_cast<BST_DeleteName>(bst);
                InternedString id = visitor.code_constants.getInternedString(del->index_id);
                if (id.s()[0] == '#')
                    continue;
            }


            //if (bst->type() != AST_TYPE::Return)
                //continue;
            if (bst->lineno == 0) {
                rtn->print(visitor.code_constants);
                printf("\n");
                print_bst(bst, visitor.code_constants);
                printf("\n");
            }
            assert(bst->lineno > 0);
        }
    }
#endif

// TODO make sure the result of Invoke nodes are not used on the exceptional path
#endif

    rtn->getVRegInfo().assignVRegs(visitor.code_constants, rtn, param_names, visitor.id_vreg);

    pruneUnnecessaryBlocks(rtn);

    visitor.code_constants.optimizeSize();
    rtn->bytecode.optimizeSize();
    rtn->blocks.shrink_to_fit();

    static StatCounter bst_bytecode_bytes("num_bst_bytecode_bytes");
    bst_bytecode_bytes.log(rtn->bytecode.getSize());

    // TODO add code which serializes the final bytecode to disk

    if (VERBOSITY("cfg") >= 2) {
        printf("Final cfg:\n");
        rtn->print(visitor.code_constants, llvm::outs());
    }

    return std::make_pair(rtn, std::move(visitor.code_constants));
}


BoxedCode* ModuleCFGProcessor::runRecursively(llvm::ArrayRef<AST_stmt*> body, BoxedString* name, int lineno,
                                              AST_arguments* args, AST* orig_node) {
    ScopeInfo* scope_info = scoping.getScopeInfoForNode(orig_node);

    AST_TYPE::AST_TYPE ast_type = orig_node->type;
    bool is_generator;
    switch (ast_type) {
        case AST_TYPE::ClassDef:
        case AST_TYPE::Module:
        case AST_TYPE::Expression:
        case AST_TYPE::Suite:
            is_generator = false;
            break;
        case AST_TYPE::GeneratorExp:
        case AST_TYPE::DictComp:
        case AST_TYPE::SetComp:
            is_generator = ast_type == AST_TYPE::GeneratorExp;
            assert(containsYield(body) == is_generator);

            // Hack: our old system represented this as ast_type == FuntionDef, so
            // keep doing that for now
            ast_type = AST_TYPE::FunctionDef;

            break;
        case AST_TYPE::FunctionDef:
        case AST_TYPE::Lambda:
            is_generator = containsYield(orig_node);
            break;
        default:
            RELEASE_ASSERT(0, "Unknown type: %d", ast_type);
            break;
    }

    std::unique_ptr<SourceInfo> si(new SourceInfo(bm, ScopingResults(scope_info, scoping.areGlobalsFromModule()),
                                                  future_flags, ast_type, is_generator));

    assert((bool)args == (ast_type == AST_TYPE::FunctionDef || ast_type == AST_TYPE::Lambda));

    ParamNames param_names(args, stringpool);

    for (auto e : param_names.allArgsAsName())
        fillScopingInfo(e, scope_info);

    CodeConstants code_constants;
    std::tie(si->cfg, code_constants)
        = computeCFG(body, ast_type, lineno, args, fn, si.get(), param_names, scope_info, this);

    BoxedCode* code;
    if (args)
        code = new BoxedCode(args->args.size(), args->vararg, args->kwarg, lineno, std::move(si),
                             std::move(code_constants), std::move(param_names), fn, name,
                             autoDecref(getDocString(body)));
    else
        code = new BoxedCode(0, false, false, lineno, std::move(si), std::move(code_constants), std::move(param_names),
                             fn, name, autoDecref(getDocString(body)));

    return code;
}

BoxedCode* computeAllCFGs(AST* ast, bool globals_from_module, FutureFlags future_flags, BoxedString* fn,
                          BoxedModule* bm) {
    return ModuleCFGProcessor(ast, globals_from_module, future_flags, fn, bm)
        .runRecursively(ast->getBody(), ast->getName(), ast->lineno, nullptr, ast);
}

void printCFG(CFG* cfg, const CodeConstants& code_constants) {
    cfg->print(code_constants);
}
}
