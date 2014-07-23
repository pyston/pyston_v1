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

#include "codegen/irgen/irgenerator.h"

#include "llvm/IR/Module.h"

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "analysis/type_analysis.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "codegen/patchpoints.h"
#include "codegen/type_recording.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

llvm::Value* IRGenState::getScratchSpace(int min_bytes) {
    llvm::BasicBlock& entry_block = getLLVMFunction()->getEntryBlock();

    if (scratch_space) {
        assert(scratch_space->getParent() == &entry_block);
        assert(scratch_space->isStaticAlloca());
        if (scratch_size >= min_bytes)
            return scratch_space;
    }

    llvm::AllocaInst* new_scratch_space;
    // If the entry block is currently empty, we have to be more careful:
    if (entry_block.begin() == entry_block.end()) {
        new_scratch_space = new llvm::AllocaInst(g.i8, getConstantInt(min_bytes, g.i64), "scratch", &entry_block);
    } else {
        new_scratch_space = new llvm::AllocaInst(g.i8, getConstantInt(min_bytes, g.i64), "scratch",
                                                 entry_block.getFirstInsertionPt());
    }

    assert(new_scratch_space->isStaticAlloca());

    if (scratch_space)
        scratch_space->replaceAllUsesWith(new_scratch_space);

    scratch_size = min_bytes;
    scratch_space = new_scratch_space;

    return scratch_space;
}

ScopeInfo* IRGenState::getScopeInfo() {
    SourceInfo* source = getSourceInfo();
    return source->scoping->getScopeInfoForNode(source->ast);
}

GuardList::ExprTypeGuard::ExprTypeGuard(CFGBlock* cfg_block, llvm::BranchInst* branch, AST_expr* ast_node,
                                        CompilerVariable* val, const SymbolTable& st)
    : cfg_block(cfg_block), branch(branch), ast_node(ast_node) {
    DupCache cache;
    this->val = val->dup(cache);

    for (const auto& p : st) {
        this->st[p.first] = p.second->dup(cache);
    }
}

GuardList::BlockEntryGuard::BlockEntryGuard(CFGBlock* cfg_block, llvm::BranchInst* branch,
                                            const SymbolTable& symbol_table)
    : cfg_block(cfg_block), branch(branch) {
    DupCache cache;
    for (const auto& p : symbol_table) {
        this->symbol_table[p.first] = p.second->dup(cache);
    }
}

class IREmitterImpl : public IREmitter {
private:
    IRGenState* irstate;
    IRBuilder* builder;
    llvm::BasicBlock*& curblock;

public:
    explicit IREmitterImpl(IRGenState* irstate, llvm::BasicBlock*& curblock)
        : irstate(irstate), builder(new IRBuilder(g.context)), curblock(curblock) {
        builder->setEmitter(this);
        builder->SetInsertPoint(curblock);
    }

    IRBuilder* getBuilder() override { return builder; }

    GCBuilder* getGC() override { return irstate->getGC(); }

    llvm::Function* getIntrinsic(llvm::Intrinsic::ID intrinsic_id) override {
        return llvm::Intrinsic::getDeclaration(g.cur_module, intrinsic_id);
    }

    llvm::Value* getScratch(int num_bytes) override { return irstate->getScratchSpace(num_bytes); }

    void releaseScratch(llvm::Value* scratch) override { assert(0); }

    CompiledFunction* currentFunction() override { return irstate->getCurFunction(); }

    llvm::CallSite createCall(ExcInfo exc_info, llvm::Value* callee, const std::vector<llvm::Value*>& args) override {
        if (exc_info.needsInvoke()) {
            llvm::BasicBlock* normal_dest
                = llvm::BasicBlock::Create(g.context, curblock->getName(), irstate->getLLVMFunction());
            normal_dest->moveAfter(curblock);

            llvm::InvokeInst* rtn = getBuilder()->CreateInvoke(callee, normal_dest, exc_info.exc_dest, args);
            getBuilder()->SetInsertPoint(normal_dest);
            curblock = normal_dest;
            return rtn;
        } else {
            return getBuilder()->CreateCall(callee, args);
        }
    }

    llvm::CallSite createCall(ExcInfo exc_info, llvm::Value* callee, llvm::Value* arg1) override {
        return createCall(exc_info, callee, std::vector<llvm::Value*>({ arg1 }));
    }

    llvm::CallSite createCall2(ExcInfo exc_info, llvm::Value* callee, llvm::Value* arg1, llvm::Value* arg2) override {
        return createCall(exc_info, callee, { arg1, arg2 });
    }

    llvm::CallSite createCall3(ExcInfo exc_info, llvm::Value* callee, llvm::Value* arg1, llvm::Value* arg2,
                               llvm::Value* arg3) override {
        return createCall(exc_info, callee, { arg1, arg2, arg3 });
    }

    llvm::CallSite createPatchpoint(const PatchpointSetupInfo* pp, void* func_addr,
                                    const std::vector<llvm::Value*>& args, ExcInfo exc_info) override {
        if (exc_info.needsInvoke()) {
            std::vector<llvm::Type*> arg_types;
            for (auto v : args)
                arg_types.push_back(v->getType());

            return createCall(
                exc_info, embedConstantPtr(func_addr, llvm::FunctionType::get(g.i64, arg_types, false)->getPointerTo()),
                args);
        }

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

        llvm::Intrinsic::ID intrinsic_id = pp->hasReturnValue() ? llvm::Intrinsic::experimental_patchpoint_i64
                                                                : llvm::Intrinsic::experimental_patchpoint_void;
        llvm::Function* patchpoint = this->getIntrinsic(intrinsic_id);

        llvm::CallSite rtn = this->createCall(exc_info, patchpoint, pp_args);
        rtn.setCallingConv(pp->getCallingConvention());
        return rtn;
    }
};
IREmitter* createIREmitter(IRGenState* irstate, llvm::BasicBlock*& curblock) {
    return new IREmitterImpl(irstate, curblock);
}

static std::unordered_map<AST_expr*, std::vector<const std::string*>*> made_keyword_storage;
static std::vector<const std::string*>* getKeywordNameStorage(AST_Call* node) {
    auto it = made_keyword_storage.find(node);
    if (it != made_keyword_storage.end())
        return it->second;

    auto rtn = new std::vector<const std::string*>();
    made_keyword_storage.insert(it, std::make_pair(node, rtn));
    return rtn;
}

static const std::string CREATED_CLOSURE_NAME = "!created_closure";
static const std::string PASSED_CLOSURE_NAME = "!passed_closure";

class IRGeneratorImpl : public IRGenerator {
private:
    IRGenState* irstate;

    llvm::BasicBlock* curblock;
    IREmitterImpl emitter;
    SymbolTable symbol_table;
    std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks;
    CFGBlock* myblock;
    TypeAnalysis* types;
    GuardList& out_guards;
    const GuardList& in_guards;

    enum State {
        PARTIAL,  // running through a partial block, waiting to hit the first in_guard
        RUNNING,  // normal
        DEAD,     // passed a Return statement; still syntatically valid but the code should not be compiled
        FINISHED, // passed a pseudo-node such as Branch or Jump; internal error if there are any more statements
    } state;

public:
    IRGeneratorImpl(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                    CFGBlock* myblock, TypeAnalysis* types, GuardList& out_guards, const GuardList& in_guards,
                    bool is_partial)
        : irstate(irstate), curblock(entry_blocks[myblock]), emitter(irstate, curblock), entry_blocks(entry_blocks),
          myblock(myblock), types(types), out_guards(out_guards), in_guards(in_guards),
          state(is_partial ? PARTIAL : RUNNING) {}

    ~IRGeneratorImpl() { delete emitter.getBuilder(); }

private:
    OpInfo getOpInfoForNode(AST* ast, ExcInfo exc_info) {
        assert(ast);

        EffortLevel::EffortLevel effort = irstate->getEffortLevel();
        bool record_types = (effort != EffortLevel::INTERPRETED && effort != EffortLevel::MAXIMAL);

        TypeRecorder* type_recorder;
        if (record_types) {
            type_recorder = getTypeRecorderForNode(ast);
        } else {
            type_recorder = NULL;
        }

        return OpInfo(irstate->getEffortLevel(), type_recorder, exc_info);
    }

    OpInfo getEmptyOpInfo(ExcInfo exc_info) { return OpInfo(irstate->getEffortLevel(), NULL, exc_info); }

    void createExprTypeGuard(llvm::Value* check_val, AST_expr* node, CompilerVariable* node_value) {
        assert(check_val->getType() == g.i1);

        llvm::Value* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1000), getConstantInt(1) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));

        // For some reason there doesn't seem to be the ability to place the new BB
        // right after the current bb (can only place it *before* something else),
        // but we can put it somewhere arbitrary and then move it.
        llvm::BasicBlock* success_bb
            = llvm::BasicBlock::Create(g.context, "check_succeeded", irstate->getLLVMFunction());
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

    CompilerVariable* evalAttribute(AST_Attribute* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* value = evalExpr(node->value, exc_info);

        CompilerVariable* rtn = value->getattr(emitter, getOpInfoForNode(node, exc_info), &node->attr, false);
        value->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalClsAttribute(AST_ClsAttribute* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* value = evalExpr(node->value, exc_info);
        CompilerVariable* rtn = value->getattr(emitter, getOpInfoForNode(node, exc_info), &node->attr, true);
        value->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalLangPrimitive(AST_LangPrimitive* node, ExcInfo exc_info) {
        switch (node->opcode) {
            case AST_LangPrimitive::ISINSTANCE: {
                assert(node->args.size() == 3);
                CompilerVariable* obj = evalExpr(node->args[0], exc_info);
                CompilerVariable* cls = evalExpr(node->args[1], exc_info);
                CompilerVariable* flags = evalExpr(node->args[2], exc_info);

                ConcreteCompilerVariable* converted_obj = obj->makeConverted(emitter, obj->getBoxType());
                ConcreteCompilerVariable* converted_cls = cls->makeConverted(emitter, cls->getBoxType());
                ConcreteCompilerVariable* converted_flags = flags->makeConverted(emitter, INT);
                obj->decvref(emitter);
                cls->decvref(emitter);
                flags->decvref(emitter);

                llvm::Value* v = emitter.createCall(exc_info, g.funcs.isinstance,
                                                    { converted_obj->getValue(), converted_cls->getValue(),
                                                      converted_flags->getValue() }).getInstruction();
                assert(v->getType() == g.i1);

                return new ConcreteCompilerVariable(BOOL, v, true);
            }
            case AST_LangPrimitive::LANDINGPAD: {
                // llvm::Function* _personality_func = g.stdlib_module->getFunction("__py_personality_v0");
                llvm::Function* _personality_func = g.stdlib_module->getFunction("__gxx_personality_v0");
                assert(_personality_func);
                llvm::Value* personality_func = g.cur_module->getOrInsertFunction(_personality_func->getName(),
                                                                                  _personality_func->getFunctionType());
                assert(personality_func);
                llvm::LandingPadInst* landing_pad = emitter.getBuilder()->CreateLandingPad(
                    llvm::StructType::create(std::vector<llvm::Type*>{ g.i8_ptr, g.i64 }), personality_func, 1);
                landing_pad->addClause(embedConstantPtr(NULL, g.i8_ptr));

                llvm::Value* exc_pointer = emitter.getBuilder()->CreateExtractValue(landing_pad, { 0 });

                llvm::Value* exc_obj;
                if (irstate->getEffortLevel() != EffortLevel::INTERPRETED) {
                    llvm::Value* exc_obj_pointer
                        = emitter.getBuilder()->CreateCall(g.funcs.__cxa_begin_catch, exc_pointer);
                    llvm::Value* exc_obj_pointer_casted
                        = emitter.getBuilder()->CreateBitCast(exc_obj_pointer, g.llvm_value_type_ptr->getPointerTo());
                    exc_obj = emitter.getBuilder()->CreateLoad(exc_obj_pointer_casted);
                    emitter.getBuilder()->CreateCall(g.funcs.__cxa_end_catch);
                } else {
                    // The interpreter can't really support the full C++ exception handling model since it's
                    // itself written in C++.  Let's make it easier for the interpreter and use a simpler interface:
                    exc_obj = emitter.getBuilder()->CreateBitCast(exc_pointer, g.llvm_value_type_ptr);
                }
                return new ConcreteCompilerVariable(UNKNOWN, exc_obj, true);
            }
            case AST_LangPrimitive::LOCALS: {
                assert(node->args.size() == 0);

                llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
                ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(DICT, v, true);

                for (auto& p : symbol_table) {
                    if (p.first[0] == '!')
                        continue;

                    ConcreteCompilerVariable* is_defined_var = static_cast<ConcreteCompilerVariable*>(
                        _getFake(_getFakeName("is_defined", p.first.c_str()), true));

                    static const std::string setitem_str("__setitem__");
                    if (!is_defined_var) {
                        ConcreteCompilerVariable* converted = p.second->makeConverted(emitter, p.second->getBoxType());

                        // TODO super dumb that it reallocates the name again
                        CompilerVariable* _r
                            = rtn->callattr(emitter, getEmptyOpInfo(exc_info), &setitem_str, true, ArgPassSpec(2),
                                            { makeStr(new std::string(p.first)), converted }, NULL);
                        converted->decvref(emitter);
                        _r->decvref(emitter);
                    } else {
                        assert(is_defined_var->getType() == BOOL);

                        llvm::BasicBlock* was_defined
                            = llvm::BasicBlock::Create(g.context, "was_defined", irstate->getLLVMFunction());
                        llvm::BasicBlock* join
                            = llvm::BasicBlock::Create(g.context, "join", irstate->getLLVMFunction());
                        emitter.getBuilder()->CreateCondBr(is_defined_var->getValue(), was_defined, join);

                        emitter.getBuilder()->SetInsertPoint(was_defined);
                        ConcreteCompilerVariable* converted = p.second->makeConverted(emitter, p.second->getBoxType());
                        // TODO super dumb that it reallocates the name again
                        CompilerVariable* _r
                            = rtn->callattr(emitter, getEmptyOpInfo(exc_info), &setitem_str, true, ArgPassSpec(2),
                                            { makeStr(new std::string(p.first)), converted }, NULL);
                        converted->decvref(emitter);
                        _r->decvref(emitter);
                        emitter.getBuilder()->CreateBr(join);
                        emitter.getBuilder()->SetInsertPoint(join);
                    }
                }

                return rtn;
            }
            default:
                RELEASE_ASSERT(0, "%d", node->opcode);
        }
    }

    CompilerVariable* _evalBinExp(AST* node, CompilerVariable* left, CompilerVariable* right, AST_TYPE::AST_TYPE type,
                                  BinExpType exp_type, ExcInfo exc_info) {
        assert(state != PARTIAL);

        assert(left);
        assert(right);

        return left->binexp(emitter, getOpInfoForNode(node, exc_info), right, type, exp_type);
    }

    CompilerVariable* evalBinOp(AST_BinOp* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* left = evalExpr(node->left, exc_info);
        CompilerVariable* right = evalExpr(node->right, exc_info);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        CompilerVariable* rtn = this->_evalBinExp(node, left, right, node->op_type, BinOp, exc_info);
        left->decvref(emitter);
        right->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalAugBinOp(AST_AugBinOp* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* left = evalExpr(node->left, exc_info);
        CompilerVariable* right = evalExpr(node->right, exc_info);

        assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

        CompilerVariable* rtn = this->_evalBinExp(node, left, right, node->op_type, AugBinOp, exc_info);
        left->decvref(emitter);
        right->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalCompare(AST_Compare* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        RELEASE_ASSERT(node->ops.size() == 1, "");

        CompilerVariable* left = evalExpr(node->left, exc_info);
        CompilerVariable* right = evalExpr(node->comparators[0], exc_info);

        assert(left);
        assert(right);

        CompilerVariable* rtn = _evalBinExp(node, left, right, node->ops[0], Compare, exc_info);
        left->decvref(emitter);
        right->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalCall(AST_Call* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        bool is_callattr;
        bool callattr_clsonly = false;
        std::string* attr = NULL;
        CompilerVariable* func;
        if (node->func->type == AST_TYPE::Attribute) {
            is_callattr = true;
            callattr_clsonly = false;
            AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
            func = evalExpr(attr_ast->value, exc_info);
            attr = &attr_ast->attr;
        } else if (node->func->type == AST_TYPE::ClsAttribute) {
            is_callattr = true;
            callattr_clsonly = true;
            AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
            func = evalExpr(attr_ast->value, exc_info);
            attr = &attr_ast->attr;
        } else {
            is_callattr = false;
            func = evalExpr(node->func, exc_info);
        }

        std::vector<CompilerVariable*> args;
        std::vector<const std::string*>* keyword_names;
        if (node->keywords.size()) {
            keyword_names = getKeywordNameStorage(node);

            // Only add the keywords to the array the first time, since
            // the later times we will hit the cache which will have the
            // keyword names already populated:
            if (!keyword_names->size()) {
                for (auto kw : node->keywords) {
                    keyword_names->push_back(&kw->arg);
                }
            }
        } else {
            keyword_names = NULL;
        }

        for (int i = 0; i < node->args.size(); i++) {
            CompilerVariable* a = evalExpr(node->args[i], exc_info);
            args.push_back(a);
        }

        for (int i = 0; i < node->keywords.size(); i++) {
            CompilerVariable* a = evalExpr(node->keywords[i]->value, exc_info);
            args.push_back(a);
        }

        if (node->starargs)
            args.push_back(evalExpr(node->starargs, exc_info));
        if (node->kwargs)
            args.push_back(evalExpr(node->kwargs, exc_info));

        struct ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs != NULL,
                                   node->kwargs != NULL);


        // if (VERBOSITY("irgen") >= 1)
        //_addAnnotation("before_call");

        CompilerVariable* rtn;
        if (is_callattr) {
            rtn = func->callattr(emitter, getOpInfoForNode(node, exc_info), attr, callattr_clsonly, argspec, args,
                                 keyword_names);
        } else {
            rtn = func->call(emitter, getOpInfoForNode(node, exc_info), argspec, args, keyword_names);
        }

        func->decvref(emitter);
        for (int i = 0; i < args.size(); i++) {
            args[i]->decvref(emitter);
        }

        return rtn;
    }

    CompilerVariable* evalDict(AST_Dict* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createDict);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(DICT, v, true);
        if (node->keys.size()) {
            static const std::string setitem_str("__setitem__");
            CompilerVariable* setitem = rtn->getattr(emitter, getEmptyOpInfo(exc_info), &setitem_str, true);
            for (int i = 0; i < node->keys.size(); i++) {
                CompilerVariable* key = evalExpr(node->keys[i], exc_info);
                CompilerVariable* value = evalExpr(node->values[i], exc_info);
                assert(key);
                assert(value);

                std::vector<CompilerVariable*> args;
                args.push_back(key);
                args.push_back(value);
                // TODO should use callattr
                CompilerVariable* rtn = setitem->call(emitter, getEmptyOpInfo(exc_info), ArgPassSpec(2), args, NULL);
                rtn->decvref(emitter);

                key->decvref(emitter);
                value->decvref(emitter);
            }
            setitem->decvref(emitter);
        }
        return rtn;
    }

    void _addAnnotation(const char* message) {
        llvm::Instruction* inst = emitter.getBuilder()->CreateCall(
            llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::donothing));
        llvm::Value* md_vals[] = { getConstantInt(0) };
        llvm::MDNode* mdnode = llvm::MDNode::get(g.context, md_vals);
        inst->setMetadata(message, mdnode);
    }

    CompilerVariable* evalIndex(AST_Index* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        return evalExpr(node->value, exc_info);
    }

    CompilerVariable* evalLambda(AST_Lambda* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        AST_Return* expr = new AST_Return();
        expr->value = node->body;

        std::vector<AST_stmt*> body = { expr };
        CompilerVariable* func = _createFunction(node, exc_info, node->args, body);
        ConcreteCompilerVariable* converted = func->makeConverted(emitter, func->getBoxType());
        func->decvref(emitter);

        return converted;
    }


    CompilerVariable* evalList(AST_List* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], exc_info);
            elts.push_back(value);
        }

        llvm::Value* v = emitter.getBuilder()->CreateCall(g.funcs.createList);
        ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(LIST, v, true);

        llvm::Value* f = g.funcs.listAppendInternal;
        llvm::Value* bitcast = emitter.getBuilder()->CreateBitCast(
            v, *llvm::cast<llvm::FunctionType>(llvm::cast<llvm::PointerType>(f->getType())->getElementType())
                    ->param_begin());

        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* elt = elts[i];
            ConcreteCompilerVariable* converted = elt->makeConverted(emitter, elt->getBoxType());
            elt->decvref(emitter);

            emitter.createCall2(exc_info, f, bitcast, converted->getValue());
            converted->decvref(emitter);
        }
        return rtn;
    }

    CompilerVariable* getNone() {
        ConcreteCompilerVariable* v = new ConcreteCompilerVariable(
            typeFromClass(none_cls), embedConstantPtr(None, g.llvm_value_type_ptr), false);
        return v;
    }

    ConcreteCompilerVariable* _getGlobal(AST_Name* node, ExcInfo exc_info) {
        bool do_patchpoint = ENABLE_ICGETGLOBALS && (irstate->getEffortLevel() != EffortLevel::INTERPRETED);
        if (do_patchpoint) {
            PatchpointSetupInfo* pp = patchpoints::createGetGlobalPatchpoint(
                emitter.currentFunction(), getOpInfoForNode(node, exc_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));
            llvm_args.push_back(embedConstantPtr(&node->id, g.llvm_str_type_ptr));

            llvm::Value* uncasted
                = emitter.createPatchpoint(pp, (void*)pyston::getGlobal, llvm_args, exc_info).getInstruction();
            llvm::Value* r = emitter.getBuilder()->CreateIntToPtr(uncasted, g.llvm_value_type_ptr);
            return new ConcreteCompilerVariable(UNKNOWN, r, true);
        } else {
            llvm::Value* r
                = emitter.createCall2(exc_info, g.funcs.getGlobal,
                                      embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr),
                                      embedConstantPtr(&node->id, g.llvm_str_type_ptr)).getInstruction();
            return new ConcreteCompilerVariable(UNKNOWN, r, true);
        }
    }

    CompilerVariable* evalName(AST_Name* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        auto scope_info = irstate->getScopeInfo();

        if (scope_info->refersToGlobal(node->id)) {
            return _getGlobal(node, exc_info);
        } else if (scope_info->refersToClosure(node->id)) {
            assert(scope_info->takesClosure());

            CompilerVariable* closure = _getFake(PASSED_CLOSURE_NAME, false);
            assert(closure);

            return closure->getattr(emitter, getEmptyOpInfo(exc_info), &node->id, false);
        } else {
            if (symbol_table.find(node->id) == symbol_table.end()) {
                // classdefs have different scoping rules than functions:
                if (irstate->getSourceInfo()->ast->type == AST_TYPE::ClassDef) {
                    return _getGlobal(node, exc_info);
                }

                // TODO should mark as DEAD here, though we won't end up setting all the names appropriately
                // state = DEAD;
                llvm::CallSite call = emitter.createCall2(exc_info, g.funcs.assertNameDefined, getConstantInt(0, g.i1),
                                                          getStringConstantPtr(node->id + '\0'));
                call.setDoesNotReturn();
                return undefVariable();
            }

            std::string defined_name = _getFakeName("is_defined", node->id.c_str());
            ConcreteCompilerVariable* is_defined_var
                = static_cast<ConcreteCompilerVariable*>(_getFake(defined_name, true));

            if (is_defined_var) {
                // classdefs have different scoping rules than functions:
                if (irstate->getSourceInfo()->ast->type == AST_TYPE::ClassDef) {
                    llvm::BasicBlock* from_local
                        = llvm::BasicBlock::Create(g.context, "from_local", irstate->getLLVMFunction());
                    llvm::BasicBlock* from_global
                        = llvm::BasicBlock::Create(g.context, "from_global", irstate->getLLVMFunction());
                    llvm::BasicBlock* join = llvm::BasicBlock::Create(g.context, "join", irstate->getLLVMFunction());

                    emitter.getBuilder()->CreateCondBr(is_defined_var->getValue(), from_local, from_global);

                    emitter.getBuilder()->SetInsertPoint(from_local);
                    CompilerVariable* local = symbol_table[node->id];
                    ConcreteCompilerVariable* converted_local = local->makeConverted(emitter, local->getBoxType());
                    // don't decvref local here, because are manufacturing a new vref
                    emitter.getBuilder()->CreateBr(join);

                    emitter.getBuilder()->SetInsertPoint(from_global);
                    ConcreteCompilerVariable* global = _getGlobal(node, exc_info);
                    emitter.getBuilder()->CreateBr(join);

                    emitter.getBuilder()->SetInsertPoint(join);
                    llvm::PHINode* phi = emitter.getBuilder()->CreatePHI(g.llvm_value_type_ptr, 2, node->id);
                    phi->addIncoming(converted_local->getValue(), from_local);
                    phi->addIncoming(global->getValue(), from_global);

                    return new ConcreteCompilerVariable(UNKNOWN, phi, true);
                }

                emitter.createCall2(exc_info, g.funcs.assertNameDefined, is_defined_var->getValue(),
                                    getStringConstantPtr(node->id + '\0'));

                // At this point we know the name must be defined (otherwise the assert would have fired):
                _popFake(defined_name);
            }

            CompilerVariable* rtn = symbol_table[node->id];
            rtn->incvref();
            return rtn;
        }
    }

    CompilerVariable* evalNum(AST_Num* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        if (node->num_type == AST_Num::INT)
            return makeInt(node->n_int);
        else if (node->num_type == AST_Num::FLOAT)
            return makeFloat(node->n_float);
        else
            RELEASE_ASSERT(0, "");
    }

    CompilerVariable* evalRepr(AST_Repr* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* var = evalExpr(node->value, exc_info);
        ConcreteCompilerVariable* cvar = var->makeConverted(emitter, var->getBoxType());
        var->decvref(emitter);

        std::vector<llvm::Value*> args{ cvar->getValue() };
        llvm::Value* rtn = emitter.createCall(exc_info, g.funcs.repr, args).getInstruction();
        cvar->decvref(emitter);

        return new ConcreteCompilerVariable(STR, rtn, true);
    }

    CompilerVariable* evalSlice(AST_Slice* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* start, *stop, *step;
        start = node->lower ? evalExpr(node->lower, exc_info) : getNone();
        stop = node->upper ? evalExpr(node->upper, exc_info) : getNone();
        step = node->step ? evalExpr(node->step, exc_info) : getNone();

        ConcreteCompilerVariable* cstart, *cstop, *cstep;
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

    CompilerVariable* evalStr(AST_Str* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        return makeStr(&node->s);
    }

    CompilerVariable* evalSubscript(AST_Subscript* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* value = evalExpr(node->value, exc_info);
        CompilerVariable* slice = evalExpr(node->slice, exc_info);

        CompilerVariable* rtn = value->getitem(emitter, getOpInfoForNode(node, exc_info), slice);
        value->decvref(emitter);
        slice->decvref(emitter);
        return rtn;
    }

    CompilerVariable* evalTuple(AST_Tuple* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        std::vector<CompilerVariable*> elts;
        for (int i = 0; i < node->elts.size(); i++) {
            CompilerVariable* value = evalExpr(node->elts[i], exc_info);
            elts.push_back(value);
        }

        // TODO makeTuple should probably just transfer the vref, but I want to keep things consistent
        CompilerVariable* rtn = makeTuple(elts);
        for (int i = 0; i < node->elts.size(); i++) {
            elts[i]->decvref(emitter);
        }
        return rtn;
    }

    CompilerVariable* evalUnaryOp(AST_UnaryOp* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        CompilerVariable* operand = evalExpr(node->operand, exc_info);

        if (node->op_type == AST_TYPE::Not) {
            ConcreteCompilerVariable* rtn = operand->nonzero(emitter, getOpInfoForNode(node, exc_info));
            operand->decvref(emitter);

            llvm::Value* v = rtn->getValue();
            assert(v->getType() == g.i1);
            llvm::Value* negated = emitter.getBuilder()->CreateNot(v);
            rtn->decvref(emitter);
            return new ConcreteCompilerVariable(BOOL, negated, true);
        } else {
            // TODO These are pretty inefficient, but luckily I don't think they're used that often:
            ConcreteCompilerVariable* converted = operand->makeConverted(emitter, operand->getBoxType());
            operand->decvref(emitter);

            llvm::Value* rtn = emitter.createCall2(exc_info, g.funcs.unaryop, converted->getValue(),
                                                   getConstantInt(node->op_type, g.i32)).getInstruction();
            converted->decvref(emitter);

            return new ConcreteCompilerVariable(UNKNOWN, rtn, true);
        }
    }

    ConcreteCompilerVariable* unboxVar(ConcreteCompilerType* t, llvm::Value* v, bool grabbed) {
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
        if (t == BOXED_BOOL) {
            llvm::Value* unboxed = emitter.getBuilder()->CreateCall(g.funcs.unboxBool, v);
            ConcreteCompilerVariable* rtn = new ConcreteCompilerVariable(BOOL, unboxed, true);
            return rtn;
        }
        return new ConcreteCompilerVariable(t, v, grabbed);
    }

    CompilerVariable* evalExpr(AST_expr* node, ExcInfo exc_info) {
        // printf("%d expr: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        CompilerVariable* rtn = NULL;
        if (state != PARTIAL) {
            switch (node->type) {
                case AST_TYPE::Attribute:
                    rtn = evalAttribute(ast_cast<AST_Attribute>(node), exc_info);
                    break;
                case AST_TYPE::AugBinOp:
                    rtn = evalAugBinOp(ast_cast<AST_AugBinOp>(node), exc_info);
                    break;
                case AST_TYPE::BinOp:
                    rtn = evalBinOp(ast_cast<AST_BinOp>(node), exc_info);
                    break;
                case AST_TYPE::Call:
                    rtn = evalCall(ast_cast<AST_Call>(node), exc_info);
                    break;
                case AST_TYPE::Compare:
                    rtn = evalCompare(ast_cast<AST_Compare>(node), exc_info);
                    break;
                case AST_TYPE::Dict:
                    rtn = evalDict(ast_cast<AST_Dict>(node), exc_info);
                    break;
                case AST_TYPE::Index:
                    rtn = evalIndex(ast_cast<AST_Index>(node), exc_info);
                    break;
                case AST_TYPE::Lambda:
                    rtn = evalLambda(ast_cast<AST_Lambda>(node), exc_info);
                    break;
                case AST_TYPE::List:
                    rtn = evalList(ast_cast<AST_List>(node), exc_info);
                    break;
                case AST_TYPE::Name:
                    rtn = evalName(ast_cast<AST_Name>(node), exc_info);
                    break;
                case AST_TYPE::Num:
                    rtn = evalNum(ast_cast<AST_Num>(node), exc_info);
                    break;
                case AST_TYPE::Repr:
                    rtn = evalRepr(ast_cast<AST_Repr>(node), exc_info);
                    break;
                case AST_TYPE::Slice:
                    rtn = evalSlice(ast_cast<AST_Slice>(node), exc_info);
                    break;
                case AST_TYPE::Str:
                    rtn = evalStr(ast_cast<AST_Str>(node), exc_info);
                    break;
                case AST_TYPE::Subscript:
                    rtn = evalSubscript(ast_cast<AST_Subscript>(node), exc_info);
                    break;
                case AST_TYPE::Tuple:
                    rtn = evalTuple(ast_cast<AST_Tuple>(node), exc_info);
                    break;
                case AST_TYPE::UnaryOp:
                    rtn = evalUnaryOp(ast_cast<AST_UnaryOp>(node), exc_info);
                    break;
                case AST_TYPE::ClsAttribute:
                    rtn = evalClsAttribute(ast_cast<AST_ClsAttribute>(node), exc_info);
                    break;
                case AST_TYPE::LangPrimitive:
                    rtn = evalLangPrimitive(ast_cast<AST_LangPrimitive>(node), exc_info);
                    break;
                default:
                    printf("Unhandled expr type: %d (irgenerator.cpp:" STRINGIFY(__LINE__) ")\n", node->type);
                    exit(1);
            }

            assert(rtn);

            // Out-guarding:
            BoxedClass* speculated_class = types->speculatedExprClass(node);
            if (speculated_class != NULL) {
                assert(rtn);

                ConcreteCompilerType* speculated_type = typeFromClass(speculated_class);
                if (VERBOSITY("irgen") >= 1) {
                    printf("Speculating that %s is actually %s, at ", rtn->getConcreteType()->debugName().c_str(),
                           speculated_type->debugName().c_str());
                    PrintVisitor printer;
                    node->accept(&printer);
                    printf("\n");
                }

                // That's not really a speculation.... could potentially handle this here, but
                // I think it's better to just not generate bad speculations:
                assert(!rtn->canConvertTo(speculated_type));

                ConcreteCompilerVariable* old_rtn = rtn->makeConverted(emitter, UNKNOWN);
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
                llvm::BasicBlock* ramp_block
                    = llvm::BasicBlock::Create(g.context, "deopt_ramp", irstate->getLLVMFunction());
                llvm::BasicBlock* join_block
                    = llvm::BasicBlock::Create(g.context, "deopt_join", irstate->getLLVMFunction());
                SymbolTable joined_st;
                for (const auto& p : guard->st) {
                    // if (VERBOSITY("irgen") >= 1) printf("merging %s\n", p.first.c_str());
                    CompilerVariable* curval = symbol_table[p.first];
                    // I'm not sure this is necessary or even correct:
                    // ASSERT(curval->getVrefs() == p.second->getVrefs(), "%s %d %d", p.first.c_str(),
                    // curval->getVrefs(), p.second->getVrefs());

                    ConcreteCompilerType* merged_type = curval->getConcreteType();

                    emitter.getBuilder()->SetInsertPoint(ramp_block);
                    ConcreteCompilerVariable* converted1 = p.second->makeConverted(emitter, merged_type);
                    p.second->decvref(emitter); // for makeconverted
                    // guard->st[p.first] = converted;
                    // p.second->decvref(emitter); // for the replaced version

                    emitter.getBuilder()->SetInsertPoint(curblock);
                    ConcreteCompilerVariable* converted2 = curval->makeConverted(emitter, merged_type);
                    curval->decvref(emitter); // for makeconverted
                    // symbol_table[p.first] = converted;
                    // curval->decvref(emitter); // for the replaced version

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
                    ConcreteCompilerType* this_merged_type = rtn->getConcreteType();

                    emitter.getBuilder()->SetInsertPoint(ramp_block);
                    ConcreteCompilerVariable* converted_guard_rtn
                        = guard->val->makeConverted(emitter, this_merged_type);
                    guard->val->decvref(emitter);

                    emitter.getBuilder()->SetInsertPoint(curblock);
                    ConcreteCompilerVariable* converted_rtn = rtn->makeConverted(emitter, this_merged_type);
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
        CompilerVariable*& cur = symbol_table[name];
        assert(cur == NULL);
        cur = val;
    }

    CompilerVariable* _getFake(std::string name, bool allow_missing = false) {
        assert(name[0] == '!');
        auto it = symbol_table.find(name);
        if (it == symbol_table.end()) {
            assert(allow_missing);
            return NULL;
        }
        return it->second;
    }

    CompilerVariable* _popFake(std::string name, bool allow_missing = false) {
        CompilerVariable* rtn = _getFake(name, allow_missing);
        symbol_table.erase(name);
        if (!allow_missing)
            assert(rtn != NULL);
        return rtn;
    }

    void _doSet(const std::string& name, CompilerVariable* val, ExcInfo exc_info) {
        assert(name != "None");

        auto scope_info = irstate->getScopeInfo();
        assert(!scope_info->refersToClosure(name));

        if (scope_info->refersToGlobal(name)) {
            assert(!scope_info->saveInClosure(name));

            // TODO do something special here so that it knows to only emit a monomorphic inline cache?
            ConcreteCompilerVariable* module = new ConcreteCompilerVariable(
                MODULE, embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_value_type_ptr), false);
            module->setattr(emitter, getEmptyOpInfo(exc_info), &name, val);
            module->decvref(emitter);
        } else {
            CompilerVariable*& prev = symbol_table[name];
            if (prev != NULL) {
                prev->decvref(emitter);
            }
            prev = val;
            val->incvref();

            // Clear out the is_defined name since it is now definitely defined:
            assert(!startswith(name, "!is_defined"));
            std::string defined_name = _getFakeName("is_defined", name.c_str());
            _popFake(defined_name, true);

            if (scope_info->saveInClosure(name)) {
                CompilerVariable* closure = _getFake(CREATED_CLOSURE_NAME, false);
                assert(closure);

                closure->setattr(emitter, getEmptyOpInfo(ExcInfo::none()), &name, val);
            }
        }
    }

    void _doSetattr(AST_Attribute* target, CompilerVariable* val, ExcInfo exc_info) {
        assert(state != PARTIAL);
        CompilerVariable* t = evalExpr(target->value, exc_info);
        t->setattr(emitter, getEmptyOpInfo(exc_info), &target->attr, val);
        t->decvref(emitter);
    }

    void _doSetitem(AST_Subscript* target, CompilerVariable* val, ExcInfo exc_info) {
        assert(state != PARTIAL);
        CompilerVariable* tget = evalExpr(target->value, exc_info);
        CompilerVariable* slice = evalExpr(target->slice, exc_info);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());
        tget->decvref(emitter);
        slice->decvref(emitter);

        ConcreteCompilerVariable* converted_val = val->makeConverted(emitter, val->getBoxType());

        // TODO add a CompilerVariable::setattr, which can (similar to getitem)
        // statically-resolve the function if possible, and only fall back to
        // patchpoints if it couldn't.
        bool do_patchpoint = ENABLE_ICSETITEMS && (irstate->getEffortLevel() != EffortLevel::INTERPRETED);
        if (do_patchpoint) {
            PatchpointSetupInfo* pp = patchpoints::createSetitemPatchpoint(emitter.currentFunction(),
                                                                           getEmptyOpInfo(exc_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());
            llvm_args.push_back(converted_val->getValue());

            emitter.createPatchpoint(pp, (void*)pyston::setitem, llvm_args, exc_info);
        } else {
            emitter.createCall3(exc_info, g.funcs.setitem, converted_target->getValue(), converted_slice->getValue(),
                                converted_val->getValue());
        }

        converted_target->decvref(emitter);
        converted_slice->decvref(emitter);
        converted_val->decvref(emitter);
    }

    void _doUnpackTuple(AST_Tuple* target, CompilerVariable* val, ExcInfo exc_info) {
        assert(state != PARTIAL);
        int ntargets = target->elts.size();
        // TODO do type recording here?
        ConcreteCompilerVariable* len = val->len(emitter, getEmptyOpInfo(exc_info));
        emitter.createCall2(exc_info, g.funcs.checkUnpackingLength, getConstantInt(ntargets, g.i64), len->getValue());

        for (int i = 0; i < ntargets; i++) {
            CompilerVariable* unpacked = val->getitem(emitter, getEmptyOpInfo(exc_info), makeInt(i));
            _doSet(target->elts[i], unpacked, exc_info);
            unpacked->decvref(emitter);
        }
    }

    void _doSet(AST* target, CompilerVariable* val, ExcInfo exc_info) {
        assert(state != PARTIAL);
        switch (target->type) {
            case AST_TYPE::Attribute:
                _doSetattr(ast_cast<AST_Attribute>(target), val, exc_info);
                break;
            case AST_TYPE::Name:
                _doSet(ast_cast<AST_Name>(target)->id, val, exc_info);
                break;
            case AST_TYPE::Subscript:
                _doSetitem(ast_cast<AST_Subscript>(target), val, exc_info);
                break;
            case AST_TYPE::Tuple:
                _doUnpackTuple(ast_cast<AST_Tuple>(target), val, exc_info);
                break;
            default:
                ASSERT(0, "Unknown type for IRGenerator: %d", target->type);
                abort();
        }
    }

    void doAssert(AST_Assert* node, ExcInfo exc_info) {
        AST_expr* test = node->test;
        assert(test->type == AST_TYPE::Num);
        AST_Num* num = ast_cast<AST_Num>(test);
        assert(num->num_type == AST_Num::INT);
        assert(num->n_int == 0);

        std::vector<llvm::Value*> llvm_args;
        llvm_args.push_back(embedConstantPtr(irstate->getSourceInfo()->parent_module, g.llvm_module_type_ptr));

        ConcreteCompilerVariable* converted_msg = NULL;
        if (node->msg) {
            CompilerVariable* msg = evalExpr(node->msg, exc_info);
            converted_msg = msg->makeConverted(emitter, msg->getBoxType());
            msg->decvref(emitter);
            llvm_args.push_back(converted_msg->getValue());
        } else {
            llvm_args.push_back(embedConstantPtr(NULL, g.llvm_value_type_ptr));
        }
        llvm::CallSite call = emitter.createCall(exc_info, g.funcs.assertFail, llvm_args);
        call.setDoesNotReturn();
    }

    void doAssign(AST_Assign* node, ExcInfo exc_info) {
        CompilerVariable* val = evalExpr(node->value, exc_info);
        if (state == PARTIAL)
            return;

        for (int i = 0; i < node->targets.size(); i++) {
            _doSet(node->targets[i], val, exc_info);
        }
        val->decvref(emitter);
    }

    void doClassDef(AST_ClassDef* node, ExcInfo exc_info) {
        if (state == PARTIAL)
            return;

        assert(node->type == AST_TYPE::ClassDef);
        ScopeInfo* scope_info = irstate->getSourceInfo()->scoping->getScopeInfoForNode(node);
        assert(scope_info);

        if (node->bases.size() == 0) {
            printf("Warning: old-style class '%s' in file '%s' detected! Converting to a new-style class!\n",
                   node->name.c_str(), irstate->getSourceInfo()->parent_module->fn.c_str());

            AST_Name* base = new AST_Name();
            base->id = "object";
            base->ctx_type = AST_TYPE::Load;
            node->bases.push_back(base);
        }

        RELEASE_ASSERT(node->bases.size() == 1, "");
        RELEASE_ASSERT(node->decorator_list.size() == 0, "");

        CompilerVariable* base = evalExpr(node->bases[0], exc_info);
        ConcreteCompilerVariable* converted_base = base->makeConverted(emitter, base->getBoxType());
        base->decvref(emitter);

        CLFunction* cl = _wrapFunction(node, nullptr, node->body);

        // TODO duplication with _createFunction:
        CompilerVariable* created_closure = NULL;
        if (scope_info->takesClosure()) {
            created_closure = _getFake(CREATED_CLOSURE_NAME, false);
            assert(created_closure);
        }

        // TODO kind of silly to create the function just to usually-delete it afterwards;
        // one reason to do this is to pass the closure through if necessary,
        // but since the classdef can't create its own closure, shouldn't need to explicitly
        // create that scope to pass the closure through.
        CompilerVariable* func = makeFunction(emitter, cl, created_closure, {});

        CompilerVariable* attr_dict = func->call(emitter, getEmptyOpInfo(exc_info), ArgPassSpec(0), {}, NULL);

        func->decvref(emitter);

        ConcreteCompilerVariable* converted_attr_dict = attr_dict->makeConverted(emitter, attr_dict->getBoxType());
        attr_dict->decvref(emitter);


        llvm::Value* classobj
            = emitter.createCall3(exc_info, g.funcs.createUserClass, embedConstantPtr(&node->name, g.llvm_str_type_ptr),
                                  converted_base->getValue(), converted_attr_dict->getValue()).getInstruction();

        // Note: createuserClass is free to manufacture non-class objects
        auto cls = new ConcreteCompilerVariable(UNKNOWN, classobj, true);
        _doSet(node->name, cls, exc_info);
        cls->decvref(emitter);
    }

    void doDelete(AST_Delete* node, ExcInfo exc_info) {
        assert(state != PARTIAL);
        for (AST_expr* target : node->targets) {
            switch (target->type) {
                case AST_TYPE::Subscript:
                    _doDelitem(static_cast<AST_Subscript*>(target), exc_info);
                    break;
                default:
                    ASSERT(0, "UnSupported del target: %d", target->type);
                    abort();
            }
        }
    }

    // invoke delitem in objmodel.cpp, which will invoke the listDelitem of list
    void _doDelitem(AST_Subscript* target, ExcInfo exc_info) {
        assert(state != PARTIAL);
        CompilerVariable* tget = evalExpr(target->value, exc_info);
        CompilerVariable* slice = evalExpr(target->slice, exc_info);

        ConcreteCompilerVariable* converted_target = tget->makeConverted(emitter, tget->getBoxType());
        ConcreteCompilerVariable* converted_slice = slice->makeConverted(emitter, slice->getBoxType());
        tget->decvref(emitter);
        slice->decvref(emitter);

        bool do_patchpoint = ENABLE_ICDELITEMS && (irstate->getEffortLevel() != EffortLevel::INTERPRETED);
        if (do_patchpoint) {
            PatchpointSetupInfo* pp = patchpoints::createDelitemPatchpoint(emitter.currentFunction(),
                                                                           getEmptyOpInfo(exc_info).getTypeRecorder());

            std::vector<llvm::Value*> llvm_args;
            llvm_args.push_back(converted_target->getValue());
            llvm_args.push_back(converted_slice->getValue());

            emitter.createPatchpoint(pp, (void*)pyston::delitem, llvm_args, exc_info);
        } else {
            emitter.getBuilder()->CreateCall2(g.funcs.delitem, converted_target->getValue(),
                                              converted_slice->getValue());
        }

        converted_target->decvref(emitter);
        converted_slice->decvref(emitter);
    }

    CLFunction* _wrapFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body) {
        // Different compilations of the parent scope of a functiondef should lead
        // to the same CLFunction* being used:
        static std::unordered_map<AST*, CLFunction*> made;

        CLFunction*& cl = made[node];
        if (cl == NULL) {
            SourceInfo* source = irstate->getSourceInfo();
            SourceInfo* si = new SourceInfo(source->parent_module, source->scoping, node, body);
            if (args)
                cl = new CLFunction(args->args.size(), args->defaults.size(), args->vararg.size(), args->kwarg.size(),
                                    si);
            else
                cl = new CLFunction(0, 0, 0, 0, si);
        }
        return cl;
    }

    CompilerVariable* _createFunction(AST* node, ExcInfo exc_info, AST_arguments* args,
                                      const std::vector<AST_stmt*>& body) {
        CLFunction* cl = this->_wrapFunction(node, args, body);

        std::vector<ConcreteCompilerVariable*> defaults;
        for (auto d : args->defaults) {
            CompilerVariable* e = evalExpr(d, exc_info);
            ConcreteCompilerVariable* converted = e->makeConverted(emitter, e->getBoxType());
            e->decvref(emitter);
            defaults.push_back(converted);
        }

        CompilerVariable* created_closure = NULL;

        bool takes_closure;
        // Optimization: when compiling a module, it's nice to not have to run analyses into the
        // entire module's source code.
        // If we call getScopeInfoForNode, that will trigger an analysis of that function tree,
        // but we're only using it here to figure out if that function takes a closure.
        // Top level functions never take a closure, so we can skip the analysis.
        if (irstate->getSourceInfo()->ast->type == AST_TYPE::Module)
            takes_closure = false;
        else
            takes_closure = irstate->getSourceInfo()->scoping->getScopeInfoForNode(node)->takesClosure();

        if (takes_closure) {
            if (irstate->getScopeInfo()->createsClosure()) {
                created_closure = _getFake(CREATED_CLOSURE_NAME, false);
            } else {
                assert(irstate->getScopeInfo()->passesThroughClosure());
                created_closure = _getFake(PASSED_CLOSURE_NAME, false);
            }
            assert(created_closure);
        }

        CompilerVariable* func = makeFunction(emitter, cl, created_closure, defaults);

        for (auto d : defaults) {
            d->decvref(emitter);
        }

        // llvm::Type* boxCLFuncArgType = g.funcs.boxCLFunction->arg_begin()->getType();
        // llvm::Value *boxed = emitter.getBuilder()->CreateCall(g.funcs.boxCLFunction, embedConstantPtr(cl,
        // boxCLFuncArgType));
        // CompilerVariable *func = new ConcreteCompilerVariable(typeFromClass(function_cls), boxed, true);
        return func;
    }

    void doFunctionDef(AST_FunctionDef* node, ExcInfo exc_info) {
        if (state == PARTIAL)
            return;

        assert(!node->decorator_list.size());

        CompilerVariable* func = _createFunction(node, exc_info, node->args, node->body);
        _doSet(node->name, func, exc_info);
        func->decvref(emitter);
    }

    void doImport(AST_Import* node, ExcInfo exc_info) {
        if (state == PARTIAL)
            return;

        for (int i = 0; i < node->names.size(); i++) {
            AST_alias* alias = node->names[i];

            const std::string& modname = alias->name;
            const std::string& asname = alias->asname.size() ? alias->asname : alias->name;

            llvm::Value* imported
                = emitter.createCall(exc_info, g.funcs.import, embedConstantPtr(&modname, g.llvm_str_type_ptr))
                      .getInstruction();
            ConcreteCompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, imported, true);
            _doSet(asname, v, exc_info);
            v->decvref(emitter);
        }
    }

    void doImportFrom(AST_ImportFrom* node, ExcInfo exc_info) {
        if (state == PARTIAL)
            return;

        assert(node->level == 0);

        llvm::Value* imported_v
            = emitter.createCall(exc_info, g.funcs.import, embedConstantPtr(&node->module, g.llvm_str_type_ptr))
                  .getInstruction();
        ConcreteCompilerVariable* module = new ConcreteCompilerVariable(typeFromClass(module_cls), imported_v, true);

        for (int i = 0; i < node->names.size(); i++) {
            AST_alias* alias = node->names[i];

            const std::string& name = alias->name;
            const std::string& asname = alias->asname.size() ? alias->asname : alias->name;

            // TODO add patchpoints to this?
            llvm::Value* r = emitter.createCall2(exc_info, g.funcs.importFrom, module->getValue(),
                                                 embedConstantPtr(&name, g.llvm_str_type_ptr)).getInstruction();

            CompilerVariable* v = new ConcreteCompilerVariable(UNKNOWN, r, true);
            _doSet(asname, v, exc_info);
            v->decvref(emitter);
        }

        module->decvref(emitter);
    }

    void doPrint(AST_Print* node, ExcInfo exc_info) {
        if (state == PARTIAL)
            return;

        RELEASE_ASSERT(node->dest == NULL, "");
        for (int i = 0; i < node->values.size(); i++) {
            if (i > 0) {
                emitter.getBuilder()->CreateCall(g.funcs.printf, getStringConstantPtr(" "));
            }
            CompilerVariable* var = evalExpr(node->values[i], exc_info);
            var->print(emitter, getEmptyOpInfo(exc_info));
            var->decvref(emitter);
        }

        if (node->nl)
            emitter.createCall(exc_info, g.funcs.printf, getStringConstantPtr("\n"));
        else
            emitter.createCall(exc_info, g.funcs.printf, getStringConstantPtr(" "));
    }

    void doReturn(AST_Return* node, ExcInfo exc_info) {
        assert(!exc_info.needsInvoke());

        CompilerVariable* val;
        if (node->value == NULL) {
            if (irstate->getReturnType() == VOID) {
                endBlock(DEAD);
                emitter.getBuilder()->CreateRetVoid();
                return;
            }

            val = new ConcreteCompilerVariable(NONE, embedConstantPtr(None, g.llvm_value_type_ptr), false);
        } else {
            val = evalExpr(node->value, exc_info);
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
        ConcreteCompilerType* opt_rtn_type = irstate->getReturnType();
        if (irstate->getReturnType()->llvmType() == val->getConcreteType()->llvmType())
            opt_rtn_type = val->getConcreteType();

        ConcreteCompilerVariable* rtn = val->makeConverted(emitter, opt_rtn_type);
        rtn->ensureGrabbed(emitter);
        val->decvref(emitter);

        for (auto& p : symbol_table) {
            p.second->decvref(emitter);
        }
        symbol_table.clear();

        endBlock(DEAD);

        ASSERT(rtn->getVrefs() == 1, "%d", rtn->getVrefs());
        assert(rtn->getValue());
        emitter.getBuilder()->CreateRet(rtn->getValue());
    }

    void doBranch(AST_Branch* node, ExcInfo exc_info) {
        assert(!exc_info.needsInvoke());

        assert(node->iftrue->idx > myblock->idx);
        assert(node->iffalse->idx > myblock->idx);

        CompilerVariable* val = evalExpr(node->test, exc_info);
        assert(state != PARTIAL);
        assert(val);

        // ASSERT(val->getType() == BOOL, "%s", val->getType()->debugName().c_str());

        ConcreteCompilerVariable* nonzero = val->nonzero(emitter, getOpInfoForNode(node, exc_info));
        assert(nonzero->getType() == BOOL);
        val->decvref(emitter);

        llvm::Value* llvm_nonzero = nonzero->getValue();
        llvm::BasicBlock* iftrue = entry_blocks[node->iftrue];
        llvm::BasicBlock* iffalse = entry_blocks[node->iffalse];

        nonzero->decvref(emitter);

        endBlock(FINISHED);

        emitter.getBuilder()->CreateCondBr(llvm_nonzero, iftrue, iffalse);
    }

    void doExpr(AST_Expr* node, ExcInfo exc_info) {
        CompilerVariable* var = evalExpr(node->value, exc_info);
        if (state == PARTIAL)
            return;

        var->decvref(emitter);
    }

    void doOSRExit(llvm::BasicBlock* normal_target, AST_Jump* osr_key) {
        assert(state != PARTIAL);

        llvm::BasicBlock* starting_block = curblock;
        llvm::BasicBlock* onramp = llvm::BasicBlock::Create(g.context, "onramp", irstate->getLLVMFunction());

        // Code to check if we want to do the OSR:
        llvm::GlobalVariable* edgecount_ptr = new llvm::GlobalVariable(
            *g.cur_module, g.i64, false, llvm::GlobalValue::InternalLinkage, getConstantInt(0, g.i64), "edgecount");
        llvm::Value* curcount = emitter.getBuilder()->CreateLoad(edgecount_ptr);
        llvm::Value* newcount = emitter.getBuilder()->CreateAdd(curcount, getConstantInt(1, g.i64));
        emitter.getBuilder()->CreateStore(newcount, edgecount_ptr);

        int OSR_THRESHOLD = 10000;
        if (irstate->getEffortLevel() == EffortLevel::INTERPRETED)
            OSR_THRESHOLD = 100;
        llvm::Value* osr_test = emitter.getBuilder()->CreateICmpSGT(newcount, getConstantInt(OSR_THRESHOLD));

        llvm::Value* md_vals[]
            = { llvm::MDString::get(g.context, "branch_weights"), getConstantInt(1), getConstantInt(1000) };
        llvm::MDNode* branch_weights = llvm::MDNode::get(g.context, llvm::ArrayRef<llvm::Value*>(md_vals));
        emitter.getBuilder()->CreateCondBr(osr_test, onramp, normal_target, branch_weights);

        // Emitting the actual OSR:
        emitter.getBuilder()->SetInsertPoint(onramp);
        OSRExit* exit
            = new OSRExit(irstate->getCurFunction(), OSREntryDescriptor::create(irstate->getCurFunction(), osr_key));
        llvm::Value* partial_func = emitter.getBuilder()->CreateCall(g.funcs.compilePartialFunc,
                                                                     embedConstantPtr(exit, g.i8->getPointerTo()));

        std::vector<llvm::Value*> llvm_args;
        std::vector<llvm::Type*> llvm_arg_types;
        std::vector<ConcreteCompilerVariable*> converted_args;

        SortedSymbolTable sorted_symbol_table(symbol_table.begin(), symbol_table.end());

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

        llvm::Value* arg_array = NULL, *malloc_save = NULL;
        if (sorted_symbol_table.size() > 3) {
            // Leave in the ability to use malloc but I guess don't use it.
            // Maybe if there are a ton of live variables it'd be nice to have them be
            // heap-allocated, or if we don't immediately return the result of the OSR?
            bool use_malloc = false;
            if (false) {
                llvm::Value* n_bytes = getConstantInt((sorted_symbol_table.size() - 3) * sizeof(Box*), g.i64);
                llvm::Value* l_malloc = embedConstantPtr(
                    (void*)malloc, llvm::FunctionType::get(g.i8->getPointerTo(), g.i64, false)->getPointerTo());
                malloc_save = emitter.getBuilder()->CreateCall(l_malloc, n_bytes);
                arg_array = emitter.getBuilder()->CreateBitCast(malloc_save, g.llvm_value_type_ptr->getPointerTo());
            } else {
                llvm::Value* n_varargs = llvm::ConstantInt::get(g.i64, sorted_symbol_table.size() - 3, false);
                arg_array = emitter.getBuilder()->CreateAlloca(g.llvm_value_type_ptr, n_varargs);
            }
        }

        int arg_num = -1;
        for (const auto& p : sorted_symbol_table) {
            arg_num++;

            // This line can never get hit right now since we unnecessarily force every variable to be concrete
            // for a loop, since we generate all potential phis:
            ASSERT(p.second->getType() == p.second->getConcreteType(), "trying to pass through %s\n",
                   p.second->getType()->debugName().c_str());

            ConcreteCompilerVariable* var = p.second->makeConverted(emitter, p.second->getConcreteType());
            converted_args.push_back(var);

            assert(var->getType() != BOXED_INT && "should probably unbox it, but why is it boxed in the first place?");
            assert(var->getType() != BOXED_FLOAT
                   && "should probably unbox it, but why is it boxed in the first place?");

            // This line can never get hit right now for the same reason that the variables must already be concrete,
            // because we're over-generating phis.
            ASSERT(var->isGrabbed(), "%s", p.first.c_str());
            // var->ensureGrabbed(emitter);

            llvm::Value* val = var->getValue();

            if (arg_num < 3) {
                llvm_args.push_back(val);
                llvm_arg_types.push_back(val->getType());
            } else {
                llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(arg_array, arg_num - 3);

                if (var->getType() == INT || var->getType() == BOOL) {
                    val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                } else if (var->getType() == FLOAT) {
                    // val = emitter.getBuilder()->CreateBitCast(val, g.llvm_value_type_ptr);
                    ptr = emitter.getBuilder()->CreateBitCast(ptr, g.double_->getPointerTo());
                } else if (var->getType() == UNDEF) {
                    // TODO if there are any undef variables, we're in 'unreachable' territory.
                    // Do we even need to generate any of this code?

                    // Currently we represent 'undef's as 'i16 undef'
                    val = emitter.getBuilder()->CreateIntToPtr(val, g.llvm_value_type_ptr);
                } else {
                    assert(val->getType() == g.llvm_value_type_ptr);
                }

                emitter.getBuilder()->CreateStore(val, ptr);
            }

            ConcreteCompilerType*& t = exit->entry->args[p.first];
            if (t == NULL)
                t = var->getType();
            else
                ASSERT(t == var->getType(), "%s %s\n", t->debugName().c_str(), var->getType()->debugName().c_str());
        }

        if (sorted_symbol_table.size() > 3) {
            llvm_args.push_back(arg_array);
            llvm_arg_types.push_back(arg_array->getType());
        }

        llvm::FunctionType* ft
            = llvm::FunctionType::get(irstate->getReturnType()->llvmType(), llvm_arg_types, false /*vararg*/);
        partial_func = emitter.getBuilder()->CreateBitCast(partial_func, ft->getPointerTo());

        llvm::CallInst* rtn = emitter.getBuilder()->CreateCall(partial_func, llvm_args);

        // If we alloca'd the arg array, we can't make this into a tail call:
        if (arg_array == NULL && malloc_save != NULL) {
            rtn->setTailCall(true);
        }

        if (malloc_save != NULL) {
            llvm::Value* l_free = embedConstantPtr(
                (void*)free, llvm::FunctionType::get(g.void_, g.i8->getPointerTo(), false)->getPointerTo());
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

    void doJump(AST_Jump* node, ExcInfo exc_info) {
        assert(state != PARTIAL);

        endBlock(FINISHED);

        llvm::BasicBlock* target = entry_blocks[node->target];

        if (ENABLE_OSR && node->target->idx < myblock->idx && irstate->getEffortLevel() < EffortLevel::MAXIMAL) {
            assert(node->target->predecessors.size() > 1);
            doOSRExit(target, node);
        } else {
            emitter.getBuilder()->CreateBr(target);
        }
    }

    void doRaise(AST_Raise* node, ExcInfo exc_info) {
        std::vector<llvm::Value*> args;
        llvm::Value* raise_func = g.funcs.raise0;

        if (node->arg0) {
            raise_func = g.funcs.raise1;

            CompilerVariable* arg0 = evalExpr(node->arg0, exc_info);
            ConcreteCompilerVariable* converted_arg0 = arg0->makeConverted(emitter, arg0->getBoxType());
            arg0->decvref(emitter);
            args.push_back(converted_arg0->getValue());
        }
        RELEASE_ASSERT(node->arg1 == NULL, "");
        RELEASE_ASSERT(node->arg2 == NULL, "");

        emitter.createCall(exc_info, raise_func, args);
        emitter.getBuilder()->CreateUnreachable();

        endBlock(DEAD);
    }

    void doStmt(AST* node, ExcInfo exc_info) {
        // printf("%d stmt: %d\n", node->type, node->lineno);
        if (node->lineno) {
            emitter.getBuilder()->SetCurrentDebugLocation(
                llvm::DebugLoc::get(node->lineno, 0, irstate->getFuncDbgInfo()));
        }

        switch (node->type) {
            case AST_TYPE::Assert:
                doAssert(ast_cast<AST_Assert>(node), exc_info);
                break;
            case AST_TYPE::Assign:
                doAssign(ast_cast<AST_Assign>(node), exc_info);
                break;
            case AST_TYPE::ClassDef:
                doClassDef(ast_cast<AST_ClassDef>(node), exc_info);
                break;
            case AST_TYPE::Delete:
                doDelete(ast_cast<AST_Delete>(node), exc_info);
                break;
            case AST_TYPE::Expr:
                doExpr(ast_cast<AST_Expr>(node), exc_info);
                break;
            case AST_TYPE::FunctionDef:
                doFunctionDef(ast_cast<AST_FunctionDef>(node), exc_info);
                break;
            // case AST_TYPE::If:
            // doIf(ast_cast<AST_If>(node));
            // break;
            case AST_TYPE::Import:
                doImport(ast_cast<AST_Import>(node), exc_info);
                break;
            case AST_TYPE::ImportFrom:
                doImportFrom(ast_cast<AST_ImportFrom>(node), exc_info);
                break;
            case AST_TYPE::Global:
                // Should have been handled already
                break;
            case AST_TYPE::Pass:
                break;
            case AST_TYPE::Print:
                doPrint(ast_cast<AST_Print>(node), exc_info);
                break;
            case AST_TYPE::Return:
                assert(!exc_info.needsInvoke());
                doReturn(ast_cast<AST_Return>(node), exc_info);
                break;
            case AST_TYPE::Branch:
                assert(!exc_info.needsInvoke());
                doBranch(ast_cast<AST_Branch>(node), exc_info);
                break;
            case AST_TYPE::Jump:
                assert(!exc_info.needsInvoke());
                doJump(ast_cast<AST_Jump>(node), exc_info);
                break;
            case AST_TYPE::Invoke: {
                assert(!exc_info.needsInvoke());
                AST_Invoke* invoke = ast_cast<AST_Invoke>(node);
                doStmt(invoke->stmt, ExcInfo(entry_blocks[invoke->exc_dest]));

                assert(state == RUNNING || state == DEAD);
                if (state == RUNNING) {
                    emitter.getBuilder()->CreateBr(entry_blocks[invoke->normal_dest]);
                    endBlock(FINISHED);
                }

                break;
            }
            case AST_TYPE::Raise:
                doRaise(ast_cast<AST_Raise>(node), exc_info);
                break;
            case AST_TYPE::Unreachable:
                emitter.getBuilder()->CreateUnreachable();
                endBlock(FINISHED);
                break;
            default:
                printf("Unhandled stmt type at " __FILE__ ":" STRINGIFY(__LINE__) ": %d\n", node->type);
                exit(1);
        }
    }

    template <typename T> void loadArgument(const T& name, ConcreteCompilerType* t, llvm::Value* v, ExcInfo exc_info) {
        ConcreteCompilerVariable* var = unboxVar(t, v, false);
        _doSet(name, var, exc_info);
        var->decvref(emitter);
    }

    bool allowableFakeEndingSymbol(const std::string& name) {
        return startswith(name, "!is_defined") || name == PASSED_CLOSURE_NAME || name == CREATED_CLOSURE_NAME;
    }

    void endBlock(State new_state) {
        assert(state == RUNNING);

        // cf->func->dump();

        SourceInfo* source = irstate->getSourceInfo();
        ScopeInfo* scope_info = irstate->getScopeInfo();

        for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end();) {
            if (allowableFakeEndingSymbol(it->first)) {
                ++it;
                continue;
            }

            // ASSERT(it->first[0] != '!' || startswith(it->first, "!is_defined"), "left a fake variable in the real
            // symbol table? '%s'", it->first.c_str());

            if (!source->liveness->isLiveAtEnd(it->first, myblock)) {
                // printf("%s dead at end of %d; grabbed = %d, %d vrefs\n", it->first.c_str(), myblock->idx,
                // it->second->isGrabbed(), it->second->getVrefs());
                it->second->decvref(emitter);
                it = symbol_table.erase(it);
            } else if (source->phis->isRequiredAfter(it->first, myblock)) {
                assert(!scope_info->refersToGlobal(it->first));
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(it->first, myblock);
                // printf("Converting %s from %s to %s\n", it->first.c_str(),
                // it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                // printf("have to convert %s from %s to %s\n", it->first.c_str(),
                // it->second->getType()->debugName().c_str(), phi_type->debugName().c_str());
                ConcreteCompilerVariable* v = it->second->makeConverted(emitter, phi_type);
                it->second->decvref(emitter);
                symbol_table[it->first] = v->split(emitter);
                ++it;
            } else {
#ifndef NDEBUG
                if (myblock->successors.size()) {
                    // TODO getTypeAtBlockEnd will automatically convert up to the concrete type, which we don't want
                    // here, but this is just for debugging so I guess let it happen for now:
                    ConcreteCompilerType* ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                    ASSERT(it->second->canConvertTo(ending_type), "%s is supposed to be %s, but somehow is %s",
                           it->first.c_str(), ending_type->debugName().c_str(),
                           it->second->getType()->debugName().c_str());
                }
#endif

                ++it;
            }
        }

        const PhiAnalysis::RequiredSet& all_phis = source->phis->getAllRequiredAfter(myblock);
        for (PhiAnalysis::RequiredSet::const_iterator it = all_phis.begin(), end = all_phis.end(); it != end; ++it) {
            // printf("phi will be required for %s\n", it->c_str());
            assert(!scope_info->refersToGlobal(*it));
            CompilerVariable*& cur = symbol_table[*it];

            std::string defined_name = _getFakeName("is_defined", it->c_str());

            if (cur != NULL) {
                // printf("defined on this path; ");

                ConcreteCompilerVariable* is_defined
                    = static_cast<ConcreteCompilerVariable*>(_popFake(defined_name, true));

                if (source->phis->isPotentiallyUndefinedAfter(*it, myblock)) {
                    // printf("is potentially undefined later, so marking it defined\n");
                    if (is_defined) {
                        _setFake(defined_name, is_defined);
                    } else {
                        _setFake(defined_name, new ConcreteCompilerVariable(BOOL, getConstantInt(1, g.i1), true));
                    }
                } else {
                    // printf("is defined in all later paths, so not marking\n");
                    assert(!is_defined);
                }
            } else {
                // printf("no st entry, setting undefined\n");
                ConcreteCompilerType* phi_type = types->getTypeAtBlockEnd(*it, myblock);
                cur = new ConcreteCompilerVariable(phi_type, llvm::UndefValue::get(phi_type->llvmType()), true);
                _setFake(defined_name, new ConcreteCompilerVariable(BOOL, getConstantInt(0, g.i1), true));
            }
        }

        state = new_state;
    }

public:
    EndingState getEndingSymbolTable() override {
        assert(state == FINISHED || state == DEAD);

        // for (SymbolTable::iterator it = symbol_table.begin(); it != symbol_table.end(); ++it) {
        // printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
        //}

        SourceInfo* source = irstate->getSourceInfo();

        SymbolTable* st = new SymbolTable(symbol_table);
        ConcreteSymbolTable* phi_st = new ConcreteSymbolTable();

        if (myblock->successors.size() == 0) {
            for (auto& p : *st) {
                p.second->decvref(emitter);
            }
            st->clear();
            symbol_table.clear();

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
            if (allowableFakeEndingSymbol(it->first) || source->phis->isRequiredAfter(it->first, myblock)) {
                ASSERT(it->second->isGrabbed(), "%s", it->first.c_str());
                assert(it->second->getVrefs() == 1);
                // this conversion should have already happened... should refactor this.
                ConcreteCompilerType* ending_type;
                if (startswith(it->first, "!is_defined")) {
                    assert(it->second->getType() == BOOL);
                    ending_type = BOOL;
                } else if (it->first == PASSED_CLOSURE_NAME) {
                    ending_type = getPassedClosureType();
                } else if (it->first == CREATED_CLOSURE_NAME) {
                    ending_type = getCreatedClosureType();
                } else {
                    ending_type = types->getTypeAtBlockEnd(it->first, myblock);
                }
                //(*phi_st)[it->first] = it->second->makeConverted(emitter, it->second->getConcreteType());
                // printf("%s %p %d\n", it->first.c_str(), it->second, it->second->getVrefs());
                (*phi_st)[it->first] = it->second->split(emitter)->makeConverted(emitter, ending_type);
                it = st->erase(it);
            } else {
                ++it;
            }
        }
        return EndingState(st, phi_st, curblock);
    }

    void giveLocalSymbol(const std::string& name, CompilerVariable* var) override {
        assert(name != "None");
        ASSERT(!irstate->getScopeInfo()->refersToGlobal(name), "%s", name.c_str());
        assert(var->getType() != BOXED_INT);
        assert(var->getType() != BOXED_FLOAT);
        CompilerVariable*& cur = symbol_table[name];
        assert(cur == NULL);
        cur = var;
    }

    void copySymbolsFrom(SymbolTable* st) override {
        assert(st);
        DupCache cache;
        for (SymbolTable::iterator it = st->begin(); it != st->end(); it++) {
            // printf("Copying in %s: %p, %d\n", it->first.c_str(), it->second, it->second->getVrefs());
            symbol_table[it->first] = it->second->dup(cache);
            // printf("got: %p, %d\n", symbol_table[it->first], symbol_table[it->first]->getVrefs());
        }
    }

    ConcreteCompilerType* getPassedClosureType() {
        // TODO could know the exact closure shape
        return CLOSURE;
    }

    ConcreteCompilerType* getCreatedClosureType() {
        // TODO could know the exact closure shape
        return CLOSURE;
    }

    void doFunctionEntry(const SourceInfo::ArgNames& arg_names,
                         const std::vector<ConcreteCompilerType*>& arg_types) override {
        assert(arg_names.totalParameters() == arg_types.size());

        auto scope_info = irstate->getScopeInfo();

        llvm::Value* passed_closure = NULL;
        llvm::Function::arg_iterator AI = irstate->getLLVMFunction()->arg_begin();
        if (scope_info->takesClosure()) {
            passed_closure = AI;
            _setFake(PASSED_CLOSURE_NAME, new ConcreteCompilerVariable(getPassedClosureType(), AI, true));
            ++AI;
        }

        if (scope_info->createsClosure()) {
            if (!passed_closure)
                passed_closure = embedConstantPtr(nullptr, g.llvm_closure_type_ptr);

            llvm::Value* new_closure = emitter.getBuilder()->CreateCall(g.funcs.createClosure, passed_closure);
            _setFake(CREATED_CLOSURE_NAME, new ConcreteCompilerVariable(getCreatedClosureType(), new_closure, true));
        }


        std::vector<llvm::Value*> python_parameters;
        for (int i = 0; i < arg_types.size(); i++) {
            assert(AI != irstate->getLLVMFunction()->arg_end());

            if (i == 3) {
                for (int i = 3; i < arg_types.size(); i++) {
                    llvm::Value* ptr = emitter.getBuilder()->CreateConstGEP1_32(AI, i - 3);
                    llvm::Value* loaded = emitter.getBuilder()->CreateLoad(ptr);

                    if (arg_types[i]->llvmType() == g.i64)
                        loaded = emitter.getBuilder()->CreatePtrToInt(loaded, arg_types[i]->llvmType());
                    else
                        assert(arg_types[i]->llvmType() == g.llvm_value_type_ptr);

                    python_parameters.push_back(loaded);
                }
                ++AI;
                break;
            }

            python_parameters.push_back(AI);
            ++AI;
        }

        assert(AI == irstate->getLLVMFunction()->arg_end());
        assert(python_parameters.size() == arg_names.totalParameters());

        if (arg_names.args) {
            int i = 0;
            for (; i < arg_names.args->size(); i++) {
                loadArgument((*arg_names.args)[i], arg_types[i], python_parameters[i], ExcInfo::none());
            }

            if (arg_names.vararg->size()) {
                loadArgument(*arg_names.vararg, arg_types[i], python_parameters[i], ExcInfo::none());
                i++;
            }

            if (arg_names.kwarg->size()) {
                loadArgument(*arg_names.kwarg, arg_types[i], python_parameters[i], ExcInfo::none());
                i++;
            }

            assert(i == arg_types.size());
        }
    }

    void run(const CFGBlock* block) override {
        ExcInfo exc_info = ExcInfo::none();
        for (int i = 0; i < block->body.size(); i++) {
            if (state == DEAD)
                break;
            assert(state != FINISHED);
            doStmt(block->body[i], exc_info);
        }
    }

    void doSafePoint() override { emitter.getBuilder()->CreateCall(g.funcs.allowGLReadPreemption); }
};

IRGenerator* createIRGenerator(IRGenState* irstate, std::unordered_map<CFGBlock*, llvm::BasicBlock*>& entry_blocks,
                               CFGBlock* myblock, TypeAnalysis* types, GuardList& out_guards,
                               const GuardList& in_guards, bool is_partial) {
    return new IRGeneratorImpl(irstate, entry_blocks, myblock, types, out_guards, in_guards, is_partial);
}
}
