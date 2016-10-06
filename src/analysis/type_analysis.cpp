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

    BoxedClass* speculatedExprClass(BST_stmt_with_dest*) override { return NULL; }
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

    return predictClassFor(node);
}

typedef VRegMap<CompilerType*> TypeMap;
typedef llvm::DenseMap<CFGBlock*, TypeMap> AllTypeMap;
typedef llvm::DenseMap<BST_stmt*, CompilerType*> ExprTypeMap;
typedef llvm::DenseMap<BST_stmt*, BoxedClass*> TypeSpeculations;
class BasicBlockTypePropagator : public StmtVisitor {
private:
    static const bool EXPAND_UNNEEDED = true;

    CFGBlock* block;
    TypeMap& sym_table;
    ExprTypeMap& expr_types;
    TypeSpeculations& type_speculations;
    TypeAnalysis::SpeculationLevel speculation;

    BasicBlockTypePropagator(CFGBlock* block, TypeMap& initial, ExprTypeMap& expr_types,
                             TypeSpeculations& type_speculations, TypeAnalysis::SpeculationLevel speculation,
                             const CodeConstants& code_constants)
        : StmtVisitor(code_constants),
          block(block),
          sym_table(initial),
          expr_types(expr_types),
          type_speculations(type_speculations),
          speculation(speculation) {}

    void run() {
        for (int i = 0; i < block->body.size(); i++) {
            block->body[i]->accept_stmt(this);
        }
    }

    CompilerType* processSpeculation(BoxedClass* speculated_cls, BST_stmt* node, CompilerType* old_type) {
        assert(old_type);
        assert(speculation != TypeAnalysis::NONE);

        if (speculated_cls != NULL && speculated_cls->is_constant) {
            CompilerType* speculated_type = unboxedType(typeFromClass(speculated_cls));
            if (!old_type->canConvertTo(speculated_type)) {
                if (VERBOSITY() >= 2) {
                    printf("in propagator, speculating that %s would actually be %s, at ",
                           old_type->debugName().c_str(), speculated_type->debugName().c_str());
                    fflush(stdout);
                    print_bst(node, code_constants);
                    llvm::outs().flush();
                    printf("\n");
                }

                type_speculations[node] = speculated_cls;
                return speculated_type;
            }
        }
        return old_type;
    }

    CompilerType* getConstantType(int vreg) {
        Box* o = code_constants.getConstant(vreg);
        if (o->cls == int_cls)
            return INT;
        else if (o->cls == float_cls)
            return FLOAT;
        else if (o->cls == complex_cls)
            return BOXED_COMPLEX;
        else if (o->cls == long_cls)
            return LONG;
        else if (o->cls == str_cls)
            return STR;
        else if (o->cls == unicode_cls)
            return typeFromClass(unicode_cls);
        else if (o->cls == none_cls)
            return NONE;
        else if (o->cls == ellipsis_cls)
            return typeFromClass(ellipsis_cls);
        else
            RELEASE_ASSERT(0, "");
    }

    CompilerType* getType(int vreg) {
        if (vreg == VREG_UNDEFINED)
            return UNDEF;
        if (vreg < 0)
            return getConstantType(vreg);
        CompilerType*& t = sym_table[vreg];
        if (t == NULL) {
            // if (VERBOSITY() >= 2) {
            // printf("%s is undefined!\n", node->id.c_str());
            // raise(SIGTRAP);
            //}
            t = UNDEF;
        }
        return t;
    }

    void _doSet(int vreg, CompilerType* t) {
        assert(t->isUsable());
        if (t && vreg != VREG_UNDEFINED)
            sym_table[vreg] = t;
    }

    bool hasFixedOps(CompilerType* type) {
        // This is non-exhaustive:
        return type == STR || type == INT || type == FLOAT || type == LIST || type == DICT;
    }

    CompilerType* visit_augbinopHelper(BST_AugBinOp* node) {
        CompilerType* left = getType(node->vreg_left);
        CompilerType* right = getType(node->vreg_right);
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
    void visit_augbinop(BST_AugBinOp* node) override {
        CompilerType* t = visit_augbinopHelper(node);
        _doSet(node->vreg_dst, t);
    }

    CompilerType* visit_binopHelper(BST_BinOp* node) {
        CompilerType* left = getType(node->vreg_left);
        CompilerType* right = getType(node->vreg_right);
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
    void visit_binop(BST_BinOp* node) override {
        CompilerType* t = visit_binopHelper(node);
        _doSet(node->vreg_dst, t);
    }

    template <typename CallType> CompilerType* visit_callHelper(CompilerType* func, CallType* node) {
        std::vector<CompilerType*> arg_types;
        for (int i = 0; i < node->num_args; i++) {
            arg_types.push_back(getType(node->elts[i]));
        }

        for (int i = 0; i < node->num_keywords; i++) {
            getType(node->elts[node->num_args + i]);
        }

        CompilerType* starargs = node->vreg_starargs != VREG_UNDEFINED ? getType(node->vreg_starargs) : NULL;
        CompilerType* kwargs = node->vreg_kwargs != VREG_UNDEFINED ? getType(node->vreg_kwargs) : NULL;

        if (starargs || kwargs || node->num_keywords) {
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

    void visit_callfunc(BST_CallFunc* node) override {
        CompilerType* func = getType(node->vreg_func);
        _doSet(node->vreg_dst, visit_callHelper(func, node));
    }

    void visit_callattr(BST_CallAttr* node) override {
        CompilerType* t = getType(node->vreg_value);
        CompilerType* func = t->getattrType(node->attr, false);

        if (VERBOSITY() >= 2 && func == UNDEF) {
            printf("Think %s.%s is undefined, at %d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno);
            print_bst(node, code_constants);
            printf("\n");
        }

        _doSet(node->vreg_dst, visit_callHelper(func, node));
    }

    void visit_callclsattr(BST_CallClsAttr* node) override {
        CompilerType* t = getType(node->vreg_value);
        CompilerType* func = t->getattrType(node->attr, true);

        if (VERBOSITY() >= 2 && func == UNDEF) {
            printf("Think %s.%s is undefined, at %d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno);
            print_bst(node, code_constants);
            printf("\n");
        }

        _doSet(node->vreg_dst, visit_callHelper(func, node));
    }

    CompilerType* visit_compareHelper(BST_Compare* node) {
        CompilerType* left = getType(node->vreg_left);
        CompilerType* right = getType(node->vreg_comparator);

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
    void visit_compare(BST_Compare* node) override { _doSet(node->vreg_dst, visit_compareHelper(node)); }

    void visit_dict(BST_Dict* node) override { _doSet(node->vreg_dst, DICT); }

    void visit_landingpad(BST_Landingpad* node) override {
        _doSet(node->vreg_dst, makeTupleType({ UNKNOWN, UNKNOWN, UNKNOWN }));
    }
    void visit_locals(BST_Locals* node) override { _doSet(node->vreg_dst, DICT); }
    void visit_getiter(BST_GetIter* node) override {
        _doSet(node->vreg_dst, getType(node->vreg_value)->getPystonIterType());
    }
    void visit_importfrom(BST_ImportFrom* node) override { _doSet(node->vreg_dst, UNKNOWN); }
    void visit_importname(BST_ImportName* node) override { _doSet(node->vreg_dst, UNKNOWN); }
    void visit_importstar(BST_ImportStar* node) override { _doSet(node->vreg_dst, UNKNOWN); }
    void visit_nonzero(BST_Nonzero* node) override { return _doSet(node->vreg_dst, UNKNOWN); }
    void visit_checkexcmatch(BST_CheckExcMatch* node) override { return _doSet(node->vreg_dst, UNKNOWN); }
    void visit_setexcinfo(BST_SetExcInfo* node) override {}
    void visit_uncacheexcinfo(BST_UncacheExcInfo* node) override {}
    void visit_hasnext(BST_HasNext* node) override { return _doSet(node->vreg_dst, BOOL); }
    void visit_printexpr(BST_PrintExpr* node) override {}

    void visit_list(BST_List* node) override {
        // Get all the sub-types, even though they're not necessary to
        // determine the expression type, so that things like speculations
        // can be processed.
        for (int i = 0; i < node->num_elts; ++i) {
            getType(node->elts[i]);
        }

        _doSet(node->vreg_dst, LIST);
    }

    void visit_repr(BST_Repr* node) override { _doSet(node->vreg_dst, STR); }

    void visit_set(BST_Set* node) override { _doSet(node->vreg_dst, SET); }

    void visit_makeslice(BST_MakeSlice* node) override { _doSet(node->vreg_dst, SLICE); }

    void visit_storename(BST_StoreName* node) override {
        assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
        if (node->lookup_type == ScopeInfo::VarScopeType::FAST
            || node->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
            _doSet(node->vreg, getType(node->vreg_value));
        } else
            assert(node->vreg == VREG_UNDEFINED);
    }
    void visit_storeattr(BST_StoreAttr* node) override {}
    void visit_storesub(BST_StoreSub* node) override {}
    void visit_storesubslice(BST_StoreSubSlice* node) override {}

    void visit_loadname(BST_LoadName* node) override {
        CompilerType* t = UNKNOWN;
        assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
        auto name_scope = node->lookup_type;

        if (name_scope == ScopeInfo::VarScopeType::GLOBAL) {
            if (node->id.s() == "None")
                t = NONE;
        } else if (name_scope == ScopeInfo::VarScopeType::FAST || name_scope == ScopeInfo::VarScopeType::CLOSURE)
            t = getType(node->vreg);

        _doSet(node->vreg_dst, t);
    }

    void visit_loadattr(BST_LoadAttr* node) override {
        CompilerType* t = getType(node->vreg_value);
        CompilerType* rtn = t->getattrType(node->attr, node->clsonly);

        if (speculation != TypeAnalysis::NONE) {
            BoxedClass* speculated_class = predictClassFor(node);
            rtn = processSpeculation(speculated_class, node, rtn);
        }

        if (VERBOSITY() >= 2 && rtn == UNDEF) {
            printf("Think %s.%s is undefined, at %d\n", t->debugName().c_str(), node->attr.c_str(), node->lineno);
            print_bst(node, code_constants);
            printf("\n");
        }
        _doSet(node->vreg_dst, rtn);
    }

    void visit_loadsub(BST_LoadSub* node) override {
        CompilerType* val = getType(node->vreg_value);
        CompilerType* slice = getType(node->vreg_slice);
        static BoxedString* name = getStaticString("__getitem__");
        CompilerType* getitem_type = val->getattrType(name, true);
        std::vector<CompilerType*> args;
        args.push_back(slice);
        _doSet(node->vreg_dst, getitem_type->callType(ArgPassSpec(1), args, NULL));
    }

    void visit_loadsubslice(BST_LoadSubSlice* node) override {
        // TODO this is not 100% correct, it should call into __getslice__ but I think for a all static types we
        // shupport it does not make a difference
        CompilerType* val = getType(node->vreg_value);
        static BoxedString* name = getStaticString("__getitem__");
        CompilerType* getitem_type = val->getattrType(name, true);
        std::vector<CompilerType*> args;
        args.push_back(SLICE);
        _doSet(node->vreg_dst, getitem_type->callType(ArgPassSpec(1), args, NULL));
    }

    void visit_tuple(BST_Tuple* node) override {
        std::vector<CompilerType*> elt_types;
        for (int i = 0; i < node->num_elts; i++) {
            elt_types.push_back(getType(node->elts[i]));
        }
        _doSet(node->vreg_dst, makeTupleType(elt_types));
    }

    void visit_unaryop(BST_UnaryOp* node) override {
        CompilerType* operand = getType(node->vreg_operand);
        if (!hasFixedOps(operand)) {
            _doSet(node->vreg_dst, UNKNOWN);
            return;
        }

        // TODO this isn't the exact behavior
        BoxedString* name = getOpName(node->op_type);
        CompilerType* attr_type = operand->getattrType(name, true);

        if (attr_type == UNDEF)
            attr_type = UNKNOWN;

        std::vector<CompilerType*> arg_types;
        CompilerType* rtn_type = attr_type->callType(ArgPassSpec(0), arg_types, NULL);
        rtn_type = unboxedType(rtn_type->getConcreteType());
        _doSet(node->vreg_dst, rtn_type);
    }

    void visit_unpackintoarray(BST_UnpackIntoArray* node) override {
        auto src_types = getType(node->vreg_src)->unpackTypes(node->num_elts);
        assert(src_types.size() == node->num_elts);
        for (int i = 0; i < node->num_elts; i++) {
            _doSet(node->vreg_dst[i], src_types[i]);
        }
    }

    void visit_yield(BST_Yield* node) override { _doSet(node->vreg_dst, UNKNOWN); }


    void visit_assert(BST_Assert* node) override { getType(node->vreg_msg); }

    void visit_copyvreg(BST_CopyVReg* node) override {
        CompilerType* t = getType(node->vreg_src);
        _doSet(node->vreg_dst, t);
    }

    void visit_branch(BST_Branch* node) override {
        if (EXPAND_UNNEEDED) {
            getType(node->vreg_test);
        }
    }

    void visit_makeclass(BST_MakeClass* mkclass) override {
        BST_ClassDef* node = mkclass->class_def;

        for (int i = 0; i < node->num_decorator; ++i) {
            getType(node->decorator[i]);
        }

        getType(node->vreg_bases_tuple);

        // TODO should we speculate that classdefs will generally return a class?
        // return typeFromClass(type_cls);
        _doSet(mkclass->vreg_dst, UNKNOWN);
    }

    void visit_deletesub(BST_DeleteSub* node) override { getType(node->vreg_value); }
    void visit_deletesubslice(BST_DeleteSubSlice* node) override { getType(node->vreg_value); }
    void visit_deleteattr(BST_DeleteAttr* node) override { getType(node->vreg_value); }
    void visit_deletename(BST_DeleteName* node) override {
        assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
        if (node->lookup_type == ScopeInfo::VarScopeType::FAST
            || node->lookup_type == ScopeInfo::VarScopeType::CLOSURE) {
            sym_table[node->vreg] = NULL;
        } else
            assert(node->vreg == VREG_UNDEFINED);
    }

    void visit_makefunction(BST_MakeFunction* mkfn) override {
        BST_FunctionDef* node = mkfn->function_def;

        for (int i = 0; i < node->num_defaults + node->num_decorator; ++i) {
            getType(node->elts[i]);
        }

        CompilerType* t = UNKNOWN;
        if (node->num_decorator == 0)
            t = typeFromClass(function_cls);
        _doSet(mkfn->vreg_dst, t);
    }

    void visit_exec(BST_Exec* node) override {
        getType(node->vreg_body);
        getType(node->vreg_globals);
        getType(node->vreg_locals);
    }

    void visit_invoke(BST_Invoke* node) override { node->stmt->accept_stmt(this); }

    void visit_jump(BST_Jump* node) override {}

    void visit_print(BST_Print* node) override {
        getType(node->vreg_dest);

        if (EXPAND_UNNEEDED)
            getType(node->vreg_value);
    }

    void visit_raise(BST_Raise* node) override {
        if (EXPAND_UNNEEDED) {
            getType(node->vreg_arg0);
            getType(node->vreg_arg1);
            getType(node->vreg_arg2);
        }
    }

    void visit_return(BST_Return* node) override {
        if (EXPAND_UNNEEDED) {
            getType(node->vreg_value);
        }
    }

public:
    static TypeMap propagate(CFGBlock* block, const TypeMap& starting, ExprTypeMap& expr_types,
                             TypeSpeculations& type_speculations, TypeAnalysis::SpeculationLevel speculation,
                             const CodeConstants& code_constants) {
        TypeMap ending = starting;
        BasicBlockTypePropagator(block, ending, expr_types, type_speculations, speculation, code_constants).run();
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

    BoxedClass* speculatedExprClass(BST_stmt_with_dest* call) override { return type_speculations[call]; }

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
                                               CFGBlock* initial_block, const CodeConstants& code_constants) {
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
                                                                 type_speculations, speculation, code_constants);

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
                             EffortLevel effort, TypeAnalysis::SpeculationLevel speculation,
                             const CodeConstants& code_constants) {
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

    return PropagatingTypeAnalysis::doAnalysis(speculation, std::move(initial_types), cfg->getStartingBlock(),
                                               code_constants);
}

TypeAnalysis* doTypeAnalysis(const OSREntryDescriptor* entry_descriptor, EffortLevel effort,
                             TypeAnalysis::SpeculationLevel speculation, const CodeConstants& code_constants) {
    auto cfg = entry_descriptor->code->source->cfg;
    auto&& vreg_info = cfg->getVRegInfo();
    TypeMap initial_types(vreg_info.getTotalNumOfVRegs());

    for (auto&& p : entry_descriptor->args) {
        initial_types[p.first] = p.second;
    }

    return PropagatingTypeAnalysis::doAnalysis(speculation, std::move(initial_types),
                                               entry_descriptor->backedge->target, code_constants);
}
}
