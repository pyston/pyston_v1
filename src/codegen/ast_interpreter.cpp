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

#include "codegen/ast_interpreter.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <unordered_map>

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/baseline_jit.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "core/bst.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/contiguous_map.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/inline/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

namespace pyston {

namespace {

class ASTInterpreter;
extern "C" Box* executeInnerAndSetupFrame(ASTInterpreter& interpreter, CFGBlock* start_block, BST_stmt* start_at);

/*
 * ASTInterpreters exist per function frame - there's no global interpreter object that executes
 * all non-jitted code!
 *
 * All ASTInterpreter instances have to live on the stack because otherwise the GC won't scan the fields.
 */
class ASTInterpreter {
public:
    ASTInterpreter(BoxedCode* code, Box** vregs, FrameInfo* deopt_frame_info = NULL);

    void initArguments(BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3, Box** args);

    static Box* execute(ASTInterpreter& interpreter, CFGBlock* start_block = NULL, BST_stmt* start_at = NULL);
    static Box* executeInner(ASTInterpreter& interpreter, CFGBlock* start_block, BST_stmt* start_at);

private:
    Value createFunction(BST_FunctionDef* node, BoxedCode* node_code);
    Value doBinOp(BST_stmt* node, Value left, Value right, int op, BinExpType exp_type);
    void doStore(int vreg, STOLEN(Value) value);
    void doStoreArg(BST_Name* name, STOLEN(Value) value);
    Box* doOSR(BST_Jump* node);
    Value getNone();

    Value getVReg(int vreg, bool kill = true);

    Value visit_augBinOp(BST_AugBinOp* node);
    Value visit_binop(BST_BinOp* node);
    Value visit_call(BST_Call* node);
    Value visit_checkexcmatch(BST_CheckExcMatch* node);
    Value visit_compare(BST_Compare* node);
    Value visit_copyvreg(BST_CopyVReg* node);
    Value visit_dict(BST_Dict* node);
    Value visit_getiter(BST_GetIter* node);
    Value visit_hasnext(BST_HasNext* node);
    Value visit_importfrom(BST_ImportFrom* node);
    Value visit_importname(BST_ImportName* node);
    Value visit_importstar(BST_ImportStar* node);
    Value visit_invoke(BST_Invoke* node);
    Value visit_jump(BST_Jump* node);
    Value visit_landingpad(BST_Landingpad* node);
    Value visit_list(BST_List* node);
    Value visit_loadattr(BST_LoadAttr* node);
    Value visit_loadname(BST_LoadName* node);
    Value visit_loadsub(BST_LoadSub* node);
    Value visit_loadsubslice(BST_LoadSubSlice* node);
    Value visit_locals(BST_Locals* node);
    Value visit_makeClass(BST_MakeClass* node);
    Value visit_makeFunction(BST_MakeFunction* node);
    Value visit_makeslice(BST_MakeSlice* node);
    Value visit_nonzero(BST_Nonzero* node);
    Value visit_repr(BST_Repr* node);
    Value visit_return(BST_Return* node);
    Value visit_set(BST_Set* node);
    Value visit_stmt(BST_stmt* node);
    Value visit_tuple(BST_Tuple* node);
    Value visit_unaryop(BST_UnaryOp* node);
    Value visit_yield(BST_Yield* node);
    void visit_assert(BST_Assert* node);
    void visit_branch(BST_Branch* node);
    void visit_deleteattr(BST_DeleteAttr* node);
    void visit_deletename(BST_DeleteName* node);
    void visit_deletesub(BST_DeleteSub* node);
    void visit_deletesubslice(BST_DeleteSubSlice* node);
    void visit_exec(BST_Exec* node);
    void visit_print(BST_Print* node);
    void visit_printexpr(BST_PrintExpr* node);
    void visit_raise(BST_Raise* node);
    void visit_setexcinfo(BST_SetExcInfo* node);
    void visit_storeattr(BST_StoreAttr* node);
    void visit_storename(BST_StoreName* node);
    void visit_storesub(BST_StoreSub* node);
    void visit_storesubslice(BST_StoreSubSlice* node);
    void visit_uncacheexcinfo(BST_UncacheExcInfo* node);
    void visit_unpackintoarray(BST_UnpackIntoArray* node);

    // for doc on 'exit_offset' have a look at JitFragmentWriter::num_bytes_exit and num_bytes_overlapping
    void startJITing(CFGBlock* block, int exit_offset = 0,
                     llvm::DenseSet<int> known_non_null_vregs = llvm::DenseSet<int>());
    void abortJITing();
    void finishJITing(CFGBlock* continue_block = NULL);
    Box* execJITedBlock(CFGBlock* b);

    // this variables are used by the baseline JIT, make sure they have an offset < 0x80 so we can use shorter
    // instructions
    CFGBlock* next_block, *current_block;
    FrameInfo frame_info;
    unsigned edgecount;

    SourceInfo* source_info;
    const ScopingResults& scope_info;
    PhiAnalysis* phis;
    Box** vregs;
    ExcInfo last_exception;
    BoxedClosure* created_closure;
    BoxedGenerator* generator;
    BoxedModule* parent_module;

    std::unique_ptr<JitFragmentWriter> jit;
    bool should_jit;

public:
    ~ASTInterpreter() { Py_XDECREF(this->created_closure); }

    const VRegInfo& getVRegInfo() const { return source_info->cfg->getVRegInfo(); }

#ifndef NDEBUG
    const llvm::DenseMap<InternedString, DefaultedInt<VREG_UNDEFINED>>& getSymVRegMap() const {
        return getVRegInfo().getSymVRegMap();
    }
#endif

    BST_stmt* getCurrentStatement() {
        assert(frame_info.stmt);
        return frame_info.stmt;
    }

    void setCurrentStatement(BST_stmt* stmt) { frame_info.stmt = stmt; }

    BoxedCode* getCode() { return frame_info.code; }
    FrameInfo* getFrameInfo() { return &frame_info; }
    BoxedClosure* getPassedClosure() { return frame_info.passed_closure; }
    Box** getVRegs() { return vregs; }
    const ScopingResults& getScopeInfo() { return scope_info; }
    const CodeConstants& getCodeConstants() { return getCode()->code_constants; }
    LivenessAnalysis* getLiveness() { return source_info->getLiveness(getCodeConstants()); }

    void addSymbol(int vreg, Box* value, bool allow_duplicates);
    void setGenerator(Box* gen);
    void setPassedClosure(Box* closure);
    void setCreatedClosure(Box* closure);
    void setBoxedLocals(STOLEN(Box*));
    void setGlobals(Box* globals);

    friend struct pyston::ASTInterpreterJitInterface;
};

void ASTInterpreter::addSymbol(int vreg, Box* new_value, bool allow_duplicates) {
    Box*& value = vregs[vreg];
    Box* old_value = value;
    value = incref(new_value);
    if (allow_duplicates)
        Py_XDECREF(old_value);
    else
        assert(old_value == NULL);
}

void ASTInterpreter::setGenerator(Box* gen) {
    assert(!this->generator); // This should only used for initialization
    assert(gen->cls == generator_cls);
    this->generator = static_cast<BoxedGenerator*>(gen);
}

void ASTInterpreter::setPassedClosure(Box* closure) {
    assert(!frame_info.passed_closure); // This should only used for initialization
    assert(!closure || closure->cls == closure_cls);
    frame_info.passed_closure = static_cast<BoxedClosure*>(closure);
}

void ASTInterpreter::setCreatedClosure(Box* closure) {
    assert(!this->created_closure); // This should only used for initialization
    assert(closure->cls == closure_cls);
    // we have to incref the closure because the interpreter destructor will decref it
    this->created_closure = static_cast<BoxedClosure*>(incref(closure));
}

void ASTInterpreter::setBoxedLocals(Box* boxedLocals) {
    assert(!this->frame_info.boxedLocals);
    this->frame_info.boxedLocals = boxedLocals;
}

void ASTInterpreter::setGlobals(Box* globals) {
    assert(!this->frame_info.globals);
    this->frame_info.globals = incref(globals);
}

ASTInterpreter::ASTInterpreter(BoxedCode* code, Box** vregs, FrameInfo* deopt_frame_info)
    : current_block(0),
      frame_info(ExcInfo(NULL, NULL, NULL)),
      edgecount(0),
      source_info(code->source.get()),
      scope_info(source_info->scoping),
      phis(NULL),
      vregs(vregs),
      last_exception(NULL, NULL, NULL),
      created_closure(0),
      generator(0),
      parent_module(source_info->parent_module),
      should_jit(false) {

    if (deopt_frame_info) {
        // copy over all fields and clear the deopt frame info
        frame_info = *deopt_frame_info;

        // Well, don't actually copy over the vregs.  We'll deal with them separately
        // (using the locals dict), so just clear out and decref the old ones:
        frame_info.vregs = NULL;
        frame_info.num_vregs = 0;
        for (int i = 0; i < deopt_frame_info->num_vregs; ++i)
            Py_XDECREF(deopt_frame_info->vregs[i]);

        // We are taking responsibility for calling deinit:
        deopt_frame_info->disableDeinit(&this->frame_info);
    }

    frame_info.vregs = vregs;
    frame_info.code = code;
    frame_info.num_vregs = getVRegInfo().getNumOfCrossBlockVRegs();
}

void ASTInterpreter::initArguments(BoxedClosure* _closure, BoxedGenerator* _generator, Box* arg1, Box* arg2, Box* arg3,
                                   Box** args) {
    setPassedClosure(_closure);
    generator = _generator;

    if (scope_info.createsClosure())
        created_closure = createClosure(_closure, scope_info.getClosureSize());

    const ParamNames& param_names = getCode()->param_names;

    int i = 0;
    for (auto& name : param_names.argsAsName()) {
        doStoreArg(name, Value(incref(getArg(i++, arg1, arg2, arg3, args)), 0));
    }

    if (param_names.has_vararg_name)
        doStoreArg(param_names.varArgAsName(), Value(incref(getArg(i++, arg1, arg2, arg3, args)), 0));

    if (param_names.has_kwarg_name) {
        Box* val = getArg(i++, arg1, arg2, arg3, args);
        if (!val)
            val = createDict();
        else
            Py_INCREF(val);
        doStoreArg(param_names.kwArgAsName(), Value(val, 0));
    }
    assert(i == param_names.totalParameters());
}

void ASTInterpreter::startJITing(CFGBlock* block, int exit_offset, llvm::DenseSet<int> known_non_null_vregs) {
    assert(ENABLE_BASELINEJIT);
    assert(!jit);

    auto& code_blocks = getCode()->code_blocks;
    JitCodeBlock* code_block = NULL;
    if (!code_blocks.empty())
        code_block = code_blocks[code_blocks.size() - 1].get();

    if (!code_block || code_block->shouldCreateNewBlock()) {
        code_blocks.push_back(llvm::make_unique<JitCodeBlock>(getCode(), getCode()->name->s()));
        code_block = code_blocks[code_blocks.size() - 1].get();
        exit_offset = 0;
    }

    // small optimization: we know that the passed arguments in the entry block are non zero
    if (block == block->cfg->getStartingBlock() && block->predecessors.empty()) {
        auto param_names = getCode()->param_names;
        for (auto&& arg : param_names.allArgsAsName()) {
            if (arg->vreg >= 0)
                known_non_null_vregs.insert(arg->vreg);
        }
    }
    jit = code_block->newFragment(block, exit_offset, std::move(known_non_null_vregs));
}

void ASTInterpreter::abortJITing() {
    if (jit) {
        static StatCounter bjit_aborts("num_baselinejit_aborts");
        bjit_aborts.log();
        jit->abortCompilation();
        jit.reset();
    }
}

void ASTInterpreter::finishJITing(CFGBlock* continue_block) {
    if (!jit)
        return;

    int exit_offset = 0;
    llvm::DenseSet<int> known_non_null;
    std::tie(exit_offset, known_non_null) = jit->finishCompilation();
    jit.reset();
    if (continue_block && !continue_block->code) {
        // check if we can reuse the known non null vreg set
        if (continue_block->predecessors.size() == 1)
            assert(current_block == continue_block->predecessors[0]);
        else
            known_non_null.clear();

        startJITing(continue_block, exit_offset, std::move(known_non_null));
    }
}

Box* ASTInterpreter::execJITedBlock(CFGBlock* b) {
    auto& num_inside = getCode()->bjit_num_inside;
    try {
        UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_baseline_jitted_code");
        ++num_inside;
        std::pair<CFGBlock*, Box*> rtn = b->entry_code(this, b, vregs);
        --num_inside;
        next_block = rtn.first;
        return rtn.second;
    } catch (ExcInfo e) {
        --num_inside;
        BST_stmt* stmt = getCurrentStatement();
        if (stmt->type != BST_TYPE::Invoke)
            throw e;

        assert(getPythonFrameInfo(0) == getFrameInfo());

        ++getCode()->cxx_exception_count[stmt];
        caughtCxxException(&e);

        next_block = ((BST_Invoke*)stmt)->exc_dest;
        last_exception = e;
    }
    return nullptr;
}

Box* ASTInterpreter::executeInner(ASTInterpreter& interpreter, CFGBlock* start_block, BST_stmt* start_at) {
    Value v(nullptr, nullptr);

    bool from_start = start_block == NULL && start_at == NULL;

    assert((start_block == NULL) == (start_at == NULL));
    if (start_block == NULL) {
        start_block = interpreter.source_info->cfg->getStartingBlock();
        start_at = start_block->body[0];
    }

    // Important that this happens after RegisterHelper:
    interpreter.setCurrentStatement(start_at);
    threading::allowGLReadPreemption();
    interpreter.setCurrentStatement(NULL);

    if (!from_start) {
        interpreter.current_block = start_block;
        bool started = false;
        for (auto s : start_block->body) {
            if (!started) {
                if (s != start_at)
                    continue;
                started = true;
            }

            interpreter.setCurrentStatement(s);
            Py_XDECREF(v.o);
            v = interpreter.visit_stmt(s);
        }
    } else {
        interpreter.next_block = start_block;
    }

    if (ENABLE_BASELINEJIT && interpreter.getCode()->times_interpreted >= REOPT_THRESHOLD_INTERPRETER)
        interpreter.should_jit = true;

    while (interpreter.next_block) {
        interpreter.current_block = interpreter.next_block;
        interpreter.next_block = 0;

        if (ENABLE_BASELINEJIT && !interpreter.jit) {
            CFGBlock* b = interpreter.current_block;
            if (b->entry_code) {
                Box* rtn = interpreter.execJITedBlock(b);
                if (interpreter.next_block)
                    continue;

                // check if we returned from the baseline JIT because we should do a OSR.
                if (unlikely(rtn == (Box*)ASTInterpreterJitInterface::osr_dummy_value)) {
                    BST_Jump* cur_stmt = (BST_Jump*)interpreter.getCurrentStatement();
                    RELEASE_ASSERT(cur_stmt->type == BST_TYPE::Jump, "");
                    // WARNING: do not put a try catch + rethrow block around this code here.
                    //          it will confuse our unwinder!
                    rtn = interpreter.doOSR(cur_stmt);
                    Py_CLEAR(v.o);

                    // rtn == NULL when the OSR failed and we have to continue with interpreting
                    if (rtn)
                        return rtn;
                    // if we get here OSR failed, fallthrough to the interpreter loop
                } else {
                    Py_XDECREF(v.o);
                    return rtn;
                }
            }
        }

        if (ENABLE_BASELINEJIT && interpreter.should_jit && !interpreter.jit) {
            assert(!interpreter.current_block->code);
            interpreter.startJITing(interpreter.current_block);
        }

        for (BST_stmt* s : interpreter.current_block->body) {
            interpreter.setCurrentStatement(s);
            if (interpreter.jit)
                interpreter.jit->emitSetCurrentInst(s);
            if (v.o) {
                Py_DECREF(v.o);
            }
            v = interpreter.visit_stmt(s);
        }
    }
    return v.o;
}

Box* ASTInterpreter::execute(ASTInterpreter& interpreter, CFGBlock* start_block, BST_stmt* start_at) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_interpreter");
    RECURSIVE_BLOCK(CXX, " in function call");

    return executeInnerAndSetupFrame(interpreter, start_block, start_at);
}

Value ASTInterpreter::doBinOp(BST_stmt* node, Value left, Value right, int op, BinExpType exp_type) {
    switch (exp_type) {
        case BinExpType::AugBinOp:
            return Value(augbinop(left.o, right.o, op), jit ? jit->emitAugbinop(node, left, right, op) : NULL);
        case BinExpType::BinOp:
            return Value(binop(left.o, right.o, op), jit ? jit->emitBinop(node, left, right, op) : NULL);
        case BinExpType::Compare:
            return Value(compare(left.o, right.o, op), jit ? jit->emitCompare(node, left, right, op) : NULL);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
}

void ASTInterpreter::doStore(int vreg, STOLEN(Value) value) {
    if (vreg == VREG_UNDEFINED) {
        Py_DECREF(value.o);
        return;
    }
    if (jit) {
        bool is_live = getLiveness()->isLiveAtEnd(vreg, current_block);
        if (is_live)
            jit->emitSetLocal(vreg, value);
        else
            jit->emitSetBlockLocal(vreg, value);
    }

    frame_info.num_vregs = std::max(frame_info.num_vregs, vreg + 1);
    Box* prev = vregs[vreg];
    vregs[vreg] = value.o;
    Py_XDECREF(prev);
}

void ASTInterpreter::doStoreArg(BST_Name* node, STOLEN(Value) value) {
    assert(!jit);
    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);

    InternedString name = node->id;
    ScopeInfo::VarScopeType vst = node->lookup_type;
    if (vst == ScopeInfo::VarScopeType::NAME) {
        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        AUTO_DECREF(value.o);
        setitem(frame_info.boxedLocals, name.getBox(), value.o);
    } else if (vst == ScopeInfo::VarScopeType::FAST || vst == ScopeInfo::VarScopeType::CLOSURE) {
        bool closure = vst == ScopeInfo::VarScopeType::CLOSURE;
        if (closure) {
            ASTInterpreterJitInterface::setLocalClosureHelper(this, node->vreg, node->closure_offset, value.o);
        } else {
            assert(getVRegInfo().getVReg(node->id) == node->vreg);
            frame_info.num_vregs = std::max(frame_info.num_vregs, node->vreg + 1);
            Box* prev = vregs[node->vreg];
            vregs[node->vreg] = value.o;
            Py_XDECREF(prev);
        }
    } else {
        RELEASE_ASSERT(0, "");
    }
}

Value ASTInterpreter::getNone() {
    RewriterVar* v = NULL;
    if (jit) {
        v = jit->imm(Py_None)->setType(RefType::BORROWED);
    }
    return Value(incref(Py_None), v);
}

Value ASTInterpreter::visit_unaryop(BST_UnaryOp* node) {
    Value operand = getVReg(node->vreg_operand);
    AUTO_DECREF(operand.o);
    if (node->op_type == AST_TYPE::Not)
        return Value(boxBool(!nonzero(operand.o)), jit ? jit->emitNotNonzero(operand) : NULL);
    else
        return Value(unaryop(operand.o, node->op_type), jit ? jit->emitUnaryop(operand, node->op_type) : NULL);
}

void ASTInterpreter::visit_unpackintoarray(BST_UnpackIntoArray* node) {
    Value value = getVReg(node->vreg_src);
    AUTO_DECREF(value.o);

    Box* keep_alive;
    Box** array = unpackIntoArray(value.o, node->num_elts, &keep_alive);
    AUTO_DECREF(keep_alive);

    std::vector<RewriterVar*> array_vars;
    if (jit) {
        array_vars = jit->emitUnpackIntoArray(value, node->num_elts);
        assert(array_vars.size() == node->num_elts);
    }

    for (int i = 0; i < node->num_elts; ++i) {
        doStore(node->vreg_dst[i], Value(array[i], jit ? array_vars[i] : NULL));
    }
}

Value ASTInterpreter::visit_binop(BST_BinOp* node) {
    Value left = getVReg(node->vreg_left);
    AUTO_DECREF(left.o);
    Value right = getVReg(node->vreg_right);
    AUTO_DECREF(right.o);
    return doBinOp(node, left, right, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_makeslice(BST_MakeSlice* node) {
    Value lower = node->vreg_lower != VREG_UNDEFINED ? getVReg(node->vreg_lower) : getNone();
    AUTO_DECREF(lower.o);
    Value upper = node->vreg_upper != VREG_UNDEFINED ? getVReg(node->vreg_upper) : getNone();
    AUTO_DECREF(upper.o);
    Value step = node->vreg_step != VREG_UNDEFINED ? getVReg(node->vreg_step) : getNone();
    AUTO_DECREF(step.o);

    Value v;
    if (jit)
        v.var = jit->emitCreateSlice(lower, upper, step);
    v.o = createSlice(lower.o, upper.o, step.o);
    return v;
}

void ASTInterpreter::visit_branch(BST_Branch* node) {
    Value v = getVReg(node->vreg_test);
    ASSERT(v.o == Py_True || v.o == Py_False, "Should have called NONZERO before this branch");

    // TODO could potentially avoid doing this if we skip the incref in NONZERO
    AUTO_DECREF(v.o);

    if (jit) {
        // Special note: emitSideExit decrefs v for us.
        // TODO: since the value is always True or False, maybe could optimize by putting the decref
        // before the conditional instead of after.
        jit->emitSideExit(v, v.o, v.o == Py_True ? node->iffalse : node->iftrue);
    }

    if (v.o == Py_True)
        next_block = node->iftrue;
    else
        next_block = node->iffalse;

    if (jit) {
        jit->emitJump(next_block);
        finishJITing(next_block);
    }
}

Value ASTInterpreter::visit_jump(BST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx;
    if (backedge) {
        threading::allowGLReadPreemption();

        if (jit)
            jit->call(false, (void*)threading::allowGLReadPreemption);
    }

    if (jit) {
        if (backedge && ENABLE_OSR && !FORCE_INTERPRETER)
            jit->emitOSRPoint(node);
        jit->emitJump(node->target);
        finishJITing(node->target);

        // we may have started JITing because the OSR thresholds got triggered in this case we don't want to jit
        // additional blocks ouside of the loop if the function is cold.
        if (getCode()->times_interpreted < REOPT_THRESHOLD_INTERPRETER)
            should_jit = false;
    }

    if (backedge)
        ++edgecount;

    if (ENABLE_BASELINEJIT && backedge && edgecount >= OSR_THRESHOLD_INTERPRETER && !jit && !node->target->code) {
        should_jit = true;
        startJITing(node->target);
    }

    if (backedge && edgecount >= OSR_THRESHOLD_BASELINE) {
        Box* rtn = doOSR(node);
        if (rtn)
            return Value(rtn, NULL);
    }

    next_block = node->target;
    return Value();
}

Box* ASTInterpreter::doOSR(BST_Jump* node) {
    bool can_osr = ENABLE_OSR && !FORCE_INTERPRETER;
    if (!can_osr)
        return NULL;

    static StatCounter ast_osrs("num_ast_osrs");
    ast_osrs.log();

    LivenessAnalysis* liveness = getLiveness();
    std::unique_ptr<PhiAnalysis> phis = computeRequiredPhis(getCode()->param_names, source_info->cfg, liveness);

    llvm::SmallVector<int, 16> dead_vregs;

    for (int vreg = 0; vreg < getVRegInfo().getTotalNumOfVRegs(); ++vreg) {
        if (!liveness->isLiveAtEnd(vreg, current_block)) {
            dead_vregs.push_back(vreg);
        }
    }
    for (auto&& vreg_num : dead_vregs) {
        Py_CLEAR(vregs[vreg_num]);
    }

    const OSREntryDescriptor* found_entry = nullptr;
    for (auto& p : getCode()->osr_versions) {
        if (p.first->backedge != node)
            continue;

        found_entry = p.first;
    }

    int num_vregs = source_info->cfg->getVRegInfo().getTotalNumOfVRegs();
    VRegMap<Box*> sorted_symbol_table(num_vregs);
    VRegSet potentially_undefined(num_vregs);

    // TODO: maybe use a different placeholder (=NULL)?
    // - new issue with that -- we can no longer distinguish NULL from unset-in-sorted_symbol_table
    // Currently we pass None because the LLVM jit will decref this value even though it may not be set.
    static Box* const VAL_UNDEFINED = (Box*)Py_None;

    const VRegSet& defined = phis->definedness.getDefinedVregsAtEnd(current_block);
    for (int vreg : defined) {

        if (!liveness->isLiveAtEnd(vreg, current_block))
            continue;

        Box* val = vregs[vreg];
        if (phis->isPotentiallyUndefinedAfter(vreg, current_block)) {
            potentially_undefined.set(vreg);
            bool is_defined = val != NULL;
            sorted_symbol_table[vreg] = is_defined ? incref(val) : incref(VAL_UNDEFINED);
        } else {
            assert(val != NULL);
            sorted_symbol_table[vreg] = incref(val);
        }
    }

    // Manually free these here, since we might not return from this scope for a long time.
    phis.reset(nullptr);

    // LLVM has a limit on the number of operands a machine instruction can have (~255),
    // in order to not hit the limit with the patchpoints cancel OSR when we have a high number of symbols.
    if (sorted_symbol_table.numSet() > 225) {
        static StatCounter times_osr_cancel("num_osr_cancel_too_many_syms");
        times_osr_cancel.log();
        return nullptr;
    }

    if (found_entry == nullptr) {
        OSREntryDescriptor* entry = OSREntryDescriptor::create(getCode(), node, CXX);

        // TODO can we just get rid of this?
        for (auto&& p : sorted_symbol_table) {
            entry->args[p.first] = UNKNOWN;
        }

        entry->potentially_undefined = potentially_undefined;

        found_entry = entry;
    }

    OSRExit exit(found_entry);

    std::vector<Box*> arg_array;
    arg_array.reserve(sorted_symbol_table.numSet() + potentially_undefined.numSet());
    for (auto&& p : sorted_symbol_table) {
        arg_array.push_back(p.second);
    }
    for (int vreg : potentially_undefined) {
        bool is_defined = sorted_symbol_table[vreg] != VAL_UNDEFINED;
        arg_array.push_back((Box*)is_defined);
    }

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_jitted_code");
    CompiledFunction* partial_func = compilePartialFuncInternal(&exit);

    // generated is only borrowed in order to not introduce cycles
    Box* r = partial_func->call_osr(generator, created_closure, &frame_info, &arg_array[0]);

    if (partial_func->exception_style == CXX) {
        assert(r);
        return r;
    } else {
        if (!r)
            throwCAPIException();
        return r;
    }
}

Value ASTInterpreter::visit_invoke(BST_Invoke* node) {
    Value v;
    try {
        v = visit_stmt(node->stmt);
        next_block = node->normal_dest;

        if (jit) {
            jit->emitJump(next_block);
            finishJITing(next_block);
        }
    } catch (ExcInfo e) {
        assert(node == getCurrentStatement());
        abortJITing();

        assert(getPythonFrameInfo(0) == getFrameInfo());

        ++getCode()->cxx_exception_count[node];
        caughtCxxException(&e);

        next_block = node->exc_dest;
        last_exception = e;
    }

    return v;
}

Value ASTInterpreter::visit_augBinOp(BST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = getVReg(node->vreg_left);
    AUTO_DECREF(left.o);
    Value right = getVReg(node->vreg_right);
    AUTO_DECREF(right.o);
    return doBinOp(node, left, right, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_landingpad(BST_Landingpad* node) {
    assert(last_exception.type);
    return Value(ASTInterpreterJitInterface::landingpadHelper(this), jit ? jit->emitLandingpad() : NULL);
}

Value ASTInterpreter::visit_locals(BST_Locals* node) {
    assert(frame_info.boxedLocals != NULL);
    return Value(incref(frame_info.boxedLocals), jit ? jit->emitGetBoxedLocals() : NULL);
}

Value ASTInterpreter::visit_getiter(BST_GetIter* node) {
    Value val = getVReg(node->vreg_value);
    AUTO_DECREF(val.o);
    return Value(getPystonIter(val.o), jit ? jit->emitGetPystonIter(val) : NULL);
}

Value ASTInterpreter::visit_importfrom(BST_ImportFrom* node) {
    Value module = getVReg(node->vreg_module);
    AUTO_DECREF(module.o);

    Value name_boxed = getVReg(node->vreg_name);
    AUTO_DECREF(name_boxed.o);

    Value v;
    if (jit)
        v.var = jit->emitImportFrom(module, name_boxed);
    v.o = importFrom(module.o, (BoxedString*)name_boxed.o);
    return v;
}
Value ASTInterpreter::visit_importname(BST_ImportName* node) {
    int level = node->level;
    Value froms = getVReg(node->vreg_from);
    AUTO_DECREF(froms.o);
    Value module_name = getVReg(node->vreg_name);
    AUTO_DECREF(module_name.o);
    Value v;
    if (jit)
        v.var = jit->emitImportName(level, froms, module_name);
    v.o = import(level, froms.o, (BoxedString*)module_name.o);
    return v;
}

Value ASTInterpreter::visit_importstar(BST_ImportStar* node) {
    Value module = getVReg(node->vreg_name);
    AUTO_DECREF(module.o);
    return Value(importStar(module.o, frame_info.globals), jit ? jit->emitImportStar(module) : NULL);
}

Value ASTInterpreter::visit_nonzero(BST_Nonzero* node) {
    Value obj = getVReg(node->vreg_value);
    AUTO_DECREF(obj.o);
    return Value(boxBool(nonzero(obj.o)), jit ? jit->emitNonzero(obj) : NULL);
}

Value ASTInterpreter::visit_checkexcmatch(BST_CheckExcMatch* node) {
    Value obj = getVReg(node->vreg_value);
    AUTO_DECREF(obj.o);
    Value cls = getVReg(node->vreg_cls);
    AUTO_DECREF(cls.o);
    return Value(boxBool(exceptionMatches(obj.o, cls.o)), jit ? jit->emitExceptionMatches(obj, cls) : NULL);
}

void ASTInterpreter::visit_setexcinfo(BST_SetExcInfo* node) {
    Value type = getVReg(node->vreg_type);
    assert(type.o);
    Value value = getVReg(node->vreg_value);
    assert(value.o);
    Value traceback = getVReg(node->vreg_traceback);
    assert(traceback.o);

    if (jit)
        jit->emitSetExcInfo(type, value, traceback);
    setFrameExcInfo(getFrameInfo(), type.o, value.o, traceback.o);
}

void ASTInterpreter::visit_uncacheexcinfo(BST_UncacheExcInfo* node) {
    if (jit)
        jit->emitUncacheExcInfo();
    setFrameExcInfo(getFrameInfo(), NULL, NULL, NULL);
}

Value ASTInterpreter::visit_hasnext(BST_HasNext* node) {
    Value obj = getVReg(node->vreg_value);
    AUTO_DECREF(obj.o);
    return Value(boxBool(hasnext(obj.o)), jit ? jit->emitHasnext(obj) : NULL);
}

void ASTInterpreter::visit_printexpr(BST_PrintExpr* node) {
    abortJITing();
    Value obj = getVReg(node->vreg_value);
    AUTO_DECREF(obj.o);
    printExprHelper(obj.o);
}

Value ASTInterpreter::visit_yield(BST_Yield* node) {
    Value value = node->vreg_value != VREG_UNDEFINED ? getVReg(node->vreg_value) : getNone();
    return Value(ASTInterpreterJitInterface::yieldHelper(this, value.o), jit ? jit->emitYield(value) : NULL);
}

Value ASTInterpreter::visit_stmt(BST_stmt* node) {
#if ENABLE_SAMPLING_PROFILER
    threading::allowGLReadPreemption();
#endif

    if (0) {
        printf("%20s % 2d ", getCode()->name->c_str(), current_block->idx);
        print_bst(node, getCodeConstants());
        printf("\n");
    }

    // Any statement that returns a value needs
    // to be careful to wrap pendingCallsCheckHelper, and it can signal that it was careful
    // by returning from the function instead of breaking.

    switch (node->type) {
        case BST_TYPE::Assert:
            visit_assert((BST_Assert*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::DeleteAttr:
            visit_deleteattr((BST_DeleteAttr*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::DeleteSub:
            visit_deletesub((BST_DeleteSub*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::DeleteSubSlice:
            visit_deletesubslice((BST_DeleteSubSlice*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::DeleteName:
            visit_deletename((BST_DeleteName*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Exec:
            visit_exec((BST_Exec*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Print:
            visit_print((BST_Print*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Raise:
            visit_raise((BST_Raise*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Return: {
            Value rtn = visit_return((BST_Return*)node);
            try {
                ASTInterpreterJitInterface::pendingCallsCheckHelper();
            } catch (ExcInfo e) {
                Py_DECREF(rtn.o);
                throw e;
            }
            return rtn;
        }
        case BST_TYPE::StoreName:
            visit_storename((BST_StoreName*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::StoreAttr:
            visit_storeattr((BST_StoreAttr*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::StoreSub:
            visit_storesub((BST_StoreSub*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::StoreSubSlice:
            visit_storesubslice((BST_StoreSubSlice*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::UnpackIntoArray:
            visit_unpackintoarray((BST_UnpackIntoArray*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Branch:
            visit_branch((BST_Branch*)node);
            break;
        case BST_TYPE::Jump:
            return visit_jump((BST_Jump*)node);
        case BST_TYPE::Invoke:
            visit_invoke((BST_Invoke*)node);
            break;
        case BST_TYPE::SetExcInfo:
            visit_setexcinfo((BST_SetExcInfo*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::UncacheExcInfo:
            visit_uncacheexcinfo((BST_UncacheExcInfo*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::PrintExpr:
            visit_printexpr((BST_PrintExpr*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;

        // Handle all cases which are derived from BST_stmt_with_dest
        default: {
            Value v;
            switch (node->type) {
                case BST_TYPE::CopyVReg:
                    v = visit_copyvreg((BST_CopyVReg*)node);
                    break;
                case BST_TYPE::AugBinOp:
                    v = visit_augBinOp((BST_AugBinOp*)node);
                    break;
                case BST_TYPE::CallFunc:
                case BST_TYPE::CallAttr:
                case BST_TYPE::CallClsAttr:
                    v = visit_call((BST_Call*)node);
                    break;
                case BST_TYPE::Compare:
                    v = visit_compare((BST_Compare*)node);
                    break;
                case BST_TYPE::BinOp:
                    v = visit_binop((BST_BinOp*)node);
                    break;
                case BST_TYPE::Dict:
                    v = visit_dict((BST_Dict*)node);
                    break;
                case BST_TYPE::List:
                    v = visit_list((BST_List*)node);
                    break;
                case BST_TYPE::Repr:
                    v = visit_repr((BST_Repr*)node);
                    break;
                case BST_TYPE::Set:
                    v = visit_set((BST_Set*)node);
                    break;
                case BST_TYPE::Tuple:
                    v = visit_tuple((BST_Tuple*)node);
                    break;
                case BST_TYPE::UnaryOp:
                    v = visit_unaryop((BST_UnaryOp*)node);
                    break;
                case BST_TYPE::Yield:
                    v = visit_yield((BST_Yield*)node);
                    break;
                case BST_TYPE::Landingpad:
                    v = visit_landingpad((BST_Landingpad*)node);
                    break;
                case BST_TYPE::Locals:
                    v = visit_locals((BST_Locals*)node);
                    break;
                case BST_TYPE::LoadName:
                    v = visit_loadname((BST_LoadName*)node);
                    break;
                case BST_TYPE::LoadAttr:
                    v = visit_loadattr((BST_LoadAttr*)node);
                    break;
                case BST_TYPE::GetIter:
                    v = visit_getiter((BST_GetIter*)node);
                    break;
                case BST_TYPE::ImportFrom:
                    v = visit_importfrom((BST_ImportFrom*)node);
                    break;
                case BST_TYPE::ImportName:
                    v = visit_importname((BST_ImportName*)node);
                    break;
                case BST_TYPE::ImportStar:
                    v = visit_importstar((BST_ImportStar*)node);
                    break;
                case BST_TYPE::Nonzero:
                    v = visit_nonzero((BST_Nonzero*)node);
                    break;
                case BST_TYPE::CheckExcMatch:
                    v = visit_checkexcmatch((BST_CheckExcMatch*)node);
                    break;
                case BST_TYPE::HasNext:
                    v = visit_hasnext((BST_HasNext*)node);
                    break;
                case BST_TYPE::MakeClass:
                    v = visit_makeClass((BST_MakeClass*)node);
                    break;
                case BST_TYPE::MakeFunction:
                    v = visit_makeFunction((BST_MakeFunction*)node);
                    break;
                case BST_TYPE::LoadSub:
                    v = visit_loadsub((BST_LoadSub*)node);
                    break;
                case BST_TYPE::LoadSubSlice:
                    v = visit_loadsubslice((BST_LoadSubSlice*)node);
                    break;
                case BST_TYPE::MakeSlice:
                    v = visit_makeslice((BST_MakeSlice*)node);
                    break;
                default:
                    RELEASE_ASSERT(0, "not implemented %d", node->type);
            };
            doStore(((BST_stmt_with_dest*)node)->vreg_dst, v);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        }
    };

    return Value();
}

Value ASTInterpreter::visit_return(BST_Return* node) {
    Value s = node->vreg_value != VREG_UNDEFINED ? getVReg(node->vreg_value) : getNone();

    if (jit) {
        jit->emitReturn(s);
        finishJITing();
    }

// Some day, we should make sure that all temporaries got deleted (and decrefed) at the right time:
#if 0
    bool temporaries_alive = false;
#ifndef NDEBUG
    for (auto&& v : getSymVRegMap()) {
        if (v.first.s()[0] == '#' && vregs[v.second]) {
            fprintf(stderr, "%s still alive\n", v.first.c_str());
            temporaries_alive = true;
        }
    }

    if (temporaries_alive)
        source_info->cfg->print();

    assert(!temporaries_alive);
#endif
#endif

    next_block = 0;
    return s;
}

Value ASTInterpreter::createFunction(BST_FunctionDef* node, BoxedCode* code) {
    assert(code);

    std::vector<Box*> defaults;
    llvm::SmallVector<RewriterVar*, 4> defaults_vars;
    defaults.reserve(node->num_defaults);

    RewriterVar* defaults_var = NULL;
    if (jit) {
        defaults_var = node->num_defaults ? jit->allocate(node->num_defaults) : jit->imm(0ul);
        defaults_vars.reserve(node->num_defaults);
    }

    for (int i = 0; i < node->num_defaults; ++i) {
        Value v = getVReg(node->elts[node->num_decorator + i]);
        defaults.push_back(v.o);
        if (jit) {
            defaults_var->setAttr(i * sizeof(void*), v, RewriterVar::SetattrType::REF_USED);
            defaults_vars.push_back(v.var);
        }
    }
    defaults.push_back(0);
    AUTO_XDECREF_ARRAY(defaults.data(), defaults.size());

    // FIXME: Using initializer_list is pretty annoying since you're not supposed to create them:
    union {
        struct {
            Box** ptr;
            size_t s;
        } d;
        std::initializer_list<Box*> il = {};
    } u;

    u.d.ptr = &defaults[0];
    u.d.s = defaults.size() - 1;

    bool takes_closure = code->source->scoping.takesClosure();

    BoxedClosure* closure = 0;
    RewriterVar* closure_var = NULL;
    if (takes_closure) {
        if (scope_info.createsClosure()) {
            closure = created_closure;
            if (jit)
                closure_var = jit->getInterp()->getAttr(offsetof(ASTInterpreter, created_closure));
        } else {
            assert(scope_info.passesThroughClosure());
            closure = frame_info.passed_closure;
            if (jit)
                closure_var = jit->getInterp()->getAttr(offsetof(ASTInterpreter, frame_info.passed_closure));
        }
        assert(closure);
    }

    Box* passed_globals = NULL;
    RewriterVar* passed_globals_var = NULL;
    if (!getCode()->source->scoping.areGlobalsFromModule()) {
        passed_globals = frame_info.globals;
        if (jit)
            passed_globals_var = jit->getInterp()->getAttr(offsetof(ASTInterpreter, frame_info.globals));
    }

    Value rtn;
    if (jit) {
        if (!closure_var)
            closure_var = jit->imm(0ul);
        if (!passed_globals_var)
            passed_globals_var = jit->imm(0ul);
        // TODO: have to track this GC ref
        rtn.var = jit->call(false, (void*)createFunctionFromMetadata, { jit->imm(code), closure_var, passed_globals_var,
                                                                        defaults_var, jit->imm(node->num_defaults) },
                            {}, defaults_vars)->setType(RefType::OWNED);
    }

    rtn.o = createFunctionFromMetadata(code, closure, passed_globals, u.il);

    return rtn;
}

Value ASTInterpreter::visit_makeFunction(BST_MakeFunction* mkfn) {
    auto func_entry = getCodeConstants().getFuncOrClass(mkfn->index_func_def);
    auto node = bst_cast<BST_FunctionDef>(func_entry.first);

    std::vector<Value> decorators;
    decorators.reserve(node->num_decorator);
    for (int i = 0; i < node->num_decorator; ++i)
        decorators.push_back(getVReg(node->elts[i]));

    Value func = createFunction(node, func_entry.second);

    for (int i = decorators.size() - 1; i >= 0; i--) {
        func.o = runtimeCall(autoDecref(decorators[i].o), ArgPassSpec(1), autoDecref(func.o), 0, 0, 0, 0);

        if (jit) {
            auto prev_func_var = func.var;
            func.var = jit->emitRuntimeCall(NULL, decorators[i], ArgPassSpec(1), { func }, NULL);
        }
    }
    return func;
}

Value ASTInterpreter::visit_makeClass(BST_MakeClass* mkclass) {
    abortJITing();

    auto class_entry = getCodeConstants().getFuncOrClass(mkclass->index_class_def);
    auto node = bst_cast<BST_ClassDef>(class_entry.first);

    BoxedTuple* bases_tuple = (BoxedTuple*)getVReg(node->vreg_bases_tuple).o;
    assert(bases_tuple->cls == tuple_cls);
    AUTO_DECREF(bases_tuple);

    std::vector<Box*> decorators;
    decorators.reserve(node->num_decorator);
    for (int i = 0; i < node->num_decorator; ++i) {
        decorators.push_back(getVReg(node->decorator[i]).o);
    }

    BoxedCode* code = class_entry.second;
    assert(code);

    const ScopingResults& scope_info = code->source->scoping;

    BoxedClosure* closure = NULL;
    if (scope_info.takesClosure()) {
        if (this->scope_info.passesThroughClosure())
            closure = getPassedClosure();
        else
            closure = created_closure;
        assert(closure);
    }

    Box* passed_globals = NULL;
    if (!getCode()->source->scoping.areGlobalsFromModule())
        passed_globals = frame_info.globals;
    Box* attrDict = runtimeCall(autoDecref(createFunctionFromMetadata(code, closure, passed_globals, {})),
                                ArgPassSpec(0), 0, 0, 0, 0, 0);
    AUTO_DECREF(attrDict);

    InternedString name = getCodeConstants().getInternedString(node->index_name);
    Box* classobj = createUserClass(name.getBox(), bases_tuple, attrDict);

    for (int i = decorators.size() - 1; i >= 0; i--) {
        classobj = runtimeCall(autoDecref(decorators[i]), ArgPassSpec(1), autoDecref(classobj), 0, 0, 0, 0);
    }

    return Value(classobj, NULL);
}

void ASTInterpreter::visit_raise(BST_Raise* node) {
    if (node->vreg_arg0 == VREG_UNDEFINED) {
        assert(node->vreg_arg1 == VREG_UNDEFINED);
        assert(node->vreg_arg2 == VREG_UNDEFINED);

        if (jit) {
            jit->emitRaise0();
            finishJITing();
        }

        ASTInterpreterJitInterface::raise0Helper(this);
    }

    Value arg0 = node->vreg_arg0 != VREG_UNDEFINED ? getVReg(node->vreg_arg0) : getNone();
    Value arg1 = node->vreg_arg1 != VREG_UNDEFINED ? getVReg(node->vreg_arg1) : getNone();
    Value arg2 = node->vreg_arg2 != VREG_UNDEFINED ? getVReg(node->vreg_arg2) : getNone();

    if (jit) {
        jit->emitRaise3(arg0, arg1, arg2);
        finishJITing();
    }

    raise3(arg0.o, arg1.o, arg2.o);
}

void ASTInterpreter::visit_assert(BST_Assert* node) {
    abortJITing();

    static BoxedString* AssertionError_str = getStaticString("AssertionError");
    Box* assertion_type = getGlobal(frame_info.globals, AssertionError_str);
    AUTO_DECREF(assertion_type);
    Box* msg = node->vreg_msg != VREG_UNDEFINED ? getVReg(node->vreg_msg).o : 0;
    AUTO_XDECREF(msg);
    assertFail(assertion_type, msg);
}

void ASTInterpreter::visit_deleteattr(BST_DeleteAttr* node) {
    Value target = getVReg(node->vreg_value);
    AUTO_DECREF(target.o);
    BoxedString* str = getCodeConstants().getInternedString(node->index_attr).getBox();
    if (jit)
        jit->emitDelAttr(target, str);
    delattr(target.o, str);
}

void ASTInterpreter::visit_deletesub(BST_DeleteSub* node) {
    Value value = getVReg(node->vreg_value);
    AUTO_DECREF(value.o);
    Value slice = getVReg(node->vreg_slice);
    AUTO_DECREF(slice.o);
    if (jit)
        jit->emitDelItem(value, slice);
    delitem(value.o, slice.o);
}

void ASTInterpreter::visit_deletesubslice(BST_DeleteSubSlice* node) {
    Value value = getVReg(node->vreg_value);
    AUTO_DECREF(value.o);
    Value lower = node->vreg_lower != VREG_UNDEFINED ? getVReg(node->vreg_lower) : Value();
    AUTO_XDECREF(lower.o);
    Value upper = node->vreg_upper != VREG_UNDEFINED ? getVReg(node->vreg_upper) : Value();
    AUTO_XDECREF(upper.o);

    if (jit)
        jit->emitAssignSlice(value, lower, upper, jit->imm(0ul));
    assignSlice(value.o, lower.o, upper.o, NULL);
}

void ASTInterpreter::visit_deletename(BST_DeleteName* target) {
    assert(target->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);

    ScopeInfo::VarScopeType vst = target->lookup_type;
    InternedString id = getCodeConstants().getInternedString(target->index_id);
    if (vst == ScopeInfo::VarScopeType::GLOBAL) {
        if (jit)
            jit->emitDelGlobal(id.getBox());
        delGlobal(frame_info.globals, id.getBox());
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        if (jit)
            jit->emitDelName(id);
        ASTInterpreterJitInterface::delNameHelper(this, id);
    } else {
        assert(vst == ScopeInfo::VarScopeType::FAST);

        assert(getVRegInfo().getVReg(id) == target->vreg);

        if (id.s()[0] == '#') {
            assert(vregs[target->vreg] != NULL);
            if (jit)
                jit->emitKillTemporary(target->vreg);
        } else {
            abortJITing();
            if (vregs[target->vreg] == 0) {
                assertNameDefined(0, id.c_str(), NameError, true /* local_var_msg */);
                return;
            }
        }

        frame_info.num_vregs = std::max(frame_info.num_vregs, target->vreg + 1);
        Py_DECREF(vregs[target->vreg]);
        vregs[target->vreg] = NULL;
    }
}

Value ASTInterpreter::visit_copyvreg(BST_CopyVReg* node) {
    return getVReg(node->vreg_src, false /* don't kill */);
}

void ASTInterpreter::visit_print(BST_Print* node) {
    Value dest = node->vreg_dest != VREG_UNDEFINED ? getVReg(node->vreg_dest) : Value();
    Value var = node->vreg_value != VREG_UNDEFINED ? getVReg(node->vreg_value) : Value();

    if (jit)
        jit->emitPrint(dest, var, node->nl);

    if (node->vreg_dest != VREG_UNDEFINED)
        printHelper(autoDecref(dest.o), autoXDecref(var.o), node->nl);
    else
        printHelper(NULL, autoXDecref(var.o), node->nl);
}

void ASTInterpreter::visit_exec(BST_Exec* node) {
    // TODO implement the locals and globals arguments
    Value code = getVReg(node->vreg_body);
    AUTO_DECREF(code.o);
    Value globals = node->vreg_globals == VREG_UNDEFINED ? Value() : getVReg(node->vreg_globals);
    AUTO_XDECREF(globals.o);
    Value locals = node->vreg_locals == VREG_UNDEFINED ? Value() : getVReg(node->vreg_locals);
    AUTO_XDECREF(locals.o);

    if (jit)
        jit->emitExec(code, globals, locals, this->source_info->future_flags);
    exec(code.o, globals.o, locals.o, this->source_info->future_flags);
}

Value ASTInterpreter::visit_compare(BST_Compare* node) {
    Value left = getVReg(node->vreg_left);
    AUTO_DECREF(left.o);
    Value right = getVReg(node->vreg_comparator);
    AUTO_DECREF(right.o);
    return doBinOp(node, left, right, node->op, BinExpType::Compare);
}

Value ASTInterpreter::visit_call(BST_Call* node) {
    Value v;
    Value func;

    InternedString attr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    int* vreg_elts = NULL;
    if (node->type == BST_TYPE::CallAttr) {
        is_callattr = true;
        callattr_clsonly = false;
        auto* attr_ast = bst_cast<BST_CallAttr>(node);
        func = getVReg(attr_ast->vreg_value);
        attr = getCodeConstants().getInternedString(attr_ast->index_attr);
        vreg_elts = bst_cast<BST_CallAttr>(node)->elts;
    } else if (node->type == BST_TYPE::CallClsAttr) {
        is_callattr = true;
        callattr_clsonly = true;
        auto* attr_ast = bst_cast<BST_CallClsAttr>(node);
        func = getVReg(attr_ast->vreg_value);
        attr = getCodeConstants().getInternedString(attr_ast->index_attr);
        vreg_elts = bst_cast<BST_CallClsAttr>(node)->elts;
    } else {
        auto* attr_ast = bst_cast<BST_CallFunc>(node);
        func = getVReg(attr_ast->vreg_func);
        vreg_elts = bst_cast<BST_CallFunc>(node)->elts;
    }

    AUTO_DECREF(func.o);

    llvm::SmallVector<Box*, 8> args;
    llvm::SmallVector<RewriterVar*, 8> args_vars;
    args.reserve(node->num_args + node->num_keywords);
    args_vars.reserve(node->num_args + node->num_keywords);

    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        Value v = getVReg(vreg_elts[i]);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    const std::vector<BoxedString*>* keyword_names = NULL;
    if (node->num_keywords)
        keyword_names = getCodeConstants().getKeywordNames(node->index_keyword_names);

    if (node->vreg_starargs != VREG_UNDEFINED) {
        Value v = getVReg(node->vreg_starargs);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    if (node->vreg_kwargs != VREG_UNDEFINED) {
        Value v = getVReg(node->vreg_kwargs);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    AUTO_DECREF_ARRAY(args.data(), args.size());

    ArgPassSpec argspec(node->num_args, node->num_keywords, node->vreg_starargs != VREG_UNDEFINED,
                        node->vreg_kwargs != VREG_UNDEFINED);

    if (is_callattr) {
        CallattrFlags callattr_flags{.cls_only = callattr_clsonly, .null_on_nonexistent = false, .argspec = argspec };

        if (jit)
            v.var = jit->emitCallattr(node, func, attr.getBox(), callattr_flags, args_vars, keyword_names);

        v.o = callattr(func.o, attr.getBox(), callattr_flags, args.size() > 0 ? args[0] : 0,
                       args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0,
                       keyword_names);
    } else {
        if (jit)
            v.var = jit->emitRuntimeCall(node, func, argspec, args_vars, keyword_names);

        v.o = runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                          args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, keyword_names);
    }

    return v;
}

Value ASTInterpreter::visit_repr(BST_Repr* node) {
    Value v = getVReg(node->vreg_value);
    AUTO_DECREF(v.o);
    return Value(repr(v.o), jit ? jit->emitRepr(v) : NULL);
}

Value ASTInterpreter::visit_dict(BST_Dict* node) {
    BoxedDict* dict = new BoxedDict();
    RewriterVar* r_dict = jit ? jit->emitCreateDict() : NULL;
    return Value(dict, r_dict);
}

Value ASTInterpreter::visit_set(BST_Set* node) {
    BoxedSet* set = (BoxedSet*)createSet();
    try {
        // insert the elements in reverse like cpython does
        // important for {1, 1L}
        llvm::SmallVector<RewriterVar*, 8> items;
        for (int i = node->num_elts - 1; i >= 0; --i) {
            Value v = getVReg(node->elts[i]);
            _setAddStolen(set, v.o);
            items.push_back(v);
        }
        return Value(set, jit ? jit->emitCreateSet(items) : NULL);
    } catch (ExcInfo e) {
        Py_DECREF(set);
        throw e;
    }
}

Value ASTInterpreter::getVReg(int vreg, bool is_kill) {
    assert(vreg != VREG_UNDEFINED);
    if (vreg < 0) {
        Box* o = getCodeConstants().getConstant(vreg);
        return Value(incref(o), jit ? jit->imm(o)->setType(RefType::BORROWED) : NULL);
    }

    Value v;
    if (jit) {
        bool is_live = true;
        if (is_kill) {
            is_live = false;
        } else {
            is_live = getLiveness()->isLiveAtEnd(vreg, current_block);
        }

        if (is_live) {
            assert(!is_kill);
            v.var = jit->emitGetLocalMustExist(vreg);
        } else {
            v.var = jit->emitGetBlockLocalMustExist(vreg);
            if (is_kill)
                jit->emitKillTemporary(vreg);
        }
    }

    frame_info.num_vregs = std::max(frame_info.num_vregs, vreg + 1);
    Box* val = vregs[vreg];

    if (val) {
        v.o = val;
        if (is_kill)
            vregs[vreg] = NULL;
        else
            Py_INCREF(val);
        return v;
    }


    current_block->print(getCodeConstants());
    printf("vreg: %d num cross: %d\n", vreg, getVRegInfo().getNumOfCrossBlockVRegs());
    printf("\n\n");
    current_block->cfg->print(getCodeConstants());

    assert(0);
    return Value();
}

Value ASTInterpreter::visit_loadname(BST_LoadName* node) {
    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);
    InternedString id = getCodeConstants().getInternedString(node->index_id);
    switch (node->lookup_type) {
        case ScopeInfo::VarScopeType::GLOBAL: {
            Value v;
            if (jit)
                v.var = jit->emitGetGlobal(id.getBox());

            v.o = getGlobal(frame_info.globals, id.getBox());
            return v;
        }
        case ScopeInfo::VarScopeType::DEREF: {
            return Value(ASTInterpreterJitInterface::derefHelper(this, node), jit ? jit->emitDeref(node) : NULL);
        }
        case ScopeInfo::VarScopeType::FAST:
        case ScopeInfo::VarScopeType::CLOSURE: {
            Value v;
            if (jit) {
                bool is_live = true;
                if (node->lookup_type == ScopeInfo::VarScopeType::FAST) {
                    assert(node->vreg >= 0);
                    is_live = getLiveness()->isLiveAtEnd(node->vreg, current_block);
                }

                if (is_live)
                    v.var = jit->emitGetLocal(id, node->vreg);
                else
                    v.var = jit->emitGetBlockLocal(id, node->vreg);
            }

            assert(node->vreg >= 0);
            assert(getVRegInfo().getVReg(id) == node->vreg);
            frame_info.num_vregs = std::max(frame_info.num_vregs, node->vreg + 1);
            Box* val = vregs[node->vreg];

            if (val) {
                v.o = val;
                Py_INCREF(val);
                return v;
            }

            assertNameDefined(0, id.c_str(), UnboundLocalError, true);
            RELEASE_ASSERT(0, "should be unreachable");
        }
        case ScopeInfo::VarScopeType::NAME: {
            Value v;
            if (jit)
                v.var = jit->emitGetBoxedLocal(id.getBox());
            v.o = boxedLocalsGet(frame_info.boxedLocals, id.getBox(), frame_info.globals);
            return v;
        }
        default:
            abort();
    }
}

Value ASTInterpreter::visit_loadattr(BST_LoadAttr* node) {
    Value v = getVReg(node->vreg_value);
    AUTO_DECREF(v.o);

    BoxedString* attr = getCodeConstants().getInternedString(node->index_attr).getBox();
    Value r;
    if (node->clsonly)
        r = Value(getclsattr(v.o, attr), jit ? jit->emitGetClsAttr(v, attr) : NULL);
    else
        r = Value(pyston::getattr(v.o, attr), jit ? jit->emitGetAttr(node, v, attr) : NULL);
    return r;
}


Value ASTInterpreter::visit_loadsub(BST_LoadSub* node) {
    Value value = getVReg(node->vreg_value);
    AUTO_DECREF(value.o);

    Value slice = getVReg(node->vreg_slice);
    AUTO_DECREF(slice.o);

    Value v;
    if (jit)
        v.var = jit->emitGetItem(node, value, slice);
    v.o = getitem(value.o, slice.o);
    return v;
}

Value ASTInterpreter::visit_loadsubslice(BST_LoadSubSlice* node) {
    Value value = getVReg(node->vreg_value);
    AUTO_DECREF(value.o);
    Value lower = node->vreg_lower != VREG_UNDEFINED ? getVReg(node->vreg_lower) : Value();
    AUTO_XDECREF(lower.o);
    Value upper = node->vreg_upper != VREG_UNDEFINED ? getVReg(node->vreg_upper) : Value();
    AUTO_XDECREF(upper.o);

    Value v;
    if (jit)
        v.var = jit->emitApplySlice(value, lower, upper);
    v.o = applySlice(value.o, lower.o, upper.o);
    return v;
}

void ASTInterpreter::visit_storename(BST_StoreName* node) {
    Value value = getVReg(node->vreg_value);

    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);

    InternedString name = getCodeConstants().getInternedString(node->index_id);
    ScopeInfo::VarScopeType vst = node->lookup_type;
    if (vst == ScopeInfo::VarScopeType::GLOBAL) {
        if (jit)
            jit->emitSetGlobal(name.getBox(), value, getCode()->source->scoping.areGlobalsFromModule());
        setGlobal(frame_info.globals, name.getBox(), value.o);
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        if (jit)
            jit->emitSetItemName(name.getBox(), value);
        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        AUTO_DECREF(value.o);
        setitem(frame_info.boxedLocals, name.getBox(), value.o);
    } else {
        bool closure = vst == ScopeInfo::VarScopeType::CLOSURE;
        if (jit) {
            bool is_live = true;
            if (!closure)
                is_live = getLiveness()->isLiveAtEnd(node->vreg, current_block);
            if (is_live) {
                if (closure) {
                    jit->emitSetLocalClosure(node, value);
                } else
                    jit->emitSetLocal(node->vreg, value);
            } else
                jit->emitSetBlockLocal(node->vreg, value);
        }

        if (closure) {
            ASTInterpreterJitInterface::setLocalClosureHelper(this, node->vreg, node->closure_offset, value.o);
        } else {
            assert(getVRegInfo().getVReg(name) == node->vreg);
            frame_info.num_vregs = std::max(frame_info.num_vregs, node->vreg + 1);
            Box* prev = vregs[node->vreg];
            vregs[node->vreg] = value.o;
            Py_XDECREF(prev);
        }
    }
}


void ASTInterpreter::visit_storeattr(BST_StoreAttr* node) {
    Value value = getVReg(node->vreg_value);
    Value o = getVReg(node->vreg_target);
    InternedString attr = getCodeConstants().getInternedString(node->index_attr);
    if (jit) {
        jit->emitSetAttr(node, o, attr.getBox(), value);
    }
    AUTO_DECREF(o.o);
    pyston::setattr(o.o, attr.getBox(), value.o);
}

void ASTInterpreter::visit_storesub(BST_StoreSub* node) {
    Value value = getVReg(node->vreg_value);
    AUTO_DECREF(value.o);

    Value target = getVReg(node->vreg_target);
    AUTO_DECREF(target.o);

    Value slice = getVReg(node->vreg_slice);

    AUTO_DECREF(slice.o);

    if (jit)
        jit->emitSetItem(target, slice, value);
    setitem(target.o, slice.o, value.o);
}

void ASTInterpreter::visit_storesubslice(BST_StoreSubSlice* node) {
    Value value = getVReg(node->vreg_value);
    AUTO_DECREF(value.o);

    Value target = getVReg(node->vreg_target);
    AUTO_DECREF(target.o);

    Value lower = node->vreg_lower != VREG_UNDEFINED ? getVReg(node->vreg_lower) : Value();
    AUTO_XDECREF(lower.o);
    Value upper = node->vreg_upper != VREG_UNDEFINED ? getVReg(node->vreg_upper) : Value();
    AUTO_XDECREF(upper.o);

    if (jit)
        jit->emitAssignSlice(target, lower, upper, value);
    assignSlice(target.o, lower.o, upper.o, value.o);
}

Value ASTInterpreter::visit_list(BST_List* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    BoxedList* list = new BoxedList();
    list->ensure(node->num_elts);
    for (int i = 0; i < node->num_elts; ++i) {
        try {
            Value v = getVReg(node->elts[i]);
            items.push_back(v);
            listAppendInternalStolen(list, v.o);
        } catch (ExcInfo e) {
            // The CFG currently converts all list expressions to something like
            // #0 = <expr1>
            // #1 = <expr2>
            // #2 = [#0, #1]
            RELEASE_ASSERT(0, "list elements should not be throwing");
        }
    }

    return Value(list, jit ? jit->emitCreateList(items) : NULL);
}

Value ASTInterpreter::visit_tuple(BST_Tuple* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    BoxedTuple* rtn = BoxedTuple::create(node->num_elts);
    for (int i = 0; i < node->num_elts; ++i) {
        Value v = getVReg(node->elts[i]);
        rtn->elts[i] = v.o;
        items.push_back(v);
    }

    return Value(rtn, jit ? jit->emitCreateTuple(items) : NULL);
}
}


int ASTInterpreterJitInterface::getBoxedLocalsOffset() {
    return offsetof(ASTInterpreter, frame_info.boxedLocals);
}

int ASTInterpreterJitInterface::getCreatedClosureOffset() {
    return offsetof(ASTInterpreter, created_closure);
}

int ASTInterpreterJitInterface::getCurrentBlockOffset() {
    return offsetof(ASTInterpreter, current_block);
}

int ASTInterpreterJitInterface::getCurrentInstOffset() {
    return offsetof(ASTInterpreter, frame_info.stmt);
}

int ASTInterpreterJitInterface::getEdgeCountOffset() {
    static_assert(sizeof(ASTInterpreter::edgecount) == 4, "caller assumes that");
    return offsetof(ASTInterpreter, edgecount);
}

int ASTInterpreterJitInterface::getGeneratorOffset() {
    return offsetof(ASTInterpreter, generator);
}

int ASTInterpreterJitInterface::getGlobalsOffset() {
    return offsetof(ASTInterpreter, frame_info.globals);
}

void ASTInterpreterJitInterface::delNameHelper(void* _interpreter, InternedString name) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    Box* boxed_locals = interpreter->frame_info.boxedLocals;
    assert(boxed_locals != NULL);
    if (boxed_locals->cls == dict_cls) {
        auto& d = static_cast<BoxedDict*>(boxed_locals)->d;
        auto it = d.find(name.getBox());
        if (it == d.end()) {
            assertNameDefined(0, name.c_str(), NameError, false /* local_var_msg */);
        }
        Py_DECREF(it->first.value);
        Py_DECREF(it->second);
        d.erase(it);
    } else if (boxed_locals->cls == attrwrapper_cls) {
        attrwrapperDel(boxed_locals, name);
    } else {
        RELEASE_ASSERT(0, "%s", boxed_locals->cls->tp_name);
    }
}

Box* ASTInterpreterJitInterface::derefHelper(void* _interpreter, BST_LoadName* node) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    DerefInfo deref_info = interpreter->scope_info.getDerefInfo(node);
    assert(interpreter->getPassedClosure());
    BoxedClosure* closure = interpreter->getPassedClosure();
    for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
        closure = closure->parent;
    }
    Box* val = closure->elts[deref_info.offset];
    if (val == NULL) {
        InternedString id = interpreter->getCodeConstants().getInternedString(node->index_id);
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", id.c_str());
    }
    Py_INCREF(val);
    return val;
}

Box* ASTInterpreterJitInterface::landingpadHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    ExcInfo& last_exception = interpreter->last_exception;
    Box* type = last_exception.type;
    Box* value = last_exception.value ? last_exception.value : Py_None;
    Box* traceback = last_exception.traceback ? last_exception.traceback : Py_None;
    Box* rtn = BoxedTuple::create({ type, value, traceback });
    Py_CLEAR(last_exception.type);
    Py_CLEAR(last_exception.value);
    Py_CLEAR(last_exception.traceback);
    return rtn;
}

void ASTInterpreterJitInterface::pendingCallsCheckHelper() {
#if ENABLE_SIGNAL_CHECKING
    if (unlikely(_stop_thread))
        makePendingCalls();
#endif
}

void ASTInterpreterJitInterface::setExcInfoHelper(void* _interpreter, STOLEN(Box*) type, STOLEN(Box*) value,
                                                  STOLEN(Box*) traceback) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    setFrameExcInfo(interpreter->getFrameInfo(), type, value, traceback);
}

void ASTInterpreterJitInterface::setLocalClosureHelper(void* _interpreter, int vreg, int closure_offset, Box* v) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    interpreter->frame_info.num_vregs = std::max(interpreter->frame_info.num_vregs, (int)vreg + 1);
    Box* prev = interpreter->vregs[vreg];
    interpreter->vregs[vreg] = v;
    Box* prev_closure_elt = interpreter->created_closure->elts[closure_offset];
    interpreter->created_closure->elts[closure_offset] = incref(v);
    Py_XDECREF(prev);
    Py_XDECREF(prev_closure_elt);
}

void ASTInterpreterJitInterface::uncacheExcInfoHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    setFrameExcInfo(interpreter->getFrameInfo(), NULL, NULL, NULL);
}

void ASTInterpreterJitInterface::raise0Helper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    raise0(&interpreter->getFrameInfo()->exc);
}

Box* ASTInterpreterJitInterface::yieldHelper(void* _interpreter, STOLEN(Box*) value) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    auto generator = interpreter->generator;
    assert(generator && generator->cls == generator_cls);

    Box* live_values = { interpreter->created_closure };
    return yield(generator, value, live_values);
}

const void* interpreter_instr_addr = (void*)&executeInnerAndSetupFrame;

// small wrapper around executeInner because we can not directly call the member function from asm.
extern "C" Box* executeInnerFromASM(ASTInterpreter& interpreter, CFGBlock* start_block, BST_stmt* start_at) {
    initFrame(interpreter.getFrameInfo());
    Box* rtn = ASTInterpreter::executeInner(interpreter, start_block, start_at);
    deinitFrameMaybe(interpreter.getFrameInfo());
    return rtn;
}

Box* astInterpretFunction(BoxedCode* code, Box* closure, Box* generator, Box* globals, Box* arg1, Box* arg2, Box* arg3,
                          Box** args) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_interpreter");

    SourceInfo* source_info = code->source.get();

    assert((!globals) == source_info->scoping.areGlobalsFromModule());
    bool can_reopt = ENABLE_REOPT && !FORCE_INTERPRETER;

    if (unlikely(can_reopt
                 && (FORCE_OPTIMIZE || !ENABLE_INTERPRETER || code->times_interpreted > REOPT_THRESHOLD_BASELINE))) {
        code->times_interpreted = 0;

        // EffortLevel new_effort = EffortLevel::MODERATE;
        EffortLevel new_effort = EffortLevel::MAXIMAL; // always use max opt (disabled moderate opt tier)
        if (FORCE_OPTIMIZE)
            new_effort = EffortLevel::MAXIMAL;

        std::vector<ConcreteCompilerType*> arg_types;
        for (int i = 0; i < code->param_names.totalParameters(); i++) {
            Box* arg = getArg(i, arg1, arg2, arg3, args);

            assert(arg || i == code->param_names.kwargsIndex()); // only builtin functions can pass NULL args

            // TODO: reenable argument-type specialization
            arg_types.push_back(UNKNOWN);
            // arg_types.push_back(typeFromClass(arg->cls));
        }
        FunctionSpecialization* spec = new FunctionSpecialization(UNKNOWN, arg_types);

        // this also pushes the new CompiledVersion to the back of the version list:
        CompiledFunction* optimized = compileFunction(code, spec, new_effort, NULL);

        code->dependent_interp_callsites.invalidateAll();

        UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_jitted_code");
        Box* r;
        Box* maybe_args[3];
        int nmaybe_args = 0;
        if (closure)
            maybe_args[nmaybe_args++] = closure;
        if (generator)
            maybe_args[nmaybe_args++] = generator;
        if (globals)
            maybe_args[nmaybe_args++] = globals;
        if (nmaybe_args == 0)
            r = optimized->call(arg1, arg2, arg3, args);
        else if (nmaybe_args == 1)
            r = optimized->call1(maybe_args[0], arg1, arg2, arg3, args);
        else if (nmaybe_args == 2)
            r = optimized->call2(maybe_args[0], maybe_args[1], arg1, arg2, arg3, args);
        else {
            assert(nmaybe_args == 3);
            r = optimized->call3(maybe_args[0], maybe_args[1], maybe_args[2], arg1, arg2, arg3, args);
        }

        if (optimized->exception_style == CXX)
            return r;
        else {
            if (!r)
                throwCAPIException();
            return r;
        }
    }

    assert(source_info->cfg);

    Box** vregs = NULL;
    int num_vregs = source_info->cfg->getVRegInfo().getTotalNumOfVRegs();
    if (num_vregs > 0) {
        vregs = (Box**)alloca(sizeof(Box*) * num_vregs);
        memset(vregs, 0, sizeof(Box*) * num_vregs);
    }

    ++code->times_interpreted;
    ASTInterpreter interpreter(code, vregs);

    const ScopingResults& scope_info = code->source->scoping;

    if (unlikely(scope_info.usesNameLookup())) {
        interpreter.setBoxedLocals(new BoxedDict());
    }

    assert((!globals) == code->source->scoping.areGlobalsFromModule());
    if (globals) {
        interpreter.setGlobals(globals);
    } else {
        interpreter.setGlobals(source_info->parent_module);
    }

    interpreter.initArguments((BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    Box* v = ASTInterpreter::execute(interpreter);
    return v ? v : incref(Py_None);
}

Box* astInterpretFunctionEval(BoxedCode* code, Box* globals, Box* boxedLocals) {
    ++code->times_interpreted;

    SourceInfo* source_info = code->source.get();
    assert(source_info->cfg);

    Box** vregs = NULL;
    int num_vregs = source_info->cfg->getVRegInfo().getTotalNumOfVRegs();
    if (num_vregs > 0) {
        vregs = (Box**)alloca(sizeof(Box*) * num_vregs);
        memset(vregs, 0, sizeof(Box*) * num_vregs);
    }

    ASTInterpreter interpreter(code, vregs);
    interpreter.initArguments(NULL, NULL, NULL, NULL, NULL, NULL);
    interpreter.setBoxedLocals(incref(boxedLocals));

    assert(!code->source->scoping.areGlobalsFromModule());
    assert(globals);
    interpreter.setGlobals(globals);

    Box* v = ASTInterpreter::execute(interpreter);
    return v ? v : incref(Py_None);
}

// caution when changing the function arguments: this function gets called from an assembler wrapper!
extern "C" Box* astInterpretDeoptFromASM(BoxedCode* code, BST_stmt* enclosing_stmt, Box* expr_val,
                                         STOLEN(FrameStackState) frame_state) {
    static_assert(sizeof(FrameStackState) <= 2 * 8, "astInterpretDeopt assumes that all args get passed in regs!");

    assert(code);
    assert(enclosing_stmt);
    assert(frame_state.locals);
    assert(expr_val);

    SourceInfo* source_info = code->source.get();

    // We can't reuse the existing vregs from the LLVM tier because they only contain the user visible ones this means
    // there wouldn't be enough space for the compiler generated ones which the interpreter (+bjit) stores inside the
    // vreg array.
    Box** vregs = NULL;
    int num_vregs = source_info->cfg->getVRegInfo().getTotalNumOfVRegs();
    if (num_vregs > 0) {
        vregs = (Box**)alloca(sizeof(Box*) * num_vregs);
        memset(vregs, 0, sizeof(Box*) * num_vregs);
    }

    // We need to remove the old python frame created in the LLVM tier otherwise we would have a duplicate frame because
    // the interpreter will set the new state before executing the first statement.
    RELEASE_ASSERT(cur_thread_state.frame_info == frame_state.frame_info, "");
    cur_thread_state.frame_info = frame_state.frame_info->back;

    ASTInterpreter interpreter(code, vregs, frame_state.frame_info);

    for (const auto& p : *frame_state.locals) {
        if (p.first->cls == int_cls) {
            int vreg = static_cast<BoxedInt*>(p.first)->n;
            interpreter.addSymbol(vreg, p.second, false);
        } else {
            assert(p.first->cls == str_cls);
            auto name = static_cast<BoxedString*>(p.first)->s();
            if (name == PASSED_GENERATOR_NAME) {
                interpreter.setGenerator(p.second);
            } else if (name == PASSED_CLOSURE_NAME) {
                // this should have already got set because its stored in the frame info
                assert(p.second == interpreter.getFrameInfo()->passed_closure);
            } else if (name == CREATED_CLOSURE_NAME) {
                interpreter.setCreatedClosure(p.second);
            } else {
                RELEASE_ASSERT(0, "");
            }
        }
    }

    CFGBlock* start_block = NULL;
    BST_stmt* starting_statement = NULL;
    while (true) {
        if (enclosing_stmt->type == BST_TYPE::Invoke) {
            auto invoke = bst_cast<BST_Invoke>(enclosing_stmt);
            start_block = invoke->normal_dest;
            starting_statement = start_block->body[0];
            enclosing_stmt = invoke->stmt;
        } else if (enclosing_stmt->has_dest_vreg()) {
            int vreg_dst = ((BST_stmt_with_dest*)enclosing_stmt)->vreg_dst;
            if (vreg_dst != VREG_UNDEFINED)
                interpreter.addSymbol(vreg_dst, expr_val, true);
            break;
        } else {
            RELEASE_ASSERT(0, "encountered an yet unimplemented opcode (got %d)", enclosing_stmt->type);
        }
    }

    if (start_block == NULL) {
        // TODO innefficient
        for (auto block : code->source->cfg->blocks) {
            int n = block->body.size();
            for (int i = 0; i < n; i++) {
                if (block->body[i] == enclosing_stmt) {
                    ASSERT(i + 1 < n, "how could we deopt from a non-invoke terminator?");
                    start_block = block;
                    starting_statement = block->body[i + 1];
                    break;
                }
            }

            if (start_block)
                break;
        }

        ASSERT(start_block, "was unable to find the starting block??");
        assert(starting_statement);
    }

    // clear the frame_state now that we have initalized the interpreter with it.
    // this make sure that we don't have unneccessary references around (e.g. could be a problem for PASSED_GENERATOR)
    Py_CLEAR(frame_state.locals);

    Box* v = ASTInterpreter::execute(interpreter, start_block, starting_statement);
    return v ? v : incref(Py_None);
}

extern "C" void printExprHelper(Box* obj) {
    Box* displayhook = PySys_GetObject("displayhook");
    if (!displayhook)
        raiseExcHelper(RuntimeError, "lost sys.displayhook");
    autoDecref(runtimeCall(displayhook, ArgPassSpec(1), obj, 0, 0, 0, 0));
}

static ASTInterpreter* getInterpreterFromFramePtr(void* frame_ptr) {
    // This offsets have to match the layout inside executeInnerAndSetupFrame
    ASTInterpreter** ptr = (ASTInterpreter**)(((uint8_t*)frame_ptr) - 8);
    return *ptr;
}

FrameInfo* getFrameInfoForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = getInterpreterFromFramePtr(frame_ptr);
    assert(interpreter);
    return interpreter->getFrameInfo();
}
}
