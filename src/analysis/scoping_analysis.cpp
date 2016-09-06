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

#include "analysis/scoping_analysis.h"

#include "llvm/ADT/DenseSet.h"

#include "core/ast.h"
#include "core/bst.h"
#include "core/common.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/types.h"

namespace pyston {

ScopingResults::ScopingResults(ScopeInfo* scope_info, bool globals_from_module)
    : are_locals_from_module(scope_info->areLocalsFromModule()),
      are_globals_from_module(globals_from_module),
      creates_closure(scope_info->createsClosure()),
      takes_closure(scope_info->takesClosure()),
      passes_through_closure(scope_info->passesThroughClosure()),
      uses_name_lookup(scope_info->usesNameLookup()),
      closure_size(creates_closure ? scope_info->getClosureSize() : 0) {
    deref_info = scope_info->getAllDerefVarsAndInfo();
}

DerefInfo ScopingResults::getDerefInfo(BST_Name* node) const {
    assert(node->lookup_type == ScopeInfo::VarScopeType::DEREF);
    assert(node->deref_info.offset != INT_MAX);
    return node->deref_info;
}
size_t ScopingResults::getClosureOffset(BST_Name* node) const {
    assert(node->lookup_type == ScopeInfo::VarScopeType::CLOSURE);
    assert(node->closure_offset != -1);
    return node->closure_offset;
}

class YieldVisitor : public NoopASTVisitor {
public:
    AST* starting_node;
    bool contains_yield;

    YieldVisitor(AST* initial_node) : starting_node(initial_node), contains_yield(false) {}

    // we are only interested if the statements of the initial node contain a yield not if any child function contains a
    // yield
    bool shouldSkip(AST* node) const { return starting_node != node; }
    bool visit_classdef(AST_ClassDef* node) override { return shouldSkip(node); }
    bool visit_functiondef(AST_FunctionDef* node) override { return shouldSkip(node); }
    bool visit_lambda(AST_Lambda* node) override { return shouldSkip(node); }

    bool visit_yield(AST_Yield*) override {
        contains_yield = true;
        return true;
    }
};

bool containsYield(AST* ast) {
    YieldVisitor visitor(ast);
    ast->accept(&visitor);
    return visitor.contains_yield;
}

bool containsYield(llvm::ArrayRef<AST_stmt*> body) {
    for (auto e : body)
        if (containsYield(e))
            return true;
    return false;
}

// TODO
// Combine this with the below? Basically the same logic with different string types...
// Also should this go in this file?
BoxedString* mangleNameBoxedString(BoxedString* id, BoxedString* private_name) {
    assert(id);
    assert(private_name);
    int len = id->size();
    if (len < 2 || id->s()[0] != '_' || id->s()[1] != '_')
        return incref(id);

    if ((id->s()[len - 2] == '_' && id->s()[len - 1] == '_') || id->s().find('.') != llvm::StringRef::npos)
        return incref(id);

    const char* p = private_name->data();
    while (*p == '_') {
        p++;
        len--;
    }
    if (*p == '\0')
        return incref(id);

    return static_cast<BoxedString*>(boxStringTwine("_" + (p + id->s())));
}

static void mangleNameInPlace(InternedString& id, llvm::StringRef private_name, InternedStringPool& interned_strings) {
    if (!private_name.size())
        return;

    int len = id.s().size();
    if (len < 2 || id.s()[0] != '_' || id.s()[1] != '_')
        return;

    if ((id.s()[len - 2] == '_' && id.s()[len - 1] == '_') || id.s().find('.') != std::string::npos)
        return;

    assert(private_name.data()[private_name.size()] == '\0');
    const char* p = private_name.data();
    while (*p == '_') {
        p++;
        len--;
    }
    if (*p == '\0')
        return;

    // TODO add a twine interface to interned strings?
    id = interned_strings.get("_" + std::string(p) + std::string(id.s()));
}

static InternedString mangleName(InternedString id, llvm::StringRef private_name,
                                 InternedStringPool& interned_strings) {
    InternedString rtn(id);
    mangleNameInPlace(rtn, private_name, interned_strings);
    return rtn;
}

class ModuleScopeInfo : public ScopeInfo {
public:
    ScopeInfo* getParent() override { return NULL; }

    bool createsClosure() override { return false; }
    bool takesClosure() override { return false; }
    bool passesThroughClosure() override { return false; }

    VarScopeType getScopeTypeOfName(InternedString name) override {
        if (name.isCompilerCreatedName())
            return VarScopeType::FAST;
        else
            return VarScopeType::GLOBAL;
    }

    bool usesNameLookup() override { return false; }

    bool areLocalsFromModule() override { return true; }

    DerefInfo getDerefInfo(InternedString) override { RELEASE_ASSERT(0, "This should never get called"); }
    size_t getClosureOffset(InternedString) override { RELEASE_ASSERT(0, "This should never get called"); }
    size_t getClosureSize() override { RELEASE_ASSERT(0, "This should never get called"); }
    std::vector<std::pair<InternedString, DerefInfo>> v;
    const std::vector<std::pair<InternedString, DerefInfo>>& getAllDerefVarsAndInfo() override { return v; }

    InternedString mangleName(InternedString id) override { return id; }
    InternedString internString(llvm::StringRef s) override { abort(); }
};

typedef llvm::DenseSet<InternedString> StrSet;

// Handles the scope in eval or exec
// For example for exec, if you write
// exec "global a ; print a ; print b"
// It will give `a` the GLOBAL scope type and `b` the NAME type.
// (For eval, you can't have global statements, so it will just
// mark everything NAME.)
class EvalExprScopeInfo : public ScopeInfo {
private:
    StrSet forced_globals;

    struct GlobalStmtVisitor : NoopASTVisitor {
        StrSet& result;
        GlobalStmtVisitor(StrSet& result) : result(result) {}

        bool visit_functiondef(AST_FunctionDef*) override { return true; }
        bool visit_classdef(AST_ClassDef*) override { return true; }

        bool visit_global(AST_Global* global_stmt) override {
            for (InternedString name : global_stmt->names) {
                result.insert(name);
            }
            return true;
        }
    };

    bool globals_from_module;

public:
    EvalExprScopeInfo(AST* node, bool globals_from_module) : globals_from_module(globals_from_module) {
        // Find all the global statements in the node's scope (not delving into FuncitonDefs
        // or ClassDefs) and put the names in `forced_globals`.
        GlobalStmtVisitor visitor(forced_globals);
        node->accept(&visitor);
    }

    ScopeInfo* getParent() override { return NULL; }

    bool createsClosure() override { return false; }
    bool takesClosure() override { return false; }
    bool passesThroughClosure() override { return false; }

    VarScopeType getScopeTypeOfName(InternedString name) override {
        if (name.isCompilerCreatedName())
            return VarScopeType::FAST;
        else if (forced_globals.find(name) != forced_globals.end())
            return VarScopeType::GLOBAL;
        else
            return VarScopeType::NAME;
    }

    bool usesNameLookup() override { return true; }

    bool areLocalsFromModule() override { return false; }

    DerefInfo getDerefInfo(InternedString) override { RELEASE_ASSERT(0, "This should never get called"); }
    size_t getClosureOffset(InternedString) override { RELEASE_ASSERT(0, "This should never get called"); }
    size_t getClosureSize() override { RELEASE_ASSERT(0, "This should never get called"); }
    std::vector<std::pair<InternedString, DerefInfo>> v;
    const std::vector<std::pair<InternedString, DerefInfo>>& getAllDerefVarsAndInfo() override { return v; }

    InternedString mangleName(InternedString id) override { return id; }
    InternedString internString(llvm::StringRef s) override { abort(); }
};

struct ScopeNameUsageEntry {
    // Properties determined from crawling the scope:
    unsigned char read : 1;
    unsigned char written : 1;
    unsigned char forced_globals : 1;
    unsigned char params : 1;

    // Properties determined by looking at other scopes as well:
    unsigned char referenced_from_nested : 1;
    unsigned char got_from_closure : 1;
    unsigned char passthrough_accesses : 1; // what names a child scope accesses a name from a parent scope

    ScopeNameUsageEntry()
        : read(0),
          written(0),
          forced_globals(0),
          params(0),
          referenced_from_nested(0),
          got_from_closure(0),
          passthrough_accesses(0) {}
};

struct ScopingAnalysis::ScopeNameUsage {
    AST* node;
    ScopeNameUsage* parent;
    llvm::StringRef private_name;
    ScopingAnalysis* scoping;

    std::unordered_map<InternedString, ScopeNameUsageEntry> results;

    std::vector<AST_Name*> del_name_nodes;

    // `import *` and `exec` both force the scope to use the NAME lookup
    // However, this is not allowed to happen (a SyntaxError) if the scope has
    // "free variables", variables read but not written (and not forced to be global)
    // Furthermore, no child of the scope can have any free variables either
    // (not even if the variables would refer to a closure in an in-between child).
    AST_ImportFrom* nameForcingNodeImportStar;
    AST_Exec* nameForcingNodeBareExec;
    bool hasNameForcingSyntax() { return nameForcingNodeImportStar != NULL || nameForcingNodeBareExec != NULL; }

    // If it has a free variable / if any child has a free variable
    // `free` is set to true if there is a variable which is read but not written,
    // unless there is a `global` statement (possibly in a parent scope--but note
    // that `forced_globals` only contains the `global` statements in *this* scope).
    // See the computation in process_name_usages below for specifics.
    // `child_free` is then set on any parent scopes of a scope that has `free` set.
    bool free;
    bool child_free;

    ScopeNameUsage(AST* node, ScopeNameUsage* parent, ScopingAnalysis* scoping)
        : node(node),
          parent(parent),
          scoping(scoping),
          nameForcingNodeImportStar(NULL),
          nameForcingNodeBareExec(NULL),
          free(false),
          child_free(false) {
        if (node->type == AST_TYPE::ClassDef) {
            AST_ClassDef* classdef = ast_cast<AST_ClassDef>(node);

            // classes have an implicit write to "__module__"
            results[scoping->getInternedStrings().get("__module__")].written = 1;

            if (classdef->body.size() && classdef->body[0]->type == AST_TYPE::Expr) {
                AST_Expr* first_expr = ast_cast<AST_Expr>(classdef->body[0]);
                if (first_expr->value->type == AST_TYPE::Str) {
                    results[scoping->getInternedStrings().get("__doc__")].written = 1;
                }
            }
        }

        if (node->type == AST_TYPE::ClassDef) {
            private_name = ast_cast<AST_ClassDef>(node)->name;
        } else if (parent) {
            private_name = parent->private_name;
        } else {
            private_name = llvm::StringRef();
        }
    }

    void dump() {
#define DUMP(n)                                                                                                        \
    printf(STRINGIFY(n) ":\n");                                                                                        \
    for (auto s : results) {                                                                                           \
        if (!s.second.n)                                                                                               \
            continue;                                                                                                  \
        printf("%s\n", s.first.c_str());                                                                               \
    }

        DUMP(read);
        DUMP(written);
        DUMP(forced_globals);
        printf("\n");
        DUMP(referenced_from_nested);
        DUMP(got_from_closure);
        DUMP(passthrough_accesses);
    }
};

class ScopeInfoBase : public ScopeInfo {
private:
    ScopeInfo* parent;
    ScopingAnalysis::ScopeNameUsage* usage;
    AST* ast;
    bool usesNameLookup_;

    llvm::DenseMap<InternedString, size_t> closure_offsets;

    std::vector<std::pair<InternedString, DerefInfo>> allDerefVarsAndInfo;
    bool allDerefVarsAndInfoCached;

    bool globals_from_module;
    bool takes_closure = false;
    bool passthrough_accesses = false;

public:
    ScopeInfoBase(ScopeInfo* parent, ScopingAnalysis::ScopeNameUsage* usage, AST* ast, bool usesNameLookup,
                  bool globals_from_module)
        : parent(parent),
          usage(usage),
          ast(ast),
          usesNameLookup_(usesNameLookup),
          allDerefVarsAndInfoCached(false),
          globals_from_module(globals_from_module) {
        assert(usage);
        assert(ast);

        bool got_from_closure = false;
        std::vector<InternedString> referenced_from_nested_sorted;
        for (auto&& r : usage->results) {
            if (r.second.referenced_from_nested)
                referenced_from_nested_sorted.push_back(r.first);
            got_from_closure = got_from_closure || r.second.got_from_closure;
            passthrough_accesses = passthrough_accesses || r.second.passthrough_accesses;
        }

        // Sort the entries by name to make the order deterministic.
        std::sort(referenced_from_nested_sorted.begin(), referenced_from_nested_sorted.end());
        int i = 0;
        for (auto& p : referenced_from_nested_sorted) {
            closure_offsets[p] = i;
            i++;
        }

        takes_closure = got_from_closure || passthrough_accesses;
    }

    ~ScopeInfoBase() override { delete this->usage; }

    ScopeInfo* getParent() override { return parent; }

    bool createsClosure() override { return closure_offsets.size() > 0; }

    bool takesClosure() override { return takes_closure; }

    bool passesThroughClosure() override { return passthrough_accesses && !createsClosure(); }

    VarScopeType getScopeTypeOfName(InternedString name) override {
        if (name.isCompilerCreatedName())
            return VarScopeType::FAST;

        ScopeNameUsageEntry r;
        auto it = usage->results.find(name);
        if (it != usage->results.end())
            r = it->second;

        if (r.forced_globals)
            return VarScopeType::GLOBAL;

        if (r.got_from_closure)
            return VarScopeType::DEREF;

        if (usesNameLookup_) {
            return VarScopeType::NAME;
        } else {
            if (!r.written)
                return VarScopeType::GLOBAL;
            else if (r.referenced_from_nested)
                return VarScopeType::CLOSURE;
            else
                return VarScopeType::FAST;
        }
    }

    bool usesNameLookup() override { return usesNameLookup_; }

    bool areLocalsFromModule() override { return false; }

    DerefInfo getDerefInfo(InternedString name) override {
        assert(getScopeTypeOfName(name) == VarScopeType::DEREF);

        // TODO pre-compute this?

        size_t parentCounter = 0;
        // Casting to a ScopeInfoBase* is okay because only a ScopeInfoBase can have a closure.
        // We just walk up the scopes until we find the scope with this name. Count the number
        // of parent links we follow, and then get the offset of the name.
        for (ScopeInfoBase* parent = static_cast<ScopeInfoBase*>(this->parent); parent != NULL;
             parent = static_cast<ScopeInfoBase*>(parent->parent)) {
            if (parent->createsClosure()) {
                auto it = parent->closure_offsets.find(name);
                if (it != parent->closure_offsets.end()) {
                    return DerefInfo{.num_parents_from_passed_closure = parentCounter, .offset = it->second };
                }
                parentCounter++;
            }
        }

        RELEASE_ASSERT(0, "Should not get here");
    }

    size_t getClosureOffset(InternedString name) override {
        assert(getScopeTypeOfName(name) == VarScopeType::CLOSURE);
        return closure_offsets[name];
    }

    size_t getClosureSize() override {
        assert(createsClosure());
        return closure_offsets.size();
    }

    InternedString mangleName(const InternedString id) override {
        return pyston::mangleName(id, usage->private_name, usage->scoping->getInternedStrings());
    }

    InternedString internString(llvm::StringRef s) override { return usage->scoping->getInternedStrings().get(s); }

    const std::vector<std::pair<InternedString, DerefInfo>>& getAllDerefVarsAndInfo() override {
        if (!allDerefVarsAndInfoCached) {
            allDerefVarsAndInfoCached = true;

            // TODO this could probably be implemented faster

            // Get all the variables that we need to return: any variable from the
            // passed-in closure that is accessed in this scope or in a child scope.
            for (auto&& r : usage->results) {
                if (!r.second.got_from_closure)
                    continue;

                auto&& name = r.first;
                // Call `getDerefInfo` on all of these variables and put the results in
                // `allDerefVarsAndInfo`
                allDerefVarsAndInfo.push_back({ name, getDerefInfo(name) });
            }


            // Sort in order of `num_parents_from_passed_closure`
            std::sort(
                allDerefVarsAndInfo.begin(), allDerefVarsAndInfo.end(),
                [](const std::pair<InternedString, DerefInfo>& p1, const std::pair<InternedString, DerefInfo>& p2) {
                    return p1.second.num_parents_from_passed_closure < p2.second.num_parents_from_passed_closure;
                });
        }
        return allDerefVarsAndInfo;
    }
};

static void raiseGlobalAndLocalException(InternedString name, AST* node) {
    assert(node->type == AST_TYPE::FunctionDef);
    AST_FunctionDef* funcNode = ast_cast<AST_FunctionDef>(node);
    char buf[1024];
    snprintf(buf, sizeof(buf), "name '%s' is local and global", name.c_str());
    raiseSyntaxError(buf, funcNode->lineno, funcNode->col_offset, "" /* file?? */, funcNode->name.s());
}

class NameCollectorVisitor : public ASTVisitor {
private:
    AST* orig_node;
    ScopingAnalysis::NameUsageMap* map;
    ScopingAnalysis::ScopeNameUsage* cur;
    ScopingAnalysis* scoping;
    bool currently_visiting_functiondef_args;

    NameCollectorVisitor(AST* node, ScopingAnalysis::NameUsageMap* map, ScopingAnalysis* scoping)
        : orig_node(node), map(map), scoping(scoping), currently_visiting_functiondef_args(false) {
        assert(map);
        cur = (*map)[node];
        assert(cur);
    }

public:
    void doWrite(InternedString name) {
        assert(name == mangleName(name, cur->private_name, scoping->getInternedStrings()));
        auto& r = cur->results[name];
        r.read = 1;
        r.written = 1;
        if (this->currently_visiting_functiondef_args) {
            r.params = 1;
        }
    }

    void doRead(InternedString name) {
        assert(name == mangleName(name, cur->private_name, scoping->getInternedStrings()));
        cur->results[name].read = 1;
    }

    void doDel(AST_Name* node) { cur->del_name_nodes.push_back(node); }

    void doImportStar(AST_ImportFrom* node) {
        if (cur->nameForcingNodeImportStar == NULL)
            cur->nameForcingNodeImportStar = node;
    }

    void doBareExec(AST_Exec* node) {
        if (cur->nameForcingNodeBareExec == NULL)
            cur->nameForcingNodeBareExec = node;
    }

    bool visit_name(AST_Name* node) override {
        mangleNameInPlace(node->id, cur->private_name, scoping->getInternedStrings());

        switch (node->ctx_type) {
            case AST_TYPE::Load:
                doRead(node->id);
                break;
            case AST_TYPE::Del:
                doDel(node);
            // fallthrough
            case AST_TYPE::Param:
            case AST_TYPE::Store:
                doWrite(node->id);
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->ctx_type);
        }
        return true;
    }

    bool visit_assert(AST_Assert* node) override { return false; }
    bool visit_assign(AST_Assign* node) override { return false; }
    bool visit_augassign(AST_AugAssign* node) override { return false; }
    bool visit_attribute(AST_Attribute* node) override { return false; }
    bool visit_binop(AST_BinOp* node) override { return false; }
    bool visit_boolop(AST_BoolOp* node) override { return false; }
    bool visit_break(AST_Break* node) override { return false; }
    bool visit_call(AST_Call* node) override { return false; }
    bool visit_compare(AST_Compare* node) override { return false; }
    bool visit_comprehension(AST_comprehension* node) override { return false; }
    // bool visit_classdef(AST_ClassDef *node) override { return false; }
    bool visit_continue(AST_Continue* node) override { return false; }
    bool visit_dict(AST_Dict* node) override { return false; }
    bool visit_ellipsis(AST_Ellipsis* node) override { return false; }
    bool visit_excepthandler(AST_ExceptHandler* node) override { return false; }
    bool visit_expr(AST_Expr* node) override { return false; }
    bool visit_extslice(AST_ExtSlice* node) override { return false; }
    bool visit_for(AST_For* node) override { return false; }
    // bool visit_functiondef(AST_FunctionDef *node) override { return false; }
    // bool visit_global(AST_Global *node) override { return false; }
    bool visit_if(AST_If* node) override { return false; }
    bool visit_ifexp(AST_IfExp* node) override { return false; }
    bool visit_index(AST_Index* node) override { return false; }
    bool visit_keyword(AST_keyword* node) override { return false; }
    bool visit_list(AST_List* node) override { return false; }
    bool visit_listcomp(AST_ListComp* node) override { return false; }
    bool visit_expression(AST_Expression* node) override { return false; }
    bool visit_suite(AST_Suite* node) override { return false; }
    // bool visit_module(AST_Module *node) override { return false; }
    // bool visit_name(AST_Name *node) override { return false; }
    bool visit_num(AST_Num* node) override { return false; }
    bool visit_pass(AST_Pass* node) override { return false; }
    bool visit_print(AST_Print* node) override { return false; }
    bool visit_raise(AST_Raise* node) override { return false; }
    bool visit_repr(AST_Repr* node) override { return false; }
    bool visit_return(AST_Return* node) override { return false; }
    bool visit_set(AST_Set* node) override { return false; }
    bool visit_slice(AST_Slice* node) override { return false; }
    bool visit_str(AST_Str* node) override { return false; }
    bool visit_subscript(AST_Subscript* node) override { return false; }
    bool visit_tryexcept(AST_TryExcept* node) override { return false; }
    bool visit_tryfinally(AST_TryFinally* node) override { return false; }
    bool visit_tuple(AST_Tuple* node) override { return false; }
    bool visit_unaryop(AST_UnaryOp* node) override { return false; }
    bool visit_while(AST_While* node) override { return false; }
    bool visit_with(AST_With* node) override { return false; }
    bool visit_yield(AST_Yield* node) override { return false; }
    bool visit_delete(AST_Delete* node) override { return false; }

    bool visit_global(AST_Global* node) override {
        for (int i = 0; i < node->names.size(); i++) {
            mangleNameInPlace(node->names[i], cur->private_name, scoping->getInternedStrings());
            auto& r = cur->results[node->names[i]];
            if (r.params) {
                // Throw an exception if a name is both declared global and a parameter
                raiseGlobalAndLocalException(node->names[i], this->orig_node);
            }
            r.forced_globals = 1;
        }
        return true;
    }

    bool visit_classdef(AST_ClassDef* node) override {
        if (node == orig_node) {
            for (AST_stmt* s : node->body)
                s->accept(this);
            return true;
        } else {
            for (auto* e : node->bases)
                e->accept(this);
            for (auto* e : node->decorator_list)
                e->accept(this);

            // TODO: this is one of very few places we don't mangle in place.
            // The AST doesn't have a way of representing that the class name is the
            // unmangled name but the name it gets stored as is the mangled name.
            doWrite(mangleName(node->name, cur->private_name, scoping->getInternedStrings()));
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur, scoping);
            collect(node, map, scoping);
            return true;
        }
    }

    void visitOrignodeArgs(AST_arguments* args) {
        this->currently_visiting_functiondef_args = true;

        int counter = 0;
        for (AST_expr* e : args->args) {
            if (e->type == AST_TYPE::Tuple) {
                doWrite(scoping->getInternedStrings().get("." + std::to_string(counter)));
            }
            counter++;
            e->accept(this);
        }

        if (args->vararg) {
            mangleNameInPlace(args->vararg->id, cur->private_name, scoping->getInternedStrings());
            doWrite(args->vararg->id);
        }
        if (args->kwarg) {
            mangleNameInPlace(args->kwarg->id, cur->private_name, scoping->getInternedStrings());
            doWrite(args->kwarg->id);
        }

        this->currently_visiting_functiondef_args = false;
    }

    bool visit_functiondef(AST_FunctionDef* node) override {
        if (node == orig_node) {
            visitOrignodeArgs(node->args);

            for (AST_stmt* s : node->body)
                s->accept(this);
            return true;
        } else {
            for (auto* e : node->args->defaults)
                e->accept(this);
            for (auto* e : node->decorator_list)
                e->accept(this);

            // TODO: this is one of very few places we don't mangle in place.
            // The AST doesn't have a way of representing that the class name is the
            // unmangled name but the name it gets stored as is the mangled name.
            doWrite(mangleName(node->name, cur->private_name, scoping->getInternedStrings()));
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur, scoping);
            collect(node, map, scoping);
            return true;
        }
    }

    // helper methods for visit_{generatorexp,dictcomp,setcomp}
    void visit_comp_values(AST_GeneratorExp* node) { node->elt->accept(this); }
    void visit_comp_values(AST_SetComp* node) { node->elt->accept(this); }
    void visit_comp_values(AST_DictComp* node) {
        node->key->accept(this);
        node->value->accept(this);
    }

    template <typename CompType> bool visit_comp(CompType* node) {
        // NB. comprehensions evaluate their first for-subject's expression outside of the function scope they create.
        if (node == orig_node) {
            bool first = true;
            for (AST_comprehension* c : node->generators) {
                if (!first)
                    c->iter->accept(this);
                for (auto i : c->ifs)
                    i->accept(this);
                c->target->accept(this);
                first = false;
            }

            visit_comp_values(node);
        } else {
            node->generators[0]->iter->accept(this);
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur, scoping);
            collect(node, map, scoping);
        }
        return true;
    }

    bool visit_generatorexp(AST_GeneratorExp* node) override { return visit_comp(node); }
    bool visit_dictcomp(AST_DictComp* node) override { return visit_comp(node); }
    bool visit_setcomp(AST_SetComp* node) override { return visit_comp(node); }

    bool visit_lambda(AST_Lambda* node) override {
        if (node == orig_node) {
            visitOrignodeArgs(node->args);
            node->body->accept(this);
        } else {
            for (auto* e : node->args->defaults)
                e->accept(this);
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur, scoping);
            collect(node, map, scoping);
        }

        return true;
    }

    bool visit_import(AST_Import* node) override {
        for (int i = 0; i < node->names.size(); i++) {
            AST_alias* alias = node->names[i];
            mangleNameInPlace(alias->asname, cur->private_name, scoping->getInternedStrings());
            mangleNameInPlace(alias->name, cur->private_name, scoping->getInternedStrings());
            if (alias->asname.s().size())
                doWrite(alias->asname);
            else
                doWrite(alias->name);
        }
        return true;
    }

    bool visit_importfrom(AST_ImportFrom* node) override {
        mangleNameInPlace(node->module, cur->private_name, scoping->getInternedStrings());
        for (int i = 0; i < node->names.size(); i++) {
            AST_alias* alias = node->names[i];
            if (alias->name.s() == std::string("*")) {
                mangleNameInPlace(alias->asname, cur->private_name, scoping->getInternedStrings());
                doImportStar(node);
            } else {
                mangleNameInPlace(alias->asname, cur->private_name, scoping->getInternedStrings());
                mangleNameInPlace(alias->name, cur->private_name, scoping->getInternedStrings());
                if (alias->asname.s().size())
                    doWrite(alias->asname);
                else
                    doWrite(alias->name);
            }
        }
        return true;
    }

    bool visit_exec(AST_Exec* node) override {
        if (node->globals == NULL) {
            doBareExec(node);
        }
        return false;
    }

    static void collect(AST* node, ScopingAnalysis::NameUsageMap* map, ScopingAnalysis* scoping) {
        assert(map);
        assert(map->count(node));

        NameCollectorVisitor vis(node, map, scoping);
        node->accept(&vis);
    }
};

static std::vector<ScopingAnalysis::ScopeNameUsage*> sortNameUsages(ScopingAnalysis::NameUsageMap* usages) {
    std::vector<ScopingAnalysis::ScopeNameUsage*> rtn;
    std::unordered_set<ScopingAnalysis::ScopeNameUsage*> added;

    for (const auto& p : *usages) {
        ScopingAnalysis::ScopeNameUsage* usage = p.second;

        std::vector<ScopingAnalysis::ScopeNameUsage*> traversed;

        while (usage && added.count(usage) == 0) {
            traversed.push_back(usage);
            usage = usage->parent;
        }

        for (int i = traversed.size() - 1; i >= 0; i--) {
            rtn.push_back(traversed[i]);
            added.insert(traversed[i]);
        }
    }

    assert(rtn.size() == usages->size());

    return rtn;
}

static void raiseNameForcingSyntaxError(const char* msg, ScopingAnalysis::ScopeNameUsage* usage) {
    assert(usage->node->type == AST_TYPE::FunctionDef);

    AST_FunctionDef* funcNode = static_cast<AST_FunctionDef*>(usage->node);
    int lineno;

    const char* syntaxElemMsg;
    if (usage->nameForcingNodeImportStar && usage->nameForcingNodeBareExec) {
        syntaxElemMsg = "function '%s' uses import * and bare exec, which are illegal because it %s";
        lineno = std::min(usage->nameForcingNodeImportStar->lineno, usage->nameForcingNodeBareExec->lineno);
    } else if (usage->nameForcingNodeImportStar) {
        syntaxElemMsg = "import * is not allowed in function '%s' because it %s";
        lineno = usage->nameForcingNodeImportStar->lineno;
    } else {
        if (PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION == 7 && PY_MICRO_VERSION < 8)
            syntaxElemMsg = "unqualified exec is not allowed in function '%.100s' it %s";
        else
            syntaxElemMsg = "unqualified exec is not allowed in function '%.100s' because it %s";
        lineno = usage->nameForcingNodeBareExec->lineno;
    }

    char buf[1024];
    snprintf(buf, sizeof(buf), syntaxElemMsg, funcNode->name.c_str(), msg);
    raiseSyntaxError(buf, lineno, 0, "" /* file?? */, funcNode->name.s());
}

void ScopingAnalysis::processNameUsages(ScopingAnalysis::NameUsageMap* usages) {
    // Resolve name lookups:
    for (const auto& p : *usages) {
        ScopeNameUsage* usage = p.second;

        bool is_any_name_free = false;

        for (auto&& r : usage->results) {
            if (!r.second.read)
                continue;
            if (r.second.forced_globals)
                continue;
            if (r.second.written)
                continue;

            auto&& name = r.first;

            bool is_name_free = true;

            std::vector<ScopeNameUsage*> intermediate_parents;

            ScopeNameUsage* parent = usage->parent;
            while (parent) {
                auto parent_result = parent->results.find(name);
                bool found_parent = parent_result != parent->results.end();
                if (parent->node->type == AST_TYPE::ClassDef) {
                    intermediate_parents.push_back(parent);
                    parent = parent->parent;
                } else if (found_parent && parent_result->second.forced_globals) {
                    is_name_free = false;
                    break;
                } else if (found_parent && parent_result->second.written) {
                    r.second.got_from_closure = 1;
                    parent_result->second.referenced_from_nested = 1;

                    for (ScopeNameUsage* iparent : intermediate_parents) {
                        iparent->results[name].passthrough_accesses = 1;
                    }

                    break;
                } else {
                    intermediate_parents.push_back(parent);
                    parent = parent->parent;
                }
            }

            if (is_name_free)
                is_any_name_free = true;
        }

        if (is_any_name_free) {
            // This intentionally loops through *all* parents, not just the ones in intermediate_parents
            // Label any parent FunctionDef as `child_free`, and if such a parent exists, also label
            // this node as `free` itself.
            for (ScopeNameUsage* parent = usage->parent; parent != NULL; parent = parent->parent) {
                if (parent->node->type == AST_TYPE::FunctionDef) {
                    usage->free = true;
                    parent->child_free = true;
                }
            }
        }
    }

    for (const auto& p : *usages) {
        ScopeNameUsage* usage = p.second;
        if (usage->hasNameForcingSyntax()) {
            if (usage->child_free)
                raiseNameForcingSyntaxError("contains a nested function with free variables", usage);
            else if (usage->free)
                raiseNameForcingSyntaxError("is a nested function", usage);
        }

        // Trying to `del` a varaible in the closure in a SyntaxError.
        // NOTE(travis): I'm not sure why this is a syntax error;
        // it doesn't seem like there is anything intrinisically difficult about supporting
        // `del` for closure variables. But it is, so, there you go:
        for (AST_Name* name_node : usage->del_name_nodes) {
            InternedString name = name_node->id;
            if (usage->results.count(name) && usage->results[name].referenced_from_nested) {
                char buf[1024];
                snprintf(buf, sizeof(buf), "can not delete variable '%s' referenced in nested scope", name.c_str());
                assert(usage->node->type == AST_TYPE::FunctionDef);
                AST_FunctionDef* funcNode = static_cast<AST_FunctionDef*>(usage->node);
                raiseSyntaxError(buf, name_node->lineno, 0, "" /* file?? */, funcNode->name);
            }
        }
    }

    std::vector<ScopeNameUsage*> sorted_usages = sortNameUsages(usages);

    // Construct the public-facing ScopeInfo's from the analyzed data:
    for (int i = 0; i < sorted_usages.size(); i++) {
        ScopeNameUsage* usage = sorted_usages[i];
        AST* node = usage->node;

        ScopeInfo* parent_info
            = this->scopes[(usage->parent == NULL) ? this->parent_module : usage->parent->node].get();

        switch (node->type) {
            case AST_TYPE::ClassDef: {
                this->scopes[node] = llvm::make_unique<ScopeInfoBase>(parent_info, usage, usage->node,
                                                                      true /* usesNameLookup */, globals_from_module);
                break;
            }
            case AST_TYPE::FunctionDef:
            case AST_TYPE::Lambda:
            case AST_TYPE::GeneratorExp:
            case AST_TYPE::DictComp:
            case AST_TYPE::SetComp: {
                this->scopes[node] = llvm::make_unique<ScopeInfoBase>(
                    parent_info, usage, usage->node, usage->hasNameForcingSyntax() /* usesNameLookup */,
                    globals_from_module);
                break;
            }
            default:
                RELEASE_ASSERT(0, "%d", usage->node->type);
                break;
        }
    }
}

InternedStringPool& ScopingAnalysis::getInternedStrings() {
    return *interned_strings;
}

void ScopingAnalysis::analyzeSubtree(AST* node) {
    NameUsageMap usages;
    usages[node] = new ScopeNameUsage(node, NULL, this);
    NameCollectorVisitor::collect(node, &usages, this);

    processNameUsages(&usages);
}

ScopeInfo* ScopingAnalysis::getScopeInfoForNode(AST* node) {
    assert(node);

    if (!scopes.count(node))
        analyzeSubtree(node);

    assert(scopes.count(node));
    return scopes[node].get();
}

ScopingAnalysis::ScopingAnalysis(AST* ast, bool globals_from_module)
    : parent_module(NULL), globals_from_module(globals_from_module) {
    switch (ast->type) {
        case AST_TYPE::Module:
            interned_strings = static_cast<AST_Module*>(ast)->interned_strings.get();
            break;
        case AST_TYPE::Expression:
            interned_strings = static_cast<AST_Expression*>(ast)->interned_strings.get();
            break;
        case AST_TYPE::Suite:
            interned_strings = static_cast<AST_Suite*>(ast)->interned_strings.get();
            break;
        default:
            RELEASE_ASSERT(0, "%d", ast->type);
    }

    if (globals_from_module) {
        assert(ast->type == AST_TYPE::Module);
        scopes[ast] = llvm::make_unique<ModuleScopeInfo>();
        parent_module = static_cast<AST_Module*>(ast);
    } else {
        scopes[ast] = llvm::make_unique<EvalExprScopeInfo>(ast, globals_from_module);
    }
}
}
