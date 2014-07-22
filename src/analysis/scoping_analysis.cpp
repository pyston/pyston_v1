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

#include "analysis/scoping_analysis.h"

#include "core/ast.h"
#include "core/common.h"
#include "core/util.h"

namespace pyston {

static bool isCompilerCreatedName(const std::string& name) {
    return name[0] == '!' || name[0] == '#';
}

class ModuleScopeInfo : public ScopeInfo {
public:
    virtual ScopeInfo* getParent() { return NULL; }

    virtual bool createsClosure() { return false; }

    virtual bool takesClosure() { return false; }

    bool passesThroughClosure() override { return false; }

    virtual bool refersToGlobal(const std::string& name) {
        if (isCompilerCreatedName(name))
            return false;

        // assert(name[0] != '#' && "should test this");
        return true;
    }
    virtual bool refersToClosure(const std::string name) { return false; }
    virtual bool saveInClosure(const std::string name) { return false; }

    virtual const std::unordered_set<std::string>& getClassDefLocalNames() { RELEASE_ASSERT(0, ""); }
};

struct ScopingAnalysis::ScopeNameUsage {
    AST* node;
    ScopeNameUsage* parent;

    typedef std::unordered_set<std::string> StrSet;

    // Properties determined from crawling the scope:
    StrSet read;
    StrSet written;
    StrSet forced_globals;

    // Properties determined by looking at other scopes as well:
    StrSet referenced_from_nested;
    StrSet got_from_closure;
    StrSet passthrough_accesses; // what names a child scope accesses a name from a parent scope

    ScopeNameUsage(AST* node, ScopeNameUsage* parent) : node(node), parent(parent) {
        if (node->type == AST_TYPE::ClassDef) {
            AST_ClassDef* classdef = ast_cast<AST_ClassDef>(node);

            // classes have an implicit write to "__module__"
            written.insert("__module__");

            if (classdef->body.size() && classdef->body[0]->type == AST_TYPE::Expr) {
                AST_Expr* first_expr = ast_cast<AST_Expr>(classdef->body[0]);
                if (first_expr->value->type == AST_TYPE::Str) {
                    written.insert("__doc__");
                }
            }
        }
    }
};

class ScopeInfoBase : public ScopeInfo {
private:
    ScopeInfo* parent;
    ScopingAnalysis::ScopeNameUsage* usage;

public:
    ScopeInfoBase(ScopeInfo* parent, ScopingAnalysis::ScopeNameUsage* usage) : parent(parent), usage(usage) {
        assert(parent);
        assert(usage);
    }

    virtual ~ScopeInfoBase() { delete this->usage; }

    virtual ScopeInfo* getParent() { return parent; }

    virtual bool createsClosure() { return usage->referenced_from_nested.size() > 0; }

    virtual bool takesClosure() { return usage->got_from_closure.size() > 0 || usage->passthrough_accesses.size() > 0; }

    bool passesThroughClosure() override { return usage->passthrough_accesses.size() > 0 && !createsClosure(); }

    virtual bool refersToGlobal(const std::string& name) {
        // HAX
        if (isCompilerCreatedName(name))
            return false;

        if (usage->forced_globals.count(name))
            return true;
        return usage->written.count(name) == 0 && usage->got_from_closure.count(name) == 0;
    }
    virtual bool refersToClosure(const std::string name) {
        // HAX
        if (isCompilerCreatedName(name))
            return false;
        return usage->got_from_closure.count(name) != 0;
    }
    virtual bool saveInClosure(const std::string name) {
        // HAX
        if (isCompilerCreatedName(name))
            return false;
        return usage->referenced_from_nested.count(name) != 0;
    }

    virtual const std::unordered_set<std::string>& getClassDefLocalNames() {
        RELEASE_ASSERT(usage->node->type == AST_TYPE::ClassDef, "");
        return usage->written;
    }
};

class NameCollectorVisitor : public ASTVisitor {
private:
    AST* orig_node;
    ScopingAnalysis::NameUsageMap* map;
    ScopingAnalysis::ScopeNameUsage* cur;
    NameCollectorVisitor(AST* node, ScopingAnalysis::NameUsageMap* map) : orig_node(node), map(map) {
        assert(map);
        cur = (*map)[node];
        assert(cur);
    }

public:
    void doWrite(const std::string& name) {
        cur->read.insert(name);
        cur->written.insert(name);
    }

    void doRead(const std::string& name) { cur->read.insert(name); }

    virtual bool visit_name(AST_Name* node) {
        switch (node->ctx_type) {
            case AST_TYPE::Load:
                doRead(node->id);
                break;
            case AST_TYPE::Param:
            case AST_TYPE::Store:
                doWrite(node->id);
                break;
            default:
                RELEASE_ASSERT(0, "%d", node->ctx_type);
        }
        return true;
    }

    virtual bool visit_assert(AST_Assert* node) { return false; }
    virtual bool visit_assign(AST_Assign* node) { return false; }
    virtual bool visit_augassign(AST_AugAssign* node) { return false; }
    virtual bool visit_attribute(AST_Attribute* node) { return false; }
    virtual bool visit_binop(AST_BinOp* node) { return false; }
    virtual bool visit_boolop(AST_BoolOp* node) { return false; }
    virtual bool visit_break(AST_Break* node) { return false; }
    virtual bool visit_call(AST_Call* node) { return false; }
    virtual bool visit_compare(AST_Compare* node) { return false; }
    virtual bool visit_comprehension(AST_comprehension* node) { return false; }
    // virtual bool visit_classdef(AST_ClassDef *node) { return false; }
    virtual bool visit_continue(AST_Continue* node) { return false; }
    virtual bool visit_dict(AST_Dict* node) { return false; }
    virtual bool visit_dictcomp(AST_DictComp* node) { return false; }
    virtual bool visit_excepthandler(AST_ExceptHandler* node) { return false; }
    virtual bool visit_expr(AST_Expr* node) { return false; }
    virtual bool visit_for(AST_For* node) { return false; }
    // virtual bool visit_functiondef(AST_FunctionDef *node) { return false; }
    // virtual bool visit_global(AST_Global *node) { return false; }
    virtual bool visit_if(AST_If* node) { return false; }
    virtual bool visit_ifexp(AST_IfExp* node) { return false; }
    virtual bool visit_index(AST_Index* node) { return false; }
    virtual bool visit_keyword(AST_keyword* node) { return false; }
    virtual bool visit_list(AST_List* node) { return false; }
    virtual bool visit_listcomp(AST_ListComp* node) { return false; }
    // virtual bool visit_module(AST_Module *node) { return false; }
    // virtual bool visit_name(AST_Name *node) { return false; }
    virtual bool visit_num(AST_Num* node) { return false; }
    virtual bool visit_pass(AST_Pass* node) { return false; }
    virtual bool visit_print(AST_Print* node) { return false; }
    virtual bool visit_raise(AST_Raise* node) { return false; }
    virtual bool visit_repr(AST_Repr* node) { return false; }
    virtual bool visit_return(AST_Return* node) { return false; }
    virtual bool visit_slice(AST_Slice* node) { return false; }
    virtual bool visit_str(AST_Str* node) { return false; }
    virtual bool visit_subscript(AST_Subscript* node) { return false; }
    virtual bool visit_tryexcept(AST_TryExcept* node) { return false; }
    virtual bool visit_tryfinally(AST_TryFinally* node) { return false; }
    virtual bool visit_tuple(AST_Tuple* node) { return false; }
    virtual bool visit_unaryop(AST_UnaryOp* node) { return false; }
    virtual bool visit_while(AST_While* node) { return false; }
    virtual bool visit_with(AST_With* node) { return false; }

    virtual bool visit_branch(AST_Branch* node) { return false; }
    virtual bool visit_jump(AST_Jump* node) { return false; }


    virtual bool visit_delete(AST_Delete* node) {
        for (auto t : node->targets) {
            RELEASE_ASSERT(t->type != AST_TYPE::Name, "");
        }
        return false;
    }

    virtual bool visit_global(AST_Global* node) {
        for (int i = 0; i < node->names.size(); i++) {
            const std::string& name = node->names[i];
            cur->forced_globals.insert(name);
        }
        return true;
    }

    virtual bool visit_classdef(AST_ClassDef* node) {
        if (node == orig_node) {
            for (AST_stmt* s : node->body)
                s->accept(this);
            return true;
        } else {
            for (auto* e : node->bases)
                e->accept(this);
            for (auto* e : node->decorator_list)
                e->accept(this);

            doWrite(node->name);
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur);
            collect(node, map);
            return true;
        }
    }

    virtual bool visit_functiondef(AST_FunctionDef* node) {
        if (node == orig_node) {
            for (AST_expr* e : node->args->args)
                e->accept(this);
            if (node->args->vararg.size())
                doWrite(node->args->vararg);
            if (node->args->kwarg.size())
                doWrite(node->args->kwarg);
            for (AST_stmt* s : node->body)
                s->accept(this);
            return true;
        } else {
            for (auto* e : node->args->defaults)
                e->accept(this);
            for (auto* e : node->decorator_list)
                e->accept(this);

            doWrite(node->name);
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur);
            collect(node, map);
            return true;
        }
    }

    virtual bool visit_lambda(AST_Lambda* node) {
        if (node == orig_node) {
            for (AST_expr* e : node->args->args)
                e->accept(this);
            if (node->args->vararg.size())
                doWrite(node->args->vararg);
            if (node->args->kwarg.size())
                doWrite(node->args->kwarg);
            node->body->accept(this);
        } else {
            for (auto* e : node->args->defaults)
                e->accept(this);
            (*map)[node] = new ScopingAnalysis::ScopeNameUsage(node, cur);
            collect(node, map);
        }

        return true;
    }

    virtual bool visit_import(AST_Import* node) {
        for (int i = 0; i < node->names.size(); i++) {
            AST_alias* alias = node->names[i];
            if (alias->asname.size())
                doWrite(alias->asname);
            else
                doWrite(alias->name);
        }
        return true;
    }

    static void collect(AST* node, ScopingAnalysis::NameUsageMap* map) {
        assert(map);
        assert(map->count(node));

        NameCollectorVisitor vis(node, map);
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
            case AST_TYPE::ClassDef:
            case AST_TYPE::FunctionDef:
            case AST_TYPE::Lambda:
                this->scopes[node] = new ScopeInfoBase(parent_info, usage);
                break;
            default:
                RELEASE_ASSERT(0, "%d", usage->node->type);
                break;
        }
    }
}

ScopeInfo* ScopingAnalysis::analyzeSubtree(AST* node) {
    NameUsageMap usages;
    usages[node] = new ScopeNameUsage(node, NULL);
    NameCollectorVisitor::collect(node, &usages);

    processNameUsages(&usages);

    ScopeInfo* rtn = scopes[node];
    assert(rtn);
    return rtn;
}

ScopeInfo* ScopingAnalysis::getScopeInfoForNode(AST* node) {
    assert(node);

    ScopeInfo* rtn = scopes[node];
    if (rtn)
        return rtn;

    switch (node->type) {
        case AST_TYPE::ClassDef:
        case AST_TYPE::FunctionDef:
        case AST_TYPE::Lambda:
            return analyzeSubtree(node);
        // this is handled in the constructor:
        // case AST_TYPE::Module:
        // return new ModuleScopeInfo();
        default:
            RELEASE_ASSERT(0, "%d", node->type);
    }
}

ScopingAnalysis::ScopingAnalysis(AST_Module* m) : parent_module(m) {
    scopes[m] = new ModuleScopeInfo();
}

ScopingAnalysis* runScopingAnalysis(AST_Module* m) {
    return new ScopingAnalysis(m);
}
}
