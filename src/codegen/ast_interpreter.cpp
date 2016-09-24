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
    Value createFunction(BST_FunctionDef* node, BST_arguments* args);
    Value doBinOp(BST_expr* node, Value left, Value right, int op, BinExpType exp_type);
    void doStore(BST_expr* node, STOLEN(Value) value);
    void doStore(BST_Name* name, STOLEN(Value) value);
    Box* doOSR(BST_Jump* node);
    Value getNone();

    Value visit_assert(BST_Assert* node);
    Value visit_assign(BST_Assign* node);
    Value visit_binop(BST_BinOp* node);
    Value visit_call(BST_Call* node);
    Value visit_compare(BST_Compare* node);
    Value visit_delete(BST_Delete* node);
    Value visit_exec(BST_Exec* node);
    Value visit_print(BST_Print* node);
    Value visit_raise(BST_Raise* node);
    Value visit_return(BST_Return* node);
    Value visit_stmt(BST_stmt* node);
    Value visit_unaryop(BST_UnaryOp* node);

    Value visit_attribute(BST_Attribute* node);
    Value visit_dict(BST_Dict* node);
    Value visit_ellipsis(BST_Ellipsis* node);
    Value visit_expr(BST_expr* node);
    Value visit_expr(BST_Expr* node);
    Value visit_extslice(BST_ExtSlice* node);
    Value visit_index(BST_Index* node);
    Value visit_list(BST_List* node);
    Value visit_name(BST_Name* node);
    Value visit_num(BST_Num* node);
    Value visit_repr(BST_Repr* node);
    Value visit_set(BST_Set* node);
    Value visit_str(BST_Str* node);
    Value visit_subscript(BST_Subscript* node);
    Value visit_slice(BST_Slice* node);
    Value visit_slice(BST_slice* node);
    Value visit_tuple(BST_Tuple* node);
    Value visit_yield(BST_Yield* node);

    Value visit_makeClass(BST_MakeClass* node);
    Value visit_makeFunction(BST_MakeFunction* node);

    // pseudo
    Value visit_augBinOp(BST_AugBinOp* node);
    Value visit_branch(BST_Branch* node);
    Value visit_clsAttribute(BST_ClsAttribute* node);
    Value visit_invoke(BST_Invoke* node);
    Value visit_jump(BST_Jump* node);
    Value visit_langPrimitive(BST_LangPrimitive* node);

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
    const llvm::DenseMap<InternedString, DefaultedInt<-1>>& getSymVRegMap() const {
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
        doStore(name, Value(incref(getArg(i++, arg1, arg2, arg3, args)), 0));
    }

    if (param_names.has_vararg_name)
        doStore(param_names.varArgAsName(), Value(incref(getArg(i++, arg1, arg2, arg3, args)), 0));

    if (param_names.has_kwarg_name) {
        Box* val = getArg(i++, arg1, arg2, arg3, args);
        if (!val)
            val = createDict();
        else
            Py_INCREF(val);
        doStore(param_names.kwArgAsName(), Value(val, 0));
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

        stmt->cxx_exception_count++;
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

Value ASTInterpreter::doBinOp(BST_expr* node, Value left, Value right, int op, BinExpType exp_type) {
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
    return Value();
}

void ASTInterpreter::doStore(BST_Name* node, STOLEN(Value) value) {
    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);

    InternedString name = node->id;
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
                is_live = source_info->getLiveness()->isLiveAtEnd(node->vreg, current_block);
            if (is_live)
                jit->emitSetLocal(node, closure, value);
            else
                jit->emitSetBlockLocal(node, value);
        }

        if (closure) {
            ASTInterpreterJitInterface::setLocalClosureHelper(this, node, value.o);
        } else {
            assert(getVRegInfo().getVReg(node->id) == node->vreg);
            frame_info.num_vregs = std::max(frame_info.num_vregs, node->vreg + 1);
            Box* prev = vregs[node->vreg];
            vregs[node->vreg] = value.o;
            Py_XDECREF(prev);
        }
    }
}

void ASTInterpreter::doStore(BST_expr* node, STOLEN(Value) value) {
    if (node->type == BST_TYPE::Name) {
        BST_Name* name = (BST_Name*)node;
        doStore(name, value);
    } else if (node->type == BST_TYPE::Attribute) {
        BST_Attribute* attr = (BST_Attribute*)node;
        Value o = visit_expr(attr->value);
        if (jit) {
            jit->emitSetAttr(node, o, attr->attr.getBox(), value);
        }
        AUTO_DECREF(o.o);
        pyston::setattr(o.o, attr->attr.getBox(), value.o);
    } else if (node->type == BST_TYPE::Tuple) {
        AUTO_DECREF(value.o);

        BST_Tuple* tuple = (BST_Tuple*)node;
        Box* keep_alive;
        Box** array = unpackIntoArray(value.o, tuple->elts.size(), &keep_alive);
        AUTO_DECREF(keep_alive);

        std::vector<RewriterVar*> array_vars;
        if (jit) {
            array_vars = jit->emitUnpackIntoArray(value, tuple->elts.size());
            assert(array_vars.size() == tuple->elts.size());
        }

        unsigned i = 0;
        for (BST_expr* e : tuple->elts) {
            doStore(e, Value(array[i], jit ? array_vars[i] : NULL));
            ++i;
        }
    } else if (node->type == BST_TYPE::List) {
        AUTO_DECREF(value.o);

        BST_List* list = (BST_List*)node;
        Box* keep_alive;
        Box** array = unpackIntoArray(value.o, list->elts.size(), &keep_alive);
        AUTO_DECREF(keep_alive);

        std::vector<RewriterVar*> array_vars;
        if (jit) {
            array_vars = jit->emitUnpackIntoArray(value, list->elts.size());
            assert(array_vars.size() == list->elts.size());
        }

        unsigned i = 0;
        for (BST_expr* e : list->elts) {
            doStore(e, Value(array[i], jit ? array_vars[i] : NULL));
            ++i;
        }
    } else if (node->type == BST_TYPE::Subscript) {
        AUTO_DECREF(value.o);
        BST_Subscript* subscript = (BST_Subscript*)node;

        Value target = visit_expr(subscript->value);
        AUTO_DECREF(target.o);

        bool is_slice = (subscript->slice->type == BST_TYPE::Slice) && (((BST_Slice*)subscript->slice)->step == NULL);
        if (is_slice) {
            BST_Slice* slice = (BST_Slice*)subscript->slice;
            Value lower = slice->lower ? visit_expr(slice->lower) : Value();
            AUTO_XDECREF(lower.o);
            Value upper = slice->upper ? visit_expr(slice->upper) : Value();
            AUTO_XDECREF(upper.o);
            assert(slice->step == NULL);

            if (jit)
                jit->emitAssignSlice(target, lower, upper, value);
            assignSlice(target.o, lower.o, upper.o, value.o);
        } else {
            Value slice = visit_slice(subscript->slice);

            AUTO_DECREF(slice.o);

            if (jit)
                jit->emitSetItem(target, slice, value);
            setitem(target.o, slice.o, value.o);
        }
    } else {
        RELEASE_ASSERT(0, "not implemented");
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
    Value operand = visit_expr(node->operand);
    AUTO_DECREF(operand.o);
    if (node->op_type == AST_TYPE::Not)
        return Value(boxBool(!nonzero(operand.o)), jit ? jit->emitNotNonzero(operand) : NULL);
    else
        return Value(unaryop(operand.o, node->op_type), jit ? jit->emitUnaryop(operand, node->op_type) : NULL);
}

Value ASTInterpreter::visit_binop(BST_BinOp* node) {
    Value left = visit_expr(node->left);
    AUTO_DECREF(left.o);
    Value right = visit_expr(node->right);
    AUTO_DECREF(right.o);
    return doBinOp(node, left, right, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(BST_slice* node) {
    switch (node->type) {
        case BST_TYPE::ExtSlice:
            return visit_extslice(static_cast<BST_ExtSlice*>(node));
        case BST_TYPE::Ellipsis:
            return visit_ellipsis(static_cast<BST_Ellipsis*>(node));
            break;
        case BST_TYPE::Index:
            return visit_index(static_cast<BST_Index*>(node));
        case BST_TYPE::Slice:
            return visit_slice(static_cast<BST_Slice*>(node));
        default:
            RELEASE_ASSERT(0, "Attempt to handle invalid slice type");
    }
    return Value();
}

Value ASTInterpreter::visit_ellipsis(BST_Ellipsis* node) {
    return Value(incref(Ellipsis), jit ? jit->imm(Ellipsis) : NULL);
}

Value ASTInterpreter::visit_slice(BST_Slice* node) {
    Value lower = node->lower ? visit_expr(node->lower) : getNone();
    AUTO_DECREF(lower.o);
    Value upper = node->upper ? visit_expr(node->upper) : getNone();
    AUTO_DECREF(upper.o);
    Value step = node->step ? visit_expr(node->step) : getNone();
    AUTO_DECREF(step.o);

    Value v;
    if (jit)
        v.var = jit->emitCreateSlice(lower, upper, step);
    v.o = createSlice(lower.o, upper.o, step.o);
    return v;
}

Value ASTInterpreter::visit_extslice(BST_ExtSlice* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    int num_slices = node->dims.size();
    BoxedTuple* rtn = BoxedTuple::create(num_slices);
    for (int i = 0; i < num_slices; ++i) {
        Value v = visit_slice(node->dims[i]);
        rtn->elts[i] = v.o;
        items.push_back(v);
    }

    return Value(rtn, jit ? jit->emitCreateTuple(items) : NULL);
}

Value ASTInterpreter::visit_branch(BST_Branch* node) {
    Value v = visit_expr(node->test);
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

    return Value();
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

    LivenessAnalysis* liveness = source_info->getLiveness();
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

        node->cxx_exception_count++;
        caughtCxxException(&e);

        next_block = node->exc_dest;
        last_exception = e;
    }

    return v;
}

Value ASTInterpreter::visit_clsAttribute(BST_ClsAttribute* node) {
    Value obj = visit_expr(node->value);
    BoxedString* attr = node->attr.getBox();
    AUTO_DECREF(obj.o);
    return Value(getclsattr(obj.o, attr), jit ? jit->emitGetClsAttr(obj, attr) : NULL);
}

Value ASTInterpreter::visit_augBinOp(BST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = visit_expr(node->left);
    AUTO_DECREF(left.o);
    Value right = visit_expr(node->right);
    AUTO_DECREF(right.o);
    return doBinOp(node, left, right, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_langPrimitive(BST_LangPrimitive* node) {
    Value v;
    if (node->opcode == BST_LangPrimitive::GET_ITER) {
        assert(node->args.size() == 1);
        Value val = visit_expr(node->args[0]);
        AUTO_DECREF(val.o);
        v = Value(getPystonIter(val.o), jit ? jit->emitGetPystonIter(val) : NULL);
    } else if (node->opcode == BST_LangPrimitive::IMPORT_FROM) {
        assert(node->args.size() == 2);
        assert(node->args[0]->type == BST_TYPE::Name);
        assert(node->args[1]->type == BST_TYPE::Str);

        Value module = visit_expr(node->args[0]);
        AUTO_DECREF(module.o);

        auto ast_str = bst_cast<BST_Str>(node->args[1]);
        assert(ast_str->str_type == AST_Str::STR);
        const std::string& name = ast_str->str_data;
        assert(name.size());
        BORROWED(BoxedString*)name_boxed = source_info->parent_module->getStringConstant(name, true);

        if (jit)
            v.var = jit->emitImportFrom(module, name_boxed);
        v.o = importFrom(module.o, name_boxed);
    } else if (node->opcode == BST_LangPrimitive::IMPORT_NAME) {
        assert(node->args.size() == 3);
        assert(node->args[0]->type == BST_TYPE::Num);
        assert(static_cast<BST_Num*>(node->args[0])->num_type == AST_Num::INT);
        assert(node->args[2]->type == BST_TYPE::Str);

        int level = static_cast<BST_Num*>(node->args[0])->n_int;
        Value froms = visit_expr(node->args[1]);
        AUTO_DECREF(froms.o);
        auto ast_str = bst_cast<BST_Str>(node->args[2]);
        assert(ast_str->str_type == AST_Str::STR);
        const std::string& module_name = ast_str->str_data;
        if (jit)
            v.var = jit->emitImportName(level, froms, module_name);
        v.o = import(level, froms.o, module_name);
    } else if (node->opcode == BST_LangPrimitive::IMPORT_STAR) {
        assert(node->args.size() == 1);
        assert(node->args[0]->type == BST_TYPE::Name);

        RELEASE_ASSERT(source_info->ast_type == BST_TYPE::Module || source_info->ast_type == BST_TYPE::Suite,
                       "import * not supported in functions");

        Value module = visit_expr(node->args[0]);
        AUTO_DECREF(module.o);
        v = Value(importStar(module.o, frame_info.globals), jit ? jit->emitImportStar(module) : NULL);
    } else if (node->opcode == BST_LangPrimitive::NONE) {
        v = getNone();
    } else if (node->opcode == BST_LangPrimitive::LANDINGPAD) {
        assert(last_exception.type);
        v = Value(ASTInterpreterJitInterface::landingpadHelper(this), jit ? jit->emitLandingpad() : NULL);
    } else if (node->opcode == BST_LangPrimitive::CHECK_EXC_MATCH) {
        assert(node->args.size() == 2);
        Value obj = visit_expr(node->args[0]);
        AUTO_DECREF(obj.o);
        Value cls = visit_expr(node->args[1]);
        AUTO_DECREF(cls.o);
        v = Value(boxBool(exceptionMatches(obj.o, cls.o)), jit ? jit->emitExceptionMatches(obj, cls) : NULL);
    } else if (node->opcode == BST_LangPrimitive::LOCALS) {
        assert(frame_info.boxedLocals != NULL);
        v = Value(incref(frame_info.boxedLocals), jit ? jit->emitGetBoxedLocals() : NULL);
    } else if (node->opcode == BST_LangPrimitive::NONZERO) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        AUTO_DECREF(obj.o);
        v = Value(boxBool(nonzero(obj.o)), jit ? jit->emitNonzero(obj) : NULL);
    } else if (node->opcode == BST_LangPrimitive::SET_EXC_INFO) {
        assert(node->args.size() == 3);

        Value type = visit_expr(node->args[0]);
        assert(type.o);
        Value value = visit_expr(node->args[1]);
        assert(value.o);
        Value traceback = visit_expr(node->args[2]);
        assert(traceback.o);

        if (jit)
            jit->emitSetExcInfo(type, value, traceback);
        setFrameExcInfo(getFrameInfo(), type.o, value.o, traceback.o);
        v = getNone();
    } else if (node->opcode == BST_LangPrimitive::UNCACHE_EXC_INFO) {
        assert(node->args.empty());
        if (jit)
            jit->emitUncacheExcInfo();
        setFrameExcInfo(getFrameInfo(), NULL, NULL, NULL);
        v = getNone();
    } else if (node->opcode == BST_LangPrimitive::HASNEXT) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        AUTO_DECREF(obj.o);
        v = Value(boxBool(hasnext(obj.o)), jit ? jit->emitHasnext(obj) : NULL);
    } else if (node->opcode == BST_LangPrimitive::PRINT_EXPR) {
        abortJITing();
        Value obj = visit_expr(node->args[0]);
        AUTO_DECREF(obj.o);
        printExprHelper(obj.o);
        v = getNone();
    } else
        RELEASE_ASSERT(0, "unknown opcode %d", node->opcode);
    return v;
}

Value ASTInterpreter::visit_yield(BST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : getNone();
    return Value(ASTInterpreterJitInterface::yieldHelper(this, value.o), jit ? jit->emitYield(value) : NULL);
}

Value ASTInterpreter::visit_stmt(BST_stmt* node) {
#if ENABLE_SAMPLING_PROFILER
    threading::allowGLReadPreemption();
#endif

    if (0) {
        printf("%20s % 2d ", getCode()->name->c_str(), current_block->idx);
        print_bst(node);
        printf("\n");
    }

    Value rtn;
    switch (node->type) {
        case BST_TYPE::Assert:
            rtn = visit_assert((BST_Assert*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Assign:
            rtn = visit_assign((BST_Assign*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Delete:
            rtn = visit_delete((BST_Delete*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Exec:
            rtn = visit_exec((BST_Exec*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Expr:
            // docstrings are str constant expression statements.
            // ignore those while interpreting.
            if ((((BST_Expr*)node)->value)->type != BST_TYPE::Str) {
                rtn = visit_expr((BST_Expr*)node);
                Py_DECREF(rtn.o);
                rtn = Value();
                ASTInterpreterJitInterface::pendingCallsCheckHelper();
            }
            break;
        case BST_TYPE::Pass:
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break; // nothing todo
        case BST_TYPE::Print:
            rtn = visit_print((BST_Print*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Raise:
            rtn = visit_raise((BST_Raise*)node);
            ASTInterpreterJitInterface::pendingCallsCheckHelper();
            break;
        case BST_TYPE::Return:
            rtn = visit_return((BST_Return*)node);
            try {
                ASTInterpreterJitInterface::pendingCallsCheckHelper();
            } catch (ExcInfo e) {
                Py_DECREF(rtn.o);
                throw e;
            }
            return rtn;

        // pseudo
        case BST_TYPE::Branch:
            rtn = visit_branch((BST_Branch*)node);
            break;
        case BST_TYPE::Jump:
            rtn = visit_jump((BST_Jump*)node);
            return rtn;
        case BST_TYPE::Invoke:
            rtn = visit_invoke((BST_Invoke*)node);
            break;
        default:
            RELEASE_ASSERT(0, "not implemented");
    };

    // This assertion tries to make sure that we are refcount-safe if an exception
    // is thrown from pendingCallsCheckHelper.  Any statement that returns a value needs
    // to be careful to wrap pendingCallsCheckHelper, and it can signal that it was careful
    // by returning from the function instead of breaking.
    assert(!rtn.o);

    return rtn;
}

Value ASTInterpreter::visit_return(BST_Return* node) {
    Value s = node->value ? visit_expr(node->value) : getNone();

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

Value ASTInterpreter::createFunction(BST_FunctionDef* node, BST_arguments* args) {
    BoxedCode* code = node->code;
    assert(code);

    std::vector<Box*> defaults;
    llvm::SmallVector<RewriterVar*, 4> defaults_vars;
    defaults.reserve(args->defaults.size());

    RewriterVar* defaults_var = NULL;
    if (jit) {
        defaults_var = args->defaults.size() ? jit->allocate(args->defaults.size()) : jit->imm(0ul);
        defaults_vars.reserve(args->defaults.size());
    }

    int i = 0;
    for (BST_expr* d : args->defaults) {
        Value v = visit_expr(d);
        defaults.push_back(v.o);
        if (jit) {
            defaults_var->setAttr(i++ * sizeof(void*), v, RewriterVar::SetattrType::REF_USED);
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
                                                                        defaults_var, jit->imm(args->defaults.size()) },
                            {}, defaults_vars)->setType(RefType::OWNED);
    }

    rtn.o = createFunctionFromMetadata(code, closure, passed_globals, u.il);

    return rtn;
}

Value ASTInterpreter::visit_makeFunction(BST_MakeFunction* mkfn) {
    BST_FunctionDef* node = mkfn->function_def;
    BST_arguments* args = node->args;

    std::vector<Value> decorators;
    decorators.reserve(node->decorator_list.size());
    for (BST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d));

    Value func = createFunction(node, args);

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
    BST_ClassDef* node = mkclass->class_def;


    BoxedTuple* basesTuple = BoxedTuple::create(node->bases.size());
    AUTO_DECREF(basesTuple);
    int base_idx = 0;
    for (BST_expr* b : node->bases) {
        basesTuple->elts[base_idx++] = visit_expr(b).o;
    }

    std::vector<Box*> decorators;
    decorators.reserve(node->decorator_list.size());
    for (BST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    BoxedCode* code = node->code;
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

    Box* classobj = createUserClass(node->name.getBox(), basesTuple, attrDict);

    for (int i = decorators.size() - 1; i >= 0; i--) {
        classobj = runtimeCall(autoDecref(decorators[i]), ArgPassSpec(1), autoDecref(classobj), 0, 0, 0, 0);
    }

    return Value(classobj, NULL);
}

Value ASTInterpreter::visit_raise(BST_Raise* node) {
    if (node->arg0 == NULL) {
        assert(!node->arg1);
        assert(!node->arg2);

        if (jit) {
            jit->emitRaise0();
            finishJITing();
        }

        ASTInterpreterJitInterface::raise0Helper(this);
    }

    Value arg0 = node->arg0 ? visit_expr(node->arg0) : getNone();
    Value arg1 = node->arg1 ? visit_expr(node->arg1) : getNone();
    Value arg2 = node->arg2 ? visit_expr(node->arg2) : getNone();

    if (jit) {
        jit->emitRaise3(arg0, arg1, arg2);
        finishJITing();
    }

    raise3(arg0.o, arg1.o, arg2.o);
    return Value();
}

Value ASTInterpreter::visit_assert(BST_Assert* node) {
    abortJITing();
#ifndef NDEBUG
    // Currently we only generate "assert 0" statements
    Value v = visit_expr(node->test);
    assert(v.o->cls == int_cls && static_cast<BoxedInt*>(v.o)->n == 0);
    Py_DECREF(v.o);
#endif

    static BoxedString* AssertionError_str = getStaticString("AssertionError");
    Box* assertion_type = getGlobal(frame_info.globals, AssertionError_str);
    AUTO_DECREF(assertion_type);
    Box* msg = node->msg ? visit_expr(node->msg).o : 0;
    AUTO_XDECREF(msg);
    assertFail(assertion_type, msg);

    return Value();
}

Value ASTInterpreter::visit_delete(BST_Delete* node) {
    BST_expr* target_ = node->target;
    switch (target_->type) {
        case BST_TYPE::Subscript: {
            BST_Subscript* sub = (BST_Subscript*)target_;
            Value value = visit_expr(sub->value);
            AUTO_DECREF(value.o);

            bool is_slice = (sub->slice->type == BST_TYPE::Slice) && (((BST_Slice*)sub->slice)->step == NULL);
            if (is_slice) {
                BST_Slice* slice = (BST_Slice*)sub->slice;
                Value lower = slice->lower ? visit_expr(slice->lower) : Value();
                AUTO_XDECREF(lower.o);
                Value upper = slice->upper ? visit_expr(slice->upper) : Value();
                AUTO_XDECREF(upper.o);
                assert(slice->step == NULL);

                if (jit)
                    jit->emitAssignSlice(value, lower, upper, jit->imm(0ul));
                assignSlice(value.o, lower.o, upper.o, NULL);
            } else {
                Value slice = visit_slice(sub->slice);
                AUTO_DECREF(slice.o);
                if (jit)
                    jit->emitDelItem(value, slice);
                delitem(value.o, slice.o);
            }
            break;
        }
        case BST_TYPE::Attribute: {
            BST_Attribute* attr = (BST_Attribute*)target_;
            Value target = visit_expr(attr->value);
            AUTO_DECREF(target.o);
            BoxedString* str = attr->attr.getBox();
            if (jit)
                jit->emitDelAttr(target, str);
            delattr(target.o, str);
            break;
        }
        case BST_TYPE::Name: {
            BST_Name* target = (BST_Name*)target_;
            assert(target->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);

            ScopeInfo::VarScopeType vst = target->lookup_type;
            if (vst == ScopeInfo::VarScopeType::GLOBAL) {
                if (jit)
                    jit->emitDelGlobal(target->id.getBox());
                delGlobal(frame_info.globals, target->id.getBox());
                break;
            } else if (vst == ScopeInfo::VarScopeType::NAME) {
                if (jit)
                    jit->emitDelName(target->id);
                ASTInterpreterJitInterface::delNameHelper(this, target->id);
            } else {
                assert(vst == ScopeInfo::VarScopeType::FAST);

                assert(getVRegInfo().getVReg(target->id) == target->vreg);

                if (target->id.s()[0] == '#') {
                    assert(vregs[target->vreg] != NULL);
                    if (jit)
                        jit->emitKillTemporary(target);
                } else {
                    abortJITing();
                    if (vregs[target->vreg] == 0) {
                        assertNameDefined(0, target->id.c_str(), NameError, true /* local_var_msg */);
                        return Value();
                    }
                }

                frame_info.num_vregs = std::max(frame_info.num_vregs, target->vreg + 1);
                Py_DECREF(vregs[target->vreg]);
                vregs[target->vreg] = NULL;
            }
            break;
        }
        default:
            ASSERT(0, "Unsupported del target: %d", target_->type);
            abort();
    }
    return Value();
}

Value ASTInterpreter::visit_assign(BST_Assign* node) {
    Value v = visit_expr(node->value);
    doStore(node->target, v);
    return Value();
}

Value ASTInterpreter::visit_print(BST_Print* node) {
    Value dest = node->dest ? visit_expr(node->dest) : Value();
    Value var = node->value ? visit_expr(node->value) : Value();

    if (jit)
        jit->emitPrint(dest, var, node->nl);

    if (node->dest)
        printHelper(autoDecref(dest.o), autoXDecref(var.o), node->nl);
    else
        printHelper(NULL, autoXDecref(var.o), node->nl);

    return Value();
}

Value ASTInterpreter::visit_exec(BST_Exec* node) {
    // TODO implement the locals and globals arguments
    Value code = visit_expr(node->body);
    AUTO_DECREF(code.o);
    Value globals = node->globals == NULL ? Value() : visit_expr(node->globals);
    AUTO_XDECREF(globals.o);
    Value locals = node->locals == NULL ? Value() : visit_expr(node->locals);
    AUTO_XDECREF(locals.o);

    if (jit)
        jit->emitExec(code, globals, locals, this->source_info->future_flags);
    exec(code.o, globals.o, locals.o, this->source_info->future_flags);

    return Value();
}

Value ASTInterpreter::visit_compare(BST_Compare* node) {
    Value left = visit_expr(node->left);
    AUTO_DECREF(left.o);
    Value right = visit_expr(node->comparator);
    AUTO_DECREF(right.o);
    return doBinOp(node, left, right, node->op, BinExpType::Compare);
}

Value ASTInterpreter::visit_expr(BST_expr* node) {
    switch (node->type) {
        case BST_TYPE::Attribute:
            return visit_attribute((BST_Attribute*)node);
        case BST_TYPE::BinOp:
            return visit_binop((BST_BinOp*)node);
        case BST_TYPE::Call:
            return visit_call((BST_Call*)node);
        case BST_TYPE::Compare:
            return visit_compare((BST_Compare*)node);
        case BST_TYPE::Dict:
            return visit_dict((BST_Dict*)node);
        case BST_TYPE::List:
            return visit_list((BST_List*)node);
        case BST_TYPE::Name:
            return visit_name((BST_Name*)node);
        case BST_TYPE::Num:
            return visit_num((BST_Num*)node);
        case BST_TYPE::Repr:
            return visit_repr((BST_Repr*)node);
        case BST_TYPE::Set:
            return visit_set((BST_Set*)node);
        case BST_TYPE::Str:
            return visit_str((BST_Str*)node);
        case BST_TYPE::Subscript:
            return visit_subscript((BST_Subscript*)node);
        case BST_TYPE::Tuple:
            return visit_tuple((BST_Tuple*)node);
        case BST_TYPE::UnaryOp:
            return visit_unaryop((BST_UnaryOp*)node);
        case BST_TYPE::Yield:
            return visit_yield((BST_Yield*)node);

        // pseudo
        case BST_TYPE::AugBinOp:
            return visit_augBinOp((BST_AugBinOp*)node);
        case BST_TYPE::ClsAttribute:
            return visit_clsAttribute((BST_ClsAttribute*)node);
        case BST_TYPE::LangPrimitive:
            return visit_langPrimitive((BST_LangPrimitive*)node);
        case BST_TYPE::MakeClass:
            return visit_makeClass((BST_MakeClass*)node);
        case BST_TYPE::MakeFunction:
            return visit_makeFunction((BST_MakeFunction*)node);
        default:
            RELEASE_ASSERT(0, "");
    };
    return Value();
}


Value ASTInterpreter::visit_call(BST_Call* node) {
    Value v;
    Value func;

    InternedString attr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    if (node->func->type == BST_TYPE::Attribute) {
        is_callattr = true;
        callattr_clsonly = false;
        BST_Attribute* attr_ast = bst_cast<BST_Attribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = attr_ast->attr;
    } else if (node->func->type == BST_TYPE::ClsAttribute) {
        is_callattr = true;
        callattr_clsonly = true;
        BST_ClsAttribute* attr_ast = bst_cast<BST_ClsAttribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = attr_ast->attr;
    } else {
        func = visit_expr(node->func);
    }

    AUTO_DECREF(func.o);

    llvm::SmallVector<Box*, 8> args;
    llvm::SmallVector<RewriterVar*, 8> args_vars;
    args.reserve(node->args.size());
    args_vars.reserve(node->args.size());

    for (BST_expr* e : node->args) {
        Value v = visit_expr(e);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    std::vector<BoxedString*>* keyword_names = NULL;
    if (node->keywords.size())
        keyword_names = getKeywordNameStorage(node);

    for (BST_keyword* k : node->keywords) {
        Value v = visit_expr(k->value);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    if (node->starargs) {
        Value v = visit_expr(node->starargs);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    if (node->kwargs) {
        Value v = visit_expr(node->kwargs);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    AUTO_DECREF_ARRAY(args.data(), args.size());

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

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


Value ASTInterpreter::visit_expr(BST_Expr* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_num(BST_Num* node) {
    Box* o = NULL;
    if (node->num_type == AST_Num::INT) {
        o = parent_module->getIntConstant(node->n_int);
    } else if (node->num_type == AST_Num::FLOAT) {
        o = parent_module->getFloatConstant(node->n_float);
    } else if (node->num_type == AST_Num::LONG) {
        o = parent_module->getLongConstant(node->n_long);
    } else if (node->num_type == AST_Num::COMPLEX) {
        o = parent_module->getPureImaginaryConstant(node->n_float);
    } else
        RELEASE_ASSERT(0, "not implemented");
    Py_INCREF(o);
    RewriterVar* v = NULL;
    if (jit) {
        v = jit->imm(o)->setType(RefType::BORROWED);
    }
    return Value(o, v);
}

Value ASTInterpreter::visit_index(BST_Index* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_repr(BST_Repr* node) {
    Value v = visit_expr(node->value);
    AUTO_DECREF(v.o);
    return Value(repr(v.o), jit ? jit->emitRepr(v) : NULL);
}

Value ASTInterpreter::visit_dict(BST_Dict* node) {
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");

    BoxedDict* dict = new BoxedDict();
    RewriterVar* r_dict = jit ? jit->emitCreateDict() : NULL;
    for (size_t i = 0; i < node->keys.size(); ++i) {
        Value v = visit_expr(node->values[i]);
        Value k = visit_expr(node->keys[i]);

        dictSetInternal(dict, k.o, v.o);
        if (jit) {
            jit->emitDictSet(r_dict, k, v);
        }
    }

    return Value(dict, r_dict);
}

Value ASTInterpreter::visit_set(BST_Set* node) {
    BoxedSet* set = (BoxedSet*)createSet();
    try {
        // insert the elements in reverse like cpython does
        // important for {1, 1L}
        llvm::SmallVector<RewriterVar*, 8> items;
        for (auto it = node->elts.rbegin(), it_end = node->elts.rend(); it != it_end; ++it) {
            Value v = visit_expr(*it);
            _setAddStolen(set, v.o);
            items.push_back(v);
        }
        return Value(set, jit ? jit->emitCreateSet(items) : NULL);
    } catch (ExcInfo e) {
        Py_DECREF(set);
        throw e;
    }
}

Value ASTInterpreter::visit_str(BST_Str* node) {
    Box* o = NULL;
    if (node->str_type == AST_Str::STR) {
        o = parent_module->getStringConstant(node->str_data, true);
    } else if (node->str_type == AST_Str::UNICODE) {
        o = parent_module->getUnicodeConstant(node->str_data);
    } else {
        RELEASE_ASSERT(0, "%d", node->str_type);
    }
    Py_INCREF(o);
    return Value(o, jit ? jit->imm(o)->setType(RefType::BORROWED) : NULL);
}

Value ASTInterpreter::visit_name(BST_Name* node) {
    assert(node->lookup_type != ScopeInfo::VarScopeType::UNKNOWN);

    switch (node->lookup_type) {
        case ScopeInfo::VarScopeType::GLOBAL: {
            assert(!node->is_kill);
            Value v;
            if (jit)
                v.var = jit->emitGetGlobal(node->id.getBox());

            v.o = getGlobal(frame_info.globals, node->id.getBox());
            return v;
        }
        case ScopeInfo::VarScopeType::DEREF: {
            assert(!node->is_kill);
            return Value(ASTInterpreterJitInterface::derefHelper(this, node), jit ? jit->emitDeref(node) : NULL);
        }
        case ScopeInfo::VarScopeType::FAST:
        case ScopeInfo::VarScopeType::CLOSURE: {
            Value v;
            if (jit) {
                bool is_live = true;
                if (node->is_kill) {
                    is_live = false;
                } else if (node->lookup_type == ScopeInfo::VarScopeType::FAST) {
                    assert(node->vreg != -1);
                    is_live = source_info->getLiveness()->isLiveAtEnd(node->vreg, current_block);
                }

                if (is_live) {
                    assert(!node->is_kill);
                    v.var = jit->emitGetLocal(node);
                } else {
                    v.var = jit->emitGetBlockLocal(node);
                    if (node->is_kill) {
                        assert(node->id.s()[0] == '#');
                        jit->emitKillTemporary(node);
                    }
                }
            }

            assert(node->vreg >= 0);
            assert(getVRegInfo().getVReg(node->id) == node->vreg);
            frame_info.num_vregs = std::max(frame_info.num_vregs, node->vreg + 1);
            Box* val = vregs[node->vreg];

            if (val) {
                v.o = val;
                if (node->is_kill)
                    vregs[node->vreg] = NULL;
                else
                    Py_INCREF(val);
                return v;
            }

            assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
            RELEASE_ASSERT(0, "should be unreachable");
        }
        case ScopeInfo::VarScopeType::NAME: {
            assert(!node->is_kill && "we might need to support this");
            Value v;
            if (jit)
                v.var = jit->emitGetBoxedLocal(node->id.getBox());
            v.o = boxedLocalsGet(frame_info.boxedLocals, node->id.getBox(), frame_info.globals);
            return v;
        }
        default:
            abort();
    }
}

Value ASTInterpreter::visit_subscript(BST_Subscript* node) {
    Value value = visit_expr(node->value);
    AUTO_DECREF(value.o);

    bool is_slice = (node->slice->type == BST_TYPE::Slice) && (((BST_Slice*)node->slice)->step == NULL);
    if (is_slice) {
        BST_Slice* slice = (BST_Slice*)node->slice;
        Value lower = slice->lower ? visit_expr(slice->lower) : Value();
        AUTO_XDECREF(lower.o);
        Value upper = slice->upper ? visit_expr(slice->upper) : Value();
        AUTO_XDECREF(upper.o);
        assert(slice->step == NULL);

        Value v;
        if (jit)
            v.var = jit->emitApplySlice(value, lower, upper);
        v.o = applySlice(value.o, lower.o, upper.o);
        return v;
    } else {
        Value slice = visit_slice(node->slice);
        AUTO_DECREF(slice.o);

        Value v;
        if (jit)
            v.var = jit->emitGetItem(node, value, slice);
        v.o = getitem(value.o, slice.o);
        return v;
    }
}

Value ASTInterpreter::visit_list(BST_List* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    BoxedList* list = new BoxedList();
    list->ensure(node->elts.size());
    for (BST_expr* e : node->elts) {
        try {
            Value v = visit_expr(e);
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

    BoxedTuple* rtn = BoxedTuple::create(node->elts.size());
    int rtn_idx = 0;
    for (BST_expr* e : node->elts) {
        Value v = visit_expr(e);
        rtn->elts[rtn_idx++] = v.o;
        items.push_back(v);
    }

    return Value(rtn, jit ? jit->emitCreateTuple(items) : NULL);
}

Value ASTInterpreter::visit_attribute(BST_Attribute* node) {
    Value v = visit_expr(node->value);
    AUTO_DECREF(v.o);
    Value r(pyston::getattr(v.o, node->attr.getBox()), jit ? jit->emitGetAttr(v, node->attr.getBox(), node) : NULL);
    return r;
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

Box* ASTInterpreterJitInterface::derefHelper(void* _interpreter, BST_Name* node) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    DerefInfo deref_info = interpreter->scope_info.getDerefInfo(node);
    assert(interpreter->getPassedClosure());
    BoxedClosure* closure = interpreter->getPassedClosure();
    for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
        closure = closure->parent;
    }
    Box* val = closure->elts[deref_info.offset];
    if (val == NULL) {
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope",
                       node->id.c_str());
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

void ASTInterpreterJitInterface::setLocalClosureHelper(void* _interpreter, BST_Name* name, Box* v) {
    assert(name->lookup_type == ScopeInfo::VarScopeType::CLOSURE);

    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    auto vreg = name->vreg;
    interpreter->frame_info.num_vregs = std::max(interpreter->frame_info.num_vregs, (int)vreg + 1);
    Box* prev = interpreter->vregs[vreg];
    interpreter->vregs[vreg] = v;
    auto closure_offset = interpreter->scope_info.getClosureOffset(name);
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
extern "C" Box* astInterpretDeoptFromASM(BoxedCode* code, BST_expr* after_expr, BST_stmt* enclosing_stmt, Box* expr_val,
                                         STOLEN(FrameStackState) frame_state) {
    static_assert(sizeof(FrameStackState) <= 2 * 8, "astInterpretDeopt assumes that all args get passed in regs!");

    assert(code);
    assert(enclosing_stmt);
    assert(frame_state.locals);
    assert(after_expr);
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
        if (enclosing_stmt->type == BST_TYPE::Assign) {
            auto asgn = bst_cast<BST_Assign>(enclosing_stmt);
            RELEASE_ASSERT(asgn->value == after_expr, "%p %p", asgn->value, after_expr);
            assert(asgn->target->type == BST_TYPE::Name);
            auto name = bst_cast<BST_Name>(asgn->target);
            assert(name->id.s()[0] == '#');
            interpreter.addSymbol(name->vreg, expr_val, true);
            break;
        } else if (enclosing_stmt->type == BST_TYPE::Expr) {
            auto expr = bst_cast<BST_Expr>(enclosing_stmt);
            RELEASE_ASSERT(expr->value == after_expr, "%p %p", expr->value, after_expr);
            assert(expr->value == after_expr);
            break;
        } else if (enclosing_stmt->type == BST_TYPE::Invoke) {
            auto invoke = bst_cast<BST_Invoke>(enclosing_stmt);
            start_block = invoke->normal_dest;
            starting_statement = start_block->body[0];
            enclosing_stmt = invoke->stmt;
        } else {
            RELEASE_ASSERT(0, "should not be able to reach here with anything other than an Assign (got %d)",
                           enclosing_stmt->type);
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
