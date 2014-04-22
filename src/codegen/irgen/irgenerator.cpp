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


#include "llvm/IR/Module.h"

#include "core/ast.h"
#include "core/cfg.h"
#include "core/util.h"

#include "codegen/compvars.h"
#include "codegen/osrentry.h"
#include "codegen/patchpoints.h"
#include "codegen/type_recording.h"

#include "codegen/irgen.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"

#include "runtime/objmodel.h"
#include "runtime/types.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "analysis/type_analysis.h"

namespace pyston {

llvm::Value* IRGenState::getScratchSpace(int min_bytes) {
    llvm::BasicBlock &entry_block = getLLVMFunction()->getEntryBlock();

    if (scratch_space) {
        assert(scratch_space->getParent() == &entry_block);
        assert(scratch_space->isStaticAlloca());
        if (scratch_size >= min_bytes)
            return scratch_space;
    }

    // Not sure why, but LLVM wants to canonicalize an alloca into an array alloca (assuming
    // the alloca is static); just to keep things straightforward, let's do that here:
    llvm::Type* array_type = llvm::ArrayType::get(g.i8, min_bytes);

    llvm::AllocaInst* new_scratch_space;
    // If the entry block is currently empty, we have to be more careful:
    if (entry_block.begin() == entry_block.end()) {
        new_scratch_space = new llvm::AllocaInst(array_type, getConstantInt(1, g.i64), "scratch", &entry_block);
    } else {
        new_scratch_space = new llvm::AllocaInst(array_type, getConstantInt(1, g.i64), "scratch", entry_block.getFirstInsertionPt());
    }
    assert(new_scratch_space->isStaticAlloca());

    if (scratch_space)
        scratch_space->replaceAllUsesWith(new_scratch_space);

    scratch_size = min_bytes;
    scratch_space = new_scratch_space;

    return scratch_space;
}

ScopeInfo* IRGenState::getScopeInfo() {
    SourceInfo *source = getSourceInfo();
    return source->scoping->getScopeInfoForNode(source->ast);
}

GuardList::ExprTypeGuard::ExprTypeGuard(CFGBlock *cfg_block, llvm::BranchInst* branch, AST_expr* ast_node, CompilerVariable* val, const SymbolTable &st) :
        cfg_block(cfg_block), branch(branch), ast_node(ast_node) {
    DupCache cache;
    this->val = val->dup(cache);

    for (auto p : st) {
        this->st[p.first] = p.second->dup(cache);
    }
}

GuardList::BlockEntryGuard::BlockEntryGuard(CFGBlock *cfg_block, llvm::BranchInst* branch, const SymbolTable &symbol_table) :
        cfg_block(cfg_block), branch(branch) {
    DupCache cache;
    for (auto p : symbol_table) {
        this->symbol_table[p.first] = p.second->dup(cache);
    }
}

class IREmitterImpl : public IREmitter {
    private:
        IRGenState *irstate;
        IRBuilder *builder;

    public:
        explicit IREmitterImpl(IRGenState *irstate) : irstate(irstate), builder(new IRBuilder(g.context)) {
            builder->setEmitter(this);
        }

        IRBuilder* getBuilder() override {
            return builder;
        }

        GCBuilder* getGC() override {
            return irstate->getGC();
        }

        llvm::Function* getIntrinsic(llvm::Intrinsic::ID intrinsic_id) override {
            return llvm::Intrinsic::getDeclaration(g.cur_module, intrinsic_id);
        }

        CompiledFunction* currentFunction() override {
            return irstate->getCurFunction();
        }

        llvm::Value* createPatchpoint(const PatchpointSetupInfo* pp, void* func_addr, const std::vector<llvm::Value*> &args) override {
            assert(irstate->getEffortLevel() != EffortLevel::INTERPRETED);

            std::vector<llvm::Value*> pp_args;
            pp_args.push_back(getConstantInt(pp->getPatchpointId(), g.i64));
            pp_args.push_back(getConstantInt(pp->totalSize(), g.i32));
            pp_args.push_back(embedConstantPtr(func_addr, g.i8->getPointerTo()));
            pp_args.push_back(getConstantInt(args.size(), g.i32));

            pp_args.insert(pp_args.end(), args.begin(), args.end());

            int num_scratch_bytes = pp->numScratchBytes();
            if (num_scratch_bytes) {
                llvm::Value* scratch_space = irstate->getScratchSpace(num_scratch_bytes);
                pp_args.push_back(scratch_space);
            }

            llvm::Intrinsic::ID intrinsic_id = pp->hasReturnValue() ? llvm::Intrinsic::experimental_patchpoint_i64 : llvm::Intrinsic::experimental_patchpoint_void;
            llvm::Function *patchpoint = this->getIntrinsic(intrinsic_id);
            llvm::CallInst* rtn = this->getBuilder()->CreateCall(patchpoint, pp_args);

            /*
            static int n = 0;
            n++;
            if (n >= 3)
                rtn->setCallingConv(llvm::CallingConv::PreserveAll);
            */
            rtn->setCallingConv(pp->getCallingConvention());

            // Not sure why this doesn't work:
            //rtn->setCallingConv(llvm::CallingConv::AnyReg);

            return rtn;
        }
};
IREmitter* createIREmitter(IRGenState *irstate) {
    return new IREmitterImpl(irstate);
}

class IRGeneratorImpl : public IRGenerator {
    private:
        IRGenState *irstate;

        IREmitterImpl emitter;
        SymbolTable symbol_table;
        std::vector<llvm::BasicBlock*> &entry_blocks;
        llvm::BasicBlock *curblock;
        CFGBlock *myblock;
        TypeAnalysis *types;
        GuardList &out_guards;
        const GuardList &in_guards;

        enum State {
            PARTIAL, // running through a partial block, waiting to hit the first in_guard
            RUNNING, // normal
            DEAD, // passed a Return statement; still syntatically valid but the code should not be compiled
            FINISHED, // passed a pseudo-node such as Branch or Jump; internal error if there are any more statements
        } state;

    public:
        IRGeneratorImpl(IRGenState *irstate, std::vector<llvm::BasicBlock*> &entry_blocks, CFGBlock *myblock, TypeAnalysis *types, GuardList &out_guards, const GuardList &in_guards, bool is_partial) : irstate(irstate), emitter(irstate), entry_blocks(entry_blocks), myblock(myblock), types(types), out_guards(out_guards), in_guards(in_guards), state(is_partial ? PARTIAL : RUNNING) {
            llvm::BasicBlock* entry_block = entry_blocks[myblock->idx];
            emitter.getBuilder()->SetInsertPoint(entry_block);
            curblock = entry_block;
        }

        ~IRGeneratorImpl() {
            delete emitter.getBuilder();
        }

    private:
        OpInfo getOpInfoForNode(AST* ast) {
            assert(ast);

            EffortLevel::EffortLevel effort = irstate->getEffortLevel();
            bool record_types = (effort != EffortLevel::INTERPRETED && effort != EffortLevel::MAXIMAL);

            TypeRecorder *type_recorder;
            if (record_types) {
                type_recorder = getTypeRecorderForNode(ast);
            } else {
                type_recorder = NULL;
            }

            return OpInfo(irstate->getEffortLevel(), type_recorder);
        }

        OpInfo getEmptyOpInfo() {
            return OpInfo(irstate->getEffortLevel(), NULL);
        }

        void createExprTypeGuard(llvm::Value *check_val, AST_expr* node, CompilerVariable* node_value) {
            assert(check_val->getType() == g.i1);

            llvm::Value* md_vals[] = {llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1000), getConstantInt(1)};
            llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));

            // For some reason there doesn't seem to be the ability to place the new BB
            // right after the current bb (can only place it *before* something else),
            // but we can put it somewhere arbitrary and then move it.
            llvm::BasicBlock* success_bb = llvm::BasicBlock::Create(g.context, "check_succeeded", irstate->getLLVMFunction());
            success_bb->moveAfter(curblock);

            // Create the guard with both branches leading to the success_bb,
            // and let the deopt path change the failure case to point to the
            // as-yet-unknown deopt block.
            // TODO Not the best approach since if we fail to do that patching,
            // the guard will just silently be ignored.
            llvm::BranchInst* guard = emitter.getBuilder()->CreateCondBr(check_val, success_bb, success_bb, branch_weights);

            curblock = success_bb;
            emitter.getBuilder()->SetInsertPoint(curblock);

            out_guards.addExprTypeGuard(myblock, guard, node, node_value, symbol_table);
        }

        CompilerVariable* evalAttribute(AST_Attribute *node) {
            assert(state != PARTIAL);

            CompilerVariable *value = evalExpr(node->value);

            CompilerVariable *rtn = value->getattr(emitter, getOpInfoForNode(node), &node->attr, false);
            value->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalClsAttribute(AST_ClsAttribute *node) {
            assert(state != PARTIAL);

            CompilerVariable *value = evalExpr(node->value);
            CompilerVariable *rtn = value->getattr(emitter, getOpInfoForNode(node), &node->attr, true);
            value->decvref(emitter);
            return rtn;
        }

        enum BinExpType {
            AugBinOp,
            BinOp,
            Compare,
        };
        CompilerVariable* _evalBinExp(AST* node, CompilerVariable *left, CompilerVariable *right, AST_TYPE::AST_TYPE type, BinExpType exp_type) {
            assert(state != PARTIAL);

            assert(left);
            assert(right);

            if (left->getType() == INT && right->getType() == INT) {
                ConcreteCompilerVariable *converted_left = left->makeConverted(emitter, INT);
                ConcreteCompilerVariable *converted_right = right->makeConverted(emitter, INT);
                llvm::Value *v;
                if (type == AST_TYPE::Mod) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.mod_i64_i64, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Div || type == AST_TYPE::FloorDiv) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.div_i64_i64, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Pow) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.pow_i64_i64, converted_left->getValue(), converted_right->getValue());
                } else if (exp_type == BinOp || exp_type == AugBinOp) {
                    llvm::Instruction::BinaryOps binopcode;
                    switch (type) {
                        case AST_TYPE::Add:
                            binopcode = llvm::Instruction::Add;
                            break;
                        case AST_TYPE::BitAnd:
                            binopcode = llvm::Instruction::And;
                            break;
                        case AST_TYPE::BitOr:
                            binopcode = llvm::Instruction::Or;
                            break;
                        case AST_TYPE::BitXor:
                            binopcode = llvm::Instruction::Xor;
                            break;
                        case AST_TYPE::LShift:
                            binopcode = llvm::Instruction::Shl;
                            break;
                        case AST_TYPE::RShift:
                            binopcode = llvm::Instruction::AShr;
                            break;
                        case AST_TYPE::Mult:
                            binopcode = llvm::Instruction::Mul;
                            break;
                        case AST_TYPE::Sub:
                            binopcode = llvm::Instruction::Sub;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }
                    v = emitter.getBuilder()->CreateBinOp(binopcode, converted_left->getValue(), converted_right->getValue());
                } else {
                    assert(exp_type == Compare);
                    llvm::CmpInst::Predicate cmp_pred;
                    switch(type) {
                        case AST_TYPE::Eq:
                        case AST_TYPE::Is:
                            cmp_pred = llvm::CmpInst::ICMP_EQ;
                            break;
                        case AST_TYPE::Lt:
                            cmp_pred = llvm::CmpInst::ICMP_SLT;
                            break;
                        case AST_TYPE::LtE:
                            cmp_pred = llvm::CmpInst::ICMP_SLE;
                            break;
                        case AST_TYPE::Gt:
                            cmp_pred = llvm::CmpInst::ICMP_SGT;
                            break;
                        case AST_TYPE::GtE:
                            cmp_pred = llvm::CmpInst::ICMP_SGE;
                            break;
                        case AST_TYPE::NotEq:
                        case AST_TYPE::IsNot:
                            cmp_pred = llvm::CmpInst::ICMP_NE;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }
                    v = emitter.getBuilder()->CreateICmp(cmp_pred, converted_left->getValue(), converted_right->getValue());
                }
                converted_left->decvref(emitter);
                converted_right->decvref(emitter);
                assert(v->getType() == g.i64 || v->getType() == g.i1);
                return new ConcreteCompilerVariable(v->getType() == g.i64 ? INT : BOOL, v, true);
            }

            if (left->getType() == FLOAT && (right->getType() == FLOAT || right->getType() == INT)) {
                ConcreteCompilerVariable *converted_left = left->makeConverted(emitter, FLOAT);

                ConcreteCompilerVariable *converted_right;
                if (right->getType() == FLOAT) {
                    converted_right = right->makeConverted(emitter, FLOAT);
                } else {
                    converted_right = right->makeConverted(emitter, INT);
                    llvm::Value* conv = emitter.getBuilder()->CreateSIToFP(converted_right->getValue(), g.double_);
                    converted_right->decvref(emitter);
                    converted_right = new ConcreteCompilerVariable(FLOAT, conv, true);
                }
                llvm::Value *v;
                bool succeeded = true;
                if (type == AST_TYPE::Mod) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.mod_float_float, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Div || type == AST_TYPE::FloorDiv) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.div_float_float, converted_left->getValue(), converted_right->getValue());
                } else if (type == AST_TYPE::Pow) {
                    v = emitter.getBuilder()->CreateCall2(g.funcs.pow_float_float, converted_left->getValue(), converted_right->getValue());
                } else if (exp_type == BinOp || exp_type == AugBinOp) {
                    llvm::Instruction::BinaryOps binopcode;
                    switch (type) {
                        case AST_TYPE::Add:
                            binopcode = llvm::Instruction::FAdd;
                            break;
                        case AST_TYPE::Mult:
                            binopcode = llvm::Instruction::FMul;
                            break;
                        case AST_TYPE::Sub:
                            binopcode = llvm::Instruction::FSub;
                            break;
                        case AST_TYPE::BitAnd:
                        case AST_TYPE::BitOr:
                        case AST_TYPE::BitXor:
                        case AST_TYPE::LShift:
                        case AST_TYPE::RShift:
                            succeeded = false;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }

                    if (succeeded) {
                        v = emitter.getBuilder()->CreateBinOp(binopcode, converted_left->getValue(), converted_right->getValue());
                    }
                } else {
                    assert(exp_type == Compare);
                    llvm::CmpInst::Predicate cmp_pred;
                    switch(type) {
                        case AST_TYPE::Eq:
                        case AST_TYPE::Is:
                            cmp_pred = llvm::CmpInst::FCMP_OEQ;
                            break;
                        case AST_TYPE::Lt:
                            cmp_pred = llvm::CmpInst::FCMP_OLT;
                            break;
                        case AST_TYPE::LtE:
                            cmp_pred = llvm::CmpInst::FCMP_OLE;
                            break;
                        case AST_TYPE::Gt:
                            cmp_pred = llvm::CmpInst::FCMP_OGT;
                            break;
                        case AST_TYPE::GtE:
                            cmp_pred = llvm::CmpInst::FCMP_OGE;
                            break;
                        case AST_TYPE::NotEq:
                        case AST_TYPE::IsNot:
                            cmp_pred = llvm::CmpInst::FCMP_UNE;
                            break;
                        default:
                            ASSERT(0, "%s", getOpName(type).c_str());
                            abort();
                            break;
                    }
                    v = emitter.getBuilder()->CreateFCmp(cmp_pred, converted_left->getValue(), converted_right->getValue());
                }
                converted_left->decvref(emitter);
                converted_right->decvref(emitter);

                if (succeeded) {
                    assert(v->getType() == g.double_ || v->getType() == g.i1);
                    return new ConcreteCompilerVariable(v->getType() == g.double_ ? FLOAT : BOOL, v, true);
                }
            }
            //ASSERT(left->getType() == left->getBoxType() || right->getType() == right->getBoxType(), "%s %s",
                    //left->getType()->debugName().c_str(), right->getType()->debugName().c_str());

            ConcreteCompilerVariable *boxed_left = left->makeConverted(emitter, left->getBoxType());
            ConcreteCompilerVariable *boxed_right = right->makeConverted(emitter, right->getBoxType());

            llvm::Value* rtn;
            bool do_patchpoint = ENABLE_ICBINEXPS && (irstate ->getEffortLevel() != EffortLevel::INTERPRETED);

            llvm::Value *rt_func;
            void* rt_func_addr;
            if (exp_type == BinOp) {
                rt_func = g.funcs.binop;
                rt_func_addr = (void*)binop;
            } else if (exp_type == AugBinOp) {
                rt_func = g.funcs.augbinop;
                rt_func_addr = (void*)augbinop;
            } else {
                rt_func = g.funcs.compare;
                rt_func_addr = (void*)compare;
            }

            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createBinexpPatchpoint(emitter.currentFunction(), getOpInfoForNode(node).getTypeRecorder());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(boxed_left->getValue());
                llvm_args.push_back(boxed_right->getValue());
                llvm_args.push_back(getConstantInt(type, g.i32));

                llvm::Value* uncasted = emitter.createPatchpoint(pp, rt_func_addr, llvm_args);
                rtn = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            } else {

                rtn = emitter.getBuilder()->CreateCall3(rt_func, boxed_left->getValue(), boxed_right->getValue(), getConstantInt(type, g.i32));
            }

            boxed_left->decvref(emitter);
            boxed_right->decvref(emitter);

            return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
        }

        CompilerVariable* evalBinOp(AST_BinOp *node) {
            assert(state != PARTIAL);

            CompilerVariable *left = evalExpr(node->left);
            CompilerVariable *right = evalExpr(node->right);

            assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

            CompilerVariable *rtn = this->_evalBinExp(node, left, right, node->op_type, BinOp);
            left->decvref(emitter);
            right->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalAugBinOp(AST_AugBinOp *node) {
            assert(state != PARTIAL);

            CompilerVariable *left = evalExpr(node->left);
            CompilerVariable *right = evalExpr(node->right);

            assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

            CompilerVariable *rtn = this->_evalBinExp(node, left, right, node->op_type, AugBinOp);
            left->decvref(emitter);
            right->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalCompare(AST_Compare *node) {
            assert(state != PARTIAL);

            RELEASE_ASSERT(node->ops.size() == 1, "");

            CompilerVariable *left = evalExpr(node->left);
            CompilerVariable *right = evalExpr(node->comparators[0]);

            assert(left);
            assert(right);

            CompilerVariable *rtn = _evalBinExp(node, left, right, node->ops[0], Compare);
            left->decvref(emitter);
            right->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalCall(AST_Call *node) {
            assert(state != PARTIAL);

            bool is_callattr;
            bool callattr_clsonly = false;
            std::string *attr = NULL;
            CompilerVariable *func;
            if (node->func->type == AST_TYPE::Attribute) {
                is_callattr = true;
                callattr_clsonly = false;
                AST_Attribute* attr_ast = static_cast<AST_Attribute*>(node->func);
                func = evalExpr(attr_ast->value);
                attr = &attr_ast->attr;
            } else if (node->func->type == AST_TYPE::ClsAttribute) {
                is_callattr = true;
                callattr_clsonly = true;
                AST_ClsAttribute* attr_ast = static_cast<AST_ClsAttribute*>(node->func);
                func = evalExpr(attr_ast->value);
                attr = &attr_ast->attr;
            } else {
                is_callattr = false;
                func = evalExpr(node->func);
            }

            std::vector<CompilerVariable*> args;
            for (int i = 0; i < node->args.size(); i++) {
                CompilerVariable *a = evalExpr(node->args[i]);
                args.push_back(a);
            }

            //if (VERBOSITY("irgen") >= 1)
                //_addAnnotation("before_call");

            CompilerVariable *rtn;
            if (is_callattr) {
                rtn = func->callattr(emitter, getOpInfoForNode(node), attr, callattr_clsonly, args);
            } else {
                rtn = func->call(emitter, getOpInfoForNode(node), args);
            }

            func->decvref(emitter);
            for (int i = 0; i < args.size(); i++) {
                args[i]->decvref(emitter);
            }

            //if (VERBOSITY("irgen") >= 1)
                //_addAnnotation("end_of_call");

            return rtn;
        }

        CompilerVariable* evalDict(AST_Dict *node) {
            assert(state != PARTIAL);

            llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
            ConcreteCompilerVariable *rtn = new ConcreteCompilerVariable(DICT, v, true);
            if (node->keys.size()) {
                static const std::string setitem_str("__setitem__");
                CompilerVariable *setitem = rtn->getattr(emitter, getEmptyOpInfo(), &setitem_str, true);
                for (int i = 0; i < node->keys.size(); i++) {
                    CompilerVariable *key = evalExpr(node->keys[i]);
                    CompilerVariable *value = evalExpr(node->values[i]);
                    assert(key);
                    assert(value);

                    std::vector<CompilerVariable*> args;
                    args.push_back(key);
                    args.push_back(value);
                    // TODO could use the internal _listAppend function to avoid incref/decref'ing None
                    CompilerVariable *rtn = setitem->call(emitter, getEmptyOpInfo(), args);
                    rtn->decvref(emitter);

                    key->decvref(emitter);
                    value->decvref(emitter);
                }
                setitem->decvref(emitter);
            }
            return rtn;
        }

        void _addAnnotation(const char* message) {
            llvm::Instruction *inst = emitter.getBuilder()->CreateCall(llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::donothing));
            llvm::Value* md_vals[] = {getConstantInt(0)};
            llvm::MDNode* mdnode = llvm::MDNode::get(g.context, md_vals);
            inst->setMetadata(message, mdnode);
        }

        CompilerVariable* evalIndex(AST_Index *node) {
            assert(state != PARTIAL);

            return evalExpr(node->value);
        }

        CompilerVariable* evalList(AST_List *node) {
            assert(state != PARTIAL);

            std::vector<CompilerVariable*> elts;
            for (int i = 0; i < node->elts.size(); i++) {
                CompilerVariable *value = evalExpr(node->elts[i]);
                elts.push_back(value);
            }

            llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createList);
            ConcreteCompilerVariable *rtn = new ConcreteCompilerVariable(LIST, v, true);

            llvm::Value *f = g.funcs.listAppendInternal;
            llvm::Value *bitcast = emitter.getBuilder()->CreateBitCast(v, *llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(f->getType())->getElementType())->param_begin());

            for (int i = 0; i < node->elts.size(); i++) {
                CompilerVariable *elt = elts[i];
                ConcreteCompilerVariable *converted = elt->makeConverted(emitter, elt->getBoxType());
                elt->decvref(emitter);

                emitter.getBuilder()->CreateCall2(f, bitcast, converted->getValue());
                converted->decvref(emitter);
            }
            return rtn;
        }

        CompilerVariable* getNone() {
            ConcreteCompilerVariable *v = new ConcreteCompilerVariable(typeFromClass(none_cls), embedConstantPtr(None, g.llvm_value_type_ptr), false);
            return v;
        }

        CompilerVariable* evalName(AST_Name *node) {
            assert(state != PARTIAL);

            if (irstate->getScopeInfo()->refersToGlobal(node->id)) {
                if (1) {
                    // Method 1: calls into the runtime getGlobal(), which handles things like falling back to builtins
                    // or raising the correct error message.
                    bool do_patchpoint = ENABLE_ICGETGLOBALS && (irstate ->getEffortLevel() != EffortLevel::INTERPRETED);
                    bool from_global = irstate->getSourceInfo()->ast->type == AST_TYPE::Module;
                    if (do_patchpoint) {
                        PatchpointSetupInfo *pp = patchpoints::createGetGlobalPatchpoint(emitter.currentFunction(), getOpInfoForNode(node).getTypeRecorder());

                        std::vector<llvm::Value*> llvm_args;
                        llvm_args.push_back(embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
                        llvm_args.push_back(embedConstantPtr(&node->id, g.llvm_str_type_ptr));
                        llvm_args.push_back(getConstantInt(from_global, g.i1));

                        llvm::Value* uncasted = emitter.createPatchpoint(pp, (void*)pyston::getGlobal, llvm_args);
                        llvm::Value* r = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
                        return new ConcreteCompilerVariable(UNKNOWN, r, true);
                    } else {
                        llvm::Value *r = emitter.getBuilder()->CreateCall3(g.funcs.getGlobal, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr), embedConstantPtr(&node->id, g.llvm_str_type_ptr), getConstantInt(from_global, g.i1));
                        return new ConcreteCompilerVariable(UNKNOWN, r, true);
                    }
                } else {
                    // Method 2 [testing-only]: (ab)uses existing getattr patchpoints and just calls module.getattr()
                    // This option exists for performance testing because method 1 does not currently use patchpoints.
                    ConcreteCompilerVariable *mod = new ConcreteCompilerVariable(MODULE, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_value_type_ptr), false);
                    CompilerVariable *attr = mod->getattr(emitter, getOpInfoForNode(node), &node->id, false);
                    mod->decvref(emitter);
                    return attr;
                }
            } else {
                if (symbol_table.find(node->id) == symbol_table.end()) {
                    // TODO should mark as DEAD here, though we won't end up setting all the names appropriately
                    //state = DEAD;
                    llvm::CallInst *call = emitter.getBuilder()->CreateCall2(g.funcs.assertNameDefined, getConstantInt(0, g.i1), getStringConstantPtr(node->id + '\0'));
                    call->setDoesNotReturn();
                    return undefVariable();
                }

                std::string defined_name = _getFakeName("is_defined", node->id.c_str());
                ConcreteCompilerVariable *is_defined = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));
                if (is_defined) {
                    emitter.getBuilder()->CreateCall2(g.funcs.assertNameDefined, is_defined->getValue(), getStringConstantPtr(node->id + '\0'));
                }

                CompilerVariable *rtn = symbol_table[node->id];
                rtn->incvref();
                return rtn;
            }
        }

        CompilerVariable* evalNum(AST_Num *node) {
            assert(state != PARTIAL);

            if (node->num_type == AST_Num::INT)
                return makeInt(node->n_int);
            else if (node->num_type == AST_Num::FLOAT)
                return makeFloat(node->n_float);
            else
                RELEASE_ASSERT(0, "");
        }

        CompilerVariable* evalSlice(AST_Slice *node) {
            assert(state != PARTIAL);

            CompilerVariable *start, *stop, *step;
            start = node->lower ? evalExpr(node->lower) : getNone();
            stop = node->upper ? evalExpr(node->upper) : getNone();
            step = node->step ? evalExpr(node->step) : getNone();

            ConcreteCompilerVariable *cstart, *cstop, *cstep;
            cstart = start->makeConverted(emitter, start->getBoxType());
            cstop = stop->makeConverted(emitter, stop->getBoxType());
            cstep = step->makeConverted(emitter, step->getBoxType());
            start->decvref(emitter);
            stop->decvref(emitter);
            step->decvref(emitter);

            std::vector<llvm::Value*> args;
            args.push_back(cstart->getValue());
            args.push_back(cstop->getValue());
            args.push_back(cstep->getValue());
            llvm::Value* rtn = emitter.getBuilder()->CreateCall(g.funcs.createSlice, args);

            cstart->decvref(emitter);
            cstop->decvref(emitter);
            cstep->decvref(emitter);
            return new ConcreteCompilerVariable(SLICE, rtn, true);
        }

        CompilerVariable* evalStr(AST_Str *node) {
            assert(state != PARTIAL);

            return makeStr(&node->s);
        }

        CompilerVariable* evalSubscript(AST_Subscript *node) {
            assert(state != PARTIAL);

            CompilerVariable *value = evalExpr(node->value);
            CompilerVariable *slice = evalExpr(node->slice);

            CompilerVariable *rtn = value->getitem(emitter, getOpInfoForNode(node), slice);
            value->decvref(emitter);
            slice->decvref(emitter);
            return rtn;
        }

        CompilerVariable* evalTuple(AST_Tuple *node) {
            assert(state != PARTIAL);

            std::vector<CompilerVariable*> elts;
            for (int i = 0; i < node->elts.size(); i++) {
                CompilerVariable *value = evalExpr(node->elts[i]);
                elts.push_back(value);
            }

            // TODO makeTuple should probably just transfer the vref, but I want to keep things consistent
            CompilerVariable *rtn = makeTuple(elts);
            for (int i = 0; i < node->elts.size(); i++) {
                elts[i]->decvref(emitter);
            }
            return rtn;
        }

        CompilerVariable* evalUnaryOp(AST_UnaryOp *node) {
            assert(state != PARTIAL);

            CompilerVariable* operand = evalExpr(node->operand);

            if (node->op_type == AST_TYPE::Not) {
                ConcreteCompilerVariable *rtn = operand->nonzero(emitter, getOpInfoForNode(node));
                operand->decvref(emitter);

                llvm::Value* v = rtn->getValue();
                assert(v->getType() == g.i1);
                llvm::Value* negated = emitter.getBuilder()->CreateNot(v);
                rtn->decvref(emitter);
                return new ConcreteCompilerVariable(BOOL, negated, true);
            } else {
                // TODO These are pretty inefficient, but luckily I don't think they're used that often:
                ConcreteCompilerVariable *converted = operand->makeConverted(emitter, operand->getBoxType());
                operand->decvref(emitter);

                llvm::Value* rtn = emitter.getBuilder()->CreateCall2(g.funcs.unaryop, converted->getValue(), getConstantInt(node->op_type, g.i32));
                converted->decvref(emitter);

                return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
            }
        }

        ConcreteCompilerVariable* unboxVar(ConcreteCompilerType *t, llvm::Value *v, bool grabbed) {
            assert(state != PARTIAL);

            if (t == BOXED_INT) {
                llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxInt, v);
                ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(INT, unboxed, true);
                return rtn;
            }
            if (t == BOXED_FLOAT) {
                llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxFloat, v);
                ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(FLOAT, unboxed, true);
                return rtn;
            }
            return new ConcreteCompilerVariable(t, v, grabbed);
        }

        CompilerVariable* evalExpr(AST_expr *node) {
            emitter.getBuilder()->SetCurrentDebugLocation(llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));

            CompilerVariable *rtn = NULL;
            if (state != PARTIAL) {
                switch (node->type) {
                    case AST_TYPE::Attribute:
                        rtn = evalAttribute(static_cast<AST_Attribute*>(node));
                        break;
                    case AST_TYPE::AugBinOp:
                        rtn = evalAugBinOp(static_cast<AST_AugBinOp*>(node));
                        break;
                    case AST_TYPE::BinOp:
                        rtn = evalBinOp(static_cast<AST_BinOp*>(node));
                        break;
                    case AST_TYPE::Call:
                        rtn = evalCall(static_cast<AST_Call*>(node));
                        break;
                    case AST_TYPE::Compare:
                        rtn = evalCompare(static_cast<AST_Compare*>(node));
                        break;
                    case AST_TYPE::Dict:
                        rtn = evalDict(static_cast<AST_Dict*>(node));
                        break;
                    case AST_TYPE::Index:
                        rtn = evalIndex(static_cast<AST_Index*>(node));
                        break;
                    case AST_TYPE::List:
                        rtn = evalList(static_cast<AST_List*>(node));
                        break;
                    case AST_TYPE::Name:
                        rtn = evalName(static_cast<AST_Name*>(node));
                        break;
                    case AST_TYPE::Num:
                        rtn = evalNum(static_cast<AST_Num*>(node));
                        break;
                    case AST_TYPE::Slice:
                        rtn = evalSlice(static_cast<AST_Slice*>(node));
                        break;
                    case AST_TYPE::Str:
                        rtn = evalStr(static_cast<AST_Str*>(node));
                        break;
                    case AST_TYPE::Subscript:
                        rtn = evalSubscript(static_cast<AST_Subscript*>(node));
                        break;
                    case AST_TYPE::Tuple:
                        rtn = evalTuple(static_cast<AST_Tuple*>(node));
                        break;
                    case AST_TYPE::UnaryOp:
                        rtn = evalUnaryOp(static_cast<AST_UnaryOp*>(node));
                        break;
                    case AST_TYPE::ClsAttribute:
                        rtn = evalClsAttribute(static_cast<AST_ClsAttribute*>(node));
                        break;
                    default:
                        printf("Unhandled expr type: %d (irgen.cpp:" STRINGIFY(__LINE__) ")\n", node->type);
                        exit(1);
                }

                assert(rtn);

                // Out-guarding:
                BoxedClass *speculated_class = types->speculatedExprClass(node);
                if (speculated_class != NULL) {
                    assert(rtn);

                    ConcreteCompilerType *speculated_type = typeFromClass(speculated_class);
                    if (VERBOSITY("irgen") >= 1) {
                        printf("Speculating that %s is actually %s, at ", rtn->getConcreteType()->debugName().c_str(), speculated_type->debugName().c_str());
                        PrintVisitor printer;
                        node->accept(&printer);
                        printf("\n");
                    }

                    // That's not really a speculation.... could potentially handle this here, but
                    // I think it's better to just not generate bad speculations:
                    assert(!rtn->canConvertTo(speculated_type));

                    ConcreteCompilerVariable *old_rtn = rtn->makeConverted(emitter, UNKNOWN);
                    rtn->decvref(emitter);

                    llvm::Value* guard_check = old_rtn->makeClassCheck(emitter, speculated_class);
                    assert(guard_check->getType() == g.i1);
                    createExprTypeGuard(guard_check, node, old_rtn);

                    rtn = unboxVar(speculated_type, old_rtn->getValue(), true);
                }
            }

            // In-guarding:
            GuardList::ExprTypeGuard* guard = in_guards.getNodeTypeGuard(node);
            if (guard != NULL) {
                if (VERBOSITY("irgen") >= 1) {
                    printf("merging guard after ");
                    PrintVisitor printer;
                    node->accept(&printer);
                    printf("; is_partial=%d\n", state == PARTIAL);
                }

                if (state == PARTIAL) {
                    guard->branch->setSuccessor(1, curblock);
                    symbol_table = SymbolTable(guard->st);
                    assert(guard->val);
                    state = RUNNING;

                    return guard->val;
                } else {
                    assert(state == RUNNING);
                    compareKeyset(&symbol_table, &guard->st);

                    assert(symbol_table.size() == guard->st.size());
                    llvm::BasicBlock *ramp_block = llvm::BasicBlock::Create(g.context, "deopt_ramp", irstate->getLLVMFunction());
                    llvm::BasicBlock *join_block = llvm::BasicBlock::Create(g.context, "deopt_join", irstate->getLLVMFunction());
                    SymbolTable joined_st;
                    for (auto p : guard->st) {
                        //if (VERBOSITY("irgen") >= 1) printf("merging %s\n", p.first.c_str());
                        CompilerVariable *curval = symbol_table[p.first];
                        // I'm not sure this is necessary or even correct:
                        //ASSERT(curval->getVrefs() == p.second->getVrefs(), "%s %d %d", p.first.c_str(), curval->getVrefs(), p.second->getVrefs());

                        ConcreteCompilerType *merged_type = curval->getConcreteType();

                        emitter.getBuilder()->SetInsertPoint(ramp_block);
                        ConcreteCompilerVariable* converted1 = p.second->makeConverted(emitter, merged_type);
                        p.second->decvref(emitter); // for makeconverted
                        //guard->st[p.first] = converted;
                        //p.second->decvref(emitter); // for the replaced version

                        emitter.getBuilder()->SetInsertPoint(curblock);
                        ConcreteCompilerVariable* converted2 = curval->makeConverted(emitter, merged_type);
                        curval->decvref(emitter); // for makeconverted
                        //symbol_table[p.first] = converted;
                        //curval->decvref(emitter); // for the replaced version

                        if (converted1->getValue() == converted2->getValue()) {
                            joined_st[p.first] = new ConcreteCompilerVariable(merged_type, converted1->getValue(), true);
                        } else {
                            emitter.getBuilder()->SetInsertPoint(join_block);
                            llvm::PHINode* phi = emitter.getBuilder()->CreatePHI(merged_type->llvmType(), 2, p.first);
                            phi->addIncoming(converted1->getValue(), ramp_block);
                            phi->addIncoming(converted2->getValue(), curblock);
                            joined_st[p.first] = new ConcreteCompilerVariable(merged_type, phi, true);
                        }

                        // TODO free dead Variable objects!
                    }
                    symbol_table = joined_st;

                    emitter.getBuilder()->SetInsertPoint(curblock);
                    emitter.getBuilder()->CreateBr(join_block);

                    emitter.getBuilder()->SetInsertPoint(ramp_block);
                    emitter.getBuilder()->CreateBr(join_block);

                    guard->branch->setSuccessor(1, ramp_block);

                    {
                        ConcreteCompilerType *this_merged_type = rtn->getConcreteType();

                        emitter.getBuilder()->SetInsertPoint(ramp_block);
                        ConcreteCompilerVariable *converted_guard_rtn = guard->val->makeConverted(emitter, this_merged_type);
                        guard->val->decvref(emitter);

                        emitter.getBuilder()->SetInsertPoint(curblock);
                        ConcreteCompilerVariable *converted_rtn = rtn->makeConverted(emitter, this_merged_type);
                        rtn->decvref(emitter);

                        emitter.getBuilder()->SetInsertPoint(join_block);
                        llvm::PHINode* this_phi = emitter.getBuilder()->CreatePHI(this_merged_type->llvmType(), 2);
                        this_phi->addIncoming(converted_rtn->getValue(), curblock);
                        this_phi->addIncoming(converted_guard_rtn->getValue(), ramp_block);
                        rtn = new ConcreteCompilerVariable(this_merged_type, this_phi, true);

                        // TODO free dead Variable objects!
                    }

                    curblock = join_block;
                    emitter.getBuilder()->SetInsertPoint(curblock);
                }
            }

            assert(rtn || state == PARTIAL);

            return rtn;
        }

        static std::string _getFakeName(const char* prefix, const char* token) {
            char buf[40];
            snprintf(buf, 40, "!%s_%s", prefix, token);
            return std::string(buf);
        }
        void _setFake(std::string name, CompilerVariable* val) {
            assert(name[0] == '!');
            CompilerVariable* &cur = symbol_table[name];
            assert(cur == NULL);
            cur = val;
        }
        CompilerVariable* _clearFake(std::string name) {
            assert(name[0] == '!');
            CompilerVariable* rtn = symbol_table[name];
            assert(rtn == NULL);
            symbol_table.erase(name);
            return rtn;
        }
        CompilerVariable* _getFake(std::string name, bool allow_missing=false) {
            assert(name[0] == '!');
            CompilerVariable* rtn = symbol_table[name];
            if (!allow_missing)
                assert(rtn != NULL);
            symbol_table.erase(name);
            return rtn;
        }

        void _doSet(const std::string &name, CompilerVariable* val) {
            assert(name != "None");
            if (irstate->getScopeInfo()->refersToGlobal(name)) {
                // TODO do something special here so that it knows to only emit a monomorphic inline cache?
                ConcreteCompilerVariable* module = new ConcreteCompilerVariable(MODULE, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_value_type_ptr), false);
                module->setattr(emitter, getEmptyOpInfo(), &name, val);
                module->decvref(emitter);
            } else {
                CompilerVariable* &prev = symbol_table[name];
                if (prev != NULL) {
                    prev->decvref(emitter);
                }
                prev = val;
                val->incvref();

                // Clear out the is_defined name since it is now definitely defined:
                assert(!startswith(name, "!is_defined"));
                std::string defined_name = _getFakeName("is_defined", name.c_str());
                _getFake(defined_name, true);
            }
        }

        void _doSetattr(AST_Attribute* target, CompilerVariable* val) {
            assert(state != PARTIAL);
            CompilerVariable *t = evalExpr(target->value);
            t->setattr(emitter, getEmptyOpInfo(), &target->attr, val);
            t->decvref(emitter);
        }

        void _doSetitem(AST_Subscript* target, CompilerVariable* val) {
            assert(state != PARTIAL);
            CompilerVariable *tget = evalExpr(target->value);
            CompilerVariable *slice = evalExpr(target->slice);

            ConcreteCompilerVariable *converted_target = tget->makeConverted(emitter, tget->getBoxType());
            ConcreteCompilerVariable *converted_slice = slice->makeConverted(emitter, slice->getBoxType());
            tget->decvref(emitter);
            slice->decvref(emitter);

            ConcreteCompilerVariable *converted_val = val->makeConverted(emitter, val->getBoxType());

            bool do_patchpoint = ENABLE_ICSETITEMS && (irstate ->getEffortLevel() != EffortLevel::INTERPRETED);
            if (do_patchpoint) {
                PatchpointSetupInfo *pp = patchpoints::createSetitemPatchpoint(emitter.currentFunction(), getEmptyOpInfo().getTypeRecorder());

                std::vector<llvm::Value*> llvm_args;
                llvm_args.push_back(converted_target->getValue());
                llvm_args.push_back(converted_slice->getValue());
                llvm_args.push_back(converted_val->getValue());

                emitter.createPatchpoint(pp, (void*)pyston::setitem, llvm_args);
            } else {
                emitter.getBuilder()->CreateCall3(g.funcs.setitem,
                        converted_target->getValue(), converted_slice->getValue(), converted_val->getValue());
            }

            converted_target->decvref(emitter);
            converted_slice->decvref(emitter);
            converted_val->decvref(emitter);
        }

        void _doUnpackTuple(AST_Tuple* target, CompilerVariable* val) {
            assert(state != PARTIAL);
            int ntargets = target->elts.size();
            // TODO do type recording here?
            ConcreteCompilerVariable *len = val->len(emitter, getEmptyOpInfo());
            emitter.getBuilder()->CreateCall2(g.funcs.checkUnpackingLength,
                    getConstantInt(ntargets, g.i64), len->getValue());

            for (int i = 0; i < ntargets; i++) {
                CompilerVariable *unpacked = val->getitem(emitter, getEmptyOpInfo(), makeInt(i));
                _doSet(target->elts[i], unpacked);
                unpacked->decvref(emitter);
            }
        }

        void _doSet(AST* target, CompilerVariable* val) {
            assert(state != PARTIAL);
            switch (target->type) {
                case AST_TYPE::Attribute:
                    _doSetattr(static_cast<AST_Attribute*>(target), val);
                    break;
                case AST_TYPE::Name:
                    _doSet(static_cast<AST_Name*>(target)->id, val);
                    break;
                case AST_TYPE::Subscript:
                    _doSetitem(static_cast<AST_Subscript*>(target), val);
                    break;
                case AST_TYPE::Tuple:
                    _doUnpackTuple(static_cast<AST_Tuple*>(target), val);
                    break;
                default:
                    ASSERT(0, "Unknown type for IRGenerator: %d", target->type);
                    abort();
            }
        }

        void doAssign(AST_Assign *node) {
            CompilerVariable *val = evalExpr(node->value);
            if (state == PARTIAL)
                return;

            for (int i = 0; i < node->targets.size(); i++) {
                _doSet(node->targets[i], val);
            }
            val->decvref(emitter);
        }

        void doClassDef(AST_ClassDef *node) {
            if (state == PARTIAL)
                return;

            ScopeInfo *scope_info = irstate->getSourceInfo()->scoping->getScopeInfoForNode(node);

            llvm::Value *classobj = emitter.getBuilder()->CreateCall2(g.funcs.createClass, embedConstantPtr(&node->name, g.llvm_str_type_ptr), embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
            ConcreteCompilerVariable* cls = new ConcreteCompilerVariable(typeFromClass(type_cls), classobj, true);

            RELEASE_ASSERT(node->bases.size() == 1, "");
            RELEASE_ASSERT(node->bases[0]->type == AST_TYPE::Name, "");
            RELEASE_ASSERT(static_cast<AST_Name*>(node->bases[0])->id == "object", "");

            //CompilerVariable* name = makeStr(&node->name);
            //cls->setattr(emitter, "__name__", name);
            //name->decvref(emitter);

            for (int i = 0, n = node->body.size(); i < n; i++) {
                AST_TYPE::AST_TYPE type = node->body[i]->type;
                if (type == AST_TYPE::Pass) {
                    continue;
                } else if (type == AST_TYPE::FunctionDef) {
                    AST_FunctionDef *fdef = static_cast<AST_FunctionDef*>(node->body[i]);
                    CLFunction *cl = this->_wrapFunction(fdef);
                    CompilerVariable *func = makeFunction(emitter, cl);
                    cls->setattr(emitter, getEmptyOpInfo(), &fdef->name, func);
                    func->decvref(emitter);
                } else {
                    RELEASE_ASSERT(node->body[i]->type == AST_TYPE::Pass, "%d", type);
                }
            }

            _doSet(node->name, cls);
            cls->decvref(emitter);
        }

        CLFunction* _wrapFunction(AST_FunctionDef *node) {
            // Different compilations of the parent scope of a functiondef should lead
            // to the same CLFunction* being used:
            static std::unordered_map<AST_FunctionDef*, CLFunction*> made;

            CLFunction* &cl = made[node];
            if (cl == NULL) {
                SourceInfo *si = new SourceInfo(irstate->getSourceInfo()->parent_module, irstate->getSourceInfo()->scoping);
                si->ast = node;
                cl = new CLFunction(si);
            }
            return cl;
        }

        void doFunction(AST_FunctionDef *node) {
            if (state == PARTIAL)
                return;

            CLFunction *cl = this->_wrapFunction(node);
            CompilerVariable *func = makeFunction(emitter, cl);

            //llvm::Type* boxCLFuncArgType = g.funcs.boxCLFunction->arg_begin()->getType();
            //llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxCLFunction, embedConstantPtr(cl, boxCLFuncArgType));
            //CompilerVariable *func = new ConcreteCompilerVariable(typeFromClass(function_cls), boxed, true);

            _doSet(node->name, func);
            func->decvref(emitter);
        }

        void doImport(AST_Import *node) {
            if (state == PARTIAL)
                return;

            for (int i = 0; i < node->names.size(); i++) {
                AST_alias *alias = node->names[i];

                std::string &modname = alias->name;
                std::string &asname = alias->asname.size() ? alias->asname : alias->name;

                llvm::Value* imported = emitter.getBuilder()->CreateCall(g.funcs.import, embedConstantPtr(&modname, g.llvm_str_type_ptr));
                ConcreteCompilerVariable *v = new ConcreteCompilerVariable(UNKNOWN, imported, true);
                _doSet(asname, v);
                v->decvref(emitter);
            }
        }

        void doPrint(AST_Print *node) {
            if (state == PARTIAL)
                return;

            assert(node->dest == NULL);
            for (int i = 0; i < node->values.size(); i++) {
                if (i > 0) {
                    emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr(" "));
                }
                CompilerVariable* var = evalExpr(node->values[i]);
                var->print(emitter);
                var->decvref(emitter);
            }

            if (node->nl)
                emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr("\n"));
            else
                emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr(" "));
        }

        void doReturn(AST_Return *node) {

            CompilerVariable *val;
            if (node->value == NULL) {
                if (irstate->getReturnType() == VOID) {
                    endBlock(DEAD);
                    emitter.getBuilder()->CreateRetVoid();
                    return;
                }

                val = new ConcreteCompilerVariable(NONE, embedConstantPtr(None, g.llvm_value_type_ptr), false);
            } else {
                val = evalExpr(node->value);
            }
            assert(state != PARTIAL);
            assert(val);

            // If we ask the return variable to become UNKNOWN (the typical return type),
            // it will be forced to split a copy of itself and incref.
            // But often the return variable will already be in the right shape, so in
            // that case asking it to convert to itself ends up just being an incvref
            // and doesn't end up emitting an incref+decref pair.
            // This could also be handled by casting from the CompilerVariable to
            // ConcreteCOmpilerVariable, but this way feels a little more robust to me.
            ConcreteCompilerType *opt_rtn_type = irstate->getReturnType();
            if (irstate->getReturnType()->llvmType() == val->getConcreteType()->llvmType())
                opt_rtn_type = val->getConcreteType();

            ConcreteCompilerVariable* rtn = val->makeConverted(emitter, opt_rtn_type);
            rtn->ensureGrabbed(emitter);
            val->decvref(emitter);

            endBlock(DEAD);

            ASSERT(rtn->getVrefs() == 1, "%d", rtn->getVrefs());
            emitter.getBuilder()->CreateRet(rtn->getValue());
        }

        void doBranch(AST_Branch *node) {
            assert(node->iftrue->idx > myblock->idx);
            assert(node->iffalse->idx > myblock->idx);

            CompilerVariable *val = evalExpr(node->test);
            assert(state != PARTIAL);
            assert(val);

            ConcreteCompilerVariable* nonzero = val->nonzero(emitter, getOpInfoForNode(node));
            assert(nonzero->getType() == BOOL);
            val->decvref(emitter);

            llvm::Value *llvm_nonzero = nonzero->getValue();
            llvm::BasicBlock *iftrue = entry_blocks[node->iftrue->idx];
            llvm::BasicBlock *iffalse = entry_blocks[node->iffalse->idx];

            nonzero->decvref(emitter);

            endBlock(FINISHED);

            emitter.getBuilder()->CreateCondBr(llvm_nonzero, iftrue, iffalse);
        }

        void doExpr(AST_Expr *node) {
            CompilerVariable *var = evalExpr(node->value);
            if (state == PARTIAL)
                return;

            var->decvref(emitter);
        }

        void doOSRExit(llvm::BasicBlock *normal_target, AST_Jump* osr_key) {
            assert(state != PARTIAL);

            llvm::BasicBlock *starting_block = curblock;
            llvm::BasicBlock *onramp = llvm::BasicBlock::Create(g.context, "onramp", irstate->getLLVMFunction());

            // Code to check if we want to do the OSR:
            llvm::GlobalVariable* edgecount_ptr = new llvm::GlobalVariable(*g.cur_module, g.i64, false, llvm::GlobalValue::InternalLinkage, getConstantInt(0, g.i64), "edgecount");
            llvm::Value* curcount = emitter.getBuilder()->CreateLoad(edgecount_ptr);
            llvm::Value* newcount = emitter.getBuilder()->CreateAdd(curcount, getConstantInt(1, g.i64));
            emitter.getBuilder()->CreateStore(newcount, edgecount_ptr);

            int OSR_THRESHOLD = 10000;
            if (irstate->getEffortLevel() == EffortLevel::INTERPRETED)
                OSR_THRESHOLD = 100;
            llvm::Value* osr_test = emitter.getBuilder()->CreateICmpSGT(newcount, getConstantInt(OSR_THRESHOLD));

            llvm::Value* md_vals[] = {llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1), getConstantInt(1000)};
            llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));
            emitter.getBuilder()->CreateCondBr(osr_test, onramp, normal_target, branch_weights);

            // Emitting the actual OSR:
            emitter.getBuilder()->SetInsertPoint(onramp);
            OSRExit* exit = new OSRExit(irstate->getCurFunction(), OSREntryDescriptor::create(irstate->getCurFunction(), osr_key));
            llvm::Value* partial_func = emitter.getBuilder()->CreateCall(g.funcs.compilePartialFunc, embedConstantPtr(exit, g.i8->getPointerTo()));

            std::vector<llvm::Value*> llvm_args;
            std::vector<llvm::Type*> llvm_arg_types;
            std::vector<ConcreteCompilerVariable*> converted_args;

            SortedSymbolTable sorted_symbol_table(symbol_table.begin(), symbol_table.end());
            /*
            for (SortedSymbolTable::iterator it = sorted_symbol_table.begin(), end = sorted_symbol_table.end(); it != end; ) {
                if (!source->liveness->isLiveAtEnd(it->first, myblock)) {
                    // I think this line can never get hit: nothing can die on a backedge, since control flow can eventually
                    // reach this block again, where the symbol was live (as shown by it being in the symbol table)
                    printf("Not sending %s to osr since it will die\n", it->first.c_str());
                    it = sorted_symbol_table.erase(it);
                } else {
                    ++it;
                }
            }*/

            // For OSR calls, we use the same calling convention as in some other places; namely,
            // arg1, arg2, arg3, argarray [nargs is ommitted]
            // It would be nice to directly pass all variables as arguments, instead of packing them into
            // an array, for a couple reasons (eliminate copies, and allow for a tail call).
            // But this doesn't work if the IR is being interpreted, because the interpreter can't
            // do arbitrary-arity function calls (yet?).  One possibility is to pass them as an
            // array for the interpreter and as all arguments for compilation, but I'd rather avoid
            // having two different calling conventions for the same thing.  Plus, this would
            // prevent us from having two OSR exits point to the same OSR entry; not something that
            // we're doing right now but something that would be nice in the future.

            llvm::Value *arg_array = NULL, *malloc_save = NULL;
            if (sorted_symbol_table.size() > 3) {
                // Leave in the ability to use malloc but I guess don't use it.
                // Maybe if there are a ton of live variables it'd be nice to have them be
                // heap-allocated, or if we don't immediately return the result of the OSR?
                bool use_malloc = false;
                if (false) {
                    llvm::Value *n_bytes = getConstantInt((sorted_symbol_table.size() - 3) * sizeof(Box*), g.i64);
                    llvm::Value *l_malloc = embedConstantPtr((void*)malloc, llvm::FunctionType::get(g.i8->getPointerTo(), g.i64, false)->getPointerTo());
                    malloc_save = emitter.getBuilder()->CreateCall(l_malloc, n_bytes);
                    arg_array = emitter.getBuilder()->CreateBitCast(malloc_save, g.llvm_value_type_ptr->getPointerTo());
                } else {
                    llvm::Value *n_varargs = llvm::ConstantInt::get(g.i64, sorted_symbol_table.size() - 3, false);
                    arg_array = emitter.getBuilder()->CreateAlloca(g.llvm_value_type_ptr, n_varargs);
                }
            }

            int arg_num = -1;
            for (auto p : sorted_symbol_table) {
                arg_num++;
                // I don't think this can fail, but if it can we should filter out dead symbols before
                // passing them on:
                ASSERT(startswith(p.first, "!is_defined") ||  irstate->getSourceInfo()->liveness->isLiveAtEnd(p.first, myblock), "%d %s", myblock->idx, p.first.c_str());

                // This line can never get hit right now since we unnecessarily force every variable to be concrete
                // for a loop, since we generate all potential phis:
                ASSERT(p.second->getType() == p.second->getConcreteType(), "trying to pass through %s\n", p.second->getType()->debugName().c_str());

                ConcreteCompilerVariable* var = p.second->makeConverted(emitter, p.second->getConcreteType());
                converted_args.push_back(var);

                assert(var->getType() != BOXED_INT && "should probably unbox it, but why is it boxed in the first place?");
                assert(var->getType() != BOXED_FLOAT && "should probably unbox it, but why is it boxed in the first place?");

                // This line can never get hit right now for the same reason that the variables must already be concrete,
                // because we're over-generating phis.
                ASSERT(var->isGrabbed(), "%s", p.first.c_str());
                //var->ensureGrabbed(emitter);

                llvm::Value* val = var->getValue();

                if (arg_num < 3) {
                    llvm_args.push_back(val);
                    llvm_arg_types.push_back(val->getType());
                } else {
                    llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, arg_num-3);

                    if (var->getType() == INT) {
                        val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                    } else if (var->getType() == FLOAT) {
                        //val = emitter.getBuilder()->CreateBitCast(val, g.llvm_value_type_ptr);
                        ptr = emitter.getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                    } else {
                        assert(val->getType() == g.llvm_value_type_ptr);
                    }

                    emitter.getBuilder()->CreateStore(val, ptr);
                }

                ConcreteCompilerType* &t = exit->entry->args[p.first];
                if (t == NULL)
                    t = var->getType();
                else
                    ASSERT(t == var->getType(), "%s %s\n", t->debugName().c_str(), var->getType()->debugName().c_str());
            }

            if (sorted_symbol_table.size() > 3) {
                llvm_args.push_back(arg_array);
                llvm_arg_types.push_back(arg_array->getType());
            }

            llvm::FunctionType* ft = llvm::FunctionType::get(irstate->getReturnType()->llvmType(), llvm_arg_types, false /*vararg*/);
            partial_func = emitter.getBuilder()->CreateBitCast(partial_func, ft->getPointerTo());

            llvm::CallInst *rtn = emitter.getBuilder()->CreateCall(partial_func, llvm_args);

            // If we alloca'd the arg array, we can't make this into a tail call:
            if (arg_array == NULL && malloc_save != NULL) {
                rtn->setTailCall(true);
            }

            if (malloc_save != NULL) {
                llvm::Value *l_free = embedConstantPtr((void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
                emitter.getBuilder()->CreateCall(l_free, malloc_save);
            }

            for (int i = 0; i < converted_args.size(); i++) {
                converted_args[i]->decvref(emitter);
            }

            if (irstate->getReturnType() == VOID)
                emitter.getBuilder()->CreateRetVoid();
            else
                emitter.getBuilder()->CreateRet(rtn);

            emitter.getBuilder()->SetInsertPoint(starting_block);
        }

        void doJump(AST_Jump *node) {
            assert(state != PARTIAL);

            endBlock(FINISHED);

            llvm::BasicBlock *target = entry_blocks[node->target->idx];

            if (ENABLE_OSR && node->target->idx < myblock->idx && irstate->getEffortLevel() < EffortLevel::MAXIMAL) {
                assert(node->target->predecessors.size() > 1);
                doOSRExit(target, node);
            } else {
                emitter.getBuilder()->CreateBr(target);
            }
        }

        void doStmt(AST *node) {
            switch (node->type) {
                case AST_TYPE::Assign:
                    doAssign(static_cast<AST_Assign*>(node));
                    break;
                case AST_TYPE::ClassDef:
                    doClassDef(static_cast<AST_ClassDef*>(node));
                    break;
                case AST_TYPE::Expr:
                    doExpr(static_cast<AST_Expr*>(node));
                    break;
                case AST_TYPE::FunctionDef:
                    doFunction(static_cast<AST_FunctionDef*>(node));
                    break;
                //case AST_TYPE::If:
                    //doIf(static_cast<AST_If*>(node));
                    //break;
                case AST_TYPE::Import:
                    doImport(static_cast<AST_Import*>(node));
                    break;
                case AST_TYPE::Global:
                    // Should have been handled already
                    break;
                case AST_TYPE::Pass:
                    break;
                case AST_TYPE::Print:
                    doPrint(static_cast<AST_Print*>(node));
                    break;
                case AST_TYPE::Return:
                    doReturn(static_cast<AST_Return*>(node));
                    break;
                case AST_TYPE::Branch:
                    doBranch(static_cast<AST_Branch*>(node));
                    break;
                case AST_TYPE::Jump:
                    doJump(static_cast<AST_Jump*>(node));
                    break;
                default:
                    printf("Unhandled stmt type at " __FILE__ ":" STRINGIFY(__LINE__) ": %d\n", node->type);
                    exit(1);
            }
        }

        template <typename T>
        void loadArgument(const T &name, ConcreteCompilerType* t, llvm::Value* v) {
            ConcreteCompilerVariable *var = unboxVar(t, v, false);
            _doSet(name, var);
            var->decvref(emitter);
        }

        void endBlock(State new_state) {
            assert(state == RUNNING);

            //cf->func->dump();

            SourceInfo* source = irstate->getSourceInfo();
            ScopeInfo *scope_info = irstate->getScopeInfo();

            for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end();) {
                if (startswith(it->first, "!is_defined")) {
                    ++it;
                    continue;
                }

                //ASSERT(it->first[0] != '!' || startswith(it->first, "!is_defined"), "left a fake variable in the real symbol table? '%s'", it->first.c_str());

                if (!source->liveness->isLiveAtEnd(it->first, myblock)) {
                    //printf("%s dead at end of %d; grabbed = %d, %d vrefs\n", it->first.c_str(), myblock->idx, it->second->isGrabbed(), it->second->getVrefs());
                    it->second->decvref(emitter);
                    it = symbol_table.erase(it);
                } else if (source->phis->isRequiredAfter(it->first, myblock)) {
                    assert(!scope_info->refersToGlobal(it->first));
                    ConcreteCompilerType *phi_type = types->getTypeAtBlockEnd(it->first, myblock);
                    //printf("Converting %s from %s to %s\n", it->first.c_str(), it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                    //printf("have to convert %s from %s to %s\n", it->first.c_str(), it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                    ConcreteCompilerVariable *v = it->second->makeConverted(emitter, phi_type);
                    it->second->decvref(emitter);
                    symbol_table[it->first] = v->split(emitter);
                    ++it;
                } else {
#ifndef NDEBUG
                    // TODO getTypeAtBlockEnd will automatically convert up to the concrete type, which we don't want here,
                    // but this is just for debugging so I guess let it happen for now:
                    ConcreteCompilerType *ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                    ASSERT(it->second->canConvertTo(ending_type), "%s is supposed to be %s, but somehow is %s", it->first.c_str(), ending_type->debugName().c_str(), it->second->getType()->debugName().c_str());
#endif

                    ++it;
                }
            }

            const PhiAnalysis::RequiredSet &all_phis = source->phis->getAllRequiredAfter(myblock);
            for (PhiAnalysis::RequiredSet::const_iterator it = all_phis.begin(), end = all_phis.end(); it != end; ++it) {
                //printf("phi will be required for %s\n", it->c_str());
                assert(!scope_info->refersToGlobal(*it));
                CompilerVariable* &cur = symbol_table[*it];

                std::string defined_name = _getFakeName("is_defined", it->c_str());

                if (cur != NULL) {
                    //printf("defined on this path; ");

                    ConcreteCompilerVariable *is_defined = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));

                    if (source->phis->isPotentiallyUndefinedAfter(*it, myblock)) {
                        //printf("is potentially undefined later, so marking it defined\n");
                        if (is_defined) {
                            _setFake(defined_name, is_defined);
                        } else {
                            _setFake(defined_name, new ConcreteCompilerVariable(BOOL, getConstantInt(1, g.i1), true));
                        }
                    } else {
                        //printf("is defined in all later paths, so not marking\n");
                        assert(!is_defined);
                    }
                } else {
                    //printf("no st entry, setting undefined\n");
                    ConcreteCompilerType *phi_type = types->getTypeAtBlockEnd(*it, myblock);
                    cur = new ConcreteCompilerVariable(phi_type, llvm::UndefValue::get(phi_type->llvmType()), true);
                    _setFake(defined_name, new ConcreteCompilerVariable(BOOL, getConstantInt(0, g.i1), true));
                }
            }

            state = new_state;
        }

    public:
        EndingState getEndingSymbolTable() override {
            assert(state == FINISHED || state == DEAD);

            //for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end(); ++it) {
                //printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
            //}

            SourceInfo* source = irstate->getSourceInfo();

            SymbolTable *st = new SymbolTable(symbol_table);
            ConcreteSymbolTable *phi_st = new ConcreteSymbolTable();
            for (SymbolTable::iterator it = st->begin(); it != st->end(); it++) {
                if (it->first[0] == '!') {
                    //ASSERT(startswith(it->first, _getFakeName("is_defined", "")), "left a fake variable in the real symbol table? '%s'", it->first.c_str());
                } else {
                    ASSERT(source->liveness->isLiveAtEnd(it->first, myblock), "%s", it->first.c_str());
                }
            }

            if (myblock->successors.size() == 0) {
                assert(st->size() == 0); // shouldn't have anything live if there are no successors!
                return EndingState(st, phi_st, curblock);
            } else if (myblock->successors.size() > 1) {
                // Since there are no critical edges, all successors come directly from this node,
                // so there won't be any required phis.
                return EndingState(st, phi_st, curblock);
            }

            assert(myblock->successors.size() == 1); // other cases should have been handled

            // In theory this case shouldn't be necessary:
            if (myblock->successors[0]->predecessors.size() == 1) {
                // If the next block has a single predecessor, don't have to
                // emit any phis.
                // Should probably not emit no-op jumps like this though.
                return EndingState(st, phi_st, curblock);
            }

            for (SymbolTable::iterator it = st->begin(); it != st->end();) {
                if (startswith(it->first, "!is_defined") || source->phis->isRequiredAfter(it->first, myblock)) {
                    assert(it->second->isGrabbed());
                    assert(it->second->getVrefs() == 1);
                    // this conversion should have already happened... should refactor this.
                    ConcreteCompilerType *ending_type;
                    if (startswith(it->first, "!is_defined")) {
                        assert(it->second->getType() == BOOL);
                        ending_type = BOOL;
                    } else {
                        ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                    }
                    //(*phi_st)[it->first] = it->second->makeConverted(emitter, it->second->getConcreteType());
                    //printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
                    (*phi_st)[it->first] = it->second->split(emitter)->makeConverted(emitter, ending_type);
                    it = st->erase(it);
                } else {
                    ++it;
                }
            }
            return EndingState(st, phi_st, curblock);
        }

        void giveLocalSymbol(const std::string &name, CompilerVariable *var) override {
            assert(name != "None");
            ASSERT(!irstate->getScopeInfo()->refersToGlobal(name), "%s", name.c_str());
            assert(var->getType() != BOXED_INT);
            assert(var->getType() != BOXED_FLOAT);
            CompilerVariable* &cur = symbol_table[name];
            assert(cur == NULL);
            cur = var;
        }

        void copySymbolsFrom(SymbolTable* st) override {
            assert(st);
            DupCache cache;
            for (SymbolTable::iterator it = st->begin(); it != st->end(); it++) {
                //printf("Copying in %s: %p, %d\n", it->first.c_str(), it->second, it->second->getVrefs());
                symbol_table[it->first] = it->second->dup(cache);
                //printf("got: %p, %d\n", symbol_table[it->first], symbol_table[it->first]->getVrefs());
            }
        }

        void unpackArguments(const std::vector<AST_expr*> &arg_names, const std::vector<ConcreteCompilerType*> &arg_types) override {
            int i = 0;
            llvm::Value* argarray = NULL;
            for (llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin(); AI != irstate->getLLVMFunction()->arg_end(); AI++, i++) {
                if (i == 3) {
                    argarray = AI;
                    break;
                }
                loadArgument(arg_names[i], arg_types[i], AI);
            }

            for (int i = 3; i < arg_types.size(); i++) {
                llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(argarray, i-3);
                llvm::Value* loaded = emitter.getBuilder()->CreateLoad(ptr);

                if (arg_types[i]->llvmType() == g.i64)
                    loaded = emitter.getBuilder()->CreatePtrToInt(loaded, arg_types[i]->llvmType());
                else
                    assert(arg_types[i]->llvmType() == g.llvm_value_type_ptr);

                loadArgument(arg_names[i], arg_types[i], loaded);
            }
        }

        void run(const CFGBlock* block) override {
            for (int i = 0; i < block->body.size(); i++) {
                if (state == DEAD)
                    break;
                assert(state != FINISHED);
                doStmt(block->body[i]);
            }
        }

};

IRGenerator *createIRGenerator(IRGenState *irstate, std::vector<llvm::BasicBlock*> &entry_blocks, CFGBlock *myblock, TypeAnalysis *types, GuardList &out_guards, const GuardList &in_guards, bool is_partial) {
    return new IRGeneratorImpl(irstate, entry_blocks, myblock, types, out_guards, in_guards, is_partial);
}

}
