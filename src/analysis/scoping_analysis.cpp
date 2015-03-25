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

#include "analysis/scoping_analysis.h"

#include "llvm/ADT/DenseSet.h"

#include "core/ast.h"
#include "core/common.h"
#include "core/util.h"

namespace pyston {

class YieldVisitor : public NoopASTVisitor {
public:
    YieldVisitor() : containsYield(false) {}

    bool visit_functiondef(AST_FunctionDef*) override { return true; }

    bool visit_yield(AST_Yield*) override {
        containsYield = true;
        return true;
    }

    bool containsYield;
};

bool containsYield(AST* ast) {
    YieldVisitor visitor;
    if (ast->type == AST_TYPE::FunctionDef) {
        AST_FunctionDef* funcDef = static_cast<AST_FunctionDef*>(ast);
        for (auto& e : funcDef->body) {
            e->accept(&visitor);
            if (visitor.containsYield)
                return true;
        }
    } else {
        ast->accept(&visitor);
    }
    return visitor.containsYield;
}

static void mangleNameInPlace(InternedString& id, const std::string* private_name,
                              InternedStringPool& interned_strings) {
    if (!private_name)
        return;

    int len = id.str().size();
    if (len < 2 || id.str()[0] != '_' || id.str()[1] != '_')
        return;

    if ((id.str()[len - 2] == '_' && id.str()[len - 1] == '_') || id.str().find('.') != std::string::npos)
        return;

    const char* p = private_name->c_str();
    while (*p == '_') {
        p++;
        len--;
    }
    if (*p == '\0')
        return;

    id = interned_strings.get("_" + (p + id.str()));
}

static InternedString mangleName(InternedString id, const std::string* private_name,
                                 InternedStringPool& interned_strings) {
    InternedString rtn(id);
    mangleNameInPlace(rtn, private_name, interned_strings);
    return rtn;
}

static bool isCompilerCreatedName(InternedString name) {
    return name.str()[0] == '!' || name.str()[0] == '#';
}

class ModuleScopeInfo : public ScopeInfo {
public:
    ScopeInfo* getParent() override { return NULL; }

    bool createsClosure() override { return false; }

    bool takesClosure() override { return false; }

    bool passesThroughClosure() override { return false; }

    bool refersToGlobal(InternedString name) override {
        if (isCompilerCreatedName(name))
            return false;

        // assert(name[0] != '#' && "should test this");
        return true;
    }
    bool refersToClosure(InternedString name) override { return false; }
    bool saveInClosure(InternedString name) override { return false; }
    VarScopeType getScopeTypeOfName(InternedString name) override {
        return refersToGlobal(name) ? VarScopeType::GLOBAL : VarScopeType::FAST;
    }

    InternedString mangleName(InternedString id) override { return id; }
    InternedString internString(llvm::StringRef s) override { abort(); }
};

struct ScopingAnalysis::ScopeNameUsage {
    AST* node;
    ScopeNameUsage* parent;
    const std::string* private_name;
    ScopingAnalysis* scoping;

    typedef llvm::DenseSet<InternedString> StrSet;

    // Properties determined from crawling the scope:
    StrSet read;
    StrSet written;
    StrSet forced_globals;

    // Properties determined by looking at other scopes as well:
    StrSet referenced_from_nested;
    StrSet got_from_closure;
    StrSet passthrough_accesses; // what names a child scope accesses a name from a parent scope

    ScopeNameUsage(AST* node, ScopeNameUsage* parent, ScopingAnalysis* scoping)
        : node(node), parent(parent), scoping(scoping) {
        if (node->type == AST_TYPE::ClassDef) {
            AST_ClassDef* classdef = ast_cast<AST_ClassDef>(node);

            // classes have an implicit write to "__module__"
            written.insert(scoping->getInternedStrings().get("__module__"));

            if (classdef->body.size() && classdef->body[0]->type == AST_TYPE::Expr) {
                AST_Expr* first_expr = ast_cast<AST_Expr>(classdef->body[0]);
                if (first_expr->value->type == AST_TYPE::Str) {
                    written.insert(scoping->getInternedStrings().get("__doc__"));
                }
            }
        }

        if (node->type == AST_TYPE::ClassDef) {
            private_name = &ast_cast<AST_ClassDef>(node)->name.str();
        } else if (parent) {
            private_name = parent->private_name;
        } else {
            private_name = NULL;
        }
    }

    void dump() {
#define DUMP(n)                                                                                                        \
    printf(STRINGIFY(n) ":\n");                                                                                        \
    for (auto s : n) {                                                                                                 \
        printf("%s\n", s.c_str());                                                                                     \
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
    bool usesNameLookup;

public:
    ScopeInfoBase(ScopeInfo* parent, ScopingAnalysis::ScopeNameUsage* usage, AST* ast, bool usesNameLookup)
        : parent(parent), usage(usage), ast(ast), usesNameLookup(usesNameLookup) {
        // not true anymore: Expression
        // assert(parent);
        assert(usage);
        assert(ast);
    }

    ~ScopeInfoBase() override { delete this->usage; }

    ScopeInfo* getParent() override { return parent; }

    bool createsClosure() override { return usage->referenced_from_nested.size() > 0; }

    bool takesClosure() override {
        return usage->got_from_closure.size() > 0 || usage->passthrough_accesses.size() > 0;
    }

    bool passesThroughClosure() override { return usage->passthrough_accesses.size() > 0 && !createsClosure(); }

    bool refersToGlobal(InternedString name) override {
        // HAX
        if (isCompilerCreatedName(name))
            return false;

        if (usage->forced_globals.count(name))
            return true;
        if (usesNameLookup)
            return false;
        return usage->written.count(name) == 0 && usage->got_from_closure.count(name) == 0;
    }
    bool refersToClosure(InternedString name) override {
        // HAX
        if (isCompilerCreatedName(name))
            return false;
        return usage->got_from_closure.count(name) != 0;
    }
    bool saveInClosure(InternedString name) override {
        // HAX
        if (isCompilerCreatedName(name) || usesNameLookup)
            return false;
        return usage->referenced_from_nested.count(name) != 0;
    }

    VarScopeType getScopeTypeOfName(InternedString name) override {
        // HAX
        if (isCompilerCreatedName(name))
            return VarScopeType::FAST;
        if (refersToClosure(name))
            return VarScopeType::DEREF;
        if (refersToGlobal(name))
            return VarScopeType::GLOBAL;
        if (saveInClosure(name))
            return VarScopeType::CLOSURE;
        if (usesNameLookup)
            return VarScopeType::NAME;
        return VarScopeType::FAST;
    }

    InternedString mangleName(const InternedString id) override {
        return pyston::mangleName(id, usage->private_name, usage->scoping->getInternedStrings());
    }

    InternedString internString(llvm::StringRef s) override { return usage->scoping->getInternedStrings().get(s); }
};

class NameCollectorVisitor : public ASTVisitor {
private:
    AST* orig_node;
    ScopingAnalysis::NameUsageMap* map;
    ScopingAnalysis::ScopeNameUsage* cur;
    ScopingAnalysis* scoping;
    NameCollectorVisitor(AST* node, ScopingAnalysis::NameUsageMap* map, ScopingAnalysis* scoping)
        : orig_node(node), map(map), scoping(scoping) {
        assert(map);
        cur = (*map)[node];
        assert(cur);
    }

public:
    void doWrite(InternedString name) {
        assert(name == mangleName(name, cur->private_name, scoping->getInternedStrings()));
        cur->read.insert(name);
        cur->written.insert(name);
    }

    void doRead(InternedString name) {
        assert(name == mangleName(name, cur->private_name, scoping->getInternedStrings()));
        cur->read.insert(name);
    }

    bool visit_name(AST_Name* node) override {
        mangleNameInPlace(node->id, cur->private_name, scoping->getInternedStrings());

        switch (node->ctx_type) {
            case AST_TYPE::Load:
                doRead(node->id);
                break;
            case AST_TYPE::Param:
            case AST_TYPE::Store:
            case AST_TYPE::Del:
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
    bool visit_excepthandler(AST_ExceptHandler* node) override { return false; }
    bool visit_expr(AST_Expr* node) override { return false; }
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
    bool visit_branch(AST_Branch* node) override { return false; }
    bool visit_jump(AST_Jump* node) override { return false; }
    bool visit_delete(AST_Delete* node) override { return false; }

    bool visit_global(AST_Global* node) override {
        for (int i = 0; i < node->names.size(); i++) {
            mangleNameInPlace(node->names[i], cur->private_name, scoping->getInternedStrings());
            cur->forced_globals.insert(node->names[i]);
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

    bool visit_functiondef(AST_FunctionDef* node) override {
        if (node == orig_node) {
            for (AST_expr* e : node->args->args)
                e->accept(this);
            if (node->args->vararg.str().size()) {
                mangleNameInPlace(node->args->vararg, cur->private_name, scoping->getInternedStrings());
                doWrite(node->args->vararg);
            }
            if (node->args->kwarg.str().size()) {
                mangleNameInPlace(node->args->kwarg, cur->private_name, scoping->getInternedStrings());
                doWrite(node->args->kwarg);
            }
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
            for (AST_expr* e : node->args->args)
                e->accept(this);
            if (node->args->vararg.str().size()) {
                mangleNameInPlace(node->args->vararg, cur->private_name, scoping->getInternedStrings());
                doWrite(node->args->vararg);
            }
            if (node->args->kwarg.str().size()) {
                mangleNameInPlace(node->args->kwarg, cur->private_name, scoping->getInternedStrings());
                doWrite(node->args->kwarg);
            }
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
            if (alias->asname.str().size())
                doWrite(alias->asname);
            else
                doWrite(alias->name);
        }
        return true;
    }

    bool visit_importfrom(AST_ImportFrom* node) override {
        for (int i = 0; i < node->names.size(); i++) {
            AST_alias* alias = node->names[i];
            mangleNameInPlace(alias->asname, cur->private_name, scoping->getInternedStrings());
            mangleNameInPlace(alias->name, cur->private_name, scoping->getInternedStrings());
            if (alias->asname.str().size())
                doWrite(alias->asname);
            else
                doWrite(alias->name);
        }
        return true;
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

void ScopingAnalysis::processNameUsages(ScopingAnalysis::NameUsageMap* usages) {
    // Resolve name lookups:
    for (const auto& p : *usages) {
        ScopeNameUsage* usage = p.second;
        for (const auto& name : usage->read) {
            if (usage->forced_globals.count(name))
                continue;
            if (usage->written.count(name))
                continue;

            std::vector<ScopeNameUsage*> intermediate_parents;

            ScopeNameUsage* parent = usage->parent;
            while (parent) {
                if (parent->node->type == AST_TYPE::ClassDef) {
                    intermediate_parents.push_back(parent);
                    parent = parent->parent;
                } else if (parent->forced_globals.count(name)) {
                    break;
                } else if (parent->written.count(name)) {
                    usage->got_from_closure.insert(name);
                    parent->referenced_from_nested.insert(name);

                    for (ScopeNameUsage* iparent : intermediate_parents) {
                        iparent->passthrough_accesses.insert(name);
                    }

                    break;
                } else {
                    intermediate_parents.push_back(parent);
                    parent = parent->parent;
                }
            }
        }
    }


    std::vector<ScopeNameUsage*> sorted_usages = sortNameUsages(usages);

    // Construct the public-facing ScopeInfo's from the analyzed data:
    for (int i = 0; i < sorted_usages.size(); i++) {
        ScopeNameUsage* usage = sorted_usages[i];
        AST* node = usage->node;

        ScopeInfo* parent_info = this->scopes[(usage->parent == NULL) ? this->parent_module : usage->parent->node];

        switch (node->type) {
            case AST_TYPE::Expression:
            case AST_TYPE::ClassDef: {
                ScopeInfoBase* scopeInfo
                    = new ScopeInfoBase(parent_info, usage, usage->node, true /* usesNameLookup */);
                this->scopes[node] = scopeInfo;
                break;
            }
            case AST_TYPE::FunctionDef:
            case AST_TYPE::Lambda:
            case AST_TYPE::GeneratorExp:
            case AST_TYPE::DictComp:
            case AST_TYPE::SetComp: {
                ScopeInfoBase* scopeInfo
                    = new ScopeInfoBase(parent_info, usage, usage->node, false /* usesNameLookup */);
                this->scopes[node] = scopeInfo;
                break;
            }
            default:
                RELEASE_ASSERT(0, "%d", usage->node->type);
                break;
        }
    }
}

InternedStringPool& ScopingAnalysis::getInternedStrings() {
    return interned_strings;
}

ScopeInfo* ScopingAnalysis::analyzeSubtree(AST* node) {
    NameUsageMap usages;
    usages[node] = new ScopeNameUsage(node, NULL, this);
    NameCollectorVisitor::collect(node, &usages, this);

    processNameUsages(&usages);

    ScopeInfo* rtn = scopes[node];
    assert(rtn);
    return rtn;
}

void ScopingAnalysis::registerScopeReplacement(AST* original_node, AST* new_node) {
    assert(scope_replacements.count(original_node) == 0);
    assert(scope_replacements.count(new_node) == 0);
    assert(scopes.count(new_node) == 0);

#ifndef NDEBUG
    // NULL this out just to make sure it doesn't get accessed:
    scopes[new_node] = NULL;
#endif

    scope_replacements[new_node] = original_node;
}

ScopeInfo* ScopingAnalysis::getScopeInfoForNode(AST* node) {
    assert(node);

    auto it = scope_replacements.find(node);
    if (it != scope_replacements.end())
        node = it->second;

    auto rtn = scopes.find(node);
    if (rtn != scopes.end()) {
        assert(rtn->second);
        return rtn->second;
    }

    return analyzeSubtree(node);
}

ScopingAnalysis::ScopingAnalysis(AST_Module* m) : parent_module(m), interned_strings(*m->interned_strings.get()) {
    scopes[m] = new ModuleScopeInfo();
}

ScopingAnalysis* runScopingAnalysis(AST_Module* m) {
    return new ScopingAnalysis(m);
}

ScopingAnalysis::ScopingAnalysis(AST_Expression* e) : interned_strings(*e->interned_strings.get()) {
    auto scope_info = getScopeInfoForNode(e);
    scopes[e] = scope_info;
}
}
