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
#include "core/ast.h"
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

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

namespace pyston {

namespace {

static BoxedClass* astinterpreter_cls;

class ASTInterpreter;

// Map from stack frame pointers for frames corresponding to ASTInterpreter::execute() to the ASTInterpreter handling
// them. Used to look up information about that frame. This is used for getting tracebacks, for CPython introspection
// (sys._getframe & co), and for GC scanning.
static std::unordered_map<void*, ASTInterpreter*> s_interpreterMap;
static_assert(THREADING_USE_GIL, "have to make the interpreter map thread safe!");

class RegisterHelper {
private:
    void* frame_addr;
    ASTInterpreter* interpreter;

public:
    RegisterHelper();
    ~RegisterHelper();
    void doRegister(void* frame_addr, ASTInterpreter* interpreter);
    static void deregister(void* frame_addr);
};

class ASTInterpreter : public Box {
public:
    typedef ContiguousMap<InternedString, Box*, llvm::SmallDenseMap<InternedString, int, 16>> SymMap;

    ASTInterpreter(CLFunction* clfunc);

    void initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args);

    static Value execute(ASTInterpreter& interpreter, CFGBlock* start_block = NULL, AST_stmt* start_at = NULL);
    // This must not be inlined, because we rely on being able to detect when we're inside of it (by checking whether
    // %rip is inside its instruction range) during a stack-trace in order to produce tracebacks inside interpreted
    // code.
    __attribute__((__no_inline__)) __attribute__((noinline)) static Value
        executeInner(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at, RegisterHelper* reg);

private:
    Box* createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body);
    Value doBinOp(Value left, Value right, int op, BinExpType exp_type);
    void doStore(AST_expr* node, Value value);
    void doStore(InternedString name, Value value);
    Box* doOSR(AST_Jump* node);
    Value getNone();

    Value visit_assert(AST_Assert* node);
    Value visit_assign(AST_Assign* node);
    Value visit_binop(AST_BinOp* node);
    Value visit_call(AST_Call* node);
    Value visit_compare(AST_Compare* node);
    Value visit_delete(AST_Delete* node);
    Value visit_exec(AST_Exec* node);
    Value visit_global(AST_Global* node);
    Value visit_module(AST_Module* node);
    Value visit_print(AST_Print* node);
    Value visit_raise(AST_Raise* node);
    Value visit_return(AST_Return* node);
    Value visit_stmt(AST_stmt* node);
    Value visit_unaryop(AST_UnaryOp* node);

    Value visit_attribute(AST_Attribute* node);
    Value visit_dict(AST_Dict* node);
    Value visit_expr(AST_expr* node);
    Value visit_expr(AST_Expr* node);
    Value visit_extslice(AST_ExtSlice* node);
    Value visit_index(AST_Index* node);
    Value visit_lambda(AST_Lambda* node);
    Value visit_list(AST_List* node);
    Value visit_name(AST_Name* node);
    Value visit_num(AST_Num* node);
    Value visit_repr(AST_Repr* node);
    Value visit_set(AST_Set* node);
    Value visit_slice(AST_Slice* node);
    Value visit_str(AST_Str* node);
    Value visit_subscript(AST_Subscript* node);
    Value visit_tuple(AST_Tuple* node);
    Value visit_yield(AST_Yield* node);

    Value visit_makeClass(AST_MakeClass* node);
    Value visit_makeFunction(AST_MakeFunction* node);

    // pseudo
    Value visit_augBinOp(AST_AugBinOp* node);
    Value visit_branch(AST_Branch* node);
    Value visit_clsAttribute(AST_ClsAttribute* node);
    Value visit_invoke(AST_Invoke* node);
    Value visit_jump(AST_Jump* node);
    Value visit_langPrimitive(AST_LangPrimitive* node);

    // for doc on 'exit_offset' have a look at JitFragmentWriter::num_bytes_exit and num_bytes_overlapping
    void startJITing(CFGBlock* block, int exit_offset = 0);
    void abortJITing();
    void finishJITing(CFGBlock* continue_block = NULL);
    // This method is not allowed to get inlined into 'executeInner' otherwise tracebacks are wrong.
    __attribute__((__no_inline__)) __attribute__((noinline)) Box* execJITedBlock(CFGBlock* b);

    // this variables are used by the baseline JIT, make sure they have an offset < 0x80 so we can use shorter
    // instructions
    CFGBlock* next_block, *current_block;
    AST_stmt* current_inst;

    CLFunction* clfunc;
    SourceInfo* source_info;
    ScopeInfo* scope_info;
    PhiAnalysis* phis;

    SymMap sym_table;
    ExcInfo last_exception;
    BoxedClosure* passed_closure, *created_closure;
    BoxedGenerator* generator;
    unsigned edgecount;
    FrameInfo frame_info;

    // This is either a module or a dict
    Box* globals;
    void* frame_addr; // used to clear entry inside the s_interpreterMap on destruction
    std::unique_ptr<JitFragmentWriter> jit;

public:
    DEFAULT_CLASS_SIMPLE(astinterpreter_cls);

    AST_stmt* getCurrentStatement() {
        assert(current_inst);
        return current_inst;
    }

    Box* getGlobals() {
        assert(globals);
        return globals;
    }

    CLFunction* getCL() { return clfunc; }
    FrameInfo* getFrameInfo() { return &frame_info; }
    BoxedClosure* getPassedClosure() { return passed_closure; }
    const SymMap& getSymbolTable() { return sym_table; }
    const ScopeInfo* getScopeInfo() { return scope_info; }

    void addSymbol(InternedString name, Box* value, bool allow_duplicates);
    void setGenerator(Box* gen);
    void setPassedClosure(Box* closure);
    void setCreatedClosure(Box* closure);
    void setBoxedLocals(Box*);
    void setFrameInfo(const FrameInfo* frame_info);
    void setGlobals(Box* globals);

    static void gcHandler(GCVisitor* visitor, Box* box);
    static void simpleDestructor(Box* box) {
        ASTInterpreter* inter = (ASTInterpreter*)box;
        assert(inter->cls == astinterpreter_cls);
        if (inter->frame_addr)
            RegisterHelper::deregister(inter->frame_addr);
        inter->~ASTInterpreter();
    }

    friend class RegisterHelper;
    friend struct pyston::ASTInterpreterJitInterface;
};

void ASTInterpreter::addSymbol(InternedString name, Box* value, bool allow_duplicates) {
    if (!allow_duplicates)
        assert(sym_table.count(name) == 0);
    sym_table[name] = value;
}

void ASTInterpreter::setGenerator(Box* gen) {
    assert(!this->generator); // This should only used for initialization
    assert(gen->cls == generator_cls);
    this->generator = static_cast<BoxedGenerator*>(gen);
}

void ASTInterpreter::setPassedClosure(Box* closure) {
    assert(!this->passed_closure); // This should only used for initialization
    assert(closure->cls == closure_cls);
    this->passed_closure = static_cast<BoxedClosure*>(closure);
}

void ASTInterpreter::setCreatedClosure(Box* closure) {
    assert(!this->created_closure); // This should only used for initialization
    assert(closure->cls == closure_cls);
    this->created_closure = static_cast<BoxedClosure*>(closure);
}

void ASTInterpreter::setBoxedLocals(Box* boxedLocals) {
    this->frame_info.boxedLocals = boxedLocals;
}

void ASTInterpreter::setFrameInfo(const FrameInfo* frame_info) {
    this->frame_info = *frame_info;
}

void ASTInterpreter::setGlobals(Box* globals) {
    assert(gc::isValidGCObject(globals));
    this->globals = globals;
}

void ASTInterpreter::gcHandler(GCVisitor* visitor, Box* box) {
    boxGCHandler(visitor, box);

    ASTInterpreter* interp = (ASTInterpreter*)box;
    auto&& vec = interp->sym_table.vector();
    visitor->visitRange((void* const*)&vec[0], (void* const*)&vec[interp->sym_table.size()]);
    visitor->visit(interp->passed_closure);
    visitor->visit(interp->created_closure);
    visitor->visit(interp->generator);
    visitor->visit(interp->globals);
    visitor->visit(interp->source_info->parent_module);
    interp->frame_info.gcVisit(visitor);
}

ASTInterpreter::ASTInterpreter(CLFunction* clfunc)
    : current_block(0),
      current_inst(0),
      clfunc(clfunc),
      source_info(clfunc->source.get()),
      scope_info(0),
      phis(NULL),
      last_exception(NULL, NULL, NULL),
      passed_closure(0),
      created_closure(0),
      generator(0),
      edgecount(0),
      frame_info(ExcInfo(NULL, NULL, NULL)),
      globals(0),
      frame_addr(0) {

    scope_info = source_info->getScopeInfo();

    assert(scope_info);
}

void ASTInterpreter::initArguments(int nargs, BoxedClosure* _closure, BoxedGenerator* _generator, Box* arg1, Box* arg2,
                                   Box* arg3, Box** args) {
    passed_closure = _closure;
    generator = _generator;

    if (scope_info->createsClosure())
        created_closure = createClosure(passed_closure, scope_info->getClosureSize());

    std::vector<Box*, StlCompatAllocator<Box*>> argsArray{ arg1, arg2, arg3 };
    for (int i = 3; i < nargs; ++i)
        argsArray.push_back(args[i - 3]);

    const ParamNames& param_names = clfunc->param_names;

    int i = 0;
    for (auto& name : param_names.args) {
        doStore(source_info->getInternedStrings().get(name), Value(argsArray[i++], 0));
    }

    if (!param_names.vararg.str().empty()) {
        doStore(source_info->getInternedStrings().get(param_names.vararg), Value(argsArray[i++], 0));
    }

    if (!param_names.kwarg.str().empty()) {
        doStore(source_info->getInternedStrings().get(param_names.kwarg), Value(argsArray[i++], 0));
    }
}

RegisterHelper::RegisterHelper() : frame_addr(NULL), interpreter(NULL) {
}

RegisterHelper::~RegisterHelper() {
    assert(interpreter);
    assert(interpreter->frame_addr == frame_addr);
    interpreter->frame_addr = nullptr;
    deregister(frame_addr);
}

void RegisterHelper::doRegister(void* frame_addr, ASTInterpreter* interpreter) {
    assert(!this->interpreter);
    assert(!this->frame_addr);
    this->frame_addr = frame_addr;
    this->interpreter = interpreter;
    interpreter->frame_addr = frame_addr;
    s_interpreterMap[frame_addr] = interpreter;
}

void RegisterHelper::deregister(void* frame_addr) {
    assert(frame_addr);
    assert(s_interpreterMap.count(frame_addr));
    s_interpreterMap.erase(frame_addr);
}

void ASTInterpreter::startJITing(CFGBlock* block, int exit_offset) {
    assert(ENABLE_BASELINEJIT);
    assert(!jit);

    auto& code_blocks = clfunc->code_blocks;
    JitCodeBlock* code_block = NULL;
    if (!code_blocks.empty())
        code_block = code_blocks[code_blocks.size() - 1].get();

    if (!code_block || code_block->shouldCreateNewBlock()) {
        code_blocks.push_back(std::unique_ptr<JitCodeBlock>(new JitCodeBlock(source_info->getName())));
        code_block = code_blocks[code_blocks.size() - 1].get();
        exit_offset = 0;
    }

    jit = code_block->newFragment(block, exit_offset);
}

void ASTInterpreter::abortJITing() {
    if (jit) {
        jit->abortCompilation();
        jit.reset();
    }
}

void ASTInterpreter::finishJITing(CFGBlock* continue_block) {
    if (!jit)
        return;
    int exit_offset = jit->finishCompilation();
    jit.reset();
    if (continue_block && !continue_block->code)
        startJITing(continue_block, exit_offset);
}

Box* ASTInterpreter::execJITedBlock(CFGBlock* b) {
    try {
        UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_baseline_jitted_code");
        std::pair<CFGBlock*, Box*> rtn = b->entry_code(this, b);
        next_block = rtn.first;
        if (!next_block)
            return rtn.second;
    } catch (ExcInfo e) {
        AST_stmt* stmt = getCurrentStatement();
        if (stmt->type != AST_TYPE::Invoke)
            throw e;

        auto source = getCL()->source.get();
        exceptionCaughtInInterpreter(LineInfo(stmt->lineno, stmt->col_offset, source->fn, source->getName()), &e);

        next_block = ((AST_Invoke*)stmt)->exc_dest;
        last_exception = e;
    }
    return nullptr;
}

Value ASTInterpreter::executeInner(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at,
                                   RegisterHelper* reg) {

    void* frame_addr = __builtin_frame_address(0);
    reg->doRegister(frame_addr, &interpreter);

    Value v;

    bool should_jit = false;
    bool from_start = start_block == NULL && start_at == NULL;

    assert((start_block == NULL) == (start_at == NULL));
    if (start_block == NULL) {
        start_block = interpreter.source_info->cfg->getStartingBlock();
        start_at = start_block->body[0];

        if (ENABLE_BASELINEJIT && interpreter.clfunc->times_interpreted >= REOPT_THRESHOLD_INTERPRETER
            && !start_block->code)
            should_jit = true;
    }

    // Important that this happens after RegisterHelper:
    interpreter.current_inst = start_at;
    threading::allowGLReadPreemption();
    interpreter.current_inst = NULL;

    if (!from_start) {
        interpreter.current_block = start_block;
        bool started = false;
        for (auto s : start_block->body) {
            if (!started) {
                if (s != start_at)
                    continue;
                started = true;
            }

            interpreter.current_inst = s;
            v = interpreter.visit_stmt(s);
        }
    } else {
        if (should_jit)
            interpreter.startJITing(start_block);

        interpreter.next_block = start_block;
    }

    while (interpreter.next_block) {
        interpreter.current_block = interpreter.next_block;
        interpreter.next_block = 0;

        if (ENABLE_BASELINEJIT && !interpreter.jit) {
            CFGBlock* b = interpreter.current_block;
            if (b->entry_code) {
                should_jit = true;
                Box* rtn = interpreter.execJITedBlock(b);
                if (interpreter.next_block)
                    continue;
                return Value(rtn, nullptr);
            }
        }

        if (ENABLE_BASELINEJIT && should_jit && !interpreter.jit) {
            assert(!interpreter.current_block->code);
            interpreter.startJITing(interpreter.current_block);
        }

        for (AST_stmt* s : interpreter.current_block->body) {
            interpreter.current_inst = s;
            if (interpreter.jit)
                interpreter.jit->emitSetCurrentInst(s);
            v = interpreter.visit_stmt(s);
        }
    }
    return v;
}

Value ASTInterpreter::execute(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_interpreter");

    // Note: due to some (avoidable) restrictions, this check is pretty constrained in where
    // it can go, due to the fact that it can throw an exception.
    // It can't go in the ASTInterpreter constructor, since that will cause the C++ runtime to
    // delete the partially-constructed memory which we don't currently handle.  It can't go into
    // executeInner since we want the SyntaxErrors to happen *before* the stack frame is entered.
    // (For instance, throwing the exception will try to fetch the current statement, but we determine
    // that by looking at the cfg.)
    if (!interpreter.source_info->cfg)
        interpreter.source_info->cfg = computeCFG(interpreter.source_info, interpreter.source_info->body);

    RegisterHelper frame_registerer;
    return executeInner(interpreter, start_block, start_at, &frame_registerer);
}

Value ASTInterpreter::doBinOp(Value left, Value right, int op, BinExpType exp_type) {
    switch (exp_type) {
        case BinExpType::AugBinOp:
            return Value(augbinop(left.o, right.o, op), jit ? jit->emitAugbinop(left, right, op) : NULL);
        case BinExpType::BinOp:
            return Value(binop(left.o, right.o, op), jit ? jit->emitBinop(left, right, op) : NULL);
        case BinExpType::Compare:
            return Value(compare(left.o, right.o, op), jit ? jit->emitCompare(left, right, op) : NULL);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
    return Value();
}

void ASTInterpreter::doStore(InternedString name, Value value) {
    ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(name);
    if (vst == ScopeInfo::VarScopeType::GLOBAL) {
        if (jit)
            jit->emitSetGlobal(globals, name.getBox(), value);
        setGlobal(globals, name.getBox(), value.o);
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        if (jit)
            jit->emitSetItemName(name.getBox(), value);
        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        setitem(frame_info.boxedLocals, name.getBox(), value.o);
    } else {
        bool closure = vst == ScopeInfo::VarScopeType::CLOSURE;
        if (jit) {
            if (!closure) {
                bool is_live = source_info->getLiveness()->isLiveAtEnd(name, current_block);
                if (is_live)
                    jit->emitSetLocal(name, closure, value);
                else
                    jit->emitSetBlockLocal(name, value);
            } else
                jit->emitSetLocal(name, closure, value);
        }

        sym_table[name] = value.o;
        if (closure) {
            created_closure->elts[scope_info->getClosureOffset(name)] = value.o;
        }
    }
}

void ASTInterpreter::doStore(AST_expr* node, Value value) {
    if (node->type == AST_TYPE::Name) {
        AST_Name* name = (AST_Name*)node;
        doStore(name->id, value);
    } else if (node->type == AST_TYPE::Attribute) {
        AST_Attribute* attr = (AST_Attribute*)node;
        Value o = visit_expr(attr->value);
        if (jit)
            jit->emitSetAttr(o, attr->attr.getBox(), value);
        pyston::setattr(o.o, attr->attr.getBox(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());

        RewriterVar* array_var = NULL;
        if (jit)
            array_var = jit->emitUnpackIntoArray(value, tuple->elts.size());

        unsigned i = 0;
        for (AST_expr* e : tuple->elts) {
            doStore(e, Value(array[i], jit ? array_var->getAttr(i * sizeof(void*)) : NULL));
            ++i;
        }
    } else if (node->type == AST_TYPE::List) {
        AST_List* list = (AST_List*)node;
        Box** array = unpackIntoArray(value.o, list->elts.size());

        RewriterVar* array_var = NULL;
        if (jit)
            array_var = jit->emitUnpackIntoArray(value, list->elts.size());

        unsigned i = 0;
        for (AST_expr* e : list->elts) {
            doStore(e, Value(array[i], jit ? array_var->getAttr(i * sizeof(void*)) : NULL));
            ++i;
        }
    } else if (node->type == AST_TYPE::Subscript) {
        AST_Subscript* subscript = (AST_Subscript*)node;

        Value target = visit_expr(subscript->value);
        Value slice = visit_expr(subscript->slice);

        if (jit)
            jit->emitSetItem(target, slice, value);
        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::getNone() {
    return Value(None, jit ? jit->imm(None) : NULL);
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not)
        return Value(boxBool(!nonzero(operand.o)), jit ? jit->emitNotNonzero(operand) : NULL);
    else
        return Value(unaryop(operand.o, node->op_type), jit ? jit->emitUnaryop(operand, node->op_type) : NULL);
}

Value ASTInterpreter::visit_binop(AST_BinOp* node) {
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left, right, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(AST_Slice* node) {
    Value lower = node->lower ? visit_expr(node->lower) : getNone();
    Value upper = node->upper ? visit_expr(node->upper) : getNone();
    Value step = node->step ? visit_expr(node->step) : getNone();

    Value v;
    if (jit)
        v.var = jit->emitCreateSlice(lower, upper, step);
    v.o = createSlice(lower.o, upper.o, step.o);
    return v;
}

Value ASTInterpreter::visit_extslice(AST_ExtSlice* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    int num_slices = node->dims.size();
    BoxedTuple* rtn = BoxedTuple::create(num_slices);
    for (int i = 0; i < num_slices; ++i) {
        Value v = visit_expr(node->dims[i]);
        rtn->elts[i] = v.o;
        items.push_back(v);
    }

    return Value(rtn, jit ? jit->emitCreateTuple(items) : NULL);
}

Value ASTInterpreter::visit_branch(AST_Branch* node) {
    Value v = visit_expr(node->test);
    ASSERT(v.o == True || v.o == False, "Should have called NONZERO before this branch");

    if (jit)
        jit->emitSideExit(v, v.o, v.o == True ? node->iffalse : node->iftrue);

    if (v.o == True)
        next_block = node->iftrue;
    else
        next_block = node->iffalse;

    if (jit) {
        jit->emitJump(next_block);
        finishJITing(next_block);
    }

    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx;
    if (backedge) {
        threading::allowGLReadPreemption();

        if (jit)
            jit->call(false, (void*)threading::allowGLReadPreemption);
    }

    if (jit) {
        if (backedge)
            jit->emitOSRPoint(node);
        jit->emitJump(node->target);
        finishJITing(node->target);
    }

    if (backedge)
        ++edgecount;

    if (ENABLE_BASELINEJIT && backedge && edgecount == OSR_THRESHOLD_INTERPRETER && !jit && !node->target->code)
        startJITing(node->target);

    if (backedge && edgecount == OSR_THRESHOLD_BASELINE) {
        Box* rtn = doOSR(node);
        if (rtn)
            return Value(rtn, NULL);
    }

    next_block = node->target;
    return Value();
}

Box* ASTInterpreter::doOSR(AST_Jump* node) {
    bool can_osr = ENABLE_OSR && !FORCE_INTERPRETER && source_info->scoping->areGlobalsFromModule();
    if (!can_osr)
        return NULL;

    static StatCounter ast_osrs("num_ast_osrs");
    ast_osrs.log();

    LivenessAnalysis* liveness = source_info->getLiveness();
    std::unique_ptr<PhiAnalysis> phis
        = computeRequiredPhis(clfunc->param_names, source_info->cfg, liveness, scope_info);

    std::vector<InternedString> dead_symbols;
    for (auto& it : sym_table) {
        if (!liveness->isLiveAtEnd(it.first, current_block)) {
            dead_symbols.push_back(it.first);
        } else if (phis->isRequiredAfter(it.first, current_block)) {
            assert(scope_info->getScopeTypeOfName(it.first) != ScopeInfo::VarScopeType::GLOBAL);
        } else {
        }
    }
    for (auto&& dead : dead_symbols)
        sym_table.erase(dead);

    const OSREntryDescriptor* found_entry = nullptr;
    for (auto& p : clfunc->osr_versions) {
        if (p.first->backedge != node)
            continue;

        found_entry = p.first;
    }

    std::map<InternedString, Box*> sorted_symbol_table;

    // TODO: maybe use a different placeholder?
    static Box* const VAL_UNDEFINED = (Box*)-1;

    for (auto& name : phis->definedness.getDefinedNamesAtEnd(current_block)) {
        auto it = sym_table.find(name);
        if (!liveness->isLiveAtEnd(name, current_block))
            continue;

        if (phis->isPotentiallyUndefinedAfter(name, current_block)) {
            bool is_defined = it != sym_table.end();
            // TODO only mangle once
            sorted_symbol_table[getIsDefinedName(name, source_info->getInternedStrings())] = (Box*)is_defined;
            if (is_defined)
                assert(sym_table.getMapped(it->second) != NULL);
            sorted_symbol_table[name] = is_defined ? sym_table.getMapped(it->second) : VAL_UNDEFINED;
        } else {
            ASSERT(it != sym_table.end(), "%s", name.c_str());
            Box* v = sorted_symbol_table[it->first] = sym_table.getMapped(it->second);
            assert(gc::isValidGCObject(v));
        }
    }

    // Manually free these here, since we might not return from this scope for a long time.
    phis.reset(nullptr);

    // LLVM has a limit on the number of operands a machine instruction can have (~255),
    // in order to not hit the limit with the patchpoints cancel OSR when we have a high number of symbols.
    if (sorted_symbol_table.size() > 225) {
        static StatCounter times_osr_cancel("num_osr_cancel_too_many_syms");
        times_osr_cancel.log();
        return nullptr;
    }

    if (generator)
        sorted_symbol_table[source_info->getInternedStrings().get(PASSED_GENERATOR_NAME)] = generator;

    if (passed_closure)
        sorted_symbol_table[source_info->getInternedStrings().get(PASSED_CLOSURE_NAME)] = passed_closure;

    if (created_closure)
        sorted_symbol_table[source_info->getInternedStrings().get(CREATED_CLOSURE_NAME)] = created_closure;

    sorted_symbol_table[source_info->getInternedStrings().get(FRAME_INFO_PTR_NAME)] = (Box*)&frame_info;

    if (found_entry == nullptr) {
        OSREntryDescriptor* entry = OSREntryDescriptor::create(clfunc, node);

        for (auto& it : sorted_symbol_table) {
            if (isIsDefinedName(it.first))
                entry->args[it.first] = BOOL;
            else if (it.first.s() == PASSED_GENERATOR_NAME)
                entry->args[it.first] = GENERATOR;
            else if (it.first.s() == PASSED_CLOSURE_NAME || it.first.s() == CREATED_CLOSURE_NAME)
                entry->args[it.first] = CLOSURE;
            else if (it.first.s() == FRAME_INFO_PTR_NAME)
                entry->args[it.first] = FRAME_INFO;
            else {
                assert(it.first.s()[0] != '!');
                entry->args[it.first] = UNKNOWN;
            }
        }

        found_entry = entry;
    }

    OSRExit exit(found_entry);

    std::vector<Box*, StlCompatAllocator<Box*>> arg_array;
    for (auto& it : sorted_symbol_table) {
        arg_array.push_back(it.second);
    }

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_jitted_code");
    CompiledFunction* partial_func = compilePartialFuncInternal(&exit);
    auto arg_tuple = getTupleFromArgsArray(&arg_array[0], arg_array.size());
    Box* r = partial_func->call(std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                                std::get<3>(arg_tuple));

    assert(r);
    return r;
}

Value ASTInterpreter::visit_invoke(AST_Invoke* node) {
    Value v;
    try {
        v = visit_stmt(node->stmt);
        next_block = node->normal_dest;

        if (jit) {
            jit->emitJump(next_block);
            finishJITing(next_block);
        }
    } catch (ExcInfo e) {
        abortJITing();

        auto source = getCL()->source.get();
        exceptionCaughtInInterpreter(LineInfo(node->lineno, node->col_offset, source->fn, source->getName()), &e);

        next_block = node->exc_dest;
        last_exception = e;
    }

    return v;
}

Value ASTInterpreter::visit_clsAttribute(AST_ClsAttribute* node) {
    Value obj = visit_expr(node->value);
    BoxedString* attr = node->attr.getBox();
    return Value(getclsattr(obj.o, attr), jit ? jit->emitGetClsAttr(obj, attr) : NULL);
}

Value ASTInterpreter::visit_augBinOp(AST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left, right, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_langPrimitive(AST_LangPrimitive* node) {
    Value v;
    if (node->opcode == AST_LangPrimitive::GET_ITER) {
        assert(node->args.size() == 1);
        Value val = visit_expr(node->args[0]);
        v = Value(getPystonIter(val.o), jit ? jit->emitGetPystonIter(val) : NULL);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_FROM) {
        abortJITing();
        assert(node->args.size() == 2);
        assert(node->args[0]->type == AST_TYPE::Name);
        assert(node->args[1]->type == AST_TYPE::Str);

        Value module = visit_expr(node->args[0]);
        auto ast_str = ast_cast<AST_Str>(node->args[1]);
        assert(ast_str->str_type == AST_Str::STR);
        const std::string& name = ast_str->str_data;
        assert(name.size());
        // TODO: shouldn't have to rebox here
        v.o = importFrom(module.o, internStringMortal(name));
    } else if (node->opcode == AST_LangPrimitive::IMPORT_NAME) {
        abortJITing();
        assert(node->args.size() == 3);
        assert(node->args[0]->type == AST_TYPE::Num);
        assert(static_cast<AST_Num*>(node->args[0])->num_type == AST_Num::INT);
        assert(node->args[2]->type == AST_TYPE::Str);

        int level = static_cast<AST_Num*>(node->args[0])->n_int;
        Value froms = visit_expr(node->args[1]);
        auto ast_str = ast_cast<AST_Str>(node->args[2]);
        assert(ast_str->str_type == AST_Str::STR);
        const std::string& module_name = ast_str->str_data;
        v.o = import(level, froms.o, module_name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_STAR) {
        abortJITing();
        assert(node->args.size() == 1);
        assert(node->args[0]->type == AST_TYPE::Name);

        RELEASE_ASSERT(source_info->ast->type == AST_TYPE::Module || source_info->ast->type == AST_TYPE::Suite,
                       "import * not supported in functions");

        Value module = visit_expr(node->args[0]);

        v.o = importStar(module.o, globals);
    } else if (node->opcode == AST_LangPrimitive::NONE) {
        v = getNone();
    } else if (node->opcode == AST_LangPrimitive::LANDINGPAD) {
        assert(last_exception.type);
        Box* type = last_exception.type;
        Box* value = last_exception.value ? last_exception.value : None;
        Box* traceback = last_exception.traceback ? last_exception.traceback : None;
        v = Value(BoxedTuple::create({ type, value, traceback }), jit ? jit->emitLandingpad() : NULL);
        last_exception = ExcInfo(NULL, NULL, NULL);
    } else if (node->opcode == AST_LangPrimitive::CHECK_EXC_MATCH) {
        assert(node->args.size() == 2);
        Value obj = visit_expr(node->args[0]);
        Value cls = visit_expr(node->args[1]);
        v = Value(boxBool(exceptionMatches(obj.o, cls.o)), jit ? jit->emitExceptionMatches(obj, cls) : NULL);
    } else if (node->opcode == AST_LangPrimitive::LOCALS) {
        assert(frame_info.boxedLocals != NULL);
        v = Value(frame_info.boxedLocals, jit ? jit->emitGetBoxedLocals() : NULL);
    } else if (node->opcode == AST_LangPrimitive::NONZERO) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        v = Value(boxBool(nonzero(obj.o)), jit ? jit->emitNonzero(obj) : NULL);
    } else if (node->opcode == AST_LangPrimitive::SET_EXC_INFO) {
        assert(node->args.size() == 3);

        Value type = visit_expr(node->args[0]);
        assert(type.o);
        Value value = visit_expr(node->args[1]);
        assert(value.o);
        Value traceback = visit_expr(node->args[2]);
        assert(traceback.o);

        if (jit)
            jit->emitSetExcInfo(type, value, traceback);
        getFrameInfo()->exc = ExcInfo(type.o, value.o, traceback.o);
        v = getNone();
    } else if (node->opcode == AST_LangPrimitive::UNCACHE_EXC_INFO) {
        assert(node->args.empty());
        if (jit)
            jit->emitUncacheExcInfo();
        getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        v = getNone();
    } else if (node->opcode == AST_LangPrimitive::HASNEXT) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        v = Value(boxBool(hasnext(obj.o)), jit ? jit->emitHasnext(obj) : NULL);
    } else
        RELEASE_ASSERT(0, "unknown opcode %d", node->opcode);
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : getNone();
    assert(generator && generator->cls == generator_cls);

    return Value(yield(generator, value.o), jit ? jit->emitYield(value) : NULL);
}

Value ASTInterpreter::visit_stmt(AST_stmt* node) {
#if ENABLE_SAMPLING_PROFILER
    threading::allowGLReadPreemption();
#endif

    if (0) {
        printf("%20s % 2d ", source_info->getName().data(), current_block->idx);
        print_ast(node);
        printf("\n");
    }

    switch (node->type) {
        case AST_TYPE::Assert:
            return visit_assert((AST_Assert*)node);
        case AST_TYPE::Assign:
            return visit_assign((AST_Assign*)node);
        case AST_TYPE::Delete:
            return visit_delete((AST_Delete*)node);
        case AST_TYPE::Exec:
            return visit_exec((AST_Exec*)node);
        case AST_TYPE::Expr:
            // docstrings are str constant expression statements.
            // ignore those while interpreting.
            if ((((AST_Expr*)node)->value)->type != AST_TYPE::Str)
                return visit_expr((AST_Expr*)node);
            break;
        case AST_TYPE::Pass:
            return Value(); // nothing todo
        case AST_TYPE::Print:
            return visit_print((AST_Print*)node);
        case AST_TYPE::Raise:
            return visit_raise((AST_Raise*)node);
        case AST_TYPE::Return:
            return visit_return((AST_Return*)node);
        case AST_TYPE::Global:
            return visit_global((AST_Global*)node);

        // pseudo
        case AST_TYPE::Branch:
            return visit_branch((AST_Branch*)node);
        case AST_TYPE::Jump:
            return visit_jump((AST_Jump*)node);
        case AST_TYPE::Invoke:
            return visit_invoke((AST_Invoke*)node);
        default:
            RELEASE_ASSERT(0, "not implemented");
    };
    return Value();
}

Value ASTInterpreter::visit_return(AST_Return* node) {
    Value s = node->value ? visit_expr(node->value) : getNone();

    if (jit) {
        jit->emitReturn(s);
        finishJITing();
    }

    next_block = 0;
    return s;
}

Box* ASTInterpreter::createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body) {
    abortJITing();
    CLFunction* cl = wrapFunction(node, args, body, source_info);

    std::vector<Box*, StlCompatAllocator<Box*>> defaults;
    for (AST_expr* d : args->defaults)
        defaults.push_back(visit_expr(d).o);
    defaults.push_back(0);

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

    bool takes_closure;
    // Optimization: when compiling a module, it's nice to not have to run analyses into the
    // entire module's source code.
    // If we call getScopeInfoForNode, that will trigger an analysis of that function tree,
    // but we're only using it here to figure out if that function takes a closure.
    // Top level functions never take a closure, so we can skip the analysis.
    if (source_info->ast->type == AST_TYPE::Module)
        takes_closure = false;
    else {
        takes_closure = source_info->scoping->getScopeInfoForNode(node)->takesClosure();
    }

    BoxedClosure* closure = 0;
    if (takes_closure) {
        if (scope_info->createsClosure()) {
            closure = created_closure;
        } else {
            assert(scope_info->passesThroughClosure());
            closure = passed_closure;
        }
        assert(closure);
    }

    Box* passed_globals = NULL;
    if (!getCL()->source->scoping->areGlobalsFromModule())
        passed_globals = globals;
    return boxCLFunction(cl, closure, passed_globals, u.il);
}

Value ASTInterpreter::visit_makeFunction(AST_MakeFunction* mkfn) {
    abortJITing();
    AST_FunctionDef* node = mkfn->function_def;
    AST_arguments* args = node->args;

    std::vector<Box*, StlCompatAllocator<Box*>> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    Box* func = createFunction(node, args, node->body);

    for (int i = decorators.size() - 1; i >= 0; i--)
        func = runtimeCall(decorators[i], ArgPassSpec(1), func, 0, 0, 0, 0);

    return Value(func, NULL);
}

Value ASTInterpreter::visit_makeClass(AST_MakeClass* mkclass) {
    abortJITing();
    AST_ClassDef* node = mkclass->class_def;
    ScopeInfo* scope_info = source_info->scoping->getScopeInfoForNode(node);
    assert(scope_info);

    BoxedTuple* basesTuple = BoxedTuple::create(node->bases.size());
    int base_idx = 0;
    for (AST_expr* b : node->bases) {
        basesTuple->elts[base_idx++] = visit_expr(b).o;
    }

    std::vector<Box*, StlCompatAllocator<Box*>> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    BoxedClosure* closure = NULL;
    if (scope_info->takesClosure()) {
        if (this->scope_info->passesThroughClosure())
            closure = passed_closure;
        else
            closure = created_closure;
        assert(closure);
    }
    CLFunction* cl = wrapFunction(node, nullptr, node->body, source_info);

    Box* passed_globals = NULL;
    if (!getCL()->source->scoping->areGlobalsFromModule())
        passed_globals = globals;
    Box* attrDict = runtimeCall(boxCLFunction(cl, closure, passed_globals, {}), ArgPassSpec(0), 0, 0, 0, 0, 0);

    Box* classobj = createUserClass(node->name.getBox(), basesTuple, attrDict);

    for (int i = decorators.size() - 1; i >= 0; i--)
        classobj = runtimeCall(decorators[i], ArgPassSpec(1), classobj, 0, 0, 0, 0);

    return Value(classobj, NULL);
}

Value ASTInterpreter::visit_raise(AST_Raise* node) {
    if (node->arg0 == NULL) {
        assert(!node->arg1);
        assert(!node->arg2);

        if (jit) {
            jit->emitRaise0();
            finishJITing();
        }

        raise0();
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

Value ASTInterpreter::visit_assert(AST_Assert* node) {
    abortJITing();
#ifndef NDEBUG
    // Currently we only generate "assert 0" statements
    Value v = visit_expr(node->test);
    assert(v.o->cls == int_cls && static_cast<BoxedInt*>(v.o)->n == 0);
#endif

    static BoxedString* AssertionError_str = internStringImmortal("AssertionError");
    Box* assertion_type = getGlobal(globals, AssertionError_str);
    assertFail(assertion_type, node->msg ? visit_expr(node->msg).o : 0);

    return Value();
}

Value ASTInterpreter::visit_global(AST_Global* node) {
    abortJITing();
    for (auto name : node->names)
        sym_table.erase(name);
    return Value();
}

Value ASTInterpreter::visit_delete(AST_Delete* node) {
    abortJITing();
    for (AST_expr* target_ : node->targets) {
        switch (target_->type) {
            case AST_TYPE::Subscript: {
                AST_Subscript* sub = (AST_Subscript*)target_;
                Value value = visit_expr(sub->value);
                Value slice = visit_expr(sub->slice);
                delitem(value.o, slice.o);
                break;
            }
            case AST_TYPE::Attribute: {
                AST_Attribute* attr = (AST_Attribute*)target_;
                pyston::delattr(visit_expr(attr->value).o, attr->attr.getBox());
                break;
            }
            case AST_TYPE::Name: {
                AST_Name* target = (AST_Name*)target_;

                ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(target->id);
                if (vst == ScopeInfo::VarScopeType::GLOBAL) {
                    delGlobal(globals, target->id.getBox());
                    continue;
                } else if (vst == ScopeInfo::VarScopeType::NAME) {
                    assert(frame_info.boxedLocals != NULL);
                    if (frame_info.boxedLocals->cls == dict_cls) {
                        auto& d = static_cast<BoxedDict*>(frame_info.boxedLocals)->d;
                        auto it = d.find(target->id.getBox());
                        if (it == d.end()) {
                            assertNameDefined(0, target->id.c_str(), NameError, false /* local_var_msg */);
                        }
                        d.erase(it);
                    } else if (frame_info.boxedLocals->cls == attrwrapper_cls) {
                        attrwrapperDel(frame_info.boxedLocals, target->id);
                    } else {
                        RELEASE_ASSERT(0, "%s", frame_info.boxedLocals->cls->tp_name);
                    }
                } else {
                    assert(vst == ScopeInfo::VarScopeType::FAST);

                    if (sym_table.count(target->id) == 0) {
                        assertNameDefined(0, target->id.c_str(), NameError, true /* local_var_msg */);
                        return Value();
                    }

                    sym_table.erase(target->id);
                }
                break;
            }
            default:
                ASSERT(0, "Unsupported del target: %d", target_->type);
                abort();
        }
    }
    return Value();
}

Value ASTInterpreter::visit_assign(AST_Assign* node) {
    assert(node->targets.size() == 1 && "cfg should have lowered it to a single target");

    Value v = visit_expr(node->value);
    for (AST_expr* e : node->targets)
        doStore(e, v);
    return Value();
}

Value ASTInterpreter::visit_print(AST_Print* node) {
    assert(node->values.size() <= 1 && "cfg should have lowered it to 0 or 1 values");
    Value dest = node->dest ? visit_expr(node->dest) : Value();
    Value var = node->values.size() ? visit_expr(node->values[0]) : Value();

    if (jit)
        jit->emitPrint(dest, var, node->nl);

    if (node->dest)
        printHelper(dest.o, var.o, node->nl);
    else
        printHelper(getSysStdout(), var.o, node->nl);

    return Value();
}

Value ASTInterpreter::visit_exec(AST_Exec* node) {
    // TODO implement the locals and globals arguments
    Value code = visit_expr(node->body);
    Value globals = node->globals == NULL ? Value() : visit_expr(node->globals);
    Value locals = node->locals == NULL ? Value() : visit_expr(node->locals);

    if (jit)
        jit->emitExec(code, globals, locals, this->source_info->future_flags);
    exec(code.o, globals.o, locals.o, this->source_info->future_flags);

    return Value();
}

Value ASTInterpreter::visit_compare(AST_Compare* node) {
    RELEASE_ASSERT(node->comparators.size() == 1, "not implemented");
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->comparators[0]);
    return doBinOp(left, right, node->ops[0], BinExpType::Compare);
}

Value ASTInterpreter::visit_expr(AST_expr* node) {
    switch (node->type) {
        case AST_TYPE::Attribute:
            return visit_attribute((AST_Attribute*)node);
        case AST_TYPE::BinOp:
            return visit_binop((AST_BinOp*)node);
        case AST_TYPE::Call:
            return visit_call((AST_Call*)node);
        case AST_TYPE::Compare:
            return visit_compare((AST_Compare*)node);
        case AST_TYPE::Dict:
            return visit_dict((AST_Dict*)node);
        case AST_TYPE::ExtSlice:
            return visit_extslice((AST_ExtSlice*)node);
        case AST_TYPE::Index:
            return visit_index((AST_Index*)node);
        case AST_TYPE::Lambda:
            return visit_lambda((AST_Lambda*)node);
        case AST_TYPE::List:
            return visit_list((AST_List*)node);
        case AST_TYPE::Name:
            return visit_name((AST_Name*)node);
        case AST_TYPE::Num:
            return visit_num((AST_Num*)node);
        case AST_TYPE::Repr:
            return visit_repr((AST_Repr*)node);
        case AST_TYPE::Set:
            return visit_set((AST_Set*)node);
        case AST_TYPE::Slice:
            return visit_slice((AST_Slice*)node);
        case AST_TYPE::Str:
            return visit_str((AST_Str*)node);
        case AST_TYPE::Subscript:
            return visit_subscript((AST_Subscript*)node);
        case AST_TYPE::Tuple:
            return visit_tuple((AST_Tuple*)node);
        case AST_TYPE::UnaryOp:
            return visit_unaryop((AST_UnaryOp*)node);
        case AST_TYPE::Yield:
            return visit_yield((AST_Yield*)node);

        // pseudo
        case AST_TYPE::AugBinOp:
            return visit_augBinOp((AST_AugBinOp*)node);
        case AST_TYPE::ClsAttribute:
            return visit_clsAttribute((AST_ClsAttribute*)node);
        case AST_TYPE::LangPrimitive:
            return visit_langPrimitive((AST_LangPrimitive*)node);
        case AST_TYPE::MakeClass:
            return visit_makeClass((AST_MakeClass*)node);
        case AST_TYPE::MakeFunction:
            return visit_makeFunction((AST_MakeFunction*)node);
        default:
            RELEASE_ASSERT(0, "");
    };
    return Value();
}


Value ASTInterpreter::visit_call(AST_Call* node) {
    Value v;
    Value func;

    InternedString attr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    if (node->func->type == AST_TYPE::Attribute) {
        is_callattr = true;
        callattr_clsonly = false;
        AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = attr_ast->attr;
    } else if (node->func->type == AST_TYPE::ClsAttribute) {
        is_callattr = true;
        callattr_clsonly = true;
        AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = attr_ast->attr;
    } else {
        func = visit_expr(node->func);
    }

    std::vector<Box*, StlCompatAllocator<Box*>> args;
    llvm::SmallVector<RewriterVar*, 8> args_vars;
    for (AST_expr* e : node->args) {
        Value v = visit_expr(e);
        args.push_back(v.o);
        args_vars.push_back(v);
    }

    std::vector<BoxedString*>* keyword_names = NULL;
    if (node->keywords.size())
        keyword_names = getKeywordNameStorage(node);

    for (AST_keyword* k : node->keywords) {
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

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        CallattrFlags callattr_flags{.cls_only = callattr_clsonly, .null_on_nonexistent = false, .argspec = argspec };

        if (jit)
            v.var = jit->emitCallattr(node, func, attr.getBox(), callattr_flags, args_vars, keyword_names);

        v.o = callattr(func.o, attr.getBox(), callattr_flags, args.size() > 0 ? args[0] : 0,
                       args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0,
                       keyword_names);
        return v;
    } else {
        Value v;

        if (jit)
            v.var = jit->emitRuntimeCall(node, func, argspec, args_vars, keyword_names);

        v.o = runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                          args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, keyword_names);
        return v;
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    Box* o = NULL;
    if (node->num_type == AST_Num::INT) {
        o = source_info->parent_module->getIntConstant(node->n_int);
    } else if (node->num_type == AST_Num::FLOAT) {
        o = source_info->parent_module->getFloatConstant(node->n_float);
    } else if (node->num_type == AST_Num::LONG) {
        o = source_info->parent_module->getLongConstant(node->n_long);
    } else if (node->num_type == AST_Num::COMPLEX) {
        o = source_info->parent_module->getPureImaginaryConstant(node->n_float);
    } else
        RELEASE_ASSERT(0, "not implemented");
    return Value(o, jit ? jit->imm(o) : NULL);
}

Value ASTInterpreter::visit_index(AST_Index* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_repr(AST_Repr* node) {
    Value v = visit_expr(node->value);
    return Value(repr(v.o), jit ? jit->emitRepr(v) : NULL);
}

Value ASTInterpreter::visit_lambda(AST_Lambda* node) {
    abortJITing();
    AST_Return* expr = new AST_Return();
    expr->value = node->body;

    std::vector<AST_stmt*> body = { expr };
    return Value(createFunction(node, node->args, body), NULL);
}

Value ASTInterpreter::visit_dict(AST_Dict* node) {
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");

    llvm::SmallVector<RewriterVar*, 8> keys;
    llvm::SmallVector<RewriterVar*, 8> values;

    BoxedDict* dict = new BoxedDict();
    for (size_t i = 0; i < node->keys.size(); ++i) {
        Value v = visit_expr(node->values[i]);
        Value k = visit_expr(node->keys[i]);
        dict->d[k.o] = v.o;

        values.push_back(v);
        keys.push_back(k);
    }

    return Value(dict, jit ? jit->emitCreateDict(keys, values) : NULL);
}

Value ASTInterpreter::visit_set(AST_Set* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    BoxedSet::Set set;
    for (AST_expr* e : node->elts) {
        Value v = visit_expr(e);
        set.insert(v.o);
        items.push_back(v);
    }

    return Value(new BoxedSet(std::move(set)), jit ? jit->emitCreateSet(items) : NULL);
}

Value ASTInterpreter::visit_str(AST_Str* node) {
    Box* o = NULL;
    if (node->str_type == AST_Str::STR) {
        o = source_info->parent_module->getStringConstant(node->str_data);
    } else if (node->str_type == AST_Str::UNICODE) {
        o = source_info->parent_module->getUnicodeConstant(node->str_data);
    } else {
        RELEASE_ASSERT(0, "%d", node->str_type);
    }
    return Value(o, jit ? jit->imm(o) : NULL);
}

Value ASTInterpreter::visit_name(AST_Name* node) {
    if (node->lookup_type == ScopeInfo::VarScopeType::UNKNOWN) {
        node->lookup_type = scope_info->getScopeTypeOfName(node->id);
    }

    switch (node->lookup_type) {
        case ScopeInfo::VarScopeType::GLOBAL: {
            Value v;
            if (jit)
                v.var = jit->emitGetGlobal(globals, node->id.getBox());

            v.o = getGlobal(globals, node->id.getBox());
            return v;
        }
        case ScopeInfo::VarScopeType::DEREF: {
            return Value(ASTInterpreterJitInterface::derefHelper(this, node->id),
                         jit ? jit->emitDeref(node->id) : NULL);
        }
        case ScopeInfo::VarScopeType::FAST:
        case ScopeInfo::VarScopeType::CLOSURE: {
            Value v;
            if (jit) {
                bool is_live = false;
                if (node->lookup_type == ScopeInfo::VarScopeType::FAST)
                    is_live = source_info->getLiveness()->isLiveAtEnd(node->id, current_block);

                if (is_live)
                    v.var = jit->emitGetLocal(node->id);
                else
                    v.var = jit->emitGetBlockLocal(node->id);
            }

            v.o = ASTInterpreterJitInterface::getLocalHelper(this, node->id);
            return v;
        }
        case ScopeInfo::VarScopeType::NAME: {
            Value v;
            if (jit)
                v.var = jit->emitGetBoxedLocal(node->id.getBox());
            v.o = boxedLocalsGet(frame_info.boxedLocals, node->id.getBox(), globals);
            return v;
        }
        default:
            abort();
    }
}

Value ASTInterpreter::visit_subscript(AST_Subscript* node) {
    Value value = visit_expr(node->value);
    Value slice = visit_expr(node->slice);

    return Value(getitem(value.o, slice.o), jit ? jit->emitGetItem(value, slice) : NULL);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts) {
        Value v = visit_expr(e);
        items.push_back(v);
        listAppendInternal(list, v.o);
    }

    return Value(list, jit ? jit->emitCreateList(items) : NULL);
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    llvm::SmallVector<RewriterVar*, 8> items;

    BoxedTuple* rtn = BoxedTuple::create(node->elts.size());
    int rtn_idx = 0;
    for (AST_expr* e : node->elts) {
        Value v = visit_expr(e);
        rtn->elts[rtn_idx++] = v.o;
        items.push_back(v);
    }

    return Value(rtn, jit ? jit->emitCreateTuple(items) : NULL);
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    Value v = visit_expr(node->value);
    return Value(pyston::getattr(v.o, node->attr.getBox()),
                 jit ? jit->emitGetAttr(v, node->attr.getBox(), node) : NULL);
}
}


int ASTInterpreterJitInterface::getCurrentBlockOffset() {
    return offsetof(ASTInterpreter, current_block);
}

int ASTInterpreterJitInterface::getCurrentInstOffset() {
    return offsetof(ASTInterpreter, current_inst);
}

Box* ASTInterpreterJitInterface::derefHelper(void* _interpreter, InternedString s) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    DerefInfo deref_info = interpreter->scope_info->getDerefInfo(s);
    assert(interpreter->passed_closure);
    BoxedClosure* closure = interpreter->passed_closure;
    for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
        closure = closure->parent;
    }
    Box* val = closure->elts[deref_info.offset];
    if (val == NULL) {
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", s.c_str());
    }
    return val;
}

Box* ASTInterpreterJitInterface::doOSRHelper(void* _interpreter, AST_Jump* node) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    ++interpreter->edgecount;
    if (interpreter->edgecount >= OSR_THRESHOLD_BASELINE)
        return interpreter->doOSR(node);
    return NULL;
}

Box* ASTInterpreterJitInterface::getBoxedLocalHelper(void* _interpreter, BoxedString* s) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    return boxedLocalsGet(interpreter->frame_info.boxedLocals, s, interpreter->globals);
}

Box* ASTInterpreterJitInterface::getBoxedLocalsHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    return interpreter->frame_info.boxedLocals;
}

Box* ASTInterpreterJitInterface::getLocalHelper(void* _interpreter, InternedString id) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    auto it = interpreter->sym_table.find(id);
    if (it != interpreter->sym_table.end()) {
        Box* v = interpreter->sym_table.getMapped(it->second);
        assert(gc::isValidGCObject(v));
        return v;
    }

    assertNameDefined(0, id.c_str(), UnboundLocalError, true);
    return 0;
}

Box* ASTInterpreterJitInterface::landingpadHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    ExcInfo& last_exception = interpreter->last_exception;
    Box* type = last_exception.type;
    Box* value = last_exception.value ? last_exception.value : None;
    Box* traceback = last_exception.traceback ? last_exception.traceback : None;
    Box* rtn = BoxedTuple::create({ type, value, traceback });
    last_exception = ExcInfo(NULL, NULL, NULL);
    return rtn;
}

Box* ASTInterpreterJitInterface::setExcInfoHelper(void* _interpreter, Box* type, Box* value, Box* traceback) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    interpreter->getFrameInfo()->exc = ExcInfo(type, value, traceback);
    return None;
}

Box* ASTInterpreterJitInterface::uncacheExcInfoHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    interpreter->getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
    return None;
}

Box* ASTInterpreterJitInterface::yieldHelper(void* _interpreter, Box* val) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    return yield(interpreter->generator, val);
}

void ASTInterpreterJitInterface::setItemNameHelper(void* _interpreter, Box* str, Box* val) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    assert(interpreter->frame_info.boxedLocals != NULL);
    setitem(interpreter->frame_info.boxedLocals, str, val);
}

void ASTInterpreterJitInterface::setLocalClosureHelper(void* _interpreter, InternedString id, Box* v) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    assert(gc::isValidGCObject(v));
    interpreter->sym_table[id] = v;

    interpreter->created_closure->elts[interpreter->scope_info->getClosureOffset(id)] = v;
}

void ASTInterpreterJitInterface::setLocalHelper(void* _interpreter, InternedString id, Box* v) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    assert(gc::isValidGCObject(v));
    interpreter->sym_table[id] = v;
}


const void* interpreter_instr_addr = (void*)&ASTInterpreter::executeInner;

Box* astInterpretFunction(CLFunction* clfunc, int nargs, Box* closure, Box* generator, Box* globals, Box* arg1,
                          Box* arg2, Box* arg3, Box** args) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_interpreter");

    SourceInfo* source_info = clfunc->source.get();

    assert((!globals) == source_info->scoping->areGlobalsFromModule());
    bool can_reopt = ENABLE_REOPT && !FORCE_INTERPRETER && (globals == NULL);

    // If the cfg hasn't been computed yet, just conservatively say that it will be a big function.
    // It shouldn't matter, since the cfg should only be NULL if this is the first execution of this
    // function.
    int num_blocks = source_info->cfg ? source_info->cfg->blocks.size() : 10000;
    int threshold = num_blocks <= 20 ? (REOPT_THRESHOLD_BASELINE / 3) : REOPT_THRESHOLD_BASELINE;
    if (unlikely(can_reopt && (FORCE_OPTIMIZE || !ENABLE_INTERPRETER || clfunc->times_interpreted > threshold))) {
        assert(!globals);

        clfunc->times_interpreted = 0;

        EffortLevel new_effort = EffortLevel::MODERATE;
        if (FORCE_OPTIMIZE)
            new_effort = EffortLevel::MAXIMAL;

        std::vector<ConcreteCompilerType*> arg_types;
        for (int i = 0; i < nargs; i++) {
            Box* arg = getArg(i, arg1, arg2, arg3, args);
            assert(arg); // only builtin functions can pass NULL args

            // TODO: reenable argument-type specialization
            arg_types.push_back(UNKNOWN);
            // arg_types.push_back(typeFromClass(arg->cls));
        }
        FunctionSpecialization* spec = new FunctionSpecialization(UNKNOWN, arg_types);

        // this also pushes the new CompiledVersion to the back of the version list:
        CompiledFunction* optimized = compileFunction(clfunc, spec, new_effort, NULL);

        clfunc->dependent_interp_callsites.invalidateAll();

        UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_jitted_code");
        if (closure && generator)
            return optimized->closure_generator_call((BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2,
                                                     arg3, args);
        else if (closure)
            return optimized->closure_call((BoxedClosure*)closure, arg1, arg2, arg3, args);
        else if (generator)
            return optimized->generator_call((BoxedGenerator*)generator, arg1, arg2, arg3, args);
        return optimized->call(arg1, arg2, arg3, args);
    }

    ++clfunc->times_interpreted;
    ASTInterpreter* interpreter = new ASTInterpreter(clfunc);

    ScopeInfo* scope_info = clfunc->source->getScopeInfo();
    if (unlikely(scope_info->usesNameLookup())) {
        interpreter->setBoxedLocals(new BoxedDict());
    }

    assert((!globals) == clfunc->source->scoping->areGlobalsFromModule());
    if (globals) {
        interpreter->setGlobals(globals);
    } else {
        interpreter->setGlobals(source_info->parent_module);
    }

    interpreter->initArguments(nargs, (BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    Value v = ASTInterpreter::execute(*interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretFunctionEval(CLFunction* clfunc, Box* globals, Box* boxedLocals) {
    ++clfunc->times_interpreted;

    ASTInterpreter* interpreter = new ASTInterpreter(clfunc);
    interpreter->initArguments(0, NULL, NULL, NULL, NULL, NULL, NULL);
    interpreter->setBoxedLocals(boxedLocals);

    ScopeInfo* scope_info = clfunc->source->getScopeInfo();
    SourceInfo* source_info = clfunc->source.get();

    assert(!clfunc->source->scoping->areGlobalsFromModule());
    assert(globals);
    interpreter->setGlobals(globals);

    Value v = ASTInterpreter::execute(*interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretDeopt(CLFunction* clfunc, AST_expr* after_expr, AST_stmt* enclosing_stmt, Box* expr_val,
                       FrameStackState frame_state) {
    assert(clfunc);
    assert(enclosing_stmt);
    assert(frame_state.locals);
    assert(after_expr);
    assert(expr_val);

    ASTInterpreter* interpreter = new ASTInterpreter(clfunc);

    ScopeInfo* scope_info = clfunc->source->getScopeInfo();
    SourceInfo* source_info = clfunc->source.get();
    assert(clfunc->source->scoping->areGlobalsFromModule());
    interpreter->setGlobals(source_info->parent_module);

    for (const auto& p : frame_state.locals->d) {
        assert(p.first->cls == str_cls);
        auto name = static_cast<BoxedString*>(p.first)->s();
        if (name == PASSED_GENERATOR_NAME) {
            interpreter->setGenerator(p.second);
        } else if (name == PASSED_CLOSURE_NAME) {
            interpreter->setPassedClosure(p.second);
        } else if (name == CREATED_CLOSURE_NAME) {
            interpreter->setCreatedClosure(p.second);
        } else {
            InternedString interned = clfunc->source->getInternedStrings().get(name);
            interpreter->addSymbol(interned, p.second, false);
        }
    }

    interpreter->setFrameInfo(frame_state.frame_info);

    CFGBlock* start_block = NULL;
    AST_stmt* starting_statement = NULL;
    while (true) {
        if (enclosing_stmt->type == AST_TYPE::Assign) {
            auto asgn = ast_cast<AST_Assign>(enclosing_stmt);
            assert(asgn->value == after_expr);
            assert(asgn->targets.size() == 1);
            assert(asgn->targets[0]->type == AST_TYPE::Name);
            auto name = ast_cast<AST_Name>(asgn->targets[0]);
            assert(name->id.s()[0] == '#');
            interpreter->addSymbol(name->id, expr_val, true);
            break;
        } else if (enclosing_stmt->type == AST_TYPE::Expr) {
            auto expr = ast_cast<AST_Expr>(enclosing_stmt);
            assert(expr->value == after_expr);
            break;
        } else if (enclosing_stmt->type == AST_TYPE::Invoke) {
            auto invoke = ast_cast<AST_Invoke>(enclosing_stmt);
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
        for (auto block : clfunc->source->cfg->blocks) {
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

    Value v = ASTInterpreter::execute(*interpreter, start_block, starting_statement);

    return v.o ? v.o : None;
}

AST_stmt* getCurrentStatementForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getCurrentStatement();
}

Box* getGlobalsForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getGlobals();
}

CLFunction* getCLForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getCL();
}

FrameInfo* getFrameInfoForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getFrameInfo();
}

BoxedDict* localsForInterpretedFrame(void* frame_ptr, bool only_user_visible) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    BoxedDict* rtn = new BoxedDict();
    for (auto& l : interpreter->getSymbolTable()) {
        if (only_user_visible && (l.first.s()[0] == '!' || l.first.s()[0] == '#'))
            continue;

        rtn->d[l.first.getBox()] = interpreter->getSymbolTable().getMapped(l.second);
    }

    return rtn;
}

BoxedClosure* passedClosureForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getPassedClosure();
}

void setupInterpreter() {
    astinterpreter_cls = BoxedHeapClass::create(type_cls, object_cls, ASTInterpreter::gcHandler, 0, 0,
                                                sizeof(ASTInterpreter), false, "astinterpreter");
    astinterpreter_cls->tp_dealloc = ASTInterpreter::simpleDestructor;
    astinterpreter_cls->has_safe_tp_dealloc = true;
    astinterpreter_cls->freeze();
}
}
