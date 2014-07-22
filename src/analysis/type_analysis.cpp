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

#include "analysis/type_analysis.h"

#include <cstdio>
#include <deque>
#include <unordered_set>

#include "analysis/fpc.h"
#include "analysis/scoping_analysis.h"
#include "codegen/type_recording.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/options.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

class NullTypeAnalysis : public TypeAnalysis {
public:
    virtual ConcreteCompilerType* getTypeAtBlockStart(const std::string& name, CFGBlock* block);
    virtual ConcreteCompilerType* getTypeAtBlockEnd(const std::string& name, CFGBlock* block);
};

ConcreteCompilerType* NullTypeAnalysis::getTypeAtBlockStart(const std::string& name, CFGBlock* block) {
    return UNKNOWN;
}

ConcreteCompilerType* NullTypeAnalysis::getTypeAtBlockEnd(const std::string& name, CFGBlock* block) {
    assert(block->successors.size() > 0);
    return getTypeAtBlockStart(name, block->successors[0]);
}


static ConcreteCompilerType* unboxedType(ConcreteCompilerType* t) {
    if (t == BOXED_INT)
        return INT;
    if (t == BOXED_FLOAT)
        return FLOAT;
    if (t == BOXED_BOOL)
        return BOOL;
    return t;
}

static BoxedClass* simpleCallSpeculation(AST_Call* node, CompilerType* rtn_type, std::vector<CompilerType*> arg_types) {
    if (rtn_type->getConcreteType()->llvmType() != g.llvm_value_type_ptr) {
        // printf("Not right shape; it's %s\n", rtn_type->debugName().c_str());
        return NULL;
    }

    if (node->func->type == AST_TYPE::Name && ast_cast<AST_Name>(node->func)->id == "xrange")
        return xrange_cls;

    // if (node->func->type == AST_TYPE::Attribute && ast_cast<AST_Attribute>(node->func)->attr == "dot")
    // return float_cls;

    return NULL;
}

typedef std::unordered_map<std::string, CompilerType*> TypeMap;
typedef std::unordered_map<CFGBlock*, TypeMap> AllTypeMap;
typedef std::unordered_map<AST_expr*, CompilerType*> ExprTypeMap;
typedef std::unordered_map<AST_expr*, BoxedClass*> TypeSpeculations;
class BasicBlockTypePropagator : public ExprVisitor, public StmtVisitor {
private:
    static const bool EXPAND_UNNEEDED = true;

    CFGBlock* block;
    TypeMap& sym_table;
    ExprTypeMap& expr_types;
    TypeSpeculations& type_speculations;
    TypeAnalysis::SpeculationLevel speculation;
    ScopeInfo* scope_info;

    BasicBlockTypePropagator(CFGBlock* block, TypeMap& initial, ExprTypeMap& expr_types,
                             TypeSpeculations& type_speculations, TypeAnalysis::SpeculationLevel speculation,
                             ScopeInfo* scope_info)
        : block(block), sym_table(initial), expr_types(expr_types), type_speculations(type_speculations),
          speculation(speculation), scope_info(scope_info) {}

    void run() {
        for (int i = 0; i < block->body.size(); i++) {
            block->body[i]->accept_stmt(this);
        }
    }

    CompilerType* processSpeculation(BoxedClass* speculated_cls, AST_expr* node, CompilerType* old_type) {
        assert(old_type);
        assert(speculation != TypeAnalysis::NONE);

        if (speculated_cls != NULL && speculated_cls->is_constant) {
            ConcreteCompilerType* speculated_type = unboxedType(typeFromClass(speculated_cls));
            if (VERBOSITY() >= 2) {
                printf("in propagator, speculating that %s would actually be %s, at:\n", old_type->debugName().c_str(),
                       speculated_type->debugName().c_str());
                print_ast(node);
                printf("\n");
            }

            if (!old_type->canConvertTo(speculated_type)) {
                type_speculations[node] = speculated_cls;
                return speculated_type;
            }
        }
        return old_type;
    }

    CompilerType* getType(AST_expr* node) {
        type_speculations.erase(node);

        void* raw_rtn = node->accept_expr(this);
        CompilerType* rtn = static_cast<CompilerType*>(raw_rtn);

        if (VERBOSITY() >= 2) {
            print_ast(node);
            printf(" %s\n", rtn->debugName().c_str());
        }

        expr_types[node] = rtn;
        return rtn;
    }

    void _doSet(std::string target, CompilerType* t) {
        if (t)
            sym_table[target] = t;
    }

    void _doSet(AST_expr* target, CompilerType* t) {
        switch (target->type) {
            case AST_TYPE::Attribute:
                // doesn't affect types (yet?)
                break;
            case AST_TYPE::Name:
                _doSet(ast_cast<AST_Name>(target)->id, t);
                break;
            case AST_TYPE::Subscript:
                break;
            case AST_TYPE::Tuple: {
                AST_Tuple* tt = ast_cast<AST_Tuple>(target);
                for (int i = 0; i < tt->elts.size(); i++) {
                    _doSet(tt->elts[i], UNKNOWN);
                }
                break;
            }
            default:
                ASSERT(0, "Unknown type for TypePropagator: %d", target->type);
                abort();
        }
    }

    virtual void* visit_attribute(AST_Attribute* node) {
        CompilerType* t = getType(node->value);
        CompilerType* rtn = t->getattrType(&node->attr, false);

        // if (speculation != TypeAnalysis::NONE && (node->attr == "x" || node->attr == "y" || node->attr == "z")) {
        // rtn = processSpeculation(float_cls, node, rtn);
        //}

        if (speculation != TypeAnalysis::NONE) {
            BoxedClass* speculated_class = predictClassFor(node);
            rtn = processSpeculation(speculated_class, node, rtn);
        }

        if (VERBOSITY() >= 2 && rtn == UNDEF) {
            printf("Think %s.%s is undefined, at %d:%d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno,
                   node->col_offset);
            print_ast(node);
            printf("\n");
        }
        return rtn;
    }

    virtual void* visit_clsattribute(AST_ClsAttribute* node) {
        CompilerType* t = getType(node->value);
        CompilerType* rtn = t->getattrType(&node->attr, true);
        if (VERBOSITY() >= 2 && rtn == UNDEF) {
            printf("Think %s.%s is undefined, at %d:%d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno,
                   node->col_offset);
            print_ast(node);
            printf("\n");
        }
        return rtn;
    }

    bool hasFixedBinops(CompilerType* type) {
        // This is non-exhaustive:
        return type == STR || type == INT || type == FLOAT || type == LIST || type == DICT;
    }

    virtual void* visit_augbinop(AST_AugBinOp* node) {
        CompilerType* left = getType(node->left);
        CompilerType* right = getType(node->right);
        if (!hasFixedBinops(left) || !hasFixedBinops(right))
            return UNKNOWN;

        // TODO this isn't the exact behavior
        std::string name = getInplaceOpName(node->op_type);
        CompilerType* attr_type = left->getattrType(&name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        arg_types.push_back(right);
        CompilerType* rtn = attr_type->callType(ArgPassSpec(2), arg_types, NULL);

        if (left == right && (left == INT || left == FLOAT)) {
            ASSERT((rtn == left || rtn == UNKNOWN) && "not strictly required but probably something worth looking into",
                   "%s %s %s -> %s", left->debugName().c_str(), name.c_str(), right->debugName().c_str(),
                   rtn->debugName().c_str());
        }

        ASSERT(rtn != UNDEF, "need to implement the actual semantics here for %s.%s", left->debugName().c_str(),
               name.c_str());

        return rtn;
    }

    virtual void* visit_binop(AST_BinOp* node) {
        CompilerType* left = getType(node->left);
        CompilerType* right = getType(node->right);
        if (!hasFixedBinops(left) || !hasFixedBinops(right))
            return UNKNOWN;

        // TODO this isn't the exact behavior
        const std::string& name = getOpName(node->op_type);
        CompilerType* attr_type = left->getattrType(&name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        arg_types.push_back(right);
        CompilerType* rtn = attr_type->callType(ArgPassSpec(2), arg_types, NULL);

        if (left == right && (left == INT || left == FLOAT)) {
            ASSERT((rtn == left || rtn == UNKNOWN) && "not strictly required but probably something worth looking into",
                   "%s %s %s -> %s", left->debugName().c_str(), name.c_str(), right->debugName().c_str(),
                   rtn->debugName().c_str());
        }

        ASSERT(rtn != UNDEF, "need to implement the actual semantics here for %s.%s", left->debugName().c_str(),
               name.c_str());

        return rtn;
    }

    virtual void* visit_boolop(AST_BoolOp* node) {
        int n = node->values.size();

        CompilerType* rtn = NULL;
        for (int i = 0; i < n; i++) {
            CompilerType* t = getType(node->values[i]);
            if (rtn == NULL)
                rtn = t;
            else if (rtn != t)
                rtn = UNKNOWN;
        }

        return rtn;
    }

    virtual void* visit_call(AST_Call* node) {
        CompilerType* func = getType(node->func);

        std::vector<CompilerType*> arg_types;
        for (int i = 0; i < node->args.size(); i++) {
            arg_types.push_back(getType(node->args[i]));
        }

        std::vector<std::pair<const std::string&, CompilerType*> > kw_types;
        for (AST_keyword* kw : node->keywords) {
            kw_types.push_back(std::make_pair<const std::string&, CompilerType*>(kw->arg, getType(kw->value)));
        }

        CompilerType* starargs = node->starargs ? getType(node->starargs) : NULL;
        CompilerType* kwargs = node->kwargs ? getType(node->kwargs) : NULL;

        if (starargs || kwargs || kw_types.size()) {
            // Bail out for anything but simple calls, for now:
            return UNKNOWN;
        }

        CompilerType* rtn_type = func->callType(ArgPassSpec(arg_types.size()), arg_types, NULL);

        // Should be unboxing things before getting here; would like to assert, though
        // we haven't specialized all of the stdlib.
        // ASSERT(rtn_type == unboxedType(rtn_type->getConcreteType()), "%s", rtn_type->debugName().c_str());
        rtn_type = unboxedType(rtn_type->getConcreteType());

        if (speculation != TypeAnalysis::NONE) {
            BoxedClass* speculated_rtn_cls = simpleCallSpeculation(node, rtn_type, arg_types);
            rtn_type = processSpeculation(speculated_rtn_cls, node, rtn_type);
        }

        return rtn_type;
    }

    virtual void* visit_compare(AST_Compare* node) {
        if (node->ops.size() == 1) {
            CompilerType* left = getType(node->left);
            CompilerType* right = getType(node->comparators[0]);

            AST_TYPE::AST_TYPE op_type = node->ops[0];
            if (op_type == AST_TYPE::Is || op_type == AST_TYPE::IsNot || op_type == AST_TYPE::In
                || op_type == AST_TYPE::NotIn) {
                assert(node->ops.size() == 1 && "I don't think this should happen");
                return BOOL;
            }

            const std::string& name = getOpName(node->ops[0]);
            CompilerType* attr_type = left->getattrType(&name, true);

            if (attr_type == UNDEF)
                attr_type = UNKNOWN;

            std::vector<CompilerType*> arg_types;
            arg_types.push_back(right);
            return attr_type->callType(ArgPassSpec(2), arg_types, NULL);
        } else {
            return UNKNOWN;
        }
    }

    virtual void* visit_dict(AST_Dict* node) {
        // Get all the sub-types, even though they're not necessary to
        // determine the expression type, so that things like speculations
        // can be processed.
        for (AST_expr* k : node->keys)
            getType(k);
        for (AST_expr* v : node->values)
            getType(v);

        return DICT;
    }

    virtual void* visit_index(AST_Index* node) { return getType(node->value); }

    virtual void* visit_lambda(AST_Lambda* node) { return typeFromClass(function_cls); }

    virtual void* visit_langprimitive(AST_LangPrimitive* node) {
        switch (node->opcode) {
            case AST_LangPrimitive::ISINSTANCE:
                return BOOL;
            case AST_LangPrimitive::LANDINGPAD:
                return UNKNOWN;
            case AST_LangPrimitive::LOCALS:
                return DICT;
            default:
                RELEASE_ASSERT(0, "%d", node->opcode);
        }
    }

    virtual void* visit_list(AST_List* node) {
        // Get all the sub-types, even though they're not necessary to
        // determine the expression type, so that things like speculations
        // can be processed.
        for (AST_expr* elt : node->elts) {
            getType(elt);
        }

        return LIST;
    }

    virtual void* visit_name(AST_Name* node) {
        if (scope_info->refersToGlobal(node->id)) {
            if (node->id == "xrange") {
                // printf("TODO guard here and return the classobj\n");
                // return typeOfClassobj(xrange_cls);
            }
            return UNKNOWN;
        }

        if (scope_info->refersToClosure(node->id)) {
            return UNKNOWN;
        }

        CompilerType*& t = sym_table[node->id];
        if (t == NULL) {
            // if (VERBOSITY() >= 2) {
            // printf("%s is undefined!\n", node->id.c_str());
            // raise(SIGTRAP);
            //}
            t = UNDEF;
        }
        return t;
    }

    virtual void* visit_num(AST_Num* node) {
        switch (node->num_type) {
            case AST_Num::INT:
                return INT;
            case AST_Num::FLOAT:
                return FLOAT;
        }
        abort();
    }

    virtual void* visit_repr(AST_Repr* node) { return STR; }

    virtual void* visit_slice(AST_Slice* node) { return SLICE; }

    virtual void* visit_str(AST_Str* node) { return STR; }

    virtual void* visit_subscript(AST_Subscript* node) {
        CompilerType* val = getType(node->value);
        CompilerType* slice = getType(node->slice);
        static std::string name("__getitem__");
        CompilerType* getitem_type = val->getattrType(&name, true);
        std::vector<CompilerType*> args;
        args.push_back(slice);
        return getitem_type->callType(ArgPassSpec(1), args, NULL);
    }

    virtual void* visit_tuple(AST_Tuple* node) {
        std::vector<CompilerType*> elt_types;
        for (int i = 0; i < node->elts.size(); i++) {
            elt_types.push_back(getType(node->elts[i]));
        }
        return makeTupleType(elt_types);
    }

    virtual void* visit_unaryop(AST_UnaryOp* node) {
        CompilerType* operand = getType(node->operand);

        // TODO this isn't the exact behavior
        const std::string& name = getOpName(node->op_type);
        CompilerType* attr_type = operand->getattrType(&name, true);
        std::vector<CompilerType*> arg_types;
        return attr_type->callType(ArgPassSpec(0), arg_types, NULL);
    }



    virtual void visit_assert(AST_Assert* node) {
        getType(node->test);
        if (node->msg)
            getType(node->msg);
    }

    virtual void visit_assign(AST_Assign* node) {
        CompilerType* t = getType(node->value);
        for (int i = 0; i < node->targets.size(); i++) {
            _doSet(node->targets[i], t);
        }
    }

    virtual void visit_branch(AST_Branch* node) {
        if (EXPAND_UNNEEDED) {
            getType(node->test);
        }
    }

    virtual void visit_classdef(AST_ClassDef* node) {
        // TODO should we speculate that classdefs will generally return a class?
        // CompilerType* t = typeFromClass(type_cls);
        CompilerType* t = UNKNOWN;
        _doSet(node->name, t);
    }

    virtual void visit_delete(AST_Delete* node) {
        for (AST_expr* target : node->targets) {
            RELEASE_ASSERT(target->type == AST_TYPE::Subscript, "");
            getType(ast_cast<AST_Subscript>(target)->value);
        }
    }

    virtual void visit_expr(AST_Expr* node) {
        if (EXPAND_UNNEEDED) {
            if (node->value != NULL)
                getType(node->value);
        }
    }

    virtual void visit_functiondef(AST_FunctionDef* node) { _doSet(node->name, typeFromClass(function_cls)); }

    virtual void visit_global(AST_Global* node) {}

    virtual void visit_alias(AST_alias* node) {
        const std::string* name = &node->name;
        if (node->asname.size())
            name = &node->asname;

        _doSet(*name, UNKNOWN);
    }

    virtual void visit_import(AST_Import* node) {
        for (AST_alias* alias : node->names)
            visit_alias(alias);
    }

    virtual void visit_importfrom(AST_ImportFrom* node) {
        for (AST_alias* alias : node->names)
            visit_alias(alias);
    }

    virtual void visit_invoke(AST_Invoke* node) { node->stmt->accept_stmt(this); }

    virtual void visit_jump(AST_Jump* node) {}
    virtual void visit_pass(AST_Pass* node) {}

    virtual void visit_print(AST_Print* node) {
        if (node->dest)
            getType(node->dest);

        if (EXPAND_UNNEEDED) {
            for (int i = 0; i < node->values.size(); i++) {
                getType(node->values[i]);
            }
        }
    }

    virtual void visit_raise(AST_Raise* node) {
        if (EXPAND_UNNEEDED) {
            if (node->arg0)
                getType(node->arg0);
            if (node->arg1)
                getType(node->arg1);
            if (node->arg2)
                getType(node->arg2);
        }
    }

    virtual void visit_return(AST_Return* node) {
        if (EXPAND_UNNEEDED) {
            if (node->value != NULL)
                getType(node->value);
        }
    }

    virtual void visit_unreachable(AST_Unreachable* node) {}

public:
    static void propagate(CFGBlock* block, const TypeMap& starting, TypeMap& ending, ExprTypeMap& expr_types,
                          TypeSpeculations& type_speculations, TypeAnalysis::SpeculationLevel speculation,
                          ScopeInfo* scope_info) {
        ending.insert(starting.begin(), starting.end());
        BasicBlockTypePropagator(block, ending, expr_types, type_speculations, speculation, scope_info).run();
    }
};

class PropagatingTypeAnalysis : public TypeAnalysis {
private:
    AllTypeMap starting_types;
    ExprTypeMap expr_types;
    TypeSpeculations type_speculations;
    SpeculationLevel speculation;

    PropagatingTypeAnalysis(const AllTypeMap& starting_types, const ExprTypeMap& expr_types,
                            TypeSpeculations& type_speculations, SpeculationLevel speculation)
        : starting_types(starting_types), expr_types(expr_types), type_speculations(type_speculations),
          speculation(speculation) {}

public:
    virtual ConcreteCompilerType* getTypeAtBlockEnd(const std::string& name, CFGBlock* block) {
        assert(block->successors.size() > 0);
        return getTypeAtBlockStart(name, block->successors[0]);
    }
    virtual ConcreteCompilerType* getTypeAtBlockStart(const std::string& name, CFGBlock* block) {
        CompilerType* base = starting_types[block][name];
        ASSERT(base != NULL, "%s %d", name.c_str(), block->idx);

        ConcreteCompilerType* rtn = base->getConcreteType();
        ASSERT(rtn != NULL, "%s %d", name.c_str(), block->idx);
        return rtn;
    }

    virtual BoxedClass* speculatedExprClass(AST_expr* call) { return type_speculations[call]; }

    static bool merge(CompilerType* lhs, CompilerType*& rhs) {
        assert(lhs);
        if (rhs == NULL) {
            rhs = lhs;
            return true;
        }

        if (lhs == UNDEF)
            return false;

        if (rhs == UNDEF) {
            rhs = lhs;
            return true;
        }

        if (lhs == rhs)
            return false;
        if (rhs == UNKNOWN)
            return false;
        if (lhs == UNKNOWN) {
            rhs = UNKNOWN;
            return true;
        }

        rhs = UNKNOWN;
        return true;

        ASSERT(0, "dont know how to merge these types: %s, %s", lhs->debugName().c_str(), rhs->debugName().c_str());
        abort();
    }
    static bool merge(const TypeMap& ending, TypeMap& next) {
        bool changed = false;
        for (TypeMap::const_iterator it = ending.begin(); it != ending.end(); it++) {
            CompilerType*& prev = next[it->first];
            changed = merge(it->second, prev) || changed;
        }
        return changed;
    }

    static PropagatingTypeAnalysis* doAnalysis(CFG* cfg, const SourceInfo::ArgNames& arg_names,
                                               const std::vector<ConcreteCompilerType*>& arg_types,
                                               SpeculationLevel speculation, ScopeInfo* scope_info) {
        AllTypeMap starting_types;
        ExprTypeMap expr_types;
        TypeSpeculations type_speculations;

        assert(arg_names.totalParameters() == arg_types.size());

        if (arg_names.args) {
            TypeMap& initial_types = starting_types[cfg->getStartingBlock()];
            int i = 0;

            for (; i < arg_names.args->size(); i++) {
                AST_expr* arg = (*arg_names.args)[i];
                assert(arg->type == AST_TYPE::Name);
                AST_Name* arg_name = ast_cast<AST_Name>(arg);
                initial_types[arg_name->id] = unboxedType(arg_types[i]);
            }

            if (arg_names.vararg->size()) {
                initial_types[*arg_names.vararg] = unboxedType(arg_types[i]);
                i++;
            }

            if (arg_names.kwarg->size()) {
                initial_types[*arg_names.kwarg] = unboxedType(arg_types[i]);
                i++;
            }

            assert(i == arg_types.size());
        }

        std::unordered_set<CFGBlock*> in_queue;
        std::priority_queue<CFGBlock*, std::vector<CFGBlock*>, CFGBlockMinIndex> queue;
        queue.push(cfg->getStartingBlock());
        in_queue.insert(cfg->getStartingBlock());

        int num_evaluations = 0;
        while (queue.size()) {
            ASSERT(queue.size() == in_queue.size(), "%ld %ld", queue.size(), in_queue.size());
            num_evaluations++;
            CFGBlock* block = queue.top();
            queue.pop();
            in_queue.erase(block);

            TypeMap ending;

            if (VERBOSITY("types")) {
                printf("processing types for block %d\n", block->idx);
            }
            if (VERBOSITY("types") >= 2) {
                printf("before:\n");
                TypeMap& starting = starting_types[block];
                for (const auto& p : starting) {
                    ASSERT(p.second, "%s", p.first.c_str());
                    printf("%s: %s\n", p.first.c_str(), p.second->debugName().c_str());
                }
            }

            BasicBlockTypePropagator::propagate(block, starting_types[block], ending, expr_types, type_speculations,
                                                speculation, scope_info);

            if (VERBOSITY("types") >= 2) {
                printf("before (after):\n");
                TypeMap& starting = starting_types[block];
                for (const auto& p : starting) {
                    ASSERT(p.second, "%s", p.first.c_str());
                    printf("%s: %s\n", p.first.c_str(), p.second->debugName().c_str());
                }
                printf("after:\n");
                for (const auto& p : ending) {
                    ASSERT(p.second, "%s", p.first.c_str());
                    printf("%s: %s\n", p.first.c_str(), p.second->debugName().c_str());
                }
            }

            for (int i = 0; i < block->successors.size(); i++) {
                CFGBlock* next_block = block->successors[i];
                bool first = (starting_types.count(next_block) == 0);
                bool changed = merge(ending, starting_types[next_block]);
                if ((first || changed) && in_queue.insert(next_block).second) {
                    queue.push(next_block);
                }
            }
        }

        if (VERBOSITY("types")) {
            printf("%ld BBs, %d evaluations = %.1f evaluations/block\n", cfg->blocks.size(), num_evaluations,
                   1.0 * num_evaluations / cfg->blocks.size());
        }

        if (VERBOSITY("types") >= 2) {
            for (CFGBlock* b : cfg->blocks) {
                printf("Types at beginning of block %d:\n", b->idx);

                TypeMap& starting = starting_types[b];
                for (const auto& p : starting) {
                    ASSERT(p.second, "%s", p.first.c_str());
                    printf("%s: %s\n", p.first.c_str(), p.second->debugName().c_str());
                }
            }
        }

        return new PropagatingTypeAnalysis(starting_types, expr_types, type_speculations, speculation);
    }
};


// public entry point:
TypeAnalysis* doTypeAnalysis(CFG* cfg, const SourceInfo::ArgNames& arg_names,
                             const std::vector<ConcreteCompilerType*>& arg_types,
                             TypeAnalysis::SpeculationLevel speculation, ScopeInfo* scope_info) {
    // return new NullTypeAnalysis();
    return PropagatingTypeAnalysis::doAnalysis(cfg, arg_names, arg_types, speculation, scope_info);
}
}
