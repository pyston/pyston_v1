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

#include "codegen/llvm_interpreter.h"

#include <sstream>
#include <unordered_map>

#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/util.h"
#include "codegen/patchpoints.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/types.h"

//#undef VERBOSITY
//#define VERBOSITY(x) 2
//#define TIME_INTERPRETS

extern "C" void* __cxa_allocate_exception(size_t);

namespace pyston {

union Val {
    bool b;
    int64_t n;
    double d;
    Box* o;

    Val(bool b) : b(b) {}
    Val(int64_t n) : n(n) {}
    Val(double d) : d(d) {}
    Val(Box* o) : o(o) {}
};

typedef std::unordered_map<llvm::Value*, Val> SymMap;

int width(llvm::Type* t, const llvm::DataLayout& dl) {
    return dl.getTypeSizeInBits(t) / 8;
    // if (t == g.i1) return 1;
    // if (t == g.i64) return 8;
    // if (t->isPointerTy()) return 8;
    //
    // t->dump();
    // RELEASE_ASSERT(0, "");
}

int width(llvm::Value* v, const llvm::DataLayout& dl) {
    return width(v->getType(), dl);
}

Val fetch(llvm::Value* v, const llvm::DataLayout& dl, const SymMap& symbols) {
    assert(v);

    int opcode = v->getValueID();

    // std::ostringstream os("");
    // os << "fetch_" << opcode;
    // int statid = Stats::getStatId(os.str());
    // Stats::log(statid);

    if (opcode >= llvm::Value::InstructionVal) {
        assert(symbols.count(v));
        return symbols.find(v)->second;
    }

    switch (opcode) {
        case llvm::Value::ArgumentVal: {
            assert(symbols.count(v));
            return symbols.find(v)->second;
        }
        case llvm::Value::ConstantIntVal: {
            if (v->getType() == g.i1)
                return (int64_t)llvm::cast<llvm::ConstantInt>(v)->getZExtValue();
            if (v->getType() == g.i64 || v->getType() == g.i32)
                return llvm::cast<llvm::ConstantInt>(v)->getSExtValue();
            v->dump();
            RELEASE_ASSERT(0, "");
        }
        case llvm::Value::ConstantFPVal: {
            return llvm::cast<llvm::ConstantFP>(v)->getValueAPF().convertToDouble();
        }
        case llvm::Value::ConstantExprVal: {
            llvm::ConstantExpr* ce = llvm::cast<llvm::ConstantExpr>(v);
            if (ce->isCast()) {
                if (ce->getOpcode() == llvm::Instruction::IntToPtr && ce->getOperand(0)->getType() == g.i1) {
                    // inttoptr is specified to zero-extend
                    Val o = fetch(ce->getOperand(0), dl, symbols);
                    return o.n & 0x1;
                }

                assert(width(ce->getOperand(0), dl) == 8 && width(ce, dl) == 8);

                Val o = fetch(ce->getOperand(0), dl, symbols);
                return o;
            } else if (ce->getOpcode() == llvm::Instruction::GetElementPtr) {
                int64_t base = (int64_t)fetch(ce->getOperand(0), dl, symbols).o;
                llvm::Type* t = ce->getOperand(0)->getType();

                llvm::User::value_op_iterator begin = ce->value_op_begin();
                ++begin;
                std::vector<llvm::Value*> indices(begin, ce->value_op_end());

                int64_t offset = dl.getIndexedOffset(t, indices);

                /*if (VERBOSITY()) {
                    ce->dump();
                    ce->getOperand(0)->dump();
                    for (int i = 0; i < indices.size() ;i++) {
                        indices[i]->dump();
                    }
                    printf("resulting offset: %ld\n", offset);
                }*/

                return base + offset;
            } else {
                v->dump();
                RELEASE_ASSERT(0, "");
            }
        }
        /*case llvm::Value::FunctionVal: {
            llvm::Function* f = llvm::cast<llvm::Function>(v);
            if (f->getName() == "printf") {
                return (int64_t)printf;
            } else if (f->getName() == "reoptCompiledFunc") {
                return (int64_t)reoptCompiledFunc;
            } else if (f->getName() == "compilePartialFunc") {
                return (int64_t)compilePartialFunc;
            } else if (startswith(f->getName(), "runtimeCall")) {
                return (int64_t)g.func_registry.getFunctionAddress("runtimeCall");
            } else {
                return (int64_t)g.func_registry.getFunctionAddress(f->getName());
            }
        }*/
        case llvm::Value::GlobalVariableVal: {
            llvm::GlobalVariable* gv = llvm::cast<llvm::GlobalVariable>(v);
            if (!gv->isDeclaration() && gv->getLinkage() == llvm::GlobalVariable::InternalLinkage) {
                static std::unordered_map<llvm::GlobalVariable*, void*> made;

                void*& r = made[gv];
                if (r == NULL) {
                    llvm::Type* t = gv->getType()->getElementType();
                    r = (void*)malloc(width(t, dl));
                    if (gv->hasInitializer()) {
                        llvm::Constant* init = gv->getInitializer();
                        assert(init->getType() == t);
                        if (t == g.i64) {
                            llvm::ConstantInt* ci = llvm::cast<llvm::ConstantInt>(init);
                            *(int64_t*)r = ci->getSExtValue();
                        } else {
                            gv->dump();
                            RELEASE_ASSERT(0, "");
                        }
                    }
                }

                // gv->getType()->dump();
                // gv->dump();
                // printf("%p\n", r);
                // RELEASE_ASSERT(0, "");
                return (int64_t)r;
            }

            gv->dump();
            RELEASE_ASSERT(0, "");
        }
        case llvm::Value::UndefValueVal:
            // It's ok to evaluate an undef as long as we're being careful
            // to not use it later.
            // Typically this happens if we need to propagate the 'value' of an
            // maybe-defined Python variable; we won't actually read from it if
            // it's undef, since it should be guarded by an !is_defined variable.
            return (int64_t)-1337;
        case llvm::Value::ConstantPointerNullVal:
            return (int64_t)0;
        default:
            v->dump();
            RELEASE_ASSERT(0, "%d", v->getValueID());
    }
}

static void set(SymMap& symbols, const llvm::BasicBlock::iterator& it, Val v) {
    if (VERBOSITY() >= 2) {
        printf("Setting to %lx / %f: ", v.n, v.d);
        fflush(stdout);
        it->dump();
    }

    SymMap::iterator f = symbols.find(it);
    if (f != symbols.end())
        f->second = v;
    else
        symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*it)), v));
    //#define SET(v) symbols.insert(std::make_pair(static_cast<llvm::Value*>(&(*it)), Val(v)))
}

static std::unordered_map<void*, llvm::Instruction*> cur_instruction_map;

typedef std::vector<const SymMap*> root_stack_t;
static threading::PerThreadSet<root_stack_t> root_stack_set;
/*
void gatherInterpreterRoots(GCVisitor* visitor) {
    root_stack_set.forEachValue(std::function<void(root_stack_t*, GCVisitor*)>([](root_stack_t* v, GCVisitor* visitor) {
                                    for (const SymMap* sym_map : *v) {
                                        for (const auto& p2 : *sym_map) {
                                            visitor->visitPotential(p2.second.o);
                                        }
                                    }
                                }),
                                visitor);
}
*/
#if 0
BoxedDict* localsForInterpretedFrame(void* frame_ptr, bool only_user_visible) {
    llvm::Instruction* inst = cur_instruction_map[frame_ptr];
    assert(inst);

    const SymMap* syms = root_stack_set.get()->back();
    assert(syms);

    ASSERT(llvm::isa<llvm::CallInst>(inst) || llvm::isa<llvm::InvokeInst>(inst),
           "trying to unwind from not within a patchpoint!");

    llvm::CallSite CS(inst);
    llvm::Function* f = CS.getCalledFunction();
    assert(startswith(f->getName(), "llvm.experimental.patchpoint."));

    llvm::Value* pp_arg = CS.getArgument(0);
    int64_t pp_id = llvm::cast<llvm::ConstantInt>(pp_arg)->getSExtValue();
    PatchpointInfo* pp = reinterpret_cast<PatchpointInfo*>(pp_id);

    llvm::DataLayout dl(inst->getParent()->getParent()->getParent());

    BoxedDict* rtn = new BoxedDict();

    int stackmap_args_start = 4 + llvm::cast<llvm::ConstantInt>(CS.getArgument(3))->getZExtValue();
    assert(CS.arg_size() == stackmap_args_start + pp->totalStackmapArgs());

    // TODO: too much duplication here with other code

    int cur_arg = pp->frameStackmapArgsStart();
    for (const PatchpointInfo::FrameVarInfo& frame_var : pp->getFrameVars()) {
        int num_args = frame_var.type->numFrameArgs();

        if (only_user_visible && (frame_var.name[0] == '!' || frame_var.name[0] == '#')) {
            cur_arg += num_args;
            continue;
        }

        llvm::SmallVector<uint64_t, 1> vals;

        for (int i = cur_arg, e = cur_arg + num_args; i < e; i++) {
            Val r = fetch(CS.getArgument(stackmap_args_start + i), dl, *syms);
            vals.push_back(r.n);
        }

        Box* b = frame_var.type->deserializeFromFrame(vals);
        ASSERT(gc::isValidGCObject(b), "%p", b);
        rtn->d[boxString(frame_var.name)] = b;

        cur_arg += num_args;
    }
    assert(cur_arg - pp->frameStackmapArgsStart() == pp->numFrameStackmapArgs());

    return rtn;
}
#endif

class UnregisterHelper {
private:
    void* frame_ptr;

public:
    constexpr UnregisterHelper(void* frame_ptr) : frame_ptr(frame_ptr) {}

    ~UnregisterHelper() {
        root_stack_set.get()->pop_back();

        assert(cur_instruction_map.count(frame_ptr));
        cur_instruction_map.erase(frame_ptr);
    }
};

static std::unordered_map<llvm::Instruction*, LineInfo*> line_infos;
/*
const LineInfo* getLineInfoForInterpretedFrame(void* frame_ptr) {
    llvm::Instruction* cur_instruction = cur_instruction_map[frame_ptr];
    assert(cur_instruction);

    auto it = line_infos.find(cur_instruction);
    if (it == line_infos.end()) {
        const llvm::DebugLoc& debug_loc = cur_instruction->getDebugLoc();
        llvm::DISubprogram subprog(debug_loc.getScope(g.context));

        // TODO better lifetime management
        LineInfo* rtn = new LineInfo(debug_loc.getLine(), debug_loc.getCol(), subprog.getFilename(), subprog.getName());
        line_infos.insert(it, std::make_pair(cur_instruction, rtn));
        return rtn;
    } else {
        return it->second;
    }
}
*/
void dumpLLVM(llvm::Value* v) {
    v->dump();
}
void dumpLLVM(llvm::Instruction* v) {
    v->dump();
}

Box* interpretFunction(llvm::Function* f, int nargs, Box* closure, Box* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args) {
    assert(f);

#ifdef TIME_INTERPRETS
    Timer _t("to interpret", 1000000);
    long this_us = 0;
#endif

    static StatCounter interpreted_runs("interpreted_runs");
    interpreted_runs.log();

    llvm::DataLayout dl(f->getParent());

    // f->dump();
    // assert(nargs == f->getArgumentList().size());

    SymMap symbols;

    void* frame_ptr = __builtin_frame_address(0);
    root_stack_set.get()->push_back(&symbols);
    UnregisterHelper helper(frame_ptr);

    int arg_num = -1;
    int closure_indicator = closure ? 1 : 0;
    int generator_indicator = generator ? 1 : 0;
    int arg_offset = closure_indicator + generator_indicator;
    for (llvm::Argument& arg : f->args()) {
        arg_num++;

        if (arg_num == 0 && closure)
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&arg), Val(closure)));
        else if ((arg_num == 0 || (arg_num == 1 && closure)) && generator)
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&arg), Val(generator)));
        else if (arg_num == 0 + arg_offset)
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&arg), Val(arg1)));
        else if (arg_num == 1 + arg_offset)
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&arg), Val(arg2)));
        else if (arg_num == 2 + arg_offset)
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&arg), Val(arg3)));
        else {
            assert(arg_num == 3 + arg_offset);
            assert(f->getArgumentList().size() == 4 + arg_offset);
            assert(f->getArgumentList().back().getType() == g.llvm_value_type_ptr->getPointerTo());
            symbols.insert(std::make_pair(static_cast<llvm::Value*>(&arg), Val((int64_t)args)));
            // printf("loading %%4 with %p\n", (void*)args);
            break;
        }
    }

    llvm::BasicBlock* prevblock = NULL;
    llvm::BasicBlock* curblock = &f->getEntryBlock();

    // The symbol table at the end of the previous BB
    // This is important for the following case:
    // %a = phi [0, %l1], [1, %l2]
    // %b = phi [0, %l1], [%a, %l2]
    // The reference to %a in the definition of %b resolves to the *previous* value of %a,
    // not the value of %a that we just set in the phi.
    SymMap prev_symbols;

    struct {
        Box* exc_obj;
        int64_t exc_selector;
    } landingpad_value;

    while (true) {
        for (llvm::Instruction& _inst : *curblock) {
            llvm::Instruction* inst = &_inst;
            cur_instruction_map[frame_ptr] = inst;

            if (VERBOSITY("interpreter") >= 2) {
                printf("executing in %s: ", f->getName().data());
                fflush(stdout);
                inst->dump();
                // f->dump();
            }

#define SET(v) set(symbols, inst, (v))

            if (llvm::LandingPadInst* lpad = llvm::dyn_cast<llvm::LandingPadInst>(inst)) {
                SET((intptr_t)&landingpad_value);
                continue;
            } else if (llvm::ExtractValueInst* ev = llvm::dyn_cast<llvm::ExtractValueInst>(inst)) {
                Val r = fetch(ev->getAggregateOperand(), dl, symbols);
                llvm::ArrayRef<unsigned> indexes = ev->getIndices();

#ifndef NDEBUG
                assert(indexes.size() == 1);
                llvm::Type* t = llvm::ExtractValueInst::getIndexedType(ev->getAggregateOperand()->getType(), indexes);
                assert(width(t, dl) == 8);
#endif

                int64_t* ptr = (int64_t*)r.n;
                int64_t val = ptr[indexes[0]];
                SET(val);
                continue;
            } else if (llvm::LoadInst* li = llvm::dyn_cast<llvm::LoadInst>(inst)) {
                llvm::Value* ptr = li->getOperand(0);
                Val v = fetch(ptr, dl, symbols);
                // printf("loading from %p\n", v.o);

                if (width(li, dl) == 1) {
                    Val r = Val(*(bool*)v.o);
                    SET(r);
                    continue;
                } else if (width(li, dl) == 8) {
                    Val r = Val(*(int64_t*)v.o);
                    SET(r);
                    continue;
                } else {
                    li->dump();
                    RELEASE_ASSERT(0, "");
                }
            } else if (llvm::StoreInst* si = llvm::dyn_cast<llvm::StoreInst>(inst)) {
                llvm::Value* val = si->getOperand(0);
                llvm::Value* ptr = si->getOperand(1);
                Val v = fetch(val, dl, symbols);
                Val p = fetch(ptr, dl, symbols);

                // printf("storing %lx at %lx\n", v.n, p.n);

                if (width(val, dl) == 1) {
                    *(bool*)p.o = v.b;
                    continue;
                } else if (width(val, dl) == 8) {
                    *(int64_t*)p.o = v.n;
                    continue;
                } else {
                    si->dump();
                    RELEASE_ASSERT(0, "");
                }
            } else if (llvm::CmpInst* ci = llvm::dyn_cast<llvm::CmpInst>(inst)) {
                assert(ci->getType() == g.i1);

                Val a0 = fetch(ci->getOperand(0), dl, symbols);
                Val a1 = fetch(ci->getOperand(1), dl, symbols);
                llvm::CmpInst::Predicate pred = ci->getPredicate();
                switch (pred) {
                    case llvm::CmpInst::ICMP_EQ:
                        SET(a0.n == a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_NE:
                        SET(a0.n != a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SLT:
                        SET(a0.n < a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SLE:
                        SET(a0.n <= a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SGT:
                        SET(a0.n > a1.n);
                        continue;
                    case llvm::CmpInst::ICMP_SGE:
                        SET(a0.n >= a1.n);
                        continue;
                    case llvm::CmpInst::FCMP_OEQ:
                        SET(a0.d == a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_UNE:
                        SET(a0.d != a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OLT:
                        SET(a0.d < a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OLE:
                        SET(a0.d <= a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OGT:
                        SET(a0.d > a1.d);
                        continue;
                    case llvm::CmpInst::FCMP_OGE:
                        SET(a0.d >= a1.d);
                        continue;
                    default:
                        ci->dump();
                        RELEASE_ASSERT(0, "");
                }
                continue;
            } else if (llvm::BinaryOperator* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
                if (bo->getOperand(0)->getType() == g.i64 || bo->getOperand(0)->getType() == g.i1) {
                    // assert(bo->getOperand(0)->getType() == g.i64);
                    // assert(bo->getOperand(1)->getType() == g.i64);

                    Val a0 = fetch(bo->getOperand(0), dl, symbols);
                    Val a1 = fetch(bo->getOperand(1), dl, symbols);
                    llvm::Instruction::BinaryOps opcode = bo->getOpcode();
                    switch (opcode) {
                        case llvm::Instruction::Add:
                            SET(a0.n + a1.n);
                            continue;
                        case llvm::Instruction::And:
                            SET(a0.n & a1.n);
                            continue;
                        case llvm::Instruction::AShr:
                            SET(a0.n >> a1.n);
                            continue;
                        case llvm::Instruction::Mul:
                            SET(a0.n * a1.n);
                            continue;
                        case llvm::Instruction::Or:
                            SET(a0.n | a1.n);
                            continue;
                        case llvm::Instruction::Shl:
                            SET(a0.n << a1.n);
                            continue;
                        case llvm::Instruction::Sub:
                            SET(a0.n - a1.n);
                            continue;
                        case llvm::Instruction::Xor:
                            SET(a0.n ^ a1.n);
                            continue;
                        default:
                            bo->dump();
                            RELEASE_ASSERT(0, "");
                    }
                    continue;
                } else if (bo->getOperand(0)->getType() == g.double_) {
                    // assert(bo->getOperand(0)->getType() == g.i64);
                    // assert(bo->getOperand(1)->getType() == g.i64);

                    double lhs = fetch(bo->getOperand(0), dl, symbols).d;
                    double rhs = fetch(bo->getOperand(1), dl, symbols).d;
                    llvm::Instruction::BinaryOps opcode = bo->getOpcode();
                    switch (opcode) {
                        case llvm::Instruction::FAdd:
                            SET(lhs + rhs);
                            continue;
                        case llvm::Instruction::FMul:
                            SET(lhs * rhs);
                            continue;
                        case llvm::Instruction::FSub:
                            SET(lhs - rhs);
                            continue;
                        default:
                            bo->dump();
                            RELEASE_ASSERT(0, "");
                    }
                    continue;
                } else {
                    bo->dump();
                    RELEASE_ASSERT(0, "");
                }
            } else if (llvm::GetElementPtrInst* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
                int64_t base = fetch(gep->getPointerOperand(), dl, symbols).n;

                llvm::User::value_op_iterator begin = gep->value_op_begin();
                ++begin;
                std::vector<llvm::Value*> indices(begin, gep->value_op_end());

                int64_t offset = dl.getIndexedOffset(gep->getPointerOperandType(), indices);
                // gep->dump();
                // printf("offset for inst: %ld (base is %lx)\n", offset, base);
                SET(base + offset);
                continue;
            } else if (llvm::AllocaInst* al = llvm::dyn_cast<llvm::AllocaInst>(inst)) {
                int size = fetch(al->getArraySize(), dl, symbols).n * width(al->getAllocatedType(), dl);
                void* ptr = alloca(size);
                // void* ptr = malloc(size);
                // printf("alloca()'d at %p\n", ptr);
                SET((int64_t)ptr);
                continue;
            } else if (llvm::SIToFPInst* si = llvm::dyn_cast<llvm::SIToFPInst>(inst)) {
                assert(width(si->getOperand(0), dl) == 8);
                SET((double)fetch(si->getOperand(0), dl, symbols).n);
                continue;
            } else if (llvm::BitCastInst* bc = llvm::dyn_cast<llvm::BitCastInst>(inst)) {
                assert(width(bc->getOperand(0), dl) == 8);
                SET(fetch(bc->getOperand(0), dl, symbols));
                continue;
            } else if (llvm::IntToPtrInst* bc = llvm::dyn_cast<llvm::IntToPtrInst>(inst)) {
                if (bc->getOperand(0)->getType() == g.i1) {
                    SET(fetch(bc->getOperand(0), dl, symbols).n & 0xff);
                } else {
                    assert(width(bc->getOperand(0), dl) == 8);
                    SET(fetch(bc->getOperand(0), dl, symbols));
                }
                continue;
                //} else if (llvm::CallInst* ci = llvm::dyn_cast<llvm::CallInst>(inst)) {
            } else if (llvm::TruncInst* tr = llvm::dyn_cast<llvm::TruncInst>(inst)) {
                Val r = fetch(tr->getOperand(0), dl, symbols);
                assert(tr->getType() == g.i1);
                SET(r.n & 0x1);
                continue;
            } else if (llvm::ZExtInst* se = llvm::dyn_cast<llvm::ZExtInst>(inst)) {
                Val r = fetch(se->getOperand(0), dl, symbols);
                assert(se->getOperand(0)->getType() == g.i1);
                assert(se->getType() == g.i64);
                SET((int64_t)(uint64_t)(uint8_t)r.n);
                continue;
            } else if (llvm::isa<llvm::CallInst>(inst) || llvm::isa<llvm::InvokeInst>(inst)) {
                llvm::CallSite cs(inst);
                llvm::InvokeInst* invoke = llvm::dyn_cast<llvm::InvokeInst>(inst);

                void* f;
                int arg_start, num_args;
                if (cs.getCalledFunction()
                    && (cs.getCalledFunction()->getName() == "llvm.experimental.patchpoint.void"
                        || cs.getCalledFunction()->getName() == "llvm.experimental.patchpoint.i64"
                        || cs.getCalledFunction()->getName() == "llvm.experimental.patchpoint.double")) {
// cs.dump();
#ifndef NDEBUG
                    // We use size == CALL_ONLY_SIZE to imply that the call isn't patchable
                    int pp_size = (int64_t)fetch(cs.getArgument(1), dl, symbols).n;
                    ASSERT(pp_size == CALL_ONLY_SIZE, "shouldn't be generating patchpoints for interpretation");
#endif

                    f = (void*)fetch(cs.getArgument(2), dl, symbols).n;
                    arg_start = 4;
                    num_args = (int64_t)fetch(cs.getArgument(3), dl, symbols).n;
                } else {
                    f = (void*)fetch(cs.getCalledValue(), dl, symbols).n;
                    arg_start = 0;
                    num_args = cs.arg_size();
                }

                if (VERBOSITY("interpreter") >= 2)
                    printf("calling %s\n", g.func_addr_registry.getFuncNameAtAddress(f, true).c_str());

                std::vector<Val> args;
                for (int i = arg_start; i < arg_start + num_args; i++) {
                    // cs.getArgument(i)->dump();
                    args.push_back(fetch(cs.getArgument(i), dl, symbols));
                }

#ifdef TIME_INTERPRETS
                this_us += _t.end();
#endif
                // This is dumb but I don't know how else to do it:

                int mask = 1;
                if (cs.getType() == g.double_)
                    mask = 3;
                else
                    mask = 2;

                for (int i = arg_start; i < arg_start + num_args; i++) {
                    mask <<= 1;
                    if (cs.getArgument(i)->getType() == g.double_)
                        mask |= 1;
                }

                Val r((int64_t)0);
                try {
                    switch (mask) {
                        case 0b10:
                            r = reinterpret_cast<int64_t (*)()>(f)();
                            break;
                        case 0b11:
                            r = reinterpret_cast<double (*)()>(f)();
                            break;
                        case 0b100:
                            r = reinterpret_cast<int64_t (*)(int64_t)>(f)(args[0].n);
                            break;
                        case 0b101:
                            r = reinterpret_cast<int64_t (*)(double)>(f)(args[0].d);
                            break;
                        case 0b110:
                            r = reinterpret_cast<double (*)(int64_t)>(f)(args[0].n);
                            break;
                        case 0b1000:
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(f)(args[0].n, args[1].n);
                            break;
                        case 0b1001:
                            r = reinterpret_cast<int64_t (*)(int64_t, double)>(f)(args[0].n, args[1].d);
                            break;
                        case 0b1011:
                            r = reinterpret_cast<int64_t (*)(double, double)>(f)(args[0].d, args[1].d);
                            break;
                        case 0b1100:
                            r = reinterpret_cast<double (*)(int64_t, int64_t)>(f)(args[0].n, args[1].n);
                            break;
                        case 0b1111:
                            r = reinterpret_cast<double (*)(double, double)>(f)(args[0].d, args[1].d);
                            break;
                        case 0b10000:
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t)>(f)(args[0].n, args[1].n,
                                                                                            args[2].n);
                            break;
                        case 0b10001:
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, double)>(f)(args[0].n, args[1].n,
                                                                                           args[2].d);
                            break;
                        case 0b10011:
                            r = reinterpret_cast<int64_t (*)(int64_t, double, double)>(f)(args[0].n, args[1].d,
                                                                                          args[2].d);
                            break;
                        case 0b100000: // 32
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t)>(f)(
                                args[0].n, args[1].n, args[2].n, args[3].n);
                            break;
                        case 0b100001: // 33
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, double)>(f)(
                                args[0].n, args[1].n, args[2].n, args[3].d);
                            break;
                        case 0b100010: // 34
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, double, int64_t)>(f)(
                                args[0].n, args[1].n, args[2].d, args[3].n);
                            break;
                        case 0b100110:
                            r = reinterpret_cast<int64_t (*)(int64_t, double, double, int64_t)>(f)(
                                args[0].n, args[1].d, args[2].d, args[3].n);
                            break;
                        case 0b101010:
                            r = reinterpret_cast<int64_t (*)(double, int64_t, double, int64_t)>(f)(
                                args[0].d, args[1].n, args[2].d, args[3].n);
                            break;
                        case 0b1000000: // 64
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t)>(f)(
                                args[0].n, args[1].n, args[2].n, args[3].n, args[4].n);
                            break;
                        case 0b10000000: // 128
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)>(f)(
                                args[0].n, args[1].n, args[2].n, args[3].n, args[4].n, args[5].n);
                            break;
                        case 0b100000000: // 256
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                                             int64_t)>(f)(args[0].n, args[1].n, args[2].n, args[3].n,
                                                                          args[4].n, args[5].n, args[6].n);
                            break;
                        case 0b1000000000: // 512
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                                             int64_t, int64_t)>(f)(
                                args[0].n, args[1].n, args[2].n, args[3].n, args[4].n, args[5].n, args[6].n, args[7].n);
                            break;
                        case 0b10000000000: // 1024
                            r = reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                                             int64_t, int64_t, int64_t)>(f)(
                                args[0].n, args[1].n, args[2].n, args[3].n, args[4].n, args[5].n, args[6].n, args[7].n,
                                args[8].n);
                            break;
                        default:
                            inst->dump();
                            RELEASE_ASSERT(0, "%d", mask);
                            break;
                    }
                    if (cs.getType() != g.void_)
                        SET(r);

                    if (invoke != nullptr) {
                        prevblock = curblock;
                        curblock = invoke->getNormalDest();
                        prev_symbols = symbols;
                    }
                } catch (Box* e) {
                    if (VERBOSITY("interpreter") >= 2) {
                        printf("Caught exception: %p\n", e);
                    }

                    if (invoke == nullptr)
                        throw;

                    prevblock = curblock;
                    curblock = invoke->getUnwindDest();
                    prev_symbols = symbols;

                    landingpad_value.exc_obj = e;
                    landingpad_value.exc_selector
                        = 1; // I don't think it's possible to determine what the value should be
                }


#ifdef TIME_INTERPRETS
                _t.restart("to interpret", 10000000);
#endif
                continue;
            } else if (llvm::SelectInst* si = llvm::dyn_cast<llvm::SelectInst>(inst)) {
                Val test = fetch(si->getCondition(), dl, symbols);
                Val vt = fetch(si->getTrueValue(), dl, symbols);
                Val vf = fetch(si->getFalseValue(), dl, symbols);
                if (test.b)
                    SET(vt);
                else
                    SET(vf);
                continue;
            } else if (llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
                assert(prevblock);
                SET(fetch(phi->getIncomingValueForBlock(prevblock), dl, prev_symbols));
                continue;
            } else if (llvm::BranchInst* br = llvm::dyn_cast<llvm::BranchInst>(inst)) {
                prevblock = curblock;
                if (br->isConditional()) {
                    Val t = fetch(br->getCondition(), dl, symbols);
                    if (t.b) {
                        curblock = br->getSuccessor(0);
                    } else {
                        curblock = br->getSuccessor(1);
                    }
                } else {
                    curblock = br->getSuccessor(0);
                }
                prev_symbols = symbols;
                // if (VERBOSITY()) {
                // printf("jumped to %s\n", curblock->getName().data());
                //}
                break;
            } else if (llvm::ReturnInst* ret = llvm::dyn_cast<llvm::ReturnInst>(inst)) {
                llvm::Value* r = ret->getReturnValue();

#ifdef TIME_INTERPRETS
                this_us += _t.end();
                static StatCounter us_interpreting("us_interpreting");
                us_interpreting.log(this_us);
#endif

                if (!r)
                    return NULL;
                Val t = fetch(r, dl, symbols);
                return t.o;
            }


            inst->dump();
            RELEASE_ASSERT(0, "");
        }
    }

    RELEASE_ASSERT(0, "");
}
}
