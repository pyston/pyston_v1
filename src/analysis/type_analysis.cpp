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

#include <cstdio>
#include <deque>
#include <unordered_set>

#include "core/options.h"

#include "core/ast.h"
#include "core/cfg.h"

#include "analysis/scoping_analysis.h"
#include "analysis/type_analysis.h"

#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {

class NullTypeAnalysis : public TypeAnalysis {
    public:
        virtual ConcreteCompilerType* getTypeAtBlockStart(const std::string &name, CFGBlock* block);
        virtual ConcreteCompilerType* getTypeAtBlockEnd(const std::string &name, CFGBlock* block);
};

ConcreteCompilerType* NullTypeAnalysis::getTypeAtBlockStart(const std::string &name, CFGBlock *block) {
    return UNKNOWN;
}

ConcreteCompilerType* NullTypeAnalysis::getTypeAtBlockEnd(const std::string &name, CFGBlock *block) {
    assert(block->successors.size() > 0);
    return getTypeAtBlockStart(name, block->successors[0]);
}


static ConcreteCompilerType* unboxedType(ConcreteCompilerType *t) {
    if (t == BOXED_INT)
        return INT;
    if (t == BOXED_FLOAT)
        return FLOAT;
    return t;
}

static BoxedClass* simpleCallSpeculation(AST_Call* node, CompilerType* rtn_type, std::vector<CompilerType*> arg_types) {
    if (rtn_type->getConcreteType()->llvmType() != g.llvm_value_type_ptr) {
        //printf("Not right shape; it's %s\n", rtn_type->debugName().c_str());
        return NULL;
    }

    if (node->func->type == AST_TYPE::Name && static_cast<AST_Name*>(node->func)->id == "xrange")
        return xrange_cls;

    //if (node->func->type == AST_TYPE::Attribute && static_cast<AST_Attribute*>(node->func)->attr == "dot")
        //return float_cls;

    return NULL;
}

typedef std::unordered_map<std::string, CompilerType*> TypeMap;
typedef std::unordered_map<int, TypeMap> AllTypeMap;
typedef std::unordered_map<AST_expr*, CompilerType*> ExprTypeMap;
typedef std::unordered_map<AST_expr*, BoxedClass*> TypeSpeculations;
class BasicBlockTypePropagator : public ExprVisitor, public StmtVisitor {
    private:
        static const bool EXPAND_UNNEEDED = true;

        CFGBlock *block;
        TypeMap &sym_table;
        ExprTypeMap &expr_types;
        TypeSpeculations &type_speculations;
        TypeAnalysis::SpeculationLevel speculation;
        ScopeInfo *scope_info;

        BasicBlockTypePropagator(CFGBlock *block, TypeMap &initial, ExprTypeMap &expr_types, TypeSpeculations &type_speculations, TypeAnalysis::SpeculationLevel speculation, ScopeInfo *scope_info) : block(block), sym_table(initial), expr_types(expr_types), type_speculations(type_speculations), speculation(speculation), scope_info(scope_info) {}

        void run() {
            for (int i = 0; i < block->body.size(); i++) {
                block->body[i]->accept_stmt(this);
            }
        }

        CompilerType* processSpeculation(BoxedClass* speculated_cls, AST_expr* node, CompilerType* old_type) {
            assert(old_type);
            assert(speculation != TypeAnalysis::NONE);

            if (speculated_cls != NULL) {
                ConcreteCompilerType* speculated_type = unboxedType(typeFromClass(speculated_cls));
                if (VERBOSITY() >= 2) {
                    printf("in propagator, speculating that %s would actually be %s, at:\n", old_type->debugName().c_str(), speculated_type->debugName().c_str());
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
            CompilerType *rtn = static_cast<CompilerType*>(raw_rtn);

            if (VERBOSITY() >= 2) {
                print_ast(node);
                printf(" %s\n", rtn->debugName().c_str());
            }

            expr_types[node] = rtn;
            return rtn;
        }

        void _doSet(std::string target, CompilerType *t) {
            if (t)
                sym_table[target] = t;
        }

        void _doSet(AST_expr* target, CompilerType *t) {
            switch (target->type) {
                case AST_TYPE::Attribute:
                    // doesn't affect types (yet?)
                    break;
                case AST_TYPE::Name:
                    _doSet(static_cast<AST_Name*>(target)->id, t);
                    break;
                case AST_TYPE::Subscript:
                    break;
                case AST_TYPE::Tuple: {
                    AST_Tuple *tt = static_cast<AST_Tuple*>(target);
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

        virtual void* visit_attribute(AST_Attribute *node) {
            CompilerType *t = getType(node->value);
            assert(node->ctx_type == AST_TYPE::Load);
            CompilerType *rtn = t->getattrType(node->attr);

            //if (speculation != TypeAnalysis::NONE && (node->attr == "x" || node->attr == "y" || node->attr == "z")) {
                //rtn = processSpeculation(float_cls, node, rtn);
            //}

            if (VERBOSITY() >= 2 && rtn == UNDEF) {
                printf("Think %s.%s is undefined, at %d:%d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno, node->col_offset);
                print_ast(node);
                printf("\n");
            }
            return rtn;
        }

        virtual void* visit_clsattribute(AST_ClsAttribute *node) {
            CompilerType *t = getType(node->value);
            CompilerType *rtn = t->getattrType(node->attr);
            if (VERBOSITY() >= 2 && rtn == UNDEF) {
                printf("Think %s.%s is undefined, at %d:%d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno, node->col_offset);
                print_ast(node);
                printf("\n");
            }
            return rtn;
        }

        virtual void* visit_binop(AST_BinOp *node) {
            CompilerType *left = getType(node->left);
            CompilerType *right = getType(node->right);

            // TODO this isn't the exact behavior
            std::string name = getOpName(node->op_type);
            CompilerType *attr_type = left->getattrType(name);

            std::vector<CompilerType*> arg_types;
            arg_types.push_back(right);
            CompilerType *rtn = attr_type->callType(arg_types);

            if (left == right && (left == INT || left == FLOAT)) {
                ASSERT((rtn == left || rtn == UNDEF) && "not strictly required but probably something worth looking into", "%s %s", name.c_str(), rtn->debugName().c_str());
            }

            return rtn;
        }

        virtual void* visit_boolop(AST_BoolOp *node) {
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

        virtual void* visit_call(AST_Call *node) {
            assert(!node->starargs);
            assert(!node->kwargs);
            assert(node->keywords.size() == 0);
            CompilerType* func = getType(node->func);

            std::vector<CompilerType*> arg_types;
            for (int i = 0; i < node->args.size(); i++) {
                arg_types.push_back(getType(node->args[i]));
            }
            CompilerType *rtn_type = func->callType(arg_types);

            // Should be unboxing things before getting here:
            ASSERT(rtn_type == unboxedType(rtn_type->getConcreteType()), "%s", rtn_type->debugName().c_str());
            rtn_type = unboxedType(rtn_type->getConcreteType());

            if (speculation != TypeAnalysis::NONE) {
                BoxedClass* speculated_rtn_cls = simpleCallSpeculation(node, rtn_type, arg_types);
                rtn_type = processSpeculation(speculated_rtn_cls, node, rtn_type);
            }

            return rtn_type;
        }

        virtual void* visit_compare(AST_Compare *node) {
            RELEASE_ASSERT(node->ops.size() == 1, "unimplemented");

            CompilerType *left = getType(node->left);
            CompilerType *right = getType(node->comparators[0]);

            if (node->ops[0] == AST_TYPE::Is || node->ops[0] == AST_TYPE::IsNot) {
                assert(node->ops.size() == 1 && "I don't think this should happen");
                return BOOL;
            }
            std::string name = getOpName(node->ops[0]);
            CompilerType *attr_type = left->getattrType(name);
            std::vector<CompilerType*> arg_types;
            arg_types.push_back(right);
            return attr_type->callType(arg_types);
        }

        virtual void* visit_dict(AST_Dict *node) {
            return DICT;
        }

        virtual void* visit_index(AST_Index* node) {
            return getType(node->value);
        }

        virtual void* visit_list(AST_List *node) {
            return LIST;
        }

        virtual void* visit_name(AST_Name *node) {
            if (scope_info->refersToGlobal(node->id)) {
                if (node->id == "xrange") {
                    //printf("TODO guard here and return the classobj\n");
                    //return typeOfClassobj(xrange_cls);
                }
                return UNKNOWN;
            }

            CompilerType* &t = sym_table[node->id];
            if (t == NULL) {
                if (VERBOSITY() >= 2) {
                    printf("%s is undefined!\n", node->id.c_str());
                    raise(SIGTRAP);
                }
                t = UNDEF;
            }
            return t;
        }

        virtual void* visit_num(AST_Num *node) {
            switch (node->num_type) {
                case AST_Num::INT:
                    return INT;
                case AST_Num::FLOAT:
                    return FLOAT;
            }
            abort();
        }

        virtual void* visit_slice(AST_Slice *node) {
            return SLICE;
        }

        virtual void* visit_str(AST_Str *node) {
            return STR;
        }

        virtual void* visit_subscript(AST_Subscript *node) {
            CompilerType *val = getType(node->value);
            CompilerType *slice = getType(node->slice);
            CompilerType *getitem_type = val->getattrType("__getitem__");
            std::vector<CompilerType*> args;
            args.push_back(slice);
            return getitem_type->callType(args);
        }

        virtual void* visit_tuple(AST_Tuple *node) {
            std::vector<CompilerType*> elt_types;
            for (int i = 0; i < node->elts.size(); i++) {
                elt_types.push_back(getType(node->elts[i]));
            }
            return makeTupleType(elt_types);
        }

        virtual void* visit_unaryop(AST_UnaryOp *node) {
            CompilerType *operand = getType(node->operand);

            // TODO this isn't the exact behavior
            std::string name = getOpName(node->op_type);
            CompilerType *attr_type = operand->getattrType(name);
            std::vector<CompilerType*> arg_types;
            return attr_type->callType(arg_types);
        }




        virtual void visit_assign(AST_Assign* node) {
            CompilerType* t = getType(node->value);
            for (int i = 0; i < node->targets.size(); i++) {
                _doSet(node->targets[i], t);
            }
        }

        virtual void visit_augassign(AST_AugAssign* node) {
            CompilerType *t = getType(node->target);
            CompilerType *v = getType(node->value);

            // TODO this isn't the right behavior
            std::string name = getOpName(node->op_type);
            name = "__i" + name.substr(2);
            CompilerType *attr_type = t->getattrType(name);

            std::vector<CompilerType*> arg_types;
            arg_types.push_back(v);
            CompilerType *rtn = attr_type->callType(arg_types);

            _doSet(node->target, rtn);
        }

        virtual void visit_branch(AST_Branch* node) {
            if (EXPAND_UNNEEDED) {
                getType(node->test);
            }
        }

        virtual void visit_classdef(AST_ClassDef *node) {
            CompilerType *t = typeFromClass(type_cls);
            _doSet(node->name, t);
        }

        virtual void visit_expr(AST_Expr* node) {
            if (EXPAND_UNNEEDED) {
                if (node->value != NULL)
                    getType(node->value);
            }
        }

        virtual void visit_functiondef(AST_FunctionDef *node) {
            _doSet(node->name, typeFromClass(function_cls));
        }

        virtual void visit_global(AST_Global* node) {
        }

        virtual void visit_import(AST_Import *node) {
            for (int i = 0; i < node->names.size(); i++) {
                AST_alias *alias = node->names[i];
                std::string &name = alias->name;
                if (alias->asname.size())
                    name = alias->asname;

                _doSet(name, UNKNOWN);
            }
        }

        virtual void visit_jump(AST_Jump* node) {}
        virtual void visit_pass(AST_Pass* node) {}

        virtual void visit_print(AST_Print* node) {
            assert(node->dest == NULL);
            if (EXPAND_UNNEEDED) {
                for (int i = 0; i < node->values.size(); i++) {
                    getType(node->values[i]);
                }
            }
        }

        virtual void visit_return(AST_Return* node) {
            if (EXPAND_UNNEEDED) {
                if (node->value != NULL)
                    getType(node->value);
            }
        }

    public:
        static void propagate(CFGBlock *block, const TypeMap &starting, TypeMap &ending, ExprTypeMap &expr_types, TypeSpeculations &type_speculations, TypeAnalysis::SpeculationLevel speculation, ScopeInfo *scope_info) {
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

        PropagatingTypeAnalysis(const AllTypeMap &starting_types, const ExprTypeMap &expr_types, TypeSpeculations &type_speculations, SpeculationLevel speculation) : starting_types(starting_types), expr_types(expr_types), type_speculations(type_speculations), speculation(speculation) {}

    public:
        virtual ConcreteCompilerType* getTypeAtBlockEnd(const std::string &name, CFGBlock* block) {
            assert(block->successors.size() > 0);
            return getTypeAtBlockStart(name, block->successors[0]);
        }
        virtual ConcreteCompilerType* getTypeAtBlockStart(const std::string &name, CFGBlock* block) {
            CompilerType *base = starting_types[block->idx][name];
            ASSERT(base != NULL, "%s %d", name.c_str(), block->idx);

            ConcreteCompilerType *rtn = base->getConcreteType();
            ASSERT(rtn != NULL, "%s %d", name.c_str(), block->idx);
            return rtn;
        }

        virtual BoxedClass* speculatedExprClass(AST_expr* call) {
            return type_speculations[call];
        }

        static bool merge(CompilerType *lhs, CompilerType* &rhs) {
            assert(lhs);
            if (rhs == NULL) {
                rhs = lhs;
                return true;
            }

            if (lhs == UNDEF || rhs == UNDEF)
                return false;

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
        static bool merge(const TypeMap &ending, TypeMap &next) {
            bool changed = false;
            for (TypeMap::const_iterator it = ending.begin(); it != ending.end(); it++) {
                CompilerType* &prev = next[it->first];
                changed = merge(it->second, prev) || changed;
            }
            return changed;
        }

        static PropagatingTypeAnalysis* doAnalysis(CFG *cfg, const std::vector<AST_expr*> &arg_names, const std::vector<ConcreteCompilerType*> &arg_types, SpeculationLevel speculation, ScopeInfo *scope_info) {
            AllTypeMap starting_types;
            ExprTypeMap expr_types;
            TypeSpeculations type_speculations;

            assert(arg_names.size() == arg_types.size());

            {
                TypeMap &initial_types = starting_types[0];
                for (int i = 0; i < arg_names.size(); i++) {
                    AST_expr* arg = arg_names[i];
                    assert(arg->type == AST_TYPE::Name);
                    AST_Name *arg_name = static_cast<AST_Name*>(arg);
                    initial_types[arg_name->id] = unboxedType(arg_types[i]);
                }
            }

            std::unordered_set<int> in_queue;
            std::deque<int> queue;
            queue.push_back(0);

            while (queue.size()) {
                int block_id = queue.front();
                queue.pop_front();
                in_queue.erase(block_id);

                CFGBlock *block = cfg->blocks[block_id];

                TypeMap ending;

                if (VERBOSITY("types")) {
                    printf("processing types for block %d\n", block_id);
                }
                if (VERBOSITY("types") >= 2) {
                    printf("before:\n");
                    TypeMap &starting = starting_types[block_id];
                    for (TypeMap::iterator it = starting.begin(), end = starting.end(); it != end; ++it) {
                        ASSERT(it->second, "%s", it->first.c_str());
                        printf("%s: %s\n", it->first.c_str(), it->second->debugName().c_str());
                    }
                }

                BasicBlockTypePropagator::propagate(block, starting_types[block_id], ending, expr_types, type_speculations, speculation, scope_info);

                if (VERBOSITY("types") >= 2) {
                    printf("before (after):\n");
                    TypeMap &starting = starting_types[block_id];
                    for (TypeMap::iterator it = starting.begin(), end = starting.end(); it != end; ++it) {
                        ASSERT(it->second, "%s", it->first.c_str());
                        printf("%s: %s\n", it->first.c_str(), it->second->debugName().c_str());
                    }
                    printf("after:\n");
                    for (TypeMap::iterator it = ending.begin(), end = ending.end(); it != end; ++it) {
                        ASSERT(it->second, "%s", it->first.c_str());
                        printf("%s: %s\n", it->first.c_str(), it->second->debugName().c_str());
                    }
                }

                for (int i = 0; i < block->successors.size(); i++) {
                    int next_id = block->successors[i]->idx;
                    bool first = (starting_types.count(next_id) == 0);
                    bool changed = merge(ending, starting_types[next_id]);
                    if ((first || changed) && in_queue.insert(next_id).second) {
                        queue.push_back(next_id);
                    }
                }
            }

            if (VERBOSITY("types") >= 2) {
                for (int i = 0; i < cfg->blocks.size(); i++) {
                    printf("Types at beginning of block %d:\n", i);
                    CFGBlock *b = cfg->blocks[i];

                    TypeMap &starting = starting_types[i];
                    for (TypeMap::iterator it = starting.begin(), end = starting.end(); it != end; ++it) {
                        ASSERT(it->second, "%s", it->first.c_str());
                        printf("%s: %s\n", it->first.c_str(), it->second->debugName().c_str());
                    }
                }
            }

            return new PropagatingTypeAnalysis(starting_types, expr_types, type_speculations, speculation);
        }
};


// public entry point:
TypeAnalysis* doTypeAnalysis(CFG *cfg, const std::vector<AST_expr*> &arg_names, const std::vector<ConcreteCompilerType*> &arg_types, TypeAnalysis::SpeculationLevel speculation, ScopeInfo *scope_info) {
    //return new NullTypeAnalysis();
    return PropagatingTypeAnalysis::doAnalysis(cfg, arg_names, arg_types, speculation, scope_info);
}

}
