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

#include "analysis/type_analysis.h"

#include <cstdio>
#include <deque>
#include <unordered_set>

#include "llvm/ADT/SmallPtrSet.h"

#include "analysis/fpc.h"
#include "analysis/scoping_analysis.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/osrentry.h"
#include "codegen/type_recording.h"
#include "core/bst.h"
#include "core/cfg.h"
#include "core/options.h"
#include "core/util.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 4

namespace pyston {

class NullTypeAnalysis : public TypeAnalysis {
public:
    ConcreteCompilerType* getTypeAtBlockStart(int vreg, CFGBlock* block) override;
    ConcreteCompilerType* getTypeAtBlockEnd(int vreg, CFGBlock* block) override;

    BoxedClass* speculatedExprClass(BST_expr*) override { return NULL; }
    BoxedClass* speculatedExprClass(BST_slice*) override { return NULL; }
};

ConcreteCompilerType* NullTypeAnalysis::getTypeAtBlockStart(int vreg, CFGBlock* block) {
    return UNKNOWN;
}

ConcreteCompilerType* NullTypeAnalysis::getTypeAtBlockEnd(int vreg, CFGBlock* block) {
    assert(block->successors.size() > 0);
    return getTypeAtBlockStart(vreg, block->successors[0]);
}


// Note: the behavior of this function must match irgenerator.cpp::unboxVar()
static CompilerType* unboxedType(ConcreteCompilerType* t) {
    if (t == BOXED_BOOL)
        return BOOL;
    if (t == BOXED_INT)
        return INT;
    if (t == BOXED_FLOAT)
        return FLOAT;
    return t;
}

static BoxedClass* simpleCallSpeculation(BST_Call* node, CompilerType* rtn_type, std::vector<CompilerType*> arg_types) {
    if (rtn_type->getConcreteType()->llvmType() != g.llvm_value_type_ptr) {
        // printf("Not right shape; it's %s\n", rtn_type->debugName().c_str());
        return NULL;
    }

    if (node->func->type == BST_TYPE::Name && bst_cast<BST_Name>(node->func)->id.s() == "xrange")
        return xrange_cls;

    return predictClassFor(node);
}

typedef VRegMap<CompilerType*> TypeMap;
typedef llvm::DenseMap<CFGBlock*, TypeMap> AllTypeMap;
typedef llvm::DenseMap<BST*, CompilerType*> ExprTypeMap;
typedef llvm::DenseMap<BST*, BoxedClass*> TypeSpeculations;
class BasicBlockTypePropagator : public ExprVisitor, public StmtVisitor, public SliceVisitor {
private:
    static const bool EXPAND_UNNEEDED = true;

    CFGBlock* block;
    TypeMap& sym_table;
    ExprTypeMap& expr_types;
    TypeSpeculations& type_speculations;
    TypeAnalysis::SpeculationLevel speculation;

    BasicBlockTypePropagator(CFGBlock* block, TypeMap& initial, ExprTypeMap& expr_types,
                             TypeSpeculations& type_speculations, TypeAnalysis::SpeculationLevel speculation)
        : block(block),
          sym_table(initial),
          expr_types(expr_types),
          type_speculations(type_speculations),
          speculation(speculation) {}

    void run() {
        for (int i = 0; i < block->body.size(); i++) {
            block->body[i]->accept_stmt(this);
        }
    }

    CompilerType* processSpeculation(BoxedClass* speculated_cls, BST_expr* node, CompilerType* old_type) {
        assert(old_type);
        assert(speculation != TypeAnalysis::NONE);

        if (speculated_cls != NULL && speculated_cls->is_constant) {
            CompilerType* speculated_type = unboxedType(typeFromClass(speculated_cls));
            if (!old_type->canConvertTo(speculated_type)) {
                if (VERBOSITY() >= 2) {
                    printf("in propagator, speculating that %s would actually be %s, at ",
                           old_type->debugName().c_str(), speculated_type->debugName().c_str());
                    fflush(stdout);
                    print_bst(node);
                    llvm::outs().flush();
                    printf("\n");
                }

                type_speculations[node] = speculated_cls;
                return speculated_type;
            }
        }
        return old_type;
    }

    CompilerType* getType(BST_slice* node) {
        type_speculations.erase(node);

        void* raw_rtn = node->accept_slice(this);
        CompilerType* rtn = static_cast<CompilerType*>(raw_rtn);

        if (VERBOSITY() >= 3) {
            print_bst(node);
            printf(" %s\n", rtn->debugName().c_str());
        }

        expr_types[node] = rtn;
        assert(rtn->isUsable());
        return rtn;
    }

    CompilerType* getType(BST_expr* node) {
        type_speculations.erase(node);

        void* raw_rtn = node->accept_expr(this);
        CompilerType* rtn = static_cast<CompilerType*>(raw_rtn);

        if (VERBOSITY() >= 3) {
            printf("Type of ");
            fflush(stdout);
            print_bst(node);
            printf(" is %s\n", rtn->debugName().c_str());
        }

        expr_types[node] = rtn;
        assert(rtn->isUsable());
        return rtn;
    }

    void _doSet(int vreg, CompilerType* t) {
        assert(t->isUsable());
        if (t)
            sym_table[vreg] = t;
    }

    void _doSet(BST_expr* target, CompilerType* t) {
        switch (target->type) {
            case BST_TYPE::Attribute:
                // doesn't affect types (yet?)
                break;
            case BST_TYPE::Name: {
                auto name = bst_cast<BST_Name>(target);
                assert(name->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
                if (name->lookup_type == ScopeInfo::VarScopeType::FAST
                    || name->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
                    _doSet(name->vreg, t);
                } else
                    assert(name->vreg == -1);
                break;
            }
            case BST_TYPE::Subscript:
                break;
            case BST_TYPE::Tuple: {
                BST_Tuple* tt = bst_cast<BST_Tuple>(target);
                auto val_types = t->unpackTypes(tt->elts.size());
                assert(val_types.size() == tt->elts.size());
                for (int i = 0; i < tt->elts.size(); i++) {
                    _doSet(tt->elts[i], val_types[i]);
                }
                break;
            }
            default:
                ASSERT(0, "Unknown type for TypePropagator: %d", target->type);
                abort();
        }
    }

    void* visit_ellipsis(BST_Ellipsis* node) override { return typeFromClass(ellipsis_cls); }

    void* visit_attribute(BST_Attribute* node) override {
        CompilerType* t = getType(node->value);
        CompilerType* rtn = t->getattrType(node->attr, false);

        // if (speculation != TypeAnalysis::NONE && (node->attr == "x" || node->attr == "y" || node->attr == "z")) {
        // rtn = processSpeculation(float_cls, node, rtn);
        //}

        if (speculation != TypeAnalysis::NONE) {
            BoxedClass* speculated_class = predictClassFor(node);
            rtn = processSpeculation(speculated_class, node, rtn);
        }

        if (VERBOSITY() >= 2 && rtn == UNDEF) {
            printf("Think %s.%s is undefined, at %d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno);
            print_bst(node);
            printf("\n");
        }
        return rtn;
    }

    void* visit_clsattribute(BST_ClsAttribute* node) override {
        CompilerType* t = getType(node->value);
        CompilerType* rtn = t->getattrType(node->attr, true);
        if (VERBOSITY() >= 2 && rtn == UNDEF) {
            printf("Think %s.%s is undefined, at %d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno);
            print_bst(node);
            printf("\n");
        }
        return rtn;
    }

    bool hasFixedOps(CompilerType* type) {
        // This is non-exhaustive:
        return type == STR || type == INT || type == FLOAT || type == LIST || type == DICT;
    }

    void* visit_augbinop(BST_AugBinOp* node) override {
        CompilerType* left = getType(node->left);
        CompilerType* right = getType(node->right);
        if (!hasFixedOps(left) || !hasFixedOps(right))
            return UNKNOWN;

        // TODO this isn't the exact behavior
        BoxedString* name = getInplaceOpName(node->op_type);
        AUTO_DECREF(name);
        CompilerType* attr_type = left->getattrType(name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        arg_types.push_back(right);
        CompilerType* rtn = attr_type->callType(ArgPassSpec(2), arg_types, NULL);

        if (left == right && (left == INT || left == FLOAT)) {
            ASSERT((rtn == left || rtn == UNKNOWN) && "not strictly required but probably something worth looking into",
                   "%s %s %s -> %s", left->debugName().c_str(), name->c_str(), right->debugName().c_str(),
                   rtn->debugName().c_str());
        }

        ASSERT(rtn != UNDEF, "need to implement the actual semantics here for %s.%s", left->debugName().c_str(),
               name->c_str());

        return rtn;
    }

    void* visit_binop(BST_BinOp* node) override {
        CompilerType* left = getType(node->left);
        CompilerType* right = getType(node->right);
        if (!hasFixedOps(left) || !hasFixedOps(right))
            return UNKNOWN;

        // TODO this isn't the exact behavior
        BoxedString* name = getOpName(node->op_type);
        CompilerType* attr_type = left->getattrType(name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        arg_types.push_back(right);
        CompilerType* rtn = attr_type->callType(ArgPassSpec(2), arg_types, NULL);

        ASSERT(rtn != UNDEF, "need to implement the actual semantics here for %s.%s", left->debugName().c_str(),
               name->c_str());

        return rtn;
    }

    void* visit_call(BST_Call* node) override {
        CompilerType* func = getType(node->func);

        std::vector<CompilerType*> arg_types;
        for (int i = 0; i < node->args.size(); i++) {
            arg_types.push_back(getType(node->args[i]));
        }

        std::vector<std::pair<InternedString, CompilerType*>> kw_types;
        for (BST_keyword* kw : node->keywords) {
            kw_types.push_back(std::make_pair(kw->arg, getType(kw->value)));
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

    void* visit_compare(BST_Compare* node) override {
        CompilerType* left = getType(node->left);
        CompilerType* right = getType(node->comparator);

        AST_TYPE::AST_TYPE op_type = node->op;
        if (op_type == AST_TYPE::Is || op_type == AST_TYPE::IsNot || op_type == AST_TYPE::In
            || op_type == AST_TYPE::NotIn) {
            return BOOL;
        }

        BoxedString* name = getOpName(node->op);
        CompilerType* attr_type = left->getattrType(name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        arg_types.push_back(right);
        return attr_type->callType(ArgPassSpec(2), arg_types, NULL);
    }

    void* visit_dict(BST_Dict* node) override {
        // Get all the sub-types, even though they're not necessary to
        // determine the expression type, so that things like speculations
        // can be processed.
        for (BST_expr* k : node->keys)
            getType(k);
        for (BST_expr* v : node->values)
            getType(v);

        return DICT;
    }

    void* visit_index(BST_Index* node) override { return getType(node->value); }

    void* visit_langprimitive(BST_LangPrimitive* node) override {
        switch (node->opcode) {
            case BST_LangPrimitive::CHECK_EXC_MATCH:
                return BOOL;
            case BST_LangPrimitive::LOCALS:
                return DICT;
            case BST_LangPrimitive::GET_ITER:
                return getType(node->args[0])->getPystonIterType();
            case BST_LangPrimitive::LANDINGPAD:
            case BST_LangPrimitive::IMPORT_FROM:
            case BST_LangPrimitive::IMPORT_STAR:
            case BST_LangPrimitive::IMPORT_NAME:
                return UNKNOWN;
            case BST_LangPrimitive::NONE:
            case BST_LangPrimitive::SET_EXC_INFO:
            case BST_LangPrimitive::UNCACHE_EXC_INFO:
            case BST_LangPrimitive::PRINT_EXPR:
                return NONE;
            case BST_LangPrimitive::HASNEXT:
            case BST_LangPrimitive::NONZERO:
                return BOOL;
            default:
                RELEASE_ASSERT(0, "%d", node->opcode);
        }
    }

    void* visit_list(BST_List* node) override {
        // Get all the sub-types, even though they're not necessary to
        // determine the expression type, so that things like speculations
        // can be processed.
        for (BST_expr* elt : node->elts) {
            getType(elt);
        }

        return LIST;
    }

    void* visit_name(BST_Name* node) override {
        assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
        auto name_scope = node->lookup_type;

        if (name_scope == ScopeInfo::VarScopeType::GLOBAL) {
            if (node->id.s() == "None")
                return NONE;
            return UNKNOWN;
        }

        if (name_scope == ScopeInfo::VarScopeType::NAME) {
            return UNKNOWN;
        }

        if (name_scope == ScopeInfo::VarScopeType::DEREF) {
            return UNKNOWN;
        }

        if (name_scope == ScopeInfo::VarScopeType::FAST || name_scope == ScopeInfo::VarScopeType::CLOSURE) {
            CompilerType*& t = sym_table[node->vreg];
            if (t == NULL) {
                // if (VERBOSITY() >= 2) {
                // printf("%s is undefined!\n", node->id.c_str());
                // raise(SIGTRAP);
                //}
                t = UNDEF;
            }
            return t;
        }

        RELEASE_ASSERT(0, "Unknown scope type: %d", (int)name_scope);
    }

    void* visit_num(BST_Num* node) override {
        switch (node->num_type) {
            case AST_Num::INT:
                return INT;
            case AST_Num::FLOAT:
                return FLOAT;
            case AST_Num::LONG:
                return LONG;
            case AST_Num::COMPLEX:
                return BOXED_COMPLEX;
        }
        abort();
    }

    void* visit_repr(BST_Repr* node) override { return STR; }

    void* visit_set(BST_Set* node) override { return SET; }

    void* visit_slice(BST_Slice* node) override { return SLICE; }

    void* visit_extslice(BST_ExtSlice* node) override {
        std::vector<CompilerType*> elt_types;
        for (auto* e : node->dims) {
            elt_types.push_back(getType(e));
        }
        return makeTupleType(elt_types);
    }

    void* visit_str(BST_Str* node) override {
        if (node->str_type == AST_Str::STR)
            return STR;
        else if (node->str_type == AST_Str::UNICODE)
            return typeFromClass(unicode_cls);
        RELEASE_ASSERT(0, "Unknown string type %d", (int)node->str_type);
    }

    void* visit_subscript(BST_Subscript* node) override {
        CompilerType* val = getType(node->value);
        CompilerType* slice = getType(node->slice);
        static BoxedString* name = getStaticString("__getitem__");
        CompilerType* getitem_type = val->getattrType(name, true);
        std::vector<CompilerType*> args;
        args.push_back(slice);
        return getitem_type->callType(ArgPassSpec(1), args, NULL);
    }

    void* visit_tuple(BST_Tuple* node) override {
        std::vector<CompilerType*> elt_types;
        for (int i = 0; i < node->elts.size(); i++) {
            elt_types.push_back(getType(node->elts[i]));
        }
        return makeTupleType(elt_types);
    }

    void* visit_unaryop(BST_UnaryOp* node) override {
        CompilerType* operand = getType(node->operand);
        if (!hasFixedOps(operand))
            return UNKNOWN;

        // TODO this isn't the exact behavior
        BoxedString* name = getOpName(node->op_type);
        CompilerType* attr_type = operand->getattrType(name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        CompilerType* rtn_type = attr_type->callType(ArgPassSpec(0), arg_types, NULL);
        rtn_type = unboxedType(rtn_type->getConcreteType());
        return rtn_type;
    }

    void* visit_yield(BST_Yield*) override { return UNKNOWN; }


    void visit_assert(BST_Assert* node) override {
        getType(node->test);
        if (node->msg)
            getType(node->msg);
    }

    void visit_assign(BST_Assign* node) override {
        CompilerType* t = getType(node->value);
        _doSet(node->target, t);
    }

    void visit_branch(BST_Branch* node) override {
        if (EXPAND_UNNEEDED) {
            getType(node->test);
        }
    }

    void* visit_makeclass(BST_MakeClass* mkclass) override {
        BST_ClassDef* node = mkclass->class_def;

        for (auto d : node->decorator_list) {
            getType(d);
        }

        for (auto b : node->bases) {
            getType(b);
        }

        // TODO should we speculate that classdefs will generally return a class?
        // return typeFromClass(type_cls);
        return UNKNOWN;
    }

    void visit_delete(BST_Delete* node) override {
        BST_expr* target = node->target;
        switch (target->type) {
            case BST_TYPE::Subscript:
                getType(bst_cast<BST_Subscript>(target)->value);
                break;
            case BST_TYPE::Attribute:
                getType(bst_cast<BST_Attribute>(target)->value);
                break;
            case BST_TYPE::Name: {
                auto name = bst_cast<BST_Name>(target);
                assert(name->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
                if (name->lookup_type == ScopeInfo::VarScopeType::FAST
                    || name->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
                    sym_table[name->vreg] = NULL;
                } else
                    assert(name->vreg == -1);
                break;
            }
            default:
                RELEASE_ASSERT(0, "%d", target->type);
        }
    }

    void visit_expr(BST_Expr* node) override {
        if (EXPAND_UNNEEDED) {
            if (node->value != NULL)
                getType(node->value);
        }
    }

    void* visit_makefunction(BST_MakeFunction* mkfn) override {
        BST_FunctionDef* node = mkfn->function_def;

        for (auto d : node->decorator_list) {
            getType(d);
        }

        for (auto d : node->args->defaults) {
            getType(d);
        }

        CompilerType* t = UNKNOWN;
        if (node->decorator_list.empty())
            t = typeFromClass(function_cls);
        return t;
    }

    void visit_exec(BST_Exec* node) override {
        getType(node->body);
        if (node->globals)
            getType(node->globals);
        if (node->locals)
            getType(node->locals);
    }

    void visit_invoke(BST_Invoke* node) override { node->stmt->accept_stmt(this); }

    void visit_jump(BST_Jump* node) override {}

    void visit_print(BST_Print* node) override {
        if (node->dest)
            getType(node->dest);

        if (EXPAND_UNNEEDED && node->value)
            getType(node->value);
    }

    void visit_raise(BST_Raise* node) override {
        if (EXPAND_UNNEEDED) {
            if (node->arg0)
                getType(node->arg0);
            if (node->arg1)
                getType(node->arg1);
            if (node->arg2)
                getType(node->arg2);
        }
    }

    void visit_return(BST_Return* node) override {
        if (EXPAND_UNNEEDED) {
            if (node->value != NULL)
                getType(node->value);
        }
    }

public:
    static TypeMap propagate(CFGBlock* block, const TypeMap& starting, ExprTypeMap& expr_types,
                             TypeSpeculations& type_speculations, TypeAnalysis::SpeculationLevel speculation) {
        TypeMap ending = starting;
        BasicBlockTypePropagator(block, ending, expr_types, type_speculations, speculation).run();
        return ending;
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
        : starting_types(starting_types),
          expr_types(expr_types),
          type_speculations(type_speculations),
          speculation(speculation) {}

public:
    ConcreteCompilerType* getTypeAtBlockEnd(int vreg, CFGBlock* block) override {
        assert(block->successors.size() > 0);
        return getTypeAtBlockStart(vreg, block->successors[0]);
    }
    ConcreteCompilerType* getTypeAtBlockStart(int vreg, CFGBlock* block) override {
        assert(starting_types.count(block));
        CompilerType* base = starting_types.find(block)->second[vreg];
        ASSERT(base != NULL, "%s %d", block->cfg->getVRegInfo().getName(vreg).c_str(), block->idx);

        ConcreteCompilerType* rtn = base->getConcreteType();
        ASSERT(rtn != NULL, "%s %d", block->cfg->getVRegInfo().getName(vreg).c_str(), block->idx);
        return rtn;
    }

    BoxedClass* speculatedExprClass(BST_slice* call) override { return type_speculations[call]; }
    BoxedClass* speculatedExprClass(BST_expr* call) override { return type_speculations[call]; }

    static bool merge(CompilerType* lhs, CompilerType*& rhs) {
        if (!lhs)
            return false;

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
        for (auto&& entry : ending) {
            CompilerType*& prev = next[entry.first];
            changed = merge(entry.second, prev) || changed;
        }
        return changed;
    }

    static PropagatingTypeAnalysis* doAnalysis(SpeculationLevel speculation, TypeMap&& initial_types,
                                               CFGBlock* initial_block) {
        Timer _t("PropagatingTypeAnalysis::doAnalysis()");

        CFG* cfg = initial_block->cfg;
        auto&& vreg_info = cfg->getVRegInfo();
        int num_vregs = vreg_info.getTotalNumOfVRegs();
        assert(initial_types.numVregs() == num_vregs);

        AllTypeMap starting_types;
        ExprTypeMap expr_types;
        TypeSpeculations type_speculations;

        llvm::SmallPtrSet<CFGBlock*, 32> in_queue;
        std::priority_queue<CFGBlock*, llvm::SmallVector<CFGBlock*, 32>, CFGBlockMinIndex> queue;

        starting_types.insert(std::make_pair(initial_block, std::move(initial_types)));
        queue.push(initial_block);
        in_queue.insert(initial_block);

        int num_evaluations = 0;
        while (!queue.empty()) {
            ASSERT(queue.size() == in_queue.size(), "%ld %d", queue.size(), in_queue.size());
            num_evaluations++;
            CFGBlock* block = queue.top();
            queue.pop();
            in_queue.erase(block);
            assert(starting_types.count(block));

            if (VERBOSITY("types") >= 3) {
                printf("processing types for block %d\n", block->idx);
            }
            if (VERBOSITY("types") >= 3) {
                printf("before:\n");
                TypeMap& starting = starting_types.find(block)->second;
                for (const auto& p : starting) {
                    auto name = vreg_info.getName(p.first);
                    printf("%s: %s\n", name.c_str(), p.second->debugName().c_str());
                }
            }

            TypeMap ending = BasicBlockTypePropagator::propagate(block, starting_types.find(block)->second, expr_types,
                                                                 type_speculations, speculation);

            if (VERBOSITY("types") >= 3) {
                printf("before (after):\n");
                TypeMap& starting = starting_types.find(block)->second;
                for (const auto& p : starting) {
                    auto name = vreg_info.getName(p.first);
                    printf("%s: %s\n", name.c_str(), p.second->debugName().c_str());
                }
                printf("after:\n");
                for (const auto& p : ending) {
                    auto name = vreg_info.getName(p.first);
                    printf("%s: %s\n", name.c_str(), p.second->debugName().c_str());
                }
            }

            for (int i = 0; i < block->successors.size(); i++) {
                CFGBlock* next_block = block->successors[i];
                bool first = (starting_types.count(next_block) == 0);
                if (first)
                    starting_types.insert(std::make_pair(next_block, TypeMap(num_vregs)));
                bool changed = merge(ending, starting_types.find(next_block)->second);
                if ((first || changed) && in_queue.insert(next_block).second) {
                    queue.push(next_block);
                }
            }
        }

        if (VERBOSITY("types") >= 2) {
            printf("Type analysis: %d BBs, %d evaluations = %.1f evaluations/block\n", starting_types.size(),
                   num_evaluations, 1.0 * num_evaluations / starting_types.size());
        }

        if (VERBOSITY("types") >= 3) {
            for (const auto& p : starting_types) {
                auto b = p.first;
                printf("Types at beginning of block %d:\n", b->idx);

                const TypeMap& starting = p.second;
                for (const auto& p : starting) {
                    auto name = vreg_info.getName(p.first);
                    printf("%s: %s\n", name.c_str(), p.second->debugName().c_str());
                }
            }
        }

        static StatCounter us_types("us_compiling_analysis_types");
        us_types.log(_t.end());

        return new PropagatingTypeAnalysis(starting_types, expr_types, type_speculations, speculation);
    }
};


// public entry point:
TypeAnalysis* doTypeAnalysis(CFG* cfg, const ParamNames& arg_names, const std::vector<ConcreteCompilerType*>& arg_types,
                             EffortLevel effort, TypeAnalysis::SpeculationLevel speculation) {
    // if (effort == EffortLevel::INTERPRETED) {
    // return new NullTypeAnalysis();
    //}
    assert(arg_names.totalParameters() == arg_types.size());

    TypeMap initial_types(cfg->getVRegInfo().getTotalNumOfVRegs());
    int i = 0;
    for (BST_Name* n : arg_names.allArgsAsName()) {
        ScopeInfo::VarScopeType vst = n->lookup_type;
        assert(vst != ScopeInfo::VarScopeType::UNKNOWN);
        assert(vst != ScopeInfo::VarScopeType::GLOBAL); // global-and-local error
        if (vst != ScopeInfo::VarScopeType::NAME)
            initial_types[n->vreg] = unboxedType(arg_types[i]);
        ++i;
    };

    assert(i == arg_types.size());

    return PropagatingTypeAnalysis::doAnalysis(speculation, std::move(initial_types), cfg->getStartingBlock());
}

TypeAnalysis* doTypeAnalysis(const OSREntryDescriptor* entry_descriptor, EffortLevel effort,
                             TypeAnalysis::SpeculationLevel speculation) {
    auto cfg = entry_descriptor->code->source->cfg;
    auto&& vreg_info = cfg->getVRegInfo();
    TypeMap initial_types(vreg_info.getTotalNumOfVRegs());

    for (auto&& p : entry_descriptor->args) {
        initial_types[p.first] = p.second;
    }

    return PropagatingTypeAnalysis::doAnalysis(speculation, std::move(initial_types),
                                               entry_descriptor->backedge->target);
}
}
